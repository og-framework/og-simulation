#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimPayloadBuffers-rationale.md · docs/SimPayloadBuffers-guards.md

#include "OGSimulation/CorrectionStateBufferCodec.h"
#include "OGSimulation/InputRedundancyBundleCodec.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/RelayedInputRingCodec.h"
#include "OGSimulation/SimulationComposite.h"
#include "OGSimulation/SimulationSerialization.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

class CorrectionStateBuffer
{
public:
	static constexpr std::uint32_t kBufferBytes       = 384u;
	static constexpr std::uint8_t  kWireFormatVersion = correctionStateBuffer::kWireFormatVersion;

	static_assert(kBufferBytes <= std::numeric_limits<std::uint16_t>::max(),
		"usedBytes is a u16, so kBufferBytes must fit one. Was the u16 used-byte count FSimulationStateSyncBuffer put on the wire");
	static_assert(correctionStateBuffer::kHeaderBytes <= kBufferBytes,
		"the correction header (tick, appliedCaptureTick) must fit the buffer");

	template <typename T>
	void writeToBuffer(std::uint32_t byteIndex, const T& value)
	{
		OG_CHECK(byteIndex + sizeof(T) <= kBufferBytes,
			"CorrectionStateBuffer write OOB (raise kBufferBytes). Was FSimulationStateSyncBuffer's write checkf");
		std::memcpy(m_bytes.data() + byteIndex, &value, sizeof(T));
		const std::uint32_t endByte = byteIndex + static_cast<std::uint32_t>(sizeof(T));
		if (endByte > m_usedBytes)
			m_usedBytes = static_cast<std::uint16_t>(endByte);
	}

	template <typename T>
	T readFromBuffer(std::uint32_t byteIndex) const
	{
		OG_CHECK(byteIndex + sizeof(T) <= kBufferBytes,
			"CorrectionStateBuffer read OOB. Was FSimulationStateSyncBuffer's read checkf");
		T value;
		std::memcpy(&value, m_bytes.data() + byteIndex, sizeof(T));
		return value;
	}

	template <typename... Ts>
	void write(const SimulationComposite<Ts...>& value, std::uint32_t tick, std::uint32_t appliedCaptureTick)
	{
		static_assert(correctionStateBuffer::kPayloadOffset + compositeSyncSize<Ts...>() <= kBufferBytes,
			"the correction header plus this state composite exceeds kBufferBytes (384 B). Re-check the game's state "
			"footprint (og-abilities adds to it) before raising the capacity. Was FSimulationStateSyncBuffer's sizing "
			"comment and its write checkf");
		// ⛔G-02  docs/SimPayloadBuffers-guards.md
		m_usedBytes = 0u;
		correctionStateBuffer::write(*this, value, tick, appliedCaptureTick);
	}

	template <typename... Ts>
	void write(const SimulationComposite<Ts...>& value, std::uint32_t tick)
	{
		write(value, tick, kNoInputCaptureTick);
	}

	template <typename... Ts>
	std::uint32_t readInto(SimulationComposite<Ts...>& outValue) const
	{
		static_assert(correctionStateBuffer::kPayloadOffset + compositeSyncSize<Ts...>() <= kBufferBytes,
			"the correction header plus this state composite exceeds kBufferBytes (384 B). Was FSimulationStateSyncBuffer's "
			"read checkf");
		std::uint32_t ignoredAppliedCaptureTick = kNoInputCaptureTick;
		return correctionStateBuffer::readInto(*this, outValue, ignoredAppliedCaptureTick);
	}

	std::uint32_t getAppliedCaptureTick() const
	{
		// ⛔G-03  docs/SimPayloadBuffers-guards.md
		if (m_usedBytes < correctionStateBuffer::kHeaderBytes)
			return kNoInputCaptureTick;
		return correctionStateBuffer::readAppliedCaptureTick(*this);
	}

	std::uint16_t usedBytes() const { return m_usedBytes; }

	std::span<const std::byte> payload() const
	{
		return std::span<const std::byte>(m_bytes.data(), m_usedBytes);
	}

	[[nodiscard]] bool assignFromWire(std::span<const std::byte> wire)
	{
		if (wire.size() > kBufferBytes)
			return false;
		std::copy(wire.begin(), wire.end(), m_bytes.begin());
		// ⛔G-01  docs/SimPayloadBuffers-guards.md
		m_usedBytes = static_cast<std::uint16_t>(wire.size());
		return true;
	}

private:
	std::array<std::byte, kBufferBytes> m_bytes{};
	std::uint16_t                       m_usedBytes = 0u;
};

namespace simPayloadBuffers::detail
{
	template <std::uint32_t Capacity>
	class GrowableBytes
	{
	public:
		static_assert(Capacity <= static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()),
			"bundleByteNum returns an int32, so the capacity must fit one");

		std::int32_t bundleByteNum() const { return static_cast<std::int32_t>(m_size); }

		void bundleAddZeroedBytes(std::int32_t count)
		{
			OG_CHECK(count >= 0 && m_size + static_cast<std::uint32_t>(count) <= Capacity,
				"payload buffer grow past its fixed wire capacity. Was the unbounded TArray AddZeroed of the UE wrapper");
			std::fill_n(m_bytes.begin() + m_size, static_cast<std::size_t>(count), std::byte{0});
			m_size += static_cast<std::uint32_t>(count);
		}

		void bundleTruncateTo(std::int32_t byteCount)
		{
			if (byteCount < 0)
				byteCount = 0;
			if (static_cast<std::uint32_t>(byteCount) < m_size)
				m_size = static_cast<std::uint32_t>(byteCount);
		}

		template <typename T>
		void writeToBuffer(std::uint32_t byteIndex, const T& value)
		{
			OG_CHECK(byteIndex + sizeof(T) <= m_size,
				"payload buffer write OOB. Was the UE wrapper's write checkf");
			std::memcpy(m_bytes.data() + byteIndex, &value, sizeof(T));
		}

		template <typename T>
		T readFromBuffer(std::uint32_t byteIndex) const
		{
			OG_CHECK(byteIndex + sizeof(T) <= m_size,
				"payload buffer read OOB. Was the UE wrapper's read checkf");
			T value;
			std::memcpy(&value, m_bytes.data() + byteIndex, sizeof(T));
			return value;
		}

		std::span<const std::byte> payload() const
		{
			return std::span<const std::byte>(m_bytes.data(), m_size);
		}

		[[nodiscard]] bool assignFromWire(std::span<const std::byte> wire)
		{
			if (wire.size() > Capacity)
				return false;
			std::copy(wire.begin(), wire.end(), m_bytes.begin());
			m_size = static_cast<std::uint32_t>(wire.size());
			return true;
		}

		void reset() { m_size = 0u; }

	private:
		std::array<std::byte, Capacity> m_bytes{};
		std::uint32_t                   m_size = 0u;
	};
} // namespace simPayloadBuffers::detail

class RelayRingBuffer : public simPayloadBuffers::detail::GrowableBytes<relayedInputRing::kMaxWireBytes>
{
public:
	static constexpr std::uint8_t  kWireFormatVersion = relayedInputRing::kWireFormatVersion;
	static constexpr std::uint8_t  kMaxDepth          = relayedInputRing::kMaxDepth;
	static constexpr std::uint32_t kMaxInputBytes     = relayedInputRing::kMaxInputBytes;
	static constexpr std::uint32_t kMaxWireBytes      = relayedInputRing::kMaxWireBytes;

	static_assert(kMaxWireBytes == 1066u,
		"RelayRingBuffer's capacity drifted from relayedInputRing::kMaxWireBytes. Was FRelayedInputRing's "
		"malformed-length bound pin");

	template <typename InputType>
	bool writeLatest(std::uint32_t captureTick, std::uint8_t dA, const InputType& input, std::int32_t depth)
	{
		return relayedInputRing::writeLatest<InputType>(*this, captureTick, dA, input, depth);
	}

	template <typename InputType>
	relayedInputRing::StageArrivalOutcome stageArrival(std::uint32_t captureTick, std::uint8_t dA, const InputType& input)
	{
		return relayedInputRing::stageArrival<InputType>(*this, captureTick, dA, input);
	}

	template <typename InputType>
	bool findEntry(std::uint32_t captureTick, std::uint8_t& outDA, InputType& outInput) const
	{
		return relayedInputRing::findEntry<InputType>(*this, captureTick, outDA, outInput);
	}

	template <typename InputType, typename Callback>
	void forEachEntry(Callback&& callback) const
	{
		relayedInputRing::forEachEntry<InputType>(*this, std::forward<Callback>(callback));
	}

	std::uint8_t num() const { return relayedInputRing::entryCount(*this); }

	std::uint8_t getWireFormatVersionOnWire() const { return relayedInputRing::getWireFormatVersion(*this); }
};

namespace simPayloadBuffers
{
	inline constexpr std::uint32_t kInputBundleMaxWireBytes =
		inputRedundancyBundle::kHeaderBytes
		+ static_cast<std::uint32_t>(inputRedundancyBundle::kMaxSlots)
			* (static_cast<std::uint32_t>(sizeof(std::uint32_t)) + relayedInputRing::kMaxInputBytes);
} // namespace simPayloadBuffers

class InputBundleBuffer : public simPayloadBuffers::detail::GrowableBytes<simPayloadBuffers::kInputBundleMaxWireBytes>
{
public:
	static constexpr std::uint8_t  kWireFormatVersion = inputRedundancyBundle::kWireFormatVersion;
	static constexpr std::uint8_t  kMaxSlots          = inputRedundancyBundle::kMaxSlots;
	static constexpr std::uint32_t kMaxInputBytes     = relayedInputRing::kMaxInputBytes;
	static constexpr std::uint32_t kMaxWireBytes      = simPayloadBuffers::kInputBundleMaxWireBytes;

	static_assert(kMaxWireBytes == 1058u, "InputBundleBuffer's capacity is 2 + kMaxSlots x (4 + 128) B");

	template <typename InputType>
	void appendSlot(std::uint32_t captureTick, const InputType& input)
	{
		static_assert(inputRedundancyBundle::detail::slotInputSize<InputType>() <= kMaxInputBytes,
			"a bundle slot's input exceeds kMaxInputBytes (128 B, the relay ring's per-entry bound), so a full bundle "
			"would not fit InputBundleBuffer");
		inputRedundancyBundle::appendSlot<InputType>(*this, captureTick, input);
	}

	template <typename InputType>
	[[nodiscard]] bool tryAppendSlot(std::uint32_t captureTick, const InputType& input)
	{
		static_assert(inputRedundancyBundle::detail::slotInputSize<InputType>() <= kMaxInputBytes,
			"a bundle slot's input exceeds kMaxInputBytes (128 B, the relay ring's per-entry bound), so a full bundle "
			"would not fit InputBundleBuffer");
		return inputRedundancyBundle::tryAppendSlot<InputType>(*this, captureTick, input);
	}

	template <typename InputType, typename Callback>
	void forEachSlot(Callback&& callback) const
	{
		inputRedundancyBundle::forEachSlot<InputType>(*this, std::forward<Callback>(callback));
	}

	std::uint8_t getWireFormatVersion() const { return inputRedundancyBundle::getWireFormatVersion(*this); }
};
