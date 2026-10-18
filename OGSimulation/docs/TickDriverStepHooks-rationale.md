<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverStepHooks.h` — rationale

The narrative behind `StepCostSample`, the `DriverStepDoors` concept, the `StepDoors` aggregate and
`TickDriverStepHooks`, the tick driver's implementation of og-simulation's `StepHooks` concept. The
header keeps its licence, its docs pointer, its code, its `OG_CHECK`s, its self-check `static_assert`s
and three guard tags; everything that explains it lives here. Prohibitions:
`TickDriverStepHooks-guards.md`.

**If this file and `TickDriverStepHooks.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL and glm only, through og-simulation's own headers. Everything below is true to a reader with
no game engine and no other repository.

---

## §1 What it is

`SimulationStepDriver` calls four hooks once per `runTick`, on the step's thread, and never during a
replay (`StepHooks-rationale.md`). `TickDriverStepHooks` is the tick driver's set of those hooks. It
takes the step's three wall-clock times, drains the driver's occupancy commands into the step driver,
releases the authority's inputs one tick at a time, and hands the step's results to the game, the
presenter, the world host and the driver's own mailboxes.

It is a class template over one `StepDoors` aggregate, which names every collaborator by reference
(§2). It holds the aggregate by value and owns nothing it names, so the header needs neither the
driver's type nor the manager's: the driver constructs the hooks before its `SimulationStepDriver`
and passes them to it by reference. It is header-only and adds no indirect call: every door is a
direct call on a template parameter.

The tick driver, `SimulationTickDriver`, instantiates it: it emplaces the hooks before its step driver
(`SimulationTickDriver-rationale.md` §2).

## §2 The doors

`StepDoors<DriverT, GameT, PresenterT, WorldHostT, EngineT>` is a plain aggregate of five references.
Each parameter is constrained by its concept, so a wrong collaborator fails at the aggregate, not
inside a hook body:

| member | concept | what the hooks call |
|---|---|---|
| `driver` | `DriverStepDoors` (this header) | the occupancy drain J1, the offset publish J3, the step-cost push J6, the step-context test and the owner-thread test |
| `game` | `TickDriverGame` | `releaseInputsForAuthorityTick`, `onStepStarted`, `onStepEnded` |
| `presenter` | `TickDriverPresenterSteps` | `captureCorrection_Step`, `publish_Step` |
| `worldHost` | `TickDriverWorldHost` | `afterTick_Step` |
| `engine` (const) | `TickDriverEngine` | `nowSeconds` |

`TickDriverStepHooks` accepts only a `StepDoors` specialization (`isStepDoors`), so it cannot be
instantiated over a single collaborator by mistake.

**`DriverStepDoors`** is the part of the driver the hooks reach. The driver supplies it; this header
only states its shape:

| door | role |
|---|---|
| `applyOccupancyCommands_Step()` | drain the occupancy ring J1 into the step driver's `noteOccupancy` |
| `publishTickOffset_Step(int64_t)` | store the tick offset J3 for the driver's owning thread |
| `pushStepCost_Step(const StepCostSample&)` | push one step-cost sample into the ring J6 |
| `isStepRunningOnThisThread() const` | a step of this driver is running on the calling thread (its step scope on the world host's thread context is open) |
| `isOwnerThread() const` | the calling thread is the owner thread of the driver's role (the thread context's owner mark) |

**`StepCostSample`** is the J6 record: `preSeconds = t1 - t0`, `stepSeconds = t2 - t1`, and the
outcome's `replayedTicks` and `resimRefused` (§5). It is defined here because the hooks produce it;
the driver's ring and window consume it.

`currentStepDeadline` is a public member. The driver sets it before each `runTick` from the frame's
batch; `afterTick` copies it into the presenter's publish.

## §3 The hooks, per role

| hook | client | authority |
|---|---|---|
| `beforeTick(u)` | take `t0`; check the step context; drain J1 | take `t0`; check the step context; drain J1; check the owner thread and that `u.authorityTick` is set; release the inputs of `*u.authorityTick`, one tick (`⛔G-01`) |
| `beforeSimulate(u)` | check the step context; `captureCorrection_Step(u.physicsStep)` (it does work only after a resimulation); take `t1` (`⛔G-03`) | the same; the authority never resimulates, so the capture finds nothing to do; then `t1`, after the release, immediately before the integrate |
| `beforePhysics(t)` | check the step context; `onStepStarted(t1)` | the same |
| `afterTick(o)` | take `t2`; check the step context; `onStepEnded()`; `publish_Step(o, currentStepDeadline)`; `afterTick_Step(o)`; publish J3 = `o.tick - o.physicsStep` (`⛔G-02`); push J6 | the same |

`TickDriverStepHooks.CallOrder.AClientTickWithAResimRunsEachHookOnceAroundTheReplay` and
`TickDriverStepHooks.CallOrder.AnAuthorityTickReleasesBeforeTheCaptureAndPublishesInOrder` pin these
sequences exactly, interleaved with the step driver's own world and manager calls.

## §4 Why each hook does what it does

- **J1 first.** Occupancy reaches the step driver on the step's thread, before the step reads the
  world and before any replay, so a vacated slot is cleared from every held tick before a replay could
  restore it (`SimulationStepDriver-guards.md` G-05).
- **The release per step (`⛔G-01`).** Each authority tick releases exactly the inputs due for it,
  immediately before it is simulated. The tick comes from the step driver
  (`UpcomingTick{true, serverTick + 1}`), which checks after the integrate that it simulated exactly
  that tick. The hook releases once, and it calls none of the game's per-frame hooks
  (`onFrameStepsDue`, `afterInlineBatch`): those run once per frame, and from here they would run once
  per step; a second release in the same hook would release a later tick's inputs before that tick is
  simulated. `TickDriverStepHooks.Authority.ReleasesExactlyServerTickPlusOneOncePerStep` runs twenty
  authority steps and checks one release per step, of `serverTick + 1`; the call-order cases record
  every game hook, so a per-frame hook called from a step fails them.
- **The correction capture, then `t1`.** `beforeSimulate` runs after the step driver's replay and
  before the normal step, so after a resimulation the storage holds the corrected present tick; the
  presenter compares it with what it last published. The capture runs before `t1`, so its cost is in
  `t1 - t0` with the replay it follows.
- **`t1` in `beforeSimulate` (`⛔G-03`).** That is the instant the normal step starts consuming input,
  on both roles; the game is given it in `beforePhysics` (`onStepStarted`), after the integrate has
  produced what the game reads at that point.
- **`afterTick`, in exactly this order:** `t2`; `onStepEnded()`; the presenter's publish; the world
  host's `afterTick_Step`; J3; J6.
  - `t2` is the first statement, so the step cost ends where the step ended.
  - The game's step-end call comes before the step's outputs are published (the concept's contract
    for `onStepEnded`, `TickDriverConcepts-rationale.md` §3).
  - `afterTick` runs after the step driver's post-step pass and ring save, so the presenter's
    publish and the world host's hand-over see the step's final state and, on a client, the tick the
    ring just saved. Neither runs during a replay, so a replay never publishes an intermediate state.
  - **J3 after the publishes (`⛔G-02`).** The offset is stored after the step, from the outcome; only
    the step-cost push follows it, and it changes nothing the step or the owning thread decides on.
  - J6 goes last: the times it reports were taken before the publishes.

  Both call-order cases fail when `onStepEnded` moves after the publish.

## §5 Step cost and the checks

**Step cost.** Three wall-clock times per `runTick`, from `engine.nowSeconds()`, on the step's thread:
- `t0`, the first statement of `beforeTick`, before the J1 drain;
- `t1`, the step-start time of `beforeSimulate` (`⛔G-03`): after any replay on a client, and after
  the input release on the authority;
- `t2`, the first statement of `afterTick`, after the step, the post-step pass and the ring save.

`afterTick` pushes `StepCostSample{t1 - t0, t2 - t1, replayedTicks, resimRefused}`. So `t2 - t1` is the
normal step on both roles. `t1 - t0` is, on a client, the J1 drain, the resimulation check, any replay
and the correction capture after it; on the authority, the J1 drain and the input release. No hook runs
during a replay, so a replay adds no sample of its own: it is inside the replaying tick's `t1 - t0`.
`TickDriverStepHooks.StepCost.SamplesTheThreeInstantsAndTheOutcome` checks both differences against the
positions of the three clock reads, for a granted and a refused resimulation.

**The checks** are `OG_CHECK`s, so they are code, not documentation:
- every hook checks first that a step of this driver is running on the calling thread
  (`isStepRunningOnThisThread`). The hooks reach the step driver, the world and the step-owned
  mailboxes, and from anywhere but a step they race the running step. In `beforeTick` and `afterTick`
  the check follows the time read, so the time read stays the first statement;
- an authority `beforeTick` checks that it runs on the owner thread (`isOwnerThread`): the authority
  steps inline on its owner thread, because the input release delivers into game state that only that
  thread may touch;
- an authority `beforeTick` checks that `upcoming.authorityTick` is set.

Under `OG_STANDALONE_BUILD` an `OG_CHECK` is an `assert`, compiled out under `NDEBUG`; in an engine
build it is the engine's checked assertion. The tests count the two test doors' calls only when a
probe shows the checks are compiled in.

## §6 What the compiler checks

- `driverStepDoorsSelfCheck`: the reference shape models `DriverStepDoors`; a shape without
  `pushStepCost_Step`, one without `isOwnerThread`, one whose step-context test is not `const`, and one
  whose push takes a mutable sample do not.
- `tickDriverStepHooksSelfCheck`:
  - `TickDriverStepHooks` over a `StepDoors` of the five concepts' reference shapes models `StepHooks`;
  - `StepDoors` is an aggregate;
  - `TickDriverStepHooks` cannot be named over a lone collaborator (`HooksExistFor`);
  - `TickDriverStepHooks` is not copyable: the step driver holds it by reference, and the step's times
    live in it.

## §7 Tests

The cases live in og-simulation-tests (TickDriverStepHooksTest.cpp, tag `[TickDriverHooks]`). Each case runs a real
`SimulationStepDriver` over the fake world host's `LoggedWorld`, with a scripted manager, the fake kit's
`RecordingGame`, `FakePresenter` and `FakeWorldHost`, fake driver doors, and an engine whose clock read
appends a `clock` event and returns the event count, so every time read has a position in the one
ordered event list:
- `TickDriverStepHooks.CallOrder.AClientTickWithAResimRunsEachHookOnceAroundTheReplay`;
- `TickDriverStepHooks.CallOrder.AnAuthorityTickReleasesBeforeTheCaptureAndPublishesInOrder`;
- `TickDriverStepHooks.Authority.ReleasesExactlyServerTickPlusOneOncePerStep`;
- `TickDriverStepHooks.TickOffset.IsTheTickMinusThePhysicsStepAcrossStallSkipAndHardResync`;
- `TickDriverStepHooks.Replay.NoHookFiresWhileTheStepDriverReplays`;
- `TickDriverStepHooks.StepCost.SamplesTheThreeInstantsAndTheOutcome`.

## §8 Provenance

Transcribed from the brawler's engine-hosted step hooks (the class BrawlerStepHooksUImpl, which called
its frame host's step doors). The statement order of the four hooks is unchanged. The mapping:
- the frame host's occupancy drain, offset publish, step-cost push and step-context test become the
  `DriverStepDoors` members of the same names; the engine's game-thread query becomes `isOwnerThread`;
- the frame host's input release with a step count of 1 becomes the game's
  `releaseInputsForAuthorityTick`, which takes one tick and no count;
- the two latency stamps become the game's `onStepStarted` and `onStepEnded`;
- the render correction capture and the render snapshot publish become the presenter's
  `captureCorrection_Step` and `publish_Step`;
- the shadow-slot hand-over becomes the world host's `afterTick_Step`;
- the platform clock becomes `engine.nowSeconds()`; the engine's checked assertions become `OG_CHECK`s,
  which lose the hook name the old step-context message printed;
- the engine-side step-cost record becomes `StepCostSample`, with the same four fields.

The three guards are the successors of the old class's three (`TickDriverStepHooks-guards.md`,
"Provenance of the ids").
