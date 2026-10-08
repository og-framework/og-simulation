<!-- SPDX-License-Identifier: MPL-2.0 -->
# `RenderSnapshot.h` — guards

Every prohibition that governs a line of `RenderSnapshot.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `RenderSnapshot.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `RenderSnapshot-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

---

## G-01 — `hasRotation` comes from the declaration's body-state TYPE, never from the widened value

**Tag site:** `RenderSnapshot.h`, in `fillRenderSnapshot`, inside the per-declaration lambda,
immediately above the `constexpr bool hasRotation` that the two assignments below it read.

**The prohibition.** Do not make the flag true for every body, and do not replace it with a value
derived from the widened `PhysicsBodyState` the lambda builds (for example its `rotation` differing
from identity). It is true exactly when the declaration's `bodyStateOf` returns a full
`PhysicsBodyState`, and false for a `LinearBodyState` declaration.

**The consequence.** A `LinearBodyState` declaration carries no rotation: its widening to
`PhysicsBodyState` fabricates the identity rotation. A consumer sets a body's rendered rotation only
when `hasRotation` is 1 and keeps the rendered object's own rotation otherwise. With the flag set for
a rotation-locked body, the consumer writes the fabricated identity over that object's own rotation
every snapshot: a rotation-locked character whose facing is set by the host snaps back to identity.
A test of the value cannot replace the type: a full-state body may legitimately sit at the identity
rotation, and must still report `hasRotation` = 1.

**What breaks if it moves.** Nothing orders it: it is a compile-time constant of the declaration's
type, and the body's `hasRotation` and `rotation` fields both read it rather than each other. Keep it the one
source of both; a second, separate test for the rotation can disagree with the flag.

## G-02 — The bodies are sorted by key after the walk. Do not drop or weaken the sort.

**Tag site:** `RenderSnapshot.h`, in `fillRenderSnapshot`, immediately above the `std::sort` over
`snapshot.bodies`.

**The prohibition.** Do not delete the sort, do not sort by anything but the key
`(simulatableId, declarationIndex)` ascending (`renderSnapshotDetail::keyLess`), and do not move it
before the walk. Do not replace it with "the storage already iterates in id order".

**The consequence.** `SimulationObjectStorage` keeps one hash map per simulatable type and
`forEachSimulatable` walks them type by type in hash-bucket order. That order depends on the ids, the
insertion history and the standard library, so two peers, or one peer before and after a join, list
the same bodies in a different order. Every consumer that pairs bodies across two snapshots
(interpolation between the two that bracket the render time) or across peers relies on the key
order, and would pair the wrong bodies silently. `RenderSnapshot.BodiesAreInKeyOrderOneEntryPerDeclaration`
and `RenderSnapshot.KeyOrderSpansSimulatableTypes` fail without the sort.

**What breaks if it moves.** The sort must run after the last body is written and before `bodyCount`
is published; it covers only the first `count` entries, so entries left over from an earlier, larger
fill stay outside it.
