<!-- SPDX-License-Identifier: MPL-2.0 -->
<!-- lint-external-ref: FRelayedInputRing::NetSerialize -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: FRelayedInputRing::writeLatest -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: FRelayedInputRing::kMaxInputBytes -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: noteStageOverflowDrop -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: ASimulationInputRelay::setRelayResendEntries -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
<!-- lint-external-ref: FRelayedInputRing::findEntry -- deleted by og-input-latency task P5-9 (the relay actors, their listeners, the Iris sim buffers, serializers and proofs, and UEConnectionHandle); kept in the history -->
# `RelayedInputRingCodec.h` — rationale

The narrative behind `relayedInputRing`, the byte codec for the **outbound** (server → peers) relay
payload: the replace-latest ring that carries one character's recent inputs to the other clients, its
flush stage, and the flush's re-send of the last flushed entries. The header keeps its licence, its
docs pointer, its code and its tags; everything that explains it lives here.

**If this file and `RelayedInputRingCodec.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`RelayedInputRingCodec-guards.md`, reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction (the STL, `OGAssert.h` and the serialization headers). Where a host appears below it is
"the host": whatever program owns the replicated ring, its stage and the transport that ships it.

**Provenance.** og-netcode-v2-input-relay T1 created the codec; T29, T34, T35, T43 and item 63 (RN-13)
shaped it; og-input-latency task 8 (2026-11-11) added the re-send and converted the header from
comments to this file. §17 lists what the conversion found false and corrected.

---

## §1 The payload

The wire payload is a two-byte header followed by fixed-stride entries:

    [version (u8)] [entryCount (u8)] { [captureTick (u32)] [dA (u8)] [input (fixed per InputType)] } × entryCount

- `captureTick` — the tick the **sending** client captured the input at. It is the join key the whole
  relay is built on: consumers key every entry by it.
- `dA` — the schedule stamp: the effective input delay the **server** had for that wire at receipt.
- `input` — the input value itself.

**The application tick is derived, never stored:** `applicationTick(captureTick, dA)` is
`captureTick + dA`. Storing it would create a second source of truth that could disagree with its own
two operands after a re-stamp. Peers use it to place a relayed input on their own timeline; the
client's scheduled read is the inverse of the same relation.

The per-entry input serialization reuses the field-wise machinery of the sync buffers
(`SimulationSerialization.h`, `SimulationComposite.h`), so the serialized size of one input is a
compile-time constant for a given `InputType` (`detail::entryInputSize`) and entries are a fixed
stride (`detail::entryStride`). The ring can therefore be scanned, and any entry rewritten **in place**,
without a per-entry length prefix. `detail::CompositeSerializedSize`'s primary template is
deliberately undefined: only a field-serializable type or a `SimulationComposite` specialization is a
valid relay input. A `SimulationComposite` has no `SerializableFields` specialization, so the
`Serializable` branch cleanly tells the two apart.

**The payload carries its own version byte,** `kWireFormatVersion`, bumped when the on-wire layout
changes so a mismatched peer can be detected (the receiver drops a mismatched ring whole:
`populateRemoteInputCache` reports `VersionMismatch`). It is independent of the sync buffers' fences,
such as the correction state's (`CorrectionStateBufferCodec.h`): a change to one never bumps the other.
Version 3 is the layout of version 2 in the right-handed Y-up frame (og-sim-metres-yup flip X: a relayed
input composite's world vectors, such as a brawler's move and aim directions, are in that frame); flip U
left it at 2.

## §2 Why this is not the inbound redundancy bundle (fable finding B2, the reason this codec exists)

The **inbound** (client → server) redundancy bundle (`InputRedundancyBundleCodec.h`) is append-only and
immutable per capture tick: `inputRedundancyBundle::appendSlot` checks against a duplicate capture tick,
and the payload hard-caps at `inputRedundancyBundle::kMaxSlots` (8). That is right for an RPC argument
that is rebuilt from scratch every send.

The outbound relay payload is the opposite shape: a **persistent** replicated property, written again
on every newer capture tick for the whole session. Driven the bundle's way it would overflow its slot
budget within 8 ticks and then assert. So the two directions use two payloads, and this one:

- **replaces, never appends** — a newer capture tick evicts the oldest resident entry once the ring is
  at depth;
- allows a resident capture tick to be **rewritten in place** (guard G-01).

The server's relay tap is the seam between the two directions. It sits in
`ServerReceptionCoordinator::receiveRemoteInput`'s arm for a capture that is parked, first-seen
(`acceptedNew`, a per-id watermark that is false for a redundancy re-send and for an out-of-order older
tick) and actually queued, so the server stages each newer capture tick for a character exactly once,
in increasing order. That is precisely replace-latest semantics, and it is what §15's re-send relies on.

## §3 Why it lives in core, and the adapter's names

The ring's write, evict and scan logic is pure byte manipulation over the og-simulation serialization
free functions. Its production buffer is og-simulation's own `RelayRingBuffer` (`Network/SimPayloadBuffers.h`),
a fixed-capacity byte array that delegates every operation here; og-sim-net's server publishes it per
character (`RelayRingStager`) and its client stores it on the character's channel
(`SimNetChannel::storeRing`). Until og-input-latency task P5-9 deleted it, one adapter (Unreal) wrapped it in
`FRelayedInputRing`, a USTRUCT that owned a replicated byte array. Hoisting the algorithm into core is what
lets the engine-free Low-Level Tests exercise the real production path (`WireFormat/RelayedInputRingTest.cpp`):
that target links engine-free modules only. The type-erased byte payload is also what lets one buffer
carry an arbitrary `InputType`.

The replication system, its once-per-frame poll of a replicated property, the property comparison
that decides whether that property is dirty, and the game-thread tick whose early and late broadcasts
run receipt and then the flush are named here by role only; nothing in the header depends on which
engine supplies them. In the Unreal adapter they were Iris, its poll of a replicated UPROPERTY, the
property system's identity comparison, and the world tick's dispatch and flush broadcasts, and the wire
serializer was `FRelayedInputRing::NetSerialize`, until og-input-latency task P5-7. Since then og-sim-net
sends the ring as a sim-envelope message on og-net's SimRelay lane, at most once per frame from the og-net
driver's tick flush, and only when the flush published something. Another adapter substitutes its own.

## §4 The buffer concept

Every template operates on a `Buffer` exposing:

    std::int32_t bundleByteNum() const;                    // current byte count
    void         bundleAddZeroedBytes(std::int32_t n);     // grow by n zeroed bytes
    template <typename T> void writeToBuffer(std::uint32_t off, const T& v);
    template <typename T> T    readFromBuffer(std::uint32_t off) const;

These are deliberately the same four method names as `InputRedundancyBundleCodec.h`'s buffer concept,
so one buffer type (the adapter's array-backed USTRUCT, or a `std::vector`-backed test buffer) can back
either codec without a second adapter surface.

**The flush path needs a fifth method,** scoped so the shared four are untouched:

    void bundleTruncateTo(std::int32_t byteCount);         // shrink, never grow

It is required only by `resetEntries` and `flushStagedInto`. `writeLatest`, `findEntry`, `forEachEntry`,
`entryCount` and `getWireFormatVersion` still need exactly the four, so a buffer written against the
bundle's concept still backs every read and write here, and that header is not touched (T43 finding 6:
no shared-contract change). Only a buffer that is flushed has to grow the fifth method.

**Why a shrink is unavoidable on the flush path:** the transport ships `bundleByteNum()` bytes — the
used count the adapter's wire serializer emits — so zeroing the entry count without shrinking would
leave the payload at its high-water mark and send stale trailing entries forever (guards G-03, G-06).

## §5 Order, and depth

**Order is not meaningful.** Entries sit at ring positions, not in arrival order: after the first
eviction the oldest slot holds the newest entry. Consumers must key by `captureTick` and must never
treat entry 0 as "the first" or the last entry as "the latest". A flush (§14) lays its entries out
re-sent tail first, oldest first, then the stage in its own ring order; that layout is a property of
the flush, not a promise to consumers.

**Depth is never stored on the wire.** `writeLatest`'s `depth` is passed per write; the receiver never
needs it, it just iterates what arrived. Growing the ring mid-session works (the next write appends).
Shrinking it mid-session does not reclaim already-allocated entries: a lowered depth degenerates to
replace-oldest at the larger size.

`writeLatest`'s caller used to be a session-configurable depth field, sized once at construction (the
shrink case above was a documented non-scenario). That field is retired (item 63 / RN-13, 2026-08-16;
its identifier is on record there). `writeLatest`'s only production caller is now `stageArrival`, which
passes `kMaxDepth` as a constant, so the shrink non-scenario is moot on the live relay path. It survives
only as a property of `writeLatest` itself, which direct test callers still exercise with arbitrary
depths. `RelayRingBuffer::writeLatest` forwards to it with whatever depth its caller passes; no production
code calls that member (nor did any call the Unreal `FRelayedInputRing::writeLatest` it replaced, deleted by
og-input-latency task P5-9).

The flush path's capacity is the constant, not a parameter (guard G-02).
`Network/RelayRedundancyDepthTest.cpp` pins the absence of a depth parameter on `stageArrival` at
compile time. Reintroducing one and reading a configurable value through it would cap every round at
that value and silently reproduce replace-latest (T43 finding 1).

## §6 The depth bound and its guard

`kMaxDepth` (8) is the hard upper bound on resident entries, for wire safety, and since item 34 the only
bound: `writeLatest`'s runtime depth is `clampDepth(requested)` for whatever a caller passes, and
`stageArrival` always passes `kMaxDepth` itself.

8 mirrors `inputRedundancyBundle::kMaxSlots` deliberately: it is the same class of bound (a
per-character input payload budget), so the two directions stay comparable when bandwidth is measured.
Raising it is a deliberate wire-budget decision, not a tuning tweak: it raises `kMaxWireBytes` (§8), and
with it the largest payload a receiver must accept.

`clampDepth` clamps a runtime depth into [1, `kMaxDepth`]. A requested 0 (or a negative) would make the
ring unwritable and silently disable the relay — far worse than clamping up — so 0 and negatives clamp
**up** to 1; 0 is not "off". Above the bound it caps rather than wraps. Both directions are
`static_assert`s in the header, which replaced the comments that stated them.

`clampDepth` is public deliberately (T35): `writeLatest` clamps with it on every write, so every direct
caller of `writeLatest` — production's `stageArrival`, which always passes `kMaxDepth`, and every test
that drives `writeLatest` with an arbitrary depth — goes through one guard. A future intake path has
exactly one place to clamp against and cannot drift from what the write site itself would do. The
session intake that used to be its other caller is retired (§5). `Network/RelayRedundancyDepthTest.cpp`
sweeps it at run time as well.

## §7 The re-send bound

`kMaxResendEntries` (2) is the most entries a flush re-sends from the ring it last published (§15), and
`clampResendEntries` clamps a requested count into [0, `kMaxResendEntries`]. Unlike `clampDepth`, 0 is
a real value: it is the flush as it was before og-input-latency task 8, and the default.

It must stay below `kMaxDepth`, and the header `static_assert`s that: a flush always publishes at least
one new entry, so the tail never needs the whole ring. The session knob that feeds it,
`TimeConfig::relayResendEntries`, has its own clamp to the same range in `TimeConfig.h`
(`latencyKnobs::clampRelayResendEntries`, `latencyKnobs::kMaxRelayResendEntries`). The two maxima are
held equal by a `static_assert` in `UESimulationSession.h` (until og-input-latency task P5-7 it sat where the
Unreal adapter handed the knob to the relay host, `UESimulatableOwnerBinding.h`) and by
`Network/RelayResendTest.cpp`. The codec does not include
`TimeConfig.h`: it stays a byte codec with no session dependency.

## §8 The malformed-length bound

`kMaxInputBytes`, `kMaxWireBytes` and `isAcceptableWireLength` were `FRelayedInputRing::kMaxInputBytes`
and `::kMaxWireBytes` and an inline comparison inside that USTRUCT's wire serializer until T29. They
moved here for the same reason every other rule in this file lives here: the Low-Level Test target
cannot see a USTRUCT, so a guard expressed inside one is untestable. The USTRUCT kept both names as
aliases and `static_assert`ed the value it produced (1066), so the wire format did not change: it was a
move, not a redesign. `RelayRingBuffer` keeps the aliases and the 1066 pin now; og-input-latency task P5-9
deleted the USTRUCT.

`kMaxInputBytes` (128) is the upper bound on the serialized input of one entry. It used to be anchored,
by comment only, to one adapter's 128-byte input sync buffer; that buffer was retired in
og-syncedInput-rework task 9, and nothing ever checked the two against each other. The bound now rests
on the `static_assert` in `detail::entryStride`: an `InputType` whose entry input exceeds it does not
compile, so a well-formed full-depth ring can never exceed `kMaxWireBytes`. The value 128 was kept, not
re-derived.

`kMaxWireBytes` is the largest payload a well-formed ring can produce: the header plus `kMaxDepth`
entries of the largest stride, `kMaxEntryBytes` (captureTick + dA + `kMaxInputBytes`). It is used only to
reject a malformed inbound length. A ring reaches it only at full depth with an input of exactly
`kMaxInputBytes`; a flush with a re-send tail never exceeds `kMaxDepth` entries (guard G-05), so the
bound holds for it unchanged.

`isAcceptableWireLength` is the rule; whether it **runs** is a property of the transport that calls it.
From the day the current replication system went live until T29 it did not run at all, because that
system never called the struct wire serializer the guard lived in. That is why T29 also registers a
serializer with the replication system. A length above the bound means a corrupt or hostile packet: the
transport must refuse it rather than allocate and read that many bytes.

## §9 Layout helpers and the lazy header

`kVersionOffset`, `kEntryCountOffset` and `kHeaderBytes` are the header layout. `detail::entryOffset` is
`kHeaderBytes + index × stride`.

`detail::initHeaderIfEmpty` writes the header (`[version][entryCount = 0]`) lazily, on first use, so a
default-constructed, never-written ring costs zero bytes on the wire rather than an empty header. That
laziness is why `resetEntries` truncates to the header, never to 0 (guard G-03).

`kMaxEntryBytes` is the largest entry stride any `InputType` can have. The re-send tail (§15) is a
fixed array sized by it, and a `static_assert` ties it to `kMaxWireBytes`'s per-entry term so the two
cannot drift.

The type-erased helpers `detail::captureTickAtStride` and `detail::holdsCaptureTickAtStride` read a
capture tick by an explicit stride, for the flush, which names no `InputType` (§14).

## §10 Reads

- `getWireFormatVersion` returns 0 on an empty ring (no header written yet), so a never-written ring
  never reads as a version mismatch.
- `entryCount` is 0 on an empty ring.
- `captureTickAt` — the caller guarantees `index < entryCount`.
- `containsCaptureTick` — whether a capture tick is resident.
- `findEntry` is the capture-tick lookup. On a hit it writes the entry's stamp into `outDA` and its value
  into `outInput` and returns true; on a miss it returns false and leaves both untouched. Tests use it to
  read a ring back by capture tick.
- `forEachEntry` invokes `callback(captureTick, dA, input)` once per resident entry, in ring-position
  order (§5: key by capture tick, never by position). It is a no-op on an empty ring. It is what the
  client-side store is populated from (`populateRemoteInputCache`, `Network/RemoteInputCache.h`).

## §11 `writeLatest` — the replace-latest write

It stores `(captureTick, dA, input)` and retains at most `clampDepth(depth)` of the most recent capture
ticks:

1. `captureTick` already resident → that entry is **rewritten in place** (guard G-01). A re-stamp of an
   already-relayed tick, such as the same input relayed again after a tier change moved `dA`, must
   update, not assert.
2. Ring below depth → the entry is **appended** (the ring is still filling).
3. Ring at depth → the **oldest** resident entry (lowest capture tick) is superseded, overwritten in
   place. Nothing shifts; entry order is ring position, not age.

It returns true when the entry is now resident. It returns false, leaving the buffer byte-for-byte
untouched, in exactly one case: the ring is at depth and `captureTick` is older than every resident
entry, so accepting it would evict fresher data to store staler data. Unlike the bundle's
`inputRedundancyBundle::tryAppendSlot`, false here is not a producer bug: the server's relay tap only
ever stages strictly newer ticks (§2), so this arm is a defensive guard for an out-of-order caller, not
an error channel. Callers may ignore the result.

## §12 Flush-on-poll

**The problem it solves (T34).** `writeLatest` used to be called on the replicated ring from the RPC
receipt path, which is paced by packet arrival, while the replication system polls the ring once per
server game-thread frame. At the then-shipped depth of 1, a second arrival in the same frame overwrote
the first in server memory before replication ever compared the property. That was measured at ~89.3 %
of relayed inputs surviving, and from a client that loss is indistinguishable from a wire drop.
`Network/RelayWritePathProbe.h` is the server-side probe that found it.

**The shape.** Arrivals are staged into a **separate** ring buffer of the same wire format
(`stageArrival`); once per poll the stage is published into the replicated ring and emptied
(`flushStagedInto`). The ring at poll N therefore carries exactly `arrivals(N)`, not "the newest arrival
of N" — plus, when the host sets a re-send count, the tail of §15.

**R = 0, the default.** At `resendEntries = 0` a published round is not kept for a second round: a
packet that carries it and is lost takes those inputs permanently. ~1.1 % measured wire loss was
accepted by the user on 2026-08-08 under the self-healing correction model. The replication system
exposes no send-success signal game code can read, so that loss is invisible on the server by
construction. The binding instrument is the client side: `RelayArrivalWindowSummary::lostCaptureTicksX1000`
(`Network/RelayReadProbe.h`), which counts the unfilled part of the arithmetic gaps in the per-character
monotonic capture-tick stream — `Sum(gap − delivered)`, not `Sum(gap − 1)`. The `−1` form was written for
the replace-latest path, where one arrival carried one entry; against a flush it measures the burst rate,
and it reported ~120 per mille on a run that lost ~1 %.

The 2-generation hybrid of the flush-on-poll design (T30, §11.5) was its recorded escalation for this
loss. og-input-latency task 8's re-send (§15) is that escalation's shape: a flush carries the last `r`
entries the previous flush published.

**Threading: none.** Receipt (staging) and the flush are both on the game thread, in the same
game-thread tick, receipt first (the tick's early dispatch broadcast) and the flush last (its
end-of-tick flush broadcast). Plain members, no lock, no atomics (`ThreadingCrossings.md` lists the pair
as not a crossing).

## §13 `stageArrival` — the capacity rule

The only way to write the stage. Its capacity is `kMaxDepth`, passed as a constant and deliberately not
a parameter (guard G-02, §5).

`StageArrivalOutcome::accepted` says the entry is now resident. `StageArrivalOutcome::droppedOldest`
says the stage was already at capacity, so its oldest staged entry was superseded to make room. A burst
longer than `kMaxDepth` in one frame is the only way to reach that, and the ring could not have carried
those entries either (`kMaxDepth` is its own hard bound), so it is a ceiling, not a regression against
the pre-flush behaviour. It is the only loss the server side can see, so it must never be silent: the
host counts it (in the Unreal adapter, the relay host's `noteStageOverflowDrop`).

`droppedOldest` is predicted **before** the write, because `writeLatest`'s arm 3 supersedes in place and
leaves nothing behind to detect afterwards. A capture tick that is already resident is a re-stamp
(arm 1) and evicts nothing; a refused write (arm 3's stale guard) evicted nothing either, because the
buffer is byte-for-byte untouched in that arm.

## §14 `resetEntries` and `flushStagedInto`

`resetEntries` empties a ring without destroying its identity: entry count 0, buffer truncated to
`kHeaderBytes`, version byte preserved (guard G-03). It is a no-op on a never-written buffer, which has
no header to preserve and no bytes to reclaim.

`flushStagedInto(dst, stage, resendEntries)` publishes everything staged since the last poll into `dst`,
prepends the re-send tail (§15), empties the stage, and returns the number of **staged** entries it
published. The re-sent entries are not counted in the return value; the host reads them as
`entryCount(dst)` minus that value.

- **Suppress-clear-on-empty** is the skip-recovery mechanism, not an optimization (T43 finding 3
  promoted it to a requirement; guard G-04). With an empty stage the flush touches nothing. Join-settling
  is when empty frames are common (`emptyFrames` measured with a median of 14, up to 98, per probe
  window), so the retry it allows frequently wins.
- **Type-erased by construction.** The copy is byte-wise and no `InputType` is named: the stage and the
  ring share one wire format, and the stride is derived from the stage's own byte count and entry count
  (an `OG_CHECK` holds that it divides evenly). That is what lets the flush run on the ring's host, which
  in the Unreal adapter is an actor that must not know the game's input type.
- **Re-shaping `dst`.** `detail::initHeaderIfEmpty` covers a never-written destination; the truncate then
  grow is what makes the ring **shrink** when a quiet round follows a burst (guard G-06).
- **Wire format unchanged, no version bump.** Only the number of resident entries moves, and
  `entryCount` is already on the wire and already varies from 0 to depth as a ring fills. The re-send
  adds no field: a re-sent entry is an ordinary entry. The version byte is written from
  `kWireFormatVersion`, never a literal (guard G-07).

## §15 The re-send of the last flushed entries (og-input-latency task 8)

**What it buys.** At `resendEntries = r > 0`, a flush with new entries also carries the last `r` entries
the previous flushes published. A packet loss that takes one round no longer takes its entries: the
next round's packet carries them again. A hole in a peer's relay stream heals one round later instead
of at the next correction, and a peer sees a hole only when two rounds in a row are lost (loss² instead
of loss), for the entries the tail covers.

**Where the tail comes from: `dst` itself** (guard G-05). `detail::takeResendTail` picks the newest `r`
entries of the ring as the previous flush published it, by capture tick, and copies their bytes before
`dst` is re-shaped. No second buffer exists. The proof that those are "the last `r` flushed":

- Each flush leaves `dst` holding `tail(N−1) ∪ new(N)`, where `new(N)` is the round's staged entries.
- Capture ticks are staged in strictly increasing order for one character (§2), so every entry of
  `new(N)` is newer than every entry already flushed.
- So the newest `r` entries of `dst` after flush N are the newest `r` of everything flushed so far,
  whenever `dst` holds at least `r` entries — which it does after the first rounds, because the tail
  re-carries what a one-entry round cannot supply. By induction this holds for every flush.

**What the tail never does.**
- **Never on an empty frame.** The early return comes first (guard G-04): a frame with no arrivals
  re-sends nothing and leaves `dst` byte-identical, so the skip recovery of §14 is unchanged at any `r`.
- **Never above `kMaxDepth`.** The tail takes at most `kMaxDepth − stagedCount` entries: new entries
  always win the room, and a full stage publishes no tail. An `OG_CHECK` holds the total. The stage
  capacity stays `kMaxDepth`, and `stageArrival` gained no parameter (guard G-02).
- **Never a duplicate.** A tail entry whose capture tick is also staged is skipped: the staged entry is
  the newer data, and one ring never holds a capture tick twice. Production never stages an already
  flushed tick (§2); the rule exists so the codec's own invariant holds for any caller.
- **Never a version mismatch.** A `dst` that is never-written or carries another version byte yields
  no tail.

**What it costs.** One re-sent entry is one entry's stride — about 44 B for the brawler's input — per
receiver, per flush that has new entries. The default is 0, and the flush at 0 performs exactly the
operations it performed before task 8: `Network/RelayResendTest.cpp` pins its output bytes against a
trace captured from the code before the change.

**What one re-sent entry covers.** At `r = 1` a lost round is recovered whole only when it carried one
new entry. That was the common case in T34's PIE run (2026-08), where the relay host measured about
1.13 entries per flushed round at a 60 Hz sender. og-input-latency task 8's three headless smokes
(2026-11-11) measured 1.41–1.69 on the last `[RelayFlush]` line per character, so more of today's lost
rounds carried two. A lost round that carried two (a sender at 30 frames per second sends two captures per
packet, and the server stages both in one frame) gets back only its newer entry at `r = 1`, and both at
`r = 2`. `Network/RelayResendTest.cpp` pins both shapes.

**What the receiver does with a re-sent entry.** `populateRemoteInputCache` pushes every entry, and the
store's push is last-wins on a resident tick, so re-delivering a tick the store already holds is
idempotent. It is not counted as new coverage (`RelayedInputIngestReport::newCaptureTicksIngested` counts
only ticks not already resident). A recovered tick that the store did not hold is new coverage, so
`lostCaptureTicksX1000` falls by the ticks the tail recovered and is not flattered by the ones it
re-delivered.

**Who sets it.** The host, from the session's `TimeConfig::relayResendEntries`, on the authority only.
og-sim-net's server reads it from that config and clamps it at every flush (`SimNetServer::flushRelays`).
Until og-input-latency task P5-7 the Unreal binding handed it to the relay host when it spawned it
(`ASimulationInputRelay::setRelayResendEntries`, deleted by P5-9), and the host passed it to every flush.

## §16 What the header does not do

- It does not log. Counters and lines are the host's.
- It does not know the session: no `TimeConfig`, no clock. Depth, capacity and the re-send count reach it
  as values.
- It has no thread-safety of its own (§12).

## §17 R0 — what the conversion found false, and corrected

The header's comments were verified against the tree before they moved (og-input-latency task 8,
2026-11-11). Six were false and are corrected above:

1. **"This is a brand-new property whose first-ever format is version 1, so nothing is being bumped"**
   (the old wire-version note). `kWireFormatVersion` is 3: version 3 is the layout of 2 in the
   right-handed Y-up frame (og-sim-metres-yup flip X), and flip U left it at 2. The correction-state fence
   it was contrasted with (`correctionStateBuffer::kWireFormatVersion`, `CorrectionStateBufferCodec.h`)
   is far past the "1 → 2" it named. The two fences are independent; that part was and is true.
2. **"Depth 8 already costs 8x the per-character input bytes of today's depth-1 default"** (at
   `kMaxDepth`). There is no depth-1 default: the depth field is retired (§5), and the stage capacity
   is `kMaxDepth`. Corrected in §6.
3. **"A sender never approaches it at the shipped depth of 1"** (at `kMaxWireBytes`). Same retired
   premise. Corrected in §8.
4. **"The LLT target links only Core + OGSimulation"**. The Low-Level Test target also links
   `OGSimulationJolt`. What the claim was for holds: none of those modules can see a USTRUCT. Corrected
   in §3.
5. **"Under R = 0 that retry is the only recovery that exists"** was true when written and is now true
   only at the default; §12 and §14 say so.
6. **"[`findEntry` is] the read the client-side store (T5) is populated from"**. The store is populated
   by `populateRemoteInputCache`, which iterates the ring with `forEachEntry`. Outside the tests,
   `findEntry`'s only caller was the adapter's forwarding member `FRelayedInputRing::findEntry`, which
   nothing called (deleted by og-input-latency task P5-9; `RelayRingBuffer::findEntry` forwards the same way
   now). Corrected in §10.

The old header also cited three initiative design documents (the input-relay design §4 and the
relay-delay-spectrum design §5 and §8.2). They live outside this repository, so this file does not cite
them; the task ids above are the provenance.

Outbound, one stale sentence about this payload was found in a file the conversion did not own:
`FRelayedInputRing`'s header said the property was registered with a plain replication declaration and
no condition, while the relay host registered it skip-owner. It was recorded in the task's notes for its
owner; og-input-latency task P5-9 deleted both files.
