#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimEnvelope-rationale.md · docs/SimEnvelope-guards.md

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "OGSimulation/BodySlotOccupancy.h"
#include "OGSimulation/CorrectionStateBufferCodec.h"
#include "OGSimulation/InputRedundancyBundleCodec.h"
#include "OGSimulation/Network/SimPayloadBuffers.h"
#include "OGSimulation/Network/SimTransport.h"
#include "OGSimulation/RelayedInputRingCodec.h"
#include "OGSimulation/SimulationComposite.h"

inline constexpr uint8_t kSimEnvelopeVersion = 1u;

enum class SimMessageKind : uint8_t
{
	InputBundleSet = 1,
	RelayRing,
	CorrectionState,
	ClockPublish,
	ClockPing,
	ClockPong,
	Roster,
	RelayFloor,
	ConnectionTier,
	ArrivalMarginSignal
};

enum class SimDirection : uint8_t { ClientToServer, ServerToClient };

enum class DecodeRefusal : uint8_t { None, Version, Kind, WrongLane, Truncated, Overlong, SimIdZero, CountZero, CountOverMax };

struct DecodedSimMessage
{
	DecodeRefusal refusal = DecodeRefusal::None;
	SimMessageKind kind = SimMessageKind{};
	uint8_t simId = 0u;
	uint8_t count = 0u;
	uint8_t corrVersion = 0u;
	uint8_t value = 0u;
	uint32_t tick = 0u;
	std::span<const std::byte> body;
};

namespace simEnvelope
{
	inline constexpr size_t kVersionOffset = 0u;
	inline constexpr size_t kKindOffset = 1u;
	inline constexpr size_t kHeaderBytes = 2u;
	inline constexpr size_t kInputBundleSetHeaderBytes = kHeaderBytes + 1u;

	inline constexpr uint8_t kFirstKind = static_cast<uint8_t>(SimMessageKind::InputBundleSet);
	inline constexpr uint8_t kLastKind = static_cast<uint8_t>(SimMessageKind::ArrivalMarginSignal);

	inline constexpr uint8_t kMaxInputBundlesPerSet = 8u;

	static_assert(static_cast<uint8_t>(SimMessageKind::InputBundleSet) == 1u
			&& static_cast<uint8_t>(SimMessageKind::RelayRing) == 2u
			&& static_cast<uint8_t>(SimMessageKind::CorrectionState) == 3u
			&& static_cast<uint8_t>(SimMessageKind::ClockPublish) == 4u
			&& static_cast<uint8_t>(SimMessageKind::ClockPing) == 5u
			&& static_cast<uint8_t>(SimMessageKind::ClockPong) == 6u
			&& static_cast<uint8_t>(SimMessageKind::Roster) == 7u
			&& static_cast<uint8_t>(SimMessageKind::RelayFloor) == 8u
			&& static_cast<uint8_t>(SimMessageKind::ConnectionTier) == 9u
			&& static_cast<uint8_t>(SimMessageKind::ArrivalMarginSignal) == 10u,
		"SimMessageKind: the values are the envelope's kind byte on the wire (1 to 10, kind 0 never sent); a reorder or an "
		"insertion re-labels every message a peer of another build sends. A new kind is appended. Was SimEnvelope-rationale.md §3");
	static_assert(kMaxInputBundlesPerSet == kMaxSimulatableSlots,
		"kMaxInputBundlesPerSet: one connection's set carries at most one bundle per character the world can hold. "
		"Was SimEnvelope-rationale.md §5");
	static_assert(std::endian::native == std::endian::little,
		"simEnvelope: the bundle, ring and correction codecs write their u32 fields in native byte order, and the envelope "
		"reads and writes u32 fields as little-endian; the two agree only on a little-endian target. Was SimEnvelope-rationale.md §4");

	constexpr bool isLegal(SimMessageKind kind, SimLane lane, SimDirection direction)
	{
		switch (kind)
		{
		case SimMessageKind::InputBundleSet:
			return lane == SimLane::SimInput && direction == SimDirection::ClientToServer;
		case SimMessageKind::RelayRing:
			return lane == SimLane::SimRelay && direction == SimDirection::ServerToClient;
		case SimMessageKind::CorrectionState:
			return lane == SimLane::SimState && direction == SimDirection::ServerToClient;
		case SimMessageKind::ClockPublish:
			return lane == SimLane::SimClock && direction == SimDirection::ServerToClient;
		case SimMessageKind::ClockPing:
			return lane == SimLane::SimClock && direction == SimDirection::ClientToServer;
		case SimMessageKind::ClockPong:
			return lane == SimLane::SimClock && direction == SimDirection::ServerToClient;
		case SimMessageKind::Roster:
		case SimMessageKind::RelayFloor:
		case SimMessageKind::ConnectionTier:
		case SimMessageKind::ArrivalMarginSignal:
			return lane == SimLane::Control && direction == SimDirection::ServerToClient;
		}
		return false;
	}

	constexpr bool hasBodyLayout(SimMessageKind kind)
	{
		return kind != SimMessageKind::ClockPing && kind != SimMessageKind::ClockPong
			&& kind != SimMessageKind::ArrivalMarginSignal;
	}

	namespace detail
	{
		constexpr int legalRouteCount(SimMessageKind kind)
		{
			int routes = 0;
			for (size_t laneIndex = 0u; laneIndex < kSimLaneCount; ++laneIndex)
			{
				const SimLane lane = static_cast<SimLane>(laneIndex);
				routes += isLegal(kind, lane, SimDirection::ClientToServer) ? 1 : 0;
				routes += isLegal(kind, lane, SimDirection::ServerToClient) ? 1 : 0;
			}
			return routes;
		}

		constexpr bool everyKindHasExactlyOneRoute()
		{
			for (uint8_t kindByte = kFirstKind; kindByte <= kLastKind; ++kindByte)
			{
				if (legalRouteCount(static_cast<SimMessageKind>(kindByte)) != 1)
					return false;
			}
			return legalRouteCount(SimMessageKind{}) == 0;
		}
	} // namespace detail

	static_assert(detail::everyKindHasExactlyOneRoute(),
		"isLegal: every SimMessageKind is legal on exactly one lane in exactly one direction, and kind 0 on none. Was the "
		"lane-and-direction column of SimEnvelope-rationale.md §3");

	namespace roster
	{
		inline constexpr uint8_t kWireVersion = 1u;
		inline constexpr size_t kHeaderBytes = 4u;
		inline constexpr size_t kKindOffset = 1u;
		inline constexpr size_t kSnapshotCountOffset = kHeaderBytes;
		inline constexpr uint8_t kSnapshot = 1u;
		inline constexpr uint8_t kAdd = 2u;
		inline constexpr uint8_t kRemove = 3u;
		inline constexpr size_t kEntryBytes = 3u;
		inline constexpr size_t kRemoveBodyBytes = 2u;
	} // namespace roster

	template <uint32_t InputBytes, uint32_t StateBytes>
	struct Shape
	{
		static_assert(InputBytes <= relayedInputRing::kMaxInputBytes,
			"simEnvelope::Shape: an input over relayedInputRing::kMaxInputBytes (128 B) cannot be relayed, so no ring of it "
			"decodes. Was SimEnvelope-rationale.md §6");
		static_assert(correctionStateBuffer::kHeaderBytes + StateBytes <= CorrectionStateBuffer::kBufferBytes,
			"simEnvelope::Shape: a correction of this state does not fit CorrectionStateBuffer (384 B), so a decoded correction "
			"could not be stored. Was SimEnvelope-rationale.md §6");

		static constexpr uint32_t kInputBytes = InputBytes;
		static constexpr uint32_t kStateBytes = StateBytes;
		static constexpr size_t kBundleSlotBytes = sizeof(uint32_t) + InputBytes;
		static constexpr size_t kMaxBundleBytes =
			inputRedundancyBundle::kHeaderBytes + inputRedundancyBundle::kMaxSlots * kBundleSlotBytes;
		static constexpr size_t kRingEntryBytes = sizeof(uint32_t) + sizeof(uint8_t) + InputBytes;
		static constexpr size_t kCorrectionPayloadBytes = correctionStateBuffer::kHeaderBytes + StateBytes;

		static constexpr size_t inputBundleSetMaxBytes(size_t bundles)
		{
			return kInputBundleSetHeaderBytes + bundles * (1u + kMaxBundleBytes);
		}
	};

	template <typename T>
	inline constexpr bool isShape = false;

	template <uint32_t InputBytes, uint32_t StateBytes>
	inline constexpr bool isShape<Shape<InputBytes, StateBytes>> = true;

	template <typename T>
	concept EnvelopeShape = isShape<T>;

	template <typename InputT, typename StateT>
	struct ShapeForTypes;

	template <typename InputT, typename... Ts>
	struct ShapeForTypes<InputT, SimulationComposite<Ts...>>
	{
		using type = Shape<inputRedundancyBundle::detail::slotInputSize<InputT>(), compositeSyncSize<Ts...>()>;

		static_assert(type::kRingEntryBytes == relayedInputRing::detail::entryStride<InputT>(),
			"simEnvelope::ShapeFor: the bundle codec and the ring codec disagree on this input's size, so one stride would "
			"mis-frame the other's bytes. Was SimEnvelope-rationale.md §6");
	};

	template <typename InputT, typename StateT>
	using ShapeFor = typename ShapeForTypes<InputT, StateT>::type;

	namespace detail
	{
		constexpr uint8_t u8At(std::span<const std::byte> bytes, size_t offset)
		{
			return std::to_integer<uint8_t>(bytes[offset]);
		}

		constexpr uint32_t u32LeAt(std::span<const std::byte> bytes, size_t offset)
		{
			return static_cast<uint32_t>(u8At(bytes, offset))
				| (static_cast<uint32_t>(u8At(bytes, offset + 1u)) << 8u)
				| (static_cast<uint32_t>(u8At(bytes, offset + 2u)) << 16u)
				| (static_cast<uint32_t>(u8At(bytes, offset + 3u)) << 24u);
		}

		constexpr void putU8(std::span<std::byte> out, size_t offset, uint8_t value)
		{
			out[offset] = static_cast<std::byte>(value);
		}

		constexpr void putU32Le(std::span<std::byte> out, size_t offset, uint32_t value)
		{
			putU8(out, offset, static_cast<uint8_t>(value & 0xFFu));
			putU8(out, offset + 1u, static_cast<uint8_t>((value >> 8u) & 0xFFu));
			putU8(out, offset + 2u, static_cast<uint8_t>((value >> 16u) & 0xFFu));
			putU8(out, offset + 3u, static_cast<uint8_t>((value >> 24u) & 0xFFu));
		}

		constexpr void putHeader(std::span<std::byte> out, SimMessageKind kind)
		{
			putU8(out, kVersionOffset, kSimEnvelopeVersion);
			putU8(out, kKindOffset, static_cast<uint8_t>(kind));
		}

		constexpr DecodedSimMessage refused(DecodeRefusal refusal, SimMessageKind kind = SimMessageKind{})
		{
			DecodedSimMessage message;
			message.refusal = refusal;
			message.kind = kind;
			return message;
		}

		constexpr DecodeRefusal readSimId(std::span<const std::byte> bytes, size_t offset, uint8_t& outSimId)
		{
			if (bytes.size() <= offset)
				return DecodeRefusal::Truncated;
			outSimId = u8At(bytes, offset);
			if (outSimId == 0u)
				return DecodeRefusal::SimIdZero;
			return DecodeRefusal::None;
		}

		constexpr DecodeRefusal exactLength(size_t actual, size_t expected)
		{
			if (actual < expected)
				return DecodeRefusal::Truncated;
			if (actual > expected)
				return DecodeRefusal::Overlong;
			return DecodeRefusal::None;
		}

		template <EnvelopeShape S, typename Visitor>
		constexpr DecodeRefusal walkInputBundleSet(std::span<const std::byte> setBody, uint8_t& outCount, Visitor&& visit)
		{
			if (setBody.empty())
				return DecodeRefusal::Truncated;
			const uint8_t count = u8At(setBody, 0u);
			if (count == 0u)
				return DecodeRefusal::CountZero;
			if (count > kMaxInputBundlesPerSet)
				return DecodeRefusal::CountOverMax;

			size_t offset = 1u;
			for (uint8_t index = 0u; index < count; ++index)
			{
				uint8_t simId = 0u;
				if (const DecodeRefusal simIdRefusal = readSimId(setBody, offset, simId); simIdRefusal != DecodeRefusal::None)
					return simIdRefusal;
				offset += 1u;

				if (setBody.size() - offset < inputRedundancyBundle::kHeaderBytes)
					return DecodeRefusal::Truncated;
				if (u8At(setBody, offset + inputRedundancyBundle::kVersionOffset) != inputRedundancyBundle::kWireFormatVersion)
					return DecodeRefusal::Version;
				const uint8_t slots = u8At(setBody, offset + inputRedundancyBundle::kSlotCountOffset);
				if (slots > inputRedundancyBundle::kMaxSlots)
					return DecodeRefusal::CountOverMax;
				const size_t bundleBytes = inputRedundancyBundle::kHeaderBytes + slots * S::kBundleSlotBytes;
				if (setBody.size() - offset < bundleBytes)
					return DecodeRefusal::Truncated;

				visit(simId, setBody.subspan(offset, bundleBytes));
				offset += bundleBytes;
			}
			if (offset != setBody.size())
				return DecodeRefusal::Overlong;
			outCount = count;
			return DecodeRefusal::None;
		}

		template <EnvelopeShape S>
		constexpr DecodedSimMessage decodeInputBundleSet(std::span<const std::byte> body)
		{
			uint8_t count = 0u;
			const DecodeRefusal refusal =
				walkInputBundleSet<S>(body, count, [](uint8_t, std::span<const std::byte>) {});
			if (refusal != DecodeRefusal::None)
				return refused(refusal, SimMessageKind::InputBundleSet);
			DecodedSimMessage message;
			message.kind = SimMessageKind::InputBundleSet;
			message.count = count;
			message.body = body;
			return message;
		}

		template <EnvelopeShape S>
		constexpr DecodedSimMessage decodeRelayRing(std::span<const std::byte> body)
		{
			DecodedSimMessage message;
			message.kind = SimMessageKind::RelayRing;
			if (const DecodeRefusal simIdRefusal = readSimId(body, 0u, message.simId); simIdRefusal != DecodeRefusal::None)
				return refused(simIdRefusal, message.kind);

			const std::span<const std::byte> ring = body.subspan(1u);
			if (ring.empty())
				return refused(DecodeRefusal::Truncated, message.kind);
			if (ring.size() > relayedInputRing::kMaxWireBytes)
				return refused(DecodeRefusal::Overlong, message.kind);
			// ⛔G-02  docs/SimEnvelope-guards.md
			if (u8At(ring, relayedInputRing::kVersionOffset) == relayedInputRing::kWireFormatVersion)
			{
				if (ring.size() < relayedInputRing::kHeaderBytes)
					return refused(DecodeRefusal::Truncated, message.kind);
				const uint8_t entries = u8At(ring, relayedInputRing::kEntryCountOffset);
				if (entries > relayedInputRing::kMaxDepth)
					return refused(DecodeRefusal::CountOverMax, message.kind);
				const DecodeRefusal lengthRefusal =
					exactLength(ring.size(), relayedInputRing::kHeaderBytes + entries * S::kRingEntryBytes);
				if (lengthRefusal != DecodeRefusal::None)
					return refused(lengthRefusal, message.kind);
				message.count = entries;
			}
			message.body = ring;
			return message;
		}

		template <EnvelopeShape S>
		constexpr DecodedSimMessage decodeCorrectionState(std::span<const std::byte> body)
		{
			DecodedSimMessage message;
			message.kind = SimMessageKind::CorrectionState;
			if (const DecodeRefusal simIdRefusal = readSimId(body, 0u, message.simId); simIdRefusal != DecodeRefusal::None)
				return refused(simIdRefusal, message.kind);
			if (body.size() < 2u)
				return refused(DecodeRefusal::Truncated, message.kind);
			message.corrVersion = u8At(body, 1u);

			const std::span<const std::byte> payload = body.subspan(2u);
			if (payload.size() > CorrectionStateBuffer::kBufferBytes)
				return refused(DecodeRefusal::Overlong, message.kind);
			// ⛔G-01  docs/SimEnvelope-guards.md
			if (message.corrVersion == correctionStateBuffer::kWireFormatVersion)
			{
				const DecodeRefusal lengthRefusal = exactLength(payload.size(), S::kCorrectionPayloadBytes);
				if (lengthRefusal != DecodeRefusal::None)
					return refused(lengthRefusal, message.kind);
				message.tick = u32LeAt(payload, correctionStateBuffer::kTickOffset);
			}
			message.body = payload;
			return message;
		}

		constexpr DecodedSimMessage decodeClockPublish(std::span<const std::byte> body)
		{
			const DecodeRefusal lengthRefusal = exactLength(body.size(), sizeof(uint32_t));
			if (lengthRefusal != DecodeRefusal::None)
				return refused(lengthRefusal, SimMessageKind::ClockPublish);
			DecodedSimMessage message;
			message.kind = SimMessageKind::ClockPublish;
			message.tick = u32LeAt(body, 0u);
			message.body = body;
			return message;
		}

		constexpr DecodedSimMessage decodeU8Body(SimMessageKind kind, std::span<const std::byte> body)
		{
			const DecodeRefusal lengthRefusal = exactLength(body.size(), 1u);
			if (lengthRefusal != DecodeRefusal::None)
				return refused(lengthRefusal, kind);
			DecodedSimMessage message;
			message.kind = kind;
			message.value = u8At(body, 0u);
			message.body = body;
			return message;
		}

		constexpr DecodedSimMessage decodeRoster(std::span<const std::byte> body)
		{
			if (body.empty())
				return refused(DecodeRefusal::Truncated, SimMessageKind::Roster);
			if (u8At(body, 0u) == roster::kWireVersion)
			{
				if (body.size() < roster::kHeaderBytes)
					return refused(DecodeRefusal::Truncated, SimMessageKind::Roster);
				const uint8_t rosterKind = u8At(body, roster::kKindOffset);
				size_t expected = 0u;
				if (rosterKind == roster::kSnapshot)
				{
					if (body.size() <= roster::kSnapshotCountOffset)
						return refused(DecodeRefusal::Truncated, SimMessageKind::Roster);
					expected = roster::kHeaderBytes + 1u + u8At(body, roster::kSnapshotCountOffset) * roster::kEntryBytes;
				}
				else if (rosterKind == roster::kAdd)
					expected = roster::kHeaderBytes + roster::kEntryBytes;
				else if (rosterKind == roster::kRemove)
					expected = roster::kHeaderBytes + roster::kRemoveBodyBytes;

				if (expected != 0u)
				{
					const DecodeRefusal lengthRefusal = exactLength(body.size(), expected);
					if (lengthRefusal != DecodeRefusal::None)
						return refused(lengthRefusal, SimMessageKind::Roster);
				}
			}
			DecodedSimMessage message;
			message.kind = SimMessageKind::Roster;
			message.body = body;
			return message;
		}
	} // namespace detail

	template <EnvelopeShape S>
	constexpr DecodedSimMessage decode(SimLane lane, SimDirection direction, std::span<const std::byte> bytes)
	{
		if (bytes.empty())
			return detail::refused(DecodeRefusal::Truncated);
		if (detail::u8At(bytes, kVersionOffset) != kSimEnvelopeVersion)
			return detail::refused(DecodeRefusal::Version);
		if (bytes.size() < kHeaderBytes)
			return detail::refused(DecodeRefusal::Truncated);
		const uint8_t kindByte = detail::u8At(bytes, kKindOffset);
		if (kindByte < kFirstKind || kindByte > kLastKind)
			return detail::refused(DecodeRefusal::Kind);
		const SimMessageKind kind = static_cast<SimMessageKind>(kindByte);
		if (!isLegal(kind, lane, direction))
			return detail::refused(DecodeRefusal::WrongLane, kind);
		if (!hasBodyLayout(kind))
			return detail::refused(DecodeRefusal::Kind, kind);

		const std::span<const std::byte> body = bytes.subspan(kHeaderBytes);
		switch (kind)
		{
		case SimMessageKind::InputBundleSet:
			return detail::decodeInputBundleSet<S>(body);
		case SimMessageKind::RelayRing:
			return detail::decodeRelayRing<S>(body);
		case SimMessageKind::CorrectionState:
			return detail::decodeCorrectionState<S>(body);
		case SimMessageKind::ClockPublish:
			return detail::decodeClockPublish(body);
		case SimMessageKind::Roster:
			return detail::decodeRoster(body);
		case SimMessageKind::RelayFloor:
		case SimMessageKind::ConnectionTier:
			return detail::decodeU8Body(kind, body);
		case SimMessageKind::ClockPing:
		case SimMessageKind::ClockPong:
		case SimMessageKind::ArrivalMarginSignal:
			break;
		}
		return detail::refused(DecodeRefusal::Kind, kind);
	}

	template <EnvelopeShape S, typename Fn>
	constexpr DecodeRefusal forEachInputBundle(std::span<const std::byte> setBody, Fn&& fn)
	{
		uint8_t count = 0u;
		const DecodeRefusal refusal =
			detail::walkInputBundleSet<S>(setBody, count, [](uint8_t, std::span<const std::byte>) {});
		if (refusal != DecodeRefusal::None)
			return refusal;
		// ⛔G-03  docs/SimEnvelope-guards.md
		return detail::walkInputBundleSet<S>(setBody, count, fn);
	}

	[[nodiscard]] constexpr size_t writeInputBundleSetHeader(std::span<std::byte> out)
	{
		if (out.size() < kInputBundleSetHeaderBytes)
			return 0u;
		detail::putHeader(out, SimMessageKind::InputBundleSet);
		detail::putU8(out, kHeaderBytes, 0u);
		return kInputBundleSetHeaderBytes;
	}

	[[nodiscard]] constexpr size_t appendInputBundle(
		std::span<std::byte> out, size_t used, uint8_t simId, std::span<const std::byte> bundleBytes)
	{
		if (used < kInputBundleSetHeaderBytes || used > out.size())
			return 0u;
		const std::span<const std::byte> written(out.data(), used);
		if (detail::u8At(written, kVersionOffset) != kSimEnvelopeVersion
			|| detail::u8At(written, kKindOffset) != static_cast<uint8_t>(SimMessageKind::InputBundleSet))
			return 0u;
		const uint8_t count = detail::u8At(written, kHeaderBytes);
		if (count >= kMaxInputBundlesPerSet || simId == 0u || bundleBytes.size() < inputRedundancyBundle::kHeaderBytes)
			return 0u;
		if (out.size() - used < 1u + bundleBytes.size())
			return 0u;

		detail::putU8(out, used, simId);
		std::copy(bundleBytes.begin(), bundleBytes.end(), out.begin() + static_cast<std::ptrdiff_t>(used + 1u));
		detail::putU8(out, kHeaderBytes, static_cast<uint8_t>(count + 1u));
		return used + 1u + bundleBytes.size();
	}

	[[nodiscard]] constexpr size_t writeRelayRing(std::span<std::byte> out, uint8_t simId, std::span<const std::byte> ringBytes)
	{
		const size_t size = kHeaderBytes + 1u + ringBytes.size();
		if (simId == 0u || ringBytes.empty() || out.size() < size)
			return 0u;
		detail::putHeader(out, SimMessageKind::RelayRing);
		detail::putU8(out, kHeaderBytes, simId);
		std::copy(ringBytes.begin(), ringBytes.end(), out.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + 1u));
		return size;
	}

	[[nodiscard]] constexpr size_t writeCorrectionState(
		std::span<std::byte> out, uint8_t simId, uint8_t corrVersion, std::span<const std::byte> payloadBytes)
	{
		const size_t size = kHeaderBytes + 2u + payloadBytes.size();
		if (simId == 0u || out.size() < size)
			return 0u;
		detail::putHeader(out, SimMessageKind::CorrectionState);
		detail::putU8(out, kHeaderBytes, simId);
		detail::putU8(out, kHeaderBytes + 1u, corrVersion);
		std::copy(payloadBytes.begin(), payloadBytes.end(), out.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + 2u));
		return size;
	}

	[[nodiscard]] constexpr size_t writeClockPublish(std::span<std::byte> out, uint32_t tick)
	{
		const size_t size = kHeaderBytes + sizeof(uint32_t);
		if (out.size() < size)
			return 0u;
		detail::putHeader(out, SimMessageKind::ClockPublish);
		detail::putU32Le(out, kHeaderBytes, tick);
		return size;
	}

	[[nodiscard]] constexpr size_t writeRoster(std::span<std::byte> out, std::span<const std::byte> rosterBytes)
	{
		const size_t size = kHeaderBytes + rosterBytes.size();
		if (rosterBytes.empty() || out.size() < size)
			return 0u;
		detail::putHeader(out, SimMessageKind::Roster);
		std::copy(rosterBytes.begin(), rosterBytes.end(), out.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
		return size;
	}

	namespace detail
	{
		[[nodiscard]] constexpr size_t writeU8Message(std::span<std::byte> out, SimMessageKind kind, uint8_t value)
		{
			const size_t size = kHeaderBytes + 1u;
			if (out.size() < size)
				return 0u;
			putHeader(out, kind);
			putU8(out, kHeaderBytes, value);
			return size;
		}
	} // namespace detail

	[[nodiscard]] constexpr size_t writeRelayFloor(std::span<std::byte> out, uint8_t floor)
	{
		return detail::writeU8Message(out, SimMessageKind::RelayFloor, floor);
	}

	[[nodiscard]] constexpr size_t writeConnectionTier(std::span<std::byte> out, uint8_t tier)
	{
		return detail::writeU8Message(out, SimMessageKind::ConnectionTier, tier);
	}
} // namespace simEnvelope
