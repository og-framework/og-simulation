<!-- SPDX-License-Identifier: MPL-2.0 -->
# `PhysicsWorldAdapter.h` — rationale

The seam between og-simulation's tick orchestration and a physics backend's **whole world**: stepping
it, and saving and restoring it per sim tick for rollback. The header holds the licence, a docs
pointer, the code and a compile-time self-check, and nothing else; this file carries the contracts
the concept's signatures cannot express, and why.

**If this file and `PhysicsWorldAdapter.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** It names no physics
engine and no game engine. Where a backend or a consumer outside this submodule is named below, it is
named as provenance or as an example, and nothing here depends on the reader being able to open it.

---

## 1. What the seam is for

The existing per-body seams (`PhysicsBodyAdapter.h`, `PhysicsBodyReaderAdapter.h`,
`SpatialQueryAdapter.h`) read and write individual bodies and run queries. None of them can step the
world or take it back in time: until now the physics engine's own fixed-step callback and rewind
did both. `PhysicsWorldAdapter` is the world-level half that lets og-simulation own the step loop
and the rollback instead.

A backend that satisfies it (`W`) is driven by the step driver, which decides **which** sim tick a
snapshot belongs to. The backend never decides that.

## 2. The ring is keyed by SIM TICK, not by physics step

`SimTick` is a `uint32_t`, the same width as the tick the clocks hand out (`ServerTickClock.h` ::
`getTick`, and the `unsigned int` the `ClientPredictionClock.h` :: `ResyncCallback` receives).

A physics step and a sim tick are different counters: a client clock can Stall (a step with the tick
unchanged), Skip (one step, the tick advances by two) or HardResync (the tick jumps). Keying the ring
by sim tick lets the step driver use the correction cache's own rules for when a tick is saved, so
the physics snapshot for tick T and the correction-cache slot for tick T describe the same instant.

## 3. The members, one at a time

| member | contract |
|---|---|
| `step(float dt)` | advance the live world by one fixed step |
| `saveTick(t)` | snapshot the live world under `t`. A `t` already held is **overwritten in place** and evicts nothing. Otherwise, when the ring is full, the **oldest held tick** (the smallest) is evicted, so saving a tick older than everything in a full ring holds nothing |
| `restoreTick(t)` | make the live world equal to the snapshot of `t`. Returns `false` if `t` is not held, and then the world is untouched. **Atomic:** it either applies fully or leaves the world exactly as it was; a half-applied restore is never observable |
| `hasTick(t)` | whether `t` is held. `const` |
| `oldestHeldTick()` | the smallest held tick, or empty when nothing is held. `const` |
| `invalidateAllTicks()` | drop every held tick. It exists for the clock's HardResync path, so that no snapshot from before the jump can be restored |
| `saveScratch()` | snapshot the live world into one extra slot outside the ring |
| `commitScratch(t)` | store the scratch snapshot under `t`, with the same overwrite and eviction rules as `saveTick` |
| `applyOccupancy(occ)` | set which character slots are live (`BodySlotOccupancy.h`). The caller is meant to re-apply it after every restore and before every step |
| `stateHash(t)` | a 64-bit hash of the held tick `t`, or empty if `t` is not held. `const` |
| `coverage` | a `static constexpr SnapshotCoverage` (§5) |

### 3.1 Why a scratch slot exists — the Skip backfill

On a Skip the clock advances two ticks in one step, and the correction cache backfills the skipped
tick with the state from **before** that step's integrate. The physics snapshot for the backfilled
tick has to be the same pre-step world. The step driver is therefore meant to call `saveScratch()`
before every normal step and, only on a Skip, `commitScratch(tick − 1)` after it. The scratch slot is
outside the ring, so taking it on every step evicts nothing.

### 3.2 What a restore may and may not invalidate

A restore **may invalidate engine handles** (a backend's internal body pointers, contact caches,
broadphase proxies). It **never changes a seam id**: a `BodyId`, `ShapeId` or `QueryVolumeId`
handed out before the restore names the same body or shape after it. Everything above the seam holds
only seam ids, so nothing above the seam has to re-bind after a rollback.

### 3.3 `stateHash` is over seam-level state, never over snapshot bytes

Each backend defines `stateHash` over **seam-level** state: the fixed body ids, each body's
transform and velocities, and the occupancy. It is never a hash of the backend's raw snapshot bytes.
Raw bytes carry engine-internal layout (padding, cache order, version-specific fields), so two
backends, or two versions of one backend, holding the same world would hash differently, and the hash
could not be compared across peers or across a backend swap. A canonical hash can.

### 3.4 Ring depth is a construction parameter

The concept says nothing about depth; each backend takes it at construction. **Sizing rule adopted
with this seam:** a predicting client's depth is at least `TimeConfig::rollbackWindowHardCap` + 2. The
reasoning behind the `+ 2` belongs to the step driver that consumes the ring and is not restated
here. An **authority** world never rolls back and may be built with depth 0, in which case it holds
no tick at all: `saveTick` and `commitScratch` store nothing, and `restoreTick` is always `false`.

## 4. `const` on the three queries

`hasTick`, `oldestHeldTick` and `stateHash` are required on a `const W`. They are questions the step
driver and diagnostics ask without intending to change the world, and requiring `const` stops a
backend from quietly doing work (such as a lazy rebuild) inside what callers treat as a read.

## 5. `SnapshotCoverage` — what a backend's snapshot does NOT include

`SnapshotCoverage` is a compile-time capability with three flags, all `false` by default:

| flag | `true` means the snapshot restores… | when it is `false`, the system above compensates by… |
|---|---|---|
| `bodySetAndIds` | the set of bodies and their ids | keeping a **fixed body set** (slot bodies always present) and re-applying occupancy from a per-tick sidecar after every restore (`applyOccupancy`) |
| `broadphase` | the broadphase structure | treating query hit order as history-dependent: every query result is **sorted by a peer-stable key** before use |
| `staticShapes` | static geometry | building statics **once** (`StaticGeometry.h`) and never putting them in the ring |

A backend whose snapshot omits all three declares the default, `SnapshotCoverage{}`. That is the
expected case for the first backend; the flags exist so that a backend which does cover something can
say so, and so that the compensations above are visibly tied to a declared gap rather than to habit.

The concept requires `coverage` to be **static** and **usable in a constant expression**, so code can
branch on it with `if constexpr`.

## 6. The self-check block at the bottom of the header

`physicsWorldAdapterSelfCheck` holds one conforming reference type and five near-misses, each
asserted against the concept at the point of definition, so a change to the concept that loosens or
breaks a requirement fails the build in every translation unit that includes the header:

| probe | what it lacks | the requirement it pins |
|---|---|---|
| `Conforming` | nothing | the concept is satisfiable at all (without it, every `!` assertion below would pass vacuously) |
| `MissingInvalidateAllTicks` | `invalidateAllTicks` | the HardResync wipe is required |
| `MutatingHasTick` | a `const` `hasTick` | §4 |
| `RawStateHash` | `std::optional` around the hash | an unheld tick is representable as "no hash", never as a sentinel value |
| `NonStaticCoverage` | a static `coverage` | §5 |
| `RuntimeCoverage` | a `constexpr` `coverage` | §5 |

**Witnessed, not reasoned (2026-10-06, MSVC 14.38):**
* Asserting `PhysicsWorldAdapter<physicsWorldAdapterSelfCheck::MissingInvalidateAllTicks>` true fails
  with `C2338`, and the compiler's note names the missing member: *"'invalidateAllTicks': is not a
  member of 'physicsWorldAdapterSelfCheck::MissingInvalidateAllTicks'"*.
* The concept's `typename std::integral_constant<…>` requirement is **load-bearing**: with only the
  `{ W::coverage }` requirement, `RuntimeCoverage` is accepted. `NonStaticCoverage` is already rejected
  by the `{ W::coverage }` requirement alone; it stays as a separate probe because it pins a separate
  property.

## 7. Guards

**None.** Every prohibition that applies to this header is either enforced by the compiler (§6) or
is an obligation on a **backend's** implementation (§3: atomic restore, seam ids preserved, canonical
hash, eviction order). An edit that breaks one of those is typed in the backend's own file, where a
tag in this header would not be in view, so a tag here would stop nothing. The backend that
implements the contract carries its own tests and guards.

## 8. Provenance

Added 2026-10-06 as the core seam for replacing an engine-owned fixed-step physics callback with a
step loop and rollback owned by og-simulation. The design work behind it is private initiative
material and is **not distributed with this submodule**; every claim above is anchored to this
repository.
