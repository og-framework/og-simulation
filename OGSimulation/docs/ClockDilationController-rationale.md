<!-- SPDX-License-Identifier: MPL-2.0 -->
<!-- lint-external-ref: StallDebt -- the freeze cause's name until og-input-latency P6-2 renamed it TransitionDebt; kept in the history -->
# `ClockDilationController.h` — rationale

The narrative behind `ClockDilationController`, the client's rate law: from the client's deadline lead over its estimate of
the server's tick, it computes the scheduler rate scale that holds that lead at a target. The same header holds the
per-frame client stage that feeds the law, `clockDilation::beforeClientPump` (§13), and its window statistics,
`ClockDilationWindow` (§14). The header keeps its licence, its docs pointer, its code, its `static_assert`s, one `OG_CHECK`,
nine guard tags and three derivation tags (§5.1, §3.1, §5.4); everything that explains it lives here. The prohibitions are in
`ClockDilationController-guards.md`.

**If this file and `ClockDilationController.h` disagree, the header is authoritative and this file is stale.** Fix this
file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free (the STL, `TimeConfig.h`,
`ClientPredictionClock.h` for `ClockCorrectionPhase`, `TickDriverConcepts.h` for `TickRateDoor`, `TickDriverLog.h` for the
window's line buffer and `OGAssert.h`). The law and the stage never allocate; the window allocates its line buffer once, at
construction (§14). Where a host appears below it is "the host": whatever program owns the scheduler, the clock and the log.

---

## §1 What it is

A pure function with a little state. `update(inputs)` is called once per frame on the thread that owns the client's
scheduler, right before the scheduler pumps. It returns a `ClockDilationOutput`: the scale it wants, whether the caller
should apply it, and the lead, target, error, freeze cause and guard flag it used.

Added by og-time-dilation task 6 (2026-11-21). The per-frame stage that reads the door, the sensor, the estimator and the
clock and calls `update` (§13), and the window that summarises its calls (§14), were added by task 7 the same day. A host
builds the config and calls the stage once per client frame; the Unreal host has done so since og-time-dilation task 8.

The controller does not know the scheduler, the door, the estimator or the clock: the caller copies what they hold into
`ClockDilationInputs`. That keeps the law testable value by value (§12) and keeps og-simulation's types out of it except
`ClockCorrectionPhase`. The stage is that caller.

State, all owned by the one caller thread (§11):

| member | what it holds |
|---|---|
| `m_scale` | the controller's own last scale, quantized and held inside the change hysteresis (§6). Starts at 1.0. |
| `m_filtersSeeded`, `m_targetTicks`, `m_guardTicks`, `m_lastFilterSeconds` | the target's and the guard centre's low-pass filters (§4, §5) |
| `m_lastGuardSeconds` | the clock time of the guard centre's last update: it moves only on a call with a new clock read (§5.4; task 10c) |
| `m_hasPreviousCall`, `m_previousSensorValid`, `m_previousServerTick`, `m_previousReadAdvances` | the previous non-Off call's sensor validity, sensor value and the clock read's count it saw, for the guard sample (§5.4; task 10c) |
| `m_hardResyncs`, `m_hardResyncSeconds`, `m_sawHardResync` | the last hard-resync count seen and when it changed (§9) |
| `m_hasLastCall`, `m_lastCallSeconds`, `m_frameSeeded`, `m_framePeakTicks` | the previous call's clock time and the peak-held host frame period in ticks, for the frame-aware low edge (§5.1; task 7b) |
| `m_recentFrameTicks`, `m_nextRecentFrame` | the last `kFramePeakWindow` (8) sampled frame periods in ticks, a ring, and its next slot: a frame enters the peak only at the period `kFramePeakFrames` (4) of them reach (§5.1; task 7c) |
| `m_frameMeanTicks` | the mean host frame period in ticks, time-weighted at `targetTauSeconds`, for the step lag (§3.1; task 10b) |

## §2 The quantity it controls: the deadline lead, and the error's sign

`leadTicks = predictionTick − serverTick`.
- `predictionTick` is the client's continuous sim tick on its scheduler's deadline grid. The caller computes it from the door
  as `tickOffset + physicsStepCount + (hostNow − nextDeadline) / currentInterval`, so it is "the tick the scheduler will step
  at its next deadline, minus the fraction of an interval still to wait". The frame's phase does not enter it.
- `serverTick` is the sensor's estimate of the server's continuous tick at the same instant (`ServerTickEstimator`).

**The deadline lead is phase-free; the setpoint is not.** A step does not run at its deadline but at the first frame start
at or after it, so the lead a step actually runs at, its **execution lead**, is the deadline lead minus that step's lag. The
setpoints of §3 are execution leads, so the controller compares the deadline lead with the setpoint plus the mean step lag
(§3.1). `targetTicks` is that sum: the target in the deadline coordinate.

`errorTicks = leadTicks − targetTicks`. **Positive means ahead of the target**, so the client must run slower than real time:
the law lowers the scale below 1 (§6). Test (b) pins the sign: flipped, a +1 tick error drives the scale up to 1.002, 1.004,
1.005 instead of down.

Equal leads on every client come from each client holding its own lead against one common clock, the server's. The
controller never sees another client.

## §3 The target, and its two halves

`OwnRtt` (the default):

`L* = max(F, (R + m·J)·f + ½) − (R/2)·f − ½ + targetOffsetTicks`

with R and J the estimator's smoothed RTT and jitter in seconds, m its active jitter multiplier, F `predOffsetFloorTicks` and
f the tick frequency.

It is the clock's own prediction offset, de-quantized and re-referenced from the authority tick to the server's tick:
- The clock aims `max(F, ceil((R + m·J)·f))` ticks ahead of the **received authority tick**. `ceil(x)` exceeds `x` by its
  fractional complement, which averages ½ for a uniformly distributed fraction. So the first ½ replaces the ceiling by its mean.
  That keeps today's mean lead: the controller is meant to remove the lead's spread, not to move its mean.
- The received authority tick is older than the server's current tick by its one-way trip, about `(R/2)·f`, plus the time
  from its publication to its use. It is published at the end of its step, and the client drains it about half a tick later
  on average. The second ½ is that half tick.
- Measured, before this controller existed (og-time-dilation's redesign, from the og-input-latency Ph5 captures, 100 fps):
  the mean lead was **2.453 ticks** on the emulated internet shape and **3.469** on loopback; the rule above gives **2.461** and
  **3.496** from the same runs' R and J, with no fitted constant. At 30 fps the same captures measured 2.767 and 3.913 against
  the rule's 2.427 and 3.497: there the clock parks its lead 0.3–0.4 tick high, and the controller would lower it.

⚠ **The two halves cancel whenever the floor does not bind** (`(R + m·J)·f + ½ > F`): the expression is then
`(R/2 + m·J)·f`. They matter only on the floor, which is every loopback and LAN client: at R = 0.1 ms the rule gives 3.497,
without the halves 3.997. That is why test (e)'s emulated arm (R 72.5 ms, J 9.5 ms: 2.460 either way) cannot see them and its
loopback arm can. Guard G-03.

At (e)'s rounded inputs, R 72.5 ms and J 9.5 ms, the rule gives exactly 2.460, the edge of the acceptance criterion's
2.461 ± 0.001; the 2.461 above is from the unrounded per-run means. The test asserts both bounds.

**What `OwnRtt` holds on an asymmetric link.** The sensor reads the true lead plus `(u − d)/2·f`, half the difference of the
uplink and downlink one-way times (`ServerTickEstimator-rationale.md` §4). Holding `L* = (R/2 + m·J)·f + c` therefore holds a
true lead of `u·f + m·J·f + c`: the uplink plus a margin. Every client's inputs then reach the server with the same margin,
which is what the clock's RTT-based offset aims at today.

`Common` (a deviation from today's behaviour, for the user's ruling; og-time-dilation task 14 enabled it in the Unreal host):
`L* = commonLeadTicks + targetOffsetTicks`, one session constant. The sensor then holds a true lead of `commonLeadTicks +
(u − d)/2·f`.
- **No jitter term** (og-time-dilation design Q5): the target reads neither the jitter nor its multiplier, which stay
  `OwnRtt`'s. `TheCommonTargetCarriesNoJitterTerm` pins it (§12); guard G-09.
- **The default, 3.65 ticks,** is og-time-dilation task 13's pick (its report §4.5, §5). Every client of a session must hold
  the same lead without its guard binding (§5), and the guard's centre moves with the lead on a jitter-free link, so the
  window for the constant is where no client's edge binds: the farthest client's low edge and the lowest-centre client's
  high edge. On task 13's captured shape (10, 30 and 60 ms each way) that window is [3.56, 3.71], and 3.65 is its centre.
  Task 6's 2.5 bound the far client and every loopback client. §21 has task 14's closed-loop measurement of it.
- **What a constant does not cover** (task 13's report §5 item 7): a farthest client past about 133 ms of RTT plus jitter
  margin binds its low edge, and a floored client near 67 ms keeps only about 0.15 tick below its high edge before the
  spreads; a binding client is clamped back toward its own lead, which is graceful but no longer common. At 30 fps the low edge sits a tick higher (§5.1) and task 13's shape binds at
  3.65 (§21).

`targetOffsetTicks` (default 0) shifts either target: a test knob to prove a gate can see a mean-lead change.

### §3.1 The step lag: from an execution-lead setpoint to the deadline coordinate ∴D-02

Both setpoints are **execution leads**: `OwnRtt` reproduces the mean lead the clock held before this controller, measured
at the steps (the mean of `tick − S(t)` over the steps a client ran, t the step's run time and S the server's tick at t),
and `Common` is a lead every client's steps should run at. An input for tick T is sampled in T's step and sent at once, so
its margin at the server is the execution lead, not the deadline lead (og-time-dilation's research topic
`lead-definition`, §3, derived there from the input path).

A host steps at frame starts. Step n, due at its deadline d_n, runs at the first frame start h ≥ d_n, so its execution lead
is `deadline lead − λ_n`, λ_n = (h − d_n)/I ≥ 0, I the step interval. The controller cannot see λ_n (it is called once per
frame, before the pump), but its mean over the steps is fixed by the frame period alone: over the frame grid's phase
against the deadline grid, a deadline falls uniformly inside its frame, so it waits on average half a frame. For frames of
T ticks, the mean lag is T/2 for any ratio of frame to tick (at 100 fps, frames of 0.6 tick: 0.3; at 30 fps, frames of
2 ticks: 1.0; of a frame's two steps the earlier deadline waits longer, 1.5 against 0.5 on average). So:

`meanStepLagTicks() = F̄ / 2`, and the target in the deadline coordinate is `setpoint (filtered) + F̄ / 2`.

- **F̄ is a time-weighted mean.** `noteFrame` moves `m_frameMeanTicks` toward each frame's ticks by
  `1 − exp(−frame seconds / targetTauSeconds)`, the gain of the peak's decay with the target's time constant. Weighting each
  frame by its duration makes F̄ converge to E[T²]/E[T], and E[T²]/(2·E[T]) is exactly the mean wait of a deadline that falls
  uniformly in time among frames of varying length (a long frame catches more deadlines). For jittered frames this is
  larger than half the mean frame period: at ±20 % uniform jitter by a factor 1 + 0.2²/3 = 1.013. A count-weighted mean
  would miss that factor.
- **The same frames as the peak.** The mean samples exactly the frames the peak samples: a frame longer than
  `kMaxFrameTicks` is a hitch and is not sampled (guard G-06). It is seeded with the first sampled frame, and before any
  frame is sampled the lag is 0.
- **Added after the target filter, before the guard.** The setpoint is filtered (§4) and the lag added to the filtered value,
  so the lag is low-passed once, by its own mean. The guard clamps the sum (§5): it bounds the deadline lead, whatever the
  setpoint.
- **What it does not hold.** The per-step lag of one client sits wherever that client's frame grid sits against its deadline
  grid; on a frame-capped host the two grids share a period g = gcd(frame, tick) (3.33 ms at 100 fps, 16.67 ms at 30 and
  60 fps), and the client's mean lag is `x + (T_frame − g)/2` for a phase x ∈ [0, g). F̄/2 is that mean averaged over x.
  A client parked at one x holds an execution lead within ±g/2 of the setpoint: ±0.1 tick at 100 fps, ±0.5 at 30 fps. Frame
  jitter randomises x per frame and removes that stair.

Before og-time-dilation task 10b the target had no lag term: the controller held the deadline lead at the execution-lead
setpoint, so each client's steps ran F̄/2 below it. The Unreal Shadow captures of G-DIL-0 measured that offset per client
(0.22–0.41 tick at 100 fps, 1.44–1.52 at 30 fps, the stair's two ends); the closed loop measured it as −0.305 at 100 fps
and −1.012 at 30 fps (§18).

## §4 The target's low-pass

The raw target moves with the RTT and jitter EMAs: on the captures og-time-dilation's redesign measured, the RTT-based target wandered
with an sd of 0.13–0.16 tick per 1 s window. It is low-passed with a time-step-correct gain `1 − e^(−Δt/τ)`, τ = `targetTauSeconds` (10 s), so the gain is independent of the
frame rate. Δt is the clock time since the previous filter update.

The filter is seeded with the raw value at the first call whose sensor is valid, and moves only on calls whose sensor is
valid. After a long invalid stretch Δt is long and the gain near 1, which is a re-seed in effect. Before the first valid call
the reported target is the raw value and no guard applies; the controller is frozen then anyway (§9).

## §5 The band guard

The clock corrects its own lead only outside a quiet band: its drift is `targetTick − predictionTick` with `targetTick =
authorityTick + predictionOffsetTicks` (A + O), and it corrects nothing while `|drift| ≤ softDriftThresholdTicks` (3).
Outside the band every advance counts up a graduated counter, and every `gradualCorrectionRate`-th (4th) out-of-band advance
is a Skip or a Stall. The counter is reset only by that correction or by a hard resync, **never inside the band**, so
isolated out-of-band reads accumulate into a Skip.

The guard's **centre** c is the clock's read target against the sensor: `A + O − Ŝ`, i.e. `O − age` with
`age = serverTick − authorityTick`, low-passed. Since og-time-dilation task 10c its `A + O` is the one the clock itself read,
published by the clock, and its Ŝ the sensor of the stage call before that read (§5.4); the stage's own `A` only seeds it.
The controller's lead error against it is `e = L − c`, with L the stage's lead. **The low edge depends on the frame period; the high edge does
not.** The guard clamps the target to `[low, high]`:
- `low = c + (φ̄(F) − 1) + guardLowTicks`, with `guardLowTicks` = −0.5;
- `high = c + guardHighTicks`, with `guardHighTicks` = 2.5.

`guardBound` reports that the clamp changed the target this call.

### §5.1 The frame-aware low edge ∴D-01

The clock does not read the stage's lead. It reads the drift of each advance, against the tick it last stepped (pred). A
host frame due F steps runs them back to back at one A and one O, so the frame's drifts are drift₁, drift₁ − 1, …,
drift₁ − (F − 1). The frame's **first** advance reads the highest drift.

- **The stage's coordinate.** The stage's lead is the deadline lead, `P = tickOffset + count + (host − next)/I` (§13). After
  each step the host publishes `tickOffset = pred − stepIndex`, so `P − pred = φ = 1 + (host − next)/I`. With F steps due,
  `host − next ∈ [(F − 1)·I, F·I)`, so **φ ∈ [F, F + 1)**.
- **The first read.** `drift₁ = (A + O) − pred = φ − e + δc`. δc is the instantaneous offset-and-age deviation from the
  low-passed c: an O step, or a fresh authority tick. A Skip needs reads above 3, i.e. `e < φ + δc − 3`. So the clock's low
  edge in the controller's coordinate is about `F + δc_max − 3` (the worst φ is F + 1). At 100 fps (F = 0.6 tick: only
  frames that step advance, so φ ∈ [1, 1.6)) and at 30 fps (F = 2, φ ∈ [2, 3)) the same lead gives first reads about
  1.2 ticks apart.
- **φ̄(F), the mean first-read phase,** is `meanFirstReadPhaseTicks()`. For F ≥ 1, φ is uniform over [F, F + 1), mean
  F + ½. For F < 1, only the frames that step advance the clock, with φ ∈ [1, 1 + F), mean 1 + F/2. The two meet at F = 1
  (1.5). Before any frame is sampled F is 0, so φ̄ = 1 and the edge is `c + guardLowTicks`.
- **What the edge holds.** At the edge `e = φ̄ − 1 + guardLowTicks`, so the clock's mean first read is
  `φ̄ − e = 1 − guardLowTicks` = **1.5**, the same at every frame rate.
- **The headroom.** Above that mean, the reads carry two kinds of noise that the 2 s low-pass of c hides:
  - O, `max(predOffsetFloorTicks, ceil((R + m·J)·f))`, sits near a rounding boundary on b0-like links and steps 5 → 6 on 1–2 % of frames, raising every
    read by a whole tick at once;
  - the authority tick's age ranges about 1.7–6.6 ticks.

  Both are the research topic `thirty-fps-guard`'s harness measurements, over 130 s per client.

  A ±1 % controller moves the lead by at most 0.6 tick per second, so it cannot react in time to a read that the 4-read
  counter turns into a Skip within a few frames. The edge therefore keeps standing headroom: one O quantum (1 tick) plus the
  age-and-phase spread (½ tick) above the 1.5 mean, 1.5 + 1 + ½ = 3 ≤ `softDriftThresholdTicks`. The headroom
  `static_assert` (§5.3) checks exactly this. At −1.0 the guard gives 32 steady corrections over 16 clients × 120 s at 30 fps (task 7b's
  seen-failing run), and at −0.75 the research's low-passed variant gave 8 at 45 fps. Both values fail the assert.
- **Values.** 30 fps (F = 2): `low = c + 1.0`. 100 fps (F = 0.6): `c − 0.2`. 144 fps (F = 0.42): `c − 0.29`.

**F, the frame period in ticks,** comes from the calls themselves: `(clockNowSeconds − the previous call's) · f` (no new
input; `noteFrame`). It is the **peak-held sustained** frame period. Each sampled frame first passes the sustained filter
(`sustainedFrameTicks`, task 7c): it enters as `min(frame, the kFramePeakFrames-th largest of the last kFramePeakWindow
sampled frames)`, this one included, i.e. at most at the period that 4 of the last 8 frames reached (guard G-08). Then a
longer value is taken at once; a shorter one pulls the held value toward itself by `1 − exp(−Δt / guardTauSeconds)` per
call, with Δt the frame's own length, so the hold decays at 2 s. A switch from 30 to 100 fps leaves F at `0.6 + 1.4/e` =
1.115 after 2 s: a shorter frame always enters at its own length, so the decay is the one task 7b specified. The edge
follows the worst recent sustained period because a frame-paced host jitters: a ±20 % jittered 30 fps host runs some
3-step frames, and their first read sets the Skips. In the research harness, the peak gave 2 steady corrections over 16
clients against Off's 9, where a plain low-pass of F gave 5; the sustained filter keeps the 2 (§20).

**Why a short run of long frames does not move the edge, while a sustained period does** (og-time-dilation task 7c, from
G-DIL-1's finding F2):
- **One long frame raises one read.** A frame due F steps runs them at one A and one O, so only its first advance reads
  the frame's raised phase φ ∈ [F, F + 1); each later advance reads one tick lower (above). At 100 fps the edge holds the
  1-step frames' φ̄ = 1.3. A 25–40 ms frame (1.5–2.4 ticks, 1 to 3 steps due, mostly 2) raises that one read to about
  φ̄(2) = 2.5, 1.2 ticks higher. The `OwnRtt` target sits about half a tick above the 100 fps edge (c + 0.31 against c − 0.2,
  §5.2), so the clock's mean first read is about 1.0 there and the long frame's about 2.2: inside the band (|drift| ≤ 3).
  It leaves the band only when an O step or an age-tail read lands on the same advance.
- **A correction needs four of them.** The clock fires one graduated correction per `gradualCorrectionRate` (4)
  out-of-band advances (§5). A run of fewer than 4 long frames gives the clock at most 3 raised reads, so on its own it
  cannot complete a correction. It can only add to a count already held (the counter is never reset inside the band),
  and each of its reads leaves the band only on that coincidence.
- **A sustained period raises every read.** When most frames are long, every frame's first read is raised, so four
  out-of-band reads accrue in four frames: that is the period the edge must follow. The filter takes such a period
  once 4 of the last 8 frames reach it, a delay of at most 4 frames. That costs nothing measurable, because the controller
  moves the lead at most 0.6 tick/s, so its answer to a frame-rate drop takes seconds either way: a drop from 100 to 30
  fps gives 0 corrections and nearly the same guard binding as the unfiltered peak (20,198 against 20,246 bound calls,
  §20). A filter that follows the 10 s mean
  instead is too slow for it (§12's `mean_fed` probe: 4 clients correct during the drop).
- **What the unfiltered peak did.** It took every frame under the 3-tick cap at once, so one 25–40 ms frame lifted the
  edge by φ̄(F_long) − φ̄(F) ≈ 1.2–1.5 tick for the peak's 2 s decay. The guard clamped the target above the lead and
  the controller drove the lead up and back down: in the closed loop, a frame every 3 s bound the guard on 74,084 calls
  over 16 clients, with |err| p95 up to 0.54 and 6.03 door changes per second, for no correction the clock was about to
  make (0 with the filter, 0 without it). G-DIL-1 saw the same in UE under host load (errAbsMax up to 2.39, 63 changes in
  a window).
- **Why 4 of the last 8.** `kFramePeakFrames` = 4 is the clock's own count, and a `static_assert` keeps it at or below
  `TimeConfig{}.gradualCorrectionRate`: a rejected run (at most 3 long frames) must stay below the count. A window of
  twice that lets an alternating period (every other frame long) enter. G-DIL-1's client logs held runs of up to 3
  multi-step frames within 8 frames, so a filter of 2 or 3 would still take some of them (§12's `count2` and `count3`
  probes).
- **Its limit.** Isolated long frames that come four times a second are a jittered frame period, not isolated: none
  enters, and the closed loop counts 7 corrections over 16 clients × 120 s, against the unfiltered peak's 1 and Off's 6
  (§20). A run of 4 or more long frames enters, by design.

**A frame longer than `clockDilation::kMaxFrameTicks` (3 ticks, 50 ms, below 20 fps) is a hitch, not a frame rate, and is
not sampled** (guard G-06). The cap also bounds the edge: at F = 3 the low edge is `c + 2.0`, below the high edge `c + 2.5`.
The second `static_assert` checks that the edges never cross. Below 20 fps the band cannot be held by this guard; in the
harness at 20 fps, On gives 4 steady corrections over 16 clients, about Off's 5.

`noteFrame` runs on every call that is not Off, whether or not the sensor is valid and whether or not a freeze holds, in
Shadow as in On. So Shadow's would-be target, and its trajectory (§10), equal On's. The same call updates the mean frame
period of the step lag (§3.1).

### §5.2 The high edge, and where the guard binds

The high edge is fps-independent. The frame's **last** advance reads `φ − (F − 1) − e + δc`, with `φ − (F − 1) ∈ [1, 2)`
at every F. So at `high = c + 2.5` that read's mean is 1.5 − 2.5 = −1, two ticks inside the band's −3. A `static_assert`
keeps `guardHighTicks ≤ 1 + soft − ½`. The research harness also found it marginal under a `Common` target
above the clock's lead: 6 steady corrections at `commonLeadTicks` 6 at 100 fps, from the age tail (up to 6–7.5 ticks).

**Re-derived for `Common` (og-time-dilation task 14): the edge stays 2.5.** Under `OwnRtt` it never binds; under `Common`
it binds on a client whose centre is low (a floored client at a mid RTT, task 13's 30 ms one).
- **Its range.** The edges must not cross at the longest sampled frame (§5.3): `guardHighTicks ≥ kMaxFrameTicks + ½ − 1 +
  guardLowTicks` = 2.0. So without a new low edge the high edge can move only within [2.0, 2.5]; 1.5 does not compile.
- **Where the clock corrects.** The edge holds the mean of the frame's last read two ticks inside the band (−1 against −3,
  above); the clock corrects only when single reads leave it often enough (every `gradualCorrectionRate`-th out-of-band read, §5). That depends on the read's
  tail, not on the mean: measured at the edge in the closed loop (§21), a jitter-free link with b0's loss makes 0
  corrections, og-net's shaper b0 (uniform 15–50 ms) 0 without loss and 0.37 Stalls a minute with it, and the harness's
  exponential b0 1.7–1.8 a minute. No edge in [2.0, 2.5] separates them: at 2.0 the jittered links still correct at the edge,
  and task 13's 30 ms client binds at the compiled 3.65, so `Common` no longer holds that shape.
- **At the compiled 3.65 no client of task 13's shape reaches the edge** at 100 or 60 fps, on fixed or shaper-b0 links, and
  the clock makes 0 corrections. The exponential-tail link corrects even below the edge (0.24 Stalls a minute at 3.65, 0.005
  under `OwnRtt`): a cost of a high common lead on a heavy-tailed link, which only an edge that reads the link's tail could
  remove, and such an edge needs the low edge re-derived with it (og-time-dilation's task 16 list).

Under `OwnRtt` the setpoint is the clock's own objective de-quantized (§3). In the harness it lands between about c − 0.06
(30 fps) and c + 0.01 (100 fps), whatever the frame rate; the step lag (§3.1) then raises the target by F̄/2, to about
c + 0.94 at 30 fps and c + 0.31 at 100 fps. Before task 10b's lag the guard rarely bound at 100 and 144 fps (edge c − 0.2
and c − 0.29), bound on part of the frames at 60 fps (edge c + 0.0) and on every frame at 45 and 30 fps (edge c + 0.33 and
c + 1.0), and the lead parked at the edge: at 30 fps a mean of 3.38 ticks, against Off's 3.75 and the setpoint's 2.37. With
the lag the 30 fps target sits just under the edge, which still binds: the mean lead is 3.41 (§18). The guard holds the
band at the cost of that lead. The guard also exists so that a `Common` target, or a large
`targetOffsetTicks`, can never drive the clock into its graduated zone, where the clock's Skips and Stalls would fight the
controller.

c is low-passed at `guardTauSeconds` (2 s): each sample jumps by a tick with the phase of the authority tick the clock read
and with O's rounding. It is seeded at the first call whose sensor is valid, like the target (§4), and moves only on a
call that sees a new clock read (§5.4).

### §5.3 The `static_assert`s

The `static_assert`s on the defaults tie the guard to the clock. Each reads `TimeConfig{}`, so retuning a side's default
without the other fails to compile:
- `1 − guardLowTicks + 1 + ½ ≤ soft`: the headroom (§5.1). It replaced task 6's `guardLowTicks ≥ 1 − soft + ½`, which
  measured the low edge from c alone;
- `kMaxFrameTicks + ½ − 1 + guardLowTicks ≤ guardHighTicks`: the edges never cross;
- `guardHighTicks ≤ 1 + soft − ½`;
- `guardLowTicks < guardHighTicks`. The second assert implies it at the defaults; it is kept for any change to
  `kMaxFrameTicks`;
- `1 ≤ kFramePeakFrames ≤ kFramePeakWindow` and `kFramePeakFrames ≤ TimeConfig{}.gradualCorrectionRate`: the sustained
  filter's count is a count inside its window, and a run it rejects stays below the clock's graduated count (§5.1;
  og-time-dilation task 7c).

They check defaults only. A host that sets the guard edges at runtime must keep them inside the same inequalities.

### §5.4 The centre is the clock's read, against the sensor at the stage before it ∴D-03

**What the guard bounds.** With `c = (A + O) − Ŝ` and `L = P − Ŝ`, the guard's condition `L − c ∈ [low, high]` is
`P − (A + O) ∈ [low, high]`: the deadline tick against **the target tick the clock reads**. Ŝ cancels; it only translates
the bound into the lead coordinate. So c is right only when its `A + O` is the one the clock's next advance reads, and the
first-read derivation of §5.1 (`drift₁ = φ − e + δc`) holds only for that c.

**Why the stage's own A is wrong.** A client drains its authority ticks (and SimClock samples) at each
step's start, after the stage (the tick driver's `receiveForStep`). The stage's A is then the A of the clock's previous read,
and every authority tick that arrived since is missing: c reads low by the arrivals of one frame, F ticks when every frame
steps and the mean time since the last stepping frame below that. Measured in the closed loop (§19): −2.000 tick at 30 fps,
−1.000 at 60, −0.847 at 100, −0.725 at 144. Until og-session-runtime task 13 a client could also drain before the stage,
where the stage's A was right; the centre was built not to depend on the order, and that stays.

**The sample.** The clock publishes the target of its last advance with the advance count (`publishedTargetRead()`,
`TimeConfig-rationale.md` §18.1); the stage copies it into `ClockDilationInputs::clockRead`. A call whose count differs from
the previous call's sees a new read, and its sample is

`c sample = clockRead.targetTick − Ŝ(previous call)`.

- **Why the previous call's Ŝ.** Let the read be `A′ + O`, made by the batch of frame n − 1 (after stage n − 1, before stage
  n). Its drift is `(A′ + O) − pred₀ = ((A′ + O) − Ŝ_{n−1} − L_{n−1}) + (P_{n−1} − pred₀) = φ_{n−1} − e_{n−1}` with
  `c = (A′ + O) − Ŝ_{n−1}`: §5.1's `drift₁ = φ − e`, exactly, with φ and e measured at the stage that preceded the read, in
  either drain order and for any delay between that stage and the step. The stage that preceded the read is the previous
  call, because the count changed after that call's load.
- **Pairing with this call's Ŝ is wrong by one frame.** `(A′ + O) − Ŝ_n` is low by the sensor's step over the frame, F
  ticks, in both orders: the unit case's seen-failing probe reads 0.0 where the centre is 2.0 at 30 fps.
- **The approximation.** A batch that overlaps the next frame (a worker step still running when the next stage loads the
  word) pairs one read one frame late: c is low by F on that one sample, which the 2 s low-pass reduces to its weight.
- **No new read, no update.** A 0-step frame, or a call that loads the word before the batch's first advance, sees the same
  count: the centre holds, bit-identical, whatever the stage's own A or Ŝ (the unit case changes both on such a call). The
  update's Δt is the clock time since the centre's last update (`m_lastGuardSeconds`), so the 2 s low-pass runs in time, not
  in calls.
- **The seed.** The first call whose sensor is valid seeds the centre. If it already has a sample it seeds with it; the
  first valid call never does, because a sample also needs the previous call's sensor to have been valid (the previous Ŝ
  must be a real estimate). It then seeds with the stage's own `O − (Ŝ − A)`, the formula before task 10c, which survives
  only as that one seed sample, inside the join's freeze (§9).
- **Off** returns before any of this (§10): no sample, no previous call recorded. The stage still loads the word, and
  nothing reads what it loaded.

## §6 The law, the slew and the quantum

`desired = 1 − clamp(gainPerTick · error, −maxDeviation, +maxDeviation)`, or exactly 1.0 when frozen (§9).

- **Proportional**, `gainPerTick` K_p = 0.005 per tick: a one-tick error asks for 0.5 % slower. The time constant is
  `1/(f·K_p)`, 3.3 s at 60 Hz. It saturates at |error| = maxDeviation/K_p = 2 ticks, at ±1 % (`maxDeviation` 0.01).
- **Slew:** the scale moves at most `slewPerCall` (0.002) per call from the controller's **own previous scale**, so a freeze
  returns to 1.0 over a few frames, not in one, and a cause clearing ramps back the same way.
- **Quantum:** the slewed value is rounded to a multiple of `quantum` (1e-4). The caller applies a scale only when it differs
  from the door's (`apply`), so the scheduler's rate history changes only on a quantized step. A 1e-4 change needs an error
  change of 0.02 tick. The slew default is a whole number of quanta (20), so the rounding never lets a step exceed it.
- **Change hysteresis** (`changeHysteresisQuanta`, 2.0; og-time-dilation task 10c): the controller keeps its scale until the
  slewed value is at least 2 quanta away from it, and then takes the rounded slewed value. **While frozen it always takes
  it** (guard G-07), so a freeze still lands on exactly 1.0.
- A +1 tick error therefore slews 1.0 → 0.998 → 0.996 → 0.995 and stays (test (b)); ±3 ticks saturate at 0.990 / 1.010 (c).
  Every one of those steps is at least 10 quanta, so the hysteresis never holds them.

**Why the quantum alone does not stop churn.** The quantum is not a dead band: with K_p = 0.005 the rounded scale changes on
every crossing of ε over an odd multiple of 0.01 tick. In On, ε sits near 0, i.e. on those boundaries, and a b0 link's
per-frame ε noise is 2.5–5 quanta wide, so nearly every 20 Hz sensor update can cross one. The closed loop measured 3.3–3.4
door changes per client per second at 60–144 fps and 7.9 at 30 fps (where the guard-bound target also carries c's noise),
against G-DIL-1's G4 bound of 2/s (research topic `on-readiness`, §4; this task's seen-failing arm, §19).

**Why 2 quanta, and what it costs.** The scale may now rest anywhere within ±2q/K_p = **±0.04 tick** of ε around the value
the law asks for. That is far inside every gate (G1's 0.25, the 0.10 drift bound), and on a noiseless sensor it turns into a
slow limit cycle of 0.08–0.17 changes per second. One quantum already meets the 100 fps bound but leaves 30 fps at 2.9/s;
2 quanta is the smallest that holds every unjittered 30 fps client under 2/s. Against it, a dead band on ε barely helps and
raises the drift error ×1.8, a minimum hold breaks the hitch law, a coarser quantum raises the drift error ×1.65 and an ε
filter limit-cycles (the research's candidates). The hysteresis also biases the hitch law's check slightly (§19). At 30 fps
with ±20 % frame jitter churn stayed near 3.9/s: its source is the low edge's peak-held frame period, not the sensor
(recorded, not gated). Task 7c's sustained frame filter brings it to 3.3/s (§20).

**Why the freeze is exempt.** Without the exemption a frozen controller near 1.0 stops one quantum short (0.9999 stays
0.9999: the unit case's seen-failing probe), and in the closed loop the research saw a scale held off 1.0 through ten or more
frozen calls after a 600 ms hitch (1 of 10 runs at 100 fps; 0 of 10 with the exemption). A freeze means "the clock is moving the lead itself: rate 1.0", so it must reach 1.0.

**Shadow** keeps the same state, so its would-be trajectory still equals On's (test (g)); `wouldChanges` counts the new,
rarer changes.

**Why from its own previous scale, not the door's** (guard G-04). In Shadow nothing is applied, so the door stays at 1.0: a law
slewing from the door's scale would never leave the first slew step, and Shadow would report a trajectory On never takes.
Slewing from its own state makes Shadow's sequence equal On's on the same inputs (test (g)), which is what makes Shadow a
measurement of On. In On a refused or clamped door would make a door-referenced law repeat its first step forever.

`static_assert`s: `quantum > 0`; `0 < slewPerCall ≤ maxDeviation`; `changeHysteresisQuanta ≥ 0.5` (below half a quantum
the rounding already holds the scale, so the rule would do nothing) and `changeHysteresisQuanta · quantum ≤ slewPerCall` (so
a full slew step always passes, and the slew cases are unchanged: 2 · 1e-4 against 2e-3).

## §7 The bracket bound

The display interpolates between published render snapshots at a render time `hostNow − delaySteps · dt/s`, where `dt/s` is
the scheduler's current step interval (`TickDriverPresenter.h`; `delaySteps` is clamped to at most 2, and the snapshot channel
holds 4). The oldest of the 4 snapshots is three intervals older than the newest. Just after a publish, the render time has an
older snapshot to pair with iff those three intervals exceed `2 · dt/s_now`:

`3 · dt/s_past > 2 · dt/s_now  ⇔  s_past / s_now < 1.5`

With the scale bounded to `[1 − d, 1 + d]` the worst ratio is `(1 + d)/(1 − d)`, below 1.5 iff **d < 0.2**. The default
`maxDeviation` 0.01 gives 1.0202. So dilation inside the clamp can never cause a `beyondOldest` render frame. The scheduler's
own bounds (0.75–1.25, set by the tick driver) give 1.667 and would not; the controller's clamp is what keeps the scale
inside the bracket's limit. A `static_assert` ties the default to the bound (guard G-02 covers the clamp itself).

At ±1 % one quantum moves the render time by `2 · dt · Δ(1/s)`, about 3.3 µs; one slew step by about 67 µs, against a 7–33 ms
frame advance. The render time never runs backwards.

## §8 Stability, the drift error, and why there is no integrator

`dε/dt = f · (s − 1) + f · δ`, with δ the client clock's rate mismatch against the server's (positive = the client's clock
runs fast). Neither the sensor's server tick nor the target depends on s, so the loop is first order with no transport
delay. With `s = 1 − K_p · ε` it is `dε/dt = −f · K_p · ε + f · δ`: stable for any K_p > 0 at a per-frame update with
`f · K_p · Δt_frame ≪ 1` (0.01 at 30 fps, 0.003 at 100 fps).

Under a constant drift the error settles at **ε = δ / K_p**: 100 ppm gives 0.02 tick. That is far inside every gate, so no
integral term is needed. An integrator would remove that 0.02 tick but would add a second state, overshoot after a freeze (it
would wind up against the frozen scale or need its own freeze logic), and a slower loop. Guard G-01.

Noise: with an error sd of about 0.04 tick (the sensor about 0.03, the filtered target about 0.02) the scale jitters by about
±0.02 %.

## §9 The freeze list, and why each cause exists

While a freeze holds, the desired scale is 1.0 and the slew carries the scale there (test (d)). The first matching cause is
reported, in this order:

| cause | input | why |
|---|---|---|
| `SensorInvalid` | `!sensorValid` | the server-tick estimate is unseeded or stale (the estimator is valid after 20 accepted samples and stale after 1 s with none). With no server tick there is no lead. App pauses land here: samples stop. |
| `Recentre` | `phase.recentreArmed \|\| phase.recentring` | the clock's one-shot re-centre is reading its median lead or paying whole ticks through Skips and Stalls. A rate change inside its reading window would move the median it decides on; during payment the clock is moving the lead itself. |
| `TransitionDebt` | `phase.transitionDebt` | the clock owes an input-delay transition: Stalls for a raised delay, Skips for a lowered one (og-input-latency P6-2). Either way it is deliberately moving the lead by whole ticks. Named `StallDebt` / `phase.stallDebt` until P6-2, when the clock's debt became signed and the bit came to mean "owed, either sign". |
| `AwaitFirstRtt` | `phase.awaitsFirstRtt` | a hard resync waits for the first RTT reading; the next advance may snap the clock. |
| `AfterResync` | the hard-resync count changed less than `freezeAfterResyncSeconds` (1 s) of `clockNowSeconds` ago | a hard resync snaps the clock and re-arms the re-centre; the 1 s lets the post-resync estimates settle before the re-centre's own phase freezes take over. |

The phase is the clock's `publishedCorrectionPhase()`, written by the clock's thread and safe to read from any thread
(`TimeConfig-rationale.md` §18). The hard-resync count is modulo 2²⁴. The controller treats any change of it as a resync,
starting from 0 at construction, so a controller built after a resync sees its first call as a resync (one extra second of
freeze, never a missed one). The count is tracked on every non-Off call, whichever cause is reported, so the window starts
at the call that first saw the change: frozen while `now − t_change < 1.0`, released at exactly 1.0 s (test (d)).

The freeze stops the controller from fighting a mechanism that moves the lead by whole ticks. Those mechanisms stay in
priority: hard resync, the input-delay transition debt (Stalls or Skips), the re-centre, the graduated Skips and Stalls beyond the band, and the band itself.

## §10 Mode, Shadow, `apply`, and the authority

- `Off`: `update` returns `{scale 1.0, apply false, freeze ModeOff}` and touches no state: no filter, no resync tracking, no
  frame period, no scale. The reported lead, target, error and step lag are 0.
- `Shadow`: every computation of On, every state change of On, and `apply` always false. Its scale sequence is the one On
  would apply (§6).
- `On`: `apply = (scale != currentScale)`, with `currentScale` the door's current scale. The caller calls `setRateScale`
  only when `apply` is true.

**Never apply a scale on the authority.** The server's step rate is the reference every client's lead is measured against.
The controller has no role input, so this is the caller's rule, and the stage enforces it: its first statement is
`OG_CHECK(manager.runsPrediction(), …)` (§13). An authority's manager holds no estimator and no prediction clock to read
anyway. The tick driver's door also refuses `setRateScale` on the authority and counts the refusal, so a wrong caller that
reaches the door some other way is visible in the driver's diagnostics rather than silent. (On a refusing door, `apply`
stays true every call, because the door's scale never reaches the controller's.)

## §11 Threads and allocation

One thread: the caller's, which owns the scheduler. No atomics, no locks, no allocation; every member is a scalar. The
inputs are copied by the caller from objects with their own threading contracts (the estimator's relaxed atomics, the clock's
published phase word).

The stage and the window run on that same thread, the one that owns the scheduler and calls the tick driver's
`beforePump`, once per frame before `pump`. On an immediate client that is not the thread that writes the estimator
(the step drains the SimClock samples) or the clock's phase word (the step advances the clock), which is why the stage reads
only their cross-thread-safe outputs: `serverTickEstimator()`'s relaxed atomics (count acquired first), the estimator's
smoothed RTT, jitter, authority tick and offset, `publishedCorrectionPhase()` and `publishedTargetRead()` (one 64-bit word,
so its count and target are always one advance's; §5.4). Each read is consistent on its own, not
with the others; a call that sees a sample's offset but the previous sample's count differs from the next call by one
sample's weight, far below the quantum. The door's `tickOffset` is the driver's relaxed copy of the last step's offset,
one batch stale at worst; a Skip or Stall still in flight moves the computed lead by one tick for one frame, about 0.5 %
of scale before the slew, which the slew limits to 0.2 % and the next frame undoes.

## §12 What the tests pin

`ClockDilationControllerTest.cpp` (og-simulation-tests, `[PCTM][Dilation][ClockDilationController]`), cases (a)–(g) of the
task's acceptance criteria plus the freeze order and the target filter, task 7b's three frame-period cases (the low
edge at 30, 100 and 144 fps, the peak's decay, the hitch skip) and task 10b's step-lag case (F/2 at four frame periods, a
40 ms frame moving the mean by less than 0.01, an over-cap frame leaving it unchanged, Off's zero) and task 10c's two
cases (the guard centre in both drain orders, §5.4; the change hysteresis and its freeze exemption, §6) and task 7c's
`AShortRunOfLongFramesIsNotSampled` (runs of 1, 2 and 3 frames of 40 ms at 100 fps leave the low edge at c − 0.2; the 4th
consecutive one, or 4 alternating ones in 8, lift it to c + 1.4; §5.1) and task 14's `TheCommonTargetCarriesNoJitterTerm`
(§3). Each seen-failing probe was a temporary edit of this
header, restored byte-identical:

| probe | result |
|---|---|
| the error's sign flipped | (b), (c) and both freeze cases red: a +1 tick error slews to 1.002, 1.004, 1.005 |
| both ½ terms removed | (e)'s loopback arm red, 3.997 against 3.497; its emulated arm and every other case green (§3) |
| `maxDeviation = 0.25` | compile error: the bracket `static_assert` (§7) |
| `guardLowTicks = −2.5` | compile error: task 6's low-edge `static_assert` (§5.3; task 7b's headroom assert, which replaced it, rejects it too: 1 + 2.5 + 1.5 = 5 > 3) |
| task 7b: `guardLowTicks = −1.0` | compile error: `error C2338: static_assert failed: 'ClockDilationConfig: the band guard's low edge caps the clock's mean first-advance read at 1 - guardLowTicks; …'` (the headroom, §5.3). With that assert disabled, the closed loop's 30 fps case is red: 3–5 Skips on 8 of 16 clients, pair p95 0.68–0.85, 32 corrections |
| task 7b: `guardLowTicks = 0.1` | compile error: `static_assert failed: 'ClockDilationConfig: the frame-aware low edge at the longest sampled frame, … must not cross the high edge, …'` (§5.3) |
| task 7b: the low edge as task 6 wrote it (`m_guardTicks + -1.5`, no frame term) | the closed loop's four frame-rate cases red, 72 assertions: at 30 fps 4–8 Skips per client and pair p95 0.66–0.91 with the guard never binding; 22 of the 24 any-target runs, 10 of them at 100 fps (5–23 corrections); 45 fps; 153 jittered corrections against Off's 9 |
| task 7b: the hitch skip removed (`frameTicks <= 1e9`) | `AHitchFrameIsNotSampled` red: the low edge 37.0 against 1.8 after one 0.6 s call (§5.1, guard G-06) |
| task 10b: the header as it was before the step lag (no `m_frameMeanTicks`, target = filtered setpoint; the tests' `stepLagTicks` read as 0) | `TheTargetGainsHalfTheMeanFramePeriod` red, 9 assertions: the target offset over the raw setpoint reads 0.000 at F = 0.6, 1.0, 2.0 and 0.417 ticks, and one 40 ms frame moves the lag by 0.0; the closed loop's `ExecutionLeadMeetsTheTarget` red (§18) |
| task 10c: the guard sample as before (the stage's `O − (Ŝ − A)` on every valid call) | 7 cases red, 85 assertions: `TheGuardCentreIsTheClocksReadInEitherDrainOrder` (step-start centre 0.0 against 2.0, edges 1.0 / 2.5 against 3.0 / 4.5); the closed loop's step-start arms (§19) |
| task 10c: the read paired with this call's Ŝ (`- in.serverTick`) | `TheGuardCentreIsTheClocksReadInEitherDrainOrder` red, 8 assertions: the centre 0.0 against 2.0 in **both** orders, low by the call's Ŝ step |
| task 10c: the hysteresis without the freeze exemption | `TheScaleHoldsUnderTwoQuantaExceptWhileFrozen` red, 3 assertions: a frozen call from 0.9999 returns `0.9999 == 1.0`, apply false |
| task 10c: `changeHysteresisQuanta = 0.5` (the rule inert) | the closed loop's `QuantumChangesStayUnderTheBound` red, 69 assertions: 3.37 door changes per client per second at 100 fps (worst client 3.70), 7.89 at 30 fps (§19) |
| task 10c: `changeHysteresisQuanta = 25` | compile error: `error C2338: static_assert failed: 'ClockDilationConfig: the scale holds until the slewed value is changeHysteresisQuanta quanta away. …'` |
| task 7c: `raw_frame`, the sustained filter removed (`return frameTicks;`, task 7b's peak) | 2 cases red, 202 assertions: `AShortRunOfLongFramesIsNotSampled` (one 40 ms frame: the edge 3.4 against 1.8) and the closed loop's `IsolatedLongFramesLeaveTheGuardUnbound` (client 1/101: 4,378 bound calls, \|err\| p95 0.477, 5.64 changes per second; §20, guard G-08) |
| task 7c: `count2` / `count3` (`kFramePeakFrames = 2` / `3`) | 2 cases red, 102 / 99 assertions: the runs of 2 and 3 enter (`AShortRunOfLongFramesIsNotSampled`), and the closed loop's runs-of-3 arm binds the guard (4,239 / 2,626 calls, \|err\| p95 0.41 / 0.22) |
| task 7c: `mean_fed`, the frame clipped at the 10 s mean (`std::min(frameTicks, m_frameMeanTicks)`) | 2 cases red, 28 assertions: `ASustainedFrameDropStillLiftsTheEdge` (a drop from 100 to 30 fps: 16 of the 32 On clients bind the guard on fewer than 180 frames, and 4 make a graduated correction, 2 in each drain order) and `AShortRunOfLongFramesIsNotSampled` (the 4-frame arms never lift the edge, and the short-run arms see it drift with the mean) |
| task 7c: `kFramePeakFrames = 5` | compile error: `error C2338: static_assert failed: 'clockDilation::kFramePeakFrames: a run of fewer long frames than this never enters the frame peak. …'` |
| task 7c: `kFramePeakFrames = 0` | compile error: `error C2338: static_assert failed: 'clockDilation::kFramePeakFrames: a frame period enters the frame peak once this many of the last kFramePeakWindow frames reach it, …'` |
| task 14: OwnRtt's jitter term added to `Common`'s target | `TheCommonTargetCarriesNoJitterTerm` red: the target 3.7 against 2.5 (§3) |
| task 14: `commonLeadTicks = 2.5` (task 6's) | the closed loop's `TheCompiledCommonLeadHoldsEveryClientOfTheHeterogeneousShape` red, 56 assertions: the far 50 and 60 ms and the loopback clients bound on every steady call (§21) |
| task 14: `guardHighTicks = 2.0` | the same case red, 40 assertions: the 30 ms client bound on every steady call, the shaper-b0 one on some; the finding case `TheHighEdgeCorrectsWhereTheReadTailReachesTheBand_Finding` stays green (§5.2) |
| task 14: `guardHighTicks = 1.5` | compile error: `error C2338: static_assert failed: 'ClockDilationConfig: the frame-aware low edge at the longest sampled frame, … must not cross the high edge, …'` (§5.3) |

The closed loop is pinned by `ClockDilationLoopTest.cpp` (og-simulation-tests, `[PCTM][Dilation][ClockDilationLoop]`):
a test-only harness runs a real `SimulationScheduler`, `NetworkTimeEstimator` and `ClientPredictionClock` per client behind a
`TickRateDoor`, calls the stage every frame and steps the clock once per scheduler step, against a 60 Hz server on its own
clock, under link jitter, a clock-rate offset, a hitch and (task 7c) isolated long frames or a frame-rate drop. §16 records
what it measured; §17–§21 what each later task measured.

## §13 The per-frame client stage: `clockDilation::beforeClientPump`

`beforeClientPump(door, hostNowSeconds, clockNowSeconds, manager, controller, window)` is the whole per-frame client path.
The host calls it once per frame from the tick driver's `beforePump`, on the scheduler's owning thread, right before the
scheduler pumps. It returns the controller's output.

1. `OG_CHECK(manager.runsPrediction(), …)`: a predicting client only (§10).
2. It fills `ClockDilationInputs`:

   | input | source |
   |---|---|
   | `predictionTick` | `door.tickOffset() + door.physicsStepCount() + (hostNowSeconds − door.nextDeadline()) / door.currentInterval()` |
   | `sensorValid`, `serverTick` | the estimator's `serverTickEstimator()`: `isValid(clockNowSeconds)`, `serverTickAt(clockNowSeconds)` |
   | R, J, m | `getSmoothedRTT()`, `getSmoothedJitter()`, `activeJitterMultiplier()` |
   | F, f | `getTimeConfig()`: `predOffsetFloorTicks`, `tickFrequency` |
   | A, O | `getLastAuthorityTick()`, `getPredictionOffsetTicks()` |
   | `phase` | `getClientClock().publishedCorrectionPhase()` |
   | `clockRead` | `getClientClock().publishedTargetRead()`: the guard centre's sample (§5.4; task 10c). A and O above only seed it |
   | `currentScale` | `door.rateScale()` |

3. `controller.update(in)`; when `out.apply` is true it calls `door.setRateScale(out.scale)` (guard G-05).
4. `window.note(…)` with the output, the door's scale after the apply, the sensor's counters and age, and both server-tick
   estimates at `clockNowSeconds`.

**The two times.** `hostNowSeconds` is the scheduler's time base, the one its deadlines are on. `clockNowSeconds` is the
clock the SimClock samples are stamped with, the one `ServerTickEstimator` measures its offset on. A host may pass the same
clock for both; they differ where the host clamps its frame time (a hitch longer than the host's frame clamp advances the
scheduler by less than the wall clock). Each is used only against its own base: the deadline lead from `hostNowSeconds` and
the door, the server tick from `clockNowSeconds` and the sensor, so the lead is `P(hostNow) − Ŝ(clockNow)` read at one
instant.

**Why `predictionTick` is the deadline lead.** After a step publishes tick T at physics step k, the driver's tick offset is
T − k, so `tickOffset + physicsStepCount` is the tick the scheduler steps at its next deadline (with no correction in
between). Subtracting the fraction of an interval still to wait makes it continuous in time. It does not depend on where
inside a frame the call lands, and a pump of n steps at a constant interval leaves it unchanged (n more steps, a deadline n
intervals later). A rate change starts a new interval at the next deadline, so the value moves by
`(nextDeadline − hostNow) · (1/I_old − 1/I_new)`: at most the relative change of the scale, in ticks (0.002 tick for one
full slew step).

**`ManagerT`** is a template so this header names no manager type: it needs `runsPrediction()`, `getNetworkEstimator()`,
`getTimeConfig()` and `getClientClock()`, which `SimulationManager` has. A test can hand in a facade over a real estimator
and clock.

In `Off` the controller returns `ModeOff` and the stage still notes the call: every call then counts as frozen with no
cause. A host that does not want a window in `Off` does not call the stage.

## §14 `ClockDilationWindow` and the `[Dilation.Window]` line

The host constructs one window per client controller, from the same `ClockDilationConfig` (it prints its mode and setpoint).
Every stage call notes one sample. When `isDue(clockNow)` (10 s, `clockDilation::kWindowSeconds`, since the window's start)
the host logs `formatLine()` and calls `restart()`. The window starts at its first call; `restart()` moves the start to the
last call's clock time and makes the counters' current values the next window's base. Before the first call `isDue` is
false.

The line, exactly:

```
[Dilation.Window] mode=%s setpoint=%s calls=%u lead=%.3f/%.3f/%.3f target=%.3f err=%.3f/%.3f/%.3f errAbsMax=%.3f wouldScale=%.5f/%.5f/%.5f wouldScaleDevP99=%.4f rateScale=%.5f/%.5f applied=%u frozen=%u frozenBy=sensor:%u,recentre:%u,debt:%u,firstRtt:%u,resync:%u guardBound=%u sensorSamples=%u sensorRejected=%u sensorReseeds=%u sensorAgeMsMax=%.1f sHatClock=%.6f sHat=%.3f sHatNoAge=%.3f wouldChanges=%u stepLag=%.3f
```

| field | what it is |
|---|---|
| `mode`, `setpoint` | the config's: `Off`/`Shadow`/`On`, `OwnRtt`/`Common` |
| `calls` | stage calls in the window |
| `lead` | p5/p50/p95 of `leadTicks` over the unfrozen calls |
| `target` | the mean `targetTicks` over the unfrozen calls: the target in the deadline coordinate, the one `err` is measured against. The execution-lead target, the setpoint a step-line analysis compares with, is `target − stepLag` (§3.1) |
| `err` | p5/p50/p95 of `errorTicks` over the unfrozen calls |
| `errAbsMax` | the largest unfrozen \|error\| |
| `wouldScale` | min/mean/max of the controller's scale over every call: what On applies, and what Shadow would |
| `wouldScaleDevP99` | p99 of \|scale − 1\| over every call, in percent |
| `rateScale` | min/max of the door's actual scale after each call: 1.00000/1.00000 in Shadow |
| `applied` | calls whose `apply` was true, i.e. `setRateScale` calls |
| `frozen`, `frozenBy` | frozen calls, and the count per freeze cause (§9); an `Off` call is frozen with no cause |
| `guardBound` | calls on which the band guard clamped the target (§5) |
| `sensorSamples`, `sensorRejected`, `sensorReseeds` | the estimator's accepted, gate-rejected and re-seed counts in this window: the difference from the base. The first window's base is 0, so it counts from the estimator's construction |
| `sensorAgeMsMax` | the largest `clockNow − lastSampleSeconds()` of the window, in ms, over calls made after the estimator accepted a sample |
| `sHatClock`, `sHat`, `sHatNoAge` | at the window's last call: `clockNowSeconds`, `serverTickAt` and `phaseBlindServerTickAt` there. With the host's step timeline these give the sensor against truth (§15) |
| `wouldChanges` | calls whose controller scale differs from the previous call's (the window remembers the last scale across `restart()`; the first call compares with 1.0): the quantum changes On applies, counted in Shadow too. In On with a door that follows, it equals `applied`. So a Shadow run shows the change rate On would have (task 10b) |
| `stepLag` | the last call's `stepLagTicks`, F̄/2 (§3.1; task 10b) |

A series with no sample prints `0.000` (e.g. `lead` in a window where every call was frozen); `frozen` equal to `calls`
says so. Given finite inputs the line prints no `nan` or `inf`.

**Percentiles** are nearest-rank (rank ⌈p·n/100⌉ of the sorted kept values) over at most 1,024 kept samples per series
(`clockDilation::kMaxKeptSamples`): a full series keeps every second sample and doubles its stride, so it holds an evenly
spaced subsample of the window. At 100 frames per second a 10 s window has about 1,000 calls and keeps all of them; above
about 102 frames per second it keeps every second one. Means, min, max, counts and `errAbsMax` see every call.

**Allocation.** The series are fixed arrays; the line goes into a buffer reserved for 1,024 characters at construction
(`clockDilation::kWindowLineReserve`), so formatting allocates nothing unless a line is longer, which only a value with
hundreds of digits makes. `formatLine` sorts copies into one scratch array, so it can be called twice on one window.

The format is a contract with whatever parses the log: a parser holding the printf string above reads every field by
position. The golden test (§16 (g)) pins the bytes.

## §15 Which server tick the sensor estimates: the truth convention

The server's host publishes the server's tick to its clock service **after** the step that produced it, together with the
time that step **started**, and the pong's age is measured from that start (og-sim-net's SimClock server since og-time-dilation
task 2b; `ServerTickEstimator-rationale.md` §1). So `serverTickAt(t)` reads tick n at the instant tick n's step started:
`Ŝ(t_start(n)) = n`. The Unreal host's start time is the stamp it prints as `stepSeconds=` on that tick's
`[AuthoritySimulation]` line, so against that timeline the sensor has **no bias by construction**: `Ŝ − S_start` has mean 0,
and `sHat` compares with step-start stamps as it is.

**Before task 2b** the host published with the time of the publication, a step's duration D after the start, and the sensor
read low on the step-start timeline: `Ŝ − S_start = −D · f`. The closed-loop harness measures that case (its publication
offset is a parameter): with publication at the step's start (D = 0) the steady mean of `Ŝ − S` is −0.001 tick, and with
D = 1.5 ms it is −0.091 tick (−D · f = −0.090). The controller would have held each client's true lead D · f below its
target; with the start time that part of it is gone. The pairwise split was unaffected either way: every client of one
server sees the same D. The phase-blind estimate (`sHatNoAge`) never reads the age and still reads about D · f low
(`ServerTickEstimator-rationale.md` §3).

⚠ **No bias by construction covers D only.** On the Unreal brawler server D is small: typically about 0.075 ms (0.0045 tick
at 60 Hz, the median window's p50 of the server's step cost) and at most 0.24 ms (0.014 tick). The sensor bias the Unreal
Shadow smokes measure, −0.04 to −0.11 tick per client before the change and −0.05 to −0.07 after it, is something else: the
estimator assumes each pong's downlink is half the round trip, so any difference between the two one-way delays, the hosts'
own send and receive delays included, moves `Ŝ` by half of it (§4 of `ServerTickEstimator-rationale.md`). The client's
receive stamp is not frame-bound: it is og-net's receive time, taken on og-net's own thread when the datagram leaves the
socket (og-sim-net's SimClock client; §19.8 of the Unreal session's rationale). A candidate for the difference, not
established: the shaped b0 link delivers in order (`reorder=0`), so each packet waits for the one before it, and the denser
direction, the downlink, waits longer. A model of that clamp at the smokes' packet rates reads −0.035 to −0.053 tick, the
sign and about the size of the measured bias, which would make it a property of the shaping proxy rather than of the sensor.
G-DIL-0 checks it.

## §16 What the closed loop measured (task 7)

Two b0-like clients (one-way 28 ms plus an exponential 7.5 ms tail each way, in-order delivery, R ≈ 71 ms), 100 frames per
second, 20 Hz pings, one server at 60 Hz; the second client joins 0.375 s after the first, so it lands half a tick off.
Steady = 10 s after both joins' re-centres finish.

| case | result |
|---|---|
| (a) On, seeds 1/101 | the two true deadline leads' difference p95 **0.077** tick over 120 s, mean +0.001; 0 Skip, Stall or resync |
| (a) On, 40 seed pairs | p95 median **0.084**, max 0.144; **11 of 40 exceed 0.10**; every \|mean\| ≤ 0.045, the mean of the 40 means −0.003. Each client's sensor noise is about 0.03 tick (§8), so the difference's p95 is about 1.96 · √2 · 0.03 ≈ 0.08 by itself |
| (a) On, the review's 24 further pairs (seeds 1001 + 17k / 5003 + 31k) | p95 median 0.094, max **0.216** (1035/5065), 11 of 24 exceed 0.10; every \|mean\| ≤ 0.047; 0 corrections. Over all 64 pairs: worst 0.216. The test asserts each pair's p95 ≤ 0.25 (G1's bound) and the median ≤ 0.11 (0.10 until task 7b, lead ruling 02:44 L3: fresh 40-pair medians reach 0.097, and a bootstrap puts 19 % of fresh sets above 0.10) |
| (a) On, the bias over 168 pairs (the 64, the review's second 24, and 80 not used for any bound) | the per-pair mean difference has a standard deviation of **0.020** and reaches 0.061 (31669/37771), so a per-pair "\|mean\| ≤ 0.05" (2.5 standard deviations) sits in the tail: 2 of the 168 fail it. The test asserts each pair's \|mean\| ≤ **0.10** (about 4–5 standard deviations) and the mean of the 40 means within **±0.02** (about 5 standard errors). No set shows a bias: the five sets' means of means are −0.003 to +0.004. Of the 80 unused pairs, two exceed G1's 0.25 (0.331 and 0.256) and two have one steady correction (a Skip and a Stall), so the per-pair p95 and zero-corrections checks have tails too (og-simulation-tests `ClockDilationLoopTest-rationale.md` §3) |
| (b) Off, seeds 1/101 | the residuals stay **0.5** tick apart (p95 0.5) |
| (c) +100 ppm on one client, 10 min | On: mean error **+0.020** tick (δ/K_p = 0.02), \|error\| p95 0.055, max 0.11; 0 graduated corrections on either client. Off: 3 graduated Stalls |
| (d) 600 ms hitch, seeds 1, 2001, 2008, 2015, 2022 | 0.2 s of host time lost; the catch-up batch's first advance reads every authority tick that arrived during the hitch (drift ≈ 37 > the hard threshold 21): **1 hard resync and 23–25 Stalls**, not Skips, on every seed. The residual left is 0.057–1.045 tick and decays at τ = 1/(f · K_p) = 3.3 s. The mean \|error\| over [+6, +6.5] s after the last correction is compared with the law at the window's midpoint, +6.25 s, for the three residuals ≥ 0.5 (seed 1: 0.165 against 0.146; 2001: 0.117 against 0.136; 2022: 0.158 against 0.160). The tolerance was ±0.07 until task 10c (±0.09 since, §19), about three standard deviations of that difference: 0.020 over the 30 of 213 seeds with a residual ≥ 0.5 (mean +0.000), 0.0225 pooled with the task 7 review's 34 fresh ones (task 7's ±0.06 missed seed 54423 by 0.003; lead ruling 02:44 L1). 11 checked seeds not used for the bound stay within 0.024. A law with K_p halved misses by 0.19–0.29. Below 0.5 the law at +6.25 s (≤ 0.077) is under the sensor noise, so it is not compared. \|error\| ≤ 0.13 from 10 s (0.10 until task 7b, lead ruling 02:44 L2): the five seeds reach 0.056–0.073; over 277 seeds the largest was 0.101, and 5 of the review's 256 fresh seeds exceed 0.10, up to 0.124. It is a noise maximum over about 1,800 frames |
| (d') 300 ms hitch (measured, not asserted) | 3 Skips inside the catch-up batch, leaving the lead 3 ticks high; \|error\| ≤ 0.10 after 11.8 s |
| (e) | the applied scale stays in [0.9953, 1.0022]; steady \|scale − 1\| p99 0.02–0.04 % |
| (f) | 0 `beyondOldest` and no render-time step backwards over 14,758 and 14,796 frames; an instant 1.25 → 0.75 step, ten times: 5 `beyondOldest`, 10 steps back |
| (g) | a scripted window equals a golden line byte for byte, and a log parser holding the printf string reads it |
| sensor truth | §15 |
| **F4: 30 fps** (task 7's characterisation, before task 7b; §17 has the fix) | (a)'s pair at 30 frames per second, 120 s steady, seeds 1/101. **Off**: 0 corrections, leads 4.000 / 3.500. **Shadow**: identical to Off; the would-be error's mean is +1.63 / +1.16 tick, so the clock holds the lead 1.1–1.6 above the OwnRtt target. **On**: **4 Skips per client** and as many 1-step frames (equal counts within one (window edges); not paired per event), mean leads 2.49 / 2.46 (1.51 / 1.04 below Off), pair p95 0.658 (G1 fails). The guard binds on no steady frame. Six seed pairs give 4–8 Skips per client, p95 0.66–0.91 and leads 0.9–1.5 below Off's |

Below saturation the proportional law removes a residual r to 0.10 tick in `τ · ln(r / 0.10)`: 7.5 s for a 0.95 tick
residual. Above it (\|error\| > 2 ticks, §6) the error falls linearly at f · maxDeviation = 0.6 tick per second. A
re-centre can leave up to a tick, so 0.10 within 6 s holds only for residuals below about 0.6 tick.

**F4 was the guard's low edge ignoring the frame period** (§5.1). Before task 7b, the edge was `c − 1.5` at every frame
rate. At 30 fps the clock's first advance in a frame reads about 1.2 ticks higher than at 100 fps for the same lead. So
OwnRtt's target, about c − 0.06 and never below the old edge, still put the mean first read near 2.5, and the O and age
noise (§5.1) carried enough reads above 3 to make 4–8 Skips per client per 120 s. Task 7b made the edge frame-aware (§5);
§17 records what that measured.

## §17 What the frame-aware guard measured (task 7b)

The same harness (§16) and steady rule, 120 s per pair, with "corrections" the steady Skips, Stalls and hard resyncs. The
eight pairs are 1/101, 2/102, 3/103 and 1001 + 17k / 5003 + 31k (k = 0–4). The fresh pairs, used for no bound, are
83003 + 37k / 89009 + 41k. The numbers equal research topic `thirty-fps-guard`'s probe of this change wherever both
measured.

| case | task 7b | the edge as task 6 wrote it |
|---|---|---|
| 30 fps On, 8 pairs (16 clients) | **0 corrections**; the guard binds on every steady frame; pair p95 0.179–0.262; mean lead **3.376** (Off 3.750, Shadow equal to Off) | 92 corrections; the guard never binds; p95 0.66–0.91; mean lead 2.527 |
| 30 fps On, 16 fresh pairs | 0 corrections; p95 max 0.234; mean lead 3.378 | 213 corrections |
| any target (`targetOffsetTicks` −0.5, −1, −2, −3; `Common` 0.5, 1.5), pairs 1/101 and 1001/5003 | 0 corrections at 30 and at 100 fps; 0 on 8 fresh pairs per arm | 30 fps: 47 / 110 / 183 / 183 and 183 / 84; 100 fps: 0 / 18 / 40 / 40 and 40 / 12 |
| 45 / 60 / 144 fps On, 8 pairs | 0 / 0 / 0 (fresh 16: 0 / 0 / 0); mean leads 2.690 / 2.407 / 2.369 | 14 / 0 / 0 (fresh: 36 / 0 / 0) |
| 20 fps On, 8 pairs (recorded, not asserted) | 4 (fresh 16: 5); Off 5 (fresh 16) | 419 (fresh: 846) |
| 30 fps with ±20 % frame jitter, 8 pairs | On 2, Off 9; mean lead 3.690, p95 max 0.158 (fresh 16: On 1, Off 12) | On 153 (fresh: 314) |
| 100 fps, task 7's (a)–(f) | every case passes; the largest move of any asserted number is 0.0003 (the 40 pairs' median p95 0.0844 against 0.0841, \|mean\| max 0.0451 against 0.0448; (c)'s p95 0.0553 against 0.0552) | — |
| Off at every frame rate | byte-identical traces to before the change (Off never calls the controller) | — |

The lead's cost at 30 fps: On parks at the frame-aware edge, c + 1.0, so its mean lead is 3.38 ticks, 0.37 below Off's
and about 1 tick above OwnRtt's target (2.37). In the harness the 30 fps Off landing (4.000 / 3.500 on every seed) is mostly an
artefact of frames that never jitter (research topic `thirty-fps-guard`); on a real host the guard binds only where the
30 fps target is below c + 1.0.

## §18 What the step lag measured (task 10b)

The harness of §16. "Execution lead" is the harness's per-step record `tick − S(t)` at the step's run time (the frame's);
its mean minus the mean of `target − stepLag` over the unfrozen frames is a client's **offset** from its execution-lead
setpoint. Twenty b0-like clients On, ±20 % frame jitter, joins spread over one frame period (`2.0 + (i/20)/fps` s), 120 s
from 10 s after the last client settles; five seed sets (7001 + i, 7101 + i, 7201 + i, 9001 + i, 9101 + i; the test runs
7001):

| fps | before the lag: mean offset over 20 clients | with the lag: mean offset (per-client sd; min, max over the 100 clients) | corrections |
|---|---|---|---|
| 100 | −0.305 (7001), −0.300 … −0.306 over the sets | −0.003 … +0.003 (sd 0.008–0.012; −0.033, +0.026) | 0 |
| 30 | −1.012 (7001), −1.012 … −1.015 | −0.003 … +0.000 (sd 0.008–0.013; −0.033, +0.028) | 0–1 per set |

Before the lag the offset is the mean step lag itself, 0.3 × 1.013 and 1.0 × 1.013 at ±20 % jitter (§3.1). At 30 fps the
guard binds on every steady frame in this jittered set (its peak-held frame period follows the longest frames, up to
2.4 ticks), so the 30 fps arm
checks the lag against the clamped target.

**Task 7's and 7b's cases with the lag**, measured against the header before it (the same seeds):

| quantity | before | with the lag |
|---|---|---|
| (a) pair 1/101: p95, mean | 0.0766, +0.0011 | 0.0768, +0.0010 |
| (a) 40 pairs: median p95, max, mean of means, max \|mean\| | 0.0844, 0.1442, −0.0031, 0.0451 | 0.0839, 0.1443, −0.0028, 0.0449 |
| (a) applied changes per client per second, pair 1/101 / 40 pairs | 3.32 / 3.38 | 3.23 / 3.37 |
| (c) +100 ppm: mean error, \|error\| p95, max | +0.0204, 0.0553, 0.110 | +0.0203, 0.0549, 0.102 |
| (d) hitch residuals, seeds 1/2001/2008/2015/2022 | 0.953 / 0.888 / 0.072 / 0.057 / 1.045 | 0.047 / 0.890 / 0.069 / 0.058 / 0.050 |
| 30 fps On, 8 pairs: mean lead, pair p95 max, corrections | 3.376, 0.262, 0 | 3.409, 0.205, 0 |
| any target, 30 and 100 fps | 0 corrections | 0 corrections |

The hitch's residual is the error left after the hard resync and its Stalls, which land the lead on whole ticks, so it
depends on the target modulo one tick: with the lag two of the five seeds (1, 2022) land under the law check's 0.5-tick
threshold, and the law at +6.25 s is compared on seed 2001 only (0.116 against 0.136).

**The drain order (finding).** The harness delivers every message at the frame's start, before the stage (og-simulation-tests
`ClockLoopHarness-rationale.md` §2). An immediate Unreal client drains them at each step's start, after the stage, so the stage reads an
authority tick up to a frame staler than the clock does, and the guard's centre sits lower. The harness's
`ClientSpec::drainAtStepStart` runs that order. With it, task 7b's two guard cases fail **with or without the lag**:

| case (drain at step start) | before the lag | with the lag |
|---|---|---|
| `ThirtyFpsOnHoldsTheBand`, 8 pairs On | 92 Skips on 16 of 16 clients; mean lead 2.527, mean target 2.370 (the setpoint: the clamp does not lift it); p95 max 0.913 | 31 Skips on 8 of 16 clients; mean lead 2.931; p95 max 0.847 |
| the same, Off | 0 corrections, lead 3.750 (as with the frame-start drain) | 0 corrections, 3.750 |
| `TheGuardHoldsAnyTarget`, 30 fps, offsets −0.5/−1/−2/−3, `Common` 0.5/1.5 | 47 / 106 / 111 / 111, 111 / 84 Skips | 9 / 20 / 106 / 111, 84 / 15 Skips |
| the same, 100 fps | 0 / 17 / 18 / 18, 18 / 12 Skips | 0 / 5 / 18 / 18, 18 / 0 Skips |

So the frame-aware low edge of §5.1, derived in the frame-start order, sits too low in the step-start order: at 30 fps,
without the lag, it no longer lifts the target off the setpoint. (Research topic `lead-definition` estimated the centre
about F/2 lower in that order; the centre itself was not measured here.) The guard is not retuned here (og-time-dilation task 10b's brief); the
characterisation case `StepStartDrainLetsOnSkipAtThirtyFps_Finding` pinned it, and every other case ran the frame-start
order. **Task 10c fixed it** (the centre is the clock's read, §5.4), made the step-start order the harness's default, retired
the finding case into the step-start arm of `ThirtyFpsOnHoldsTheBand` and runs every guard case in both orders (§19).

**The same on the Unreal host.** One 30 fps loopback Shadow session on the Unreal Editor build (og-time-dilation task 10b's
AC7): `stepLag` reads 0.992–1.000 on every client from the third window (1.000 from the sixth on client1 and client2b;
client2a, killed at 60 s, reaches 0.999; 0.94–0.98 in the first two, the mean converging at 10 s), and the sensor bias is 0.000. The guard binds on every steady frame, at its **high** edge: the wanted target is the
loopback setpoint 3.497 plus the lag, 4.50, and the clamped target reads 3.58–3.66 on client1, so the stage's guard centre
is 1.08–1.16. The clock's own view at the steps is O − age = 4 − 1.03 = 2.97 (client1; 2.74 and 3.29 on the others), about
one frame period (1.9 ticks) higher. In On that clamp would hold the deadline lead near 3.6, against Shadow's 5.05, and the
execution lead near 2.6, about 0.9 below the setpoint. Task 10c's centre removes that clamp (§5.4, §19).

## §19 What the clock's read and the change hysteresis measured (task 10c)

The harness of §16, now in the step-start (Unreal immediate) order by default, every guard case also in the frame-start
order. "Corrections" are steady Skips, Stalls and hard resyncs; the eight pairs are §17's. Each number below was measured on
the tree that holds both changes by a hidden survey test built and removed inside one lock hold, with 16 held-out pairs
(83003 + 37k / 89009 + 41k) and 5,800 held-out hitch seeds where a bound needed them; "as before" is the seen-failing probe
that puts the stage's `O − (Ŝ − A)` back (with the hysteresis in).

| case (b0, 8 pairs) | step-start | frame-start | as before, step-start |
|---|---|---|---|
| 30 fps On: corrections, pair p95 range, mean lead, guard-bound frames per client | **0**, 0.122–0.222, 3.409, 513–3,196 | **0**, 0.131–0.222, 3.409, 512–3,194 | 31 Skips on 8 of 16 clients, p95 0.68–0.85 |
| 30 fps On, 16 held-out pairs | 0, p95 max 0.220, guard-bound min 338 | 0, 0.218, min 340 | — |
| any target, 30 fps (offsets −0.5 / −1 / −2 / −3, `Common` 0.5 / 1.5; 2 pairs each) | 0 in every arm | 0 | 9 / 20 / 108 / 114, 87 / 16 |
| the same, 100 fps | 0 | 0 | 0 / 5 / 19 / 18, 19 / 0 |
| 45 / 60 / 144 fps On | 0 / 0 / 0 | 0 / 0 / 0 | — |
| 30 fps, ±20 % frame jitter: On against Off | 2 against 9 | 2 against 9 | 50 against 9 |
| the centre minus the clock's read centre at the frame's first step, mean over 16 clients, 30 / 60 / 100 / 144 fps | −0.000 / −0.000 / −0.006 / +0.001 | −0.000 / −0.000 / −0.005 / +0.001 | −2.000 / −1.000 / −0.847 / −0.725 |
| the same, clean link (0.3 ms + 0.2 ms each way) | −0.000 / −0.000 / −0.040 / −0.021 | +0.000 / −0.000 / −0.040 / −0.021 | −2.000 / −1.000 / −0.801 / −0.709 |
| 600 ms hitch, 100 fps: residual, seeds 1 / 2001 / 2008 / 2015 / 2022 | 0.049 / 0.886 / 0.062 / 0.039 / 0.055 | 0.052 / 0.885 / 0.064 / 0.041 / 0.061 | 3.47 / 2.69 / 3.58 / 3.67 / 3.66 (beyond the 2-tick saturation) |

The frame-start order's centre bias as before was +0.073 at 144 fps (the stage also sampled on 0-step frames, where the
clock reads nothing): the read-centred guard removes that too. At 100 and 144 fps on a clean link the centre sits 0.040 and
0.021 below the read centre in both orders, inside the ±0.05 the closed-loop case allows; its cause is not established here
(it does not depend on the drain order, and on b0 links the sensor noise hides it).

**Changes per client per second** (door scale changes over the steady 120 s, the step-start order):

| arm | with the hysteresis: mean (worst client) | the rule inert (`changeHysteresisQuanta` 0.5) |
|---|---|---|
| 100 fps b0, 8 pairs | **0.29 (0.35)**; 16 held-out pairs 0.27 (0.37) | 3.37 (3.70) |
| 60 / 144 fps b0 | 0.28 (0.37) / 0.27 (0.35); held-out 0.27 (0.34) / 0.28 (0.40) | 3.32 / 3.40 |
| 30 fps b0 | **0.99 (1.70)**; held-out 1.00 (1.80) | 7.89 (12.69) |
| 100 fps loopback (0.3 ms + 1.5 ms each way) | **0.08** (0.16); held-out 0.07 (0.17) | 0.80 (1.51) |
| 30 fps b0, ±20 % frame jitter (recorded, not gated) | 3.93 | — |

The frame-start order gives the same to ±0.02/s. Shadow's would-be changes on pair 1/101: 0.20 and 0.18 per second.

**The hitch law under the hysteresis.** The law check compares the mean |error| over [+6, +6.5] s after the last correction
with `residual · e^(−6.25 · f · K_p)` for residuals ≥ 0.5 (§16). Over 186 of 5,800 held-out seeds with such a residual the
difference has mean **−0.014** and standard deviation **0.025** (range −0.079 … +0.062; 2 beyond the earlier ±0.07); the
frame-start order agrees to 0.001. The hold zone of ±0.04 tick (§6) is the bias's source. The closed-loop test's tolerance
is |mean| + 3σ = 0.089 → **0.09**, and its seeds gain the first three from 3001 up with a residual ≥ 0.5 (3035, 3048,
3156: 1.02 / 0.97 / 1.09, differences +0.000 / −0.027 / −0.065) beside 2001 (−0.036). From 10 s after the last correction
|error| stays ≤ 0.13 on all but 2 of the 5,800 seeds in the step-start order and 1 in the frame-start order (max 0.147);
the test's eight seeds reach 0.051–0.090 in either order.

**What moved** (task 10b's numbers in its default frame-start order, against this task's default step-start order):

| quantity | task 10b | task 10c |
|---|---|---|
| (a) pair 1/101: p95, mean | 0.0768, +0.0010 | 0.0791, −0.0013 |
| (a) 40 pairs: median p95, max, mean of means, max \|mean\| | 0.0839, 0.1443, −0.0028, 0.0449 | 0.0909, 0.1446, −0.0057, 0.0447 |
| (a) applied changes per client per second, pair 1/101 / 40 pairs | 3.23 / 3.37 | 0.31 and 0.23 / 0.27 |
| (c) +100 ppm: mean error, \|error\| p95, max | +0.0203, 0.0549, 0.102 | +0.0208, 0.0612, 0.105 |
| (d) hitch residuals, seeds 1/2001/2008/2015/2022; the law difference on 2001 | 0.047 / 0.890 / 0.069 / 0.058 / 0.050; −0.020 | 0.049 / 0.886 / 0.062 / 0.039 / 0.055; −0.036 |
| (e) applied scale range, pair 1/101; steady p99 of \|s − 1\| | [0.9982, 1.0003] and 0.040 % (task 7b's measure) | [0.9995, 1.0014]; 0.03 / 0.02 % |
| (f) `beyondOldest`, render-time steps back | 0 / 0 | 0 / 0 |
| 30 fps On, 8 pairs: mean lead, pair p95 max | 3.409, 0.205 | 3.409, 0.222 |
| execution lead (§18), set 7001: mean offset at 100 / 30 fps | −0.0027 / +0.0008 | −0.0007 / +0.0051 |

No assertion's bound had to move except the hitch law's tolerance (0.07 → 0.09, above) and two that task 10c's criteria set
(the 30 fps guard-bound floor and the changes-per-second bounds; og-simulation-tests `ClockDilationLoopTest-rationale.md`
§3.3).

**The same on the Unreal host** (og-time-dilation task 10c's AC7: one 30 fps loopback Shadow session on the Editor build, the
shape of task 10b's, after a warm-up session; three clients: client1 for the whole session, client2a leaving and client2b
joining in its place mid-session, as the launcher splits it). The
centre the controller holds, read off its bound windows as `target − (φ̄ − 1) − guardLowTicks` = target − 1.0 at 30 fps,
against the host's own `O − age` at the steps (the gate tool's, from the server's step timeline and the clock's authority
tick, independent of the controller):

| client | controller centre | host O − age (age) | task 10b's session: controller centre against O − age |
|---|---|---|---|
| client1 | 3.55–3.60 (its 2nd–7th windows), then below 3.497 (unbound) | 3.46 (0.54) | 1.08–1.16 against 2.97 |
| client2a | 3.91–3.94 | 3.89 (0.11) | against 2.74 |
| client2b | 3.94–3.99 | 3.85 (0.15) | against 3.29 |

So the centre now sits on the clock's view to within 0.04–0.12 tick, where task 10b's sat 1.9 below it: the drain-order fix
holds on the real host. What the research report expected the session to show did not hold on two clients, for a reason of
the session, not of the guard: it assumed task 10b's centres (2.97 / 2.74 / 3.29), under which the 4.50 target would be
unbound. This session's authority ticks were younger (age 0.11–0.54 against 0.71–1.26), so for client2a and client2b the
frame-aware low edge c + 1.0 ≈ 4.9 lies above the wanted target 4.497 and binds on every steady window, holding the target at
the edge as §5.1 intends (the clock's mean first read at 1.5). Client1's edge fell below the wanted target in its eighth window:
from then on its target is unbound with `target − stepLag` = **3.497**, the setpoint, on all six remaining windows.
`wouldChanges`: 0.09, 0.03 and 0.12 per second over each client's windows from the third on (as before: 2.3–3.9), with two
windows above 0.2/s, both while the clamp released or moved: client1's eighth (10 changes, 158 of 300 calls bound, the
unbinding) and client2b's last (5). `stepLag` 0.992–1.000 from the third window. Every 10-second check passed (check_dil0
117/117, parity, check_ph2 33/33, stepclock).

## §20 What the sustained frame filter measured (task 7c)

In the closed loop (og-simulation-tests `ClockLoopHarness.h`), 8 seed pairs (`kGuardPairs`, 16 clients × 120 s of steady
state), UE's step-start drain order unless a row says otherwise. The long frames come from the harness's `LongFrames`
knob: runs of 1 to 4 frames of 25–40 ms, one run every 1.5–4.5 s (mean 3 s) from 5 s, about 44 runs per client. "Before" is
the unfiltered peak of task 7b (the header before task 7c), "after" this header. Each cell: corrections over the 16
clients / guard-bound calls / the worst client's steady |err| p95 / mean door changes per second / mean lead.

| scenario | before | after |
|---|---|---|
| 100 fps, no long frames | 0 / 0 / 0.066 / 0.29 / 2.666 | identical |
| 100 fps, single long frames | 0 / 74,084 / 0.543 / 6.03 / 2.801 | **0 / 0 / 0.069 / 0.28 / 2.674** |
| the same, frame-start order | 0 / 74,121 / 0.544 / 6.03 / 2.802 | 0 / 0 / 0.068 / 0.26 / 2.675 |
| runs of 2 | 0 / 87,775 / 0.576 / 7.34 / 2.856 | 1 / 0 / 0.127 / 0.30 / 2.685 |
| runs of 3 | 0 / 93,055 / 0.577 / 7.93 / 2.888 | **0 / 0 / 0.069 / 0.27 / 2.690** |
| runs of 4 (enter by design) | 0 / 97,917 / 0.594 / 8.45 / 2.921 | 1 / 38,572 / 0.348 / 3.01 / 2.740 |
| single long frames at 60 / 144 fps | 0 / 74,594 / 0.628 / 7.97 / 3.142; 0 / 110,182 / 0.598 / 7.04 / 2.727 | 0 / 0 / 0.069 / 0.28 / 2.878; 0 / 0 / 0.069 / 0.28 / 2.583 |
| single long frames 4 times a second | 1 / 175,175 / 0.426 / 15.57 / 3.374 | 7 / 0 / 0.176 / 0.37 / 2.763 (Off: 6) |
| a drop from 100 to 30 fps at 60 s | 0 / 20,246 / 0.111 / 0.73 / 2.879 | 0 / 20,198 / 0.110 / 0.74 / 2.879 (Off: 7) |
| a drop from 100 to 45 fps at 60 s | 0 / 596 / 0.083 / 0.29 / 2.788 | 0 / 582 / 0.083 / 0.29 / 2.788 |
| 30 fps, ±5 % frame jitter | 0 / 46,100 / 0.120 / 1.49 / 3.446 | 0 / 36,127 / 0.108 / 1.24 / 3.420 (Off: 6) |
| 30 fps, ±20 % frame jitter | 2 / 57,588 / 0.148 / 3.93 / 3.696 | 2 / 55,111 / 0.160 / 3.31 / 3.550 (Off: 9) |

- On a held-out set (24 other pairs, 48 clients) single long frames give, after: 0 corrections, 0 bound calls, the worst
  client's |err| p95 0.067, at most 0.41 changes per second (before: 1 correction, 215,914 bound calls, 0.560, 7.07/s).
- The isolated runs leave the lead at the no-long-frame value (2.674–2.690 against 2.666); before, they raised it by
  0.13–0.26 tick. The correction in the runs-of-2 row is one over 16 clients × 120 s, as Off's one with single long frames.
- At 30 fps the filter lowers the edge slightly under jitter (a 3-step frame among 2-step ones enters only once 4 of the
  last 8 reach it). The corrections stay at 2 and 0; the door changes drop (3.93 → 3.31 per second at ±20 %, the 30 fps
  wobble that og-time-dilation's research topic `on-readiness` recorded as F3) and the lead drops 0.15 tick.
- Every `[Dilation]` case of tasks 7, 7b, 10b and 10c passes unchanged in both drain orders. At a constant frame period
  the filter returns the frame to within the rounding of the clock differences; the no-long-frame row above is identical
  to three decimals.

## §21 What the `Common` setpoint measured (task 14)

In the closed loop (og-simulation-tests `ClockLoopHarness.h`), one client per run, 100 fps, UE's step-start drain order,
seeds 14001 + 17k, steady state from 10 s after the join settles. The links: og-time-dilation task 13's fixed lags each way
with b0's loss (2 % up, 1 % down), og-net's shaper b0 as the harness's uniform lag (15–50 ms each way, with and without
b0's loss), the harness's exponential b0 (28 ms + 7.5 ms mean tail each way) and loopback. Each cell: steady corrections
(Skips, Stalls and hard resyncs) per minute / the share of unfrozen calls the guard bound (the client-minutes). `OwnRtt` is
the client's own target; the others are `Common` targets in ticks, 6.00 binding the high edge where the centre is low.

| link | `OwnRtt` | 3.30 | 3.65 | 3.80 | 4.00 | 6.00 |
|---|---|---|---|---|---|---|
| near 10 ms | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 100.0 % (15 min) |
| mid 30 ms | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.8 % (200 min) | 0.00 / 100.0 % (200 min) | 0.00 / 100.0 % (15 min) |
| far 50 ms | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 100.0 % (15 min) |
| far 60 ms | 0.00 / 0.0 % (15 min) | 0.00 / 0.7 % (15 min) | 0.00 / 0.0 % (15 min) | 0.00 / 0.0 % (15 min) | 0.00 / 0.0 % (15 min) | 0.00 / 1.8 % (15 min) |
| loopback | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 100.0 % (15 min) |
| shaper b0, no loss | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.1 % (200 min) | 0.00 / 100.0 % (200 min) |
| shaper b0, loss | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.00 / 0.0 % (200 min) | 0.04 / 0.1 % (200 min) | 0.37 / 100.0 % (200 min) |
| exponential b0, no loss | 0.01 / 0.0 % (200 min) | 0.12 / 0.0 % (200 min) | 0.23 / 0.0 % (200 min) | 0.40 / 0.0 % (200 min) | 0.63 / 0.0 % (200 min) | 1.73 / 100.0 % (15 min) |
| exponential b0, loss | 0.01 / 0.0 % (200 min) | 0.14 / 0.0 % (200 min) | 0.26 / 0.0 % (200 min) | 0.42 / 0.0 % (200 min) | 0.69 / 0.0 % (200 min) | 1.80 / 100.0 % (15 min) |

- **At the compiled 3.65** no link binds and none corrects but the exponential one. The 60 fps cells and the frame-start
  order (10 seeds × 600 s per cell) agree: 0 corrections and 0 binding at 3.65 on every link of task 13's shape. The nearest
  edge is the 30 ms client's high one (0.11 % of its calls bound at 3.70, 0.8 % at 3.80, all at 4.00), then the 60 ms
  client's low one (0.33 % at 3.30, all at 2.50).
- **At 30 fps** 3.65 binds: the 60 ms client at its low edge and the 30 ms client at its high edge on every call, and the
  shaper-b0 client on 6–7 % of its calls, with 0 corrections. The 30 fps frame-aware low edge and step lag (§5.1, §3.1) move
  both edges by about a tick against 100 fps, so one constant does not equalise this shape at 30 fps.
- **The leads are common.** At 3.65 and 100 fps the per-link runs' mean execution leads are 3.599–3.637 ticks (eight seeds
  × 120 s per link, both orders); `TheCompiledCommonLeadHoldsEveryClientOfTheHeterogeneousShape` asserts the six clients of
  one session within the 100 fps stair's 0.2 of each other.
- **The high edge** (§5.2) corrects at the edge only where the read's tail reaches the band: 0 on the jitter-free and the
  lossless shaper-b0 links, 0.37 a minute on the lossy shaper-b0 one, 1.7–1.8 on the exponential ones. At `guardHighTicks`
  2.0, the lowest value that compiles, the closed loop's finding case still sees the jittered links correct at the edge,
  and the 30 ms client binds on every steady call at 3.65.
- **The exponential tail corrects below the edge** too: 0.12 a minute at 3.30 up to 0.63–0.69 at 4.00, against 0.01 under
  `OwnRtt`. Task 7b's F-a (6 corrections at `Common` 6) and `on-readiness`'s F1 (3 at `Common` 4)
  were measured on this harness's exponential b0, not on og-net's. On og-net's shaper b0 (a bounded tail) 3.65 costs nothing measurable here.
