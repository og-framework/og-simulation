<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverConcepts.h` — rationale

The narrative behind `TickRateDoor`, `IdleWake` and the nine tick-driver concepts:
`TickDriverEngine`, `TickDriverGame`, `TickDriverSampleGame`, `TickDriverReceiveGame`, `TickDriverIdleGame`, `TickDriverPresenterSteps`,
`TickDriverWorldHost`, `TickDriverRenderSink` and `TickDriverRunnerHost`, and the helper concept `OptionalVec3Result` (§5). The header keeps its licence,
its docs pointer, its code and its self-check `static_assert`s; everything that explains it lives here. It has no guards (`TickDriverConcepts-guards.md`
says why): every rule it carries is a `static_assert`.

**If this file and `TickDriverConcepts.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL and glm only. Everything below is true to a reader with no game engine and no other
repository.

---

## §1 What it is for

A tick driver runs a fixed-step simulation inside a host's frame loop: it pumps a `SimulationScheduler`
once per frame, runs the due steps through a `SimulationStepDriver` (inline, or on a task the engine
runs), and calls the host's code at fixed moments. It is a template over four collaborators, and each
parameter is constrained by a concept in this header, the way `StepHooks` and `PhysicsWorldAdapter`
constrain the step driver:

| collaborator | concept | what it supplies |
|---|---|---|
| the engine adapter | `TickDriverEngine` | a clock, a frame number, a log sink, a diagnostics switch and an executor (§2) |
| the game | `TickDriverGame` | its manager and storage, and the hooks at the frame's and the step's moments (§3) |
| the presenter's step half | `TickDriverPresenterSteps` | the two step-thread doors of the render path (§4) |
| the world host | `TickDriverWorldHost` | the step world, the step's lock and the thread context (§4) |

A fifth concept, `TickDriverRenderSink`, is the engine side of the render path: the presenter writes
poses into it (§5). The collaborators are template parameters, never `std::function`, so calling them
adds no indirect call to the step path.

Two more concepts serve the `FreeRunning` mode, where a runner thread of its own pumps and steps (§8):
`TickDriverIdleGame` refines `TickDriverGame` with the hook the runner waits in, and `TickDriverRunnerHost`
supplies the runner's fallback sleep.

`TickDriverSampleGame` refines `TickDriverGame` with an optional input-sample hook, for a game whose
host-side input must be sampled once per frame before the pump (§9). `TickDriverReceiveGame` refines it with an
optional per-step receive hook, for a client game that takes in what arrived from its authority at the start of
each step, before the step reads it (§10).

The driver that uses these concepts is not in this header. This file states what each requirement is
for; the driver's own documentation states when it calls each one.

## §2 `TickDriverEngine`

- `nowSeconds()` and `frameNumber()`: the host clock and the engine's frame counter, both callable on a
  const engine;
- `logEnabled(level)` and `log(level, line)`: the host log sink for the driver's lines, with the levels
  of `HostLogLevel` (`TickDriverLog.h`). `logEnabled` lets a caller skip formatting a line whose level
  is disabled;
- `kHostDiagnostics`: a `bool` **constant expression**. The driver and the world host branch on it with
  `if constexpr`, so a build without diagnostics compiles none of their diagnostic code;
- the executor half:
  - `Handle`: a task handle; a default-constructed one names no task. It is copyable, because a driver
    keeps the newest task's handle and the handle it must wait for at the same time;
  - `Completion`: the engine's end-of-frame completion event, which a deferred continuation can be
    attached to;
  - `isPending(handle)`: the task has not finished;
  - `waitFor(handle)`: block until it has;
  - `dispatchAfter(previous, task)`: queue a task that runs after `previous` and return its handle;
  - `deferUntil(completion, handle, task)`: attach a continuation that runs once `handle` finishes and
    hold `completion` until it has. It returns `false` when the engine has no completion event to hold,
    and the caller then waits instead.

`dispatchAfter` and `deferUntil` take any callable, so they are templates and the concept cannot name
them directly. It calls them with a probe callable, `tickDriverConceptProbe::Task`.

## §3 `TickDriverGame`

The game supplies three types, `Manager`, `Storage` and `RenderSnapshot`, returns its manager and
storage **by reference** (`manager()`, `storage()`), and the seven hooks, each returning `void` so none
can veto or reorder the frame:

| hook | what it is for |
|---|---|
| `beforePump(hostNowSeconds, rate)` | the game's chance to change the step rate through `TickRateDoor` before the pump, on the scheduler's owning thread. A game with no rate control makes it a no-op |
| `onFrameStepsDue(firstUpcomingTick, steps)` | once per frame that has steps, before they run |
| `afterInlineBatch(firstUpcomingTick)` | the game's work after an inline batch of steps |
| `releaseInputsForAuthorityTick(tick)` | on the authority, release the delayed inputs for the tick the step will simulate |
| `onStepStarted(seconds)` | the step-start time, when the tick's inputs are consumed |
| `onStepEnded()` | the end of the step, before the step's outputs are published |
| `onPostPhysics()` | once per frame, after the frame's steps |

## §4 `TickDriverPresenterSteps` and `TickDriverWorldHost`

`TickDriverPresenterSteps` is the presenter's step-thread half:
- `captureCorrection_Step(physicsStep)`: capture what a resimulation corrected, for the render path to
  blend out;
- `publish_Step(outcome, stepDeadlineSeconds)`: publish the step's render snapshot. It reads the
  `TickOutcome` and cannot rewrite it.

`TickDriverWorldHost` owns the step world and its thread facts:
- `World` is a `PhysicsWorldAdapter`, so the step driver can run over `stepWorld()`;
- `lockForStep()` returns the scope that holds the world for the whole step: a `WorldLockScope` and a
  `StepScope` (`TickDriverThreadContext.h`), or whatever the host's backend needs. The concept refuses a
  `void` return, which would hold nothing;
- `afterTick_Step(outcome)` and `afterRunTick_Step(physicsStep, outcome)`: the host's work inside the
  step's `afterTick` hook, and after `runTick` returns;
- `threadContext()` returns the host's `TickDriverThreadContext` by const reference: the driver reads
  the role, the mode and the predicates and cannot reconfigure them.

## §5 `TickDriverRenderSink`

The engine side of the render apply. Positions are og-simulation lengths and the API names carry no
unit suffix; the sink converts to engine units.
- `applyRootPose(id, position, rotation)`: move the id's root to the pose (the rotation is optional;
  without it the engine keeps the root's rotation) and return the position the engine holds **after**
  the write. The return converts to `std::optional<glm::vec3>`: empty when the id has no target (an
  engine object that is gone). A sink whose targets never go away may return `glm::vec3` directly;
- `readAnchor(id)`: the position the engine holds now, without writing; empty when there is no target;
- `name(id)`: the id's display name, for log lines.

**`OptionalVec3Result`.** Both position returns are checked by one helper concept: the type is not `void`, **and then**
it converts to `std::optional<glm::vec3>`. The `void` test has to come first. The `ApplyReturnsVoid` self-check (§6)
makes the compiler check a `void` return, and the libc++ of Android NDK r27c (clang 18.0.3) fails
`std::convertible_to<void, std::optional<glm::vec3>>` with a hard error inside `<optional>` ("cannot form a reference
to 'void'") instead of answering `false`. A concept's `&&` checks its left operand first and does not check the right
one when the left is not satisfied, so for `void` the conversion is never formed. The MSVC STL answers `false`, so an
MSVC build accepts the requirement with the `void` test dropped or placed second; only a libc++ compile of this header
then fails, in every translation unit that includes it. All four spellings were compiled on 2026-11-13: the NDK's
clang++ with libc++ (`--target=aarch64-linux-android26 -std=c++20`) and clang-cl with the MSVC STL.

## §6 What the self-checks pin

Each concept has a `<concept>SelfCheck` namespace with one conforming shape and at least two
non-conforming ones. Every negative shape declares (or inherits) every member of the conforming shape
except the one property it names, so each assertion stays tied to that property.

- `tickDriverEngineSelfCheck`: a non-constant `kHostDiagnostics`, a missing `deferUntil`, a non-const
  clock and a move-only `Handle` are refused;
- `tickDriverGameSelfCheck`: a missing `beforePump`, a missing `afterInlineBatch`, a hook returning
  `bool` and a manager returned by value are refused;
- `tickDriverSampleGameSelfCheck`: every game shape derives from `tickDriverGameSelfCheck::Conforming`. A game
  without `sampleInput` is refused as a sample game and is still a `TickDriverGame`; a `sampleInput` returning
  `bool` and one taking an argument are refused, and so is a type that has `sampleInput` but is not a
  `TickDriverGame`;
- `tickDriverReceiveGameSelfCheck`: the same four refusals for `receiveForStep` (missing, returning `bool`, taking an
  argument, on a type that is not a `TickDriverGame`), and a game that samples but does not receive is a sample game and
  not a receive game: each hook is opted into alone;
- `tickDriverIdleGameSelfCheck`: every shape derives from `tickDriverGameSelfCheck::Conforming`, so it is
  a `TickDriverGame` and differs from the reference only in `idleUntil`. A game without `idleUntil` is
  refused as an idle game and is still a `TickDriverGame`; an `idleUntil` returning `void` is refused;
- `tickDriverPresenterStepsSelfCheck`: a missing `captureCorrection_Step` and a `publish_Step` taking a
  mutable outcome are refused;
- `tickDriverWorldHostSelfCheck`: a missing `threadContext`, a mutable `threadContext`, a `void`
  `lockForStep` and a `World` that is not a `PhysicsWorldAdapter` are refused;
- `tickDriverRenderSinkSelfCheck`: a sink returning a plain `glm::vec3` is accepted; a missing `name`, a
  `void` `applyRootPose` and a non-const `readAnchor` are refused. The `void` shape also checks
  `OptionalVec3Result`'s order, but only under libc++: there the header does not compile when the `void` test is
  dropped or comes second (§5). Under the MSVC STL it compiles either way;
- `tickDriverRunnerHostSelfCheck`: a missing `sleepUntil` and a `sleepUntil` returning `bool` are refused.

`BasicLockable` is in `TickDriverThreadContext.h`, with its own self-check.

## §7 `TickRateDoor` and `StepThreading`

`TickRateDoor` is the only part of the scheduler a game reaches: `setRateScale`, `rateScale`,
`lostTime` and `physicsStepCount`, each forwarding to the same-named `SimulationScheduler` member. It
holds a pointer to the scheduler, which must outlive it. The scheduler has no synchronization, so the
door is used only on the scheduler's owning thread, which is where `beforePump` runs.

`StepThreading` names the three ways a driver can run its steps: `Inline` (on the thread that pumps),
`FrameBounded` (on an engine task, waited for within the frame) and `FreeRunning` (on a thread of its
own). It is declared in `TickDriverThreadContext.h`, which this header includes, so every user of this
header still sees it: the thread context is configured with the mode (`TickDriverThreadContext-rationale.md`
§8).

## §8 `IdleWake`, `TickDriverIdleGame` and `TickDriverRunnerHost`

In `FreeRunning` (§7) a runner thread of its own pumps the scheduler and runs the steps. Between steps it
has nothing to do until the next step's deadline or until input arrives. `TickDriverIdleGame` gives the
game that wait: `IdleWake idleUntil(double deadlineSeconds)`. It refines `TickDriverGame`, so a game that
never runs free does not declare it and is still a `TickDriverGame` (the self-check pins both).

**The hook contract.**
- `idleUntil` runs on the runner thread. Its deadline is absolute seconds on the engine clock
  (`TickDriverEngine::nowSeconds()`), so whatever the hook waits on must read the same clock source.
- While the deadline is ahead, it blocks on an OS wait (a socket wait, an event), not a spin.
- It returns `IdleWake::Activity` after it has handled at least one inbound event. The event's handlers
  ran on the runner thread, and whatever they sent has left before the hook returns.
- Otherwise it returns `IdleWake::Deadline`, at or after the deadline.
- It returns `IdleWake::Unavailable` when it cannot wait, for example because its socket is failing. It
  returns at once, without spinning.
- A deadline at or before now is one non-blocking pass. The hook never spins until a deadline.
- Timer precision belongs to the implementer: how late a wait returns is a property of the OS wait it
  uses.

**What the values tell the caller.** `Deadline` says the wait reached its deadline. `Activity` says it
ended early because work arrived and was handled. `Unavailable` says the hook did not wait, and so does a
`Deadline` that comes back while the deadline is still ahead. Calling the hook again at once after either
of those would busy-loop; `TickDriverRunnerHost` exists so that the caller can sleep instead.

**`TickDriverRunnerHost`.** The runner's host supplies one thing: `void sleepUntil(double deadlineSeconds)`,
a plain sleep of the runner thread to an absolute deadline on the engine clock, for when the idle hook
did not wait. It returns nothing. The clock is the engine's (`TickDriverEngine::nowSeconds()`), not the
runner host's, and the thread's priority belongs to whoever creates the runner thread, so the concept
requires neither. The runner thread is dedicated to one loop and ends with it
(`TickDriverThreadContext-rationale.md` §8).

## §9 `TickDriverSampleGame`

`TickDriverSampleGame` gives a game one more hook: `void sampleInput()`. It refines `TickDriverGame`, so a game
that does not sample does not declare it and is still a `TickDriverGame` (the self-check pins both), and a
driver that calls it checks the concept with `if constexpr` first.

**The hook contract.**
- The driver calls it once per frame, on the frame's owning thread, before anything else in that frame's
  pass: before the frame's clock read, the rate door (`beforePump`), the pump and the frame's steps
  (`SimulationTickDriver-rationale.md` §6). It is called in a frame with no due step too.
- It is where the game publishes what its host sampled for this frame (for example its input state) to
  wherever the steps read it. Everything the frame then pumps and steps sees that sample.
- It takes nothing and returns `void`: it cannot veto or skip the frame, and it is not handed the frame's
  time, which is read after it.
- It is never called in `FreeRunning`. A free-running authority has no local input, and its runner wakes
  per step deadline, not per frame.

## §10 `TickDriverReceiveGame`

`TickDriverReceiveGame` gives a client game one more hook: `void receiveForStep()`. It refines `TickDriverGame`, so a
game that does not model it does not declare it and is still a `TickDriverGame` (the self-check pins both), and the
step hooks that call it check the concept with `if constexpr` first, so such a game compiles neither the call nor its
branch (`TickDriverStepHooks-rationale.md` §3).

**What it means to a game author.** "Take in, now, everything that has arrived for this step, so that this step uses
it." A networked client's arrivals (remote inputs, corrections, clock samples, membership and lifecycle changes) are
applied in this hook, and the step that follows reads all of them: its resimulation check sees a correction received
here, and its input collect (og-simulation's `collectInputAll`) reads the inputs received here. Whatever a game applies
anywhere later in the step reaches only the next step.

**The hook contract.**
- It is called once per client step (each `runTick`, whatever kind of step it turns out to be), on the step's
  thread, at the step's start: after the step's occupancy drain and before its resimulation check, any replay,
  its correction capture, its clock advance and its input collect. It is never called during a replay; the step
  hooks are not.
- It is never called on an authority step. An authority's inbound work has its own hooks: the per-tick input release
  (`releaseInputsForAuthorityTick`) and, in `FreeRunning`, the idle wait (`idleUntil`, §8).
- It takes nothing and returns `void`: it cannot veto or skip the step, and it is not told the step's tick, which the
  step decides after it.
- Its cost is part of the step's pre-step time (`TickDriverStepHooks-rationale.md` §5).

**Beside `sampleInput`.** The two hooks are independent (the self-check pins it). `sampleInput` (§9) is the frame's
*local* sample, once per frame on the frame's owning thread before the pump; `receiveForStep` is the step's *remote*
arrivals, once per client step on the step's thread.

<!-- lint-external-ref: collectInput -- a candidate name for receiveForStep that was weighed and not chosen; it names nothing -->
<!-- lint-external-ref: beforeCollectInput -- a candidate name for receiveForStep that was weighed and not chosen; it names nothing -->
**The name.** Three were weighed, for what each tells a game author:
- `beforeTick`: says only where it runs. It is also the name of the step-hook moment that calls it
  (`StepHooks::beforeTick(const UpcomingTick&)`), which runs on both roles, so a game author would expect it on the
  authority too, and every reading of "beforeTick" in this library would need a qualifier.
- `collectInput` / `beforeCollectInput`: input-specific. `collectInput` reads as the collect itself, so an author would
  put the step's input gathering in it, which is og-simulation's `collectInputAll`, not the game's job; and it sits
  beside `sampleInput`, a different, per-frame hook. `beforeCollectInput` is right about the order but narrower than
  the hook: corrections, clock samples and lifecycle changes arrive here too, and a correction's effect (the
  resimulation check) comes before the collect.
- `receiveForStep` (chosen): it names the job (receive) and its contract (for this step, so this step uses it), it
  covers every kind of arrival, it reads as the remote counterpart of `sampleInput`, and it collides with no name in
  this library.

## §11 Provenance

Written on 2026-10-16 from the og-tickdriver design, as the contracts the brawler's Unreal frame host
and Jolt backend host fulfil today. `IdleWake`, `TickDriverIdleGame`, `TickDriverRunnerHost` and §8 were
added on 2026-11-12 for the `FreeRunning` mode; `StepThreading` moved to `TickDriverThreadContext.h` the
same day. `OptionalVec3Result` was added on 2026-11-13, when the Android build (libc++) failed on the render sink's
return-type requirements (§5). `TickDriverSampleGame` and §9 were added on 2026-11-18, so that taking the
input sample before the pump is the driver's rule rather than each host's. `TickDriverReceiveGame` and §10 were
added on 2026-11-19, so that a client step's receive precedes the step's resimulation check and input collect, rather
than following the collect in `onStepStarted`, where what it received reached only the next step. Nothing in this
file depends on any document outside this submodule.
