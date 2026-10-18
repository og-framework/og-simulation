<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationTickBackend.h` — rationale

The narrative behind `SimulationTickBackend`, the engine-free orchestration core of a host whose steps
og-simulation's `SimulationTickDriver` schedules, and behind its two world-host concepts,
`TickBackendWorldHost` and `TickBackendWorldBuild`. The header keeps its licence, its docs pointer, its code, its
`OG_CHECK`s, its concepts' self-check `static_assert`s and one member-order `static_assert`; everything that explains
it lives here. Prohibitions: `SimulationTickBackend-guards.md`.

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

* **It owns**, in this order, the render source (§5), an optional `Presenter` and an optional `Driver`. `Presenter`
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
| `release(key, id, pushOccupancy)` | `releaseSlot` |
| `slotOf(key)` (const) | `onRegistered`, `forgetPresenterTarget` |
| `editVizQuery()`, `getVizReader()` (const), `registerVizVolume(...)` | the three visualization doors |

The callables and the manager and static data in the requirement are archetypes in `tickBackendWorldHost`
(`PublishSlotArchetype`, `WorldLockWaitArchetype`, `PushOccupancyArchetype`, `ManagerArchetype`,
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
* **`start(dtSeconds, isAuthority, requestedMaxCatchUpSteps)`**, once, at the end of the session's start of play:
  1. `bindIntegratedTickSource(game.manager())`;
  2. the presenter, with the manager's `runsPrediction()`, `dtSeconds` as a `float` and the world host's
     `rootDeclarationIndex()`;
  3. an `OG_CHECK` that the presenter exists (§4);
  4. the driver, with `dtSeconds`, `isAuthority`, the threading (`Inline` when the world host's context steps inline,
     else `FrameBounded`) and `requestedMaxCatchUpSteps`. The driver's constructor takes the world host's `lockForStep()` (J2 and the access
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
  driver's `pushOccupancy(slot, true)` (an Occupy command on J1, applied by the next step), then the presenter's
  `onRegistered(id, slot)`.
* **The unregister is two calls**, so an adapter keeps its own work between them:
  * **`forgetPresenterTarget(id, key)`**: the presenter's `onUnregistered(id, slot or 0)`, when the presenter exists;
  * **`releaseSlot(key, id)`**: the world host's `release`, with the driver's `pushOccupancy` as the occupancy door, so
    the vacated slot reaches the step as a Vacate command (J1) and is never applied on this thread.
* **`present(controls, sink)`**: the presenter's `present_GameThread` with the **driver as the clock**. A world host
  with a shadow world restores it first and passes the presenter a restore note, which the presenter forwards to the
  clock's `noteShadowRestore`: so the restore's time and its refusals reach the driver's window
  (`shadowRestores`/`shadowRestoreUs`/`shadowRefused` of `[SimHost.Window]`).
* **`updateVisualization()`**: the presenter's `updateVisualization_GameThread` with the game's storage.
* **`editVizQuery()`, `getVizReader()`, `registerVizVolume(...)`** forward to the world host's doors of the same names.
* **`driver()` and `presenter()`** are the adapter's and the tests' doors; each `OG_CHECK`s that `start` built it.

## §4 The member order

Members are destroyed in reverse declaration order. The driver holds the presenter by pointer, and its step driver
unregisters its resync callback from the game's manager when it is destroyed; the presenter holds the render source by
reference. So the order is render source < presenter < driver, and the backend asserts it with one
`static_assert` in its **constructor**. A member function of a class template is compiled only when it is used, so
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

## §7 What an engine adapter supplies

* the world host (an engine's `TickBackendWorldHost`), its configuration and the split static geometry;
* a `TickDriverGame` and the simulatable type;
* the engine port, its frame host and tick functions, which call the driver `start` returns;
* the owner key, the name, the role and the replicated-slot get and publish of each acquire;
* `PresentControls` from its console variables, and a render sink;
* the call order around the backend's calls: its tick functions unregistered before `stop()`, its own render target
  registered after `onRegistered` and unregistered between `forgetPresenterTarget` and `releaseSlot`.
