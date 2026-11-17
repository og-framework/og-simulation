<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimPayloadBuffers.h` — rationale

The narrative behind the three engine-free payload buffers in `Network/SimPayloadBuffers.h`:
`CorrectionStateBuffer`, `RelayRingBuffer` and `InputBundleBuffer`. The header keeps its licence, its docs
pointer, its code and its tags; everything that explains it lives here.

**If this file and `SimPayloadBuffers.h` disagree, the header is authoritative and this file is stale.** Fix
this file; do not soften the header to match it.

⛔ **Do not move a prohibition into this file.** Prohibitions with a site live in
`SimPayloadBuffers-guards.md`, reachable from the `⛔G-nn` tag at that site.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by construction (the
STL, `OGAssert.h`, the serialization headers and the three payload codecs). It names no transport and no
socket. Where a host appears below it is "the host": whatever program owns the connections that carry these
bytes.

**Provenance.** og-input-latency P2-3 (2026-11-14) created the header, for the sim traffic that moves from the
engine's replication onto a plain message transport. No code calls it yet.

---

## §1 What the three buffers are

Each payload codec (`CorrectionStateBufferCodec.h`, `RelayedInputRingCodec.h`, `InputRedundancyBundleCodec.h`)
is a set of free templates over a **buffer**: an object that exposes `writeToBuffer` and `readFromBuffer`, and,
for the two growable payloads, `bundleByteNum`, `bundleAddZeroedBytes` and (ring only) `bundleTruncateTo`. The
codecs own every byte of the layout. Until now the only production buffers were one adapter's (Unreal's)
replicated structs: `FSimulationStateSyncBuffer`, `FRelayedInputRing` and `FInputRedundancyBundle`, each a
`TArray<uint8>` plus a property serializer.

This header supplies the same buffer surface over a **fixed array**, with no engine type, so a transport that
moves bytes as messages can hold, write, read and ship these payloads directly. Every buffer adds two members
the replicated structs did not need:
- `payload()`: the used prefix, as `std::span<const std::byte>`, the bytes a sender hands its transport;
- `assignFromWire(span)`: copies received bytes in and sets the used length from the span. It returns false,
  and leaves the buffer untouched, when the span is longer than the buffer's capacity.

The codec bytes are not changed: a buffer written here holds byte for byte what the codec writes into its own
test buffer for the same calls (`SimPayloadBuffersTest.cpp` pins all three).

## §2 `CorrectionStateBuffer` — mirrors `FSimulationStateSyncBuffer`

The UE struct is `SyncedSimulationStateBuffer.h:175-446`. Its codec-facing members, one by one:

| This header | Mirrors (`SyncedSimulationStateBuffer.h`) | Difference |
|---|---|---|
| `kBufferBytes = 384` | `kBufferBytes`, line 218 | none |
| `kWireFormatVersion` | `kWireFormatVersion`, line 244 | none: both alias `correctionStateBuffer::kWireFormatVersion` |
| `writeToBuffer` | `writeToBuffer`, lines 252-274 | `OG_CHECK` for `checkf`; `memcpy` for the byte loop; the same high-water mark |
| `readFromBuffer` | `readFromBuffer`, lines 276-291 | `OG_CHECK` for `checkf`; bounded by the capacity, as the UE struct's array is always `kBufferBytes` long |
| `write(value, tick, appliedCaptureTick)` | `write`, lines 305-310 | adds the capacity `static_assert` (§6) |
| `write(value, tick)` | `write`, lines 318-322 | none: the sentinel `kNoInputCaptureTick` |
| `readInto` | `readInto`, lines 327-332 | adds the capacity `static_assert` |
| `getAppliedCaptureTick` | `getAppliedCaptureTick`, lines 340-346 | none |
| `usedBytes()` | the private `usedBytes`, line 439 | made readable (§3) |

**What it does not mirror.** `NetSerialize` (lines 370-430) is the UE struct's transport framing, not a
codec-facing member: its version byte, its u16 used count, the T29 one-shot proof report and the forced-version
debug lever. `getReceivedWireFormatVersion` and its member (lines 353, 445) exist only to carry that version
byte to the arrival callback. On a message transport the correction's version byte and length belong to the
message framing that carries the payload, so none of them is here.

## §3 Receiving a correction: `assignFromWire` and the used length

`assignFromWire` copies the received bytes to offset 0 and sets `usedBytes` to their count (guard G-01). It is
the counterpart of `NetSerialize`'s load (`SyncedSimulationStateBuffer.h:398-406`), with two differences:
- **Above the capacity it refuses.** The UE load clamps a count above 384 to 384 and reads that many bytes,
  which leaves the archive behind the bytes the sender wrote. Here the whole message is refused and the buffer
  keeps its previous contents.
- **Nothing else changes.** As in the UE load, bytes past the received length keep what the buffer held before:
  neither side clears the tail. `readInto` reads the full composite whatever `usedBytes` says (the codec has no
  length check), so a payload shorter than its composite is read partly from old bytes, exactly as today.
  `usedBytes()` is public so that a length check can be added by the receiver without reaching into the buffer.

`write` resets the used length before the codec writes (guard G-02), as the UE `write` does at line 308.
`getAppliedCaptureTick` answers `kNoInputCaptureTick` while fewer than `correctionStateBuffer::kHeaderBytes`
bytes are used (guard G-03), as the UE member does at lines 342-343.

## §4 `RelayRingBuffer` — mirrors `FRelayedInputRing`

The UE struct is `RelayedInputRing.h:57-265`. The codec reaches it through `relayedInputRing::*` (stage, flush,
reset, read) and through `SimulationInputResolution::ingestRelayRing`, which hands it to
`populateRemoteInputCache` and `noteRelayedArrivals`; those read only `getWireFormatVersion`, `entryCount` and
`forEachEntry`, all codec free functions over the buffer surface.

| This header | Mirrors (`RelayedInputRing.h`) | Difference |
|---|---|---|
| `kWireFormatVersion`, `kMaxDepth` | lines 66-67 | none |
| `kMaxInputBytes`, `kMaxWireBytes` | lines 75-76 | `std::uint32_t` instead of `int32` |
| the `kMaxWireBytes == 1066u` assertion | line 84 | none |
| `bundleByteNum`, `bundleAddZeroedBytes` | lines 88-89 | grows inside a fixed `kMaxWireBytes` array (§5) |
| `writeToBuffer`, `readFromBuffer` | lines 92-101, 125-136 | bounded by the used length, as the UE ones are by `wireBytes.Num()` |
| `bundleTruncateTo` | lines 116-122 | none: shrink-only, a negative count is 0 |
| `writeLatest`, `stageArrival`, `findEntry`, `forEachEntry` | lines 154, 163, 171, 180 | none: each forwards to the codec |
| `num`, `getWireFormatVersionOnWire` | lines 186, 189 | none |

`assignFromWire` refuses a span longer than `kMaxWireBytes`, the bound `NetSerialize`'s load enforces with
`relayedInputRing::isAcceptableWireLength` (`RelayedInputRing.h:217`).

**What it does not check, and the UE struct does not either:** that the entry count in byte 1 agrees with the
length. The count is read from the wire, and `forEachEntry` walks that many entries at the input type's stride.
A count too large for the length reaches `readFromBuffer`'s `OG_CHECK`, as it reaches the UE struct's `checkf`.
A well-formedness check needs the input type, which `assignFromWire` does not know; it belongs where the ring is
consumed.

## §5 `InputBundleBuffer` — mirrors `FInputRedundancyBundle`

The UE struct is `SyncedSimulationStateBuffer.h:78-172`.

| This header | Mirrors (`SyncedSimulationStateBuffer.h`) | Difference |
|---|---|---|
| `kWireFormatVersion`, `kMaxSlots` | lines 89-90 | none |
| `bundleByteNum`, `bundleAddZeroedBytes` | lines 100-101 | grows inside a fixed array of `kMaxWireBytes` |
| `writeToBuffer`, `readFromBuffer` | lines 108-132 | none beyond `OG_CHECK` for `checkf` |
| `appendSlot`, `tryAppendSlot` | lines 139-155 | adds the slot-size `static_assert` (§6) |
| `forEachSlot` | lines 160-164 | none |
| `getWireFormatVersion` | lines 168-171 | none |
| `reset()` | `buildRedundancyBundle`'s `wireBytes.Reset()`, `InputRedundancyBundleBuilder.h:70` | the builder empties the bundle before each send; this is that call |

**The capacity**, `kMaxWireBytes = 2 + kMaxSlots × (4 + 128) = 1058` B: the bundle header, then `kMaxSlots`
slots of a capture tick and one input. The bundle codec has no input-size bound of its own (its UE struct's
array grows without limit), so this header borrows the relay ring's `relayedInputRing::kMaxInputBytes` (128 B).
Every bundled input is also relayed, so the ring's `entryStride` already refuses at compile time any input
above it.

## §6 The shared byte store, and what the compiler enforces

`RelayRingBuffer` and `InputBundleBuffer` share `simPayloadBuffers::detail::GrowableBytes<Capacity>`: a fixed
array, a used length, and the five buffer-surface members plus `payload`, `assignFromWire` and `reset`.
- **`bundleAddZeroedBytes` zero-fills** what it adds, as `TArray::AddZeroed` does. That is load-bearing for the
  ring: `bundleTruncateTo` shrinks the length but leaves the bytes, and the next flush's grow must not expose
  them.
- **Growth past the capacity is an `OG_CHECK`.** The UE arrays would grow. Through the codecs it cannot happen:
  `writeLatest` clamps the depth to `kMaxDepth`, `flushStagedInto` checks that a flush publishes at most
  `kMaxDepth` entries, the bundle codec refuses a slot past `kMaxSlots`, and the input-size assertions below keep
  every stride within the capacity's per-entry term.

The compiler enforces, so no comment or guard is needed for:

| Assertion | Replaced |
|---|---|
| `CorrectionStateBuffer::write`/`readInto`: header plus composite ≤ `kBufferBytes` | the UE buffer's sizing comment (`SyncedSimulationStateBuffer.h:180-217`) and its write `checkf`: a composite that outgrows 384 B now fails to compile at the first `write` |
| `kBufferBytes` fits a u16 | the u16 used count the UE struct put on the wire |
| `InputBundleBuffer::appendSlot`/`tryAppendSlot`: slot input ≤ `kMaxInputBytes` | nothing: new, it keeps a full bundle inside the fixed capacity |
| `RelayRingBuffer`: `kMaxWireBytes == 1066u`; `InputBundleBuffer`: `kMaxWireBytes == 1058u` | `FRelayedInputRing`'s drift pin (line 84); the bundle pin is new |

`SimPayloadBuffersTest.cpp` also asserts that `CorrectionStateBuffer` satisfies `CorrectionStateSyncedBufferConcept`
(`SimulationNetSync.h:102-115`) for a test composite, so it can stand where NetSync binds a correction buffer.

## §7 The brawler's sizes

For the brawler game's state composite (325 B) and input (39 B), measured by `SimPayloadBuffersBrawlerTest.cpp`
in og-brawler-tests:
- a correction uses 8 + 325 = 333 of its 384 B;
- a full relay ring is 2 + 8 × (4 + 1 + 39) = 354 B;
- a full redundancy bundle is 2 + 8 × (4 + 39) = 346 B.

The test also asserts that each, with its sim-envelope header, fits a 512 B inbox record. Those headers are 4 B
(version, kind, sim id and correction version), 3 B (version, kind, sim id) and 4 B for a one-bundle input set
(version, kind, count and sim id). The state and input footprints are game data and grow with game features:
these assertions are where such growth first fails to compile.
