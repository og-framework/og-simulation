<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationTickBackend.h` — rationale

The narrative behind `SimulationTickBackend`, the engine-free orchestration core of a host whose steps
og-simulation's `SimulationTickDriver` schedules, and behind its two world-host concepts,
`TickBackendWorldHost` and `TickBackendWorldBuild`. The header keeps its licence, its docs pointer, its code, its
`OG_CHECK`s, its concepts' self-check `static_assert`s, one member-order `static_assert` and one guard tag; everything
that explains it lives here. Prohibitions: `SimulationTickBackend-guards.md`.

**If this file and `SimulationTickBackend.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by construction: STL
and glm only, through og-simulation's own headers. It names no game engine and no physics engine; its world host is a
template parameter. Everything below is true to a reader with no game engine and no other repository.

---

## §1 What it is

`SimulationTickBackend<GameT, WorldHostT, SimulatableT, EngineT>` is the part of a driver-scheduled host that every
engine adapter would otherwise write again: the order in which the world is built, the presenter and the driver are
created, characters are bound, registered and released, the frame is presented, and the session is stopped. An engine
adapter keeps only its translation from engine objects to neutral values (owner keys, names, static geometry, console
variables, its render sink and its tick functions).

* **It owns**, in this order, the render source (§5), the presenter registration ring (J9, §10), an optional
  `Presenter` and an optional `Driver`. `Presenter`
  is `TickDriverPresenter` over the game's `RenderSnapshot`, `EngineT` and the backend's own `RenderSource`; `Driver`
  is `SimulationTickDriver` over the game, the world host, that presenter and the engine.
* **It borrows** the game (a `TickDriverGame`, with its manager and storage), the world host and the engine, by
  reference from its constructor on. The adapter keeps all three alive longer than the backend (§4).
* **It is header-only and adds no indirect call.** Every collaborator is a template parameter, as for the driver.

**The parameters.**
* `GameT`: the game binding the driver calls (`TickDriverGame`). The backend also reads its `manager()` (the
  integrated tick, `runsPrediction()`, the contact records' tick source) and its `storage()` (the render source and the
  visualization pass).
* `WorldHostT`: the engine's world host, constrained by `TickBackendWorldHost` (§2).
* `SimulatableT`: the game's simulatable type. The render source's `PhysicsComposite` is its physics composite, and
  `acquireAndBind` binds one. The world host does not have to name it, so the backend takes it.
* `EngineT`: the engine port (`TickDriverEngine`), shared by the presenter and the driver.

The presenter is not a parameter. Its render source reads the backend's driver, and the driver's type depends on the
presenter's, so a caller could not name a complete presenter type before the backend exists without a type-erased
source, an indirect call per step in `publish_Step` and `captureCorrection_Step`. The game's `RenderSnapshot` is the
only free choice the presenter has, and the game already names it.

## §2 The world-host concepts

`TickBackendWorldHost<H, SimulatableT>` refines `TickDriverWorldHost` with the members the backend calls:

| member | the backend's call |
|---|---|
| `Config` | the type `build` passes through |
| `bindIntegratedTickSource(manager)` | `start`, first |
| `rootDeclarationIndex()` (const) | `start`, into the presenter's configuration |
| `acquire(key, name, isAuthority, replicatedSlot, publish)` | `acquireAndBind` |
| `bind(slot, id, simulatable, staticData, onWorldLockWait)` | `acquireAndBind`, with a slot |
| `release(key, id, pushBodySlotOccupancy)` | `releaseSlot` |
| `slotOf(key)` (const) | `onRegistered`, `forgetPresenterTarget` |
| `editVizQuery()`, `getVizReader()` (const), `registerVizVolume(...)` | the three visualization doors |

The callables and the manager and static data in the requirement are archetypes in `tickBackendWorldHost`
(`PublishSlotArchetype`, `WorldLockWaitArchetype`, `PushBodySlotOccupancyArchetype`, `ManagerArchetype`,
`StaticDataArchetype`), so a world host whose members are templates over them satisfies the concept by its
declarations. That is a declaration-level check: a body that does not compile for the real types fails where the
backend is instantiated, which is the adapter's own build. `SimulatableArchetype` lets a world host library assert the
concept without a game.

`build` is a separate concept, `TickBackendWorldBuild<H, AdapterOptionalTs...>`, because its last arguments are the
session's adapter optionals, which are the engine's adapter types: the concept cannot state them with neutral types.
It constrains the backend's variadic `build`, so the call names the adapter types and the concept checks them there.

**Who models them.** og-simulation-jolt's `JoltTickWorldHost` `static_assert`s both, with the simulatable archetype and
its three real adapter optionals. The test kit's `FakeWorldHost` `static_assert`s both too. The header's self-check
namespace `tickBackendWorldHostSelfCheck` holds a conforming shape and one shape per refused member: a release without
the occupancy push, a bind without the lock-wait note, a mutating `slotOf`, an acquire without the replicated slot, the
driver concept's members alone, and `build` without or with the wrong adapter optional.

## §3 The calls, in order

Each call is the transcription of the engine-hosted backend's code it replaced; the order of the calls inside each is
unchanged.

* **`build(config, queryableStatics, physicsOnlyStatics, adapterOptionals...)`** hands everything to the world host's
  `build` unchanged. The adapter splits the statics and fills `config` (its console variables, the engine's log sink and
  switches, the game's categories). The world host configures its thread context **before** it emplaces any adapter
  (BL1), so the query adapter gets this role's access predicate and an inline host's owning thread may register without
  the lock; og-simulation-jolt's world host also `OG_CHECK`s that. The backend never touches an adapter optional: it
  passes the caller's.
* **`start(dtSeconds, isAuthority, requestedMaxCatchUpSteps, idleSlackSeconds = 0)`**, once, at the end of the
  session's start of play (in `FreeRunning`, on the runner, from `runAuthorityLoop`: §8):
  1. `bindIntegratedTickSource(game.manager())`;
  2. the presenter, with the manager's `runsPrediction()`, `dtSeconds` as a `float` and the world host's
     `rootDeclarationIndex()`;
  3. an `OG_CHECK` that the presenter exists (§4);
  4. the driver, with `dtSeconds`, `isAuthority`, the threading and `requestedMaxCatchUpSteps`, and
     `idleSlackSeconds` as its configuration's idle slack (used only in `FreeRunning`; the driver checks it). The
     threading is the world host's context's `threading()`, the mode its `build` configured: `Inline`,
     `FrameBounded` or `FreeRunning`. For the first two it is the value the former `stepsInline()` choice gave,
     because a context configured with a bool reports exactly those two. The driver's constructor takes the world host's `lockForStep()` (J2 and the access
     scope, never a step scope) while it builds its hooks and step driver, then logs `[SimHost.Frame]`;
  5. it returns the driver, which the adapter hands to its frame host.
  `dtSeconds` is a `double` because the driver's configuration is; the presenter's `float` is the same value when the
  adapter's step time was a `float`, since a `float` converted to `double` and back is exact.
* **`stop()`** calls the driver's `stop()` when `start` built it: the running flag clears and the pending and blocking
  step tasks are waited for. The adapter calls it after its tick functions stop dispatching and before any teardown;
  the world host must outlive it (§4).
* **`acquireAndBind(key, name, isAuthority, replicatedSlot, publish, id, simulatable, staticData)`**: the world host's
  `acquire`; no slot (a client waiting for the authority's) returns `false` and binds nothing; a slot is bound with the
  driver's `noteWorldLockWait` as the lock-wait note, so the bind's J2 wait reaches the window.
* **`onRegistered(id, key)`**: the slot of `key` (an `OG_CHECK` that there is one: the first pass bound it), the
  driver's `pushBodySlotOccupancy(slot, true)` (an Occupy command on J1, applied by the next step), then the presenter's
  `onRegistered(id, slot)`: called directly in `Inline` and `FrameBounded`, pushed onto J9 in `FreeRunning` (§10).
* **The unregister is two calls**, so an adapter keeps its own work between them:
  * **`forgetPresenterTarget(id, key)`**: the presenter's `onUnregistered(id, slot or 0)`, when the presenter exists;
    directly, or through J9 in `FreeRunning` (§10);
  * **`releaseSlot(key, id)`**: the world host's `release`, with the driver's `pushBodySlotOccupancy` as the occupancy door, so
    the vacated slot reaches the step as a Vacate command (J1) and is never applied on this thread.
* **`present(controls, sink)`**: in `FreeRunning`, first the J9 drain (§10); then the presenter's
  `present_GameThread` with the **driver as the clock**. A world host
  with a shadow world restores it first and passes the presenter a restore note, which the presenter forwards to the
  clock's `noteShadowRestore`: so the restore's time and its refusals reach the driver's window
  (`shadowRestores`/`shadowRestoreUs`/`shadowRefused` of `[SimHost.Window]`).
* **`updateVisualization()`**: the presenter's `updateVisualization_GameThread` with the game's storage.
* **`editVizQuery()`, `getVizReader()`, `registerVizVolume(...)`** forward to the world host's doors of the same names.
  In `FreeRunning` these four are refused (§9).
* **`driver()` and `presenter()`** are the adapter's and the tests' doors; each `OG_CHECK`s that `start` built it.

## §4 The member order

Members are destroyed in reverse declaration order. The driver holds the presenter by pointer, and its step driver
unregisters its resync callback from the game's manager when it is destroyed; the presenter holds the render source by
reference. The presenter registration ring (J9, §10) holds plain values only; it is declared after the render
source and before the presenter, so it outlives the presenter and the driver. So the order is render source <
J9 < presenter < driver, and the backend asserts it with one `static_assert` in its **constructor**. A member function of a class template is compiled only when it is used, so
an assertion there is checked wherever a backend is constructed: in every adapter's build and in the tests. A reorder
was seen to fail an adapter's build there.

The construction order inside `start` is the same: the presenter first, because the driver holds it from its
constructor on. The `OG_CHECK` before the driver's construction holds that at run time.

What the backend borrows must outlive it: the game and its manager (the driver unregisters from the manager), the
engine, and the world host, which the driver locks and steps until it stops. The adapter declares them so, and asserts
its own rows; the backend cannot see where they live.

## §5 The render source

`RenderSource` is the presenter's view of the session and the driver (`TickDriverRenderSource`):
* `storage()` is the game's `storage()`;
* `integratedTick()` is the game's manager's `currentIntegratedTick()`, as `uint32_t`;
* `savesTicks()` and `grantedResims()` are the driver's two read-only doors. The presenter is built before the driver,
  so the source reaches the driver only when these are called, which only the step hooks do, inside a step;
* `PhysicsComposite` is `SimulatableT`'s physics composite, and `kMaxSimulatableSlots` is og-simulation's slot count.
  The presenter `static_assert`s that the game's render snapshot holds one body per declaration in every slot.

## §6 The tests

`SimulationTickBackendTest.cpp` in og-simulation's tests runs the backend over the test kit (`FakeWorldHost` built with
`kConfigureAtBuild`, a recording game over a scripted manager, a recording engine). Its cases, each seen failing under a
named POISON:

| case | pins | POISON |
|---|---|---|
| KitFix | the kit's `lockForStep()` opens no step scope, so the driver's own `StepScope` does not nest with it | the kit's lock with a step scope again |
| Build | the thread context is configured before either adapter is emplaced, and the adapters get its predicate | the fake's build emplaces first |
| Start | the tick source, then the presenter (the root index read), then the driver | the presenter and the driver swapped |
| Stop | `stop()` waits for the outstanding step task before the world host goes; a second `stop()` and one before `start` are no-ops | `stop()` emptied |
| Unregister | the release frees the slot and its Vacate reaches the step world through the driver | the release's push dropped |
| Present | the restore note reaches the driver's window, refused or not | a clock that drops the note |
| Doors | the acquire wait, the bind's wait note, the visualization doors and the render source's reads | — |
| Start (threading) | `start` takes the threading from the world host's context: the Frame line reads `inline` for an authority and an inline client, `worker` for a worker client, and `runner` with the slack for a `FreeRunning` authority | `start` back on the `stepsInline()` choice (the driver's authority check stops the `FreeRunning` arm) |
| AuthorityLoop (order) | `runAuthorityLoop` runs build, `afterBuild`, `start` (its `[SimHost.Frame]` line), `afterStart` and the first step in that order, in virtual time; a stop set in `afterStart` returns before the first wake | — |
| AuthorityLoop (J9) | a registration made on the runner from `beforePump` reaches the presenter only at the next `present()`, and a forgotten target leaves it only then. A present without the drain (the presenter's own `present_GameThread`) shows no change | J9 bypassed: the presenter called directly |
| hidden `[TickBackendFreeRunningFrameThreadBindRefused]` | `acquireAndBind` from a thread that is not the runner stops on the backend's owner check | — (expected to fail) |

## §7 What an engine adapter supplies

* the world host (an engine's `TickBackendWorldHost`), its configuration and the split static geometry;
* a `TickDriverGame` and the simulatable type;
* the engine port, its frame host and tick functions, which call the driver `start` returns;
* the owner key, the name, the role and the replicated-slot get and publish of each acquire;
* `PresentControls` from its console variables, and a render sink;
* the call order around the backend's calls: its tick functions unregistered before `stop()`, its own render target
  registered after `onRegistered` and unregistered between `forgetPresenterTarget` and `releaseSlot`.
* for a `FreeRunning` authority (§8): a dedicated runner thread, a `TickDriverRunnerHost` for its sleep, a
  `TickDriverIdleGame` whose `beforePump` drains the game's own registration queue on the runner, the stop flag,
  the start latch it releases in `afterStart`, and the join before `stop()`.

## §8 `runAuthorityLoop`: the `FreeRunning` start and stop

`runAuthorityLoop(worldConfig, queryableStatics, physicsOnlyStatics, AuthorityLoopConfig{ dtSeconds,
requestedMaxCatchUpSteps, idleSlackSeconds }, runner, stopRequested, afterBuild, afterStart, adapterOptionals...)`
is the whole runner side of a `FreeRunning` authority. The host calls it on its dedicated runner thread, and it
returns after the stop. It requires a `TickDriverIdleGame`, a `TickDriverRunnerHost` and `TickBackendWorldBuild`
over the adapter optionals. In order:

1. `build(...)` with the two static descriptions. They are taken **by value**: the host moves them into the
   runner's start packet, and this is their one hand-over between the threads. They are moved into a local scope
   around `build` and freed when it ends, so the loop holds no copy of the imported geometry.
2. An `OG_CHECK` that the world host's context is `FreeRunning`: only that build makes the runner the owner thread.
3. `afterBuild()`: the host builds the game state that binds the adapters.
4. `start(dtSeconds, true, requestedMaxCatchUpSteps, idleSlackSeconds)`: the presenter, then the driver, whose
   constructor prints `[SimHost.Frame] … stepping=runner`.
5. `afterStart(driver)`: the host releases its start latch.
6. Unless the stop flag is already set, the driver's `runAuthorityLoop(runner, stopRequested)`.

The host's half and the runner's, step by step:

| # | frame thread (the host) | runner |
|---|---|---|
| 1 | Imports the static geometry into the two descriptions | — |
| 2 | Starts the dedicated runner thread, moving the descriptions into its start packet, and waits on the start latch | — |
| 3 | — | `build`: configures the context on this thread (the owner mark), builds the world and its statics, emplaces the session's adapters; the descriptions are freed after it |
| 4 | — | `afterBuild()` |
| 5 | — | `start`: the presenter and the driver; the driver prints `[SimHost.Frame]` |
| 6 | Released by the latch. From now on it calls `present()` only, and registrations go through the game's queue | `afterStart(driver)` releases the latch |
| 7 | — | Returns if the stop is already set; otherwise loops |
| S1 | Stops its own frame calls into the driver, then sets the stop flag | — |
| S2 | — | The loop returns at its next wake and touches nothing after it |
| S3 | Joins the runner, then calls `stop()` (the driver checks that its loop returned) and tears down as before | — |

The host destroys the backend, the driver and the world host on the frame thread after the join, or on the runner,
never on a thread started after the runner ended (the thread context's rule for its owner count).

## §9 Who calls what in `FreeRunning`

The runner owns the world host, the slots, the driver's scheduler and window, and the producer side of J1 and J9.
The frame thread keeps the presenter and the queues and snapshots it reads.

| member | thread in `FreeRunning` | how it is held |
|---|---|---|
| `build`, `start`, `runAuthorityLoop` | runner | `runAuthorityLoop` calls `build` and `start` itself, on its thread, and checks the mode; the driver's loop `OG_CHECK`s that this thread is the owner |
| `acquireAndBind`, `releaseSlot`, `onRegistered`, `forgetPresenterTarget` | runner | an `OG_CHECK` of the owner thread, first |
| `present` | frame thread | drains J9, then presents with the driver as the clock (its two present-clock doors are readable off the runner) |
| `updateVisualization`, `editVizQuery`, `getVizReader`, `registerVizVolume` | none | refused by an `OG_CHECK`: a `FreeRunning` authority has no game-thread viz world |
| `stop` | frame thread, after the join | the driver's check that the loop returned |
| `driver()`, `presenter()` | either | doors; what is called through them keeps its own thread rule |

In `Inline` and `FrameBounded` every member keeps its behaviour: each new check and branch is `FreeRunning` only.

## §10 J9: presenter registrations

The presenter's target map is the frame thread's: `present()` iterates it. In `FreeRunning` the runner registers
and forgets characters, so it must not touch the map. `onRegistered` and `forgetPresenterTarget` push a
`simulationTickBackend::PresenterRegistration{ id, slot, registered }` into J9, an
`SpscRing<PresenterRegistration, 64>` (runner → frame thread), and `present()` drains it at its top, calling the
presenter's `onRegistered` or `onUnregistered` for each entry in push order, before it presents.

- **One producer, one consumer:** the runner pushes, the frame thread's `present()` pops.
- **A full ring is an `OG_CHECK`**, as for J1: a lost registration would leave the presenter rendering a character
  that left, or never rendering one that joined. 64 entries is four times a full roster (8 slots) joining and leaving between two presents.
- **The order is the push order,** so a register and a forget of the same id in one frame apply in that order.
- In `Inline` and `FrameBounded` the presenter is called directly, as before, and J9 stays empty.
- The guard `⛔G-01` sits on the `FreeRunning` branch of the one function that routes both calls (`toPresenter`).

