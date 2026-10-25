<!-- SPDX-License-Identifier: MPL-2.0 -->
# GT↔PT crossing inventory

This is the one place every game-thread/physics-thread crossing in `og-simulation` is enumerated.
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

> **Any new GT↔PT crossing adds its row here, and prefers the staged SPSC pattern** —
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

## The eleven crossings

⚠ **Extended by item 83 (2026-08-18).** Rows 7–10 were missing from the original six-row landing —
row 7 and row 8 were named by review (item 81 finding 1); rows 9 and 10 surfaced from this task's
own completeness sweep (below), which the original six-row pass did not run. See "Completeness
sweep (item 83)" for what was checked and how these two were found.

⛔ **Every host-engine name in the table below is ONE adapter's binding, never the binding.** A
crossing is a property of *this core* — which thread writes a member and which thread reads it —
and that property survives whatever engine hosts it. Where a row needs to say *how* a thread is
entered or *what* delivered a call, it names the symbol this project's shipping adapter uses; an
adapter on another engine substitutes its own and the row's shape is unchanged. The three that
recur, with the role each plays:

| name in a row | the ROLE it plays | whose it is |
|---|---|---|
| `OnPreSimulate_Internal` | the **pre-simulate physics callback** — where the core's prediction step is entered, before the solve | one adapter's physics callback (`FSimulationManagerAsyncCallback`, on Chaos) |
| `OnPostSolve_Internal` | the **post-solve physics callback** — where the core captures state, after the same frame's solve | the same callback object; the two are separate engine callbacks with the solve running between them, which is what widens row 11's window |
| `UFUNCTION(Server,...)` | the declaration that makes a method a **client→server RPC entry point**, arriving on the game thread | one adapter's reflection macro; another declares its RPCs some other way, and row 8's GT attribution then rests on that declaration instead |

`UOGBrawlerSimulatableComponent` in row 11 is likewise one adapter's binding for the **per-character
registration site** — the object that publishes a character into the core's storage and withdraws
it at teardown.

| # | Crossing | Direction | Shape | In-file THREADING block |
|---|----------|-----------|-------|--------------------------|
| 1 | Relay stores (`m_remoteInputCaches`) | GT write (`OnRep_RelayedInputRing`) → PT read (`collectInputAll`, `collectResimInputAll`; `collectInputAll` named `prepareSimulationStep` between item 90 and item 94) | Data — accepted debt, correction-healed | `Network/RemoteInputCache.h` — "THREADING — GAME-THREAD WRITE / PHYSICS-THREAD READ. ACCEPTED DEBT." block, and its DEFERRED CLEANUP paragraph naming the SPSC fix |
| 2 | Correction inject path (OnRep → decode → `tryInsertingCorrectState`) | GT write → PT read of `m_stateBuffer` | Data — accepted debt, correction-healed | `SimulationReconciliation.h` — "Correction injection — called from OnRep_-dispatched lambdas (game thread)" (call site); `CorrectionCache.h` — the item 47 landing-stamp THREADING block's first bullet ("RIDE THE PRE-EXISTING STATE-BUFFER RACE") for the race itself |
| 3 | `m_lastUsedCaptureTicks` | PT write (`collectInputAll`) → GT read (`sendCorrectionAll`) | Data — accepted debt, correction-healed (opposite direction from 1/2/4/5 — PT is the writer here) | `SimulationInputResolution.h` — the member's own declaration comment ("Written in collectInputAll (PHYSICS thread), read in sendCorrectionAll (GAME thread)"); moved off `SimulationNetSync.h` at item 87 |
| 4 | The ±1 frontier read in `decideCorrectionArrival` | GT read of a frontier PT advances (`pushPredictionTick`) | Data — self-bounded (±1 per sample, does not accumulate), not correction-healed because it mis-files a probe sample rather than mis-deciding a write | `SimulationNetSync.h` — the "READ AFTER THE INSERT, WHICH IS SAFE AND ±1 RACY" block in `decideCorrectionArrival` |
| 5 | `m_containsCorrectTick` (the write-protect decision bit) | GT `set()` (`tryInsertingCorrectState`) ↔ PT clear (`pushPredictionTick`'s push path) | **Control-shaped bit inside a data crossing** — both are RMWs of the same `std::bitset` word; the failure mode is a lost update, not a torn read (item 81's correction — see below) | `CorrectionCache.h` — item 47's header block above `tryInsertingResimulatedState` (the honest-bound note), and the landing-stamp THREADING block's second bullet, both in the private section near `m_slotLandingSeqNr` |
| 6 | The resim gate atomic (`m_pendingResimAnchorTick`) | GT CAS-max set (`tryInsertingCorrectState`'s hit path) / PT CAS clear (resim-completion edge) / PT-or-GT wipe | **Control — correctly synchronized**, and the only one of the three correctly-synchronized crossings (rows 6, 7, 9) with true multi-writer CAS coordination — rows 7 and 9 are each a single-writer/single-reader atomic scalar. One `std::atomic<uint32>`, CAS on every writer, no RMW race | `CorrectionCache.h` — the item 45 "THE RESIM GATE — ONE WORD, EDGE-TRIGGERED" block above `getPendingResimAnchorTick` |
| 7 | `m_clientEffectiveInputDelayTicks` | GT write (`setClientEffectiveInputDelayTicks`, from `OnRep_ConnectionTier`) → PT read (`collectInputAll`, `collectResimInputAll`, once per tick each; `collectInputAll` named `prepareSimulationStep` between item 90 and item 94) | **Correctly synchronized** — `std::atomic<int32>`, relaxed ordering, a lone independent scalar with no internal structure (a stale read applies a tier change one tick late, nothing else). A SECOND correctly-synchronized crossing — item 81's original row 6 called the gate atomic "the sole" one; that claim was false the day it was written, since this member already carried its own THREADING comment in the same file three of the doc's rows (1, 3, 4) already point into | `SimulationInputResolution.h` — "THE GAME->PHYSICS THREAD CROSSING" block above `m_clientEffectiveInputDelayTicks`'s declaration (both the member and its `setClientEffectiveInputDelayTicks` writer moved off `SimulationNetSync.h` at item 87; the old forwarder was deleted, not kept) |
| 8 | `m_currentAuthorityTick` / `m_rollbackWindowTicks` | PT write (`setAuthorityGuardContext`, from `onGameSimulationAuthority`) → GT read (the RPC-arrival lambda bound in `registerAuthorityOwner`, firing from `ServerReceiveRemoteMove` (`UFUNCTION(Server,...)`) and from `deliverDelayedRemoteInput`, both GAME thread) | **Data — accepted debt, unsynchronized.** Plain `uint32`/`int32`, not atomic. Direction is PT→GT — the doc's default framing is GT→PT, so this is the same reversed shape as row 3, not row 3 itself. Honest hazard: a naturally-aligned 4-byte word is a single-bus-cycle op on x86-64 (same practical argument row 10 documents for `NetworkTimeEstimator`'s plain fields) so true tearing is not the realistic exposure; the real one is *staleness* — the RPC-arrival read may see an authority tick up to one PT tick old, which the member's own comment already prices as acceptable ("an at-most-one-tick-stale value is fine for a multi-tick rollback window"). It gates an accept/reject decision (too-far-future capture-tick rejection) so it is control-*flavored* data, but unlike row 5 there is exactly one writer thread, so no lost-update/RMW race is possible — only staleness on a plain read. Not correction-healed by a later landing (unlike rows 1–4); self-heals by being overwritten every authority tick instead | `SimulationNetSync.h` — the members' own declaration comment above `m_currentAuthorityTick` |
| 9 | `NetworkTimeEstimator::m_authorityTick` | GT write (`recordAuthorityTick`, from `OnRep_TimingInfo`) → PT read (`getTargetPredictionTick`/`getLastAuthorityTick`, called from `ClientPredictionClock::advancePrediction`) | **Correctly synchronized** — `std::atomic<unsigned int>`, explicit, single-writer/single-reader. A THIRD correctly-synchronized crossing, found by this task's own sweep, not by item 81's review | `PCTimeManagement/NetworkTimeEstimator.h` — the class's own "THREAD-SAFETY CONTRACT" banner |
| 10 | `NetworkTimeEstimator::m_smoothedRTT` / `m_smoothedJitter` / `m_hasFirstSample` | GT write (`updateRTT`, from `OnRep_TimingInfo`) → PT read (`getSmoothedRTT`/`getSmoothedJitter`/`hasFirstRTTSample`, from `advancePrediction`) | **Data — accepted debt, a DIFFERENT mechanism from rows 1–4.** Plain `double`/`double`/`bool`, not atomic, not correction-healed by a later landing. Relies on an explicit *platform* assumption the class states itself: a naturally-aligned 8-byte load/store is a single-bus-cycle op and cannot tear on x86-64/MSVC/Clang (UE's primary PT target today) — and the same comment names its own limit: "if this class is ever ported to ARM or compiled with unaligned-access enabled, wrap these fields in `std::atomic<double>` as well." `m_hasFirstSample` is write-once (GT) / read-many (PT) and carries the same caveat | `PCTimeManagement/NetworkTimeEstimator.h` — the same "THREAD-SAFETY CONTRACT" banner |
| 11 | Storage exposure at registration/teardown (`storage.add` / `storage.remove`) | **Both directions named.** GT write — `storage.add` via the component's registration loop, `UESimulatableOwnerBinding::tryRegisterWithNewFramework` (`GetTimerManager().SetTimerForNextTick`; the brawler component's own loop before og-ue-simulationcomponentandmanagersplit task 29) at registration, `storage.remove` via the unregister facade at teardown — ↔ PT read/iterate, the `forEachSimulatable`-driven sweeps: `allocateFrontierSlotsAll`, `postPredictionAll`, `collectInputAll` | **Data — accepted debt, UNCHANGED; its FATAL consequence removed, and a MARSHALLING POINT now exists but is DELIBERATELY UNADOPTED.** Unsynchronized; no lock protects `m_storage`/cache/queueMap across the boundary. og-netcode-v2-field-defects task 3 changed three things and left this row's mechanism alone. (1) The ORDER: the client `registerSimulatable` overload publishes to storage LAST and `registerLocalCharacter` creates the delay line BEFORE the provider, so a sweep that reaches an id finds every container that id's branch will ask for — the window is closed for the invariant that actually crashed. (2) The CONSEQUENCE: `collectInputForCharacter` and `collectResimInputForCharacter` look the delay line up with `find` and answer the injected neutral on a miss, behind a `NOCACHE` warning rate-limited to one line per tick, so a future tear costs one logged tick instead of the process — it used to be a `std::out_of_range` thrown out of a Chaos step on a shipped client. (3) A marshalling point, `DeferredLifecycleQueue`: the game thread enqueues a lifecycle job, the physics thread applies it between steps. ⛔⛔ **NO ADAPTER IN THIS TREE USES THE QUEUE YET, AND THAT IS A RULING, NOT AN OMISSION.** It was implemented, wired into og-brawler-unreal, measured and then HELD, because adopting it INVERTS this row instead of closing it: it makes the physics thread the sole writer, which moves the exposure onto the GAME-thread readers — and two of those ITERATE rather than look up. `SimulationNetSync::sendCorrectionAll` walks `m_authorityWriters` and `sendLocalInputToAuthorityAll` walks `m_localInputSenders`, both once per tick from `onPostSimulationGameThread`, and an insert under a live iteration is a worse failure than the `find` it would replace. Eight more game-thread readers sit behind them: the adapter's per-frame visualization handoff, its HUD accessors, its input-history poll (`findPendingInputQueue`, the local-input-cache read), the by-id relay and remote-move doors reached from OnRep/RPC, and the systems-roster notify. Before the move all of those were same-thread with the only writer. **Adopting the queue therefore requires marshalling or snapshotting the game-thread send path FIRST**; until then this row stands as written and the queue ships unused, with its semantics pinned by tests in og-simulation-tests. ⚠ The core could not enforce adoption in any case: `registerSimulatable` is a free function any consumer may call from any thread, and nothing in og-simulation checks which one. Not newly found — audited and priced by item 94's D-2 (`DesignInputResolutionPeer.md`, "D-2, AUDITED AND REJECTED AT ITEM 94"), whose rejection of a proposed capture-side pairing check rests entirely on this crossing; that section is the analysis of record for this row, not a duplicate of it. **The window is wider than a single-callback race**: the two PT sweeps run in **two separate Chaos callbacks** — `OnPreSimulate_Internal` (→ `onGameSimulationPrediction`, sweep 1 + `allocateFrontierSlotsAll`) and `OnPostSolve_Internal` (`postPredictionAll`; both in `ChaosTickBackendUImpl.cpp` since og-simulationscheduler-withjolt task 82) — with the actual physics solve running between them, so a GT registration or teardown can land anywhere across that whole span, not just inside one callback | `SimulationNetSync.h`, the client `registerSimulatable` overload (the "PUBLISH LAST — createCacheFor BEFORE storage.add" guard) and the `unregisterSimulatable` facade (item-94 Part F's re-checked mirrored-invariant paragraph); local text stays authoritative, this row indexes it, not the other way around |

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
correctly-synchronized crossings in this file is **three (rows 6, 7, 9), not one**, and row 6 no
longer claims otherwise.

## The tick driver's mailboxes (J-rows)

`SimulationTickDriver` runs a host's frame pipeline in og-simulation: the owning thread (an engine host's game
thread) pumps the scheduler, and the steps run on that thread or on one task per frame. Its mailboxes between the
owning thread and the step's thread are crossings inside og-simulation, so they are listed here, under the J-names
of the driver's design. They are not part of the eleven numbered rows above, which inventory the netcode's
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

## What is not on this list

- **The relay codec's own receipt/flush pair** (`RelayedInputRingCodec.h`) is GT-only — receipt
  (staging) and the flush are both on the game thread. Not a crossing.
- **`LocalInputCache`** is PT-only by construction (its own THREADING note: "NOT thread-safe;
  single-threaded by construction"). Not a crossing.
- **`RemoteMoveQueue` / `PendingInputQueue`** (`SimulationQueues.h`) are GT↔PT crossings, but
  already staged SPSC rings — the pattern this doc's standing rule points every new crossing at,
  not an instance of the debt class. They are cited above as the *existing conforming instances*,
  not enumerated as rows.
- **`DesyncDiagnosticSink.h`'s atomic counters** (`m_hashMismatchCount`, `m_confirmedDivergenceCount`)
  are real GT↔PT-shaped atomics by design, but the class's own header says it outright: "deliberately
  NO production consumer yet." This is task 73's staged hash channel — DEFERRED by user ruling
  (backlog item 73) — so nothing calls `onHashMismatch`/`onConfirmedDivergence` in production today;
  the crossing does not exist at runtime yet. Consistent with item 81's original ruling to keep task
  73 absent from this doc (verified 0 mentions there); when 73 is revived, its row lands then.
- **`ChaosTickMapper` (`m_offset`, an `std::atomic<int32_t>`)** is a real, *wired* GT/PT-shaped
  crossing (production call sites in `ChaosTickBackendUImpl.cpp`/`.h`; `OGBrawlerSimulatableComponent.h` only
  forward-declares it) — but it lives in `OGSimulationUnreal`, which is a different module in
  the og-simulation-ue plugin repository (`Plugins/OGSimulation/Source/OGSimulationUnreal/`; the outer game repository's
  until og-tickdriver task 16), not inside the `og-simulation` submodule
  this doc's own path (`OGSimulation/docs/ThreadingCrossings.md`) lives in and scopes itself to ("the
  one place every ... crossing in `og-simulation` is enumerated," line 4 above). Out of this doc's
  declared scope on a repo boundary, not an oversight — flagged here rather than silently passed over
  so the boundary is a stated decision, not an absence someone has to re-derive.

## Item 82 (gated)

The SPSC receive-path seam that would retire rows 2 and 5 — and, if the relay-store crossing (row
1) shares the same drain point, row 1 too — is scoped and gated in backlog item 82, dispatched
before the next inbound channel or any further weakening of correction cadence, not before. This
doc is that item's own dispatch precondition and checklist: when 82 lands, update the affected
rows here rather than deleting them silently, so the inventory's count stays true.

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
  (confirms the GT attribution row 8 depends on — `ServerReceiveRemoteMove` is a
  `UFUNCTION(Server,...)`, declared on the replicated sibling `SimulatableNetSyncComponent.h` since
  og-ue-simulationcomponentandmanagersplit task 20a and still a GAME-thread RPC; at the time it was a
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
