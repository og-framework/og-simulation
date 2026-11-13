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
with the world host's thread context, and an idle slack that is not finite or not in `[0, dt)` are
refused by `OG_CHECK`s in the constructor; a full occupancy ring, both waits taken while the world lock
is held, an inline step off the owning thread, the occupancy door outside a step, the scheduler and the
host time read off the owning thread, and a driver destroyed with a step task outstanding are
`OG_CHECK`s. In `FreeRunning`, the loop run in another mode, off the owner thread or twice, `stop()` and
the destructor while the loop runs, the frame-thread pass run on the runner, the occupancy push and the
world-lock note off the runner, and a shadow restore are `OG_CHECK`s too. That `beforePump` runs
immediately before the pump, and that the block mode chooses this frame's task only under 1, are
ordinary rationale (§3, §6), pinned by the tests: a reorder or a swapped pair of operands is not typed on
a line a tag could cover.

**Provenance of the ids.** G-01 to G-06 are the successors, with fresh ids, of the six guards of the
engine-hosted frame host this header was transcribed from (rationale §14). G-07 and G-08 are new with the
`FreeRunning` mode and succeed nothing. Each of the first six ids succeeds the
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

## G-04 — The pump time is the accumulated frame delta in `Inline` and `FrameBounded`, and the runner's engine clock in `FreeRunning`

**Tag site:** `SimulationTickDriver.h`, in `startFrame`, immediately above
`m_hostTimeSeconds += frameDeltaSeconds;`.

**The prohibition.** In `Inline` and `FrameBounded`, do not set `m_hostTimeSeconds` here from the engine
clock (`nowSeconds`) or from any source other than the accumulated `frameDeltaSeconds`. `pumpAndRunSteps`
pumps `m_hostTimeSeconds`, and hands `beforePump` the same value, in every mode. In `FreeRunning`,
`startFrame` returns before this statement and `runAuthorityLoop` sets the value from the engine clock
minus the loop's origin: do not add frame deltas to it.

**The consequence.** The engine's frame delta is what the engine's pause, its slow motion and its
frame-time clamp act on: a paused world runs no frames and accumulates nothing, and a clamped hitch
reaches the scheduler clamped. A wall clock steps a paused world on resume, ignores slow motion, and
catches up a hitch the engine meant to drop. The render clock reads the same time base
(`hostNowSeconds` returns `m_hostTimeSeconds`, and the step deadlines come from this scheduler), so a
second clock would also drift the render interpolation from the deadlines.
`SimulationTickDriver.Inline.AnAuthoritySessionMatchesTheFrameHostTranscription` fails when the pump
reads the engine clock. In `FreeRunning` the runner wakes at the deadlines on the engine clock, and the
idle deadlines, the step hooks' stamps and the present clock read that clock: frame deltas would stop the
pump whenever the frame thread stalls, the stall the mode exists to isolate the steps from, and would
drift the pump from the deadlines the runner idles to.

**What breaks if it moves.** The accumulation stays in `startFrame`, after the `FreeRunning` return and
before `pumpAndRunSteps`, on the owning thread; inside `pumpAndRunSteps`, `beforePump` and then the pump
read it, after `m_blockingSteps` captures the earlier frames' task.

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
