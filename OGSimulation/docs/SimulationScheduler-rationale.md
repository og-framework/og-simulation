<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationScheduler.h` — rationale

The narrative behind `SimulationScheduler`, the host-pumped fixed-step scheduler. The header keeps
its licence, its code and one `⛔G-nn` tag per guard; everything that explains it lives here.

**If this file and `SimulationScheduler.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`SimulationScheduler-guards.md`, reachable from the `⛔G-nn` tag at that site.

⛔ **This file is not the source of truth for any VALUE.** Defaults and constants live in the
header. Where a number appears below it is there to make an argument readable.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction (STL only). Everything below is true to a reader with no game engine and no other
repository.

---

## §1 What it is, and what it owns: physics steps, never sim ticks

`SimulationScheduler` answers one question for a host loop: *how many fixed physics steps are due
at this wall time?* The host calls `pump(now)` and runs exactly the returned number of steps. It
owns nothing else: no thread, no clock source, no physics world, no sleep.

⛔ **It owns the physics step counter only** (`physicsStepCount`). It never names, stores, exposes
or assumes a sim tick, and no parameter or return value of it is a tick. The sim tick belongs to
the clocks (`ServerTickClock`, `ClientPredictionClock`). On the authority the two counts advance
together, which makes it tempting to read one as the other; on a predicting client they diverge
by design:

| clock outcome | physics steps | sim tick |
|---|---|---|
| Normal | +1 | +1 |
| Stall | +1 | unchanged |
| Skip | +1 | +2 |
| HardResync | +1 | jumps to a new value |

So "the step the scheduler just issued" and "the tick being simulated" are different numbers, and
only the clock knows the second. The declaration-level form of this rule is guard G-01. The rule
as a whole has no single site, so it lives here.

The public surface:

| member | role |
|---|---|
| `SchedulerConfig` | `dtSeconds`, `maxCatchUpSteps` (no default, §4), `minRateScale`, `maxRateScale` |
| `pump(now)` | steps due now, at most the cap; advances `physicsStepCount` by the returned count (§3) |
| `nextDeadline()` | the wall time the next step falls due; a runner sleeps until it |
| `currentStepInterval()` | the newest segment's interval: the spacing after `nextDeadline()`, exactly `dtSeconds / rateScale()` (§5, §11) |
| `stepDeadline(step)` | the wall time a step was (or will be) scheduled for (§6) |
| `setRateScale(s)` / `rateScale()` | the clock-dilation seam (§5) |
| `reanchor(now)` | pause/resume: skip the gap, count it (§7) |
| `lostTime()` | every dropped second and step, cumulative: the sum of the two causes (§4, §7, §10) |
| `lostTimeByCause()` | the same, split into `catchUp` (`pump`'s drops) and `reanchor` (skipped gaps) (§10) |
| `ignoredTimeSamples()` | rejected `now` values (§8) |

## §2 The deadline grid

The first deadline is `startSeconds` itself: a fresh scheduler issues one step on its first pump at
the start time. Each later deadline is the previous one plus `interval = dtSeconds / rateScale`.

That sum is never accumulated (guard G-02). The scheduler keeps a short history of **segments**
(a fixed ring, §11), each `{firstStep, firstDeadline, interval}`, and computes a step's deadline as
`firstDeadline + (step − firstStep) × interval` from the segment that contains it. A segment
begins wherever the grid stops being one straight line:

- `setRateScale` with a value that changes the clamped scale (a new interval),
- a catch-up drop (§4: the grid jumps forward by the dropped intervals),
- a `reanchor` that moves the deadline (§7).

Two such events with no step issued between them replace the last segment rather than add one,
because no issued step can lie in it.

**Why not a running sum, even in `double`?** Everything stays in `double` either way, but a running
sum carries one rounding per step, while the segment form carries one rounding per evaluation,
however far from the anchor. With 10⁶ steps at 60 Hz the segment form meets `start + N·dt` well
within the 10⁻⁶ s the tests require. The test suite also keeps a control: a `float` accumulator over the same million
steps misses by more than 10⁻³ s, and narrowing the segment form's arithmetic to `float` misses by
8.7 × 10⁻⁴ s (both measured).

## §3 `pump`

`pump(now)`:

1. rejects a `now` that is non-finite or earlier than the latest accepted sample (§8);
2. counts the deadlines `<= now` from `nextDeadline()` on (`stepsDueAt`; a closed-form estimate
   corrected by at most a few probes of the same `deadlineAt` expression, so the count and the
   deadlines can never disagree about a boundary);
3. runs `min(due, maxCatchUpSteps)` and drops the rest (§4);
4. advances `physicsStepCount` by the number it returns.

The steps of one call are numbered `physicsStepCount() − n` to `physicsStepCount() − 1`, where `n`
is the return value. A time exactly on a deadline counts that deadline as due.

## §4 The catch-up cap and lost time

When more steps are due than `maxCatchUpSteps`, `pump` runs exactly the cap and drops the rest.
It drops the **oldest** due deadlines and runs the newest: the grid jumps forward by the dropped
intervals, so the last step run is scheduled for the latest due deadline (at or before `now`) and
the next deadline is the first one after `now`. The grid keeps its phase.

Worked example (60 Hz, cap 4): a pump 250 ms after the previous one finds 15 deadlines due. It runs
4, drops 11, and adds 11 steps and `11 × interval` (≈ 0.183 s) to the catch-up cause,
`lostTimeByCause().catchUp`, and so to `lostTime()` (§10). The next pump one step later runs 1, with
no burst.

**Every dropped step is counted (guard G-03).** A fixed-step loop that runs a cap and moves on is
the textbook way to lose wall time silently: the simulation falls behind real time and nothing
says why. `lostTime` is cumulative and never reset by the scheduler (each cause is cumulative too);
a host that wants a rate differences two reads.

**The cap has no default.** The host chooses it per role (an authority and a predicting client
want different caps), and a default would hide that choice. It is enforced by the
compiler, not by a comment: `MaxCatchUpSteps` has no default constructor, so a `SchedulerConfig`
that omits the cap does not compile, and the header pins both facts with `static_assert`. The
conversion from `uint32_t` is implicit, so `SchedulerConfig{dt, 8u}` and the designated form both
read naturally.

**Only a `uint32_t` converts.** A deleted constructor template takes every other argument type, so
`SchedulerConfig{dt, -1}` does not compile. Without it, `-1` converted through the `uint32_t`
constructor with nothing louder than a narrowing warning (MSVC C4838) and became a cap of
4294967295, which is no cap at all. The price is that a plain `int` literal is refused too: write
`8u`, not `8`. The header pins the accepted and the refused types with `static_assert`s, and pins
`SchedulerConfig(dt, -1)` (the parenthesised aggregate form, which permits narrowing and so is
refused only by the deleted constructor). The braced `{dt, -1}` form cannot be pinned the same way:
inside a `requires` expression MSVC treats the braced narrowing itself as a substitution failure, so
a concept over `C{dt, -1}` reads "does not compile" with or without the deleted constructor and
proves nothing. That form was checked by compiling it, which fails with C2280 (a deleted function).

## §5 The rate scale

`interval = dtSeconds / rateScale`. A scale **above 1 runs faster** (shorter intervals, more steps
per second); **below 1 runs slower**. Over 10 s at 60 Hz the tests measure 600 steps at 1.0, 570 at
0.95 and 630 at 1.05: proportional, in that direction.

`setRateScale` clamps to `[minRateScale, maxRateScale]` and ignores a non-finite value (a NaN
cannot be clamped). A change takes effect after the deadline already scheduled: `nextDeadline()`
does not move, and the interval after it is the new one. That keeps `stepDeadline` strictly
increasing (§6) and means a change never makes an already-due step un-due.

It is the seam for clock dilation. The scheduler applies a scale; it never decides one.

## §6 `stepDeadline`

`stepDeadline(step)` returns the wall time a step was scheduled for, for render interpolation and
diagnostics. It is evaluated from the segment list (§2):

- **issued steps inside the retained history** get the exact deadline they were issued against;
- **steps before the oldest retained segment** are extrapolated backwards from that segment. The
  ring keeps the last `kDeadlineHistorySegments` segments (§11), so this only happens after that
  many grid changes, and only for steps older than all of them;
- **future steps** (`step >= physicsStepCount()`) are extrapolated from the current segment: the
  plan if nothing changes.

**It is strictly increasing in `step`.** Within a segment the interval is positive. Across a
segment boundary the new segment starts at or after the deadline the previous one had scheduled
next: a rate change keeps `nextDeadline()`, a catch-up drop moves it forward, and a `reanchor`
only ever moves it later (§7). The extrapolated region before the oldest segment is that
segment's own straight line, so it joins without a step backwards.

## §7 `reanchor`

`reanchor(now)` is for a host that knows the gap was not simulation time: a pause, a resume from
background, a debug-mode switch. If `now` is later than `nextDeadline()`, it moves the next
deadline to `now` and counts the gap (guard G-04) under the reanchor cause (§10):

- `lostTimeByCause().reanchor.seconds += now − nextDeadline()` (the gap, exactly),
- `lostTimeByCause().reanchor.steps += (deadlines due at now) − 1`, because one step still runs at
  `now`.

The next `pump(now)` then returns 1. After a 10 s pause at 60 Hz that is ≈ 10 s and 599 steps.

⛔ **The lost-step count is taken before the segment changes.** After `beginSegment`, the due count
at `now` is measured against the new deadline and is always 1, so a reordered call counts zero lost
steps. The reanchor test pins `lostTime().steps == 599`, so a reorder fails it.

If `now` is not later than `nextDeadline()`, `reanchor` does nothing: there is no gap, and pulling
the deadline earlier would let steps run ahead of real time and break §6's ordering.

## §8 Rejected time samples

A `now` passed to `pump` or `reanchor` that is non-finite, or earlier than the latest sample already
accepted, is ignored: no step, no lost time, no deadline change. It is counted in
`ignoredTimeSamples()`. The high-water mark is not lowered, so a host whose clock briefly stepped
backwards resumes cleanly once its clock passes the old maximum. The scheduler never throws.

Invalid configuration is a programming error, not a runtime condition: the constructor checks
`dtSeconds > 0`, `maxCatchUpSteps >= 1`, `0 < minRateScale <= 1 <= maxRateScale` and a finite
start with `OG_CHECK`.

## §9 Threading

The scheduler is a plain value with no synchronisation. It belongs to the one thread that pumps it.
Another thread that needs a deadline (a renderer interpolating between steps) reads it from data the
owning thread publishes, never by calling into the scheduler.

## §10 Lost time by cause

Lost time has two causes, and a host that wants to tell a hitch from a pause needs them apart:

| cause | recorded by | field |
|---|---|---|
| a catch-up drop (§4) | `pump`, at guard G-03 | `lostTimeByCause().catchUp` |
| a skipped gap (§7) | `reanchor`, at guard G-04 | `lostTimeByCause().reanchor` |

Each call site names its own field (`recordLostTime(lost_.catchUp, …)`,
`recordLostTime(lost_.reanchor, …)`), so the cause is fixed where the amount is measured.

`lostTime()` returns the field-wise sum, `catchUp + reanchor`, for steps and for seconds.

- **It is bit-exact with the single total it replaced whenever only one cause has occurred.** That
  cause accumulates the same amounts in the same order as the single total did, and the other
  field is still exactly `0` and `+0.0`. Adding `+0.0` to a non-negative `double` returns it
  unchanged, and every amount recorded is non-negative.
- **Nothing in og-simulation calls `reanchor`.** `SimulationTickDriver` keeps its scheduler private
  and never calls it, and the `TickRateDoor` it hands the game's `beforePump` has no `reanchor`.
  So a host that drives the scheduler through the tick driver records catch-up drops only, and its
  `lostTime()` is the old total, bit for bit. The tick driver's window totals read `lostTime()`, so
  they print the sum.
- **With both causes,** the step count is still exact. The seconds can differ from a single running
  total by rounding, because the two sums are added once instead of interleaved.

The test `SimulationScheduler.LostTime.ByCauseSplitsCatchUpDropsFromReanchorGaps` pins the split:
a hitch beyond a cap of 3, then a reanchor gap. It fails if the gap is recorded as a catch-up drop.

## §11 The segment ring: fixed capacity, no allocation

The history is a `std::array<DeadlineSegment, kDeadlineHistorySegments>` used as a ring, with the
index of the oldest kept segment and the number kept (1 to `kDeadlineHistorySegments`). The
constructor seeds the first slot with the start segment, so there is always a newest segment.

Its semantics are those of the `std::deque` it replaced:

- `beginSegment` overwrites the newest segment when that one begins at the current step (§2: no
  issued step lies in it).
- Otherwise it appends. When the ring is full, the append overwrites the oldest slot and advances
  the oldest index: the same as the deque's append followed by dropping its front once it held more
  than `kDeadlineHistorySegments`.
- `segmentFor` scans from the newest to the oldest and falls back to the oldest kept (§6).
- `currentInterval()` is the newest segment's interval. The public `currentStepInterval()` returns it,
  read-only. Every segment is begun with `dtSeconds` (the first) or `dtSeconds / rateScale_` (a rate
  change), and a drop or a `reanchor` begins its segment with `currentInterval()`, so the value is always
  exactly `dtSeconds / rateScale()`.

**No allocation, at construction or after.** The ring lives inside the scheduler object, so
`pump`, `reanchor` and `setRateScale` never reach the heap. A `std::deque` stores its elements in
separately allocated blocks, so an append at a segment change (a rate change, a drop, a reanchor)
could allocate on the thread that pumps.

**The price is size.** The object carries all `kDeadlineHistorySegments` slots: measured with MSVC
x64, `sizeof(SimulationScheduler)` went from 120 to 1,648 bytes. A copy of the scheduler copies the
ring.

The test `SimulationScheduler.Segments.TheRingKeepsTheDequesDeadlinesOverManySegments` pins the
equivalence. It drives the scheduler beside a copy of the deque version through 1,000 pumps, more
than 300 segments and two catch-up drops. Every `stepDeadline` from the oldest kept segment to ten
steps ahead, and every `nextDeadline()`, must be exactly equal. Evicting one segment early fails it
at the oldest kept deadline.

## §12 Provenance

Written on 2026-10-06 for the project-owned fixed step that replaces an engine physics scheduler.
The catch-up rule answers a measured weakness of that engine scheduler, which discarded wall time on
a hitch without reporting it. ⚠ That research and the design it fed live in private working
material that is **not distributed with this submodule**; nothing above depends on it.

2026-11-12: lost time split by cause (§10), and the segment history moved from a `std::deque` to a
fixed ring (§11).

2026-11-20: `currentStepInterval()`, a public read of the newest segment's interval (§11), for a rate
controller that reads the step interval through the tick driver's rate door.
