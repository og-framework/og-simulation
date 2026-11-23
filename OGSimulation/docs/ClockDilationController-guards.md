<!-- SPDX-License-Identifier: MPL-2.0 -->
# `ClockDilationController.h` — guards

Every prohibition that governs `ClockDilationController`. Each entry has an **opaque, stable id**. A single line
`// ⛔G-nn  docs/ClockDilationController-guards.md` sits exactly where the forbidden edit would be typed. Eight sites are in
`ClockDilationController::update` and its private `rawTargetTicks`, `noteFrame` and `sustainedFrameTicks`; the ninth is in
the client stage, `clockDilation::beforeClientPump`.

**If this file and `ClockDilationController.h` disagree, the header is authoritative and this file is stale.** Fix this
file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved to §R and its number
is spent forever. A retired id may be **named** in prose; it may never again appear as a `⛔G-nn` **tag**.

⛔ **Nothing in this file is a rationale.** The narrative lives in `ClockDilationController-rationale.md`. A guard is a
prohibition plus the consequence of ignoring it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is true and actionable to a
reader with no game engine, no other repository and no other file open.

Two prohibitions are compile-time checks, not guards: the default `maxDeviation` below 0.2 (the bracket bound) and the
default guard edges inside the clock's quiet band: the low edge with one offset quantum and half a tick of headroom above
its capped mean read, the high edge with half a tick of margin, and the two edges never crossing at the longest sampled
frame (rationale §5.3). Each is a `static_assert` beside `ClockDilationConfig`, as is the default change hysteresis's range,
at least half a quantum and at most one slew step (rationale §6), and the sustained frame filter's count: inside its window,
and at most the clock's `gradualCorrectionRate`, so that a run it rejects stays below the clock's graduated count (rationale
§5.1, §5.3; task 7c). A third is a runtime check, not a guard: never apply a scale on the authority. The controller has
no role input, so the stage enforces it with `OG_CHECK(manager.runsPrediction(), …)` as its first statement (rationale
§10, §13).

---

## G-01 — the law is proportional: no integral term without a re-derived drift argument

**Tag site:** `ClockDilationController::update`, immediately above
`const double correction = m_config.gainPerTick * out.errorTicks;`.

**The prohibition.** Do not add an integral (or any accumulated) term to `correction`, and do not add state that biases it,
unless the stability and drift argument of rationale §8 is re-derived for the new loop and the tests re-run.

**Consequence.** The loop is first order with no transport delay because the correction is a pure function of this call's
error. Under a constant clock-rate mismatch δ its steady error is δ/K_p: 0.02 tick at 100 ppm. An integral term makes the
loop second order: it can overshoot, and it winds up while a freeze (rationale §9) holds the scale at 1.0 with a non-zero
error, so every re-centre, input-delay transition debt or resync would end in an overshoot the clock's graduated corrections may then fight.
Test (b), a +1 tick error that must settle at exactly 0.995 and stay, fails under any integrator.

**What breaks if it moves.** The statement must read this call's `out.errorTicks`, after the guard has clamped the target.
Computed from the unclamped target it would chase a target the band guard forbids.

## G-02 — the desired scale is clamped to ±`maxDeviation`, and a freeze asks for exactly 1.0

**Tag site:** `ClockDilationController::update`, immediately above
`const double desired = frozen ? 1.0 : 1.0 - std::clamp(correction, -m_config.maxDeviation, m_config.maxDeviation);`.

**The prohibition.** Do not remove or widen the clamp, do not clamp against the scheduler's own bounds instead, and do not
make the frozen branch anything but 1.0.

**Consequence.**
- The clamp is the bracket bound. The display pairs snapshots at `hostNow − delaySteps · dt/s` with `delaySteps` ≤ 2 over a
  4-slot channel; a pair exists iff the scale's ratio across three intervals stays below 1.5, i.e. iff the deviation is below
  0.2 (rationale §7). The `static_assert` checks only the default; this clamp is what holds any scale this law produces inside
  it. Unclamped, a large error (a hitch's lost time) asks for a scale the scheduler would clamp at 0.75–1.25, whose ratio of
  1.667 makes `beyondOldest` render frames.
- The clamp is also the saturation the tests pin: ±3 ticks must give exactly 0.990 / 1.010 (test (c)).
- A frozen value other than 1.0 keeps dilating while the clock moves the lead by whole ticks (re-centre, input-delay transition debt, resync),
  so the two corrections add.

**What breaks if it moves.** It must read this call's `correction` and feed the slew below it. A freeze reaches the scale
only through `desired`: a freeze test placed after the slew would jump the scale to 1.0 in one call instead of slewing there
(test (d) pins the slew).

## G-03 — the `OwnRtt` target keeps both ½ terms

**Tag site:** `ClockDilationController::rawTargetTicks`, immediately above
`return std::max(floorTicks, margin + 0.5) - halfRtt - 0.5 + m_config.targetOffsetTicks;`.

**The prohibition.** Do not cancel the `+ 0.5` inside `std::max` against the `- 0.5` outside it, and do not drop either.

**Consequence.** They cancel only when the floor does not bind. On the floor (every loopback and LAN client, where
`(R + m·J)·f + ½ ≤ predOffsetFloorTicks`) the expression is `F − (R/2)·f − ½`. Without the halves it is half a tick higher:
3.997 instead of 3.497 at R = 0.1 ms, F = 4. The target would then sit half a tick above the lead the clock holds today, and
the controller would move every LAN client's mean lead by half a tick, which this initiative's Stage 1 must not do (rationale
§3). Test (e)'s loopback arm fails; its emulated arm does not, because there the halves do cancel.

**What breaks if it moves.** Nothing outside this statement depends on its form; the two halves are the mean rounding of the
clock's `ceil` and the mean drain age of the authority tick (rationale §3), and each must stay on its own side of the `max`.

## G-04 — the slew starts from the controller's own previous scale, never from the door's

**Tag site:** `ClockDilationController::update`, immediately above
`const double slewed = m_scale + std::clamp(desired - m_scale, -m_config.slewPerCall, m_config.slewPerCall);`.

**The prohibition.** Do not replace `m_scale` with `in.currentScale` (the door's scale) in either place on this statement.

**Consequence.** In Shadow nothing is applied, so the door's scale stays 1.0. A law slewing from it would never get past its
first slew step and would report a trajectory On never takes, so a Shadow measurement would stop predicting On (test (g)
fails). In On, a door that refuses or clamps the scale would pin a door-referenced law to its first step for good.

**What breaks if it moves.** `m_scale` is assigned the quantized value on the next statement whenever the change hysteresis
lets it (always while frozen, G-07): the slew back to 1.0 after a freeze starts from it.

## G-05 — the stage applies a scale only when the controller says `apply`

**Tag site:** `clockDilation::beforeClientPump`, immediately above `if (out.apply)`, the header of the statement that calls
`door.setRateScale(out.scale)`.

**The prohibition.** Do not call `setRateScale` unconditionally, on a condition of the stage's own (a scale that differs
from the door's, a mode test), or anywhere else in the stage. `out.apply` is the only gate.

**Consequence.** `apply` is false in every mode but `On` (rationale §10). An unconditional call applies Shadow's scale, so
a Shadow session changes its step rate: it stops being a measurement and becomes the deviation it is meant to measure
before anyone rules on it. The scheduler's own early return on an unchanged scale does not help, because Shadow's scale does
change. The closed-loop Shadow test fails (every frame's door scale must stay 1.0, every window must print `applied=0` and
`rateScale=1.00000/1.00000`); with the condition removed it failed on 7,926 assertions.

**What breaks if it moves.** The window is noted after this statement with the door's scale, so the line's `rateScale` is
the scale the scheduler will pump at. Noted before it, the window would report the previous frame's scale.

## G-06 — a frame longer than `kMaxFrameTicks` is a hitch: never sample it into the frame period

**Tag site:** `ClockDilationController::noteFrame`, immediately above
`if (frameTicks <= clockDilation::kMaxFrameTicks)`.

**The prohibition.** Do not remove or widen this condition, and do not feed a skipped frame's length into the peak by
another path. (How far `kMaxFrameTicks` itself may rise is a `static_assert`: the edges must not cross at the cap.)

**Consequence.** The low edge sits `φ̄(F) − 1` above the guard centre, with F the peak-held frame period (rationale §5.1). A
hitch is a frame of tens of ticks: a 0.6 s frame is 36 ticks. Sampled, it lifts the low edge to about c + 35, far above the
high edge c + 2.5. The peak decays at 2 s, so for seconds after the hitch the target sits tens of ticks above the lead and
the law asks for its full +1 % deviation, toward a lead the clock's own band does not allow. A call after a long gap (a re-entered stage, a paused host)
does the same. `AHitchFrameIsNotSampled` fails with the edge at 37.0 instead of 1.8, and two task-6 cases whose calls are
10 s apart fail with 601.0.

**What breaks if it moves.** `m_lastCallSeconds` must still advance on a skipped call (the statement after the block): the
next frame is measured from the hitch's end, not from the frame before it. The condition must test this call's
`frameTicks`, before the peak is updated.

## G-07 — never hold the scale while frozen: the freeze must land on 1.0

**Tag site:** `ClockDilationController::update`, immediately above
`if (frozen || std::abs(slewed - m_scale) >= m_config.changeHysteresisQuanta * m_config.quantum)`, the header of the
statement that assigns the quantized scale.

**The prohibition.** Do not drop `frozen ||` from this condition, and do not make the change hysteresis apply while a freeze
holds in any other way (a hold counter, a dead band on `desired`, a test after the assignment).

**Consequence.** A freeze asks for exactly 1.0 (G-02) because the clock is moving the lead by whole ticks itself. Held by the
hysteresis, a frozen controller within 2 quanta of 1.0 never gets there: 0.9999 stays 0.9999 for the whole freeze, and the
door keeps dilating while the clock re-centres, pays a debt or resyncs. `TheScaleHoldsUnderTwoQuantaExceptWhileFrozen` fails
with the exemption removed (`0.9999 == 1.0`); in the closed loop the research topic `on-readiness` saw the non-exempt rule
hold a scale off 1.0 through ten or more frozen calls after a 600 ms hitch.

**What breaks if it moves.** `frozen` must be this call's freeze, computed above from `freezeCause`; the condition must stay
the guard of the one statement that assigns `m_scale`, so every frozen call assigns it.

## G-08 — a run of fewer than `kFramePeakFrames` long frames must not enter the frame peak

**Tag site:** `ClockDilationController::sustainedFrameTicks`, immediately above
`return std::min(frameTicks, *reached);`, the value `noteFrame` feeds the peak.

**The prohibition.** Do not return `frameTicks` alone, do not feed `frameTicks` into `m_framePeakTicks` past this function,
and do not replace the order statistic with a running mean or any other filter that takes a single long frame at once or
follows a sustained one only after seconds. (How large `kFramePeakFrames` may be is a `static_assert`: at most the clock's
`gradualCorrectionRate`.)

**Consequence.** Without the filter one frame of 25–40 ms at 100 fps, a frame a loaded host makes every few seconds, lifts the
low edge by 1.2–1.5 tick for the peak's 2 s decay (rationale §5.1). The guard clamps the target above the lead, and the
controller drives the lead up and back for a correction the clock was not about to make: in the closed loop
`IsolatedLongFramesLeaveTheGuardUnbound` fails with 4,378 bound calls, |err| p95 0.477 and 5.64 door changes per second
on one client, and `AShortRunOfLongFramesIsNotSampled` with the edge at 3.4 instead of 1.8 after one 40 ms frame. On the
Unreal host this was G-DIL-1's finding F2 (errAbsMax up to 2.39, 63 changes in one window). A filter that follows the 10 s
mean instead misses a real frame-rate drop: `ASustainedFrameDropStillLiftsTheEdge` fails, with graduated corrections after
a drop from 100 to 30 fps.

**What breaks if it moves.** The frame must be written into `m_recentFrameTicks` before the order statistic is taken, so a
sustained period enters on its `kFramePeakFrames`-th frame; the `std::min` with this frame must stay, so a shorter frame
enters at its own length and the peak decays as task 7b specified (`TheFramePeakDecaysAtTheGuardTau`); and the ring must
hold sampled frames only, after G-06's hitch test, seeded with the first sampled frame.

## G-09 — the `Common` target carries no jitter term

**Tag site:** `ClockDilationController::rawTargetTicks`, immediately above
`return m_config.commonLeadTicks + m_config.targetOffsetTicks;`.

**The prohibition.** Do not add the estimator's jitter, its multiplier or any other per-client link term to the `Common`
target. It is one session constant (plus the test knob `targetOffsetTicks`).

**Consequence.** `Common` exists so that every client of a session holds the same lead; a per-client term makes the leads
differ again by `m·J·f`, the split `OwnRtt` already has, and a jittery client's lead would rise past its guard's high edge
first. og-time-dilation's design keeps the jitter multiplier inside `OwnRtt` and drops it under `Common` (its Q5, for the
user's ruling). `TheCommonTargetCarriesNoJitterTerm` fails: the target 3.7 instead of 2.5 at J 10 ms, m 2 (rationale §3).

**What breaks if it moves.** Nothing else reads the branch; the guard's clamp and the step lag are added after it, in
`update`, for both setpoints.

---

## §R Retired ids

None.
