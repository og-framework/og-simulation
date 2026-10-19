<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationScheduler.h` — guards

Every prohibition that governs a line of `SimulationScheduler.h`. Each entry has an **opaque,
stable id**. In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be
typed.

**If this file and `SimulationScheduler.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is
moved to §R and its number is spent forever. A retired id may be **named** in prose; it may never
again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative, the derivations and the worked numbers
live in `SimulationScheduler-rationale.md`. A guard is a prohibition plus the consequence of
ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine, no other repository and no other file open.

---

## G-01 — This counter is the PHYSICS STEP count. It is never a sim tick.

**Tag site:** `SimulationScheduler.h`, immediately above `uint64_t physicsSteps_ = 0u;`.

**The prohibition.** Do not rename `physicsSteps_` to a tick, and do not give it a tick type.
(The wider rule, that the class names, stores, exposes and assumes no sim tick anywhere, is in
`SimulationScheduler-rationale.md` §1. It has no single site, so it carries no tag.)

**The consequence.** A physics step and a sim tick are different counters. The sim tick belongs to
the clocks (`ServerTickClock` on the authority, `ClientPredictionClock` on a predicting client), and
on a client it diverges from the step count by design: a Stall runs a physics step with the tick
unchanged, a Skip runs one step while the tick advances by two, and a HardResync runs one step
while the tick jumps to a new value. A scheduler that also counted ticks would be a second, wrong owner: it
would agree with the clock until the first Stall and disagree silently from then on. Anything
keyed by the tick (a per-tick snapshot, a correction-cache slot) and read through the scheduler
would land on the wrong slot with no crash and no test failure.

**What breaks if it moves.** The tag stands on the counter declaration because that is where a
second meaning would be typed. Moved away, the declaration reads as "the tick" to anyone who has
only seen the authority, where the two counts happen to advance together.

---

## G-02 — A deadline is `firstDeadline + offset × interval`. It is never a running sum.

**Tag site:** `SimulationScheduler.h`, immediately above the `return` of `deadlineAt`.

**The prohibition.** Do not replace this expression with an accumulator that adds an interval per
step (`next += interval`), in any precision, and do not narrow its arithmetic to `float`.

**The consequence.** Every deadline is computed from its segment's anchor, so the error of the
millionth step is one rounding of one product, not a million roundings. A running sum drifts. The
measured case: with the interval narrowed to `float`, the deadline after 10⁶ steps at 60 Hz was off
by **8.7 × 10⁻⁴ s**, against a required bound of 10⁻⁶ s. A drifting deadline is a step rate that is
not `1 / dt`, which a peer running the same `dt` does not share. A running sum also has no answer
for `stepDeadline` of a past step without storing one value per step.

**What breaks if it moves.** `stepDeadline`, `nextDeadline`, `pump`'s due count and every segment
change all read this one expression. They agree with each other only because there is one.

---

## G-03 — A step that `pump` does not run is COUNTED. Never drop due time silently.

**Tag site:** `SimulationScheduler.h`, in `pump`, immediately above
`recordLostTime(dropped, droppedUntil - droppedFrom);`.

**The prohibition.** Do not delete this call, and do not make it conditional.

**The consequence.** When more steps are due than `maxCatchUpSteps`, `pump` runs the cap and moves
the deadline past the rest. That is the correct behaviour after a hitch, and it is also exactly how
a fixed-step loop loses wall time without anyone noticing: the simulation runs slower than real
time, a client falls behind its server, and nothing reports why. `lostTime` is the only record that
it happened. A missing count reads as "no hitch occurred".

**What breaks if it moves.** The dropped amount exists only inside this branch. Counted anywhere
else, it has to be re-derived from deadlines that `beginSegment` has already moved.

---

## G-04 — `reanchor` COUNTS the gap it skips. Never move the deadline forward without it.

**Tag site:** `SimulationScheduler.h`, in `reanchor`, immediately above
`recordLostTime(stepsDueAt(nowSeconds) - 1u, nowSeconds - next);`.

**The prohibition.** Do not delete this call, and do not make it conditional.

**The consequence.** `reanchor` is the pause/resume path: it moves the next deadline to `now` so
the steps that fell due during the pause are not run in a burst. The time it skips is real
simulation time that did not happen, and `lostTime` is the only record of it. A missing count reads
as "no pause occurred". (Its order against the `beginSegment` that follows is a separate fact,
in `SimulationScheduler-rationale.md` §7, and is pinned by a test, not by this tag.)

**What breaks if it moves.** Same as G-03: the gap is measurable only before the segment changes.

---

## §R Retired ids

**None.**

The rule that `SchedulerConfig::maxCatchUpSteps` has no default was never a guard: it was a
compile-time check from the start. `MaxCatchUpSteps` has no default constructor, so a
`SchedulerConfig` that omits the cap does not compile, and two `static_assert`s in the header pin
it. Both were seen to fail when a default constructor was added.

---

## Provenance

These guards were written with the header on 2026-10-06 and describe the header as first shipped.
Nothing in this file depends on any document outside this submodule.
