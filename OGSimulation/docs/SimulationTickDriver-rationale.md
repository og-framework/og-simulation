<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationTickDriver.h` — rationale

The narrative behind `BodySlotOccupancyCommand`, the `simulationTickDriver` constants and `SimulationTickDriver`,
the engine- and game-agnostic frame pipeline that pumps the scheduler, runs or dispatches each frame's
batch of steps, waits for it at the end of the frame, and closes the host's telemetry window. The header
keeps its licence, its docs pointer, its code, its `OG_CHECK`s, two member-order `static_assert`s and six
guard tags; everything that explains it lives here. Prohibitions: `SimulationTickDriver-guards.md`.

**If this file and `SimulationTickDriver.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL and glm only, through og-simulation's own headers. Everything below is true to a reader
with no game engine and no other repository.

---

## §1 What it is

`SimulationTickDriver<GameT, WorldHostT, PresenterT, EngineT>` is a class template over four
collaborators, each constrained by its concept from `TickDriverConcepts.h` (`TickDriverGame`,
`TickDriverWorldHost`, `TickDriverPresenterSteps`, `TickDriverEngine`). An engine adapter calls two entry
points per frame on the owning thread:

* `startFrame(frameDeltaSeconds)` at the start of the engine's physics phase: it times the frame, pumps the
  scheduler with the accumulated frame delta, and either runs the frame's steps on this thread (`Inline`)
  or dispatches them as one executor task chained after the previous frame's (`FrameBounded`);
* `endFrame(completion, blockMode)` at the end of the physics phase: it waits for the step task the block
  mode names (by holding the engine's completion event open with `deferUntil`, or by a blocking wait when
  there is no event), then runs the post-physics pass.

`stop()` ends the session: it clears the running flag, then waits for every outstanding step task. The
driver owns the scheduler, the window and the mailboxes (§5); it borrows the game, the world host, the
presenter's step doors and the engine. It is header-only and adds no indirect call: every collaborator is a
template parameter and every call on it is direct.

An engine host instantiates it with its own game binding, world host, presenter and engine. Its behaviour is a
transcription of the engine-hosted frame host and of the per-step body of the engine-hosted Jolt host (§10), with one
intended addition, the `beforePump` seam (§6).

## §2 Ownership and member order

The driver holds raw pointers to its four borrowed collaborators, then, in this order:

| member | what |
|---|---|
| `m_config` | the configuration, `const` |
| `m_running`, `m_loggedFirstStep`, `m_hostTimeSeconds`, `m_lastFrameStartSeconds` | the running flag, the first-step flag, the host time and the previous frame's start |
| `m_scheduler` | `std::optional<SimulationScheduler>`, built in the constructor after the step driver |
| `m_window`, `m_firstStepLine` | the telemetry window and the first-step line (`TickDriverWindow.h`) |
| `m_bodySlotOccupancyCommands` | J1 |
| `m_stepCosts` | J6 |
| `m_tickOffset` | J3 |
| `m_pendingSteps`, `m_blockingSteps` | the executor handles (§4) |
| `m_doors` | the driver's part of the step doors |
| `m_hooks` | `std::optional<TickDriverStepHooks<…>>` |
| `m_stepDriver` | `std::optional<SimulationStepDriver<…>>`, last |

Members destruct in reverse declaration order, so the step driver goes first: it holds the hooks by
reference and, on a client, unregisters its resync callback from the game's manager in its destructor. The
hooks hold the doors, and the doors reach J1, J3, J6 and the step driver. Two `static_assert`s on
`__builtin_offsetof` pin the order: config < scheduler < window < J1 < J6 < J3 < the pending handle < the
blocking handle, and the blocking handle < doors < hooks < step driver.

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

1. Four `OG_CHECK`s: not `FreeRunning`; the authority is `Inline`; the configured role equals the world
   host's thread-context role; the configured threading equals the world host's stepping mode.
2. Under `worldHost.lockForStep()` (the world lock, J2), build the hooks and then the step driver
   (`StepDriverConfig{ dt, ResimPolicy::OnRequest, 0 }`). The step driver parks every slot in its
   constructor, a world write, so it is built under J2.
3. `begin()`: the cap (§6), the cap Warning when it was clamped, the scheduler (`dt`, the cap, the rate
   bounds, start time 0), the window restarted at `nowSeconds()` with the current totals, the running flag,
   and the `[SimHost.Frame]` line.

**`startFrame(frameDeltaSeconds)`**, owning thread:

1. Take the frame's start time; when there was a previous frame (`m_lastFrameStartSeconds > 0.0`), note
   the period. The window is handed the raw `double` difference.
2. `pumpAndRunSteps`:
   1. `m_blockingSteps = m_pendingSteps`: the task dispatched in earlier frames, captured before this frame's
      dispatch, every frame, with steps or not.
   2. `m_hostTimeSeconds += frameDeltaSeconds`; `beforePump(m_hostTimeSeconds, rate)`; `pump(m_hostTimeSeconds)`
      (`⛔G-04`). No step due: return 0.
   3. The batch (J4) by value: `first = physicsStepCount() − numSteps`, the count, each step's deadline.
   4. `firstUpcomingSimTick = first + J3` (`⛔G-03`, §5).
   5. `onFrameStepsDue(firstUpcomingSimTick, numSteps)` (`⛔G-02`).
   6. `Inline` (`⛔G-01`): `runBatch` here, then `afterInlineBatch(firstUpcomingSimTick)` once (`⛔G-06`).
      `FrameBounded`: `dispatchAfter(previous, task)`, where `previous` is the pending task when it is still
      pending and a default handle otherwise; the result becomes `m_pendingSteps`.
3. Note the frame, its step count and its own duration (`noteStartPhysics`).

**`runBatch`** logs the `[SimHost.FirstStep]` line once (its `thread=` word is `game` when the thread is
the role's owner thread, `worker` otherwise), then runs `runStep(first + i, deadline[i])` in order. Each step:

1. `OG_CHECK` that an `Inline` step runs on the owning thread;
2. `worldHost.lockForStep()` (J2 for the whole step), then a `StepScope` on the world host's thread context
   (the step-running bit the hooks and the access predicates test);
3. `hooks.currentStepDeadline = deadline` (the presenter copies it into the step's render snapshot);
4. `m_stepDriver->runTick(physicsStep)`;
5. `worldHost.afterRunTick_Step(step, outcome)`, still under the lock and the scope.

`runStep` is private and `runBatch` is its only caller, so "a step runs only inside the driver's batch" is
the type's shape; the old per-step body checked it at run time with a thread-local batch marker.

**`endFrame(completion, blockMode)`**, owning thread:

1. `OG_CHECK` that this thread does not hold J2 (G-05).
2. The task to wait for: `m_blockingSteps`, or `m_pendingSteps` under block mode 1.
3. Nothing to wait for, or it is complete: run the post-physics pass at once, with no wait sample.
4. A completion event that accepts the deferral (`deferUntil` returns true): return; the continuation runs
   the pass after the task. It captures the driver and returns at once when `m_running` is false.
5. Otherwise (no event, or `deferUntil` returns false): `waitFor` the task, then run the pass.

**The post-physics pass:** note the end-of-frame wait when there was one; `onPostPhysics()`; note its
duration; drain J6 into the window; when the window is due, format the `[SimHost.Window]` line, log it,
and restart the window with the same `now` and the same totals.

**`stop()`:** `m_running = false` first, so a continuation that is still deferred does nothing when it runs;
then wait (G-05's `OG_CHECK`) for the pending and then the blocking task, each one that is set, and reset
both. No step runs after `stop()` returns. The engine adapter stops calling the two entry points (the old
host unregistered its tick functions in the same statement that cleared the flag).

## §4 Threading modes

| Mode | Who pumps | Who runs the batch | `endFrame` |
|---|---|---|---|
| `Inline` | the owning thread, in `startFrame` | the same thread, then `afterInlineBatch` | never waits: the handles stay unset |
| `FrameBounded` | the owning thread, in `startFrame` | an executor task chained after the previous frame's | waits for earlier frames' task (block mode 0) or also this frame's (1), by deferral or a blocking wait |
| `FreeRunning` | — | — | refused by an `OG_CHECK` in the constructor; declared for the free-running runner that will own the pump |

Block mode 0 means the end of a frame waits only for steps dispatched in earlier frames; this frame's task
may still be stepping while the post-physics pass runs. Block mode 1 (debugging) also waits for this frame's.

The executor handles are `std::optional<EngineT::Handle>`. An unset optional is the old code's invalid
event: `stop()` waits for each handle that is set, even one that has completed (a no-op wait), exactly as the
old wait passed every valid event to one wait call. "Pending" is `has_value()` and `engine.isPending`.

**The owning thread** is the thread that configured the world host's thread context: the context's
`isOwnerThreadForRole()` replaces the engine's game-thread query everywhere in the driver (the scheduler
accessor, the host time, the first-step line's thread word, the inline-step check, and the doors' owner test
that the authority's `beforeTick` checks).

**One copy per shared library.** The thread context's masks and counts are header thread-locals
(`TickDriverThreadContext-rationale.md` §7). The driver's `StepScope` and the world host's `configure` must be
compiled into the same shared library, which holds as long as both are header templates instantiated by
the same module.

## §5 The mailboxes and the doors

* **J1, occupancy commands** (owning thread → step): `SpscRing<BodySlotOccupancyCommand, 64>`.
  `pushBodySlotOccupancy(slot, occupied)` pushes one; a full ring is an `OG_CHECK` (a lost command leaves the world
  stepping the wrong body set). The hooks drain it first thing in `beforeTick`, through the doors'
  `applyBodySlotOccupancyCommands_Step`, into the step driver's `noteBodySlotOccupancy`; the door `OG_CHECK`s it runs inside
  a step.
* **J3, the published tick offset** (step → owning thread): `std::atomic<int64_t>`, relaxed. The hooks'
  `afterTick` stores `outcome.tick − outcome.physicsStep`; `pumpAndRunSteps` adds it to `batch.first`, with
  no `+ 1`, because `outcome.tick` is the tick the step integrated (`⛔G-03`). Before the first step the
  offset is 0. `tickOffset()` is a relaxed load of it, the time-dilation seam's third door (§6).
* **J4, the batch** (owning thread → task): first step, count, and one deadline per step in a
  `std::array<double, 240>`, copied into the task by value.
* **J6, the step cost** (step → owning thread): `SpscRing<StepCostSample, 256>`; the hooks push one sample
  per step, a full ring drops it and counts the drop, and the post-physics pass drains it into the window
  (`noteStepCost`).

The doors are what `TickDriverStepHooks` needs from the driver (`DriverStepDoors`): the J1 drain, the J3
store, the J6 push, the step-running test and the owner-thread test, both read from the world host's
thread context.
`savesTicks()` and `grantedResims()` forward, read-only, to the step driver's `savesTicks()` and `getDiagnostics().grantedResims()` for a presenter's step side; the step driver exists once the constructor returns.

## §6 Time base, `beforePump`, the rate door and the cap

**Time base.** The scheduler is pumped with the sum of the frame deltas the engine adapter passes to
`startFrame`, never with the engine clock (`⛔G-04`). `hostNowSeconds()` returns that sum and
`stepIntervalSeconds()` the scheduler's step over its rate scale; both are owning-thread doors (the host
time `OG_CHECK`s the owning thread, the interval goes through the scheduler accessor), so a render clock
built on them shares the deadlines' time base.

**`beforePump(hostNowSeconds, rate)`** is the one addition to the transcribed frame. The game is called on
the owning thread immediately before `pump()`, once per frame, with the host time the pump is about to
receive and a `TickRateDoor` over the scheduler. A rate change made through the door therefore applies to
this frame's pump; called after the pump, it would apply only from the next frame's. Nothing changes the
rate through it yet. With `tickOffset()` and the timing samples in the scheduler's time base, it is the
seam a time-dilation controller plugs into.

**The cap.** `requestedMaxCatchUpSteps` is clamped to `[1, kMaxStepsPerFrame]` (240, the size of the
batch's deadline array). When the clamp changed it, the driver logs the Warning
`[SimHost.Frame] og.Sim.MaxCatchUpSteps=<requested> is outside [1, 240]; using <cap>` before the
`[SimHost.Frame]` line (both lines share one formatter buffer in the window, so the order also keeps the
first line intact until it is logged). The scheduler drops the rest of a longer frame and counts it as lost
time.

**The rate bounds** are `Config::minRateScale` and `maxRateScale`, 0.75 and 1.25 by default. The
`[SimHost.Frame]` line prints the literal `rateScale=[0.75,1.25]`, so a configuration with other bounds
prints the defaults.

## §7 Lines

Every line goes through `engine.log(HostLogLevel::Warning, …)` with the text formatted into a buffer the
window reserved at construction (`TickDriverWindow-rationale.md`):

* `[SimHost.Frame] og.Sim.MaxCatchUpSteps=… is outside [1, 240]; using …`, only when the cap was clamped;
* `[SimHost.Frame] role=<Authority|Client> stepping=<inline|worker> dt=<s> maxCatchUpSteps=<n> rateScale=[0.75,1.25] timeBase=worldDelta`,
  once, at the end of construction;
* `[SimHost.FirstStep] role=… thread=<game|worker> physicsStep=<n> batch=<n>`, once, from the thread that
  runs the first batch;
* `[SimHost.Window] …`, once per due window (`tickDriverWindow::kWindowSeconds`, by the engine clock), at
  the end of the post-physics pass that finds it due. The totals (the scheduler's `lostTime()` and
  `ignoredTimeSamples()`, J1's and J6's `drops()`) are read once and passed to both `formatWindowLine` and
  `restart`.

`stepping=` reads `inline` for `Inline` and `worker` for `FrameBounded`.

## §8 What the compiler and the checks hold

* **Compile time:** each template parameter is constrained by its concept; the hooks satisfy `StepHooks`
  and the doors `DriverStepDoors` (both checked where the types are named); the member order is two
  `static_assert`s (§2).
* **`OG_CHECK`s** (each message names what it replaced): the four construction checks (§3); a full J1;
  `endFrame` and `stop()` with the world lock held (G-05); an inline step off the owning thread; the
  occupancy door outside a step; the scheduler and the host time read off the owning thread; the scheduler
  reached before it was built; and a driver destroyed while a step task is still pending (call `stop()`
  first).
* **Allocation.** The driver's own state grows no container on the step path or the frame path: the rings
  are fixed, the batch is an array, the window's sample lists and line buffers are reserved at
  construction. What its collaborators do is theirs and unchanged by the move: the scheduler keeps a
  deadline history of at most 64 segments, the step driver a client's occupancy timeline, and the
  executor stores the dispatched task (a lambda holding the batch) in whatever way the engine's executor
  does.

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

## §10 Provenance

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

The guards are the old frame host's six, with fresh ids (`SimulationTickDriver-guards.md`, "Provenance of
the ids").
