<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationTickDriver.h` — guards

Every prohibition that governs a line of `SimulationTickDriver.h`. Each entry has an **opaque, stable id**.
In the header a single line `// ⛔G-nn` sits exactly where the forbidden edit would be typed.

**If this file and `SimulationTickDriver.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is moved
to a "Retired" section and its number is spent forever.

⛔ **Nothing in this file is a rationale.** The reasons live in `SimulationTickDriver-rationale.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below is
true and actionable to a reader with no game engine and no other repository.

The rules the compiler or a runtime check holds need no entry (rationale §8): the member order is two
`static_assert`s; a `FreeRunning` client, a `FrameBounded` authority, a role or threading that disagrees
with the world host's thread context, an idle slack that is not finite or not in `[0, dt)`, a `Monotonic`
time base in `FreeRunning`, and a `Monotonic` frame clamp that is not finite or shorter than the longest step
interval are refused by `OG_CHECK`s in the constructor; a full occupancy ring, both waits taken while the world lock
is held, an inline step off the owning thread, the occupancy door outside a step, the scheduler and the
host time read off the owning thread, `reanchor()` on a `HostDelta` driver, and a driver destroyed with a
step task outstanding are `OG_CHECK`s. In `FreeRunning`, the loop run in another mode, off the owner thread or twice, `stop()` and
the destructor while the loop runs, the frame-thread pass run on the runner, the occupancy push and the
world-lock note off the runner, a shadow restore, and a game's input sample taken in `FreeRunning` are
`OG_CHECK`s too. That `beforePump` runs immediately before the pump and that `sampleInput` runs first in
`startFrame`, before `beforePump`, are ordinary rationale (§3, §6), pinned by the tests: a reorder is not typed on a line a tag
could cover. Which task `endFrame` waits for is G-10.

**Provenance of the ids.** G-01 to G-06 are the successors, with fresh ids, of the six guards of the
engine-hosted frame host this header was transcribed from (rationale §14). G-07 and G-08 are new with the
`FreeRunning` mode, G-09 with the `Monotonic` time base, and G-10 with the frame wait; they succeed nothing. Each of the first six ids succeeds the
same number there: G-01 (the authority steps inline), G-02 (one per-frame call, before the steps, only
when steps are due), G-03 (no `+ 1` on the frame's first tick), G-04 (the scheduler is pumped with the
accumulated frame delta), G-05 (no wait while the world lock is held) and G-06 (one call after the
inline steps). The task that deletes the old frame host retires the old ids and names these.

---

## G-01 — The authority steps inline on its owning thread: the inline-or-task choice is the configured threading, and nothing else

**Tag site:** `SimulationTickDriver.h`, in `pumpAndRunSteps`, immediately above
`if (m_config.threading != StepThreading::FrameBounded)`.

**The prohibition.** Do not dispatch an `Inline` or a `FreeRunning` batch to the executor, and do not key
the choice between `runBatch` on this thread and `dispatchAfter` on anything but `m_config.threading`: not
an engine's threading switch, not whether a worker thread exists, not a role read anywhere else, and not
`== StepThreading::Inline`, which sends `FreeRunning` to the executor. Per mode: `Inline` runs the batch
here, on the frame thread; `FreeRunning` runs it here, on the runner, inside the authority loop; only
`FrameBounded` dispatches. The threading is fixed at world build from the role (the authority `Inline` or
`FreeRunning`; a client `FrameBounded` unless it chose to step inline), and the constructor checks it
against the world host's thread context.

**The consequence.** The authority's input release (`TickDriverStepHooks-guards.md` G-01) hands the
game the inputs of the tick it is about to simulate, and the game delivers them into objects that
belong to the owning thread. An authority step on a task thread touches them off that thread, and the
inline access predicates of the world host's thread context, which accept the owning thread, no longer
describe the thread that steps. In `FreeRunning` the owning thread is the runner: a dispatched batch
would run on an executor thread while the runner goes on to idle and pump, and nothing in the loop waits
for it. `SimulationTickDriver.FreeRunning.WokenAtTheInlineFrameTimesItGivesTheInlineAuthoritysStepTrace`
fails with the branch written as `== StepThreading::Inline`.

**What breaks if the tag moves.** The branch is the only place a batch chooses its thread. The
constructor refuses a `FrameBounded` authority and a threading that disagrees with the world host (both
`OG_CHECK`s), but a new dispatch path typed elsewhere would bypass both.

## G-02 — `onFrameStepsDue` runs once per frame, before the frame's steps, and only in a frame with due steps

**Tag site:** `SimulationTickDriver.h`, in `pumpAndRunSteps`, immediately above
`m_game->onFrameStepsDue(firstUpcomingSimTick, static_cast<int32_t>(numSteps));`.

**The prohibition.** Do not call `onFrameStepsDue` per step, from a step hook, or in a frame with no
due step (the `numSteps == 0u` return above it). It runs once per frame, before the frame's steps,
with the frame's first upcoming tick and its step count.

**The consequence.** The hook is the game's per-frame sample point: a game that measures the ticks it
simulates per frame from it gets several samples in one frame when it is called per step (every one
after the first reads as a zero-frame delta), and samples of zero steps it never produced before when it
is called in a frame without steps. `SimulationTickDriver.Inline.AnAuthoritySessionMatchesTheFrameHostTranscription`
fails with the call made in a zero-step frame.

**What breaks if it moves.** It must stay after the batch is built (it takes the batch's first tick)
and before `runBatch` or the dispatch, so the game sees the frame's steps before any of them runs.

## G-03 — The frame's first upcoming tick is the batch's first step plus the published offset, with no `+ 1`

**Tag site:** `SimulationTickDriver.h`, in `pumpAndRunSteps`, immediately above
`const int32_t firstUpcomingSimTick =`.

**The prohibition.** Do not read a simulation clock here, do not substitute the last completed tick, and
do not add 1. The value is the scheduler's first step of this frame (`batch.first`) plus the offset the
step hooks publish (J3, `TickDriverStepHooks-guards.md` G-02).

**The consequence.** In `FrameBounded`, a clock read is an unsynchronized read of a value the step task
writes. The last completed tick lags by the steps still in flight. A `+ 1` makes the value one high, and
both `onFrameStepsDue` and `afterInlineBatch` receive it (G-06). On an inline authority the value is
exactly the server tick plus one, because every earlier step has completed and published its offset.
`SimulationTickDriver.Inline.AnAuthoritySessionMatchesTheFrameHostTranscription` fails with `+ 1`.

**What breaks if it moves.** It must stay after the batch is built and before both per-frame calls,
which take it unchanged.

## G-04 — The pump time is the accumulated frame delta under `HostDelta`, and an engine clock minus an origin under `Monotonic` and in `FreeRunning`

**Tag site:** `SimulationTickDriver.h`, in `startFrame`'s `HostDelta` branch (the `else`), immediately above
`m_hostTimeSeconds += frameDeltaSeconds;`.

**The prohibition.** Under `HostDelta`, the default time base, do not set `m_hostTimeSeconds` here from the
engine clock (`nowSeconds`) or from any source other than the accumulated `frameDeltaSeconds`. A host that
wants the engine clock in `Inline` or `FrameBounded` sets `Config::frameTimeBase` to `Monotonic`, which takes
the other branch; it does not edit this statement. `pumpAndRunSteps` pumps `m_hostTimeSeconds`, and hands
`beforePump` the same value, in every mode. In `FreeRunning`, `startFrame` returns before this statement and
`runAuthorityLoop` sets the value from the engine clock minus the loop's origin: do not add frame deltas to
it.

**The consequence.** The engine's frame delta is what the engine's pause, its slow motion and its
frame-time clamp act on: a paused world runs no frames and accumulates nothing, and a clamped hitch
reaches the scheduler clamped. A wall clock steps a paused world on resume, ignores slow motion, and
catches up a hitch the engine meant to drop. The render clock reads the same time base
(`hostNowSeconds` returns `m_hostTimeSeconds`, and the step deadlines come from this scheduler), so a
second clock would also drift the render interpolation from the deadlines.
`SimulationTickDriver.Inline.AnAuthoritySessionMatchesTheFrameHostTranscription` fails when the pump
reads the engine clock. `Monotonic` is the configured alternative for a host whose frame delta carries no
such meaning: it brings its own frame clamp (G-09) and its own resume (`reanchor()`). In `FreeRunning` the
runner wakes at the deadlines on the engine clock, and the idle deadlines, the step hooks' stamps and the
present clock read that clock: frame deltas would stop the pump whenever the frame thread stalls, the stall
the mode exists to isolate the steps from, and would drift the pump from the deadlines the runner idles to.

**What breaks if it moves.** The accumulation stays in `startFrame`'s `HostDelta` branch, after the
`FreeRunning` return and before `pumpAndRunSteps`, on the owning thread; inside `pumpAndRunSteps`,
`beforePump` and then the pump read it, after `m_blockingSteps` captures the earlier frames' task.

## G-05 — The owning thread never waits for a step task while it holds the world lock

**Tag site:** `SimulationTickDriver.h`, in `waitForOutstandingSteps` (the wait of `stop()`), immediately
above `m_engine->waitFor(**steps);`.

**The prohibition.** Do not wait for a step task, here or in `endFrame`, while this thread holds the world
host's world lock (J2), and do not take J2 around either wait.

**The consequence.** Every step takes J2 for its whole tick (`lockForStep` in `runStep`). A thread that
holds J2 and waits for a step task deadlocks: the task waits for the lock, the thread waits for the task.

**Enforcement.** Both waits begin with an `OG_CHECK` on the thread context's "this thread holds the world
lock" query. The end-of-frame path normally holds the engine's completion event open with `deferUntil`
instead of blocking, so the tag marks the wait that always blocks.

## G-06 — `afterInlineBatch` runs once per frame, after the inline steps

**Tag site:** `SimulationTickDriver.h`, in `pumpAndRunSteps`, immediately above
`m_game->afterInlineBatch(firstUpcomingSimTick);`.

**The prohibition.** Do not move it into the step loop or a step hook, and do not call it before the
frame's steps or in a frame with no due step. It runs once, after `runBatch`, with the same
`firstUpcomingSimTick` that `onFrameStepsDue` received.

**The consequence.** It is the game's once-per-frame work after an inline batch. A game that takes a
per-frame reference tick from it (the first game's authority does: it feeds the tick its receipt gate
judges the next frame's arrivals against) would, called per step, end each frame on the frame's last
tick instead of its first, so its acceptance window would move with the frame's step count in every
multi-step frame. `SimulationTickDriver.Inline.AnAuthoritySessionMatchesTheFrameHostTranscription`
fails with the call made per step.

**What breaks if it moves.** It must stay inside the `Inline` branch, after `runBatch`: in `FrameBounded`
the batch has not run when `pumpAndRunSteps` returns.

## G-07 — An early return from the idle hook falls back to the runner's sleep on the third in a row

**Tag site:** `SimulationTickDriver.h`, in `idleUntilNextDeadline`, immediately above
`if (++earlyReturns == simulationTickDriver::kEarlyWakesBeforeFallback)`.

**The prohibition.** Do not loop back into `idleUntil` without the runner sleep: do not delete this
fallback, and do not change its condition so that it never fires.

**The consequence.** A hook that returns `Deadline` before the deadline, and keeps doing so (a timer that
fires early, a hook that cannot wait and says so as a deadline), makes the runner a busy loop: one hook
call per pass, a full core, until the approach point. `runner.sleepUntil` is the one wait the driver can
rely on. `SimulationTickDriver.FreeRunning.ThreeEarlyDeadlineReturnsFallBackToTheRunnerSleep` fails with
the fallback removed: with a hook that advances 1 µs per early return, a step takes 16,667 calls instead
of three.

**What breaks if it moves.** It must stay inside the idle's loop, after the early-wake note, so that it
counts consecutive early `Deadline` returns: `Activity` returns to the loop and `Unavailable` sleeps before
it is reached, and a `Deadline` at or after the approach leaves the loop.

## G-08 — The `FreeRunning` frame-thread pass runs the post-physics hook and nothing else

**Tag site:** `SimulationTickDriver.h`, in `endFrame`'s `FreeRunning` branch, immediately above
`m_game->onPostPhysics();`.

**The prohibition.** Do not pump, drain J6 or touch the window here: not through `runPostPhysics`,
`pumpAndRunSteps`, `drainStepCosts` or `logWindow`, and not by noting anything in `m_window`.

**The consequence.** In `FreeRunning` the runner owns the scheduler, the draining side of J6 and the
window, and it uses them while this pass runs on the frame thread. A pump here stops on the scheduler's
owner check; a J6 drain makes the frame thread a second consumer of a single-consumer ring; a window note
races the runner's notes and restarts. `SimulationTickDriver.FreeRunning.TheFrameThreadPassRunsOnlyOnPostPhysics`
fails with `runPostPhysics` here (a post-physics sample per frame in a window that must stay empty), and
stops on the scheduler's owner check with a pump here.

**What breaks if it moves.** It must stay inside the `FreeRunning` branch, after its owner check and
before its `return`; the default modes' post-physics pass below the branch is unchanged.

## G-09 — A `Monotonic` frame longer than the clamp re-anchors the next deadline by exactly the excess

**Tag site:** `SimulationTickDriver.h`, in `advanceMonotonicHostTime`, immediately above
`scheduler.reanchor(scheduler.nextDeadline() + excessSeconds);`.

**The prohibition.** Do not delete this call, and do not change its argument: not to the frame's own time
minus the clamp (`hostSeconds - m_config.monotonicFrameClampSeconds`), and not to the frame's own time
(`hostSeconds`).

**The consequence.** The clamp is `Monotonic`'s stand-in for the engine's frame-time clamp, and the
scheduler's `reanchor` is what counts it: the steps it skips are recorded as lost time of cause `reanchor`
(`SimulationScheduler-guards.md` G-04). Without the call a long frame is caught up in full, up to the cap,
and nothing is counted: `SimulationTickDriver.TimeBase.AOneSecondGapAdvancesTheClampAndCountsTheRestAsReanchorLoss`
fails with the gap frame running 60 steps instead of 24. Re-anchored at `nextDeadline() + excess`, the skipped
span is exactly the excess, and the deadlines after it are the `HostDelta` deadlines for the clamped delta,
moved by the excess: the frame runs the steps `HostDelta` runs for a frame of `monotonicFrameClampSeconds`.
Measured back from the frame's own time, the count is short of the excess by the part of the previous step
interval that had not yet elapsed (`nextDeadline()` minus the previous frame's host time), and the frames
after it no longer step as the clamped `HostDelta` frames do: in the same case, with that part half a step,
0.5917 s is counted instead of 0.6. At the frame's own time, the frame runs one step and the clamp's span is
counted as lost too: 1 step and 0.9917 s.

**What breaks if it moves.** It must stay before `pumpAndRunSteps`, which pumps the frame: after the pump,
`nextDeadline()` is past the frame's time and the steps the clamp meant to skip have run.

## G-10 — The frame wait waits for this frame's task only when the frame dispatched exactly one step

**Tag site:** `SimulationTickDriver.h`, in `waitsForThisFrame`, immediately above its `return`.

**The prohibition.** Do not widen `m_frameSteps == 1u` to every frame with steps (`>= 1u`, `> 0u`), and do
not drop the condition: the frame wait must never wait for a frame that dispatched two or more steps. Do not
drop the `blockMode == 1 ||` operand either.

**The consequence.** The wait is the owning thread's: under `ThisFramesSingleStep` the end of physics holds
the frame until this frame's task completes. A catch-up frame dispatches up to the cap (`kMaxStepsPerFrame`,
240) steps in one task; waiting for it puts the whole catch-up on the owning thread, the stall the
`FrameBounded` task exists to keep off it, in exactly the frames that are already late.
`SimulationTickDriver.FrameWait.AStepPublishedMidFrameIsShownByThatFramesPostPhysics` fails on its 2-step
frames (each shows its own two steps). Without the block-mode operand, block mode 1 stops waiting for this
frame's task under the default frame wait, and the `FrameBounded` transcription case's block-mode-1 frames
fail.

**What breaks if it moves.** `startFrame` writes `m_frameSteps` from the pump's count of the same frame;
the condition must be read in `endFrame`, after it. Read before the pump, it is the previous frame's count.
