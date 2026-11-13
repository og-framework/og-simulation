#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/RelayedInputRingCodec-rationale.md · docs/RelayedInputRingCodec-guards.md

#include "OGSimulation/OGAssert.h"
#include "OGSimulation/SimulationSerialization.h"
#include "OGSimulation/SimulationComposite.h"

#include <array>
#include <cstdint>

namespace relayedInputRing
{
	inline constexpr std::uint8_t kWireFormatVersion = 3;

	inline constexpr std::uint8_t kMaxDepth = 8;

	inline constexpr std::uint8_t kMaxResendEntries = 2;

	static_assert(kMaxResendEntries < kMaxDepth,
		"a flush re-sends at most kMaxResendEntries of the last flushed entries and always publishes at least one new "
		"one, so kMaxResendEntries must stay below kMaxDepth or the re-send could fill the ring alone. Was the "
		"og-input-latency task 8 rule that the re-send never raises the entry count above kMaxDepth");

	constexpr std::uint8_t clampDepth(std::int32_t requestedDepth)
	{
		if (requestedDepth < 1)
			return 1;
		if (requestedDepth > static_cast<std::int32_t>(kMaxDepth))
			return kMaxDepth;
		return static_cast<std::uint8_t>(requestedDepth);
	}

	static_assert(clampDepth(0) == 1 && clampDepth(-1) == 1 && clampDepth(-1000000) == 1,
		"clampDepth clamps 0 and negatives UP to 1: a depth-0 ring is unwritable and would silently disable the relay. "
		"Was the codec comment '0 and negatives clamp UP to 1; 0 is not \"off\"'");
	static_assert(clampDepth(static_cast<std::int32_t>(kMaxDepth) + 1) == kMaxDepth && clampDepth(1000000) == kMaxDepth,
		"clampDepth caps at kMaxDepth, the wire budget, rather than wrapping. Was the codec comment on the shared depth "
		"guard");

	constexpr std::uint8_t clampResendEntries(std::int32_t requestedEntries)
	{
		if (requestedEntries < 0)
			return 0;
		if (requestedEntries > static_cast<std::int32_t>(kMaxResendEntries))
			return kMaxResendEntries;
		return static_cast<std::uint8_t>(requestedEntries);
	}

	static_assert(clampResendEntries(0) == 0 && clampResendEntries(-1) == 0
			&& clampResendEntries(static_cast<std::int32_t>(kMaxResendEntries) + 1) == kMaxResendEntries,
		"clampResendEntries keeps a flush's re-send count in [0, kMaxResendEntries]; 0 is today's flush. "
		"og-input-latency task 8");

	inline constexpr std::uint32_t kVersionOffset    = 0;
	inline constexpr std::uint32_t kEntryCountOffset = 1;
	inline constexpr std::uint32_t kHeaderBytes      = 2;

	inline constexpr std::uint32_t kMaxInputBytes = 128;

	inline constexpr std::uint32_t kMaxEntryBytes =
		static_cast<std::uint32_t>(sizeof(std::uint32_t) + sizeof(std::uint8_t)) + kMaxInputBytes;

	inline constexpr std::uint32_t kMaxWireBytes =
		kHeaderBytes
		+ static_cast<std::uint32_t>(kMaxDepth)
			* (static_cast<std::uint32_t>(sizeof(std::uint32_t) + sizeof(std::uint8_t)) + kMaxInputBytes);

	static_assert(kMaxWireBytes == kHeaderBytes + static_cast<std::uint32_t>(kMaxDepth) * kMaxEntryBytes,
		"kMaxEntryBytes must be the per-entry term of kMaxWireBytes: the re-send tail buffer is sized by it. "
		"og-input-latency task 8");

	constexpr bool isAcceptableWireLength(std::uint32_t byteCount)
	{
		return byteCount <= kMaxWireBytes;
	}

	constexpr std::uint32_t applicationTick(std::uint32_t captureTick, std::uint8_t dA)
	{
		return captureTick + static_cast<std::uint32_t>(dA);
	}

	namespace detail
	{
		template <typename T>
		struct CompositeSerializedSize;

		template <typename... Ts>
		struct CompositeSerializedSize<SimulationComposite<Ts...>>
		{
			static constexpr std::uint32_t value = compositeSyncSize<Ts...>();
		};

		template <typename InputType>
		constexpr std::uint32_t entryInputSize()
		{
			if constexpr (Serializable<InputType>)
				return syncSize<InputType>();
			else
				return CompositeSerializedSize<InputType>::value;
		}

		template <typename InputType>
		constexpr std::uint32_t entryStride()
		{
			static_assert(entryInputSize<InputType>() <= kMaxInputBytes,
				"relayed input exceeds kMaxInputBytes: a full-depth ring would be refused by isAcceptableWireLength. "
				"Was a comment-only anchor to the retired 128-byte input sync buffer (og-syncedInput-rework task 9)");
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

		template <typename Buffer>
		std::uint32_t captureTickAtStride(const Buffer& buf, std::uint8_t index, std::uint32_t stride)
		{
			return buf.template readFromBuffer<std::uint32_t>(kHeaderBytes + static_cast<std::uint32_t>(index) * stride);
		}

		template <typename Buffer>
		bool holdsCaptureTickAtStride(const Buffer& buf, std::uint8_t count, std::uint32_t stride, std::uint32_t captureTick)
		{
			for (std::uint8_t i = 0; i < count; ++i)
			{
				if (captureTickAtStride(buf, i, stride) == captureTick)
					return true;
			}
			return false;
		}

		struct ResendTail
		{
			std::uint8_t                                                count = 0u;
			std::array<std::uint8_t, kMaxResendEntries * kMaxEntryBytes> bytes{};
		};

		template <typename DstBuffer, typename StageBuffer>
		ResendTail takeResendTail(const DstBuffer& dst,
		                          const StageBuffer& stage,
		                          std::uint8_t stagedCount,
		                          std::uint32_t stride,
		                          std::uint8_t wanted)
		{
			ResendTail tail;
			const std::uint8_t room = (stagedCount < kMaxDepth) ? static_cast<std::uint8_t>(kMaxDepth - stagedCount) : 0u;
			const std::uint8_t take = (wanted < room) ? wanted : room;
			if (take == 0u || dst.bundleByteNum() < static_cast<std::int32_t>(kHeaderBytes)
				|| dst.template readFromBuffer<std::uint8_t>(kVersionOffset) != kWireFormatVersion)
				return tail;

			const std::uint8_t publishedCount = dst.template readFromBuffer<std::uint8_t>(kEntryCountOffset);
			OG_CHECK(static_cast<std::uint32_t>(dst.bundleByteNum()) == kHeaderBytes + publishedCount * stride,
				"relayedInputRing::flushStagedInto: the published ring and the stage disagree on the entry stride, so "
				"they do not carry one InputType and no entry of the ring can be re-sent");
			OG_CHECK(stride <= kMaxEntryBytes,
				"relayedInputRing::flushStagedInto: an entry stride above kMaxEntryBytes cannot come from entryStride");

			std::uint8_t  newestFirst[kMaxResendEntries] = {};
			std::uint32_t below     = 0u;
			bool          haveBelow = false;
			while (tail.count < take)
			{
				bool          found    = false;
				std::uint8_t  bestIdx  = 0u;
				std::uint32_t bestTick = 0u;
				for (std::uint8_t i = 0; i < publishedCount; ++i)
				{
					const std::uint32_t tick = captureTickAtStride(dst, i, stride);
					if ((haveBelow && tick >= below) || holdsCaptureTickAtStride(stage, stagedCount, stride, tick))
						continue;
					if (!found || tick > bestTick)
					{
						found    = true;
						bestIdx  = i;
						bestTick = tick;
					}
				}
				if (!found)
					break;
				newestFirst[tail.count++] = bestIdx;
				below     = bestTick;
				haveBelow = true;
			}

			for (std::uint8_t k = 0; k < tail.count; ++k)
			{
				const std::uint32_t from = kHeaderBytes + static_cast<std::uint32_t>(newestFirst[tail.count - 1u - k]) * stride;
				for (std::uint32_t b = 0; b < stride; ++b)
				{
					tail.bytes[static_cast<std::size_t>(k) * stride + b] = dst.template readFromBuffer<std::uint8_t>(from + b);
				}
			}
			return tail;
		}
	} // namespace detail

	template <typename Buffer>
	std::uint8_t getWireFormatVersion(const Buffer& buf)
	{
		if (buf.bundleByteNum() < static_cast<std::int32_t>(kHeaderBytes))
			return 0;
		return buf.template readFromBuffer<std::uint8_t>(kVersionOffset);
	}

	template <typename Buffer>
	std::uint8_t entryCount(const Buffer& buf)
	{
		if (buf.bundleByteNum() < static_cast<std::int32_t>(kHeaderBytes))
			return 0;
		return buf.template readFromBuffer<std::uint8_t>(kEntryCountOffset);
	}

	template <typename InputType, typename Buffer>
	std::uint32_t captureTickAt(const Buffer& buf, std::uint8_t index)
	{
		return buf.template readFromBuffer<std::uint32_t>(detail::entryOffset<InputType>(index));
	}

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

	template <typename InputType, typename Buffer>
	bool writeLatest(Buffer& buf,
	                 std::uint32_t captureTick,
	                 std::uint8_t dA,
	                 const InputType& input,
	                 std::int32_t depth)
	{
		detail::initHeaderIfEmpty(buf);

		const std::uint8_t  effectiveDepth = clampDepth(depth);
		const std::uint32_t stride         = detail::entryStride<InputType>();
		const std::uint8_t  count          = entryCount(buf);

		std::uint8_t targetIndex = count;
		for (std::uint8_t i = 0; i < count; ++i)
		{
			// ⛔G-01  docs/RelayedInputRingCodec-guards.md
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
				buf.bundleAddZeroedBytes(static_cast<std::int32_t>(stride));
				buf.writeToBuffer(kEntryCountOffset, static_cast<std::uint8_t>(count + 1));
			}
			else
			{
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

	struct StageArrivalOutcome
	{
		bool accepted = false;
		bool droppedOldest = false;
	};

	template <typename InputType, typename Buffer>
	StageArrivalOutcome stageArrival(Buffer& stage,
	                                 std::uint32_t captureTick,
	                                 std::uint8_t dA,
	                                 const InputType& input)
	{
		StageArrivalOutcome outcome;

		const bool atCapacity = entryCount(stage) >= kMaxDepth;
		const bool wouldEvict = atCapacity && !containsCaptureTick<InputType>(stage, captureTick);

		// ⛔G-02  docs/RelayedInputRingCodec-guards.md
		outcome.accepted = writeLatest<InputType>(
			stage, captureTick, dA, input, static_cast<std::int32_t>(kMaxDepth));

		outcome.droppedOldest = wouldEvict && outcome.accepted;
		return outcome;
	}

	template <typename Buffer>
	void resetEntries(Buffer& buf)
	{
		if (buf.bundleByteNum() < static_cast<std::int32_t>(kHeaderBytes))
			return;

		buf.writeToBuffer(kEntryCountOffset, static_cast<std::uint8_t>(0));
		// ⛔G-03  docs/RelayedInputRingCodec-guards.md
		buf.bundleTruncateTo(static_cast<std::int32_t>(kHeaderBytes));
	}

	template <typename DstBuffer, typename StageBuffer>
	std::uint8_t flushStagedInto(DstBuffer& dst, StageBuffer& stage, std::int32_t resendEntries = 0)
	{
		const std::uint8_t stagedCount = entryCount(stage);
		// ⛔G-04  docs/RelayedInputRingCodec-guards.md
		if (stagedCount == 0u)
			return 0u;

		const std::int32_t stagedBytes = stage.bundleByteNum();
		const std::uint32_t stride =
			static_cast<std::uint32_t>(stagedBytes - static_cast<std::int32_t>(kHeaderBytes)) / stagedCount;
		OG_CHECK(static_cast<std::uint32_t>(stagedBytes) == kHeaderBytes + stagedCount * stride,
			"relayedInputRing::flushStagedInto: the stage's byte count is not a whole number of entries");

		// ⛔G-05  docs/RelayedInputRingCodec-guards.md
		const detail::ResendTail tail =
			detail::takeResendTail(dst, stage, stagedCount, stride, clampResendEntries(resendEntries));
		const std::int32_t tailBytes = static_cast<std::int32_t>(tail.count * stride);
		OG_CHECK(tail.count + stagedCount <= kMaxDepth,
			"relayedInputRing::flushStagedInto: a flush never publishes more than kMaxDepth entries, re-sent ones included");

		detail::initHeaderIfEmpty(dst);
		// ⛔G-06  docs/RelayedInputRingCodec-guards.md
		dst.bundleTruncateTo(static_cast<std::int32_t>(kHeaderBytes));
		dst.bundleAddZeroedBytes(tailBytes + stagedBytes - static_cast<std::int32_t>(kHeaderBytes));

		for (std::int32_t offset = 0; offset < tailBytes; ++offset)
		{
			dst.writeToBuffer(
				kHeaderBytes + static_cast<std::uint32_t>(offset),
				tail.bytes[static_cast<std::size_t>(offset)]);
		}

		for (std::int32_t offset = static_cast<std::int32_t>(kHeaderBytes);
		     offset < stagedBytes; ++offset)
		{
			dst.writeToBuffer(
				static_cast<std::uint32_t>(offset + tailBytes),
				stage.template readFromBuffer<std::uint8_t>(static_cast<std::uint32_t>(offset)));
		}

		// ⛔G-07  docs/RelayedInputRingCodec-guards.md
		dst.writeToBuffer(kVersionOffset,    kWireFormatVersion);
		dst.writeToBuffer(kEntryCountOffset, static_cast<std::uint8_t>(tail.count + stagedCount));

		resetEntries(stage);
		return stagedCount;
	}

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
