#pragma once
// SPDX-License-Identifier: MPL-2.0

// ---------------------------------------------------------------------------
// InputRedundancyBundleCodec
//
// Engine-agnostic byte-codec for the FInputRedundancyBundle wire payload
// (proposal §3.1 step 8; R-T5 immutability invariant; risks_and_plan.md §5.2).
//
// The unreliable + redundancy input channel re-sends the last
// `redundancyDepthTicks` ticks of input every frame so a dropped UDP datagram
// self-heals on the next send (the R-T1 streeting-saturation mitigation). The
// bundle on the wire is a length-prefixed byte blob:
//
//   [version_byte (u8)]  offset 0  == kWireFormatVersion
//   [slot_count   (u8)]  offset 1  == number of slots actually appended
//   [slots...]           offset 2  each slot is:
//       [capture_tick (u32)] [input_serialized_bytes (fixed per InputType)]
//
// WHY THIS LIVES IN CORE: the bundle's append/dedup/iteration logic is
// pure byte manipulation over the og-simulation serialization free functions —
// it has no Unreal dependency. The UE-side wrapper FInputRedundancyBundle
// (Source/OGSimulationUnreal/SyncedSimulationStateBuffer.h) is a USTRUCT that
// owns a UPROPERTY TArray<uint8> for replication and *delegates* every operation
// to the templates here. Hoisting the algorithm into core lets the pure-C++
// Low-Level-Tests exercise the REAL code path (WireFormat_Bundle.cpp) without
// linking the engine-coupled OGSimulationUnreal module. The UE-only parts of the
// wire format (custom NetSerialize watermark trim on the correction/input sync
// buffers) are not codified here and their regression tests are deferred to a
// future engine-coupled LLT target — see docs/low-level-tests.md
// "Future: testing UE-coupled code".
//
// BUFFER CONCEPT: every template below operates on a `Buffer` that exposes
//   - std::int32_t bundleByteNum() const;                  // current byte count
//   - void          bundleAddZeroedBytes(std::int32_t n);  // grow by n zeroed bytes
//   - template <typename T> void writeToBuffer(std::uint32_t off, const T& v);
//   - template <typename T> T    readFromBuffer(std::uint32_t off) const;
// FInputRedundancyBundle (TArray-backed) and the test buffer (std::vector-backed)
// both satisfy this. The per-slot input serialization reuses the same field-wise
// machinery as FSimulationStateSyncBuffer (SimulationSerialization.h /
// SimulationComposite.h): the serialized size of one slot's input is a
// compile-time constant for a given InputType, so slots are a fixed stride and
// the bundle can be scanned without an explicit per-slot length prefix:
//   - plain Serializable InputType -> syncSize<InputType>()
//   - SimulationComposite<Ts...>   -> compositeSyncSize<Ts...>()
//
// INVARIANT (R-T5 / D3.11): inputs written into FInputRedundancyBundle are
// append-only and immutable per capture-tick. The client MUST NOT revise the
// input value for a previously-emitted capture-tick; any producer that does so
// silently loses the corrected value at the dedup-by-capture-tick step on the
// server. If a future feature requires input revision post-capture, the wire
// format needs an explicit revision-number field and the dedup contract must be
// re-designed.
//
// ENFORCEMENT — two layers, deliberately different per build config (D3.11):
//   - Development / checked builds: appendSlot() OG_CHECK-fails loudly on a
//     duplicate capture_tick, so a producer bug surfaces at its origin rather
//     than as a silent value loss three hops downstream on the server.
//   - Shipping builds (OG_CHECK compiles out): the duplicate is SILENTLY
//     DROPPED and the first-arrival input is preserved. Never overwrite. A
//     shipping client must not be able to revise an already-emitted input, and
//     an unguarded append would additionally emit two slots for one capture
//     tick — a malformed bundle the server's dedup would resolve arbitrarily by
//     arrival order.
// Both layers share one always-compiled kernel, tryAppendSlot(), which owns the
// drop decision; appendSlot() is that kernel plus the debug-time OG_CHECK. The
// shipping semantics are therefore testable in a checked build (call the kernel
// directly) without needing a shipping-configured test target.
//
// The kMaxSlots wire-safety bound is enforced the SAME way (T16): the kernel
// drops an append that would exceed kMaxSlots in every build config, and
// appendSlot() keeps its OG_CHECK in front so a checked build still aborts
// loudly at the offending producer. Note this is DEFENCE IN DEPTH, not a live
// bug fix: the sole production caller (buildRedundancyBundle) already clamps
// depth to min(redundancyDepthTicks, kMaxSlots), so an overflow is unreachable
// through the current call graph. The kernel bound exists so a future caller
// that skips the clamp — or a rewritten client driving the codec directly —
// cannot emit an over-budget bundle in a shipping build, where an unguarded
// append previously produced a bounded-but-MALFORMED payload (the TArray grows,
// so this was never an out-of-bounds write; it was a wire-budget violation).
//
// The version byte exists so pre/post-Stage-1 builds refuse to interop loudly
// (compat fence, Task 11).
// ---------------------------------------------------------------------------

#include "OGSimulation/SimulationSerialization.h"
#include "OGSimulation/SimulationComposite.h"
#include "OGSimulation/OGAssert.h"

#include <cstdint>
#include <utility>

namespace inputRedundancyBundle
{
	// Wire-format version. Bumped when the on-wire layout changes so a
	// mismatched peer is detected and rejected (compat fence).
	inline constexpr std::uint8_t  kWireFormatVersion = 1;
	// Hard upper bound on slots for wire safety; the runtime depth is
	// min(TimeConfig::redundancyDepthTicks, kMaxSlots).
	inline constexpr std::uint8_t  kMaxSlots          = 8;

	// Wire-payload header layout.
	inline constexpr std::uint32_t kVersionOffset   = 0;
	inline constexpr std::uint32_t kSlotCountOffset = 1;
	inline constexpr std::uint32_t kHeaderBytes     = 2;

	namespace detail
	{
		// Compile-time serialized byte size of a SimulationComposite's input
		// payload. Primary template intentionally undefined: only Serializable
		// types (handled separately) or SimulationComposite specializations are
		// valid bundle inputs.
		template <typename T>
		struct CompositeSerializedSize;

		template <typename... Ts>
		struct CompositeSerializedSize<SimulationComposite<Ts...>>
		{
			static constexpr std::uint32_t value = compositeSyncSize<Ts...>();
		};

		// Compile-time serialized byte size of one slot's input payload.
		// SimulationComposite is not Serializable (no SerializableFields
		// specialization), so the Serializable branch cleanly distinguishes a
		// plain field-serializable InputType from a SimulationComposite.
		template <typename InputType>
		constexpr std::uint32_t slotInputSize()
		{
			if constexpr (Serializable<InputType>)
				return syncSize<InputType>();
			else
				return CompositeSerializedSize<InputType>::value;
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

		// Lazily writes the wire header ([version][slot_count=0]) on first use.
		template <typename Buffer>
		void initHeaderIfEmpty(Buffer& buf)
		{
			if (buf.bundleByteNum() == 0)
			{
				buf.bundleAddZeroedBytes(static_cast<std::int32_t>(kHeaderBytes));
				buf.writeToBuffer(kVersionOffset,   kWireFormatVersion);
				buf.writeToBuffer(kSlotCountOffset, static_cast<std::uint8_t>(0));
			}
		}
	} // namespace detail

	// Reads the version byte (first byte). Returns 0 on an empty bundle (no
	// header written yet) so a never-populated bundle never reads as a mismatch.
	template <typename Buffer>
	std::uint8_t getWireFormatVersion(const Buffer& buf)
	{
		if (buf.bundleByteNum() == 0)
			return 0;
		return buf.template readFromBuffer<std::uint8_t>(kVersionOffset);
	}

	// Number of slots currently appended (0 on an empty bundle).
	template <typename Buffer>
	std::uint8_t slotCount(const Buffer& buf)
	{
		if (buf.bundleByteNum() < static_cast<std::int32_t>(kHeaderBytes))
			return 0;
		return buf.template readFromBuffer<std::uint8_t>(kSlotCountOffset);
	}

	// True once the bundle holds kMaxSlots slots — a further append is dropped by
	// tryAppendSlot() in every build config, and additionally OG_CHECK-fails in a
	// checked build via appendSlot(). Exposed so tests and callers can query the
	// bound without tripping the abort.
	template <typename Buffer>
	bool isFull(const Buffer& buf)
	{
		return slotCount(buf) >= kMaxSlots;
	}

	// True if `captureTick` already occupies a slot (the R-T5 condition that
	// appendSlot()'s first OG_CHECK guards). Exposed so tests can verify the
	// guard predicate without tripping the abort-on-violation OG_CHECK.
	template <typename InputType, typename Buffer>
	bool containsCaptureTick(const Buffer& buf, std::uint32_t captureTick)
	{
		const std::uint8_t  count  = slotCount(buf);
		const std::uint32_t stride =
			static_cast<std::uint32_t>(sizeof(std::uint32_t)) + detail::slotInputSize<InputType>();
		std::uint32_t offset = kHeaderBytes;
		for (std::uint8_t i = 0; i < count; ++i)
		{
			if (buf.template readFromBuffer<std::uint32_t>(offset) == captureTick)
				return true;
			offset += stride;
		}
		return false;
	}

	// Always-compiled append kernel — owns the R-T5 append-only decision in EVERY
	// build config, including shipping where OG_CHECK compiles out.
	//
	// Returns true when the slot was appended. Returns false — leaving the buffer
	// byte-for-byte untouched — in either of two cases:
	//   1. `captureTick` already occupies a slot. The first-arrival input is
	//      preserved: an already-emitted capture tick is immutable, so a revision
	//      is dropped rather than overwritten (D3.11 / R-T5).
	//   2. The bundle already holds kMaxSlots slots. Appending would exceed the
	//      wire-safety budget, so the slot is dropped rather than emitted (T16).
	//
	// Callers on the normal producer path should use appendSlot(), which adds the
	// debug-time OG_CHECK on top of this kernel. Call tryAppendSlot() directly
	// only when a duplicate is an expected, recoverable outcome rather than a
	// producer bug (e.g. tests asserting the shipping-build semantics).
	template <typename InputType, typename Buffer>
	[[nodiscard]] bool tryAppendSlot(Buffer& buf, std::uint32_t captureTick, const InputType& input)
	{
		detail::initHeaderIfEmpty(buf);

		// R-T5: first arrival wins. Drop the revision, preserve the emitted value.
		if (containsCaptureTick<InputType>(buf, captureTick))
			return false;

		// Wire-safety bound (D3.11 / T16). The bundle budget is kMaxSlots slots;
		// a 9th append would produce a payload that violates that budget, so it is
		// dropped here in EVERY build config rather than only where OG_CHECK is
		// compiled in. The buffer is left byte-for-byte untouched.
		if (isFull(buf))
			return false;

		const std::uint32_t stride =
			static_cast<std::uint32_t>(sizeof(std::uint32_t)) + detail::slotInputSize<InputType>();
		const std::uint32_t writeOffset = static_cast<std::uint32_t>(buf.bundleByteNum());
		buf.bundleAddZeroedBytes(static_cast<std::int32_t>(stride));
		buf.writeToBuffer(writeOffset, captureTick);
		detail::writeInput<InputType>(
			buf, writeOffset + static_cast<std::uint32_t>(sizeof(std::uint32_t)), input);

		buf.writeToBuffer(kSlotCountOffset, static_cast<std::uint8_t>(slotCount(buf) + 1));
		return true;
	}

	// Producer-side append of one (capture_tick, input) slot. ENFORCES R-T5 and the
	// kMaxSlots wire budget: OG_CHECK-fails loudly on a duplicate capture_tick or
	// an overflow in checked builds, and silently drops either in shipping builds
	// where the OG_CHECK compiles out — see the ENFORCEMENT block at the top of
	// this file. Lazily initializes the wire header on first use.
	template <typename InputType, typename Buffer>
	void appendSlot(Buffer& buf, std::uint32_t captureTick, const InputType& input)
	{
		OG_CHECK(!containsCaptureTick<InputType>(buf, captureTick),
			"R-T5 invariant violation: cannot revise already-sent capture_tick");
		OG_CHECK(!isFull(buf),
			"FInputRedundancyBundle overflow: slot_count would exceed kMaxSlots");

		// Shipping path: the OG_CHECKs above are no-ops, so the kernel's silent
		// drops are what keep an already-emitted capture tick immutable and the
		// slot count within the kMaxSlots wire budget. A false return here means
		// exactly one of those drops — the bundle as already emitted stands, and
		// there is nothing further for the producer to do.
		(void)tryAppendSlot<InputType>(buf, captureTick, input);
	}

	// Consumer-side iteration: invokes callback(std::uint32_t capture_tick,
	// const InputType& input) for each slot in arrival order. No-op on an empty
	// bundle. The server feeds these into RemoteMoveQueue::queueMove.
	template <typename InputType, typename Buffer, typename Callback>
	void forEachSlot(const Buffer& buf, Callback&& callback)
	{
		if (buf.bundleByteNum() < static_cast<std::int32_t>(kHeaderBytes))
			return;

		const std::uint8_t  count  = slotCount(buf);
		const std::uint32_t stride =
			static_cast<std::uint32_t>(sizeof(std::uint32_t)) + detail::slotInputSize<InputType>();
		std::uint32_t offset = kHeaderBytes;
		for (std::uint8_t i = 0; i < count; ++i)
		{
			const std::uint32_t captureTick = buf.template readFromBuffer<std::uint32_t>(offset);
			InputType input{};
			detail::readInput<InputType>(
				buf, offset + static_cast<std::uint32_t>(sizeof(std::uint32_t)), input);
			callback(captureTick, input);
			offset += stride;
		}
	}
} // namespace inputRedundancyBundle
