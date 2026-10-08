<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationStepDriver.h` — rationale

The narrative behind `SimulationStepDriver`, the per-physics-step orchestration that an engine's
physics callback used to perform. The header keeps its licence, its code and one `⛔G-nn` tag per
guard; everything that explains it lives here.

**If this file and `SimulationStepDriver.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`SimulationStepDriver-guards.md`, reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction (STL and the core's own headers). Everything below is true to a reader with no game
engine and no other repository.

---

## §1 What it is

One `runTick(physicsStep)` per physics step the scheduler issues (`SimulationScheduler.h`). Each call
drives the `SimulationManager`, the physics world (any `PhysicsWorldAdapter`), the integration
executor and the host's `StepHooks` in a fixed order, on one thread. It replaces the four physics
callbacks a host engine used to call into the manager (pre-simulate step, post-solve step, rewind
request, first resim step) with one function the core owns.

| template parameter | what the driver uses |
|---|---|
| `ManagerT` | a `SimulationManager` (duck-typed): `runsPrediction`, `onGameSimulation`, `onPostGameSimulation`, `onCheckIsSimilar`, `prepareResimulation`, `currentIntegratedTick`, `lastIntegratedStep`, `editResimGateProbe`, `editClientClock` / `getClientClock`, `getServerClock` |
| `WorldT` | a `PhysicsWorldAdapter`: the step, the snapshot ring keyed by sim tick, the scratch slot, occupancy |
| `IntegrationExecT` | a `SimulationIntegrationExecutorConcept`: `pushCorrectedBodyStatesAll` after a rollback (§9) |
| `HooksT` | `StepHooks` (`StepHooks.h`): `beforeTick`, `beforeSimulate`, `beforePhysics`, `afterTick` |

The role comes from `manager.runsPrediction()` at construction. A predicting client saves ticks and
resims; the authority does neither (`savesTicks()` is the role, not a setting, so the two cannot
disagree).

`ResimPolicy::OnRequest` is production: a resim runs when `onCheckIsSimilar` names an anchor.
`ResimPolicy::Always` is a **test-only** stress mode (lightyear-style): every tick whose prediction
tick exceeds `alwaysDepthTicks` resims from `predictionTick - alwaysDepthTicks`, without asking the
manager. The constructor refuses `Always` on the authority and a depth of 0.

## §2 The client tick

1. `beforeTick({authority=false, physicsStep})`. The host drains inbound messages and applies
   lifecycle commands here; a join or leave calls `noteOccupancy` (§6).
2. The resim check and, if granted, the replay (§3).
3. If a replay left the world on an older occupancy than the live one, the live occupancy is
   applied again (§6).
4. `beforeSimulate(upcoming)`, with the `UpcomingTick` step 1 passed: after any replay, immediately
   before the normal step starts consuming input. The host takes its step-start time here (guard
   G-07).
5. `world.saveScratch()`: the world before this step's integrate.
6. `manager.onGameSimulation({normal})`. Inside it `advancePrediction` decides Normal, Stall, Skip
   or HardResync, the cache opens its frontier slot, and the integrate writes bodies.
7. The kind is read from `manager.lastIntegratedStep()` (§4). On a Skip the scratch is committed
   under `tick - 1` (guard G-02).
8. `beforePhysics(tick)`: the host's last chance before the world steps (a hot-path client sends its
   input for the tick here).
9. `world.step(dt)`.
10. `manager.onPostGameSimulation({normal})`: capture, then the cache completes its frontier slot.
11. If `stepAllocatesFrontierSlot(kind)`, `world.saveTick(tick)` (guard G-03).
12. `afterTick(outcome)`.

**Why `beforeSimulate` sits between the replay and the scratch.** Before the replay, its time would
precede the replay, which is the opposite of "the step starts consuming". After `saveScratch`, a
world change the host made there (an occupancy change through `noteOccupancy`, say) would be missing
from the scratch: on a Skip that scratch is committed as the backfilled tick (step 7), so the ring
would hold for `tick - 1` a world the live tick did not start from. Before `saveScratch`, the
scratch captures whatever the hook did. The only work between the hook and `onGameSimulation` is
the scratch save itself.

`TickOutcome` carries the tick and kind the manager integrated, whether a HardResync fired, how many
ticks were replayed, whether a resim request was refused, and the physics step.

## §3 The resim

**The anchor.** Under `OnRequest`, `manager.onCheckIsSimilar()`. Its "no resim" answer is
`UINT_MAX`, and `0` (the reserved pre-sim tick) is treated the same way. An anchor at or above the
prediction tick has nothing to replay; it is ignored and counted (`ignoredAnchors`). Such an anchor is not expected from
the cache's gate; the count makes one visible instead of silently replaying nothing.

**Refusal: the anchor is not held.** `world.hasTick(anchor)` false means the ring evicted it or a
HardResync invalidated it. The request is counted (`refusedResims`, and `resimRefused` on the
outcome) and nothing is prepared, so the cache's anchor is not consumed: the gate asks again next
tick, exactly as it did when an engine refused a rewind.

**Granted.**
1. `world.restoreTick(anchor)`, with an `OG_CHECK` on false: `hasTick` said it was held, so a failed
   restore is a backend defect, never a load condition.
2. `world.applyOccupancy(occ[anchor])` (§6).
3. `manager.prepareResimulation(physicsStep, anchor)`: the clock's resim cursor goes to the anchor
   and og-sim state is restored from the correction cache.
4. `integration.pushCorrectedBodyStatesAll()`: the restored og-sim body states are written into the
   physics world (§9).
5. **Exactly `predictionTick - anchor` replay steps** (guard G-04; user ruling Q1: one physics step
   per sim tick, Stall and Skip multiplicity not reproduced). For replayed tick `t`:
   `applyOccupancy(occ[t])`, `onGameSimulation({resim, first})`, `world.step(dt)`,
   `onPostGameSimulation({resim, first})`, `world.saveTick(manager.currentIntegratedTick())`.
   `first` is true on the first replay step only. An `OG_CHECK` requires the integrated tick to be
   `t`.

The manager's apply edge (`[Resim.Finish]`, `applyResimAll`, `consumeResimAnchorsAll`) fires inside
the last replay step's `onPostGameSimulation`, once per resim, because that is the step after which
the resim cursor equals the prediction tick.

The replay span is `anchor + 1 .. predictionTick`. The anchor itself is the restore source and is
never re-simulated. A replay re-saves ticks the ring already holds (every tick between a held anchor
and the present is held), so it never changes which ticks the ring holds.

**Why one step per tick.** The authority never stalls or skips, so replaying the original
prediction's Stall steps would add physics steps the authority never ran for those ticks. A window
that contained a Stall reaches the present with one physics step fewer than the original
prediction took; that is the intent.

## §4 The ring is keyed by sim tick, on the cache's own predicates

The scheduler counts physics steps; the clocks own the sim tick. The ring follows the tick, using
the same decisions the correction cache uses, so that the physics state for tick T and the cache
slot for tick T are the same instant:

| `advancePrediction` | world | ring | cache |
|---|---|---|---|
| Normal | one step | `saveTick(T)` after the step | slot T after the step |
| Stall | one step, tick unchanged | no save | no write (`postPredictionAll` returns early) |
| Skip | one step, tick T-1 skipped | `commitScratch(T-1)` = the world before the integrate; `saveTick(T)` | slot T-1 backfilled before the integrate; slot T after the step |
| HardResync | one step at the new tick | invalidated by the resync callback before the step; `saveTick` of the new tick | wiped by the manager's own resync callback |

The kind is read from `SimulationManager::lastIntegratedStep()` after `onGameSimulation` returns,
the only point at which the clock has decided it. `getStepKind()` reports a HardResync as Normal,
which is correct for the save decision (a HardResync tick allocates a slot) and is why the driver
learns of the HardResync itself from the callback (§5).

## §5 HardResync

The constructor registers a callback with `ClientPredictionClock::registerResyncCallback`. It fires
inside `advancePrediction`, before the physics step, and it:
- calls `world.invalidateAllTicks()`;
- clears the occupancy timeline (it describes ticks the ring no longer holds);
- marks the tick, so `TickOutcome::hardResync` reports it.

The manager registered its own callback first (it wipes the caches), so the caches and the ring are
both empty when the new tick is saved. An anchor older than the resync is then not held and is
refused (§3).

The driver never reads the clock's event counters (guard G-01).

**Callback ids are stable.** The destructor unregisters the callback by its id. The clock used to
hand out vector indices and unregister with swap-with-back, which re-indexed the moved callback, so
a later unregister of that callback's id hit the wrong entry or ran past the end. Ids now come from
a monotonic counter, and unregistering erases the matching entry, keeping every other id and the
firing order. If two drivers ever share one clock, destroying the first no longer corrupts the
second.

**Lifetime.** The driver holds references to the manager, world, executor and hooks, and its
callback captures the driver. It must be destroyed before the manager. It is neither copyable nor
movable.

## §6 Occupancy: the timeline and its boundary

**The user ruling (Q2).** A character's slot is occupied from its **first correction-cache slot
tick**: the first tick `allocateFrontierSlotsAll` pushes for it after registration. Before that the
slot is parked. That is the same tick at which the cache stops answering `NoSlot` to a replay
(`SimulationReconciliation::getAppliedCaptureTickRef`), so for every replayed tick the physics side
(parked or live) and og-sim's integrate (skipped or run) agree. A leave vacates the slot in every
held tick, retroactively. This reproduces how an engine world that disabled the body before the
spawn and removed it on leave behaved.

**How the driver gets that boundary without knowing about caches.** `noteOccupancy` updates the live
occupancy and applies it to the world at once. The timeline records an occupancy for a tick only
when the ring saves that tick (`saveTick` or the Skip's `commitScratch`), and it records the
occupancy the world stepped with. The ring saves under the cache's own predicates (§4), so the first
saved tick after a join is the first cache slot tick:
- a join applied in `beforeTick` of a Normal tick is first recorded at that tick;
- on a Stall tick nothing is saved, so the join is first recorded at the next tick, which is also the
  cache's first slot for it;
- on a Skip tick the backfill commit records it at `tick - 1`, which the cache also backfills for it.

**Replays use the timeline, not the live occupancy.** Each replayed tick applies `occ[t]` and records
that same value back when it saves the tick (guard G-06). A character that joined in this tick's
`beforeTick` is therefore parked for the whole replay, and live again for this tick's normal step,
because the driver re-applies the live occupancy after a replay when they differ (§2 step 3).

**A leave clears the slot in every held tick** (guard G-05) and parks it in the world at once.

**On the authority** there is no ring and no timeline. Occupancy is live only: it starts at the tick
the lifecycle command is applied.

A tick missing from the timeline falls back to the live occupancy and is counted
(`occupancyTimelineMisses`). Every tick between a held anchor and the present is held, so the count
stays 0 in a healthy run; a nonzero is a defect signal.

## §7 The authority tick, and the `+ 1` it replaces

1. `beforeTick({authority=true, authorityTick = getServerClock().getTick() + 1, physicsStep})`. The host
   releases the delayed inputs for `authorityTick` here.
2. `beforeSimulate` with the same `UpcomingTick` (the same `authorityTick`): the step-start moment.
   With no replay on the authority, it follows `beforeTick` directly.
3. `onGameSimulation`, 4. `beforePhysics(authorityTick)`, 5. `world.step`, 6. `onPostGameSimulation`,
7. `afterTick`. No resim, no scratch, no ring saves.

**The derivation.** `onGameSimulationAuthority` advances the server clock by exactly one as its first
action (`ServerTickClock::advanceTick`; the authority has no Stall, Skip or resim), then simulates
`getTick()`. So the tick a `runTick` simulates is the server clock's value **before** the call, plus
one, and `beforeTick` runs immediately before (only `beforeSimulate` lies between), on the same
thread. The `+ 1` is "advance, then
simulate", read directly from the clock.

An engine-hosted release did the same job from a different place: it ran on the game thread before
the physics step and could only reach the sim tick through a step-to-tick offset written one step
late, which named the previous tick, so it added one to that. Here release and step are back to back
on one thread and read the same clock, so no offset and no lag exist.

**Pinned twice.** An `OG_CHECK` after `onGameSimulation` requires the integrated tick to equal
`authorityTick`, and the authority test releases an input in `beforeTick` for `authorityTick` and
requires the step that simulates that tick to consume it (and shows that releasing for
`authorityTick - 1` is never consumed).

## §8 Probe continuity

The resim-gate probe (`ResimGateProbe`, owned by the manager) is fed at the same three points an
engine adapter fed it:

| point | feed | here |
|---|---|---|
| request | `noteRequest(anchor, lastCompletedStep, requestedChaosFrame)` | every anchor the driver acts on, before the held check |
| grant | `noteGrant(grantedChaosFrame)` | when the anchor is held, before `prepareResimulation` |
| first-step prepare | `notePrepare` | inside `prepareResimulation` (unchanged) |

**Refusal = anchor not held.** The probe already derives refusals as `requests - grants`, so a
refused request is a request with no grant, and the `[ResimProbe.Chaos]` line's `refused` field keeps
its meaning.

**The frame domain is the sim tick.** The ring is keyed by sim tick, so the "frame" asked for is the
anchor tick, and the last completed frame is the prediction tick: `noteRequest(anchor,
predictionTick, anchor)`, `noteGrant(anchor)`. The request depth (`depthMin` / `depthMax`) is
therefore the replay depth in ticks, and `clamped` reads a structural 0: a grant is always exactly
the anchor asked for.

**The lines themselves are unchanged.** The manager emits `[ResimProbe.Gate]`, `[ResimProbe.Chaos]`
and `[ResimProbe.Apply]` from `onCheckIsSimilar` as before; the driver adds no line and changes no
format. `[ResimProbe.Chaos]` keeps that spelling: it is frozen, because archived greps key on it.
Under `ResimPolicy::Always` the manager's check is not called, so no window closes; the mode is
test-only.

## §9 `pushCorrectedBodyStatesAll`

After `prepareResimulation` restored og-sim state from the cache, the physics bodies must carry the
same state before the first replay step. `SimulationIntegrationExecutor::pushCorrectedBodyStatesAll`
writes, for every physics declaration of every simulatable:
- position and linear velocity, always;
- rotation and angular velocity only when the declaration's body state is a full `PhysicsBodyState`.

A `LinearBodyState` carries no rotation or spin; widened, it reads as identity rotation and zero spin,
and writing those would overwrite the body's real orientation. So a linear declaration keeps the
body's own rotation: the push reads the body's transform, replaces its translation, and writes it
back. A full declaration writes one transform built from position and rotation.

This is the engine-free port of an engine adapter's direct writes of position, velocity, rotation
and angular velocity at the first resim step. It goes through the `PhysicsBodyAdapter` seam
(`setBodyTransform`, `setBodyLinearVelocity`, `setBodyAngularVelocity`), so any backend receives it.

## §10 The physics step handed to the manager

`prepareResimulation` and `firstResimStepAll` take the physics step as `int32_t`, the width the
engine adapters used. The driver's step is `uint64_t` and is narrowed with a cast; it only labels a
log line and a per-simulatable hook, and wraps after 2³¹ steps (about 414 days at 60 Hz).

## §11 Diagnostics

`getDiagnostics()` is a read-only view (`DiagnosticsConventions.md` §1): granted and refused resims,
replayed ticks, Skip backfills, HardResync invalidations, ignored anchors, timeline misses, and the
timeline itself (`occupancyAt`, `occupancyTimelineSize`). No production decision reads it. The
counters are written on the production path, like a probe's `note*` sites.

## §12 Threading

Every call — the driver's own, the hooks, the manager, the world and the executor — runs on the one
thread that calls `runTick`, in the order above. `noteOccupancy` must be called on that thread too,
which is why the host calls it from a hook (`beforeTick`, or `beforeSimulate`, whose world changes
the scratch still captures, §2). Nothing here is synchronised.

## §13 Provenance

Written on 2026-10-06 with the header, for the project-owned fixed step that replaces an engine
physics scheduler and its rewind. The user rulings it carries (Q1: one physics step per sim tick in a
replay; Q2: occupancy from the first cache slot tick, vacated retroactively) were made in private
working material that is **not distributed with this submodule**; nothing above depends on it.
