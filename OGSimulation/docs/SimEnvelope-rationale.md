<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimEnvelope.h` — rationale

The narrative behind `Network/SimEnvelope.h`, the sim envelope: the two bytes in front of every sim message, the
writers that put them there and the decoder that checks them. The header keeps its licence, its docs pointer, its
code and its tags; everything that explains it lives here.

**If this file and `SimEnvelope.h` disagree, the header is authoritative and this file is stale.** Fix this file;
do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in `SimEnvelope-guards.md`,
reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free and socket-free by
construction (the STL, the three payload codecs, `SimPayloadBuffers.h` and `SimTransport.h`). Where a transport
appears below it is "the transport": whatever library moves the bytes between processes, one message at a time.

**Provenance.** Written with the header on 2026-11-14, for the sim traffic that moves from the engine's replication
onto a plain message transport. No code calls it yet.

---

## §1 What it is

Every sim message a transport carries is one **envelope**:

```
version:u8 (= kSimEnvelopeVersion = 1) │ kind:u8 │ body
```

The writers (`simEnvelope::writeRelayRing` and its siblings) build an envelope into a caller's buffer. The decoder
(`simEnvelope::decode`) takes the bytes of one received message, with the lane it arrived on and the direction it
travelled, and returns a `DecodedSimMessage`: the kind, the per-kind fields, a span over the body and a
`DecodeRefusal`. A refused message yields no body, and nothing in it is to be applied.

The body of each kind is the bytes a payload codec already writes (`InputRedundancyBundleCodec.h`,
`RelayedInputRingCodec.h`, `CorrectionStateBufferCodec.h`) or a few fixed fields. **The codecs' bytes are copied
untouched**: the envelope adds bytes in front of them and never rewrites them.

## §2 Why a version byte and a kind byte

- **The version byte** lets a peer of another build refuse a layout it does not know, before it reads anything
  else. A change to any layout below (the envelope's own fields, a body's framing, a kind's lane) bumps
  `kSimEnvelopeVersion` and comes with new golden vectors.
- **The kind byte** makes each message self-describing. The decoder checks it against the lane the message
  arrived on (§3), so a message delivered on the wrong lane, or sent the wrong way, is refused instead of being
  read as another payload. It also makes the decoder fuzzable in isolation: any byte string is a valid input.
- **The cost** is 2 B per message. The largest per-character message, a correction of the brawler game's state,
  is 337 B (§8), so the two bytes are about 0.6 % of it.

## §3 The kinds and their routes

| value | `SimMessageKind` | lane | direction | body | writer |
|---|---|---|---|---|---|
| 1 | `InputBundleSet` | `SimInput` | client to server | `count │ (simId │ bundle)×count` (§5) | `writeInputBundleSetHeader`, `appendInputBundle` |
| 2 | `RelayRing` | `SimRelay` | server to client | `simId │ ring` (§7) | `writeRelayRing` |
| 3 | `CorrectionState` | `SimState` | server to client | `simId │ corrVersion │ payload` (§8) | `writeCorrectionState` |
| 4 | `ClockPublish` | `SimClock` | server to client | `tick:u32` (§9) | `writeClockPublish` |
| 5 | `ClockPing` | `SimClock` | client to server | reserved | none |
| 6 | `ClockPong` | `SimClock` | server to client | reserved | none |
| 7 | `Roster` | `Control` | server to client | one roster message (§10) | `writeRoster` |
| 8 | `RelayFloor` | `Control` | server to client | `floor:u8` (§9) | `writeRelayFloor` |
| 9 | `ConnectionTier` | `Control` | server to client | `tier:u8` (§9) | `writeConnectionTier` |
| 10 | `ArrivalMarginSignal` | `Control` | server to client | reserved | none |

- **The values are the wire's kind byte.** A static assertion pins every value. Kind 0 is never sent, so a
  zero-filled message is refused. A new kind is appended as 11, never inserted.
- **`simEnvelope::isLegal(kind, lane, direction)`** is this table's lane and direction columns. A static
  assertion checks that every kind is legal on exactly one lane in exactly one direction, and kind 0 on none, so
  a kind added to the enum without a route, or with two, fails the build.
- **`SimDirection`** is the direction the message travelled: a server decodes what it receives as
  `ClientToServer`, a client as `ServerToClient`. Nothing is legal client to server on `Control` today.
- **Reserved kinds (5, 6 and 10)** have a route but no body layout yet; `simEnvelope::hasBodyLayout` is false for
  them. The decoder refuses them as `Kind` on their own route, and as `WrongLane` anywhere else: the route is
  checked first, so a kind off its route always reads `WrongLane`. A kind gains a writer and a decoder arm when
  its body is defined.
- **Who sends what is the caller's.** A `ConnectionTier` goes only to the connection it describes; the envelope
  does not know connections.

## §4 Byte order

The envelope's own multi-byte field, `ClockPublish`'s tick, is written and read as little-endian, byte by byte.
The codecs write their `u32` fields with a `memcpy` through the buffer, so in the platform's native order. The
two agree only on a little-endian target, and the header asserts that the target is one. The decoder also reads
the correction's tick from the codec's bytes (§8), which relies on the same agreement.

## §5 `InputBundleSet`: one message per connection per send

```
count:u8 (1 … 8) │ (simId:u8 │ bundle)×count
```

- **Why a set.** A client with several local players would otherwise send one message per player per send, and
  a transport that caps messages per second on the input lane would drop some of them. One set per connection
  per send keeps the message rate to one per send, whatever the number of local players.
- **`count`** is 1 to `simEnvelope::kMaxInputBundlesPerSet` (8). The bound equals `kMaxSimulatableSlots`, the
  number of characters a world holds, so one connection's set has room for every character it could own; a
  static assertion keeps the two equal. 0 is refused as `CountZero`, more than 8 as `CountOverMax`.
- **Each bundle is the input bundle codec's bytes, version 3,** `version │ slotCount │ slots`, with no length
  prefix. Its length follows from its own header: `2 + slotCount × (4 + inputBytes)`, the codec's fixed slot
  stride. `inputBytes` is not on the wire; it is the decoder's shape (§6).
- **A bundle whose version is not the codec's** refuses the whole set as `Version`: its layout is unknown, so
  its length is too, and the decoder cannot find the next bundle. A `slotCount` over the codec's `kMaxSlots` (8)
  is `CountOverMax`, a bundle shorter than its header says is `Truncated`, and bytes after the last bundle are
  `Overlong`.
- **The decoded body is the whole set body, `count` included,** and `DecodedSimMessage::count` repeats the count.
  `simEnvelope::forEachInputBundle(body, fn)` walks it and calls `fn` with each bundle's sim id and bytes, once
  per bundle, in order.
- **`forEachInputBundle` is all or nothing.** It walks the set once to check it and only then walks it again to
  call `fn`, so a set that is refused hands out no bundle at all (guard G-03). It returns the refusal, or `None`
  after visiting every bundle. Duplicate sim ids within a set are not refused here; whether a connection owns a
  sim id is its receiver's check.
- **Size.** A brawler bundle is at most `2 + 8 × 43 = 346` B, so a set of four is `3 + 4 × 347 = 1,391` B
  (`Shape::inputBundleSetMaxBytes(4)`). More than one full brawler bundle does not fit one `SimInbox` record
  (`kMaxSimMessageBytes`, 512 B); the envelope itself does not limit a set's size.

## §6 The shape: lengths that are not on the wire

The decoder checks exact lengths, so a cut message is `Truncated` wherever it is cut and a padded one is
`Overlong`. Three of those lengths depend on the game's types and appear nowhere in the bytes: the size of one
input (a bundle slot and a ring entry), and the size of the state composite (a correction). So `decode` and
`forEachInputBundle` are templates over a **shape**:

- `simEnvelope::Shape<InputBytes, StateBytes>` gives the derived strides: `kBundleSlotBytes` (`4 + InputBytes`),
  `kMaxBundleBytes`, `kRingEntryBytes` (`4 + 1 + InputBytes`) and `kCorrectionPayloadBytes`
  (`8 + StateBytes`), and `inputBundleSetMaxBytes(n)`.
- `simEnvelope::ShapeFor<InputT, SimulationComposite<Ts...>>` is the shape of a game's own input and state
  types, from the same size functions the codecs use. It asserts that the bundle codec and the ring codec agree on
  the input's size, so one stride cannot mis-frame the other's bytes.
- `Shape` asserts its bounds: an input of at most `relayedInputRing::kMaxInputBytes` (128 B), the most a ring
  entry carries, and a correction that fits `CorrectionStateBuffer` (384 B). A shape a buffer could not hold does
  not compile.
- `simEnvelope::EnvelopeShape` is the concept the templates require: a `Shape` and nothing else.

The writers are not templates: they copy the codecs' bytes and never need a stride.

## §7 `RelayRing`

```
simId:u8 │ ring
```

`ring` is the relayed input ring codec's bytes, version 3: `version │ entryCount │ entries`, each entry
`captureTick:u32 │ dA:u8 │ input`.

- **The ring's version is the codec's:** the length must be exactly `2 + entryCount × kRingEntryBytes`, and
  `entryCount` at most `relayedInputRing::kMaxDepth` (8, else `CountOverMax`). An empty ring (0 entries) is legal.
  `DecodedSimMessage::count` is the entry count.
- **Another version passes through** with only its length bounded (guard G-02). The ring's own reader,
  `populateRemoteInputCache` in `RemoteInputCache.h`, reports a version 0 as `NeverWritten` and any other
  mismatch as `VersionMismatch`, and reads nothing structural. A refusal here would replace that report with a
  count.
- **Either way the ring is at most `relayedInputRing::kMaxWireBytes`** (1,066 B), else `Overlong`, so a decoded
  ring always fits `RelayRingBuffer` (`assignFromWire` refuses nothing it is given from here).
- **At least the version byte** must be present: a ring of no bytes is `Truncated`.

## §8 `CorrectionState`

```
simId:u8 │ corrVersion:u8 │ payload
```

- **This is the engine-replicated correction buffer's framing minus its length field.** That buffer serializes
  `version:u8 │ used:u16 │ used bytes` (`SyncedSimulationStateBuffer.h`, `NetSerialize`). Here `corrVersion` is
  that version byte, moved behind the sim id, and the length comes from the message length: the payload is the
  rest of the message. `CorrectionStateBuffer::assignFromWire` sets the used length from it.
- **`payload`** is the correction codec's bytes: `tick:u32 │ appliedCaptureTick:u32 │ state composite`.
- **`corrVersion` equal to `correctionStateBuffer::kWireFormatVersion` (15):** the payload must be exactly
  `kCorrectionPayloadBytes` (`8 + StateBytes`), and `DecodedSimMessage::tick` carries the correction's tick, the
  value a newest-wins filter compares.
- **Another `corrVersion` passes through** with the payload uncounted and `tick` 0 (guard G-01). Its receiver
  keeps today's one-shot latch: the Unreal adapter's correction receiver logs an Error with both versions, shows
  the build-mismatch toast, and ignores every later correction (`SimulatableNetSyncComponent.cpp`,
  `OnRep_CorrectionState`). That needs the message, not a refusal.
- **Either way the payload is at most `CorrectionStateBuffer::kBufferBytes`** (384 B), else `Overlong`.
- **Size.** The brawler state composite is 325 B, so a brawler correction is `2 + 2 + 8 + 325 = 337` B.

## §9 `ClockPublish`, `RelayFloor`, `ConnectionTier`

Fixed bodies, exact lengths: `ClockPublish` is a 4 B tick (`DecodedSimMessage::tick`), `RelayFloor` and
`ConnectionTier` one byte each (`DecodedSimMessage::value`). A whole message is 6 B or 3 B. The envelope gives the
bytes no meaning beyond their width; what a floor or a tier means is the receiver's.

## §10 `Roster`

The body is one roster message, version 1, unchanged:

```
version:u8 (1) │ rosterKind:u8 │ revision:u16 │ rest
  Snapshot (1): count:u8 │ entry×count        entry = simId:u8 │ physicsSlot:u8 │ localIndex:u8
  Add      (2): entry
  Remove   (3): simId:u8 │ reason:u8
```

- **The envelope checks only the length this framing implies** (`simEnvelope::roster`'s constants): 4 + 1 +
  3 × count, 7 or 6 bytes. That is what makes a cut roster message `Truncated` here.
- **Everything else is the roster decoder's:** a sim id 0, a duplicate in a snapshot, a slot or local index out
  of range, a count over 8, a revision gap, an Add of a live id. The envelope passes those messages through.
- **A roster version other than 1, or an unknown roster kind, passes through** unchecked, so the roster decoder
  refuses it with its own reason.
- **The roster codec lives outside `og-simulation`.** `simEnvelope::roster` restates the framing it needs, and
  the roster codec is the place to assert that its constants equal these.

The roster format's own golden vectors travel byte for byte inside kind 7, and the envelope's verdict on each is
pinned by `SimEnvelope.RosterGoldenVectorsTravelByteForByteInKind7`: the snapshot, the add and the remove decode
with the roster message as the body; the roster decoder's refused vectors for a version, a sim id 0 and a slot 8
pass the envelope; the cut remove is `Truncated` here.

## §11 Refusals

| `DecodeRefusal` | when |
|---|---|
| `None` | decoded; the fields and the body are valid |
| `Version` | the envelope version is not `kSimEnvelopeVersion`; or a bundle in a set has another codec version (§5) |
| `Kind` | the kind byte is 0 or above 10; or a reserved kind on its own route (§3) |
| `WrongLane` | a kind on any lane or direction `isLegal` refuses |
| `Truncated` | the message ends before a field or a length it implies; an empty message |
| `Overlong` | bytes after the last field of an exact-length body; a pass-through body over its buffer's capacity |
| `SimIdZero` | a per-character sim id is 0, in a ring, a correction or a set entry |
| `CountZero` | a set with no bundles |
| `CountOverMax` | a set count over 8, a bundle slot count over 8, a ring entry count over 8 |

- **The order:** the message's length, the version, the kind byte, the route, the reserved kinds, then the body
  front to back. The first failure is the answer.
- **A refusal carries the kind once it is known** (from `WrongLane` on), so a receiver can count refusals per
  kind. Its other fields are 0 and its body is empty.
- **Every read is checked against the span first.** The decoder never reads a byte past the message, and on
  `None` the body lies inside the message. `SimEnvelope.EveryProperPrefixOfEveryGoldenVectorIsTruncated` holds
  each cut message in a buffer whose tail is filled with 0x00 in one run and 0xFF in the other: a decode that read
  past the cut would give two different answers.
- **What a receiver does with a refusal is the receiver's.** A refused `Control` message cannot be recovered by
  a later one (nothing re-sends it), so a receiver may treat it as fatal for the connection.

## §12 Writers

- **Each writer returns the message's size, or 0 when it wrote nothing.** 0 means either that the caller's buffer
  is too small or that the envelope's own fields are invalid: a sim id 0, an empty ring, an empty roster message,
  a bundle shorter than its 2 B header, or an `appendInputBundle` to something that is not a set header, or to a
  set that already holds 8 bundles. A writer that returns 0 leaves the buffer untouched. The writers are
  `[[nodiscard]]`.
- **A set is written in steps:** `writeInputBundleSetHeader(out)` writes `version │ kind │ 0` and returns 3;
  each `appendInputBundle(out, used, simId, bundle)` appends one entry, raises the count and returns the new
  size.
- **The writers do not check the bodies** they copy: a ring, a correction payload or a roster message is written
  as given. The decoder is the check, and the receiver runs it.

## §13 What the envelope does not do

- It does not know connections, owners, revisions or ticks' meaning: those are its callers'.
- It does not copy bodies on decode: the body is a span into the caller's message, valid as long as the message
  is.
- It does not limit a message to a transport's or an inbox's maximum size; each transport and inbox has its own.

## Provenance

Written with the header on 2026-11-14. Every check was seen to fail before it was trusted:
- **The truncation sweep.** With the length check before the sim id read removed, the sweep failed on the 2 B
  prefix of the `RelayRing` vector (0x00 tail: `SimIdZero`; 0xFF tail: `Overlong`; expected `Truncated`), and on
  the set and correction prefixes that end before a sim id.
- **Each static assertion** fired with its own message on a poisoned copy compiled by clang-cl, against a clean
  control: the kind values reordered, `kMaxInputBundlesPerSet` raised to 9, `ArrivalMarginSignal` given no route,
  `ClockPing` given two directions, `Shape<129, 4>`, `Shape<2, 377>`, and a ring stride without its `dA` byte
  through `ShapeFor`. The little-endian assertion cannot be made false on a little-endian build, and was not.
- **Each guard and check** made its own case fail: a refusal of another correction version (G-01) or ring
  version (G-02), a visit without the checking walk (G-03), the reserved-kind check moved before the route check,
  `appendInputBundle` without its count limit, `decode` without the set's `CountOverMax`, and exact lengths
  without `Overlong`.
