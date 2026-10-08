<!-- SPDX-License-Identifier: MPL-2.0 -->
# `RenderSnapshot.h` — rationale

The narrative behind `RenderBody`, `RenderSnapshotT` and `fillRenderSnapshot`. The header keeps its
licence, its docs pointer, its code, two guard tags and its `static_assert` and `OG_CHECK` messages;
everything that explains it lives here. Prohibitions are in `RenderSnapshot-guards.md`.

**If this file and `RenderSnapshot.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL and glm only. Everything below is true to a reader with no game engine and no
other repository.

---

## §1 What it is for

A host that steps the simulation on one thread and draws on another needs the post-step poses of
every simulated body on the drawing side, without touching the physics world or the simulation's
storage from there. `RenderSnapshotT` is that hand-over: one value per step, carrying the step's
identity (§4) and one `RenderBody` per physics declaration (§2, §3).

`fillRenderSnapshot` writes one in place. The intended producer is the host's `afterTick`, which the
step driver calls once per `runTick` and never during a replay (`StepHooks-rationale.md` §1), so a
consumer never sees the intermediate poses of a resimulation. The intended carrier is a pooled
mailbox such as `SnapshotChannel` (`Mailbox.h`): its `beginWrite` hands out a slot to fill, which is
why the function takes the snapshot by reference instead of returning one. The fill allocates
nothing: the bodies live in a fixed `std::array`, and `std::sort` sorts in place.

## §2 The source: the simulation's own post-step body states

The fill reads the body states the simulation has already captured, not the physics world. After
each step, `SimulationManager::onPostGameSimulation` calls the integration executor's
`captureBodyStatesAll`, which copies every declaration's post-step body state from the physics
adapter into its simulatable's state (`SimulationIntegrationExecutor.h` :: `captureBodyStatesAll`).
In `afterTick` those states and the physics world describe the same instant, so the snapshot is
exact, and reading the states keeps the fill engine-free and backend-free: it compiles against any
storage whose simulatables expose `getAllState` and `getPhysicsComposite` with const overloads, as
the push loop beside the capture does (`pushCorrectedBodyStatesAll`).

The walk mirrors that push loop: for each simulatable, each declaration of its physics composite in
`forEach` order, through the declaration's const `bodyStateOf`, widened to `PhysicsBodyState`.

## §3 The key, and why the fill sorts

A body is identified by `(simulatableId, declarationIndex)`:
- `simulatableId` is the storage key the simulatable was added under. A host that adds its
  simulatables under a peer-stable id (rather than a per-process handle) gets a key that is the same
  on every peer.
- `declarationIndex` is the declaration's position in its physics composite's `forEach` order,
  counted from 0. It is a `uint8_t`; a composite with more than 256 declarations fails an `OG_CHECK`.

`SimulationObjectStorage` holds one hash map per simulatable type, and `forEachSimulatable` walks
them type by type, each in hash-bucket order. So the fill collects every body first and then sorts
the filled range by key (guard G-02). The result is in ascending key order whatever the insertion
history, the standard library or the mix of simulatable types: two snapshots of the same set list
the same bodies at the same indices, which is what lets a consumer pair them for interpolation by
walking both in step. Two simulatables of different types added under the same id would produce
two bodies with one key; the fill checks that no two adjacent keys are equal after the sort and fails
an `OG_CHECK` if they are.

## §4 The step's identity

Five fields copy the `TickOutcome` fields of the same names (`StepHooks.h`): `physicsStep`, `tick`,
`kind`, `hardResync` (a consumer that interpolates may snap on it) and `replayedTicks` (diagnostic).
The sixth, `stepDeadlineSeconds`, is the caller's argument.

`resimRefused` is not copied: it says what the resim gate did, which is a diagnostic for the step,
not something a consumer draws.

`stepDeadlineSeconds` is the time on the host's clock at which the step is due, which the host
captures when it pumps the step and passes in; the header neither knows nor needs the clock. A
consumer that interpolates must take its render time from the same clock, or the interpolation
drifts.

## §5 `hasRotation`

A declaration's body state is either a full `PhysicsBodyState` (position, rotation, both velocities)
or a `LinearBodyState` (position and linear velocity) for a rotation-locked body
(`PhysicsBodyState.h`). `hasRotation` is a compile-time property of that choice: 1 for a full state,
0 for a linear one (guard G-01). A linear body's `rotation` is the identity, which is what its
widening fabricates; a consumer should keep the rendered object's own rotation for such a body rather
than apply the identity.

Only the position and, for full states, the rotation are carried: no velocities, because a consumer
that interpolates between two snapshots does not extrapolate.

## §6 Sizing

`MaxBodies` is the template argument: the most bodies a game can have, every simulatable it can hold
times the declarations each one has. A static_assert keeps it within `bodyCount`'s `uint16_t`. If the
storage holds more bodies than that, the fill fails an `OG_CHECK` and drops the excess (in a build
where the check does not stop the process, the snapshot then holds the first `MaxBodies` bodies of
the walk, sorted).

Measured on Win64 (MSVC): `RenderBody` is 36 bytes, and a snapshot of 48 bodies is 1,760 bytes.

A fill rewrites only the first `bodyCount` entries; entries beyond it keep whatever an earlier, larger
fill left there. A consumer reads `bodies[0 .. bodyCount)` only.

## §7 What it does not carry, and where the game's instantiation lives

- **No query state.** A body's id, its root and whether a query could find it are not here: a
  consumer that needs to run spatial queries against the rendered poses keeps its own physics world
  for that (a shadow world restored from the step's saved state), not a pose list.
- **No visualisation or HUD state**, which has its own hand-over.
- **No alias for a particular game.** A game names its own instantiation, a `RenderSnapshotT` sized
  to its most simulatables times the declarations of each, beside its other per-game aliases. The
  core header stays free of any one game's numbers.

## §8 What the tests show

`RenderSnapshotTest.cpp`, tag `[RenderSnapshot]`, over the real `SimulationObjectStorage` and mock
simulatables with two declarations (one full, one linear) and one declaration (linear):
- `RenderSnapshot.BodiesAreInKeyOrderOneEntryPerDeclaration`: five simulatables added in descending
  id order, which the test first checks the storage does NOT walk in key order; the snapshot holds
  ten bodies in key order, each with its own position.
- `RenderSnapshot.HasRotationOnlyForAFullBodyState`: the full declaration carries its rotation and
  `hasRotation` 1; the linear one carries the identity and `hasRotation` 0.
- `RenderSnapshot.OutcomeFieldsAndTheDeadlineAreCopied`: every field of §4, twice over one snapshot.
- `RenderSnapshot.BodyCountFollowsTheStorageOnARefill`: a refill from a smaller storage, and from an
  empty one, sets `bodyCount` to the new count.
- `RenderSnapshot.KeyOrderSpansSimulatableTypes`: two simulatable types with interleaved ids give one
  key order across both.
