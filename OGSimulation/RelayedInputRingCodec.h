#pragma once
// SPDX-License-Identifier: MPL-2.0

// ---------------------------------------------------------------------------
// RelayedInputRingCodec — the OUTBOUND (server -> peers) relay payload.
//
// Engine-agnostic byte-codec for the replace-latest, depth-configurable relay
// ring that carries one character's recent inputs to the OTHER clients
// (og-netcode-v2-input-relay T1; InputRelayDesign.md §4;
// RelayDelaySpectrumDesign.md §5 for the schedule stamp, §8.2 for depth sizing).
//
// Each entry is:
//
//   [captureTick (u32)] [dA (u8)] [input_serialized_bytes (fixed per InputType)]
//
//   captureTick — the tick the SENDING client captured the input at (the join
//                 key this whole initiative is built on).
//   dA          — the schedule stamp: the effective input delay the SERVER had
//                 for that wire at receipt (RelayDelaySpectrumDesign.md §5.2).
//   input       — the input value itself.
//
// THE APPLICATION TICK IS DERIVED, NEVER STORED: `captureTick + dA`. See
// applicationTick() below. Storing it would create a second source of truth that
// could disagree with its own two operands after a re-stamp.
//
// ---------------------------------------------------------------------------
// WHY THIS IS NOT FInputRedundancyBundle (fable finding B2 — the reason this
// codec exists at all).
//
// The INBOUND (client -> server) redundancy bundle is append-only and immutable
// per capture tick: appendSlot() OG_CHECK-fails on a duplicate capture tick and
// the payload hard-caps at kMaxSlots = 8. That is exactly right for an RPC
// argument that is rebuilt from scratch every send.
//
// The outbound relay payload is the OPPOSITE shape: a PERSISTENT replicated
// property, written again on every newer capture tick for the whole session.
// Driving the append-only bundle that way would overflow its slot budget within
// 8 ticks and then assert. So the two directions deliberately use two different
// payloads, and this one:
//
//   * REPLACES, never appends — a newer capture tick evicts the OLDEST resident
//     entry once the ring is at depth;
//   * allows a resident capture tick to be REWRITTEN in place (the bundle's R-T5
//     immutability invariant is explicitly NOT inherited — see writeLatest()).
//
// `receiveRemoteInput`'s per-id monotonic `acceptedNew` watermark is the seam
// between the two directions: the server relays each newer capture tick exactly
// once, which is precisely replace-latest semantics.
//
// ---------------------------------------------------------------------------
// WHY THIS LIVES IN CORE (same rationale as InputRedundancyBundleCodec.h).
//
// The ring's write/evict/scan logic is pure byte manipulation over the
// og-simulation serialization free functions — no Unreal dependency. The UE-side
// wrapper FRelayedInputRing (Source/OGSimulationUnreal/RelayedInputRing.h) is a
// USTRUCT that owns a UPROPERTY TArray<uint8> for replication and DELEGATES every
// operation here. Hoisting the algorithm into core is what lets the pure-C++
// Low-Level-Tests exercise the REAL production code path
// (WireFormat/RelayedInputRingTest.cpp) — the LLT target links only Core +
// OGSimulation and cannot see a USTRUCT at all. A USTRUCT also cannot be a
// template, so the type-erased byte payload here is additionally what lets ONE
// replicated property carry an arbitrary InputType.
//
// BUFFER CONCEPT: every template below operates on a `Buffer` exposing
//   - std::int32_t bundleByteNum() const;                  // current byte count
//   - void          bundleAddZeroedBytes(std::int32_t n);  // grow by n zeroed bytes
//   - template <typename T> void writeToBuffer(std::uint32_t off, const T& v);
//   - template <typename T> T    readFromBuffer(std::uint32_t off) const;
// These are DELIBERATELY the same four method names as
// InputRedundancyBundleCodec.h's BUFFER CONCEPT, so one buffer type (the UE
// TArray-backed USTRUCT, or a std::vector-backed test buffer) can back either
// codec without a second adapter surface. The per-entry input serialization
// reuses the same field-wise machinery as the sync buffers
// (SimulationSerialization.h / SimulationComposite.h), so the serialized size of
// one input is a compile-time constant for a given InputType and entries are a
// fixed stride — the ring can therefore be scanned, and any entry rewritten IN
// PLACE, without a per-entry length prefix.
//
// ORDER IS NOT MEANINGFUL. Entries sit at ring positions, not in arrival order —
// after the first eviction the oldest slot holds the newest entry. Consumers MUST
// key by `captureTick` (which is the point of the whole design) and must never
// treat entry 0 as "the first" or the last entry as "the latest".
//
// DEPTH IS SESSION-FIXED. `depth` is passed per write (from
// TimeConfig::relayRedundancyDepthTicks) rather than stored on the wire — the
// receiver never needs it, it just iterates what arrived. Growing the ring
// mid-session works (the next write appends); SHRINKING it mid-session does not
// reclaim already-allocated entries — a lowered depth degenerates to
// replace-oldest at the larger size. TimeConfig is constructed once per session,
// so this is a documented non-scenario rather than a supported one.
//
// WIRE VERSION. This payload carries its OWN version byte, starting at 1. It is
// NOT the `kWireFormatVersion` fence on FSimulationStateSyncBuffer that T4 bumps
// 1 -> 2: this is a brand-new property whose first-ever format is version 1, so
// nothing is being bumped and the initiative still has exactly one fence bump.
// ---------------------------------------------------------------------------

#include "OGSimulation/SimulationSerialization.h"
#include "OGSimulation/SimulationComposite.h"

#include <cstdint>

namespace relayedInputRing
{
	// Wire-format version of the relay ring payload. Bumped when the on-wire
	// layout changes so a mismatched peer can be detected. Independent of the
	// sync buffers' fences (see the WIRE VERSION note above).
	inline constexpr std::uint8_t kWireFormatVersion = 1;

	// Hard upper bound on resident entries, for wire safety. The runtime depth is
	// min(TimeConfig::relayRedundancyDepthTicks, kMaxDepth).
	//
	// 8 mirrors FInputRedundancyBundle::kMaxSlots deliberately: it is the same
	// class of bound (a per-character input payload budget), so the two directions
	// stay comparable when the T9 bandwidth measurement is taken. Raising it is a
	// deliberate wire-budget decision, not a tuning tweak — depth 8 already costs
	// 8x the per-character input bytes of today's depth-1 default.
	inline constexpr std::uint8_t kMaxDepth = 8;

	// Wire-payload header layout.
	inline constexpr std::uint32_t kVersionOffset    = 0;
	inline constexpr std::uint32_t kEntryCountOffset = 1;
	inline constexpr std::uint32_t kHeaderBytes      = 2;

	// --- MALFORMED-LENGTH BOUND ------------------------------------------------
	// [og-netcode-v2-input-relay T29] These two constants and the predicate below
	// were FRelayedInputRing::kMaxInputBytes / ::kMaxWireBytes and an inline
	// comparison inside that USTRUCT's NetSerialize. They moved here for the same
	// reason every other rule in this file lives here: the LLT target links Core +
	// OGSimulation and cannot see a USTRUCT, so a guard expressed inside one is
	// untestable. The USTRUCT keeps both names as aliases, so every existing
	// reference still compiles and the WIRE FORMAT IS UNCHANGED — this is a move,
	// not a redesign.
	//
	// Upper bound on the serialized input payload of ONE entry. Anchored to the
	// project's declared per-input wire budget (FSimulationInputSyncBuffer's
	// kBufferBytes = 128, which covers a tick header + one input composite), so
	// this bound cannot silently fall behind a growing input composite. It is
	// deliberately a plain constant here rather than a reference to that USTRUCT:
	// core cannot name it, and the two are pinned together by this comment and by
	// the alias on FRelayedInputRing.
	inline constexpr std::uint32_t kMaxInputBytes = 128;

	// Largest payload a well-formed ring can produce: header + kMaxDepth entries of
	// (captureTick + dA + input). Used ONLY to reject a malformed inbound length —
	// a sender never approaches it at the shipped depth of 1.
	inline constexpr std::uint32_t kMaxWireBytes =
		kHeaderBytes
		+ static_cast<std::uint32_t>(kMaxDepth)
			* (static_cast<std::uint32_t>(sizeof(std::uint32_t) + sizeof(std::uint8_t)) + kMaxInputBytes);

	// True when an inbound byte count is within what any well-formed sender can
	// produce. A length above the bound means a corrupt or hostile bunch: the
	// transport must refuse it rather than allocate and read that many bytes.
	//
	// NOTE FOR ANYONE READING THIS AS "the guard is enforced": this predicate is
	// the RULE. Whether it RUNS is a property of the transport that calls it — and
	// from the day Iris replication went live until T29 it did not run at all,
	// because Iris never called the NetSerialize it lived in. That is why T29
	// registers an Iris serializer as well as extracting this.
	constexpr bool isAcceptableWireLength(std::uint32_t byteCount)
	{
		return byteCount <= kMaxWireBytes;
	}

	// THE derivation. The tick a relayed input is scheduled to be APPLIED at is
	// `captureTick + dA` — always computed, never stored, so it cannot drift from
	// the two fields it is made of. Peers use it to place a relayed input on their
	// own timeline (RelayDelaySpectrumDesign.md §4/§5.2); T7's scheduled read is
	// the inverse of this same relation.
	constexpr std::uint32_t applicationTick(std::uint32_t captureTick, std::uint8_t dA)
	{
		return captureTick + static_cast<std::uint32_t>(dA);
	}

	namespace detail
	{
		// Compile-time serialized byte size of a SimulationComposite payload.
		// Primary template intentionally undefined: only Serializable types
		// (handled separately) or SimulationComposite specializations are valid
		// relay inputs.
		template <typename T>
		struct CompositeSerializedSize;

		template <typename... Ts>
		struct CompositeSerializedSize<SimulationComposite<Ts...>>
		{
			static constexpr std::uint32_t value = compositeSyncSize<Ts...>();
		};

		// SimulationComposite is not Serializable (it has no SerializableFields
		// specialization), so the Serializable branch cleanly distinguishes a
		// plain field-serializable InputType from a composite.
		template <typename InputType>
		constexpr std::uint32_t entryInputSize()
		{
			if constexpr (Serializable<InputType>)
				return syncSize<InputType>();
			else
				return CompositeSerializedSize<InputType>::value;
		}

		// Fixed on-wire stride of one entry: captureTick + dA stamp + input.
		template <typename InputType>
		constexpr std::uint32_t entryStride()
		{
			return static_cast<std::uint32_t>(sizeof(std::uint32_t))
			     + static_cast<std::uint32_t>(sizeof(std::uint8_t))
			     + entryInputSize<InputType>();
		}

		template <typename InputType>
		constexpr std::uint32_t entryOffset(std::uint8_t index)
		{
			return kHeaderBytes + static_cast<std::uint32_t>(index) * entryStride<InputType>();
		}

		template <typename InputType, typename Buffer>
		void writeInput(Buffer& buf, std::uint32_t offset, const InputType& input)
		{
			if constexpr (Serializable<InputType>)
				writeToSyncedBuffer(input, buf, offset);
			else
				writeCompositeToSyncedBuffer(input, buf, offset);
		}

		template <typename InputType, typename Buffer>
		void readInput(const Buffer& buf, std::uint32_t offset, InputType& out)
		{
			if constexpr (Serializable<InputType>)
				readFromSyncedBuffer(out, buf, offset);
			else
				readCompositeFromSyncedBuffer(out, buf, offset);
		}

		// Lazily writes the wire header ([version][entryCount = 0]) on first use,
		// so a default-constructed (never-written) ring costs zero bytes on the
		// wire rather than an empty header.
		template <typename Buffer>
		void initHeaderIfEmpty(Buffer& buf)
		{
			if (buf.bundleByteNum() == 0)
			{
				buf.bundleAddZeroedBytes(static_cast<std::int32_t>(kHeaderBytes));
				buf.writeToBuffer(kVersionOffset,    kWireFormatVersion);
				buf.writeToBuffer(kEntryCountOffset, static_cast<std::uint8_t>(0));
			}
		}

		// Runtime depth, clamped into [1, kMaxDepth]. A configured 0 (or negative)
		// would make the ring unwritable, which would silently disable the relay —
		// far worse than the degenerate depth-1 behaviour the initiative ships with.
		inline std::uint8_t clampDepth(std::int32_t requestedDepth)
		{
			if (requestedDepth < 1)
				return 1;
			if (requestedDepth > static_cast<std::int32_t>(kMaxDepth))
				return kMaxDepth;
			return static_cast<std::uint8_t>(requestedDepth);
		}
	} // namespace detail

	// Reads the version byte. Returns 0 on an empty ring (no header written yet)
	// so a never-written ring never reads as a version mismatch.
	template <typename Buffer>
	std::uint8_t getWireFormatVersion(const Buffer& buf)
	{
		if (buf.bundleByteNum() < static_cast<std::int32_t>(kHeaderBytes))
			return 0;
		return buf.template readFromBuffer<std::uint8_t>(kVersionOffset);
	}

	// Number of entries currently resident (0 on an empty ring).
	template <typename Buffer>
	std::uint8_t entryCount(const Buffer& buf)
	{
		if (buf.bundleByteNum() < static_cast<std::int32_t>(kHeaderBytes))
			return 0;
		return buf.template readFromBuffer<std::uint8_t>(kEntryCountOffset);
	}

	// Capture tick of entry `index` (caller guarantees index < entryCount).
	template <typename InputType, typename Buffer>
	std::uint32_t captureTickAt(const Buffer& buf, std::uint8_t index)
	{
		return buf.template readFromBuffer<std::uint32_t>(detail::entryOffset<InputType>(index));
	}

	// True if `captureTick` is currently resident.
	template <typename InputType, typename Buffer>
	bool containsCaptureTick(const Buffer& buf, std::uint32_t captureTick)
	{
		const std::uint8_t count = entryCount(buf);
		for (std::uint8_t i = 0; i < count; ++i)
		{
			if (captureTickAt<InputType>(buf, i) == captureTick)
				return true;
		}
		return false;
	}

	// Capture-tick lookup. On a hit, writes the entry's stamp into `outDA` and its
	// value into `outInput` and returns true; on a miss returns false and leaves
	// both untouched. This is the read the client-side store (T5) is populated
	// from, and the shape T7's scheduled read is built on.
	template <typename InputType, typename Buffer>
	bool findEntry(const Buffer& buf, std::uint32_t captureTick, std::uint8_t& outDA, InputType& outInput)
	{
		const std::uint8_t count = entryCount(buf);
		for (std::uint8_t i = 0; i < count; ++i)
		{
			const std::uint32_t offset = detail::entryOffset<InputType>(i);
			if (buf.template readFromBuffer<std::uint32_t>(offset) != captureTick)
				continue;

			outDA = buf.template readFromBuffer<std::uint8_t>(
				offset + static_cast<std::uint32_t>(sizeof(std::uint32_t)));
			detail::readInput<InputType>(
				buf,
				offset + static_cast<std::uint32_t>(sizeof(std::uint32_t) + sizeof(std::uint8_t)),
				outInput);
			return true;
		}
		return false;
	}

	// REPLACE-LATEST WRITE — the whole point of this codec.
	//
	// Stores (captureTick, dA, input) into the ring, retaining at most
	// `depth` (clamped to [1, kMaxDepth]) of the most recent capture ticks:
	//
	//   1. `captureTick` already resident  -> that entry is REWRITTEN IN PLACE.
	//      Deliberately NOT the bundle's append-only/immutable behaviour: a
	//      re-stamp of an already-relayed tick (e.g. the same input re-relayed
	//      after a tier change moved dA) must update, not assert. This is the
	//      exact fable-B2 failure mode this payload exists to avoid.
	//   2. Ring below depth -> the entry is APPENDED (the ring is still filling).
	//   3. Ring at depth -> the OLDEST resident entry (lowest capture tick) is
	//      SUPERSEDED, i.e. overwritten in place. Nothing shifts; entry order is
	//      ring position, not age.
	//
	// Returns true when the entry is now resident. Returns false — leaving the
	// buffer byte-for-byte untouched — in exactly one case: the ring is at depth
	// and `captureTick` is OLDER than every resident entry, so accepting it would
	// evict fresher data to store staler data. Unlike the bundle's tryAppendSlot,
	// a false return here is NOT a producer bug: the server's `acceptedNew`
	// watermark means the production path only ever relays strictly newer ticks,
	// so this arm is a defensive guard for an out-of-order caller, not an error
	// channel. Callers may ignore the result.
	template <typename InputType, typename Buffer>
	bool writeLatest(Buffer& buf,
	                 std::uint32_t captureTick,
	                 std::uint8_t dA,
	                 const InputType& input,
	                 std::int32_t depth)
	{
		detail::initHeaderIfEmpty(buf);

		const std::uint8_t  effectiveDepth = detail::clampDepth(depth);
		const std::uint32_t stride         = detail::entryStride<InputType>();
		const std::uint8_t  count          = entryCount(buf);

		// (1) Rewrite in place.
		std::uint8_t targetIndex = count;
		for (std::uint8_t i = 0; i < count; ++i)
		{
			if (captureTickAt<InputType>(buf, i) == captureTick)
			{
				targetIndex = i;
				break;
			}
		}

		if (targetIndex == count)
		{
			if (count < effectiveDepth)
			{
				// (2) Still filling — grow by one entry.
				buf.bundleAddZeroedBytes(static_cast<std::int32_t>(stride));
				buf.writeToBuffer(kEntryCountOffset, static_cast<std::uint8_t>(count + 1));
				// targetIndex == count is already the new slot's index.
			}
			else
			{
				// (3) At depth — supersede the oldest, unless this write IS the
				// oldest, in which case there is nothing stale to evict.
				std::uint8_t  oldestIndex = 0;
				std::uint32_t oldestTick  = captureTickAt<InputType>(buf, 0);
				for (std::uint8_t i = 1; i < count; ++i)
				{
					const std::uint32_t tick = captureTickAt<InputType>(buf, i);
					if (tick < oldestTick)
					{
						oldestTick  = tick;
						oldestIndex = i;
					}
				}

				if (captureTick < oldestTick)
					return false;

				targetIndex = oldestIndex;
			}
		}

		const std::uint32_t offset = detail::entryOffset<InputType>(targetIndex);
		buf.writeToBuffer(offset, captureTick);
		buf.writeToBuffer(offset + static_cast<std::uint32_t>(sizeof(std::uint32_t)), dA);
		detail::writeInput<InputType>(
			buf,
			offset + static_cast<std::uint32_t>(sizeof(std::uint32_t) + sizeof(std::uint8_t)),
			input);
		return true;
	}

	// Consumer-side iteration: invokes callback(captureTick, dA, input) once per
	// resident entry, in RING-POSITION order (see the ORDER IS NOT MEANINGFUL note
	// at the top — key by captureTick, never by position). No-op on an empty ring.
	template <typename InputType, typename Buffer, typename Callback>
	void forEachEntry(const Buffer& buf, Callback&& callback)
	{
		const std::uint8_t count = entryCount(buf);
		for (std::uint8_t i = 0; i < count; ++i)
		{
			const std::uint32_t offset     = detail::entryOffset<InputType>(i);
			const std::uint32_t captureTick = buf.template readFromBuffer<std::uint32_t>(offset);
			const std::uint8_t  dA          = buf.template readFromBuffer<std::uint8_t>(
				offset + static_cast<std::uint32_t>(sizeof(std::uint32_t)));

			InputType input{};
			detail::readInput<InputType>(
				buf,
				offset + static_cast<std::uint32_t>(sizeof(std::uint32_t) + sizeof(std::uint8_t)),
				input);

			callback(captureTick, dA, input);
		}
	}
} // namespace relayedInputRing
