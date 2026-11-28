<!-- SPDX-License-Identifier: MPL-2.0 -->
<!-- lint-external-ref: m_requiredInputDelayIncreaseStallTicks -- the debt's plain unsigned member until og-input-latency P6-2 made it the signed atomic m_pendingInputDelayTransitionTicks; kept in row 12's history -->
<!-- lint-external-ref: ASimulationTimingRelay::OnRep_Buffer -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: ServerReceiveRemoteMove -- deleted by og-input-latency task P5-8 (the net-sync component, the pawn's subobject, the bundle builder and the owner binding's net half); kept in the history -->
<!-- lint-external-ref: SimulatableNetSyncComponent.h -- deleted by og-input-latency task P5-8 (the net-sync component, the pawn's subobject, the bundle builder and the owner binding's net half); kept in the history -->
<!-- lint-external-ref: PCTimeManagement/ChaosTickMapper.h -- deleted by og-session-runtime task 4 (OGSimulationUnreal's Chaos adapters, factory, tick mapper, time context, the Chaos engine traits and their two docs); kept in the history -->
# GT↔PT crossing inventory

This is the one place every crossing between threads in `og-simulation`'s data structures is enumerated.
It exists because the accepted-debt argument each individual crossing makes — "inherited, not
introduced, this store widens nothing" — is structurally unable to say no to the next one
(`ArchitectureReview_Fable.md` B4). A single inventory turns "the class's size" from a feeling into
a number someone owns.

Origin: `ArchitectureReview_Fable.md` B4 (recommendation + the `[pass 2]` correction), B5's
description-gap residual, `og-netcode-v2-input-relay` backlog item 81. This doc is an **index, not
a relocation**: each row below points at the crossing's own in-file THREADING block, which stays
exactly where it is. Local truth remains local; do not copy prose out of a THREADING block into
this file, and do not let this file's summary drift from it — if they disagree, the in-file block
is authoritative and this row is stale.

> ⚠ **`ArchitectureReview_Fable.md` and `DesignInputResolutionPeer.md` are private working material
> from the `og-netcode-v2-input-relay` initiative archive and are NOT distributed with this
> submodule.** They are named as provenance, deliberately unlinked. The declarations below tell
> `tools/lint/doc_anchor_lint.ps1` that both names are intentionally unresolvable, so it enumerates
> them with this reason rather than reporting them as drift.
>
> ⚠ **Retired name, kept on purpose.** Rows 1 and 7 name `prepareSimulationStep`. That identifier is
> **gone from the tree**: it was `collectInputAll`'s name between backlog items 90 and 94 only, and
> `collectInputAll` is what exists today (`SimulationInputResolution.h`). It is kept in those rows
> because a reader holding a build from that window needs the mapping; it is declared below so the
> anchor lint reports it as a marked retirement instead of as a broken anchor.

<!-- lint-external-ref: ArchitectureReview_Fable.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: DesignInputResolutionPeer.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: prepareSimulationStep -- RETIRED NAME: collectInputAll carried it between items 90 and 94 only; it became collectInputAll again and must not resolve -->
<!-- lint-external-ref: SimmableUpdateComponent.h -- RENAMED by og-ue-simulationcomponentandmanagersplit task 21a to OGBrawlerSimulatableComponent.h (one adapter's per-character component header); kept in the item-83 completeness-sweep record -->

## The standing rule

> **Any new crossing between threads adds its row here, and prefers the staged SPSC pattern** —
> `RemoteMoveQueue` and `PendingInputQueue` (both `SimulationQueues.h`) are the existing instances:
> a bounded ring, one producer thread, one consumer thread, drained at a named point in the tick.
> **"Inherited, not introduced" is not an argument this doc accepts for a new *data* crossing.** A
> new crossing that reaches for the inherited-tear shape instead of the SPSC seam needs its own
> stated reason, reviewed like any other architectural exception — not a citation of the rows
> below.

This rule governs new crossings going forward. It does not retroactively obligate every row below
to move — that is item 82's (gated) call, not this doc's.

## Data crossings vs. control crossings

`[pass 2]` of B4 sharpened the finding the first pass overstated. The first pass read the
accepted-debt argument as "structurally incapable of ever saying no" — too strong: item 45 *removed*
a crossing from the class by promoting the resim gate to one correctly-synchronized atomic word.
The accurate shape, confirmed against every row below:

> **Data crossings ratchet** — accepted, and accumulate, because a torn *value* is
> correction-healed at the next landing. **Control crossings get fixed** — a lost *trigger* is not
> healed by anything downstream, so the codebase does not tolerate one. The gate atomic (row 6) is
> the demonstration: when a bit decided something and its race was real, it was fixed, not
> inherited-and-accepted.

A crossing carrying a *decision* — not just a value a later frame supersedes — is a control
crossing, or a data crossing with a control-shaped bit riding inside it (row 5 is exactly the
latter: `m_containsCorrectTick` is bitset storage, priced as a torn *value*, but the bit it stores
is read as a decision). Classify a new crossing by that question, not by which struct it happens to
live in.

## The twelve crossings

⚠ **Extended by item 83 (2026-08-18).** Rows 7–10 were missing from the original six-row landing —
row 7 and row 8 were named by review (item 81 finding 1); rows 9 and 10 surfaced from this task's
own completeness sweep (below), which the original six-row pass did not run. See "Completeness
sweep (item 83)" for what was checked and how these two were found.

**Every row is one thread today, except one reader.** A row names a member, the side that writes it and the side that
reads it: the **feed** side (a correction landing, a relayed ring, a tier or floor receipt, a clock sample, a
registration, an input's arrival or release, a send) and the **step** side (the collects, the sweeps, the clock's
advance). The rows' GT and PT are those two sides. Each row's member is written and read on one thread, the step's,
because every feed runs there: a client's in its drain at the start of each step, an authority's in its drain, its
release and its post-batch send on the thread that steps it (a client's teardown drain runs once more after its last step, while no step runs). So a row's crossing occurs only if a feed runs on another
thread than the step, and each row prices that case. The one live crossing is a reader: the clock-dilation stage
(`clockDilation::beforeClientPump`) reads the estimator on the frame thread before the pump (rows 9 and 10). Until
og-session-runtime task 13 (the client) and task 14 (the server) a frame-locked client and the in-frame server ran their
feeds on the host's game thread, and these rows were that mode's accepted debt (og-input-latency P2-15 added the client
that drains at step start; P5-22 made it the default). Who calls what on which thread is the Unreal session's thread
table, og-sim-session's `SessionThreads-rationale.md` §7 (the table of `SessionThreads.h`, which checks it); the rows name og-simulation's members only,
and a host name in a row is history. The former Chaos adapter's two physics callbacks (removed by og-session-runtime
task 3), which row 11's history names, were its pre-simulate and post-solve callbacks, with the solve between them.

| # | Crossing | Direction | Shape | In-file THREADING block |
|---|----------|-----------|-------|--------------------------|
| 1 | Relay stores (`m_remoteInputCaches`) | feed write (the relayed-ring arrival callback `registerPredictionOwner` installs → `populateRemoteInputCache`; an OnRep until og-input-latency task P5-7) → step read (`collectInputAll`, `collectResimInputAll`; `collectInputAll` named `prepareSimulationStep` between item 90 and item 94) | Data — torn-slot-bounded, correction-healed. One thread today; on two it is the accepted debt the frame-locked client carried until og-session-runtime task 13 | `Network/RemoteInputCache.h` — the "THREADING -- ONE THREAD, BY THE HOST'S CONTRACT, NOT BY CONSTRUCTION" block, whose fences name the SPSC fix a feed on another thread would need (`RemoteInputCache-rationale.md` §6) |
| 2 | Correction inject path (`SimulationReconciliation::injectCorrectionState` → `tryInsertingCorrectState`) | feed write → step read of `m_stateBuffer` | Data — correction-healed. One thread today; on two it is the accepted debt the frame-locked client carried until og-session-runtime task 13 | `SimulationReconciliation.h` — the ORIENTATION's thread map ("ONE THREAD TOUCHES THIS CLASS"); `CorrectionCache.h` — the ORIENTATION's thread table and the item 47 landing-stamp THREADING block's first bullet ("RIDE THE PRE-EXISTING STATE-BUFFER RACE") for the race itself |
| 3 | `m_lastUsedCaptureTicks` | step write (`collectInputAll`) → feed read (`sendCorrectionAll`, the authority's send) | Data — correction-healed (opposite direction from 1/2/4/5 — the step is the writer here). One thread today, the authority's step thread; the in-frame server's send read it on the game thread until og-session-runtime task 14 | `SimulationInputResolution.h` — the member's own declaration comment ("WRITTEN IN collectInputAll, READ IN sendCorrectionAll"); moved off `SimulationNetSync.h` at item 87 |
| 4 | The ±1 frontier read in `decideCorrectionArrival` | feed read of a frontier the step advances (`pushPredictionTick`) | Data — self-bounded (±1 per sample, does not accumulate), not correction-healed because it mis-files a probe sample rather than mis-deciding a write. Exact on one thread, as today | `SimulationNetSync.h` — the "READ AFTER THE INSERT — SAFE, AND ±1 RACY OFF THE STEP'S THREAD" block in `decideCorrectionArrival` |
| 5 | `m_containsCorrectTick` (the write-protect decision bit) | feed `set()` (`tryInsertingCorrectState`) ↔ step clear (`pushPredictionTick`'s push path) | **Control-shaped bit inside a data crossing** — on two threads both are RMWs of the same `std::bitset` word and the failure mode is a lost update, not a torn read (item 81's correction — see below). One thread today, so the `set()` and the clear are serialized and no update is lost | `CorrectionCache.h` — item 47's header block above `tryInsertingResimulatedState` (the honest-bound note), and the landing-stamp THREADING block's second bullet, both in the private section near `m_slotLandingSeqNr` |
| 6 | The resim gate atomic (`m_pendingResimAnchorTick`) | feed CAS-max set (`tryInsertingCorrectState`'s hit path) / step CAS clear (resim-completion edge) / wipe | **Control — correctly synchronized**, and the only one of the five correctly-synchronized crossings (rows 6, 7, 9, 10, 12) with true multi-writer CAS coordination — rows 7 and 9 are each a single-writer atomic scalar, and rows 10 and 12 are single-writer and single-counter atomics. One `std::atomic<uint32>`, CAS on every writer, no RMW race | `CorrectionCache.h` — the item 45 "THE RESIM GATE — ONE WORD, EDGE-TRIGGERED" block above `getPendingResimAnchorTick` |
| 7 | `m_clientEffectiveInputDelayTicks` | feed write (`setClientEffectiveInputDelayTicks`, from the tier receipt, for an arrival and for the listener's replay at its bind) → step read (`collectInputAll`, `collectResimInputAll`, once per tick each; `collectInputAll` named `prepareSimulationStep` between item 90 and item 94) | **Correctly synchronized** — `std::atomic<int32>`, relaxed ordering, a lone independent scalar with no internal structure (a stale read applies a tier change one tick late, nothing else). One thread today; the rest of the tier receipt's state (the tier consumer, the runtime relay floor, the clock's input-delay transition debt, row 12) runs with it, and display readers read og-input-latency P2-18's display snapshot. A SECOND correctly-synchronized crossing — item 81's original row 6 called the gate atomic "the sole" one; that claim was false the day it was written, since this member already carried its own THREADING comment in the same file three of the doc's rows (1, 3, 4) already point into | `SimulationInputResolution.h` — the "FEED SIDE -> STEP SIDE" fence above `setClientEffectiveInputDelayTicks` and the member's declaration (both moved off `SimulationNetSync.h` at item 87; the old forwarder was deleted, not kept) |
| 8 | `m_currentAuthorityTick` / `m_rollbackWindowTicks` | step write (`setAuthorityGuardContext`, from `onGameSimulationAuthority`) → feed read (the remote-move lambda bound in `registerAuthorityOwner`, at an input's arrival and at its delayed release; until og-input-latency task P5-7 also from the `ServerReceiveRemoteMove` RPC, deleted by P5-8) | **Data — unsynchronized.** Plain `uint32`/`int32`, not atomic. One thread today, the authority's step thread; the in-frame server read them on the game thread until og-session-runtime task 14. On two threads the direction is step→feed — the same reversed shape as row 3, not row 3 itself — and the honest hazard is *staleness*, not tearing (a naturally-aligned 4-byte word is a single-bus-cycle op on x86-64): an arrival may see an authority tick up to one step old, which the member's own comment prices as acceptable ("an at-most-one-tick-stale value is fine for a multi-tick rollback window"). It gates an accept/reject decision (too-far-future capture-tick rejection), so it is control-*flavored* data, but unlike row 5 there is exactly one writer, so no lost update is possible. Not correction-healed by a later landing (unlike rows 1–4); self-heals by being overwritten every authority tick instead | `SimulationNetSync.h` — the members' own declaration comment above `m_currentAuthorityTick` |
| 9 | `NetworkTimeEstimator::m_authorityTick` | step write (`recordAuthorityTick`, from the clock feed at step start; one sample per feed, the newest, since og-input-latency task 27; from `ASimulationTimingRelay::OnRep_Buffer` until og-input-latency task P5-7) → step read (`getLastAuthorityTick`, through `ClientPredictionClock::readTarget` in `advancePrediction` and `evaluateDrift`) and **frame-thread read** (`getLastAuthorityTick` in the clock-dilation stage) | **Correctly synchronized** — `std::atomic<unsigned int>`, explicit, one writer. A THIRD correctly-synchronized crossing, found by item 83's sweep, not by item 81's review. The step read is same-thread; the frame-thread read is the live crossing the atomic synchronizes. Until og-session-runtime task 13 a frame-locked client wrote it on the game thread | `PCTimeManagement/NetworkTimeEstimator.h` — the class's own "THREAD-SAFETY CONTRACT" banner |
| 10 | `NetworkTimeEstimator::m_smoothedRTT` / `m_smoothedJitter` / `m_hasFirstSample` / `m_measuredRTTSamples` | step write (`updateRTT`, from the clock feed) → step read (`getPredictionOffsetTicks` and `hasFirstRTTSample` through `ClientPredictionClock::readTarget`, which loads `hasFirstRTTSample` once, then `getPredictionOffsetTicks` once, and adds `getLastAuthorityTick`, from `advancePrediction` and `evaluateDrift`; `getMeasuredRTTSampleCount`, `getSmoothedRTT` and `getPredictionOffsetTicks` from the same function's one-shot re-centre, og-input-latency task 27 and its rework 1; `hasFirstRTTSample` from its resync onto the first RTT reading, og-input-latency P3-10) and **frame-thread read** (`getSmoothedRTT`, `getSmoothedJitter`, `activeJitterMultiplier` and `getPredictionOffsetTicks` in the clock-dilation stage). P3-10's source-switch and SimClock-shadow members are plain and writer-thread only: no new crossing. Its `m_afterFirstSampleLoad` is a test hook that `hasFirstRTTSample` runs after its load: it is empty in shipped code and set only by a friend probe that only og-simulation-tests defines, so it is no crossing | **Correctly synchronized** — `std::atomic<double>` / `std::atomic<double>` / `std::atomic<bool>` / `std::atomic<unsigned int>`, one writer, `memory_order_relaxed` on every access (og-input-latency P2-14). A FOURTH correctly-synchronized crossing, counted with rows 6, 7 and 9: four, not three. Each member is coherent on its own, but the members are not a snapshot: a read on another thread than the writer's may combine values written by different `updateRTT` calls (e.g. the first-sample flag already true while the seed is not yet visible, which yields the floor). The step reads are same-thread today, so a mixed read cannot happen there; the frame-thread read is the live crossing, and its mix is priced as data (one wake's inputs, superseded by the next read). The clock's one-shot resync onto the first RTT reading (P3-10), which a mixed read would cancel for that join (review R1), reads the flag once per advance, before the offset, and decides only on an advance whose previous advance already read the flag true (`TimeConfig-rationale.md` §16; an acquire/release pair was the alternative, not taken). The old platform assumption (a naturally-aligned 8-byte access cannot tear on x86-64) and its stated limit ("if this class is ever ported to ARM or compiled with unaligned-access enabled, wrap these fields in `std::atomic<double>` as well") are replaced by a `static_assert(std::atomic<double>::is_always_lock_free)` in the class, whose message names that caveat. On x86-64 a relaxed atomic is the same plain load and store the fields used before. Until og-session-runtime task 13 a frame-locked client wrote them on the game thread | `PCTimeManagement/NetworkTimeEstimator.h` — the same "THREAD-SAFETY CONTRACT" banner |
| 11 | Storage exposure at registration/teardown (`storage.add` / `storage.remove`) | **Both directions named.** lifecycle write — `storage.add` through `registerSimulatable`, `storage.remove` through the `unregisterSimulatable` facade — ↔ step read/iterate, the `forEachSimulatable`-driven sweeps: `allocateFrontierSlotsAll`, `postPredictionAll`, `collectInputAll` | **Data — one thread today, its FATAL consequence removed.** Every production registration in this tree goes through a `DeferredLifecycleQueue` (og-input-latency P2-17), applied at its adopter's drain, before any inbox record, through `registerSimulatable` and `unregisterSimulatable` in their published order (publish last, unpublish first), with the adopter's own channel as the owner. That drain runs on the step's thread, and so do both sends that walk the sender maps (og-input-latency P2-16 for a client, which sends at capture; the authority's post-batch send), so storage, the cache, the resolver's containers and NetSync's three maps (`m_authorityWriters`, `m_localInputSenders`, `m_redundancyCadences`) have one writer and their sweeps and walks run on its thread; both ends of each sender's `PendingInputQueue` (the push in `collectInputAll`, the pop in the send: `forEachRecent`, `releaseAllButRecent`) too. The display readers read a display snapshot (og-sim-session's `SimulationDisplaySnapshot.h` in this tree, og-sim's until og-session-runtime task 25; og-input-latency P2-18 and P5-16), not the containers. A roster Remove of a queue-registered character unregisters it inside the drain before its channel is destroyed. The adopters are og-sim-net's sessions (`SimNetClient-rationale.md` §18, `SimNetServer-rationale.md` §15); the queue's semantics are pinned by tests in og-simulation-tests, the sessions' use of it by og-sim-net's `SimNetLifecycleQueueTest.cpp`. **History.** Until og-session-runtime tasks 13 and 14 a frame-locked client and the in-frame server registered on the game thread, unsynchronized against the step: the accepted debt this row priced. og-netcode-v2-field-defects task 3 changed three things there, and the first two still stand. (1) The ORDER: the client `registerSimulatable` overload publishes to storage LAST and `registerLocalCharacter` creates the delay line BEFORE the provider, so a sweep that reaches an id finds every container that id's branch will ask for. (2) The CONSEQUENCE: `collectInputForCharacter` and `collectResimInputForCharacter` look the delay line up with `find` and answer the injected neutral on a miss, behind a `NOCACHE` warning rate-limited to one line per tick, so a tear costs one logged tick instead of the process — it was a `std::out_of_range` thrown out of a Chaos step on a shipped client. (3) The queue itself, then HELD: ⛔⛔ **adopting it on a client whose sends ran on another thread than the apply INVERTS this row instead of closing it, and that is a ruling** — the step's thread becomes the sole writer while the other thread's send ITERATES `m_authorityWriters` and `m_localInputSenders` and inserts into `m_redundancyCadences` (`operator[]` on a sender's first send; `unregisterSimulatableContainers` step 2 erases it beside the two sender maps, og-input-latency task 7). Its precondition, moving or snapshotting those readers first, was met by P2-16, P2-18 and P5-16, and the queue was adopted on a client in P5-17 and on the authority loop in P5-19. ⚠ The core cannot enforce adoption: `registerSimulatable` is a free function any consumer may call from any thread, and nothing in og-simulation checks which one. Not newly found — audited and priced by item 94's D-2 (`DesignInputResolutionPeer.md`, "D-2, AUDITED AND REJECTED AT ITEM 94"), whose rejection of a proposed capture-side pairing check rests entirely on this crossing; that section is the analysis of record for this row. **The window, for a caller on another thread, is wider than a single callback**: the two step sweeps bracket the physics solve. On the tick driver they run inside one step of `SimulationStepDriver::runTick` — `onGameSimulation` (sweep 1 + `allocateFrontierSlotsAll`), the world step, then `onPostGameSimulation` (`postPredictionAll`) — so a registration or teardown on another thread can land anywhere across that whole span. The former Chaos adapter ran them in two separate physics callbacks (in the brawler's Chaos host since og-simulationscheduler-withjolt task 82), with the same solve between them | `SimulationNetSync.h`, the client `registerSimulatable` overload (the "PUBLISH LAST — createCacheFor BEFORE storage.add" guard) and the `unregisterSimulatable` facade (item-94 Part F's re-checked mirrored-invariant paragraph); `DeferredLifecycleQueue.h`'s banner for the precondition and the ruling; local text stays authoritative, this row indexes it, not the other way around |
| 12 | `ClientPredictionClock::m_pendingInputDelayTransitionTicks` (the input-delay transition debt; og-input-latency P6-2) | feed request (`requestInputDelayTransition`'s `fetch_add`, from the floor and tier receipts, for an arrival and for the listener's replay at its bind) ↔ step read and consume (`advancePrediction`: one tick toward 0 per call by compare-exchange, a Stall for a positive debt and a Skip for a negative one; a hard resync's `store(0)`; `evaluateDrift`, `getRequiredInputDelayIncreaseStallTicks`, `getPendingInputDelayTransitionTicks` and the published correction phase's debt bit read it) | **Correctly synchronized** — `std::atomic<int32_t>` (lock-free, a `static_assert`), relaxed, one signed counter, so a raise and a lower net. Its writer is the step's thread: the floor and tier receipts run in the drain at the start of each step (and in a teardown drain after the last step, while no step runs), so the request and the consume never run concurrently. The atomic, its compare-exchange consume and its `static_assert` stay (code): on two threads a request landing between the consume's load and its store is never lost, and a hard resync's `store(0)` may discard a request that lands in the same advance, which is the resync's own rule (the debt describes the pre-teleport frontier). A FIFTH correctly-synchronized crossing. Until og-session-runtime task 13 a frame-locked client requested on the game thread. Before P6-2 it was `m_requiredInputDelayIncreaseStallTicks`, a plain `unsigned` incremented on the GT and decremented on the PT, a read-modify-write on both sides that no row listed (found by og-input-latency's deferred P3-4 design, 2026-11-11). The request also stores the published correction phase (`TimeConfig-rationale.md` §18), which the clock-dilation stage reads on the frame thread through `ClientPredictionClock::publishedCorrectionPhase`, an atomic of its own | `PCTimeManagement/ClientPredictionClock.h`, the comment above `m_pendingInputDelayTransitionTicks`, and the header's input-delay transition block |

Row 5 is this doc's own worked example of the data/control distinction above: it is *stored* as
bitset data (priced on the tear-tolerance argument, like rows 1–4), but the bit it carries is read
as a *decision* (`resimGate::classifyResimSlotWrite` returns `Written` iff the bit is clear) — so
its true failure mode is a lost update on a decision, not a torn value, and its row says so rather
than inheriting row 1–4's framing by default. Row 6 is the demonstration that when a bit is
purely control and its race is real, the codebase fixes it instead of pricing it: item 45 promoted
what used to be derived per-slot state into one atomic word specifically to remove it from this
table's data-crossing rows. Rows 7 and 9 are the other two demonstrations of the same principle —
each is a control-*adjacent* scalar (a tier delay, an authority tick) that the codebase made
correctly synchronized with a single atomic rather than pricing as accepted debt — so the count of
correctly-synchronized crossings in this file is **five (rows 6, 7, 9, 10, 12), not one**, and row 6 no
longer claims otherwise. Row 10 joined at og-input-latency P2-14, when the estimator's RTT fields became
atomics. Row 12 joined at og-input-latency P6-2, when the clock's input-delay transition debt became a
signed atomic.

## The tick driver's mailboxes (J-rows)

`SimulationTickDriver` runs a host's frame pipeline in og-simulation: the owning thread (the host's frame thread)
pumps the scheduler, and the steps run on that thread (`Inline`) or on one task per frame (`FrameBounded`). A `FreeRunning` authority
is the exception: its owning thread is a dedicated runner thread, which pumps and steps, and the frame thread keeps
queues and snapshots only; the last three rows below are its crossings from the runner to the frame thread. Its mailboxes between the
owning thread and the step's thread are crossings inside og-simulation, so they are listed here, under the J-names
of the driver's design. They are not part of the twelve numbered rows above, which inventory the netcode's
crossings, and none is accepted debt. J5 and J7 are the render presenter's (`TickDriverPresenter`), which the
step hooks call on the step's thread. J2 is the world host's world lock (`TickDriverWorldHost`), which the driver
takes around each step through `lockForStep()`; og-simulation-jolt's `JoltTickWorldHost` is the first world host that
owns one.

| # | Crossing | Direction | Shape | Where it is described |
|---|----------|-----------|-------|-----------------------|
| J1 | occupancy commands, `SpscRing<BodySlotOccupancyCommand, 64>` | owning-thread write (`pushBodySlotOccupancy`) → step read (the step hooks' `beforeTick` drain into the step driver's `noteBodySlotOccupancy`) | **Staged SPSC ring** — one producer, one consumer (the steps are serialized); a full ring is an `OG_CHECK`, because a lost command would leave the world stepping the wrong body set | `SimulationTickDriver-rationale.md` §5 |
| J2 | the world lock, the world host's `BasicLockable`, taken through `WorldLockScope` | step ↔ owning thread: the step holds it for the whole step (`lockForStep()` in the driver's per-step body); the owning thread holds it around a world write outside a step (the world build, a join-time bind, the driver's construction) | **Mutex** — one lock per world host and role. The step world's access predicates refuse a caller that neither runs this role's step nor holds this role's lock; the scope `OG_CHECK`s that this thread does not already hold it (the lock need not be recursive), and the owning thread never waits for a step task while it holds it (`SimulationTickDriver-guards.md` G-05) | `TickDriverThreadContext-rationale.md` §4, §5; `SimulationTickDriver-rationale.md` §3 |
| J3 | the published tick offset, `std::atomic<int64_t>` | step write (`afterTick`, `tick − physicsStep`, relaxed) → owning-thread read (`pumpAndRunSteps` and `tickOffset()`, relaxed) | **Correctly synchronized** — a lone single-writer/single-reader atomic scalar. A read may see an earlier step's offset; the offset changes only on a step whose tick does not advance by exactly one (a Stall, a Skip, a hard resync) | `SimulationTickDriver-rationale.md` §5; `SimulationTickDriver-guards.md` G-03 |
| J5 | the render snapshots, `SnapshotChannel<Snapshot, 4>` | step write (`publish_Step`, from `afterTick`: fill a pooled slot in place, commit it as the newest) → owning-thread read (`present_GameThread`'s pose build peeks back from the newest, holds the bracketing pair, releases both) | **Snapshot channel** — one writer, one reader, lock-free; the writer fills a free slot or recycles the oldest one the reader does not hold, so a held snapshot is never written and a read is never torn. The reader holds at most two of four slots, so a slot is always free (`publish_Step` `OG_CHECK`s it; a refused write is counted as a drop) | `TickDriverPresenter-rationale.md` §3, §4 |
| J6 | step-cost samples, `SpscRing<StepCostSample, 256>` | step write (`afterTick` push) → owning-thread read (the post-physics drain into the window) | **Staged SPSC ring** — one producer, one consumer; a full ring drops the sample and counts it. Diagnostic only: nothing decides on it | `SimulationTickDriver-rationale.md` §5; `TickDriverStepHooks-rationale.md` §5 |
| J7 | a resimulation's render correction, `SpscRing<Correction, 16>`, with the capture's four counters (`std::atomic<uint64_t>`) | step write (`captureCorrection_Step`, from `beforeSimulate` after a replay: push the correction; count the outcome, relaxed) → owning-thread read (the pose build's `correctPoses` drains the ring; the correction window line reads the counters, relaxed) | **Staged SPSC ring** — one producer, one consumer; a full ring drops the correction and counts it (`ringDrops=`), and the render then shows that correction as a pop. The counters are lone single-writer atomics read for the window line only | `TickDriverPresenter-rationale.md` §3, §8 |
| J8 | `FreeRunning` only: the published rate scale, `std::atomic<double>` (1.0 at construction) | runner write (`pumpAndRunSteps`, right after `beforePump`, once per wake, release) → frame-thread read (`stepIntervalSeconds()`, the present clock's interval, acquire) | **Correctly synchronized** — a lone single-writer atomic scalar. A read may see the previous wake's rate; the present clock then uses that interval for one frame. The other modes never write or read it | `SimulationTickDriver-rationale.md` §5, §12 |
| loop origin | `FreeRunning` only: the loop's origin on the engine clock, `std::atomic<double>` (`-1.0` before the loop), with the loop's active flag, `std::atomic<bool>` | runner write (`runAuthorityLoop`: the active flag, then the origin, once before the first wake; the flag cleared when the loop returns; release) → frame-thread read (`hostNowSeconds()` reads the origin; `stop()` and the destructor read the flag; acquire) | **Correctly synchronized** — single-writer atomics. The origin is written once, so a read sees `-1.0` (`hostNowSeconds()` then returns 0) or the final value. The flag is read only after the frame thread joined the runner, where the join already orders it; the check catches a `stop()` before the join | `SimulationTickDriver-rationale.md` §12, §13 |
| J9 | `FreeRunning` only: presenter registrations, `SpscRing<PresenterRegistration, 64>` (`SimulationTickBackend`) | runner write (`onRegistered`, `forgetPresenterTarget`: push `{id, slot, registered}`) → frame-thread read (`present()` drains it into the presenter's `onRegistered` / `onUnregistered` before it presents) | **Staged SPSC ring** — one producer, one consumer; a full ring is an `OG_CHECK`, because a lost registration would leave the presenter rendering a character that left, or never rendering one that joined. Applied in push order | `SimulationTickBackend-rationale.md` §10; `SimulationTickBackend-guards.md` G-01 |

## The display snapshot's crossings (og-input-latency P2-18, P5-36)

A tick-driver client's host fills a display snapshot (og-sim-session's `SimulationDisplaySnapshot.h` in this tree,
og-simulation's until og-session-runtime task 25) after each step batch,
on the step's thread, from the driver's after-batch hook (`TickDriverConcepts-rationale.md` §11), and its drawing thread
reads only the published copies. Until og-input-latency P5-36 the Unreal host filled it on the game thread in the frame's
post-physics pass, which the driver runs while the frame's own batch may still be stepping: an unsynchronized read of
everything the step writes. The fill's own reads now run on the step's thread; one hand-over carries what crosses, the snapshot channel. The table's
second row, the tier reading, is a hand-over within the step's thread. Like the J-rows, none is accepted debt.

| Crossing | Direction | Shape |
|---|---|---|
| the display snapshots, `SnapshotChannel<Snapshot, 4>` (`SimulationDisplayPublisher`) | step write (`publish_Step` in the after-batch fill) → drawing-thread read (release the one held, then acquire the newest: `refreshView_GameThread`, og-input-latency P6-1; between passes the reader holds one) | **Snapshot channel**, as J5: the reader holds at most two of four, so the writer always finds a slot and a held snapshot is never rewritten |
| the display tier reading, a host's `TripleBuffer` beside the snapshot | the tier receipt's write, inside the client's drain at step start (the step's thread) → the fill's read on the step's thread | **Triple buffer**, one writer and one reader, both on the step's thread while the session steps: the reader takes the newest published reading. A host's other writes (before its first step, after its last) are in its thread table (og-sim-session's `SessionThreads-rationale.md` §7.3) |

The client's drain runs at step start, on the step's thread, so its writers (a correction landing, a relay store the collect
also reads, a registration's storage insert: rows 1, 2 and 11) run on the thread the fill runs on, and the fill crosses
nothing but the snapshot channel. A third row, the relay stage (a `TripleBuffer` of the relay fields, written on the game
thread before each batch for a frame-locked drain), lost its user in og-session-runtime task 12 and was deleted with its type
in task 13.

## What is not on this list

- **The relay codec's own receipt/flush pair** (`RelayedInputRingCodec.h`) is one-thread — receipt
  (staging) and the flush both run on the feed's thread. Not a crossing.
- **`LocalInputCache`** is PT-only by construction (its own THREADING note: "NOT thread-safe;
  single-threaded by construction"). Not a crossing.
- **`RemoteMoveQueue` / `PendingInputQueue`** (`SimulationQueues.h`) are staged SPSC rings, one
  producer and one consumer, so they stay safe if the feed and the step part (today both ends run
  on the step's thread) — the pattern this doc's standing rule points every new crossing at, not an
  instance of the debt class. They are cited above as the *existing conforming instances*, not
  enumerated as rows.
- **`DesyncDiagnosticSink.h`'s atomic counters** (`m_hashMismatchCount`, `m_confirmedDivergenceCount`)
  are real GT↔PT-shaped atomics by design, but the class's own header says it outright: "deliberately
  NO production consumer yet." This is task 73's staged hash channel — DEFERRED by user ruling
  (backlog item 73) — so nothing calls `onHashMismatch`/`onConfirmedDivergence` in production today;
  the crossing does not exist at runtime yet. Consistent with item 81's original ruling to keep task
  73 absent from this doc (verified 0 mentions there); when 73 is revived, its row lands then.
- **The Chaos tick mapper** (ChaosTickMapper, its `m_offset` an `std::atomic<int32_t>`) was a real, *wired* GT/PT-shaped
  crossing until og-session-runtime task 4 deleted it with the Chaos configuration (its production call sites were in the
  brawler's Chaos host, which task 3 deleted) — but it lived in `OGSimulationUnreal`, which is a different module in
  the og-simulation-ue plugin repository (`Plugins/OGSimulation/Source/OGSimulationUnreal/`; the outer game repository's
  until og-tickdriver task 16), not inside the `og-simulation` submodule
  this doc's own path (`OGSimulation/docs/ThreadingCrossings.md`) lives in and scopes itself to ("the
  one place every ... crossing in `og-simulation` is enumerated," line 4 above). It was out of this doc's
  declared scope on a repo boundary, not an oversight — flagged here rather than silently passed over
  so the boundary was a stated decision, not an absence someone had to re-derive.

- **A game's local input sample** (og-input-latency task P5-15). The values a client's input provider reads at capture
  are written on the host's frame thread (the engine's input events, the camera and the cursor) and read on the step's thread.
  og-simulation's answer is a hook and a mailbox, not a row: `TickDriverSampleGame::sampleInput()`, which
  `SimulationTickDriver::startFrame` calls first in `Inline` and `FrameBounded` (never in `FreeRunning`), on the frame's
  thread, before the pump; the game publishes the frame's sample there through a `TripleBuffer` (`Mailbox.h`: one
  producer, one consumer, the reader always takes the newest whole sample), and its provider reads only that. The
  Unreal brawler adopts it in P5-15: `UOGBrawlerInputCollectionComponent` stages a `BrawlerInputSample` in the
  character's tick and publishes it from `BrawlerSessionHooksUImpl::sampleInput` (which og-sim-session's `SessionTickGame`
  calls through the Unreal session's tick host since og-session-runtime task 26); `buildPlayerInput` reads only the
  published sample. Before P5-15 the provider read the component's fields directly while the game thread could write
  them, accepted as benign (each field a trivially copyable value, re-sampled every frame); that crossing was in the
  outer game repository, outside this doc's scope, and was never a row here. Like the Chaos tick mapper above, the adopting
  code is outside this submodule; the pattern it uses is og-simulation's.
- **The authority's round trip** (og-session-runtime task 39). Until task 39 the Unreal session handed its authority runner
  each og-net connection's engine ping through a game-thread → runner copy (one lock-free word per connection, published every
  frame); that crossing was in the og-simulation-ue plugin, outside this doc's scope, and was never a row here. Since task 39
  og-sim-net's server host core reads og-net's per-connection stats on the runner (`SimNetServerHost-rationale.md` §6), under
  og-net's own stats lock (og-net's `EndpointCore-rationale.md` §10): the game-thread copy is gone, and what crosses is og-net's
  servicing thread to the runner, which on a dedicated server are one thread.

## Item 82 (gated)

The SPSC receive-path seam that would retire rows 2 and 5 — and, if the relay-store crossing (row
1) shares the same drain point, row 1 too — is scoped and gated in backlog item 82, dispatched
before the next inbound channel or any further weakening of correction cadence, not before. This
doc is that item's own dispatch precondition and checklist: when 82 lands, update the affected
rows here rather than deleting them silently, so the inventory's count stays true. Since
og-session-runtime task 13 rows 1, 2 and 5 are one thread on every host in this tree, so the seam
is needed only by a host that feeds on another thread than the step.

## Completeness sweep (item 83)

Two crossings (rows 7, 8) were named by review (item 81 finding 1) and independently re-verified
against source rather than trusted from prose. **The dispatch's own instruction was not to stop
there** — "assume a third exists and run your own completeness sweep" — because a doc that misses a
crossing is worse than no doc: it gets trusted as exhaustive. What follows is what was actually
checked, not only what was found, so a future editor can tell whether a new gap is this sweep's
blind spot or a genuinely new crossing introduced after it.

**Method.** `og-simulation` (the submodule this doc lives in) was searched case-sensitively for every
occurrence of `THREADING`, `GAME THREAD`, `GAME-THREAD`, `PHYSICS THREAD`, `PHYSICS-THREAD`, and
`std::atomic` — 19 files matched. Every match was read in its surrounding context (not just the
matched line) and classified into one of: already an inventoried row; explicitly single-thread-owned
by its own comment (not a crossing); the *same* pre-existing race as an inventoried row, stated as
such by its own comment (not a new row); or a genuinely uninventoried crossing.

**Files read and their disposition:**

- `NetSyncTelemetry.h`, `SimulationManager.h`, `ResimGateProbe.h`, `Network/RelayReadProbe.h`,
  `Network/RelayWritePathProbe.h`, `Network/CorrectionVerdictProbe.h` — every probe pair is
  explicitly single-thread-owned by its own "TWO OBJECTS BECAUSE THERE ARE TWO THREADS" /
  "PHYSICS THREAD ONLY" / "GAME THREAD ONLY" banner. Not crossings; this is the pattern the codebase
  uses specifically to AVOID a crossing on hot instrumentation. (`NetSyncTelemetry.h`'s own two-thread
  rule was independently re-read for this sweep, not re-litigated — its one finding, item 79 f1, is
  handled separately in this task; see impl notes.)
- `Network\RemoteInputCache.h` (row 1), `CorrectionCache.h` (rows 2, 5, 6 — including
  `m_stateProvenance`/`SlotStateProvenance.h` and `m_slotLandingSeqNr`, both of which state in their
  own comments that they "RIDE THE PRE-EXISTING STATE-BUFFER RACE" / "the same pre-existing race,
  deliberately not a new one" rather than opening a new one), `SimulationNetSync.h` (rows 3, 4, 7, 8;
  also `getLastRelayedInput` and the relay-arrival bind path, both explicitly same-thread-as-writer by
  their own THREADING notes, hence not crossings) — all already inventoried or explicitly ruled out
  by their own text.
- `SimulationQueues.h` — the two SPSC rings (`RemoteMoveQueue`/`PendingInputQueue`); already excluded
  under "What is not on this list."
- `RelayedInputRingCodec.h`, `Network/LocalInputCache.h`, `Network/ServerReceptionCoordinator.h`,
  `Network/ReplicatedTierConsumer.h`, `Network/ServerInputDelayQueue.h` — each is explicitly
  single-thread-owned by its own THREADING contract (GT-only or PT-only), read in full to confirm no
  hedge language ("mostly," "in practice") that would demote the claim. `ReplicatedTierConsumer.h`
  feeds row 7's atomic downstream; it does not itself cross.
- `DesyncDiagnosticSink.h` — real atomics, no production wiring yet (task 73, deferred); see "What is
  not on this list."
- `SlotStateProvenance.h` — the same pre-existing state-buffer race as row 5, by its own comment.
- `PCTimeManagement/NetworkTimeEstimator.h` — **this is where the sweep earned its keep.** Its own
  "THREAD-SAFETY CONTRACT" banner names three GT-write/PT-read members
  (`m_authorityTick`, `m_smoothedRTT`/`m_smoothedJitter`, `m_hasFirstSample`) that were absent from
  every row and from "what is not on this list." None of the three prior passes (the original
  six-row landing, item 81's own review sweep, or the lead's re-derivation) had caught it — worth
  recording plainly: this class's contract banner uses "Game thread (GT)" / "Physics thread (PT)"
  (mixed case, no hyphen), not the `GAME-THREAD` / `PHYSICS-THREAD` spelling this doc's other rows
  quote verbatim, so a case-sensitive grep for the hyphenated form alone (which is what the doc's own
  citation style would suggest searching for) would miss it too — it was only caught here because
  `std::atomic` was searched as an independent term, not folded into the THREADING/GAME-THREAD
  pattern family. Filed as rows 9 and 10.
- **Adapter boundary**, per the dispatch's explicit instruction to check it: `SimmableUpdateComponent.h`
  (confirmed the GT attribution row 8 then depended on — `ServerReceiveRemoteMove` was a
  `UFUNCTION(Server,...)`, declared on the replicated sibling `SimulatableNetSyncComponent.h` from
  og-ue-simulationcomponentandmanagersplit task 20a until og-input-latency task P5-8 deleted both, and a GAME-thread RPC; at the time it was a
  `UFUNCTION(Server,...)`, `deliverDelayedRemoteInput` stated "Called on the GAME THREAD" inline; since the outer repository's
  2026-09-26 comment conversion that statement lives in its component rationale doc) and
  `PCTimeManagement/ChaosTickMapper.h` (a real, wired atomic crossing — excluded on the repo-boundary
  reason recorded under "What is not on this list," not overlooked).

**Not re-swept**: rows 1–6's own THREADING blocks were not re-read line-by-line beyond what this pass
and item 81's review already verified against source (both independently re-derived the two-writer
claim and row 3/4's cited text before this task began) — re-verifying six already-confirmed rows a
third time would not have found anything a fourth crossing search wouldn't also need to find, and the
budget went to the untouched files instead.

**What would still slip this method**: a member whose own comment omits both the THREADING/
GAME-THREAD family of words AND the word `atomic` — e.g. a plain non-atomic field crossing threads
with no comment at all. This sweep is a keyword search over a codebase that (so far, verified by
every file above) always documents its threading contracts in prose; it does not independently prove
every cross-thread member is commented. That residual is structural to the method, not specific to
this run, and is recorded here rather than left implicit.
