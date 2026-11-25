<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverWindow.h` — rationale

The narrative behind `TickDriverWindowSeries`, `TickDriverPercentiles`, `tickDriverPercentilesOf`,
`TickDriverWindowTotals`, `TickDriverWindowCounts`, `BasicTickDriverWindowSamples`, `BasicTickDriverWindow`
and `BasicTickDriverFirstStepLine` (with the aliases `TickDriverWindow` and `TickDriverFirstStepLine`), and
behind `TickDriverWindow.cpp`. The header and the `.cpp` keep their licence, their docs pointer and their
code; everything that explains them lives here. Prohibitions would be in `TickDriverWindow-guards.md`,
which has no live entry (it says why).

**If this file and `TickDriverWindow.h` or `TickDriverWindow.cpp` disagree, the code is authoritative and
this file is stale.** Fix this file; do not soften the code to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL only, plus `LostTime` from `SimulationScheduler.h`, `formatHostLine` from
`TickDriverLog.h` and `StepThreading` from `TickDriverThreadContext.h`. Everything below is true to a
reader with no game engine and no other repository.

---

## §1 What it is for

A tick driver reports how its frames and steps went in three host log lines:
- `[SimHost.Frame]`, once when it starts: role, stepping mode, step size, catch-up cap, rate bounds and
  time base; plus a Warning variant when the requested catch-up cap had to be clamped;
- `[SimHost.FirstStep]`, once, from the thread that runs the first batch of steps: which thread stepped;
- `[SimHost.Window]`, once per window of `tickDriverWindow::kWindowSeconds` (10 s): what the frames and
  steps of that window cost.

`BasicTickDriverWindow` holds the window's state (counts and ten sample series: eight on every window
line, two more on the `FreeRunning` suffix, §8) and formats all three lines except the first-step line,
which `BasicTickDriverFirstStepLine` formats (§6). It is a passive recorder: it **reads no clock and no
scheduler**. Its owner passes every time and every running total in, so the same window serves any
engine and any time base, and a test drives it with plain numbers.

The window takes its stepping mode, a `StepThreading` (`Inline`, `FrameBounded` or `FreeRunning`), at
construction and keeps it: `BasicTickDriverWindow(isAuthority, threading)`. The older
`(isAuthority, stepsInline)` constructor forwards `Inline` for `true` and `FrameBounded` for `false`, so
its callers keep compiling and printing what they printed. `threading()` returns the mode;
`stepsInline()` is true only for `Inline`.

## §2 The window

**Counts** (`TickDriverWindowCounts`): `startSeconds`, `frames`, `steps`, `maxStepsPerFrame`,
`endWaits`, `j2Waits`, `replays`, `replayedTicks`, `maxReplayDepth`, `resimRefused`, `shadowRefused`,
the three wake counts `activityWakes`, `earlyWakes` and `idleFallbacks` (§8), and `atStart`, the
owner's running totals when the window opened.

**Running totals** (`TickDriverWindowTotals`): `lost` (the scheduler's `LostTime`: dropped steps and
seconds), `ignoredTimeSamples`, and two ring drop counts, `j1Drops` and `j6Drops`. The owner reads them
from its scheduler and rings and passes them to `restart` (they become `atStart`) and to
`formatWindowLine`; the line prints each as the difference "at the line minus at the start", in the
total's own type. An unsigned total that went backwards therefore wraps, as a subtraction of the raw
totals does.

**Series** (`TickDriverWindowSeries`, ten, in the line's order): `FrameMs`, `StartPhysicsUs`,
`EndWaitUs`, `PostPhysicsUs`, `J2WaitUs`, `StepUs`, `PreUs`, `ShadowRestoreUs`, then the two that only
the `FreeRunning` suffix prints, `LateUs` and `ApproachUs`. The `static_assert` in `tickDriverWindow`
pins the count to ten.

**The note doors** take seconds and store the sample in the series' unit, as a `float`:
| door | counts | sample |
|---|---|---|
| `noteFramePeriod(seconds)` | — | `FrameMs`, seconds × 1000 |
| `noteStartPhysics(numSteps, seconds)` | `frames` + 1, `steps` + numSteps, `maxStepsPerFrame` = max | `StartPhysicsUs`, seconds × 10⁶ |
| `noteEndWait(seconds)` | `endWaits` + 1 | `EndWaitUs`, seconds × 10⁶ |
| `notePostPhysics(seconds)` | — | `PostPhysicsUs`, seconds × 10⁶ |
| `noteWorldLockWait(seconds)` | `j2Waits` + 1 | `J2WaitUs`, seconds × 10⁶ |
| `noteShadowRestore(seconds, refused)` | `shadowRefused` + 1 if refused | `ShadowRestoreUs`, seconds × 10⁶ |
| `noteStepCost(preSeconds, stepSeconds, replayedTicks, resimRefused)` | see below | `StepUs`; `PreUs` (see below) |
| `noteLateness(lateSeconds)` | — | `LateUs`, seconds × 10⁶ |
| `noteApproach(seconds)` | — | `ApproachUs`, seconds × 10⁶ |
| `noteActivityWake()` | `activityWakes` + 1 | — |
| `noteEarlyWake()` | `earlyWakes` + 1 | — |
| `noteIdleFallback()` | `idleFallbacks` + 1 | — |

`noteStepCost` records one step's cost: `StepUs` always; `PreUs` on the authority for every step, and
on a client only for a step that replayed (`replayedTicks > 0`), so that on a client the field reports
the cost of the resimulation check and the replay rather than of every step's preparation. A step
that replayed also adds one to `replays` and `replayedTicks` to `replayedTicks`, and raises
`maxReplayDepth`; `resimRefused` adds one to the count of the same name.

Every door works in every mode; the window does not check which mode its owner runs. Only the
`FreeRunning` window line prints the two suffix series and the three wake counts (§8).

The scaling is a `double` multiplication of the owner's difference, then a conversion to `float`: the
same value the owner would get by scaling the difference itself, so where the subtraction happens does
not change a sample.

`counts()` and `samples(series)` are public, mutable and const, so an owner can record a count the
doors do not cover and a test can set any count directly (the extremes in §5).

**`restart(nowSeconds, totals)`** opens a new window: it zeroes every count (the wake counts too), sets
`startSeconds` and `atStart`, and clears every series (values and overflow) without releasing capacity.
**`isDue(nowSeconds)`** is true once `nowSeconds − startSeconds ≥ kWindowSeconds`. The owner calls
`formatWindowLine`, logs it, and calls `restart` with the same time: nothing in the window closes
itself.

**`sampleOverflow()`** is the sum of the ten series' overflow counts (§5). In `Inline` and
`FrameBounded` it includes the two suffix series, which those lines do not print; no owner in those
modes notes them.

## §3 Percentiles and the fields

`tickDriverPercentilesOf(values, count)` (in `TickDriverWindow.cpp`) sorts the values in place and
returns `n`, `p50`, `p99` and `max` by **nearest rank**: the value at index
`latencyBudget::nearestRankIndex(n, 500)` and `(n, 990)` of the sorted values, the same rule as the
latency-budget probe, and the last value as `max`. With no value it returns `n == 0`.
`TickDriverWindow.Percentiles.NearestRankLikeTheLatencyProbe` checks it against `nearestRankIndex` for
counts from 1 to 16,384.

Each series prints as `p50/p99/max`: `FrameMs` with two decimals (`%.2f/%.2f/%.2f`), the microsecond
series with one (`%.1f/%.1f/%.1f`), and a lone `-` when the series has no sample. `replayDepth` prints
the mean and maximum replay depth as `%.2f/%u` (the mean is `replayedTicks / replays` in `double`), or
`-` with no replay. Formatting a window line sorts every series it prints; the order of the samples is
not used after that.

## §4 The lines

In `Inline` and `FrameBounded` the format strings are byte-identical to the lines an Unreal frame host
printed before this header existed (§9), so log readers, comparison scripts and harness filters keep
working:
- `formatWindowLine(nowSeconds, totals)`: `[SimHost.Window] role=… stepping=… seconds=%.2f frames=…`
  and 24 further fields, 28 in all, up to `sampleOverflow=%u`;
- `formatFrameLine(dtSeconds, maxCatchUpSteps, minRateScale, maxRateScale, idleSlackSeconds = 0.0,
  monotonicTimeBase = false, waitsForThisFramesStep = false)`:
  `[SimHost.Frame] role=… stepping=… dt=%.6f maxCatchUpSteps=%u rateScale=[%.2f,%.2f] timeBase=%s`, with
  `worldDelta` by default and `monotonic` when `monotonicTimeBase` is true (the owner's `Monotonic` time
  base), then ` frameWait=thisFramesSingleStep` only when `waitsForThisFramesStep` is true (the owner's
  frame wait, `SimulationTickDriver-rationale.md` §4). The rate bounds are arguments with no default: the
  owner passes its configured bounds, so a configuration with other bounds cannot print the defaults by
  omission. At 0.75 and 1.25 the field reads `rateScale=[0.75,1.25]`, the text it had when the bounds were
  a literal in the format, and at 0.9 and 1.1 it reads `rateScale=[0.90,1.10]`. Two decimals round a bound
  that needs more, so 1.005 prints `1.00`. These two modes do not print the slack;
- `formatCatchUpCapWarningLine(requestedCap, maxStepsPerFrame, cap)`: `[SimHost.Frame]
  og.Sim.MaxCatchUpSteps=%d is outside [1, %u]; using %u`. The console-variable name is frozen text. It
  is the same in every mode;
- `BasicTickDriverFirstStepLine::format(threading, onOwnerThread, physicsStep, batchCount)`:
  `[SimHost.FirstStep] role=… thread=… physicsStep=%llu batch=%u`. The older overload
  `format(onOwnerThread, physicsStep, batchCount)` forwards with `Inline`, which prints the same text
  as `FrameBounded`.

`FreeRunning` prints the same window line with a suffix, and its own frame line (§8).

The words are `Authority`/`Client` for the role; `inline`, `worker` or `runner` for the stepping mode
(`Inline`, `FrameBounded`, `FreeRunning`); and for the first step's thread `runner` in `FreeRunning`,
otherwise `game` when the caller says the batch runs on the owning thread and `worker` when it does
not. The window does not decide which thread that is; its owner asks its thread context.

Every numeric conversion is the old one, with the argument type it expects: 64-bit counts are cast to
`unsigned long long` for `%llu`, and 32-bit counts go to `%u` as `uint32_t`. The one exception is the
frame line's rate bounds, two `double`s for `%.2f` where the old line had the literal text; at the
default bounds the printed line is the old one byte for byte. The log level of each line is the owner's
choice; the window only formats.

**No rate on the window line.** The window line carries no `rateScale=` field: the configured bounds are
on the frame line, and the window does not see the rate the scheduler runs at.

**Goldens.** `TickDriverWindow.WindowLine.Goldens` pins three complete window lines (authority inline
with every kind of sample, client worker with waits, replays, refusals and restores, and an empty
window), and `TickDriverWindow.FrameAndFirstStepLines.Goldens` pins the frame, cap-warning and
first-step lines. Byte equality with the engine formatter the lines replace is pinned on the Unreal
side by its own test, TickDriverFormatParity.FStringPrintfEqualsFormatHostLine, over at least twenty
value sets; this submodule's standalone build does not compile it. It formats the frame line at the
default bounds, the only bounds the engine formatter printed. The frame-line goldens also pin
`rateScale=[0.90,1.10]` at 0.9 and 1.1 and the two-decimal rounding of 0.99 and 1.005. The `FreeRunning`
lines have their own goldens (§8).

## §5 Buffers, reservations and allocation

The window allocates only in its constructor. It is a class template over a `char` allocator
(`BasicTickDriverWindow<Allocator>`; `TickDriverWindow` uses `std::allocator<char>`) so a test can
count every allocation it makes. The constructor reserves:
- each series' `std::vector<float>` to `kMaxSamplesPerSeries` (16,384 samples). A full series drops
  every further sample and counts it as overflow, so `add` never grows a vector. The two suffix series
  are reserved in every mode, so a door never allocates whatever the mode;
- one field buffer per series (`kPercentileFieldReserve`, 160 characters), one for `replayDepth`
  (`kReplayDepthFieldReserve`, 64), the window line (`kWindowLineReserve`, 4,096), the `FreeRunning`
  suffix (`kWindowSuffixReserve`, 512) and the frame lines (`kFrameLineReserve`, 1,536); the first-step
  line reserves `kFirstStepLineReserve` (256).

Each reservation holds the longest text its formats can produce from any argument values. A `%.2f` of
`-FLT_MAX` is 43 characters, so a field is at most 131. Measured on 2026-11-12, with every series at
`-FLT_MAX`, every count and total at its maximum and both seconds fields at `-DBL_MAX`:
- the window line is 2,226 characters in `Inline` and `FrameBounded`, and 2,574 in `FreeRunning`, whose
  suffix is 348 of them. (The figure written on 2026-10-16, 2,249, counted every field at 131; the
  microsecond fields are at most 128.)
- the frame line with `dt = -DBL_MAX` was 436, and 458 in `FreeRunning` with the slack at its maximum,
  while the rate bounds were literal text. Computed on 2026-11-20 for the bounds as `%.2f` arguments, with
  `dt` and both bounds at `-DBL_MAX` (317 characters for `dt`, 313 for each bound): 1,085 with
  ` frameWait=thisFramesSingleStep`, 1,054 without it, and 1,076 in `FreeRunning` with the slack at its
  maximum. `kFrameLineReserve` went from 512 to 1,536 then;
- the cap warning is at most 95 and the first-step line 98 (computed 2026-10-16; `runner` is as long as
  `worker`).

`formatHostLine` writes within a reservation without allocating, so no line or field allocates after
construction. The `FreeRunning` suffix is formatted into its own buffer and appended to the window line,
within that line's capacity. `restart` keeps every capacity (`clear`, never `shrink_to_fit`).

`TickDriverWindow.Samples.OverflowIsCountedWithoutAllocationAfterConstruction` counts with a counting
allocator: after construction, 16,385 samples in one series report `sampleOverflow=1`; every series full
at `±FLT_MAX` with every count and total at its maximum formats every line twice; and 20,000 rounds of
every note door, a line, a restart and a second line — all with no new allocation.
`TickDriverWindow.FreeRunning.NoAllocationAfterConstruction` does the same for a `FreeRunning` window:
16,385 samples through every door into all ten series report `sampleOverflow=10` and format a line with
the suffix, and every series full at the extremes formats the window, frame and first-step lines twice,
with no new allocation. The count covers what the window allocates through its allocator. It does not
see an allocation inside `std::sort` or `std::vsnprintf`, which the standard does not forbid; none is
claimed or measured here.

Each series has its **own** field buffer because all the fields of one window line are arguments of
one format call: they must all be alive and distinct when the line is formatted.

## §6 Threads

All window state belongs to the thread that owns the driver's frame (the host's frame thread):
the note doors, `restart`, `isDue` and the three formatters run there, and a step running on another
thread reaches the window only through the owner's rings, never directly. In `FreeRunning` that owner
is the runner, the dedicated thread that runs the authority loop and its steps; the frame thread does
not touch the window.

The first-step line is the exception: the first batch prints it from whichever thread runs the batch,
which may be a worker while the owning thread formats a window line. That is why it is a separate
class, `BasicTickDriverFirstStepLine`, with its own buffer and a role fixed at construction: it holds
no reference to a window, so the step thread cannot reach window state through it. Its owner makes
sure it is formatted once (the old frame host kept a "first step logged" flag beside it). In
`FreeRunning` the batch runs on the runner, so both are formatted on one thread.

## §7 What the `.cpp` holds

Only `tickDriverPercentilesOf`, exported with `OGSIMULATION_API`: the one non-template algorithm, so
every engine module that instantiates the window shares one sort-and-rank routine. Everything else is
a template over the allocator and lives in the header.

## §8 The `FreeRunning` lines

In `FreeRunning` the authority's steps run on a dedicated runner thread that wakes at step deadlines
on a monotonic clock, instead of once per engine frame. The window keeps the same lines and fields, so
tools that read them keep working, and adds what only that mode has:
- `[SimHost.Frame] role=… stepping=runner dt=%.6f maxCatchUpSteps=%u rateScale=[%.2f,%.2f]
  timeBase=monotonic idleSlackUs=%u`. `idleSlackUs` is the `idleSlackSeconds` argument in whole
  microseconds, rounded to the nearest: a slack below 0.5 µs (a negative one included) prints 0, and one
  at or above 4,294,967,294.5 µs prints 4294967295 (`tickDriverWindow::roundedMicroseconds`);
- `[SimHost.FirstStep] role=… thread=runner …`, whatever `onOwnerThread` says;
- `[SimHost.Window]` with `stepping=runner`, the 28 fields of §4 and, after `sampleOverflow`, the
  suffix ` lateUs=%s activityWakes=%u earlyWakes=%u idleFallbacks=%u approachUs=%s`. The two series
  print as the other microsecond series do (`%.1f/%.1f/%.1f`, or `-` with no sample).

The suffix is printed only in `FreeRunning`. The `Inline` and `FrameBounded` lines stay byte-identical to
§4's, whatever was noted; `TickDriverWindow.FreeRunning.TheSuffixIsAbsentInInlineAndFrameBounded`
pins it with the same notes in each mode.

The fields keep their names but measure a stepping wake of the runner instead of an engine frame:

| field | in `FreeRunning` |
|---|---|
| `frames` | stepping wakes: wakes that ran at least one step |
| `frameMs` | the period between stepping wakes |
| `startPhysicsUs` | one stepping wake's pump and batch |
| `maxStepsPerFrame` | the most steps in one wake |
| `endWaits` | 0: the runner does not wait for a frame's end |
| `endWaitUs`, `postPhysicsUs` | `-` |
| `shadowRestores`, `shadowRestoreUs`, `shadowRefused` | 0, `-` and 0: the authority has no shadow world |
| `lateUs` | per step, the pump time minus the step's deadline, in microseconds (`noteLateness`) |
| `activityWakes` | wakes ended by inbound activity before the deadline (`noteActivityWake`) |
| `earlyWakes` | idle returns that reported the deadline before the approach point, the deadline minus the slack (`noteEarlyWake`) |
| `idleFallbacks` | idles replaced by a plain sleep, because the idle hook could not wait or returned early too often (`noteIdleFallback`) |
| `approachUs` | sampled only with a slack > 0: the polled approach from the approach point to the deadline (`noteApproach`) |

The window stores what its owner notes; the meanings in the table are what the owner that runs the
`FreeRunning` loop passes. With every field noted,
`TickDriverWindow.FreeRunning.LinesMatchTheGoldens` pins the three lines as full golden strings, an
empty window, `restart` clearing the suffix, the slack's rounding and the first-step words of every mode.

## §9 Provenance

Written on 2026-10-16 for the og-tickdriver migration. It moves, engine-free, the window state and
line formatting of the brawler's Unreal frame host (SimulationFrameHostUImpl at commit a47b6f9): its
WindowSamplesUImpl, HostWindowCountsUImpl and HostWindowSamplesUImpl structs, the percentilesOf and
formatPercentiles helpers, startWindow_GameThread, logWindow_GameThread, the drain body of
drainStepCosts_GameThread, and the three log statements of [SimHost.Frame] and [SimHost.FirstStep]. The
differences:
- the format strings move from the engine's wide literals to `char` literals; `%s` arguments are
  `const char*`; every numeric conversion is unchanged;
- the engine string formatter becomes `formatHostLine` into buffers reserved at construction;
- the scheduler and ring reads become the `TickDriverWindowTotals` the owner passes in, and the clock
  reads become `nowSeconds` and the note doors' seconds;
- the four `…AtStart` counts become one `atStart` totals member;
- the eight named sample lists become an array indexed by `TickDriverWindowSeries`;
- the first-step line's game-thread test becomes the caller's `onOwnerThread`.

On 2026-11-12 the window gained the `FreeRunning` mode (§8): the `StepThreading` constructor, the
`runner` words, the `monotonic` frame line with its slack, the two suffix series and the three wake
counts. The `Inline` and `FrameBounded` lines did not change.

On 2026-11-19 `formatFrameLine` gained the `monotonicTimeBase` flag, which prints `timeBase=monotonic` in the
`Inline` and `FrameBounded` frame line. With the flag false, its default, the line is unchanged; the
`FreeRunning` line ignores it.

On 2026-11-20 it gained the `waitsForThisFramesStep` flag, which appends ` frameWait=thisFramesSingleStep` to
the `Inline` and `FrameBounded` frame line. With the flag false, its default, the line is unchanged; the
`FreeRunning` line ignores it.

Also on 2026-11-20 `formatFrameLine` gained the `minRateScale` and `maxRateScale` arguments, with no default,
and every frame line prints them as `rateScale=[%.2f,%.2f]` where it printed the literal `rateScale=[0.75,1.25]`
(§4, §8). A configuration with other bounds had printed the defaults. At 0.75 and 1.25 every line is unchanged.
`kFrameLineReserve` went from 512 to 1,536 to keep the longest frame line within its reservation (§5).

Nothing in this file depends on any document outside this submodule.
