<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverConcepts.h` — rationale

The narrative behind `TickRateDoor`, `IdleWake` and the seven tick-driver concepts:
`TickDriverEngine`, `TickDriverGame`, `TickDriverIdleGame`, `TickDriverPresenterSteps`, `TickDriverWorldHost`,
`TickDriverRenderSink` and `TickDriverRunnerHost`, and the helper concept `OptionalVec3Result` (§5). The header keeps its licence,
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

## §9 Provenance

Written on 2026-10-16 from the og-tickdriver design, as the contracts the brawler's Unreal frame host
and Jolt backend host fulfil today. `IdleWake`, `TickDriverIdleGame`, `TickDriverRunnerHost` and §8 were
added on 2026-11-12 for the `FreeRunning` mode; `StepThreading` moved to `TickDriverThreadContext.h` the
same day. `OptionalVec3Result` was added on 2026-11-13, when the Android build (libc++) failed on the render sink's
return-type requirements (§5). Nothing in this file depends on any document outside this
submodule.
