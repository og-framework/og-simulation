<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationStepDriver.h` — guards

Every prohibition that governs a line of `SimulationStepDriver.h`. Each entry has an **opaque,
stable id**. In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be
typed.

**If this file and `SimulationStepDriver.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is
moved to §R and its number is spent forever. A retired id may be **named** in prose; it may never
again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative, the derivations and the call sequences
live in `SimulationStepDriver-rationale.md`. A guard is a prohibition plus the consequence of
ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — The driver learns of a HardResync ONLY from its resync callback.

**Tag site:** `SimulationStepDriver.h`, `runClientTick`, immediately above
`outcome.hardResync = m_hardResyncThisTick;`.

**The prohibition.** Do not derive `hardResync` (or any other decision of the driver) from the
clock's event counters: `ClientPredictionClock::getDiagnostics()` and its `hardResyncCount`,
`lastHardResyncToTick` and siblings. Do not compare a count before and after `onGameSimulation`.
The flag comes from the callback the constructor registers with
`ClientPredictionClock::registerResyncCallback`, which also invalidates the physics ring.

**The consequence.** Those counters are the clock's event seam, fenced as **diagnostic only, no
production reader, no decision** at its declaration (`EventSeamDiagnostics`), and a test scribbles
garbage into them mid-run and requires every clock output to be unchanged. A driver decision read
from them is the production reader the fence forbids: the scribble would change the driver's
outcome, and the fence's guarantee would no longer cover the code that relies on it. The callback
is also the only signal that fires **inside** `advancePrediction`, before the physics step, which
is when the ring must be emptied.

**What breaks if it moves.** The tag stands on the one line that reports the event outward; that
line is where a counter comparison would be substituted.

---

## G-02 — On a Skip, the backfilled tick is the SCRATCH, committed under `tick - 1`.

**Tag site:** `SimulationStepDriver.h`, `runClientTick`, immediately above
`m_world.commitScratch(tick - 1u);`.

**The prohibition.** Do not replace this commit with a `saveTick(tick - 1u)`, and do not re-take the
scratch anywhere between `onGameSimulation` and this commit. The world committed for the skipped
tick is the one `saveScratch` captured **before** `onGameSimulation`.

**The consequence.** On a Skip the correction cache backfills the skipped tick with the state
**before this step's integrate** (`SimulationReconciliation::backfillSkippedTick`, reached from
`allocateFrontierSlotsAll`). The physics snapshot for that tick must be the same instant, or a
later resim anchored at the skipped tick restores physics from one instant and og-sim state from
another: a systematic correction on every rollback that lands there. A snapshot taken after the
integrate already carries the integrate's body writes; one taken after the step is the next tick's
world.

**What breaks if it moves.** The skip test restores the backfilled tick and compares it with the
world as it stood before the Skip step's integrate; re-taking the scratch at this line fails it.

---

## G-03 — The ring saves a tick under `stepAllocatesFrontierSlot(kind)`. Never re-derive it.

**Tag site:** `SimulationStepDriver.h`, `runClientTick`, immediately above
`if (stepAllocatesFrontierSlot(kind))`.

**The prohibition.** Do not replace the predicate with a literal test (`kind == StepKind::Normal`,
`kind != StepKind::Stall`, `true`), and do not save on a Stall.

**The consequence.** The correction cache opens and completes its frontier slot under this same
predicate (`SimulationReconciliation::allocateFrontierSlotsAll` and `postPredictionAll`). Sharing
it is what makes the physics ring and the cache hold the same set of ticks. A Stall runs a physics
step with the tick unchanged: a save there overwrites the tick's first post-step world with a
second one the cache never recorded, and a later resim restores a world that is one step ahead of
the og-sim state restored beside it. A literal test also silently misfiles any `StepKind` added
later, where the shared predicate forces one decision in one place.

**What breaks if it moves.** The clock-sequence tests compare the ring with the cache frontier
after every sequence; saving on a Stall breaks that equality.

---

## G-04 — A replay is exactly `predictionTick - anchor` steps: one physics step per sim tick.

**Tag site:** `SimulationStepDriver.h`, `resimulateIfRequested`, immediately above
`const uint32_t replayTicks = present - *anchor;`.

**The prohibition.** Do not change the count, do not loop until the clock reports it is no longer
resimulating, and do not reproduce the Stalls and Skips the original prediction took inside the
window.

**The consequence.** The manager's apply edge (`[Resim.Finish]`, then `applyResimAll` and
`consumeResimAnchorsAll`) fires on the replay step after which the resim cursor equals the
prediction tick. One step fewer leaves the clock resimulating into the next normal tick, a stranded
resim; one more integrates a tick past the prediction frontier after the apply edge has already
fired. Reproducing Stall or Skip
multiplicity would give the replay a different number of physics steps than the authority, which
never stalls or skips, ran for the same ticks.

**What breaks if it moves.** The call-order and clock-sequence tests count the replayed steps, and
the real-manager test requires the clock to end caught up with exactly one apply edge.

---

## G-05 — A vacated slot is cleared in EVERY held tick of the occupancy timeline.

**Tag site:** `SimulationStepDriver.h`, `noteOccupancy`, immediately above the loop over
`m_occupancyTimeline` that resets the slot.

**The prohibition.** Do not delete this sweep, and do not limit it to ticks at or after the leave.

**The consequence.** A leave removes the character from og-sim storage at once, so no replay of any
held tick integrates it again. If the timeline still marked the slot occupied in older ticks, a
resim across them would unpark the slot's bodies with no og-sim owner: physical bodies that collide
in the replay and in nothing else. The rule that a leave vacates retroactively is the user ruling
recorded in `SimulationStepDriver-rationale.md`.

**What breaks if it moves.** The occupancy test leaves a character and then requires the slot
cleared in every held tick and parked in every replayed step.

---

## G-06 — A replayed tick records the occupancy it REPLAYED with, never the live one.

**Tag site:** `SimulationStepDriver.h`, `resimulateIfRequested`, immediately above
`recordOccupancy(savedTick, replayOccupancy);`.

**The prohibition.** Do not record `m_occupancy` (or `m_appliedOccupancy` after it has moved)
for a replayed tick.

**The consequence.** The live occupancy can already include a character that joined in this tick's
`beforeTick`, after the replayed ticks were first simulated. Recording it would mark that
character occupied in ticks where og-sim holds no cache slot for it, so the next resim across them
would unpark its bodies while og-sim skips it (`NoSlot`): the physics side and og-sim's skip would
disagree tick for tick, which is exactly what the occupancy boundary exists to prevent.

**What breaks if it moves.** The occupancy-boundary test replays across a registration and requires
parked-and-unintegrated before the first cache slot and live-and-integrated from it, one assertion
per tick.

---

## G-07 — The client's `beforeSimulate` stays AFTER the replay and BEFORE the scratch save.

**Tag site:** `SimulationStepDriver.h`, `runClientTick`, immediately above
`m_hooks.beforeSimulate(upcoming);`.

**The prohibition.** Do not move this call above `resimulateIfRequested` or below
`m_world.saveScratch()`, and do not pass it anything but the `UpcomingTick` this tick's
`beforeTick` received.

**The consequence.** `beforeSimulate` is the moment a host takes the step-start time: the instant
the normal step starts consuming input. Above `resimulateIfRequested`, that time precedes the whole
replay, and every latency measured from it is early by the replay's cost; it would be the same
instant `beforeTick` already offers. Below `saveScratch`, a world change the host makes in the hook
(an occupancy change through `noteOccupancy`) escapes the scratch: on a Skip the scratch is
committed as the backfilled tick (G-02), so the ring would hold for `tick - 1` a world the live
tick did not start from. A later resim anchored there restores a world without the change: only
the occupancy part is put back, by the timeline (which records the applied occupancy either way),
and any other world write made in the hook is lost from the replay. A different `UpcomingTick`
would give the host two descriptions of one tick (its role, its physics step), and a host keying
the step-start time by them would file it under the wrong step.

**What breaks if it moves.** The call-order tests (a normal tick, resims of depth 1, 5 and 12, and
a replay on every tick across a Stall, a Skip and a HardResync) require `beforeSimulate` exactly
once per tick, after the last replay event and immediately before `scratch`, with the
`UpcomingTick` `beforeTick` received; the Skip test joins a character in the hook and requires the
backfilled tick to hold that world.

---

## §R Retired ids

**None.**

---

## Provenance

G-01 to G-06 were written with the header on 2026-10-06; G-07 was written on 2026-10-08 with the
`beforeSimulate` hook. Each was seen to fail its test when the guarded line was edited the
forbidden way. Nothing in this
file depends on any document outside this submodule.
