<!-- SPDX-License-Identifier: MPL-2.0 -->
# `RemoteInputCache.h` — rationale

This is the narrative, the derivations and the deferral history for `RemoteInputCache` and its
ingest, `populateRemoteInputCache` (`Network/RemoteInputCache.h`). The header keeps the orientation
block, every per-member contract and every fence; this file carries everything else. The header is
the operational reference — read it first. Come here when you need the *why*, not the *what*.

**If this file and `Network/RemoteInputCache.h` disagree, the header is authoritative and this file
is stale.** Fix this file; do not soften the header to match it.

⛔ **This file is not the source of truth for any VALUE.** Compiled constants live in the header's
initialisers. Where a number appears below it is there to make an argument readable.

⛔ **Do not move a fence into this file.** Everything marked ⛔ in the header is a guard at the site
where the mistake gets made. A summary here is a summary; the guard is the comment in the code.

**Read alongside:** `Perspective-RemoteInputFlow.md` — the end-to-end narrative this store sits in
the middle of, and the owner of the wipe-asymmetry *narrative* (§7 there). `ThreadingCrossings.md`
row 1 — the GT→PT crossing this store is. `RelayProbes-rationale.md` — the probes fed by the ingest
report. `ConnectionTierTable-rationale.md` §5 — the derivation that consumes this store's capacity.

Origin: `og-netcode-v2-input-relay` items T5, T6, T7, T9, T19, T20, T34, T39 and task 79, the
design note `RelayDelaySpectrumDesign.md` §4 / §5.2 / §5.3 / §5.3a / §6 / §8.2 / §8.6 / §8.7, the
architect rulings of 2026-08-04, and the `og-source-doc-extraction` extraction of 2026-08-23.

> ⚠ **Every document named in that Origin line is private working material from the
> `og-netcode-v2-input-relay` initiative archive and is NOT distributed with this submodule.**
> They are named as provenance, deliberately unlinked; every claim this file *asserts* is anchored
> to a file in this repository and only to those. The declarations below tell
> `tools/lint/doc_anchor_lint.ps1` that these names are intentionally unresolvable.

<!-- lint-external-ref: RelayDelaySpectrumDesign.md -- og-netcode-v2-input-relay initiative archive; private working material, not distributed with this submodule -->
<!-- lint-external-ref: RemoteInputCacheTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->
<!-- lint-external-ref: NetSyncTelemetryTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->
<!-- lint-external-ref: SimulationInputResolutionTest.cpp -- test translation unit in the og-brawler-tests submodule, not distributed with this submodule -->
<!-- lint-external-ref: ASimulationInputRelay::OnRep_RelayedInputRing -- one adapter's replication callback, in the OGSimulationUnreal subtree, not in this engine-free library -->
<!-- lint-external-ref: OnRep_CorrectionState -- one adapter's replication callback, in the adapter subtree, not in this engine-free library -->
<!-- lint-external-ref: RelayedInputStore -- RETIRED NAME: this type's name before the 2026-08-16 rename to RemoteInputCache; it must not resolve -->
<!-- lint-external-ref: relayStaleInputHoldTicks -- A NAME THAT HAS NEVER EXISTED: the deferred stale-hold window (§5). The absence is the fence; if this ever resolves, the deferral has landed and §5 is stale -->
<!-- lint-external-ref: prepareSimulationStep -- RETIRED NAME: collectInputAll carried it between items 90 and 94 only; §10 records the correction and it must not resolve -->
<!-- lint-external-ref: getWireFormatVersionOnWire -- A NAME THAT HAS NEVER EXISTED: §10 F-38-2 records that the header named it for the real getWireFormatVersion; it must not resolve -->
<!-- lint-external-ref: m_lastUsedInputs -- RETIRED NAME: the per-character last-applied-input map, removed at T8; its surviving twin is m_lastUsedCaptureTicks -->
<!-- lint-external-ref: ClientInputDelayLine -- RETIRED NAME: LocalInputCache's name before the 2026-08-16 rename; it must not resolve -->
<!-- lint-external-ref: SimulationInputResolution::prepareSimulationStep -- RETIRED NAME, QUALIFIED SPELLING: section 10 F-38-1 quotes the header's old claim in order to correct it. The bare escape does not cover the qualified anchor, which is task 33's F-33-8 mechanism; it must not resolve -->

---

## 1. Why this is not a `LocalInputCache` — three facts, and the third is decisive

The first plan was to reuse `LocalInputCache` for the receive side. Three accumulated facts flipped
that trade. The header states all three as one-line fences; this is the argument behind them.

**What the two DO share, so the differences are visible against it.** `push` is last-wins on an
already-resident capture tick, matching both `LocalInputCache::push` and the wire codec's
`writeLatest`, which explicitly permits REWRITING a resident tick with a fresher `dA`. That shared
rule is what makes the ingest idempotent.

**1 — the payload differs.** A `LocalInputCache` slot holds an `InputT`. A slot here holds
`(dA, input)`: the input *and* the schedule stamp the server applied to it. Reuse would already have
meant a widened slot plus sidecar fields, i.e. a second type wearing the first one's name.

**2 — the miss semantics differ, and the difference is load-bearing.** `LocalInputCache::at()`
INVENTS the neutral on a miss, which is right for a delay line: a read at `T - d` before `d` ticks
have elapsed genuinely *is* neutral. `RemoteInputCache::find()` is pure hit/miss and never invents
anything. The scheduled-read ladder must SEE the miss in order to fall back to last-known, and the
resim resolution table must see it in order to distinguish "the sender's input has not arrived" from
"the sender sent a neutral-content input". A store that returned a neutral on a miss would collapse
those two into one answer, permanently. There is no neutral on the lookup path at all — the injected
zero is reachable only through `fallback()`, which is a different question.

**3 — the wipe diverges, and that is what makes reuse UNSAFE rather than merely awkward.** See §7.
`LocalInputCache::clear` is contractually swept by `SimulationInputResolution::wipeAllForResync`;
this store has no such surface at all. A reused type would be swept by whoever next mirrored the
per-id map loops in that function — silently — and the only symptom would be a proxy that goes blind
for a window after every resync.

**Combined slot, never parallel rings.** `dA` and `input` live in ONE slot keyed by ONE capture
tick. Two parallel rings would defeat the ladder's verify step, which checks a candidate tick
against ITS OWN stamp — with two rings it would be checking ring-A's tick against ring-B's stamp,
and it would do so *silently*, because both rings would answer.

---

## 2. The capacity constant, and why it is not private

`kRemoteInputCacheCapacityTicks` is a FREE constant, exactly like `kLocalInputCacheCapacityTicks`
(`Network/LocalInputCache.h`) and for the same reason: configuration-layer code must be able to
derive bounds from it without naming an arbitrary `InputT` to reach a static member of a class
template. A `RemoteInputCache<SomeGameInput>::kDefaultCapacityTicks` would force the config layer to
know a game type.

**It is load-bearing outside this file.** It is one of the two capacities
`relayDelayFloorHardCapForCapacities` (`Network/ConnectionTierTable.h`) takes the minimum of, so
lowering it lowers the ceiling on the configurable relay delay floor. In one sentence: an entry must
stay resident until the frontier passes `captureTick + dA + rollbackWindowHardCap`, and is evicted at
about `captureTick + capacity + wire`, so `dA <= capacity - rollbackWindowHardCap`. The derivation lives there,
deliberately, and is not mirrored here — a mirrored formula is exactly the artefact that rots.
`ConnectionTierTable-rationale.md` §5 carries the argument.

---

## 3. "Latest" is derived, not stored

`findLatest()` — a scan of the occupied slots for the highest capture tick — IS the truth. The
design vocabulary's `dLatest` and `lastKnown` are VIEWS on that derivation (`findLatest().dA` and
`findLatest().input`, the latter surfaced as `fallback()`). They are not fields, and there is no
second source of truth.

**Why this is a ruling and not a preference.** A stored "latest" scalar needs a `>=` update rule,
not a `>` one. `push` is last-wins, so a re-stamp of the ALREADY-latest capture tick with a fresher
`dA` must overwrite the scalar. Get that wrong and `dLatest` goes stale precisely *during a delay
transition* — the one moment it matters, and the one moment nobody is watching a scalar. Deriving
deletes the invariant outright: `push` rewrites the slot in place and the derivation reads the
slot's current `dA`.

**Cost.** At most `capacity()` comparisons. The only site that could ever matter is the resim's
frontier ticks inside a deep replay (≤ ~20 replayed ticks × N characters).

**If a profile ever justifies a memo** it must be `private`, `mutable`, co-located immediately with
`findLatest()`, refreshed ONLY inside the mutators, never writable from outside, and it must carry
the header comment *"this is a memo; if it and findLatest() disagree, findLatest() is right"*. It
may not land without a randomized-equivalence test.

**One scan, shared.** `fallback()` and `findLatest()` both go through the private `latestSlotIndex`
precisely so that "one derivation" stays literally true rather than being two functions that happen
to agree today. `residentSpan()`'s `newest` is by construction the same maximum.

**Plain `>` is correct in that scan** because the relay stream is MONOTONIC in capture tick by
construction: the server relays only on `parked && acceptedNew`, so an out-of-order-older input is
never sent. Wraparound of the tick counter itself is out of scope for the same reason the sentinel
is safe to reserve — 2³² ticks at 60 Hz is ~828 days of continuous session.

---

## 4. Sentinel, initial state, and the injected neutral

**`kNoInputCaptureTick` is never a store key.** `push` rejects it outright and `find` refuses to
look it up. This is defensive — the relay path only ever carries real capture ticks — but the store
must not be poisonable by a future feeder, because the semantic layer above resolves a sentinel ref
to the game zero and must never see it become a successful lookup. Reserving the value costs
nothing.

**`!findLatest().valid` must mean NO SCHEDULED PROBE AT ALL**, not a probe with a default `dA` of 0.
`find(N)` can genuinely HIT for a LAN peer; the ladder's verify step would then reject it on the
stamp, so it is not a correctness hole today — but *"accidentally safe because a later check catches
it"* is not a contract, and the layer above states and honours the skip.

**The neutral is injected, never `InputT{}`.** This is the same load-bearing distinction
`LocalInputCache` documents. The game's real zero (`simulatableBrawler::getZeroPlayerInput()`)
builds aim vectors of `(0,0,1)`, while a value-initialised game input would carry a `(0,0,0)` vector
into normalisation. The `InputT{}` default in this header's constructor exists purely so an
engine-free unit test can construct a store without a game type;
`SimulationInputResolution::setNeutralInput` injects the real one into every store, and
`setNeutralInput` on the store itself exists so the composition root need not order itself before
every registration.

---

## 5. `fallback()`, and the deliberately absent hold rule

`fallback()` is ARGUMENT-LESS: the newest arrived input if anything ever arrived, otherwise the
injected game zero. It takes NO `frontierTick`, and there is no `relayStaleInputHoldTicks`. **That
absence is a ruling, and the header states it as an absence fence at the site** — a reader about to
add the parameter is about to *write* the name, not read it, so there is nothing for them to grep.

**Why deferring is the safe choice, not the lazy one.**

- Without a hold window the store needs no knowledge of the caller's clock, which keeps it clear of
  the receiver-frontier-versus-sender-capture-tick geometry entirely (two different leads over the
  server, roughly 5–15 ticks of noise). The future rule adds the parameter; pre-adding it unused
  would invite someone to feed it a tick from the wrong domain.
- Unbounded hold is ALREADY today's behaviour — the proxy branch reads the correction cache's last
  input, which persists indefinitely if corrections stop. Deferring therefore *preserves* the
  increment's no-observable-regression gate rather than risking it.

**The deferred rule, so it is not lost:** hold last-known at most K ticks, then fall to the game
zero.

**Why it matters.** Relay silence does NOT mean "the player is idle". The input provider is polled
every tick, so a motionless player still emits a full-rate stream of neutral-*content* captures.
Silence means the WIRE is quiet — starvation, a loss burst, pre-registration, a disconnect in
progress — and the corrections that normally bound extrapolation share that same starving
connection. Holding a stale *"moving forward + attacking"* input indefinitely is the phantom-hit
class. Eviction does not bound it by itself: slots are reclaimed by OVERWRITE, never by time, so a
silent relay evicts nothing at all.

**It becomes load-bearing at sparse state** — a longer heal interval is a longer hold window — so it
is a named prerequisite of that increment rather than optional polish. `RelayReadProbe.h`'s
`maxConsecutiveFallbackRun` records the longest observed run, so K can be set from data rather than
from taste.

---

## 6. Threading — the accepted GT-write / PT-read debt

**This section is the analysis of record.** `ThreadingCrossings.md` row 1 and
`Perspective-RemoteInputFlow.md` §8 both point here rather than re-deriving it; the header keeps the
prohibitions.

**The crossing.** The adapter's relayed-ring arrival callback fires on the GAME thread and routes
the ring into `populateRemoteInputCache`, which writes slots. `collectInputAll` and
`collectResimInputAll` (`SimulationInputResolution.h`) read those slots on the PHYSICS thread, i.e.
wherever the host ticks physics asynchronously. There is no seam between the two.

⛔ **`LocalInputCache`'s "not thread-safe; single-threaded by construction" sentence is TRUE for the
delay line and FALSE for this type**, and the header fences against copying it across along with the
ring mechanics. It is true there because that container's push and its read are two statements
inside one `collectInputAll` call. A false threading contract is worse than none.

**Why it is accepted rather than fixed here — four reasons, in order of weight.**

1. **It is inherited, not introduced.** The correction-state arrival callback (one adapter's
   binding: `OnRep_CorrectionState`) is likewise game-thread and reaches `injectCorrectionState`
   with no queue and no seam, while the same two physics-thread readers read the correction cache.
   The same inheritance was recorded for `m_lastUsedInputs` and, after that map was retired, for its
   surviving twin `m_lastUsedCaptureTicks`. This store widens nothing.
2. **The failure mode is a TORN SLOT, not container UB.** Storage is a fixed-size slot array — as
   `StateCorrectionCache` (`CorrectionCache.h`) and `RemoteMoveQueue` (`SimulationQueues.h`) both
   are — so there is no rehash to race. This is emphatically not the situation that forced an
   earlier restructuring elsewhere. The worst case is one bad input, on one proxy, for one tick.
3. **Its worst concrete shape, stated plainly rather than left abstract:** a torn forward vector can
   transiently read near-zero, producing one tick of degenerate (or NaN) state on that proxy.
4. **It is correction-healed, including on a frontier tick.** The heal is the STATE anchor, not
   corrected-input replay: a torn read on a frontier tick mispredicts, but at every-frame correction
   cadence that tick is itself corrected shortly after — and the resim then re-reads the store, by
   which time the write has long since completed, because tears are transient. Hit detection is
   server-side, so a degenerate proxy tick cannot adjudicate anything. **The only escape is
   corrections STOPPING**, which is the already-named starving-wire class of §5 and already a
   recorded sparse-state prerequisite.

**The deferred cleanup, recorded here and not done here.** The proper fix is this codebase's own
seam pattern: have the arrival callback decode into an SPSC ring (game-thread producer) drained at
the top of `collectInputAll` (physics-thread consumer), exactly as `RemoteMoveQueue` does the
server-side GT→PT crossing and `PendingInputQueue` the client-side PT→GT one. That would make this
store genuinely physics-thread-only, and would make the cribbed sentence true. It is deliberately
out of scope because fixing only the relay store while the correction cache retains the identical
pattern buys little — **the whole receive path should move together.**

---

## 7. The wipe asymmetry — where the narrative lives, and what stays in the code

`Perspective-RemoteInputFlow.md` §7 is the narrative: both keying domains, each cache's place in the
relay path, and what a wrong wipe costs a proxy downstream. **It is not re-derived here, and it must
not be re-derived here** — a fact with three homes is a fact with three drift rates.

What this section owns is the *shape* of the guard, because it is the highest-risk fence in this
file and the reason is a naming accident:

> `LocalInputCache` and `RemoteInputCache` are symmetric names. **Their lifetimes are not
> symmetric.** The retired `ClientInputDelayLine` / `RelayedInputStore` pair encoded that asymmetry
> *by accident*; the 2026-08-16 rename removed the accident and left the prose as the only carrier.

⛔ **So the header's fence must read as a PROHIBITION, not as a label.** A block that merely says
*"this cache is sender-keyed and is not wiped"* is a description; the reader who is about to add a
third per-id map loop to `wipeAllForResync` needs to be told **not to**, told what it costs, and
told that a test will stop them. The header's fence carries all three, and this file records why
that is not over-writing.

**It is enforced, and the enforcement is the reader's grep handle.** A wipe fails the suite rather
than the session:

| what is pinned | where |
|---|---|
| the store has no `clear()` **at compile time** — a `HasClearMethod` concept, asserted true for `LocalInputCache` and false for `RemoteInputCache` | `RemoteInputCacheTest.cpp` |
| the store has no wipe surface for a resync to sweep | `RemoteInputCacheTest.cpp`, *"the store has no wipe surface to be swept by a resync"* |
| the local caches ARE cleared by the real `wipeAllForResync` | `SimulationInputResolutionTest.cpp`, `ResyncWipeClearsTheLocalInputCache` |
| the remote caches SURVIVE the real `wipeAllForResync` | `SimulationInputResolutionTest.cpp`, `RemoteInputCacheSURVIVESAHardResyncWipe` |
| a resim still resolves a remote input after a hard resync | `SimulationInputResolutionTest.cpp`, `ResimRemoteStillResolvesAfterAHardResyncWipe` |

The compile-time half is the strongest of the five and the least obvious: **the type cannot grow a
`clear()` without breaking a `static_assert`.** That is why the container-level test is worth having
even though the end-to-end cases exist — the end-to-end cases prove the current `wipeAllForResync`
behaves; the `static_assert` proves a *future* one cannot misbehave.

⚠ **A stale-ownership note, routed not fixed.** `Network/LocalInputCache.h`'s twin fence still
attributes `wipeAllForResync` to `SimulationNetSync`. It moved to `SimulationInputResolution` at
item 87. That file is owned by no task in this wave; see §10, R-38-b.

---

## 8. The ingest report's three counters

`populateRemoteInputCache` returns a `RelayedInputIngestReport`. Three of its fields exist for
instruments outside this file, and each closed a specific defect.

**`entriesIngested`** — entries PUSHED. Under the re-consume ingest that includes every
already-resident entry the ring carried again, so it is a measure of *work*, not of *coverage*.

**`newCaptureTicksIngested`** — entries whose capture tick was NOT resident when the arrival began,
i.e. the NEW COVERAGE this arrival delivered. It is what `RelayArrivalProbe::noteArrival` charges
loss against.

> **Why the distinction is load-bearing.** Under the retired replace-latest write path one arrival
> carried exactly one new capture tick, so *"the newest watermark advanced by g"* and *"g − 1 ticks
> were lost"* were the same statement. Under flush-on-poll ONE ARRIVAL CARRIES THE WHOLE BURST: a
> 2-entry burst advances the watermark by 2 while losing nothing, and a probe that assumes 1 charges
> the burst RATE as loss. The measured consequence on the archived 2-character flush run was ~120
> per mille reported against a ~11 per mille pass condition, on a flush that was working perfectly.
> `RelayProbes-rationale.md` §8 carries the full derivation and the arithmetic; it is not repeated
> here.

**Re-delivery is not coverage.** A ring that carries a tick this store already holds — a `dA`
re-stamp, or the flush republishing an entry that had not yet been evicted from the ring — adds
nothing, so it is deliberately not counted. Counting it would let a stalled sender re-delivering the
same entry look like a sender delivering new ones. The check is free: `has()` is an O(1) slot probe
and the ingest already visits every entry, so it costs one comparison per entry and no second walk.

**`newestCaptureTick` / `newestCaptureTickValid`** — the newest capture tick THIS ARRIVAL carried,
and the replication-cadence probe's entire data source (`Network/RelayReadProbe.h`, probe 2).

> **Why it lives here rather than being re-derived at the call site.** The ingest already walks every
> entry, so the maximum is free, whereas a caller would have to walk the ring a second time or call
> `store.findLatest()` — **and `findLatest()` is the wrong answer.** It scans the STORE, which holds
> up to 64 accumulated capture ticks, so on an arrival that carried nothing new it would keep
> reporting the *previous* arrival's tick and the cadence gap would silently read 0 forever. The
> quantity the probe is about is "ticks between successful relay-ring arrivals", and only the ring's
> own maximum answers it.

`valid` is a separate flag rather than a sentinel value because **tick 0 is a perfectly ordinary
capture tick**. It is false on `NeverWritten`, on `VersionMismatch`, and on the degenerate case of a
version-matching ring whose every entry was rejected by `push`.

**The resident span**, and why it is a third answer rather than a variant of the first two:
`find(probeTick)` answering false says a scheduled read MISSED; it does not say WHY, and the three
whys have three different remedies. A probe tick inside `[oldest, newest]` but absent is a COVERAGE
HOLE — produced by the sender and clobbered before it could be replicated — and the remedy is depth.
Above `newest` is a delay deficit and depth is irrelevant. Below `oldest` is clock misalignment or
capacity. **The span is the STORE's, not the RING's**, and conflating the two would invalidate the
classification: the ring carries `depth` entries per replication while this store ACCUMULATES up to
`capacity()` arrivals, which is exactly why an in-span hole is meaningful even at depth 1.

---

## 9. The wire-version fence, and the gate that left

**The version is checked before any structural read.** The wire ships a format version
(`relayedInputRing::kWireFormatVersion`, read back with `relayedInputRing::getWireFormatVersion`),
but one adapter's property serializer only LENGTH-checks. Entry stride is a compile-time constant
per `InputType`, so a layout change makes an old peer read arbitrary bytes as `captureTick` / `dA` /
`input`: plausible-looking garbage capture ticks inserted as STORE KEYS and then applied to a proxy.
Silent corruption, no crash. `entryCount` lives in the same header and is equally untrustworthy on a
mismatch, and a stride mismatch would send `forEachEntry` off the end into the buffer's own fatal
bounds check.

**Three-way, and version 0 is deliberately silent.** The codec writes its header lazily, so an empty
ring reads 0 and can never false-positive as a mismatch. A matching version consumes. Anything else
drops the whole ring and consumes nothing.

**Drop at consume — do not fail the archive.** The two checks are asymmetric on purpose:

| failure | why the reaction differs |
|---|---|
| bad LENGTH prefix | The property cannot know how many bytes to consume, so the archive position desynchronizes and every subsequent property in that bunch parses garbage. Failing the bunch is then *mandatory*, and that is what the wire layer does. |
| VERSION mismatch | Discovered AFTER the property serializer completed cleanly: the byte count was consumed exactly, the archive is still in sync, and unrelated properties in the same bunch deserialize fine. Failing the bunch there would destroy them for no integrity gain. Concretely, at the arrival/populate site the archive's failure switch is not even available — serialization has already completed. |

**The unstated invariant that argument rests on, now stated:** the u16 LENGTH-PREFIX FRAMING IS
VERSION-INVARIANT FOREVER. Only the payload layout *behind* the prefix may change across wire
versions; the prefix itself may never. Without that, *"the byte count is correct on a mismatch"* is
unfounded and the whole drop-rather-than-fail asymmetry collapses.

**Ingest re-consumes everything, no diffing.** Every resident entry is pushed, and `push` is
idempotent by capture tick, so re-consuming an entry that is already resident simply rewrites it —
which is also how a re-stamp lands. Cost is O(depth) -- 1 today, at most `relayedInputRing::kMaxDepth` ever -- and the identical code
stays correct unchanged when depth rises, which is the point.

**The one-shot mismatch log gate LEFT this class.** `shouldLogVersionMismatchOnce()` used to be a
store member — one shot per store, i.e. per character, per session. It is now
`NetSyncTelemetry::shouldLogVersionMismatchOnce(id)`, id-keyed instead of store-resident.
Log-suppression state belongs with the logger, not with the data container being logged about, and
handing the telemetry sibling a reference to the production store so it could call the old method
would have been exactly the production-container coupling that split exists to avoid. Its four
assertions moved to `NetSyncTelemetryTest.cpp`, plus one that could not be asked before: a second id
opens its OWN gate independently of the first.

---

## 10. R0 corrections — claims this header made that the tree does not support

Every factual claim in the pre-compression header was checked against the tree before it was
compressed, because **compression makes a false statement shorter, denser and more authoritative,
and coverage, the subject gate and the doc-anchor lint all pass it** — they verify that a symbol
exists, never that a sentence is true.

### F-38-1 — the reader was named by a retired identifier, and the current one was labelled historic

The threading block read *"`SimulationInputResolution::prepareSimulationStep` (T7; named
`collectInputAll` before item 90's rename)"*, twice — once for the reader and once inside the
`LocalInputCache` comparison.

**It is inverted.** `prepareSimulationStep` was item 90's name for `collectInputAll`, and **item 94
reverted it**. The member is `collectInputAll` (`SimulationInputResolution.h`). Four documents in
this tier — `History-ResimGate.md`, `SimulationInputResolution-rationale.md`,
`SimulationReconciliation-rationale.md`, `ThreadingCrossings.md` — already declare the identifier
RETIRED with a lint escape saying it *must not resolve*. `ThreadingCrossings.md` row 1, which cites
this header's threading block as authoritative, had the correct name while the block it cites did
not. **A citation whose target was wrong about the thing being cited.**

### F-38-2 — a grep handle that has never existed

The wire-version block named *"`relayedInputRing::kWireFormatVersion` and
`getWireFormatVersionOnWire()`"*. `getWireFormatVersionOnWire` has **one occurrence in the whole
repository: that comment.** The function is `relayedInputRing::getWireFormatVersion`, and the code
four lines below the block calls it by that name. A reader following the prose would have found
nothing.

### F-38-7 — a file name used where a class was meant

*"a fixed-size slot array (as `CorrectionCache` and `RemoteMoveQueue` both are)"*. `CorrectionCache`
is the FILE; the class is `StateCorrectionCache` (`CorrectionCache.h`). This is the same shape as a
correction recorded in `TimeConfig-rationale.md` §11, and it is worth naming twice because a
file-name-for-class-name substitution reads perfectly and resolves to nothing.

### A near-miss on my own side, recorded because it is the more instructive one

The first pass of this audit reported `ASimulationInputRelay::OnRep_RelayedInputRing` as
**nonexistent**, because the scan root covered `Plugins/OGSimulation` and `Source/OGBrawlerUnreal`
and the symbol lives in `Source/OGSimulationUnreal`. It exists and the header is right. ⇒ **A sweep
whose root does not cover the claim returns a confident zero.** That is this project's signature
defect appearing inside the auditor rather than in the prose, and it is the reason every negative
result in §10 is quoted with its search root.

**Verified TRUE and left alone** (recorded so a reviewer need not re-derive them): the wipe
asymmetry and its non-wipe comment at the `wipeAllForResync` site; `getZeroPlayerInput`'s `(0,0,1)`
aim vectors; `NetSyncTelemetry::shouldLogVersionMismatchOnce` and its **exactly four** assertions in
`NetSyncTelemetryTest.cpp`; `RelayArrivalProbe::noteArrival` and its use of
`newCaptureTicksIngested`; `maxConsecutiveFallbackRun`; `kMaxDepth` = 8; probe 2's identity; and the
~120-per-mille / ~11-per-mille pair, which `RelayProbes-rationale.md` §8 independently records as
122 / 129 per mille against ~11.

**Routed, not fixed here.**

- **R-38-a** — `Network/LocalInputCache.h` attributes `wipeAllForResync` to `SimulationNetSync`; it
  moved to `SimulationInputResolution` at item 87. Not this task's file.
- **R-38-b** — `Network/LocalInputCache.h` is the twin carrier of the wipe-asymmetry fence and has
  not been compressed or R0-audited.

---

## 11. Provenance — the workspace citations this header used to carry

The pre-compression header carried 44 comment lines bearing initiative-workspace tags (`T5`, `T6`,
`T7`, `T9`, `T19`, `T20`, `T34`, `T39`, `task 79`, `item 87`, `item 90`, `item 91 part J1`, `AM-4`,
`RN-14`, `RelayDelaySpectrumDesign.md` §§, and *the Backlog's "Out of scope"*). None of them resolve
for a reader who has no initiative workspace, which is what the standalone-truth rule forbids. They
are recorded here once, as provenance, and are not repeated in the header:

| tag | what it introduced |
|---|---|
| T5 | the store itself, the three-name vocabulary lockdown, the sentinel reservation |
| T6 | the resim's `find(ref)` read |
| T7 | the proxy prediction ladder and its verify step |
| T9 | the stale-hold probe (`maxConsecutiveFallbackRun`) that would set K |
| T19 | `newestCaptureTick` and the replication-cadence probe |
| T20 | `residentSpan` and the three miss classes |
| T34 | `newCaptureTicksIngested`, the loss-counter fix |
| T39 | the relayed-ring property and its OnRep moving to `ASimulationInputRelay` |
| task 79 | the one-shot mismatch gate moving to `NetSyncTelemetry` |
| item 87 / item 91 part J1 | `wipeAllForResync`, `setNeutralInput` and both collect paths re-pointing off `SimulationNetSync` onto `SimulationInputResolution` |
| item 90 / item 94 | the `collectInputAll` → `prepareSimulationStep` rename, and its revert (§10, F-38-1) |
| AM-4 | the length-prefix version-invariance invariant (§9) |
| RN-14 | the 2026-08-16 rename from `RelayedInputStore` |
| "the Backlog's Out of scope" | the deferred stale-hold rule (§5) and the deferred SPSC seam (§6), both now recorded in this file |
