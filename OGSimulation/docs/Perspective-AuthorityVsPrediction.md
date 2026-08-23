<!-- SPDX-License-Identifier: MPL-2.0 -->
# Perspective: the authority simulation vs the prediction simulation

**One class, two behaviours.** `SimulationManager` is instantiated with a single `bool`, and that
bool decides whether the object in front of you is *the truth* or *a guess about the truth*. This
document is the inventory of every difference that bool produces: what runs on the authority, what
runs on a predicting client, what runs on neither, and — for each — the mechanism that makes it so.

This is a **perspective** document. It does not replace the headers: every fence, every
per-parameter contract and every invariant stays at its site, and where this document summarises one
it names the file that is authoritative. If this file and a header disagree, **the header is right
and this file is stale** — the same rule `ThreadingCrossings.md` states for its own rows.

**Read this alongside:** `Perspective-RemoteInputFlow.md` (how one input travels between the two
roles), `ThreadingCrossings.md` (which of these calls cross a thread boundary),
`ResimGatePolicy-rationale.md` (why the resim gate has the shape §7 describes), and
`DiagnosticsConventions.md` (the `getDiagnostics()` / `emit*` shape every probe here follows). All
four ship in this directory.

> **Provenance, recorded rather than linked.** Written 2026-08-21 from a first-hand read of the
> headers named in §2, and cross-read against the `og-netcode-v2-input-relay` initiative archive
> (its architecture-review brief and its backlog items 92–96). ⚠ **That archive is NOT distributed
> with this submodule** — it is private working material, and this note deliberately does not link
> to it. Everything this document *asserts* is anchored to a file in this repository, and only to
> those. Where the archive and the tree disagreed, the tree won.

---

## 1. The shape, in one paragraph

A process runs **one** `SimulationManager` per role, constructed with `shouldRunPrediction`. That
flag is stored once as `m_runsPrediction` and is never rewritten; every role difference in the whole
core descends from it. On the authority the manager owns a `ServerTickClock`, advances one tick per
physics step, reads each character's input out of an inbound queue fed by RPC, integrates once, and
publishes state. On a predicting client it owns a `ClientPredictionClock` and a
`NetworkTimeEstimator`, runs *ahead* of the authority, keeps a `StateCorrectionCache` per character
so that any tick it predicted can be re-run, and — when a correction disagrees with what it
predicted — rolls back and replays. **The authority never rewinds, so it needs no history; that one
sentence is the root of almost every asymmetry below.**

⚠ **A listen server is two managers, not one hybrid.** The composition root resolves a *different
instance* per role, and a locally-hosted player's character is registered into both — once through
each overload of `registerSimulatable`. (One adapter's binding for that root:
`ASimulationManagerUImpl::instanceFor(bool isAuthority)`,
`Source/OGBrawlerUnreal/SimulationManagerUImpl.h`.) Nothing in this document describes a manager
that is both roles at once, because no such object exists.

---

## 2. The runs-where matrix

Every row is one named function or phase. `<core>` abbreviates
`Plugins/OGSimulation/Source/OGSimulation/og-simulation/OGSimulation/`; paths are relative to the
repository root.

⚠ **An anchor whose path starts with `Source/` is outside `og-simulation` and is one adapter's
binding, never the binding.** The `<core>` rows are the engine-free core and are what the matrix is
about; a `Source/` row is how *this project's* adapter reaches them, and the host-engine names such
a row carries — replication callbacks, net-mode tests, actor-role enumerators — are one adapter's
vocabulary for those concepts. Another adapter supplies its own and the row's verdict is unchanged.

⚠ **Read the last two columns carefully.** *Predicting client* and *proxy* are **not two machines**.
They are the same manager instance seen from two characters: a character the client controls
(registered with an input provider) and a character it merely observes (registered without one).
A row that names a manager-level or all-characters sweep therefore has the **same** entry in both,
and the column only diverges where a genuine per-character branch exists. Marking those rows
identical is the point — the common mistake is to imagine a separate "proxy path" through the tick.

| # | function / phase | authority | predicting client | proxy | anchor (`file` :: `symbol`) |
|---:|---|:--:|:--:|:--:|---|
| 1 | `onGameSimulation` — the dispatch entry point | ✅ | ✅ | ✅ | `<core>SimulationManager.h` :: `onGameSimulation` |
| 2 | `onGameSimulationAuthority` | ✅ | ⛔ | ⛔ | `<core>SimulationManager.h` :: `onGameSimulationAuthority` |
| 3 | `onGameSimulationPrediction` | ⛔ | ✅ | ✅ | `<core>SimulationManager.h` :: `onGameSimulationPrediction` |
| 4 | `onGameSimulationResimulation` | ⛔ | ✅ | ✅ | `<core>SimulationManager.h` :: `onGameSimulationResimulation` |
| 5 | `preparePredictionSimulationStep` — the collect+allocate door | ⛔ | ✅ | ✅ | `<core>SimulationStepSequencing.h` :: `preparePredictionSimulationStep` |
| 6 | `collectInputAll` | ✅ | ✅ | ✅ | `<core>SimulationInputResolution.h` :: `collectInputAll` |
| 7 | …its **provider** branch (local capture + delay line) | ⛔ | ✅ | ⛔ | `<core>SimulationInputResolution.h` :: `collectInputForCharacter` |
| 8 | …its **remote-queue** branch (dequeue what the RPC delivered) | ✅ | ⛔ | ⛔ | `<core>SimulationQueues.h` :: `dequeueMove` |
| 9 | …its **simulated-proxy** branch (the scheduled relay read) | ⛔ | ⛔ | ✅ | `<core>SimulationInputResolution.h` :: `collectInputForCharacter` |
| 10 | `collectResimInputAll` | ⛔ | ✅ | ✅ | `<core>SimulationInputResolution.h` :: `collectResimInputAll` |
| 11 | `allocateFrontierSlotsAll` — the pair's opening half | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `allocateFrontierSlotsAll` |
| 12 | `integrateAll` | ✅ | ✅ | ✅ | `<core>SimulationIntegrationExecutor.h` :: `integrateAll` |
| 13 | `onPostGameSimulation` | ✅ | ✅ | ✅ | `<core>SimulationManager.h` :: `onPostGameSimulation` |
| 14 | `captureBodyStatesAll` | ✅ | ✅ | ✅ | `<core>SimulationIntegrationExecutor.h` :: `captureBodyStatesAll` |
| 15 | `postPredictionAll` — the pair's completing half | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `postPredictionAll` |
| 16 | `postResimulationAll` | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `postResimulationAll` |
| 17 | `onCheckIsSimilar` — the resim gate | ⛔ | ✅ | ✅ | `<core>SimulationManager.h` :: `onCheckIsSimilar` |
| 18 | `checkDivergenceAll` | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `checkDivergenceAll` |
| 19 | `prepareResimulation` | ⛔ | ✅ | ✅ | `<core>SimulationManager.h` :: `prepareResimulation` |
| 20 | `prepareResimAll` | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `prepareResimAll` |
| 21 | `firstResimStepAll` | ⛔ | ✅ | ✅ | `<core>SimulationIntegrationExecutor.h` :: `firstResimStepAll` |
| 22 | `applyResimAll` | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `applyResimAll` |
| 23 | `consumeResimAnchorsAll` | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `consumeResimAnchorsAll` |
| 24 | `onPostSimulationGameThread` | ✅ | ✅ | ✅ | `<core>SimulationManager.h` :: `onPostSimulationGameThread` |
| 25 | `sendCorrectionAll` — **called** on both, **empty** on the client | ✅ | ∅ | ∅ | `<core>SimulationNetSync.h` :: `sendCorrectionAll`, `m_authorityWriters` |
| 26 | `sendLocalInputToAuthorityAll` — **called** on both, **empty** on the authority | ∅ | ✅ | ∅ | `<core>SimulationNetSync.h` :: `sendLocalInputToAuthorityAll`, `m_localInputSenders` |
| 27 | `setAuthorityGuardContext` | ✅ | ⛔ | ⛔ | `<core>SimulationNetSync.h` :: `setAuthorityGuardContext` |
| 28 | `createCacheFor` — a correction cache is allocated | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `createCacheFor` |
| 29 | `injectCorrectionState` — a correction is applied | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `injectCorrectionState` |
| 30 | `onCorrectionReceived` — **bound** on both, **fires** only on a client | ∅ | ✅ | ✅ | `<core>SimulationNetSync.h` :: `onCorrectionReceived`; `Source/OGBrawlerUnreal/SimmableUpdateComponent.h` :: `OnRep_CorrectionState` |
| 31 | `queueRemoteMove` — inbound RPC lands in the authority queue | ✅ | ⛔ | ⛔ | `<core>SimulationInputResolution.h` :: `queueRemoteMove` |
| 32 | `wipeAllForResync` (resolution peer) | ⛔ | ✅ | ✅ | `<core>SimulationInputResolution.h` :: `wipeAllForResync` |
| 33 | `wipeAllForResync` (reconciliation peer) | ⛔ | ✅ | ✅ | `<core>SimulationReconciliation.h` :: `wipeAllForResync` |
| 34 | `stepAllocatesFrontierSlot` — the shared step-kind predicate | ⛔ | ✅ | ✅ | `<core>SimulationTimeContext.h` :: `stepAllocatesFrontierSlot` |

**Legend.** ✅ runs · ⛔ never reached on this role · ∅ **reached, but structurally empty** — the call
is made and the loop body executes zero times. ∅ is not a synonym for ⛔ and §3 is about why.

⚠ **Row 34 is authority-`⛔` for a reason that is easy to get backwards.** The predicate itself is
a free `constexpr` function with no role in it; it is `⛔` on the authority because *the two call
sites that consult it* — `collectInputForCharacter`'s provider branch and
`allocateFrontierSlotsAll` — are both unreachable there. The authority's step kind is always
`Normal`: `ServerTickClock::advanceTick` produces no `Stall`, `Skip` or `HardResync`.

---

## 3. ⭐ The role gates come in four kinds, and they are not interchangeable

The single most useful thing to know about this system is **how** a role difference is enforced,
because the four mechanisms fail differently.

**G1 — the explicit `m_runsPrediction` branch.** One `if` in `onGameSimulation` and one in
`onPostGameSimulation` (`<core>SimulationManager.h`). This is the *only* gate protecting the
reconciliation sweeps in §5, and it protects them by never calling them.

**G2 — the construction-time optional.** The ctor emplaces `m_clientClock` **or** `m_serverClock`,
never both. Reaching for the wrong one is not a silent misread: `getClientClock()` calls
`std::terminate()` when the optional is empty (`<core>SimulationManager.h`). A role confusion here
is a hard stop by design.

**G3 — the empty-container gate.** The sweep *runs* on both roles and iterates a map that is only
ever populated on one. `sendCorrectionAll` folds over `m_authorityWriters`, inserted only by
`registerAuthorityOwner`, called only from the server `registerSimulatable` overload;
`sendLocalInputToAuthorityAll` folds over `m_localInputSenders`, inserted only in
`registerPredictionOwner`'s provider-present branch, which the server overload cannot take because
it passes a null provider. Both are rows `∅` in §2. **This gate costs one empty loop per tick and
cannot crash** — which is exactly why it is used for the two publish sweeps and not for the cache
sweeps.

**G4 — the per-id filter inside a sweep.** `allocateFrontierSlotsAll` iterates *all* of storage and
skips any id whose correction cache is absent, via the nullable accessor `findInputCache`. This is
the only gate in `SimulationReconciliation` that tolerates a cacheless id **inside** an already-
running sweep, and §5 is about the consequence of that being the only one.

⛔ **A fifth thing that looks like a gate and is not: a bound callback.** `registerPredictionOwner`
binds the correction-state callback **unconditionally**, on both roles
(`<core>SimulationNetSync.h`). On the authority that callback is dead, because the buffer that
drives it is a **replicated property whose change notification fires on the receiving side only** —
and the authority is the *writer* of that buffer, in `sendCorrectionAll`, so its own write never
notifies it. One adapter's binding for that property is
`UPROPERTY(ReplicatedUsing = OnRep_CorrectionState)` on
`Source/OGBrawlerUnreal/SimmableUpdateComponent.h`, whose `OnRep` the engine raises only where the
value arrived; an adapter without receive-side notifications has to establish this differently or
gate the callback itself. The core states the
same property in its own words for the sibling relay-ring callback, in the provider-absent branch of
`registerPredictionOwner`: *"On the AUTHORITY … the callback it binds can never fire there, because
OnRep is a client-only notification."* **Role safety here rests on an engine property, not on a
test in this code.**

---

## 4. The three step functions, and the one line where they differ

All three have the same five-statement shape — resolve inputs, fire pre-integrate, integrate, save
the step, fire post-integrate. Listing them side by side makes the whole difference visible:

| | prediction | authority | resimulation |
|---|---|---|---|
| clock advance | `advancePrediction` | `advanceTick` | `advanceResimulation` |
| step source | `getPredictionStep`, wrapped for `Stall`/`Skip` | `getSimulationStep` | `getResimulationStep` |
| pre-step publish | — | `setAuthorityGuardContext(tick, rollbackWindowTicks)` | — |
| input resolution | `preparePredictionSimulationStep(…)` | `collectInputAll(step)` **direct** | `collectResimInputAll(tick)` |
| frontier slot | **allocated** | never | never |
| systems + integrate | identical | identical | identical |

Two things are worth saying out loud.

**The prediction path is the only one that synthesises step kinds.** `advancePrediction` can return
`Stall`, `Skip` or `HardResync`, and the prediction step function wraps the base step into a
`SimulationTimeStep` carrying that `StepKind` (`<core>SimulationTimeContext.h` :: `StepKind`). The
authority's clock has no such notion: `ServerTickClock::advanceTick` is an increment, and
`ServerTickClock::getSimulationStep` constructs its step with a literal `StepKind::Normal`
(`<core>PCTimeManagement/ServerTickClock.cpp`). So `Stall` and `Skip` handling is prediction-only
code even though the types are shared.

⛔ **The authority must NOT be routed through `preparePredictionSimulationStep`, and the name says
so.** That facade always pairs `collectInputAll` with `allocateFrontierSlotsAll`; §6 is why the
second call must never happen on the authority. The fence stating this lives at both ends — on the
facade's own banner in `<core>SimulationStepSequencing.h` and at the authority call site in
`<core>SimulationManager.h`. This paragraph is the narrative; those two comments are the guard, and
moving the guard here would disarm it.

---

## 5. ⛔ Why there is no correction cache on the authority

The short answer — *the authority never rewinds, so it needs no history* — is true and is not the
useful half. The useful half is that **the absence is a precondition, not an optimisation.**

`SimulationReconciliation` runs **nine** sweeps of the form
`m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable){ … })`. Exactly **one** of
them — `allocateFrontierSlotsAll` — checks whether the id it is looking at actually has a correction
cache. The other eight reach the cache through `getCacheFor`, whose entire body is
`std::get<CacheMapFor<T>>(m_caches).at(id)` — a bare `std::unordered_map::at`, which **throws** on a
missing key. (Counted 2026-08-21 by
`grep -c 'm_storage.forEachSimulatable' SimulationReconciliation.h`; the filtered one is identifiable
by the literal `findInputCache<T>(id) == nullptr`.)

⇒ **If any of those eight ran on the authority, it would throw on the first character.** They do not
run there because of gate **G1** alone — one `if (m_runsPrediction)` in `onPostGameSimulation`, plus
two early returns on `runsPrediction()` in the adapter's own physics callback: the one that decides
whether to ask for a rewind, and the one that runs on the first replayed step.
(One adapter's binding: `FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal`
and `FSimulationManagerAsyncCallback::FirstPreResimStep_Internal`, both in
`Source/OGBrawlerUnreal/SimulationManagerUImpl.cpp`.) There is no second line of defence inside the
sweeps themselves, and that is a deliberate, stated position rather than an oversight: the audit
that considered adding a capture-side check and **rejected** it is recorded above `postPredictionAll`
in `<core>SimulationReconciliation.h`.

Two consequences a reader should carry away:

1. **"The authority has no cache" and "the authority does not resim" are the same fact stated at two
   different layers.** The server `registerSimulatable` overload simply does not call
   `createCacheFor` — its `reconciliation` parameter is named `/*reconciliation*/` and unused
   (`<core>SimulationNetSync.h`). ⚠ Read the direction carefully: that omission is what makes the
   cache-touching rows of §2 **unsafe** on the authority; what makes them `⛔` is the G1 role gate
   that never calls them. The two facts hold each other up, and neither alone is the guard.
2. ⭐ **A proxy DOES have a correction cache.** The client `registerSimulatable` overload calls
   `createCacheFor` unconditionally — before it has looked at whether an input provider was supplied.
   So a character a client merely observes is corrected, checked for divergence, and replayed exactly
   like the one it controls. The provider is what decides where its *input* comes from
   (§2 rows 7 and 9), not whether it is reconciled. Reading "prediction" as "only my own character"
   is the single most common way to mis-read this system.

---

## 6. Which step function can allocate a frontier slot — and why only one

**Answer: `onGameSimulationPrediction`, and only through `preparePredictionSimulationStep`.**

The frontier pair is *allocate a slot for tick N, then write tick N's state into it*. Its opening
half is `allocateFrontierSlotsAll` → `pushPredictionTick`; its completing half is `postPredictionAll`
→ `pushPredictionState`. The cache holds a flag, `m_frontierSlotAwaitingState`
(`<core>CorrectionCache.h`), that detects an opening with no completion.

Neither of the other two step functions allocates, and for two *different* reasons:

- **The authority** calls `collectInputAll` directly and never calls `allocateFrontierSlotsAll` at
  all. Even if it did, the sweep's G4 filter would skip every id, because no authority id has a
  cache (§5). The header states the stronger form of this: collect, allocate and capture "all agree
  on authority by all three being absent, not by one completing another"
  (`<core>SimulationManager.h`, above the authority collect call).
- **The resimulation path** replays ticks whose slots already exist. `collectResimInputAll` resolves
  against them via `getAppliedCaptureTickRef`; the replay's state write goes through
  `postResimulationAll`, which targets the *resim tick's* slot rather than the frontier. A replay is
  an overwrite of allocated history, never an extension of it.

Within the prediction path the allocation is further gated by step kind, through the single shared
predicate `stepAllocatesFrontierSlot(kind)`, whose body is `return kind != StepKind::Stall;`:

| `StepKind` | delay-line push | frontier slot | back-fill |
|---|:--:|:--:|:--:|
| `Normal` | ✅ | ✅ | — |
| `HardResync` | ✅ | ✅ | — |
| `Skip` | ✅ | ✅ | `backfillSkippedTick(tick − 1, …)` |
| `Stall` | ⛔ | ⛔ | — |

⛔ **Both halves of the pair consult that one predicate, deliberately.** `allocateFrontierSlotsAll`
and `postPredictionAll` each call `stepAllocatesFrontierSlot` rather than testing
`== StepKind::Stall` themselves, so a future `StepKind` cannot make two independently-written gates
disagree. The fence saying so is at both sites.

---

## 7. The resim cycle — six phases, all client-side

    checkDivergenceAll → prepareResimAll + firstResimStepAll → replay → postResimulationAll
      → applyResimAll → consumeResimAnchorsAll

Every one of those is `⛔` on the authority (§2 rows 16–23). The cycle is driven from the physics
thread through the adapter's rewind hooks — the callbacks by which a host physics engine asks the
core whether to rewind and then drives the replay (one adapter binds these to Chaos's rewind hooks)
— and the manager's three entry points map onto it as follows:

| phase | manager entry | reconciliation / executor call |
|---|---|---|
| ask | `onCheckIsSimilar` | `checkDivergenceAll(maxAnchorDepthTicks, &deepSkips)` |
| arm | `prepareResimulation` | `prepareResimAll(simTick)` + `firstResimStepAll(chaosStep)` |
| replay | `onGameSimulationResimulation` (once per replayed tick) | `collectResimInputAll` + `integrateAll` |
| record | `onPostGameSimulation`, resim branch | `postResimulationAll` |
| land | `onPostGameSimulation`, catch-up edge | `applyResimAll` |
| close | `onPostGameSimulation`, same edge, **after** land | `consumeResimAnchorsAll` |

Three properties are worth carrying, each stated fully at its own site:

- **The ordering of *land* then *close* is load-bearing.** The anchor is consumed only once the
  replayed state has been published, and only on the completion edge — because a substantial share of
  prepares never reach that edge, and an anchor consumed at prepare would take its correction with
  it. The full argument is above the `consumeResimAnchorsAll` call in `<core>SimulationManager.h`.
- **A replay tick writes state only.** `postResimulationAll` touches no gate state; the gate closes
  once, explicitly, at `consumeResimAnchorsAll`'s compare-and-swap. That is what makes a completed
  resim structurally unable to re-trigger itself, and the fence saying so is above
  `postResimulationAll`.
- ⛔ **There is deliberately no trigger cooldown.** A rate ceiling stood on the line after
  `checkDivergenceAll` and was removed by ruling; what throttles the rate instead is structural —
  at most one resim in flight and at most one more pending. The full statement is at that line in
  `<core>SimulationManager.h`, and this document does not restate its reasoning, only its existence.

---

## 8. Registration and teardown ordering — the two live crashes items 92 and 93 fixed

Registration is not a role difference so much as the place where the role differences are *installed*,
and it is where the two most recent production defects in this area lived.

The composition root picks the overload from the machine's net mode: `record.isAuthority`, decided
at the component from whether this machine is a client. (One adapter's binding:
`ASimulationManagerUImpl::tryRegister`, deriving it from `GetNetMode() != NM_Client`.)
On a client, the input provider is built only for a character this machine actually controls —
in one adapter's vocabulary, an actor whose local role is `ROLE_AutonomousProxy`
(`const bool isLocallyPredicted`, `Source/OGBrawlerUnreal/SimmableUpdateComponent.cpp`) — that is the
one place the local-vs-proxy distinction of §2's last two columns is decided.

**The invariant, in one line: registration publishes LAST; unregistration unpublishes FIRST.**

| overload | order | invariant established |
|---|---|---|
| client `registerSimulatable` | `createCacheFor` → `storage.add` → `registerPredictionOwner` | *if storage has id, cache has id* |
| server `registerSimulatable` | `registerPredictionOwner` → `registerAuthorityOwner` → `storage.add` | *if storage has id, queueMap has id* |
| `unregisterSimulatable` | `storage.remove` → `netSync.unregisterSimulatable` → `removeCacheFor` | the same, read backwards |

**What crashed without it.** `storage.add` is the single point at which an id becomes visible to
`forEachSimulatable`, and the physics thread runs those sweeps concurrently with game-thread
registration. Before the fix, the server overload added to storage *before* populating the
remote-move queue: a physics tick landing inside that window saw a storage-exposed id, followed the
frontier-allocating path, and hit a bare `.at(id)` on a cache that does not exist on the authority at
all — an unhandled MSVC C++ exception one physics tick after the character appeared. The
unregistration path was the same defect mirrored: the id stayed visible in storage while the maps
gating what a sweep did with it had already been erased, reachable on the authority on player-leave.

⚠ **The ordering is belt-and-braces rather than load-bearing for its original reason, and must not
be removed on the grounds that the crash it fixed can no longer happen.** The frontier-allocating
sweep has since moved to `SimulationReconciliation` and acquired the G4 filter, so *that specific*
crash shape is retired independently. What the ordering still protects is the **branch dispatch** in
`collectInputForCharacter`: an id exposed to storage before its remote-move-queue entry exists would
be classified as a simulated proxy instead of an authority id for the width of the window, and would
read a relay store that no authority path populates. Lower severity, still real. The fence
carrying this — including the explicit *"a future author must not remove this reorder"* — sits above
`unregisterSimulatable` in `<core>SimulationNetSync.h`.

⚠ **One artefact of the ordering that reads like a bug and is not.** On the authority, the server
overload reaches `registerPredictionOwner` with a null provider, which takes the *provider-absent*
branch and calls `registerRemoteCharacter` — allocating a `RemoteInputCache` for a character on the
one role that never reads one. `collectInputForCharacter` tests the remote-move queue before it
falls through to the proxy branch, and every fully-registered authority id has a queue entry, so the
store is created and never consulted.

---

## 9. Two facts about the resim trigger policy, and they are not the same fact

⛔ **Do not collapse these.** A single sentence conflating them is on record as this codebase's
canonical documentation defect, and it survived for weeks.

**Fact one — the code default.** With no configuration override, the compiled default is
`FrontierExact`. Read from `<core>PCTimeManagement/TimeConfig.h` on 2026-08-21, line 443, verbatim:

```cpp
	ResimTriggerPolicy resimTriggerPolicy = ResimTriggerPolicy::FrontierExact;
```

**Fact two — the shipped configuration.** The value is overridden in the host application's
configuration, which for one adapter is `Config/DefaultEngine.ini` under the `[OGNetcode]` section
header, at the `ResimTriggerPolicy` key. Read on 2026-08-21, verbatim:

```ini
ResimTriggerPolicy=OnDisagreement
```

The ini value reaches the core through `SimulationManager::setResimTriggerPolicy`, which pushes it
down to every `StateCorrectionCache`. So **a session run from this repository runs `OnDisagreement`,
while a build with the ini key deleted runs `FrontierExact`** — and the two questions *"what does the
code default to?"* and *"what does a session actually run?"* have different answers by design. The
compiled default is deliberately left at the legacy value so that removing one ini line restores the
old behaviour with no rebuild; the statement of that intent is at the field's own declaration.

⚠ **The flip is not one bit.** `resimGate::policyEnforcesDepthCeiling` (`<core>ResimGatePolicy.h`)
returns true for `OnDisagreement` only, so selecting it also **arms the anchor depth ceiling** —
`TimeConfig::rollbackWindowTicks`. An anchor deeper than that below its character's frontier is
skipped and counted rather than clamped. Under `FrontierExact` the ceiling is inert and that counter
reads a structural zero. The derivation of record is at `policyEnforcesDepthCeiling` itself and in
`ResimGatePolicy-rationale.md`; it is not re-derived here.

---

## 10. Dated observations — closed tense, on purpose

These are measurements and review findings, not claims about the tree's present state. They are
written as of their dates so that they cannot go stale; a reader who needs the current value must
read the code, and the anchors above tell them where.

- **2026-08-16, architecture review of the `og-netcode-v2-input-relay` initiative:** the resim gate
  was built on **Layer 2** of a two-layer desync design. Layer 1 — a per-tick CRC32 hash, the
  intended *detector* — was never built, so the correctness verdict driving `checkDivergenceAll` is a
  similarity heuristic (`isSimilarTo`) rather than a hash comparison. ⚠ **This is a property of the
  verdict, not of the gate**, and the two are frequently confused when reading resim rates.
- **Same review, same date:** with `OnDisagreement` selected, resims were observed firing on
  effectively **every** correction landing — because the verdict above is degenerately
  always-disagree, not because the gate is wrong. A resim count near the landing count is therefore
  the *expected* reading under that configuration.
- **2026-08-20:** the authority-path registration and teardown ordering described in §8 was
  established in response to **two live crashes**, one on each half of the lifecycle. Before that
  date the initiative had never observed a `EndPlay`-reaching session at all; the teardown fix is the
  first one with runtime evidence behind it.
- **2026-08-21:** `SimulationReconciliation.h` carried nine storage-driven sweeps, of which one
  filtered on cache presence (§5).

---

## 11. What this document does not answer

Stated plainly rather than papered over.

- **What a listen server actually does per tick.** §1 states that two manager instances exist and
  that a locally-hosted player registers into both. The interleaving — which instance's physics tick
  runs first, and what the local player's character looks like from the authority instance's
  simulated-proxy machinery — is not described here and was not read for this document.
- **Whether any correction ever reaches the authority.** §3 argues the correction callback is bound
  on the authority and cannot fire, from the `ReplicatedUsing` declaration plus one adapter's
  receive-side-only notification semantics. That is a sound reading of the declaration, **not** a
  runtime observation; no log or
  test was consulted for it.
- **The cost of the empty G3 sweeps.** Two per-tick folds over empty maps are asserted to be cheap.
  No measurement is recorded here, and a number without a run date and a config would be worse than
  its absence.
- **What the `Skip` back-fill costs a proxy.** §6 records that `backfillSkippedTick` runs on a
  `Skip` step, and nothing here says how often a predicting client actually produces one.
- **Whether the eight unfiltered sweeps of §5 should be filtered.** This document reports the count
  and the single gate protecting them. Whether that is the right trade is a design question the
  header's own rejection audit answers for one of the eight and leaves open for the rest.

---

## 12. Keeping this document true

The rule this document follows: **a statement about the current state of code or config carries a
grep-able `file` + `symbol` anchor, or is written closed-tense with a date.** §10 is the closed-tense
half; §2's matrix is the anchored half.

⚠ **Closed tense is necessary but not sufficient, and this document assumes the weaker claim.** A
dated past-tense sentence protects itself from decay — but it does *not* protect the symbols it
names, which can still be renamed out from under it. Every symbol in §2 and §10 is therefore in the
anchor set below, dated sentences included.

**The falsifiable part.** Every `file` :: `symbol` pair this document names resolves to a
**non-comment** line in that exact file. Verified 2026-08-21: **113 pairs checked, 0 failures**,
against a checker that also runs six negative controls — five dead-or-misfiled citations and one
config key that exists only on a commented-out line — all six of which it rejects. A symbol that
survives only in prose is not a citation, and a check that cannot tell the difference is not a check.

Three consequences for anyone editing this file:

1. **If you rename a symbol, this file breaks loudly.** That is the intent. The matrix is a list of
   grep targets, not prose.
2. **Do not move a fence into this file.** Everything marked ⛔ above is a *summary* of a guard that
   stays at its site, and each such paragraph names the file the guard lives in. If you find yourself
   deleting a comment because "it is in the perspective doc now", the guard has stopped working —
   the person about to make the change is reading code.
3. **If this file and a header disagree, the header wins.** Fix this file; do not soften the header.
