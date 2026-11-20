<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationTickDriver.h` — rationale

The narrative behind `BodySlotOccupancyCommand`, the `simulationTickDriver` constants and `SimulationTickDriver`,
the engine- and game-agnostic frame pipeline that pumps the scheduler, runs or dispatches each frame's
batch of steps, waits for it at the end of the frame, and closes the host's telemetry window; in
`FreeRunning` it runs the same pipeline as the authority's loop on a runner thread of its own. The header
keeps its licence, its docs pointer, its code, its `OG_CHECK`s, three `static_assert`s (two on the member
order, one on `isFiniteSeconds`) and nine guard tags; everything that explains it lives here.
Prohibitions: `SimulationTickDriver-guards.md`.

**If this file and `SimulationTickDriver.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL and glm only, through og-simulation's own headers. Everything below is true to a reader
with no game engine and no other repository.

---

## §1 What it is

`SimulationTickDriver<GameT, WorldHostT, PresenterT, EngineT>` is a class template over four
collaborators, each constrained by its concept from `TickDriverConcepts.h` (`TickDriverGame`,
`TickDriverWorldHost`, `TickDriverPresenterSteps`, `TickDriverEngine`). In `Inline` and `FrameBounded` an
engine adapter calls two entry points per frame on the owning thread:

* `startFrame(frameDeltaSeconds)` at the start of the engine's physics phase: it lets a game that samples
  its input take the frame's sample (§6), times the frame, pumps the scheduler with the frame's host time
  (the accumulated frame delta under the default `HostDelta` time base, the engine clock minus an origin
  under `Monotonic`, §6), and either runs the frame's steps on this thread (`Inline`) or dispatches them as
  one executor task chained after the previous frame's (`FrameBounded`);
* `endFrame(completion, blockMode)` at the end of the physics phase: it waits for the step task the block
  mode names (by holding the engine's completion event open with `deferUntil`, or by a blocking wait when
  there is no event), then runs the post-physics pass.

In `FreeRunning` (the authority only) a third entry point replaces both for the pump and the batch:

* `runAuthorityLoop(runner, stopRequested)` on the runner thread: it wakes at each step deadline on the
  engine clock, pumps and runs the due steps inline, and idles to the next deadline until the stop flag is
  set (§10, §11). `startFrame` then returns at once and `endFrame` runs only the game's post-physics hook
  on the frame thread (§12).

`stop()` ends the session: it clears the running flag, then waits for every outstanding step task; in
`FreeRunning` it first checks that the loop has returned (§13). A `Monotonic` driver has one more entry
point, `reanchor()`, which the host calls on the owning thread when the game resumes from a pause (§6). The driver owns the scheduler, the window
and the mailboxes (§5); it borrows the game, the world host, the presenter's step doors and the engine. It
is header-only and adds no indirect call: every collaborator is a template parameter and every call on it
is direct.

An engine host instantiates it with its own game binding, world host, presenter and engine. Its behaviour is a
transcription of the engine-hosted frame host and of the per-step body of the engine-hosted Jolt host (§14), with one
intended addition, the `beforePump` seam (§6), and three added after it: the `FreeRunning` mode, the
`sampleInput` hook for a game that models `TickDriverSampleGame` (§6), and the `Monotonic` time base (§6).

## §2 Ownership and member order

The driver holds raw pointers to its four borrowed collaborators, then, in this order:

| member | what |
|---|---|
| `m_config` | the configuration, `const` |
| `m_running`, `m_loggedFirstStep`, `m_hostTimeSeconds`, `m_lastFrameStartSeconds`, `m_monotonicOriginSeconds`, `m_frameSteps` | the running flag, the first-step flag, the host time, the previous frame's start, the `Monotonic` origin (the engine clock in `begin()`, §6) and the step count of the frame's own dispatch (§4, the frame wait) |
| `m_scheduler` | `std::optional<SimulationScheduler>`, built in the constructor after the step driver |
| `m_window`, `m_firstStepLine` | the telemetry window and the first-step line (`TickDriverWindow.h`) |
| `m_bodySlotOccupancyCommands` | J1 |
| `m_stepCosts` | J6 |
| `m_tickOffset` | J3 |
| `m_publishedRateScale` | J8, the rate scale the runner publishes after each `beforePump` (§5, §12) |
| `m_loopOriginSeconds` | the engine clock at the loop's start, `-1.0` before it (§10, §12) |
| `m_loopActive` | true while `runAuthorityLoop` runs (§13) |
| `m_pendingSteps`, `m_blockingSteps` | the executor handles (§4) |
| `m_doors` | the driver's part of the step doors |
| `m_hooks` | `std::optional<TickDriverStepHooks<…>>` |
| `m_stepDriver` | `std::optional<SimulationStepDriver<…>>`, last |

Members destruct in reverse declaration order, so the step driver goes first: it holds the hooks by
reference and, on a client, unregisters its resync callback from the game's manager in its destructor. The
hooks hold the doors, and the doors reach J1, J3, J6 and the step driver. Two `static_assert`s on
`__builtin_offsetof` pin the order: config < scheduler < window < J1 < J6 < J3 < J8 < the loop origin < the
loop-active flag < the pending handle < the blocking handle, and the blocking handle < doors < hooks < step
driver.

The hooks and the step driver are `std::optional`s, as in the engine-hosted Jolt host, because the
constructor builds them under the world lock (§3) before it builds the scheduler, which is also an
optional so that the constructor can follow the old start-of-play order statement for statement.

The doors are a nested class, not the driver itself, because `StepDoors<DriverT, …>` checks
`DriverStepDoors<DriverT>` when the hooks' type is named, and the driver is still incomplete at its own
member declarations: a class that names itself as `DriverT` in a member's type fails to compile, the
constraints of `StepDoors` not satisfied. The nested class is complete at its closing brace; its bodies
reach the driver's private members through a pointer.

## §3 The frame, step by step

**Construction** (the old start of play):

1. Seven `OG_CHECK`s: a `FreeRunning` client is refused; the authority is `Inline` or `FreeRunning`; the
   configured role equals the world host's thread-context role; the configured threading equals the
   thread context's `threading()`; `idleSlackSeconds` is finite and in `[0, dtSeconds)`; a `Monotonic` time
   base is refused in `FreeRunning`; and under `Monotonic`, `monotonicFrameClampSeconds` is finite and at
   least the longest step interval, `dtSeconds / minRateScale` (§6). Finiteness is read
   from the exponent bits (`simulationTickDriver::isFiniteSeconds`, pinned by a `static_assert` over both
   zeros, both largest finite values, the smallest denormal, both infinities and three NaNs), because a comparison can be folded to
   true for a NaN under a fast floating-point model.
2. Under `worldHost.lockForStep()` (the world lock, J2), build the hooks and then the step driver
   (`StepDriverConfig{ dt, ResimPolicy::OnRequest, 0 }`). The step driver parks every slot in its
   constructor, a world write, so it is built under J2.
3. `begin()`: the cap (§6), the cap Warning when it was clamped, the scheduler (`dt`, the cap, the rate
   bounds, start time 0), one `nowSeconds()` read that is both the `Monotonic` origin and the time the window
   is restarted at with the current totals, the running flag, and the `[SimHost.Frame]` line, which carries
   the slack in `FreeRunning` and the time-base word (§7).

**`startFrame(frameDeltaSeconds)`**, owning thread. In `FreeRunning` it returns at once. Otherwise, when the
game models `TickDriverSampleGame`, it first calls `sampleInput()` (§6); a game that does not compiles no
call there. Then:

1. Take the frame's start time; when there was a previous frame (`m_lastFrameStartSeconds > 0.0`), note
   the period. The window is handed the raw `double` difference.
2. The host time. Under `HostDelta`, `m_hostTimeSeconds += frameDeltaSeconds` (`⛔G-04`). Under `Monotonic`,
   `advanceMonotonicHostTime(start time)`: the start time minus the origin, with the frame clamp applied
   first when the frame is longer than it (`⛔G-09`, §6). Then `pumpAndRunSteps`:
   1. `m_blockingSteps = m_pendingSteps`: the task dispatched in earlier frames, captured before this frame's
      dispatch, every frame, with steps or not.
   2. `beforePump(m_hostTimeSeconds, rate)`; in `FreeRunning`, publish the rate scale to J8 (§5);
      `pump(m_hostTimeSeconds)`. No step due: return 0.
   3. The batch (J4) by value: `first = physicsStepCount() − numSteps`, the count, each step's deadline. In
      `FreeRunning`, one lateness sample per step (§10).
   4. `firstUpcomingSimTick = first + J3` (`⛔G-03`, §5).
   5. `onFrameStepsDue(firstUpcomingSimTick, numSteps)` (`⛔G-02`).
   6. `Inline` and `FreeRunning` (`⛔G-01`): `runBatch` here, then `afterInlineBatch(firstUpcomingSimTick)`
      once (`⛔G-06`). `FrameBounded`: `dispatchAfter(previous, task)`, where `previous` is the pending task
      when it is still pending and a default handle otherwise; the result becomes `m_pendingSteps`.
3. Keep the step count in `m_frameSteps` for this frame's `endFrame` (§4, the frame wait); note the frame, its
   step count and its own duration (`noteStartPhysics`).

`pumpAndRunSteps` takes no argument: it pumps `m_hostTimeSeconds`, which `startFrame` writes and the
authority loop sets (§6, §10), so both modes share every statement from the capture of `m_blockingSteps`
to the end of the batch.

**`runBatch`** logs the `[SimHost.FirstStep]` line once (its `thread=` word is `game` when the thread is
the role's owner thread, `worker` otherwise, and `runner` in `FreeRunning`), then runs
`runStep(first + i, deadline[i])` in order. Each step:

1. `OG_CHECK` that an `Inline` step runs on the owning thread;
2. `worldHost.lockForStep()` (J2 for the whole step), then a `StepScope` on the world host's thread context
   (the step-running bit the hooks and the access predicates test);
3. `hooks.currentStepDeadline = deadline` (the presenter copies it into the step's render snapshot);
4. `m_stepDriver->runTick(physicsStep)`;
5. `worldHost.afterRunTick_Step(step, outcome)`, still under the lock and the scope.

**After the batch**, a client whose game models `TickDriverDisplayGame` (`TickDriverConcepts-rationale.md` §11) gets
one `publishAfterBatch()`: under a world lock and a `StepScope` of its own, the hooks' `afterBatch()`, which calls the
game's `publishForDisplay()`. It runs on the thread that ran the batch, after its last step and before any later
batch: a later frame's task is dispatched after this one (`dispatchAfter`), so the engine starts it only once this one
has returned. Nothing steps during it, whatever `endFrame` waits for. An authority never calls it, and for a game
without the hook the call, the lock and the scope compile out (`if constexpr`), so its trace and its lock count are
unchanged. Under `ThisFramesSingleStep` a one-step frame waits for the task, so its post-physics pass reads what the
hook published; a frame that did not wait reads the newest batch published before it.

`runStep` is private and `runBatch` is its only caller, so "a step runs only inside the driver's batch" is
the type's shape; the old per-step body checked it at run time with a thread-local batch marker. A
`FreeRunning` step needs no check of its own here: `runAuthorityLoop` checks that it runs on the owner, and
`pumpAndRunSteps` reaches the scheduler only through the owner-checked accessor.

**`endFrame(completion, blockMode)`**, owning thread. In `FreeRunning` it runs on the frame thread instead:
it `OG_CHECK`s that this thread is not the role's owner (the runner), calls `onPostPhysics()` and returns
(`⛔G-08`): no wait, no J6 drain, no window. Otherwise:

1. `OG_CHECK` that this thread does not hold J2 (G-05).
2. The task to wait for (`waitsForThisFrame`, `⛔G-10`): `m_pendingSteps` under block mode 1, and under
   `Config::frameWait == ThisFramesSingleStep` when this frame dispatched exactly one step (§4); otherwise
   `m_blockingSteps`.
3. Nothing to wait for, or it is complete: run the post-physics pass at once, with no wait sample.
4. A completion event that accepts the deferral (`deferUntil` returns true): return; the continuation runs
   the pass after the task. It captures the driver and returns at once when `m_running` is false.
5. Otherwise (no event, or `deferUntil` returns false): `waitFor` the task, then run the pass.

**The post-physics pass:** note the end-of-frame wait when there was one; `onPostPhysics()`; note its
duration; drain J6 into the window; when the window is due, format the `[SimHost.Window]` line, log it,
and restart the window with the same `now` and the same totals.

**`stop()`:** `OG_CHECK` that the authority loop is not running (§13); `m_running = false`, so a
continuation that is still deferred does nothing when it runs; then wait (G-05's `OG_CHECK`) for the
pending and then the blocking task, each one that is set, and reset both. No step runs after `stop()`
returns. The engine adapter stops calling the two entry points (the old host unregistered its tick
functions in the same statement that cleared the flag).

## §4 Threading modes

| Mode | Who pumps | Who runs the batch | `endFrame` |
|---|---|---|---|
| `Inline` | the owning thread, in `startFrame` | the same thread, then `afterInlineBatch` | never waits: the handles stay unset |
| `FrameBounded` | the owning thread, in `startFrame` | an executor task chained after the previous frame's | waits for earlier frames' task (block mode 0) or also this frame's (1, or a one-step frame under the frame wait `ThisFramesSingleStep`), by deferral or a blocking wait |
| `FreeRunning` | the runner (the owning thread), in `runAuthorityLoop`, at each wake | the runner, inline, then `afterInlineBatch` | runs `onPostPhysics` only, on the frame thread; never waits (§12) |

`FreeRunning` is the authority's only: the constructor refuses a `FreeRunning` client, as the thread
context's `configure` does. A client is `FrameBounded`, or `Inline` when it chose to step inline.

Block mode 0 means the end of a frame waits only for steps dispatched in earlier frames; this frame's task
may still be stepping while the post-physics pass runs. Block mode 1 (debugging) also waits for this frame's.

**The frame wait, `Config::frameWait`.** `EarlierFrames`, the default, is block mode 0 as above.
`ThisFramesSingleStep` also waits for this frame's task when the frame dispatched exactly one step, so the
post-physics pass of that frame runs after the step and shows it. A host draws what that pass presents.
Under block mode 0 a step still running when the frame reaches the pass is shown by the next frame's pass
instead, one frame period later. Whether it is still running is a race between the step task and the
frame thread's work between the dispatch and the end of physics, which the host does not control.
* **The bound.** The wait is at most one step: a frame that dispatched two or more steps (a catch-up)
  waits only for earlier frames' task, as block mode 0 does, and its steps are shown by the next frame. A
  frame that dispatched none has no new task (the pending task is the earlier frames' one), so it waits as
  block mode 0 does too.
* **Where it applies.** Block mode 1 still waits for this frame's task whatever its size. In `Inline` and
  `FreeRunning` the handles stay unset, so the setting changes nothing there.
* **What it changes.** No hook, task or line is added or removed: only the post-physics pass of a one-step
  frame moves after that frame's step (§9's `FrameWait` cases). The wait is noted as an end-of-frame wait
  (`endWaits`, `endWaitUs` in `[SimHost.Window]`), so its cost is in the host's own window. The
  `[SimHost.Frame]` line names it (§7).

The executor handles are `std::optional<EngineT::Handle>`. An unset optional is the old code's invalid
event: `stop()` waits for each handle that is set, even one that has completed (a no-op wait), exactly as the
old wait passed every valid event to one wait call. "Pending" is `has_value()` and `engine.isPending`.
`FreeRunning` never dispatches, so its handles stay unset.

**The owning thread** is the thread that configured the world host's thread context: the context's
`isOwnerThreadForRole()` replaces the engine's game-thread query everywhere in the driver (the scheduler
accessor, the host time, the first-step line's thread word, the inline-step check, and the doors' owner test
that the authority's `beforeTick` checks). In `FreeRunning` that thread is the runner, which builds the
world host (`TickDriverThreadContext-rationale.md` §8); the frame thread is not an owner.

**One copy per shared library.** The thread context's masks and counts are header thread-locals
(`TickDriverThreadContext-rationale.md` §7). The driver's `StepScope` and the world host's `configure` must be
compiled into the same shared library, which holds as long as both are header templates instantiated by
the same module.

## §5 The mailboxes and the doors

* **J1, occupancy commands** (owning thread → step): `SpscRing<BodySlotOccupancyCommand, 64>`.
  `pushBodySlotOccupancy(slot, occupied)` pushes one; a full ring is an `OG_CHECK` (a lost command leaves the world
  stepping the wrong body set). The hooks drain it first thing in `beforeTick`, through the doors'
  `applyBodySlotOccupancyCommands_Step`, into the step driver's `noteBodySlotOccupancy`; the door `OG_CHECK`s it runs inside
  a step. In `FreeRunning` the push `OG_CHECK`s the runner, the ring's one producer (§12).
* **J3, the published tick offset** (step → owning thread): `std::atomic<int64_t>`, relaxed. The hooks'
  `afterTick` stores `outcome.tick − outcome.physicsStep`; `pumpAndRunSteps` adds it to `batch.first`, with
  no `+ 1`, because `outcome.tick` is the tick the step integrated (`⛔G-03`). Before the first step the
  offset is 0. `tickOffset()` is a relaxed load of it, the time-dilation seam's third door (§6).
* **J4, the batch** (owning thread → task): first step, count, and one deadline per step in a
  `std::array<double, 240>`, copied into the task by value.
* **J6, the step cost** (step → owning thread): `SpscRing<StepCostSample, 256>`; the hooks push one sample
  per step, a full ring drops it and counts the drop, and the post-physics pass drains it into the window
  (`noteStepCost`). In `FreeRunning` the runner drains it after each wake (§10).
* **J8, the published rate scale** (runner → any thread, `FreeRunning` only): `std::atomic<double>`,
  1.0 at construction. `pumpAndRunSteps` stores the scheduler's `rateScale()` with release order right
  after `beforePump`, so a rate the game set through the door during this wake is the one published;
  `stepIntervalSeconds()` loads it with acquire order (§12). The other modes never write or read it.

The doors are what `TickDriverStepHooks` needs from the driver (`DriverStepDoors`): the J1 drain, the J3
store, the J6 push, the step-running test and the owner-thread test, both read from the world host's
thread context.
`savesTicks()` and `grantedResims()` forward, read-only, to the step driver's `savesTicks()` and `getDiagnostics().grantedResims()` for a presenter's step side; the step driver exists once the constructor returns.

## §6 Time base, `beforePump`, the rate door and the cap

**Time base.** `pumpAndRunSteps` pumps `m_hostTimeSeconds` and hands `beforePump` the same value, in every
mode. What writes it is per mode and, in `Inline` and `FrameBounded`, per `Config::frameTimeBase`
(`⛔G-04`):

* `Inline` and `FrameBounded` under `HostDelta`, the default: the sum of the frame deltas the engine adapter
  passes to `startFrame`, never the engine clock (`timeBase=worldDelta`). `hostNowSeconds()` returns that sum
  and `stepIntervalSeconds()` the scheduler's step over its rate scale; both are owning-thread doors (the
  host time `OG_CHECK`s the owning thread, the interval goes through the scheduler accessor), so a render
  clock built on them shares the deadlines' time base. The engine's pause, slow motion and frame-time clamp
  reach the pump through the deltas.
* `Inline` and `FrameBounded` under `Monotonic` (`timeBase=monotonic`): `startFrame` ignores
  `frameDeltaSeconds` and writes the frame's start time minus the origin, the engine clock read once in
  `begin()`. It is the same clock read the frame period is taken from, so the frame reads the clock exactly
  as often as under `HostDelta`. With a scripted clock that reads 0 at the driver's construction and advances
  by each frame's delta before the frame, the host times are bit for bit the `HostDelta` sums, and with them
  every pump, step, deadline and line except the frame line's time-base word. The same owning-thread doors
  serve the render clock. What the engine's delta carried, the time base carries itself:
  * **The frame clamp.** A frame whose advance (its host time minus the previous frame's) is longer than
    `Config::monotonicFrameClampSeconds` (0.4 s by default, the length of the frame-time clamp the first
    engine applies to its own delta) runs only the clamp's worth of steps. Before the pump,
    `advanceMonotonicHostTime` calls the scheduler's `reanchor` at the next deadline plus the excess
    (`⛔G-09`): the scheduler skips exactly the excess and counts it as lost time of cause `reanchor`
    (`SimulationScheduler-guards.md` G-04), and the frame then runs the steps a `HostDelta` frame of the
    clamp's length runs, on deadlines moved by the excess. A 1 s frame at 60 Hz that starts half a step
    before a deadline runs 24 steps and counts 0.6 s and 35 steps as `reanchor` loss, where `HostDelta` given
    the unclamped second runs 60. The step count is the scheduler's: the steps due in the skipped span minus
    the one at the new anchor, and the test's step, the single-precision 1/60, is a little longer than 1/60,
    so 36 steps would take 0.60000003 s. The host time itself is not clamped: it stays the clock minus the
    origin, and the deadlines after the re-anchor
    are in that base, so the render clock still brackets the render time between them. The constructor
    requires the clamp to be at least the longest step interval (`dtSeconds / minRateScale`): the next
    deadline is at most one interval after the previous frame's host time, so the re-anchored deadline is
    never past the frame's own.
  * **Resume.** `reanchor()` is the host's call on the owning thread when the game resumes from a pause
    that stopped its frames. It reads the clock, re-anchors the scheduler at the clock minus the origin
    (the whole pause, from the next deadline, is `reanchor` loss), and writes that time to
    `m_hostTimeSeconds`, so the first frame after it runs the step due at the resume and no clamp's worth of
    catch-up. Without the call the first frame after a pause is a long frame: the clamp's steps run and the
    rest is counted. The frame period the window notes for that frame still spans the pause.
    `OG_CHECK`ed to a `Monotonic` driver: under `HostDelta` the engine's pause stops the deltas, and nothing
    is owed on resume.
  * **Loss by cause.** `getDiagnostics().lostTimeByCause()` returns the scheduler's split; `lostTime()` and
    the window line keep the sum, so the window's `lostSteps` and `lostSeconds` include the clamp's and the
    resume's loss.
  * **Not in `FreeRunning`.** The constructor refuses `Monotonic` with `FreeRunning`, which already pumps the
    runner's engine clock minus the loop's origin and has no frame clamp (§10).
* `FreeRunning`: `engine.nowSeconds()` minus the loop's origin, read on the runner at each wake
  (`timeBase=monotonic`). One clock serves the pump, the idle deadlines, the step hooks' stamps and the
  present clock, which `hostNowSeconds()` and `stepIntervalSeconds()` give to any thread (§12). The
  engine's pause, slow motion and frame-time clamp act on frame deltas and do not reach this pump; a runner
  stall is caught up to the cap and the rest is counted as lost (§10).

**`beforePump(hostNowSeconds, rate)`** is the one addition made to the transcribed frame when it was
transcribed (`sampleInput`, below, came later). The game is called on
the owning thread immediately before `pump()`, once per frame (in `FreeRunning` once per wake, a wake that
runs no step included), with the host time the pump is about to receive and a `TickRateDoor` over the
scheduler. A rate change made through the door therefore applies to this frame's pump; called after the
pump, it would apply only from the next frame's. Nothing changes the rate through it yet. With
`tickOffset()` and the timing samples in the scheduler's time base, it is the seam a time-dilation
controller plugs into.

**`sampleInput()`** is the input-sample hook of a game that models `TickDriverSampleGame`
(`TickDriverConcepts-rationale.md` §9). In `Inline` and `FrameBounded` it is the first thing `startFrame` does
after the `FreeRunning` return: once per `startFrame`, on the owning thread, before the frame's start time
is read and so before `beforePump`, the pump and the frame's batch, in a frame with no due step too.
`endFrame` never calls it. `startFrame` calls the private `sampleFrameInput()`, whose whole body is an
`if constexpr` on the concept, so a game without the hook compiles neither the call nor its check and its
trace is unchanged; for a game with it, the trace is the same with the `sampleInput` events taken out.

* **Why first.** It makes "sample, then pump, in the same frame" the driver's order rather than the host's:
  the game publishes the frame's sample here, and `beforePump`, the pump and the batch this frame runs or
  dispatches all come after it. Before the hook, a host held that order only by running its own sampling in
  an earlier part of the engine's frame than the driver's start of physics.
* **Not in the frame's timing.** The frame's start time is read after the sample, so the frame period and
  `noteStartPhysics`'s duration do not include it.
* **Never in `FreeRunning`.** `startFrame` returns before the call, and neither `runAuthorityLoop` nor
  `pumpAndRunSteps` calls it: a free-running authority has no local input to sample, and `pumpAndRunSteps` is
  shared with the runner, so a call moved into it would run on the runner at every wake. `sampleFrameInput`
  `OG_CHECK`s the mode before it calls the game, so the check goes wherever the call is moved. The order is
  pinned by the tests (§9).

**The cap.** `requestedMaxCatchUpSteps` is clamped to `[1, kMaxStepsPerFrame]` (240, the size of the
batch's deadline array). When the clamp changed it, the driver logs the Warning
`[SimHost.Frame] og.Sim.MaxCatchUpSteps=<requested> is outside [1, 240]; using <cap>` before the
`[SimHost.Frame]` line (both lines share one formatter buffer in the window, so the order also keeps the
first line intact until it is logged). The scheduler drops the rest of a longer frame and counts it as lost
time.

**The rate bounds** are `Config::minRateScale` and `maxRateScale`, 0.75 and 1.25 by default. The
`[SimHost.Frame]` line prints the literal `rateScale=[0.75,1.25]`, so a configuration with other bounds
prints the defaults.

**The idle slack** is `Config::idleSlackSeconds`, 0 by default, read only in `FreeRunning` (§11). The
constructor checks it in every mode, so a bad value fails where it is configured. The frame clamp is
checked only under `Monotonic`, the one time base that reads it.

## §7 Lines

Every line goes through `engine.log(HostLogLevel::Warning, …)` with the text formatted into a buffer the
window reserved at construction (`TickDriverWindow-rationale.md`):

* `[SimHost.Frame] og.Sim.MaxCatchUpSteps=… is outside [1, 240]; using …`, only when the cap was clamped;
* `[SimHost.Frame] role=<Authority|Client> stepping=<inline|worker> dt=<s> maxCatchUpSteps=<n> rateScale=[0.75,1.25] timeBase=<worldDelta|monotonic>`,
  once, at the end of construction, with `worldDelta` under `HostDelta` and `monotonic` under `Monotonic`,
  followed by ` frameWait=thisFramesSingleStep` only under that frame wait (§4); in `FreeRunning`,
  `[SimHost.Frame] role=Authority stepping=runner dt=<s> maxCatchUpSteps=<n> rateScale=[0.75,1.25] timeBase=monotonic idleSlackUs=<n>`;
* `[SimHost.FirstStep] role=… thread=<game|worker|runner> physicsStep=<n> batch=<n>`, once, from the thread that
  runs the first batch;
* `[SimHost.Window] …`, once per due window (`tickDriverWindow::kWindowSeconds`, by the engine clock), at
  the end of the post-physics pass that finds it due, or in `FreeRunning` after the wake that finds it due,
  on the runner. The totals (the scheduler's `lostTime()` and `ignoredTimeSamples()`, J1's and J6's
  `drops()`) are read once and passed to both `formatWindowLine` and `restart`.

`stepping=` reads `inline` for `Inline`, `worker` for `FrameBounded` and `runner` for `FreeRunning`. The
window builds every word and, in `FreeRunning` only, the window line's suffix (`lateUs`, `activityWakes`,
`earlyWakes`, `idleFallbacks`, `approachUs`) and the re-timed meaning of its fields
(`TickDriverWindow-rationale.md` §8). The driver passes the slack, the time base and the frame wait to
`formatFrameLine` in every mode; the `Inline` and `FrameBounded` lines ignore the slack, and under `HostDelta`
with the default frame wait they stay byte-identical. The `FreeRunning` line ignores the time-base and the
frame-wait flags.

## §8 What the compiler and the checks hold

* **Compile time:** each template parameter is constrained by its concept; the hooks satisfy `StepHooks`
  and the doors `DriverStepDoors` (both checked where the types are named); the member order is two
  `static_assert`s (§2); `runAuthorityLoop` takes a `TickDriverRunnerHost` and requires
  `TickDriverIdleGame<GameT>`, so a game without `idleUntil` instantiates every other member and cannot
  call the loop; `sampleFrameInput` calls `sampleInput` under `if constexpr (TickDriverSampleGame<GameT>)`, so
  a game without it compiles no call (§6); `isFiniteSeconds` is pinned by a third `static_assert` (§3).
* **`OG_CHECK`s** (each message names what it replaced, where it replaced one): the seven construction
  checks (§3); `reanchor()` on a `HostDelta` driver (§6); a full J1;
  `endFrame` and `stop()` with the world lock held (G-05); an inline step off the owning thread; the
  occupancy door outside a step; the scheduler and the host time read off the owning thread; the scheduler
  reached before it was built; and a driver destroyed while a step task is still pending (call `stop()`
  first). For `FreeRunning`: the loop's three entry checks (the mode, the owner thread, not already
  running) and its origin read below zero; `stop()` and the destructor while the loop runs; the
  frame-thread pass run on the runner; the occupancy push and the world-lock note off the runner; any
  shadow restore; and, for a sampling game, an input sample taken in `FreeRunning` (§6).
* **Allocation.** The driver's own state grows no container on the step path or the frame path: the rings
  are fixed, the batch is an array, the window's sample lists and line buffers are reserved at
  construction, and the loop's and the idle's state is locals and atomics. What its collaborators do is
  theirs and unchanged by the move: the scheduler keeps a deadline history of at most 64 segments, the step
  driver a client's occupancy timeline, and the executor stores the dispatched task (a lambda holding the
  batch) in whatever way the engine's executor does.

## §9 Tests

The `[TickDriver]` cases in og-simulation-tests run the driver and a transcription of the engine-hosted
frame host (the old frame host's code with its engine types swapped for the fake kit's executor, clock and
log, and with the same `beforePump` addition) on the same scripted frames, each over its own copy of the
fakes, and compare one ordered event trace of each: every game hook, presenter and world-host door, world
and manager call, clock read, executor dispatch, wait and deferral, task run, and log line.

* `SimulationTickDriver.Inline.AnAuthoritySessionMatchesTheFrameHostTranscription`: 600 jittered frames
  at 60 Hz with frames of 0, 1 and 2 steps and one 250 ms hitch (15 steps, none lost).
* `SimulationTickDriver.Inline.AClientSteppingInlineMatchesTheFrameHostTranscription`.
* `SimulationTickDriver.FrameBounded.AClientSessionMatchesTheFrameHostTranscription`: block modes 0 and 1,
  tasks completing before and after `endFrame`, a frame with no completion event, a refused deferral, and
  client resimulations.
* `SimulationTickDriver.CatchUpCap.AHitchBeyondACapOfThreeLosesStepsAsTheTranscriptionDoes`.
* `SimulationTickDriver.CatchUpCap.ZeroAndNineHundredNinetyNineAreClampedWithTheWarning`.
* `SimulationTickDriver.Stop.WaitsForBothOutstandingTasksAndNothingRunsAfterwards`.
* `SimulationTickDriver.TwoDrivers.AnAuthorityAndAClientInOneProcessMatchTheTranscription`: both role bits
  on one thread.
* `SimulationTickDriver.BeforePump.RunsOncePerFrameBeforeThePumpWithTheHostTime`.
* `SimulationTickDriver.BeforePump.ARateScaleThroughTheDoorChangesTheStepCountsAsTheTranscriptionDoes`: the
  step counts also equal a bare `SimulationScheduler` given the same host times with the rate set before
  the same frame's pump, and differ from one with the rate set a frame later.

The `sampleInput` cases are in `SimulationTickDriverSampleTest.cpp`, over the fake kit's sampling games
(`TickDriverFakes-rationale.md` §1) and its world host. The first two run 90 frames of 0, 1 or 2 steps over
an engine that records each clock read, once as an `Inline` authority and once as a `FrameBounded` client:

* `SimulationTickDriver.SampleInput.RunsOncePerStartFrameFirstAndBeforeBeforePump`: each `startFrame`'s first
  event is `sampleInput`, it has one `sampleInput` and one `beforePump`, the sample comes first, and no
  `endFrame` samples. With the call moved after `beforePump` the first event is the frame's clock read.
* `SimulationTickDriver.SampleInput.AGameThatSamplesHasTheSameTraceWithTheSamplesRemoved`: against the same
  frames with the kit's plain recording game.
* `SimulationTickDriver.SampleInput.AFreeRunningAuthorityNeverSamples`: a sampling idle game whose loop runs
  60 steps on the test thread, then 30 `startFrame` and `endFrame` pairs on a second thread, which add 30
  `onPostPhysics` events and nothing else; no `sampleInput` at all. With the call moved into
  `pumpAndRunSteps`, before or after `beforePump`, the runner's first wake stops on `sampleFrameInput`'s
  check.

The time-base cases are in `SimulationTickDriverTimeBaseTest.cpp`, over the fake kit's plain recording game
and world host and an engine that records each clock read. Each runs as an `Inline` authority and as a
`FrameBounded` client, at 60 Hz with the cap at 240; every frame advances the scripted clock by its delta
and then calls `startFrame` and `endFrame`, and a `Monotonic` driver is handed a frame delta of 99 s, which
it must ignore. Two `static_assert`s pin the defaults, `HostDelta` and a 0.4 s clamp.

* `SimulationTickDriver.TimeBase.MonotonicGivesTheHostDeltaStepsTraceAndLinesForTheSameDeltas`: 900 frames
  of 0, 1 or 2 steps with one 0.25 s frame (15 steps, under the clamp). The per-frame step counts, the event
  trace, the host times handed to `beforePump`, the step stamps and the published deadlines are equal, and
  so are the log lines except the frame line, which reads `timeBase=monotonic` against `timeBase=worldDelta`.
* `SimulationTickDriver.TimeBase.AOneSecondGapAdvancesTheClampAndCountsTheRestAsReanchorLoss`: after 120
  frames and a half-step frame, which leaves the next deadline half a step ahead so that no count sits on a
  rounding boundary, a 1 s frame. The `Monotonic` driver runs 24 steps, as many as a `HostDelta` driver
  handed the frame clamped to 0.4 s, and counts 0.6 s and 35 steps as `reanchor` loss (§6), none as catch-up;
  a `HostDelta` driver handed the whole second runs 60. Its host time advances the whole second. The 120
  frames after it step as the clamped `HostDelta` driver's do. With the clamp dropped, the gap frame runs 60
  steps.
* `SimulationTickDriver.TimeBase.AReanchorOnResumeSkipsThePauseAndCountsItAsReanchorLoss`: after 60 frames
  and a half-step frame, 5 s pass with no frame. With `reanchor()` the host time is the clock's, and 5 s less
  the half step, 299 steps, is `reanchor` loss; frames of 0.4 steps then run 1, 0 and 1 steps with no
  further loss. A second driver that is not re-anchored runs the clamp's 24 steps in its first frame and
  counts the rest, 4.6 s plus that frame, 276 steps.

The `FrameWait` cases (`SimulationTickDriverFrameWaitTest.cpp`) run a `FrameBounded` client over the fake
engine with no worker thread, for 120 frames of 0, 1 and 2 steps. A frame's task runs only after `endFrame`
returns (`runAllTasks`, then `runDeferred`), as a worker step that ends after the frame thread reached the
end of physics but inside the frame does. Each case runs once with a completion event (the pass is deferred)
and once without (a blocking wait). The game records, at each `onPostPhysics`, how many steps the presenter
has had published.

* `SimulationTickDriver.FrameWait.AStepPublishedMidFrameIsShownByThatFramesPostPhysics`: under
  `ThisFramesSingleStep`, a frame of 0 or 1 steps shows every step dispatched so far; a 2-step frame shows
  all but its own two (the bound); the frame line carries ` frameWait=thisFramesSingleStep`. With
  `endFrame`'s wait chosen as before the frame wait, every 1-step frame shows one step fewer.
* `SimulationTickDriver.FrameWait.TheDefaultShowsAFramesStepsInTheNextFrameAsBlockModeZeroDid`: under
  `EarlierFrames`, every frame shows all but its own steps, and the frame line has no frame-wait word.
* `SimulationTickDriver.FrameWait.TheSettingAddsNoEventToTheTraceAndOnlyMovesThePostPhysicsPass`: the two
  settings' traces hold the same events in the same numbers, in a different order, with the same per-frame
  step counts and the same published steps.

The `DisplayFill` cases (`SimulationTickDriverDisplayFillTest.cpp`) run a `FrameBounded` client over the fake
engine's worker thread, for 60 frames of 0, 1 and 2 steps, under each frame wait. The test's game counts the steps in
flight (from `receiveForStep` to the end of `onStepEnded`) and records each fill. In every frame whose batch the
driver does not wait for, it holds the batch's first step inside `onStepEnded` until that frame's post-physics pass
has seen it held and run, so whatever the pass does overlaps a step that is mid-flight.

* `SimulationTickDriver.DisplayFill.TheAfterBatchFillNeverRunsBesideAStep`: the game fills in `publishForDisplay`.
  There is one fill per batch, each on the worker, each after its batch's last step and none beside a step; under
  `ThisFramesSingleStep` a one-step frame's pass sees its own batch's fill. On the driver without the after-batch
  call there is no fill; with the game filling in `onPostPhysics` instead, 52 of 60 frame-locked fills and 26
  immediate ones ran beside a step (og-input-latency task P5-36's seen-failing runs).
* `SimulationTickDriver.DisplayFill.AFillAtPostPhysicsRunsBesideAStepTheDriverDoesNotWaitFor`: the harness's
  control. A game that fills in `onPostPhysics` fills beside a step in exactly the frames whose batch the driver
  does not wait for, so the first case's zero is not the harness failing to see an overlap.

The `FreeRunning` cases run the loop on the test thread in virtual time (the fake engine's clock, which
only the idle scripts and the fake runner host advance), unless they say otherwise. The fake kit's idle
game and runner hosts are described in the test kit's own rationale.

* `SimulationTickDriver.FreeRunning.WokenAtTheInlineFrameTimesItGivesTheInlineAuthoritysStepTrace`: the
  600 jittered deltas of the Inline authority case, accumulated into wake times. The Inline arm runs them
  after a leading `startFrame(0.0)`, which matches the loop's first pump at 0. With the clock reads, the log
  lines, the idle and sleep calls and the post-physics hook filtered out, the two traces are equal event for
  event, and so are the lost time and the host times handed to `beforePump`.
* `SimulationTickDriver.FreeRunning.ADeadlineHonouringIdleRunsOneStepPerWakeAtItsDeadline`: 600 steps of
  one per wake, each idle's deadline is the origin plus the next step's deadline, and the window line that
  closes during the run (the driver is built at −1 s) reads `maxStepsPerFrame=1`, `lateUs=0.0/0.0/0.0` and
  as many frames as steps.
* `SimulationTickDriver.FreeRunning.ActivityWakesRePumpWithoutStepping`: activity every 3 ms; 600 steps,
  none before its deadline, `activityWakes` equal to the activity returns, and one `beforePump` per idle
  return.
* `SimulationTickDriver.FreeRunning.TheSlackIdlesToTheApproachThenPollsToTheDeadline`: a 0.5 ms slack; each
  idle starts at the deadline minus the slack and polls with deadlines at now until the deadline, no step is
  early or more than one 50 µs pass late, and `approachUs` and `lateUs` have 600 samples.
* `SimulationTickDriver.FreeRunning.AnUnavailableIdleSleepsOnTheRunnerHost`: one hook call and one runner
  sleep to the same deadline per step, and `idleFallbacks` equal to the steps.
* `SimulationTickDriver.FreeRunning.ThreeEarlyDeadlineReturnsFallBackToTheRunnerSleep`: three hook calls and
  one sleep between steps, with `earlyWakes` and `idleFallbacks` counted.
* `SimulationTickDriver.FreeRunning.AStalledRunnerCatchesUpToTheCapAndCountsTheRest`: a 2 s stall at cap 60
  runs one batch of 60; the trace, the lost steps and the lost seconds equal the frame-host transcription's
  for a 2 s frame.
* `SimulationTickDriver.FreeRunning.AFrameThreadStallLeavesTheRunnersStepsOnePerWake`: `endFrame` runs on a
  second thread at each wake except for 200 ms; the runner's `maxStepsPerFrame` stays 1, while an Inline
  driver whose frame thread stalls for the same 200 ms runs at least 12 steps in one frame.
* `SimulationTickDriver.FreeRunning.TheFrameThreadPassRunsOnlyOnPostPhysics`: the driver is built on a
  runner thread; `startFrame` and `endFrame` from the test thread run one `onPostPhysics` per `endFrame`
  and nothing else: no pump, no wait, no window sample, no line.
* `SimulationTickDriver.FreeRunning.ThePresentClockIsReadableOffTheRunner`: from a second thread, 0 and
  `dt` before the loop, the engine clock minus the origin at each wake, and `dt / 1.25` from the wake after
  the game set that rate.
* `SimulationTickDriver.FreeRunning.StopFromTheFrameThreadEndsTheLoopAndStopChecksIt`: real threads and
  the real clock; the test thread waits for 5 steps, sets the stop flag, joins the runner and calls
  `stop()`; no step runs after the join, and the first-step line reads `thread=runner`.
* Hidden, run by hand: `SimulationTickDriver.FreeRunning.StopWhileTheLoopIsActiveIsRefused` (tag
  `[TickDriverStopBeforeJoin]`) calls `stop()` from inside the loop and is expected to stop on its check;
  `SimulationTickDriver.FreeRunning.Timing.LoopOverheadWithASpinPreciseIdle` (tag `[OGSimTiming]`) runs 601
  steps on the real clock with a spinning idle and bounds the window's `lateUs` p99 at 100 µs. It is a
  wall-clock bound, so it is never in the default run.

## §10 The authority loop

`runAuthorityLoop(runner, stopRequested)` is the whole frame of a `FreeRunning` driver. It runs on the
runner, the thread that built the world host and the driver (§13):

1. Three `OG_CHECK`s: the driver is `FreeRunning`; this thread is the role's owner; the loop is not already
   running. The origin is read from `engine.nowSeconds()` and checked to be at least 0, because the present
   clock reads a negative origin as "not started" (§12). Then `m_loopActive` is set and the origin is
   published, both with release order.
2. Until `stopRequested` reads true (acquire), each wake:
   1. `t = engine.nowSeconds() − origin`, written to `m_hostTimeSeconds`;
   2. `pumpAndRunSteps()`, the `Inline` branch unchanged: `beforePump(t, rate)`, J8, `pump(t)`, the batch,
      one `noteLateness(t − deadline)` per step, `onFrameStepsDue`, `runBatch`, `afterInlineBatch`;
   3. a wake that ran steps notes the period since the previous stepping wake (`noteFramePeriod`, from the
      second one on) and `noteStartPhysics(steps, the pump and batch's duration)`; a wake without steps
      notes neither, so `frames` counts stepping wakes;
   4. J6 is drained into the window, and when the window is due by the engine clock it is logged and
      restarted, with the same `logWindow` the post-physics pass uses;
   5. the idle to the next deadline (§11).
3. After the loop: `m_running = false`, then `m_loopActive = false` with release order.

**Relative time.** Every comparison the loop and the idle make is in relative time: the engine clock minus
the origin, against the scheduler's deadlines, which start at 0. The absolute `origin + deadline` is formed
only as the idle hook's argument. Comparing absolute times would let a rounding residue (`origin + d` read
back as one step below `d` after the subtraction) leave a wake at the deadline that pumps nothing; compared
relatively, such a residue costs at most another pass of the idle.

**Lateness.** Each step of a batch is noted as the pump time minus the step's deadline, in microseconds
(`noteLateness`). It is never negative: `pump` runs only steps whose deadline is at or before the time it
is given, so a wake at a deadline gives 0 and a late wake gives how late it was.

**The first wake** pumps at t = 0, which runs step 0 (its deadline is 0). An Inline frame at host time 0
does the same, which is why the equivalence case gives its Inline arm a leading `startFrame(0.0)`.

**A runner stall** (the runner, not the frame thread) is caught up to the cap, `og.Sim.MaxCatchUpSteps`
clamped to `[1, 240]` as in every mode, and the rest is counted as catch-up loss. No frame-time clamp
hides it.

**Per-frame hooks are per wake.** `onFrameStepsDue` and `afterInlineBatch` run once per stepping wake, on
the runner, and `beforePump` once per wake. A game's per-frame sample point is a wake in this mode, and
its hooks run on the runner, not on the frame thread. `sampleInput` has no wake counterpart: the loop never
calls it (§6).

## §11 The idle

`idleUntilNextDeadline` waits for the scheduler's next deadline (`nextDeadline()`, relative), with
`approach = target − idleSlackSeconds`:

1. **To the approach.** Call `idleUntil(origin + approach)`, then by its answer:
   * `Activity`: note `activityWakes` and return to the loop, which wakes again: `beforePump` runs and no
     step is due yet. The run of early returns ends with it.
   * `Unavailable`: `runner.sleepUntil(origin + approach)`, note `idleFallbacks`, go to the poll.
   * `Deadline` at or after the approach: go to the poll.
   * `Deadline` before the approach: note `earlyWakes`. The third in a row
     (`simulationTickDriver::kEarlyWakesBeforeFallback`) sleeps through the runner host to the approach and
     notes `idleFallbacks` (`⛔G-07`); the first two call the hook again.
2. **The poll.** Until the relative clock reaches the target, or `stopRequested` reads true: call
   `idleUntil(now)`, with the absolute clock just read, which the hook contract makes one non-blocking pass
   (`TickDriverConcepts-rationale.md` §8). A poll's answer is not counted: a poll is not a wait, and the
   step that is due when the poll ends runs `beforePump` anyway.
3. With a slack above 0, `noteApproach` gets the time from the end of step 1 to the end of the poll. Every
   idle that reaches the poll gives one sample, the last one too when a stop cuts its poll short.

**Why the bound.** The driver never relies on the hook to wait. A hook that returns at once (a failing
socket reported as `Unavailable`, a timer that fires early and reports its deadline) would otherwise make
the runner a busy loop. `Unavailable` sleeps at once; early `Deadline`s get two retries, then the sleep.
`Activity` costs one pump and no step: work was done, so it is not a spin.

**The slack.** With slack 0, the default, step 1 ends at the deadline itself, so the poll makes no call and
nothing spins. A slack lets a host whose timer wakes late wake early instead and poll the rest; each poll
is one pass of the hook, so inbound input is still handled as it arrives. The constructor keeps it finite
and below one step.

**Stop latency.** `stopRequested` is read before each wake and on each poll, not during step 1, so a stop
takes effect within one idle: at most one step interval plus the slack after it is set, with a hook that
honours its deadline.

## §12 Doors off the runner

In `FreeRunning` the runner owns the scheduler, the batch, J6's draining side and the window. The frame
thread keeps queues and snapshots only. The doors, by thread:

| member | thread in `FreeRunning` | rule |
|---|---|---|
| constructor, `runAuthorityLoop` | runner | the constructor's threading check; the loop's owner check |
| `pushBodySlotOccupancy`, `noteWorldLockWait` | runner | `OG_CHECK` the owner thread: J1 has one producer, and the window is the runner's |
| `noteShadowRestore` | none | refused by an `OG_CHECK`: a `FreeRunning` authority has no shadow world |
| `hostNowSeconds()` | any | `engine.nowSeconds()` minus the origin, loaded with acquire order; 0 before the loop starts (origin `-1.0`) |
| `stepIntervalSeconds()` | any | `dt` over J8, loaded with acquire order; `dt` before the first wake |
| `tickOffset()`, `savesTicks()` | any | J3 is atomic; `savesTicks()` is fixed at construction |
| `startFrame` | frame thread | returns at once |
| `endFrame` | frame thread | `OG_CHECK` not the runner, then `onPostPhysics()` only (`⛔G-08`) |
| `stop()`, destructor | frame thread, after the runner is joined | `OG_CHECK` that the loop has returned (§13) |
| `getDiagnostics()` | runner, or after the join | the window and the scheduler have no synchronization |

The two any-thread doors are the present clock: what a presenter on the frame thread reads to build its
render clock. `hostNowSeconds()` reads the engine clock at the call, not at the last wake, so the engine's
`nowSeconds()` must be safe to call from any thread in this mode. The origin is written once, before the
first wake; J8 once per wake. Inline and `FrameBounded` keep their owning-thread doors, unchanged.

## §13 The driver's part of start and stop

**Start.** The host builds the world host on the runner (its `configure` marks the runner as the owner),
constructs the driver there (the constructor takes the world lock, builds the step driver and logs the
`[SimHost.Frame]` line), and calls `runAuthorityLoop` on the same thread. The driver neither starts nor owns
the thread: the host chooses it, its priority, and the runner host's sleep.

**Stop.** The frame thread stops calling `endFrame`, sets the stop flag and joins the runner; the loop
returns at its next check (§11). Then the frame thread calls `stop()`, which `OG_CHECK`s that the loop is no
longer active, clears `m_running` and waits for the handles, which `FreeRunning` never sets. The
destructor makes the same check. The join orders the runner's last writes (`m_running`, the window, the
scheduler) before the frame thread's reads, so the diagnostics are readable after it.

**Which thread destroys.** The driver and the world host are destroyed on the frame thread after the join,
or on the runner, never on a thread started later (`TickDriverThreadContext-rationale.md` §8).

## §14 Provenance

Transcribed from the engine-hosted frame host of the first game (its start of play, its start-of-physics
and pump functions, its batch runner, its end-of-physics function, its post-physics pass and step-cost
drain, its window start and log, its outstanding-step wait, its occupancy push and drain, and its
owning-thread accessors) and from the per-step body and the step-driver construction of the engine-hosted
Jolt host. The mapping:

* the engine's tick functions → the engine adapter's two calls to `startFrame` and `endFrame`;
* the per-frame probe call → `onFrameStepsDue`; the reception coordinator's reap → `afterInlineBatch`; the
  post-physics delegate → `onPostPhysics`;
* the platform clock → `engine.nowSeconds()`; the task graph → `dispatchAfter`, `isPending`, `waitFor` and
  `deferUntil`; the console variables → `Config::requestedMaxCatchUpSteps` and `endFrame`'s `blockMode`;
* the engine's log macro → `engine.log` with the window's formatters; the engine's game-thread query →
  the thread context's `isOwnerThreadForRole()`;
* the weak owner pointer of the deferred continuation → the captured driver and `m_running` (the engine
  executor adds an owner check of its own);
* the Jolt host's contact-line call after `runTick` → `worldHost.afterRunTick_Step`.

The guards G-01 to G-06 are the old frame host's six, with fresh ids (`SimulationTickDriver-guards.md`,
"Provenance of the ids").

**2026-11-12, `FreeRunning`.** The authority loop, the idle, the frame-thread pass and the off-runner doors
(§10 to §13) were added for a server authority that steps on a runner thread of its own, woken at the step
deadlines on the engine clock. `FreeRunning` had been declared and refused by the constructor until then.
G-01 and G-04 were restated per mode; G-07 and G-08 are new. The default modes' lines and traces are
unchanged.

**2026-11-18, `sampleInput`.** The optional input-sample hook (§6) was added, called first in `startFrame` in
`Inline` and `FrameBounded`, so that "sample, then pump, in the same frame" is the driver's rule for every
host. No guard was added: the `FreeRunning` exclusion is an `OG_CHECK` and the order is pinned by the tests
(§9). A game without the hook compiles exactly the code it compiled before, so every existing trace, line and
golden is unchanged.

**2026-11-19, the `Monotonic` time base.** `Config::frameTimeBase` (`HostDelta`, the default, or `Monotonic`),
`Config::monotonicFrameClampSeconds`, `reanchor()` and `Diagnostics::lostTimeByCause()` were added (§6), for a
host whose engine delta does not carry a pause and a frame clamp the way the first engine's does. G-04 was
restated per time base; G-09 is new. `begin()` reads the clock once for the window and the origin, where it read
it once for the window, so under `HostDelta` every trace, line and golden is unchanged.

**2026-11-20, the frame wait.** `Config::frameWait` (`EarlierFrames`, the default, or `ThisFramesSingleStep`),
`m_frameSteps` and `waitsForThisFrame` were added (§4), for a client host whose post-physics pass is what it
draws, and which measured each step shown one frame period late under block mode 0 when the step ended just
after the pass. G-10 is new. With the default every trace, line and golden is unchanged.

**2026-11-20, the after-batch publish.** `publishAfterBatch` (§3) was added for a client game that models
`TickDriverDisplayGame`, so that a host's display copy is taken where nothing steps. The host that needed it filled its
display snapshot in `onPostPhysics`, which `EarlierFrames`, and `ThisFramesSingleStep` in a frame of two or more steps,
run while the frame's own batch may be stepping. No guard was added: the step-context check is an `OG_CHECK`, the
one-per-batch placement is pinned by the tests (§9), and a game without the hook compiles exactly the code it compiled
before, so every existing trace, line and golden is unchanged.
