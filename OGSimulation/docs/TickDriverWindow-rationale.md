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
construction: STL only, plus `LostTime` from `SimulationScheduler.h` and `formatHostLine` from
`TickDriverLog.h`. Everything below is true to a reader with no game engine and no other repository.

---

## §1 What it is for

A tick driver reports how its frames and steps went in three host log lines:
- `[SimHost.Frame]`, once when it starts: role, stepping mode, step size, catch-up cap, rate bounds and
  time base; plus a Warning variant when the requested catch-up cap had to be clamped;
- `[SimHost.FirstStep]`, once, from the thread that runs the first batch of steps: which thread stepped;
- `[SimHost.Window]`, once per window of `tickDriverWindow::kWindowSeconds` (10 s): what the frames and
  steps of that window cost.

`BasicTickDriverWindow` holds the window's state (counts and eight sample series) and formats all three
lines except the first-step line, which `BasicTickDriverFirstStepLine` formats (§6). It is a passive
recorder: it **reads no clock and no scheduler**. Its owner passes every time and every running total
in, so the same window serves any engine and any time base, and a test drives it with plain numbers.

## §2 The window

**Counts** (`TickDriverWindowCounts`): `startSeconds`, `frames`, `steps`, `maxStepsPerFrame`,
`endWaits`, `j2Waits`, `replays`, `replayedTicks`, `maxReplayDepth`, `resimRefused`, `shadowRefused`, and
`atStart`, the owner's running totals when the window opened.

**Running totals** (`TickDriverWindowTotals`): `lost` (the scheduler's `LostTime`: dropped steps and
seconds), `ignoredTimeSamples`, and two ring drop counts, `j1Drops` and `j6Drops`. The owner reads them
from its scheduler and rings and passes them to `restart` (they become `atStart`) and to
`formatWindowLine`; the line prints each as the difference "at the line minus at the start", in the
total's own type. An unsigned total that went backwards therefore wraps, as a subtraction of the raw
totals does.

**Series** (`TickDriverWindowSeries`, eight, in the line's order): `FrameMs`, `StartPhysicsUs`,
`EndWaitUs`, `PostPhysicsUs`, `J2WaitUs`, `StepUs`, `PreUs`, `ShadowRestoreUs`. The `static_assert`
in `tickDriverWindow` pins the count to eight.

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

`noteStepCost` records one step's cost: `StepUs` always; `PreUs` on the authority for every step, and
on a client only for a step that replayed (`replayedTicks > 0`), so that on a client the field reports
the cost of the resimulation check and the replay rather than of every step's preparation. A step
that replayed also adds one to `replays` and `replayedTicks` to `replayedTicks`, and raises
`maxReplayDepth`; `resimRefused` adds one to the count of the same name.

The scaling is a `double` multiplication of the owner's difference, then a conversion to `float`: the
same value the owner would get by scaling the difference itself, so where the subtraction happens does
not change a sample.

`counts()` and `samples(series)` are public, mutable and const, so an owner can record a count the
doors do not cover and a test can set any count directly (the extremes in §5).

**`restart(nowSeconds, totals)`** opens a new window: it zeroes every count, sets `startSeconds` and
`atStart`, and clears every series (values and overflow) without releasing capacity.
**`isDue(nowSeconds)`** is true once `nowSeconds − startSeconds ≥ kWindowSeconds`. The owner calls
`formatWindowLine`, logs it, and calls `restart` with the same time: nothing in the window closes
itself.

**`sampleOverflow()`** is the sum of the eight series' overflow counts (§5).

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
`-` with no replay. Formatting a window line sorts every series; the order of the samples is not used
after that.

## §4 The lines

The format strings are byte-identical to the lines an Unreal frame host printed before this header
existed (§8), so log readers, comparison scripts and harness filters keep working:
- `formatWindowLine(nowSeconds, totals)`: `[SimHost.Window] role=… stepping=… seconds=%.2f frames=…`
  and 24 further fields, 28 in all, up to `sampleOverflow=%u`;
- `formatFrameLine(dtSeconds, maxCatchUpSteps)`: `[SimHost.Frame] role=… stepping=… dt=%.6f
  maxCatchUpSteps=%u rateScale=[0.75,1.25] timeBase=worldDelta`. The rate bounds and the time-base
  word are part of the text, not arguments;
- `formatCatchUpCapWarningLine(requestedCap, maxStepsPerFrame, cap)`: `[SimHost.Frame]
  og.Sim.MaxCatchUpSteps=%d is outside [1, %u]; using %u`. The console-variable name is frozen text;
- `BasicTickDriverFirstStepLine::format(onOwnerThread, physicsStep, batchCount)`: `[SimHost.FirstStep]
  role=… thread=… physicsStep=%llu batch=%u`.

The words are `Authority`/`Client` for the role, `inline`/`worker` for the stepping mode, and
`game`/`worker` for the first step's thread: `game` when the caller says the batch runs on the owning
thread. The window does not decide which thread that is; its owner asks its thread context.

Every numeric conversion is the old one, with the argument type it expects: 64-bit counts are cast to
`unsigned long long` for `%llu`, and 32-bit counts go to `%u` as `uint32_t`. The log level of each line
is the owner's choice; the window only formats.

**A planned change.** A time-dilation controller will add a `rateScale=` field to the window line in
its own change, with new goldens. Until then the field is absent.

**Goldens.** `TickDriverWindow.WindowLine.Goldens` pins three complete window lines (authority inline
with every kind of sample, client worker with waits, replays, refusals and restores, and an empty
window), and `TickDriverWindow.FrameAndFirstStepLines.Goldens` pins the frame, cap-warning and
first-step lines. Byte equality with the engine formatter the lines replace is pinned on the Unreal
side by its own test, TickDriverFormatParity.FStringPrintfEqualsFormatHostLine, over at least twenty
value sets; this submodule's standalone build does not compile it.

## §5 Buffers, reservations and allocation

The window allocates only in its constructor. It is a class template over a `char` allocator
(`BasicTickDriverWindow<Allocator>`; `TickDriverWindow` uses `std::allocator<char>`) so a test can
count every allocation it makes. The constructor reserves:
- each series' `std::vector<float>` to `kMaxSamplesPerSeries` (16,384 samples). A full series drops
  every further sample and counts it as overflow, so `add` never grows a vector;
- one field buffer per series (`kPercentileFieldReserve`, 160 characters), one for `replayDepth`
  (`kReplayDepthFieldReserve`, 64), the window line (`kWindowLineReserve`, 4,096) and the frame lines
  (`kFrameLineReserve`, 512); the first-step line reserves `kFirstStepLineReserve` (256).

Each reservation holds the longest text its formats can produce from any argument values: a `%.2f` of
`-FLT_MAX` is 43 characters, so a field is at most 131; the window line with every field at its longest
and both seconds fields at `-DBL_MAX` is 2,249; the frame line with `dt = -DBL_MAX` is 436; the cap
warning is at most 95 and the first-step line 98 (computed 2026-10-16). `formatHostLine` writes within a
reservation without allocating, so no line or field allocates after construction. `restart` keeps every
capacity (`clear`, never `shrink_to_fit`).

`TickDriverWindow.Samples.OverflowIsCountedWithoutAllocationAfterConstruction` counts with a counting
allocator: after construction, 16,385 samples in one series report `sampleOverflow=1`; every series full
at `±FLT_MAX` with every count and total at its maximum formats every line twice; and 20,000 rounds of
every note door, a line, a restart and a second line — all with no new allocation. The count covers
what the window allocates through its allocator. It does not see an allocation inside `std::sort` or
`std::vsnprintf`, which the standard does not forbid; none is claimed or measured here.

Each series has its **own** field buffer because all eight fields are arguments of one window line: they
must all be alive and distinct when the line is formatted.

## §6 Threads

All window state belongs to the thread that owns the driver's frame (the game thread in an engine):
the note doors, `restart`, `isDue` and the three formatters run there, and a step running on another
thread reaches the window only through the owner's rings, never directly.

The first-step line is the exception: the first batch prints it from whichever thread runs the batch,
which may be a worker while the owning thread formats a window line. That is why it is a separate
class, `BasicTickDriverFirstStepLine`, with its own buffer and a role fixed at construction: it holds
no reference to a window, so the step thread cannot reach window state through it. Its owner makes
sure it is formatted once (the old frame host kept a "first step logged" flag beside it).

## §7 What the `.cpp` holds

Only `tickDriverPercentilesOf`, exported with `OGSIMULATION_API`: the one non-template algorithm, so
every engine module that instantiates the window shares one sort-and-rank routine. Everything else is
a template over the allocator and lives in the header.

## §8 Provenance

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

Nothing in this file depends on any document outside this submodule.
