<!-- SPDX-License-Identifier: MPL-2.0 -->
# Perspective: how a remote input reaches a proxy

**One input, followed end to end.** A player presses a key on machine A. This document follows that
single sample from the moment it is sampled on A, through the authority, to the tick on machine B
where B's *proxy* of that player integrates it. It names the file and the symbol at every hop, so
that a reader can put the cursor on any step of the journey and keep reading in the code.

This is a **perspective** document. It does not replace the headers: every fence, every
per-parameter contract and every invariant stays at its site, and where this document summarises one
it says which file is authoritative. If this file and a header disagree, **the header is right and
this file is stale** — the same rule `ThreadingCrossings.md` states for its own rows.

Origin: `og-source-doc-extraction` backlog item 1, written 2026-08-21 from a first-hand read of the
headers named in §2, cross-checked against `og-netcode-v2-input-relay`'s `ArchitectureReviewBrief.md`,
`InputRelayDesign.md`, `RelayDelaySpectrumDesign.md` and `DesignInputResolutionPeer.md`. Where those
design documents and the tree disagreed, the tree won and the disagreement is recorded in that item's
impl notes.

> ⚠ **`ArchitectureReviewBrief.md`, `InputRelayDesign.md`, `RelayDelaySpectrumDesign.md`,
> `DesignInputResolutionPeer.md` and `impl_notes_wave1_1.md` are private working material from the
> initiative archives and are NOT distributed with this submodule.** They are named as provenance,
> deliberately unlinked; every claim this file *asserts* is anchored to a file in this repository
> and only to those. The declarations below tell `tools/lint/doc_anchor_lint.ps1` that these names
> are intentionally unresolvable, so it enumerates them with this reason instead of reporting them
> as drift.

<!-- lint-external-ref: ArchitectureReviewBrief.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: InputRelayDesign.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: RelayDelaySpectrumDesign.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: DesignInputResolutionPeer.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: impl_notes_wave1_1.md -- og-source-doc-extraction initiative workspace; private working material, not distributed with this submodule -->

**Read this alongside:** `ThreadingCrossings.md` (which of these hops cross a thread boundary and what
that costs), `DiagnosticsConventions.md` (the `emit*` / `getDiagnostics()` shape every hop's telemetry
follows), `ResimGatePolicy-rationale.md` (what happens *after* a correction disagrees).

---

## 1. The shape, in one paragraph

There is no direct client-to-client channel. A's input reaches B by going **through the authority**,
and it is relayed **at receipt**, not at application. The authority parks the input in a per-wire
delay queue so that it applies it a fixed number of ticks later; at that same moment it stamps the
input with **that same delay** and relays the pair `(captureTick, dA)` to every peer. B stores the
pair keyed by **A's capture tick**, and when B predicts tick `N` for its proxy of A it asks: *which
capture tick is scheduled to be applied at `N`?* — that is `N - dA` — and reads that slot. When the
answer is there, B's proxy runs the input the authority is about to run, at the tick the authority is
about to run it. When it is not, B's proxy holds the last thing it heard.

The delay is not incidental. It is the mechanism: **the same number that makes the authority late is
the number that lets a peer be early.**

---

## 2. The trace table

Every row is one hop. `thread` is **GT** (game thread) or **PT** (physics thread); the physics thread
is whichever thread the host engine runs its asynchronous physics callback on — one adapter's binding
is Chaos's async-callback thread under `bTickPhysicsAsync`. `machine` is A (the sender), S (the
authority) or B (an observing peer). Paths are relative to the repository root; `<core>` abbreviates
`Plugins/OGSimulation/Source/OGSimulation/og-simulation/OGSimulation/`.

⚠ **Rows whose `file` column starts with `Source/` are outside `og-simulation`, and are one
adapter's binding rather than the binding.** The engine-free core owns only the `<core>` rows; the
`Source/` rows are how *this project's* adapter happens to reach them, and an adapter on another
engine substitutes its own. Two carry a whole hop rather than illustrating it, so their roles are
named here. `ASimulationManagerUImpl` — one adapter's **composition root**: the object that owns
the per-role manager and routes an arriving input to the component owning that character.
`ASimulationInputRelay` — one adapter's **per-connection transport actor**: it publishes a
connection's staged inputs once per replication poll.

| # | hop | machine · thread | file | symbol |
|---:|---|---|---|---|
| 1 | The sample is built for this sim tick | A · PT | `Source/OGBrawlerUnreal/OGBrawlerInputCollectionComponent.h` | `buildPlayerInput` |
| 2 | The provider is invoked, once per registered character per tick | A · PT | `<core>SimulationInputResolution.h` | `collectInputForCharacter` (provider branch) |
| 3 | The **raw** capture is recorded, keyed by the tick it was captured at | A · PT | `<core>Network/LocalInputCache.h` | `LocalInputCache::push` |
| 4 | A's own integrator reads the capture from `tick - effectiveDelay` | A · PT | `<core>Network/LocalInputCache.h` | `resolveDelayedInput` |
| 5 | The **undelayed** capture is queued for the wire | A · PT (producer) | `<core>SimulationQueues.h` | `PendingInputQueue::enqueue` |
| 6 | Once per game frame, the send sweep drains the queue's recent window | A · GT | `<core>SimulationNetSync.h` | `sendLocalInputToAuthorityAll` |
| 7 | The redundancy bundle is built from the last `redundancyDepthTicks` entries | A · GT | `Source/OGSimulationUnreal/InputRedundancyBundleBuilder.h` | `buildRedundancyBundle` |
| 8 | One **unreliable** RPC carries the bundle to the authority | A→S · GT | `Source/OGBrawlerUnreal/SimmableUpdateComponent.h` | `ServerReceiveRemoteMove` |
| 9 | The bundle is decoded and each slot is offered to the coordinator | S · GT | `<core>Network/ServerReceptionCoordinator.h` | `receiveInputBundle` |
| 10 | Per slot: domain gate, dedup watermark, park | S · GT | `<core>Network/ServerReceptionCoordinator.h` | `receiveRemoteInput` |
| 11 | The input is parked for `effectiveDelay` ticks | S · GT | `<core>Network/ServerInputDelayQueue.h` | `ServerInputDelayQueue::enqueue` |
| 12 | **The relay tap** — the same delay becomes the schedule stamp `dA` | S · GT | `<core>Network/ServerReceptionCoordinator.h` | `stampFromDelay` |
| 13 | The tap resolves `id` → owning component and stages the entry | S · GT | `Source/OGBrawlerUnreal/SimulationManagerUImpl.cpp` | `ASimulationManagerUImpl::relayRemoteInput` |
| 14 | The entry lands in the **staging** ring, not the replicated one | S · GT | `Source/OGBrawlerUnreal/SimmableUpdateComponent.cpp` | `stageRelayedInput` |
| 15 | …via the ring type's staging entry point | S · GT | `Source/OGSimulationUnreal/RelayedInputRing.h` | `stageArrival` |
| 16 | The whole staged burst is published once per replication poll | S · GT | `Source/OGSimulationUnreal/SimulationInputRelay.cpp` | `ASimulationInputRelay::PreReplication` |
| 17 | …by flushing staging into the replicated ring | S · GT | `<core>RelayedInputRingCodec.h` | `flushStagedInto` |
| 18 | The replicated property lands on the peer | S→B | `Source/OGSimulationUnreal/SimulationInputRelay.h` | `m_relayedInputRing` |
| 19 | The peer is notified | B · GT | `Source/OGSimulationUnreal/SimulationInputRelay.cpp` | `OnRep_RelayedInputRing` |
| 20 | The host actor routes the ring to the character's component | B · GT | `Source/OGBrawlerUnreal/SimmableUpdateComponent.cpp` | `onRelayedInputRingArrived` |
| 21 | The component's bound callback enters the engine-free core | B · GT | `<core>SimulationNetSync.h` | `onRelayedInputReceived` |
| 22 | …through the resolution peer's by-id door | B · GT | `<core>SimulationInputResolution.h` | `ingestRelayRing` |
| 23 | Every entry in the ring is re-consumed, idempotently | B · GT | `<core>Network/RemoteInputCache.h` | `populateRemoteInputCache` |
| 24 | Each `(captureTick, dA, input)` triple is stored, sender-keyed | B · GT | `<core>Network/RemoteInputCache.h` | `RemoteInputCache::push` |
| 25 | On B's next predicted tick, every character is resolved | B · PT | `<core>SimulationInputResolution.h` | `collectInputAll` |
| 26 | The proxy branch asks the scheduled question | B · PT | `<core>SimulationInputResolution.h` | `collectInputForCharacter` (proxy branch) |
| 27 | …answered by the shared, side-effect-free ladder | B · PT | `<core>SimulationInputResolution.h` | `decideScheduledRelayedRead` |
| 28 | …projected to an input plus a diagnostic report | B · PT | `<core>SimulationInputResolution.h` | `resolveScheduledRelayedInput` |
| 29 | The resolved inputs are integrated for the predicted tick | B · PT | `<core>SimulationManager.h` | `onGameSimulationPrediction` |

### 2a. The authority's own application — the branch that leaves the table at hop 11

The relay tap (hop 12) is a **fork**, not a continuation: the input the authority relays is the same
input the authority is still holding. That held copy travels its own short path.

| # | hop | machine · thread | file | symbol |
|---:|---|---|---|---|
| A1 | Once per physics frame, the drain runs for the tick(s) about to be simulated | S · GT | `Source/OGBrawlerUnreal/SimulationManagerUImpl.cpp` | `releaseDelayedInputsForStep` |
| A2 | Every claimed slot releases what is due, or overdue | S · GT | `<core>Network/ServerReceptionCoordinator.h` | `releaseDelayedInputs` |
| A3 | …surfacing the entry's **stored** capture tick, not a reconstructed one | S · GT | `<core>Network/ServerInputDelayQueue.h` | `tryDequeueForTick` |
| A4 | Delivery routes id → component → core | S · GT | `Source/OGBrawlerUnreal/SimulationManagerUImpl.cpp` | `deliverRemoteInput` |
| A5 | …into the SPSC ring the physics thread drains | S · GT (producer) | `<core>SimulationInputResolution.h` | `queueRemoteMove` |
| A6 | The authority's collect dequeues it on the remote branch | S · PT | `<core>SimulationQueues.h` | `dequeueMove` |
| A7 | The tick is integrated | S · PT | `<core>SimulationManager.h` | `onGameSimulationAuthority` |

---

## 3. Three ticks, three meanings — the vocabulary that makes the rest readable

Almost every confusion on this path is a confusion between these three numbers.

**Capture tick.** The tick on A's *prediction clock* at which the sample was taken. It is the identity
of the input, and it is the key of both `LocalInputCache` (on A) and `RemoteInputCache` (on B). It is
stamped by A and never rewritten by anyone.

**Application tick.** The tick at which the authority *applies* it: `captureTick + dA`. The codec
exposes the arithmetic as `applicationTick` (`<core>RelayedInputRingCodec.h`) so that no call site
re-derives it.

**Schedule stamp `dA`.** The effective input delay the authority held for A's wire **at the moment of
receipt**, clamped into a byte by `stampFromDelay`. It rides on the wire beside the capture tick. It
is a *plan*, not a fact: if the wire's tier moves between receipt and application, the stamp records
what was intended when the input was received.

The delay itself is derived identically at both ends. On the client,
`ReplicatedTierConsumer::effectiveInputDelayTicks` (`<core>Network/ReplicatedTierConsumer.h`); on the
authority, `ServerInputDelayQueue::effectiveDelay`. Both route through the same shared helpers in
`<core>Network/ConnectionTierTable.h` — `tierInputDelayTicks` and `applyRelayDelayFloor` — which is
what makes the two ends agree by construction rather than by coincidence. The formula is stated once,
at `effectiveInputDelayTicks`, and is not re-derived here:
`m_hasReceivedTier ? tierInputDelayTicks(m_tierIndex, m_config) : applyRelayDelayFloor(m_config.rttTierInputDelays[kMaxConnectionTierIndex], m_config)`.
⚠ The floor is applied **inside both branches**, not as an outer `max` over the ternary —
`tierInputDelayTicks` floors its own result. And the branch is keyed on **arrival**
(`m_hasReceivedTier`), never on the tier value, because the replicated property defaults to `0`,
which is also a legal tier.

⚠ **The floor dominates the tier.** The host application's configuration sets the floor: one
adapter reads it from `Config/DefaultEngine.ini`, at the `RelayDelayFloorTicks` key under the
`[OGNetcode]` section header, which was `6` when this was read on 2026-08-21. Meanwhile
`rttTierInputDelays`
(`<core>PCTimeManagement/TimeConfig.h`) tops out at 4 — so on that configuration every wire's
effective delay is the floor, every sender is scheduled with the same `D`, and the scheduled read
described in §6 is the *ordinary* regime rather than an exceptional one. Read the ini before assuming
the header default (`relayDelayFloorTicks = 0`) is what a session runs.

---

## 4. A's half — why two different values leave one branch

`collectInputForCharacter`'s provider branch produces **two** values from one provider call, and
keeping them apart is the whole content of the hop:

- **`capture`** — the raw, undelayed sample stamped at the *current* tick. This is what goes on the
  wire (hop 5), and it is what is retained in `LocalInputCache` (hop 3).
- **`applied`** — `resolveDelayedInput(delayLine, tick, effectiveDelay, capture)`, i.e. the capture
  taken at `tick - effectiveDelay`. This is what A's own integrator predicts with.

⛔ **Sending `applied` would double the delay.** The authority applies its own delay to whatever it
receives; handing it an already-delayed input makes the two ends diverge by exactly `effectiveDelay`
ticks. The fence for this lives at the branch itself in `SimulationInputResolution.h` and in
`Network/LocalInputCache.h`'s "WHAT THE OUTBOUND RPC CARRIES" block — the fence is at the site, this
paragraph is only the narrative around it.

Three details worth carrying:

1. **The provider sees history up to `tick - 1` only.** `buildPlayerInput` is handed the delay line
   *before* the current tick's capture is pushed, precisely so that the motion matcher reads history
   and the current sample is the provider's own return value.
2. **The push and the send-enqueue share one gate** — `stepAllocatesFrontierSlot(step.getStepKind())`
   (`<core>SimulationTimeContext.h`). A `Stall` step captures no history and sends nothing.
3. **The pre-session window is a legitimate state.** For the first `effectiveDelay` ticks of a
   session — and for the same window after a resync wipe, §7 — the delayed read lands on a tick that
   was never captured, and `LocalInputCache::at` answers with the **injected** neutral. That neutral
   is `getZeroPlayerInput()` (`Plugins/OGBrawler/…/OGBrawler/SimulatableBrawlerTypes.h`), not
   `InputT{}`: a value-initialised `PlayerInput` carries a `(0,0,0)`
   forward vector into normalisation. The injection happens at the composition root, and
   `NeutralInputFor` (`<core>SimulationInputResolution.h`) carries the flag that turns a missed
   injection into a warning at registration rather than a silent poison.

The RPC is **unreliable**, and that is deliberate: instead of one reliable message per tick, one
bundle per frame carries the last `redundancyDepthTicks` captures, so a dropped datagram self-heals
on the next frame's overlapping bundle. `releaseAllButRecent` keeps the queue bounded to exactly that
overlap window.

---

## 5. S's half — receipt is the fork

`receiveRemoteInput` runs four decisions in a fixed order, and each one has a different consequence
for the relay:

1. **Out-of-domain gate** (`rejectOutOfDomainCaptureTick`) — a capture tick that is not from this
   server's tick domain is refused outright. Nothing is parked, nothing is relayed.
2. **Dedup watermark** (`noteCaptureTick`) — has this id ever sent this capture tick before? The
   answer, `acceptedNew`, is computed *before* the park/fallback split so it covers both.
3. **Park** (`ServerInputDelayQueue::enqueue`) — first-wins on an already-resident capture tick, which
   is what makes a redundancy re-send harmless.
4. **Relay** — reached **only** on `acceptedNew`. Each newer capture tick is therefore relayed
   **exactly once, at receipt**, stamped with the delay the `[Park]` log line reports for that same
   entry.

⛔ **A genuinely-new but out-of-order-*older* capture tick is applied and deliberately NOT relayed.**
It is counted as `relayOooSkipCount` and traced. The reason is a property of the receiving end, not of
the server: every peer derives its probe from the *newest* resident entry (`findLatest`, §6), so
writing an older capture tick would move that anchor **backwards** and mis-schedule every subsequent
read. Peers instead experience a **hole**, which degrades to last-known for one tick and is healed by
the next state anchor. The full statement of that trade — including why the decision is deliberately
reopenable on measured evidence rather than on argument — lives above `receiveRemoteInput`.

There is a fifth path, off to one side: if the delay queue refuses the slot outright (a malformed
player slot, warned once), `receiveInputBundle` delivers that input **immediately and undelayed**
through the same sink the drain uses — so no player input is ever silently dropped. The
out-of-domain rejection above is the one case that is dropped, deliberately: delivering it undelayed
would defeat the gate that rejected it.

**Staging, and why it exists.** The tap does not write the replicated ring. `stageRelayedInput` puts
the entry in a *staging* ring, and a pre-replication hook flushes the whole staged burst into the
replicated property once per replication poll (one adapter's binding: `PreReplication`). Without that, two arrivals inside one server frame would collapse
to one on the wire — the second overwriting the first in server memory before the poll ever compared
it. The ring's capacity is the codec constant `kMaxDepth` (`<core>RelayedInputRingCodec.h`); no depth
parameter reaches `stageArrival`, deliberately, so no caller can silently reduce the flush back to
replace-latest.

**The ring is a per-connection object, not a per-character property.** The transport actor named in
§2 owns the replicated ring; the character component keeps the accessor pair and the callback pair
that the engine-free core sees, and never learns that the ring lives on a different object
altogether. That is why hop 20 exists at all — it is a pure forwarder. (One adapter's binding:
`ASimulationInputRelay` in `Source/OGSimulationUnreal/` owns the replicated `FRelayedInputRing`.)

---

## 6. B's half — the scheduled read, and what happens when nothing arrived

`RemoteInputCache` is a 64-slot ring keyed by **A's capture tick** (`kRemoteInputCacheCapacityTicks`,
`<core>Network/RemoteInputCache.h`). Its slots hold `(captureTick, dA, input)`. Ingest is
**idempotent** — `populateRemoteInputCache` re-consumes the entire ring on every arrival rather than
diffing, which is what makes a redundant re-replication free and a missed one recoverable.

The read is `decideScheduledRelayedRead(store, tick)`, and it is a five-rung ladder. Reading it as a
ladder is the point: **each rung is a different diagnosis of the same fallback.**

| rung | condition | outcome | what B integrates |
|---|---|---|---|
| 0 | `findLatest()` is invalid — nothing has *ever* arrived for this character | `NoProbe` | `fallback()` |
| 1 | `tick < dLatest` — the session is younger than the schedule | `Miss`, class `NoProbeTick` | `fallback()` |
| 2 | `find(tick - dLatest)` misses | `Miss`, class `AboveNewest` / `InSpan` / `BelowOldest` | `fallback()` |
| 3 | hit, but `candidateDA != dLatest` | `VerifyFail` | `fallback()` |
| 4 | hit, and the stamps agree | `Hit` | **the sender's real input** |

**The question answered directly: what does a proxy do on a tick for which no remote input has
arrived?** It integrates `RemoteInputCache::fallback()` — the *newest resident entry's* input, i.e.
last-known-held. If nothing has ever arrived for that character, `fallback()` returns the store's
injected neutral (the game's real zero input, not `InputT{}`). If the character has no store at all —
which is legitimate, because a character can be iterated before its registration completes — the proxy
branch substitutes the neutral directly, and that case is deliberately **not** counted as a read:
there was no probe to hit or miss, and folding it in would make an unregistered proxy look like
starvation.

⭐ **There is one code path and no regime flag.** At relay delay floor 0 the probe nearly always
misses — nothing was scheduled to make the sender's capture for tick `N` arrive in time — and the
ladder degenerates to last-known, which is byte-for-byte the pre-relay behaviour. At a high floor it
nearly always hits and the proxy consumes the authority's actual schedule, tick for tick. **Nothing
selects between these.** The regime is decided per input, per receiver, by whether the data is there,
which is why raising the floor needs no second implementation and no switch. "Nearly always" is
deliberate at both ends: a hit at floor 0 is legitimate, not a bug — a WAN sender stamped against a
LAN receiver can satisfy the schedule, and the answer is then the authority's real scheduled input
rather than a stale hold.

⭐ **Rung 3 is a diagnosis, not a failure.** A resident candidate stamped against a delay that is no
longer the current one means the delay *regime* shifted under the reader — a transition, not
starvation. Same fallback, completely different cause, and that split is why the counters are worth
reading at all.

---

## 7. ⛔ The wipe asymmetry — the centrepiece

`LocalInputCache` and `RemoteInputCache` are symmetric names. **They do not have symmetric lifetimes**,
and the type names cannot carry that.

On a hard resync, `wipeAllForResync` (`<core>SimulationInputResolution.h`) clears the pending input
queues and the local input caches — and **deliberately does not touch the remote input caches**. The
reason is entirely about *whose clock keys the container*:

- `LocalInputCache` is **clock-keyed**. Its keys are this machine's own prediction-clock tick numbers.
  A hard resync jumps that clock, so every surviving capture describes a tick that no longer means
  what it meant, and the delayed read would serve the wrong sample for `effectiveDelay` ticks.
  Dropping them re-enters the neutral-filled window — the same well-defined state as session start,
  which is exactly why the session-start case needs no special-casing.
- `RemoteInputCache` is **sender-keyed**. Its keys are *A's* capture ticks — a server-domain identity
  produced by another machine's clock, which a resync of B's clock does not touch. Wiping it would
  blind every remote proxy for a window after every resync, for no benefit. That blindness was
  observed on 2026-08-04 and is recorded at the non-wipe comment inside `wipeAllForResync` and in
  `Network/RemoteInputCache.h`'s naming block.

⛔ **The loop above the non-wipe comment is the obvious thing to mirror.** That is precisely why the
comment is there and why it must stay there: a reader adding a third per-id map loop to that function
is one keystroke away from the 2026-08-04 failure. This section is the narrative; the guard is the
comment at the site, and moving the guard here would disarm it.

There is a **second consequence** of the local wipe that is easy to misread as a bug: the delay line
is also the motion matcher's history source, so a hard resync blanks the matcher for up to
`kHistoryWindowFrames` ticks. That is correct. The correction cache it would otherwise have read was
keyed to the pre-resync clock too, so a motion "matched" out of it would have been assembled from
ticks that no longer mean what they meant. Going cold is the honest answer.

---

## 8. Which thread each hop runs on

Grouped, because the pattern matters more than the per-row answer:

- **A, PT:** hops 1–5. Provider call, capture push, delayed read, send-queue enqueue. The
  `LocalInputCache` push and read are two statements inside one `collectInputAll` call, which is why
  that container's own contract says *not thread-safe, single-threaded by construction*. ⛔ That
  sentence is TRUE of `LocalInputCache` and FALSE of `RemoteInputCache`; the fence forbidding the copy
  lives in `Network/RemoteInputCache.h`.
- **A, GT:** hops 6–8. The send sweep runs from `onPostSimulationGameThread`, reached from the
  adapter's `OnPostPhysicsStep` — a game-thread hook, which is what makes the RPC legal to fire.
- **A, PT→GT crossing:** hop 5 → hop 6. `PendingInputQueue` is a Lamport SPSC ring — physics-thread
  producer, game-thread consumer. This is the shape `ThreadingCrossings.md`'s standing rule points
  every *new* crossing at.
- **S, GT:** hops 9–17 and A1–A5. The RPC handler, the coordinator, the tap, the staging ring and the
  drain are all game-thread. `releaseDelayedInputsForStep` runs from the host engine's game-thread
  hook immediately preceding the physics step — one adapter binds that to Chaos's
  `InjectInputs_External`.
- **S, GT→PT crossing:** hop A5 → A6. `RemoteMoveQueue` is the second staged ring — game-thread
  enqueue, physics-thread dequeue. ⚠ Unlike `PendingInputQueue` it carries **plain** `size_t`
  head/tail/count, not atomics (`<core>SimulationQueues.h`); read the declarations before treating
  the two rings as the same shape.
- **B, GT:** hops 19–24. The OnRep, the forwarders, the core ingest and the `RemoteInputCache` write.
- **B, PT:** hops 25–29. Resolution and integration, entered from the adapter's pre-simulate
  physics callback (one adapter's binding: `OnPreSimulate_Internal`).
- **B, GT→PT, and it is *not* a queue:** hop 24 → hop 26. `RemoteInputCache` is written on the game
  thread and read on the physics thread with **no seam**. This is **row 1** of
  `ThreadingCrossings.md`, priced there as accepted, correction-healed debt; the analysis of record is
  that header's own `THREADING — GAME-THREAD WRITE / PHYSICS-THREAD READ. ACCEPTED DEBT.` block. It is
  not re-derived here.
- **B, GT→PT, correctly synchronized:** the effective delay itself.
  `setClientEffectiveInputDelayTicks` writes one `std::atomic<int32>` from the tier OnRep;
  `collectInputAll` loads it **once per tick**, so a concurrent tier change cannot split a single tick
  across two different delays. This is row 7 of `ThreadingCrossings.md`.

⚠ The core's step functions are named `onGameSimulation*` for historical reasons and run on the
**physics** thread. The adapter hook that genuinely runs on the game thread is
`onPostSimulationGameThread`.

---

## 9. The resim mirror — why the same ladder is called twice

When a correction lands and disagrees, B replays the ticks between the anchor and the frontier through
`collectResimInputAll`. For a remote character, the replay is a five-rung table of its own, and its
**last** rung calls the very same `resolveScheduledRelayedInput`.

That sharing is load-bearing. **A resim that resolved a frontier tick differently from the prediction
that produced it would manufacture divergence out of nothing.** The fence saying so sits above the
proxy branch (*"Do not inline a second copy of the ladder here"*) and is restated at the ladder
itself.

The rung above it is the interesting one. The correction state carries an **applied-capture-tick
reference** — what the authority actually applied at that tick — read back as
`getAppliedCaptureTickRef` (`<core>SimulationReconciliation.h`). When that ref is a real capture tick,
**the ref wins over the schedule**: the schedule is the intended plan, the ref is what the authority
has already ruled on. The stamp is bound only because `find` requires an out-param, and its value is
never read. When the ref is the sentinel `kNoInputCaptureTick`
(`<core>CorrectionStateBufferCodec.h`), the authority is saying *I applied no client capture here* —
and the value it substituted is the injected game zero, so that is what the replay applies. The
sentinel is **never** a store key: `RemoteInputCache::push` rejects it and `find` refuses to look it
up, so a sentinel can never resolve into a real slot.

Note what the sentinel means on the authority side, because it is the same fact seen from the other
end. Hop A6's `dequeueMove` on an **empty** queue returns a value-initialised move whose tick is 0 —
and 0 is an ordinary capture tick at session start. So the *returned* tick cannot distinguish "the
authority substituted" from "the authority applied the tick-0 capture". Only the pre-dequeue `empty()`
check can, which is why the underrun flag is taken first and the returned tick is never used to infer
it. On underrun the authority integrates the **injected neutral** and stamps the sentinel as the ref —
which is precisely what the resim rung above reads back. This is not a loss-only path: it runs for
every tick between an authority registration and that client's first input arriving, i.e. on every
join window, in ordinary play.

---

## 10. What this document does not answer

Stated plainly rather than papered over.

- **How long an entry stays useful, in one place.** The residency bound is a composition of
  `kRemoteInputCacheCapacityTicks`, `rollbackWindowHardCap` and the wire delay, and the derivation of
  record is `relayDelayFloorHardCapTicks` in `<core>Network/ConnectionTierTable.h`. This document
  points at it deliberately and does not mirror the formula, because a mirrored formula is exactly the
  artefact that rots.
- **The measured hit rate of the scheduled read.** The probes exist (`ScheduledRelayedReadReport`,
  `<core>Network/RelayReadProbe.h`), and the prediction and resim counters are split on purpose, but
  this document records no measurement. A number here without a run date and a config would be worse
  than the absence.
- **Whether the accepted GT→PT debt on `RemoteInputCache` should be paid.** `ThreadingCrossings.md`
  owns that question and names the SPSC shape that would retire it; this document only says where the
  crossing is.
- **What a peer observes at ring depths above 1.** The staging path is built for a burst and
  `kMaxDepth` is 8, but what actually reaches a peer at each depth is a measurement question, not a
  reading question, and it is not answered here.
- **Listen-server topology.** This document follows three distinct roles. A listen server runs a
  prediction manager and an authority manager as two instances, and this narrative does not describe
  how a locally-hosted player's input takes both paths at once.
- **How long last-known can persist.** §6 says a starved proxy holds the newest resident entry. It
  does not say for how long, because **nothing bounds it**: `fallback()` deliberately takes no tick
  (`<core>Network/RemoteInputCache.h`), and slots are reclaimed by overwrite rather than by age, so
  the hold has no expiry. A bounded hold-K-then-game-zero was designed and deferred — the reasoning
  is on record in `RelayDelaySpectrumDesign.md` §8.7, and the ⛔ note above `fallback()` states it at
  the site along with *"Do not add a `frontierTick` parameter here 'for later'"*. Named here because
  a reader who asks the question deserves to find the deferral rather than an absence.

---

## 11. Keeping this document true

This document follows the initiative's tense rule: **a statement about the current state of code or
config carries a grep-able anchor (`file:symbol`) or is written closed-tense with a date.** Every
symbol in §2 and §2a was verified present by grep on 2026-08-21; the grep output is recorded in
`impl_notes_wave1_1.md` of the `og-source-doc-extraction` initiative.

Three consequences for anyone editing this file:

1. **If you rename a symbol, this file breaks loudly.** That is the intent. The trace table is a list
   of grep targets, not prose.
2. **Do not move a fence into this file.** Everything marked ⛔ above is a *summary* of a guard that
   stays at its site. If you find yourself deleting the comment because "it is in the perspective doc
   now", the guard has stopped working — the person about to make the change is reading code.
3. **If this file and a header disagree, the header wins.** Fix this file; do not soften the header.
