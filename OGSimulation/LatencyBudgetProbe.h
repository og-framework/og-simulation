#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/LatencyBudgetProbe-rationale.md · docs/LatencyBudgetProbe-guards.md

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "OGSimulation/OGAssert.h"

namespace latencyBudget
{

enum class Stream : uint8_t
{
	Input,
	State,
	Count
};

inline constexpr std::size_t kStreamCount = static_cast<std::size_t>(Stream::Count);

enum class Point : uint8_t
{
	InputSampled,
	Captured,
	Handed,
	LeftProcess,
	ServerReceived,
	Consumed,
	ClientReceived,
	StepEnd,
	Rendered,
	Count
};

inline constexpr std::size_t kPointCount = static_cast<std::size_t>(Point::Count);

enum class Hop : uint8_t
{
	H1,
	H2,
	H3,
	H3c,
	H4,
	H4r,
	H5,
	H6,
	H6c,
	H7,
	Wire,
	Count
};

inline constexpr std::size_t kHopCount = static_cast<std::size_t>(Hop::Count);

constexpr const char* hopName(Hop hop)
{
	switch (hop)
	{
	case Hop::H1:    return "H1";
	case Hop::H2:    return "H2";
	case Hop::H3:    return "H3";
	case Hop::H3c:   return "H3c";
	case Hop::H4:    return "H4";
	case Hop::H4r:   return "H4r";
	case Hop::H5:    return "H5";
	case Hop::H6:    return "H6";
	case Hop::H6c:   return "H6c";
	case Hop::H7:    return "H7";
	case Hop::Wire:  return "WIRE";
	case Hop::Count: break;
	}
	return "?";
}

struct HopRoute
{
	Hop    hop;
	Stream stream;
	Point  from;
	Point  to;
};

inline constexpr std::array<HopRoute, kHopCount - 1> kHopRoutes{ {
	{ Hop::H1,  Stream::Input, Point::InputSampled,   Point::Captured },
	{ Hop::H2,  Stream::Input, Point::Captured,       Point::Handed },
	{ Hop::H3,  Stream::Input, Point::Handed,         Point::LeftProcess },
	{ Hop::H3c, Stream::State, Point::Handed,         Point::LeftProcess },
	{ Hop::H4,  Stream::Input, Point::ServerReceived, Point::Consumed },
	{ Hop::H4r, Stream::Input, Point::ServerReceived, Point::Handed },
	{ Hop::H5,  Stream::State, Point::StepEnd,        Point::Handed },
	{ Hop::H6,  Stream::Input, Point::ClientReceived, Point::Consumed },
	{ Hop::H6c, Stream::State, Point::ClientReceived, Point::Consumed },
	{ Hop::H7,  Stream::State, Point::StepEnd,        Point::Rendered },
} };

constexpr bool hopRoutesAreIndexedByHop()
{
	for (std::size_t index = 0; index < kHopRoutes.size(); ++index)
	{
		if (static_cast<std::size_t>(kHopRoutes[index].hop) != index)
			return false;
		if (kHopRoutes[index].from == kHopRoutes[index].to)
			return false;
	}
	return static_cast<std::size_t>(Hop::Wire) == kHopRoutes.size();
}

static_assert(hopRoutesAreIndexedByHop(),
	"kHopRoutes must list every routed hop exactly once, in Hop order, with from != to, and Hop::Wire "
	"(the only hop fed by addSample rather than by two stamps) must be the last enumerator: the probe "
	"indexes its per-hop state by Hop and walks kHopRoutes by index.");

using HopMask = uint32_t;

constexpr HopMask hopBit(Hop hop)
{
	return HopMask{ 1u } << static_cast<unsigned>(hop);
}

static_assert(kHopCount <= 32u, "HopMask is 32 bits wide");

inline constexpr HopMask kClientHops =
	hopBit(Hop::H1) | hopBit(Hop::H2) | hopBit(Hop::H3) | hopBit(Hop::H6) | hopBit(Hop::H6c)
	| hopBit(Hop::H7) | hopBit(Hop::Wire);

inline constexpr HopMask kServerHops =
	hopBit(Hop::H3) | hopBit(Hop::H3c) | hopBit(Hop::H4) | hopBit(Hop::H4r) | hopBit(Hop::H5)
	| hopBit(Hop::H7) | hopBit(Hop::Wire);

enum class EventKind : uint8_t
{
	Stamp,
	StampPending,
	StampNewestPending,
	Sample
};

struct Event
{
	double    seconds = 0.0;
	uint32_t  lane    = 0u;
	uint32_t  tick    = 0u;
	EventKind kind    = EventKind::Stamp;
	Stream    stream  = Stream::Input;
	Point     from    = Point::InputSampled;
	Point     point   = Point::InputSampled;
	Hop       hop     = Hop::Wire;

	static Event stamp(Stream stream, uint32_t lane, uint32_t tick, Point point, double seconds)
	{
		Event event;
		event.kind    = EventKind::Stamp;
		event.stream  = stream;
		event.lane    = lane;
		event.tick    = tick;
		event.point   = point;
		event.seconds = seconds;
		return event;
	}

	static Event stampPending(Stream stream, Point from, Point to, double seconds)
	{
		Event event;
		event.kind    = EventKind::StampPending;
		event.stream  = stream;
		event.from    = from;
		event.point   = to;
		event.seconds = seconds;
		return event;
	}

	static Event stampNewestPending(Stream stream, Point from, Point to, double seconds)
	{
		Event event = stampPending(stream, from, to, seconds);
		event.kind  = EventKind::StampNewestPending;
		return event;
	}

	static Event sample(Hop hop, double durationSeconds)
	{
		Event event;
		event.kind    = EventKind::Sample;
		event.hop     = hop;
		event.seconds = durationSeconds;
		return event;
	}
};

static_assert(std::is_trivially_copyable_v<Event>,
	"latencyBudget::Event crosses threads by value through SpscMailbox; it must own no memory");

template <typename T, std::size_t Capacity>
class SpscMailbox
{
	static_assert(Capacity >= 2u && (Capacity & (Capacity - 1u)) == 0u,
		"SpscMailbox capacity must be a power of two");
	static_assert(std::is_trivially_copyable_v<T>,
		"SpscMailbox copies items by value across threads; T must own no memory");

public:
	static constexpr std::size_t capacity() { return Capacity; }

	bool tryPush(const T& item)
	{
		const uint64_t tail = m_tail.load(std::memory_order_relaxed);
		const uint64_t head = m_head.load(std::memory_order_acquire);
		if (tail - head >= Capacity)
		{
			m_dropped.fetch_add(1u, std::memory_order_relaxed);
			return false;
		}
		m_items[static_cast<std::size_t>(tail & (Capacity - 1u))] = item;
		// ⛔G-01  docs/LatencyBudgetProbe-guards.md
		m_tail.store(tail + 1u, std::memory_order_release);
		return true;
	}

	template <typename Fn>
	std::size_t drain(Fn&& fn)
	{
		uint64_t head = m_head.load(std::memory_order_relaxed);
		const uint64_t tail = m_tail.load(std::memory_order_acquire);
		std::size_t drained = 0u;
		while (head != tail)
		{
			fn(m_items[static_cast<std::size_t>(head & (Capacity - 1u))]);
			++head;
			++drained;
		}
		m_head.store(head, std::memory_order_release);
		return drained;
	}

	uint64_t takeDroppedCount()
	{
		return m_dropped.exchange(0u, std::memory_order_relaxed);
	}

private:
	std::array<T, Capacity> m_items{};
	std::atomic<uint64_t>   m_head{ 0u };
	std::atomic<uint64_t>   m_tail{ 0u };
	std::atomic<uint64_t>   m_dropped{ 0u };
};

struct HopSummary
{
	Hop      hop        = Hop::Wire;
	uint32_t n          = 0u;
	double   p50        = 0.0;
	double   p95        = 0.0;
	double   p99        = 0.0;
	double   max        = 0.0;
	uint32_t noStart    = 0u;
	uint32_t noEnd      = 0u;
	uint32_t outOfOrder = 0u;
	uint32_t overflow   = 0u;
};

struct WindowReport
{
	double                             windowStartSeconds = 0.0;
	double                             windowEndSeconds   = 0.0;
	std::array<HopSummary, kHopCount>  hops{};
	std::size_t                        hopCount           = 0u;
	uint32_t                           staleStamps        = 0u;
	uint32_t                           laneOverflow       = 0u;
	uint32_t                           duplicateStamps    = 0u;
};

struct ProbeConfig
{
	double   windowSeconds    = 10.0;
	uint32_t ticksPerLane     = 64u;
	uint32_t maxLanes         = 8u;
	uint32_t maxSamplesPerHop = 4096u;
};

constexpr uint32_t nearestRankIndex(uint32_t count, uint32_t perMille)
{
	const uint64_t rank = (static_cast<uint64_t>(perMille) * count + 999u) / 1000u;
	return rank == 0u ? 0u : static_cast<uint32_t>(rank - 1u);
}

class LatencyBudgetProbe
{
public:
	explicit LatencyBudgetProbe(HopMask enabledHops, const ProbeConfig& config = ProbeConfig{})
		: m_config(config)
		, m_enabledHops(enabledHops)
		, m_lanes(config.maxLanes)
		, m_slots(static_cast<std::size_t>(config.maxLanes) * kStreamCount * config.ticksPerLane)
	{
		OG_CHECK(std::isfinite(config.windowSeconds) && config.windowSeconds > 0.0,
			"LatencyBudgetProbe: windowSeconds must be finite and positive");
		OG_CHECK(config.ticksPerLane >= 2u, "LatencyBudgetProbe: ticksPerLane must be at least 2");
		OG_CHECK(config.maxLanes >= 1u, "LatencyBudgetProbe: maxLanes must be at least 1");
		OG_CHECK(config.maxSamplesPerHop >= 1u, "LatencyBudgetProbe: maxSamplesPerHop must be at least 1");
		for (HopState& state : m_hops)
			state.samples.reserve(config.maxSamplesPerHop);
	}

	HopMask enabledHops() const { return m_enabledHops; }
	bool isEnabled(Hop hop) const { return (m_enabledHops & hopBit(hop)) != 0u; }

	void apply(const Event& event)
	{
		switch (event.kind)
		{
		case EventKind::Stamp:
			stamp(event.stream, event.lane, event.tick, event.point, event.seconds);
			break;
		case EventKind::StampPending:
			stampPending(event.stream, event.from, event.point, event.seconds);
			break;
		case EventKind::StampNewestPending:
			stampNewestPending(event.stream, event.from, event.point, event.seconds);
			break;
		case EventKind::Sample:
			addSample(event.hop, event.seconds);
			break;
		}
	}

	void stamp(Stream stream, uint32_t lane, uint32_t tick, Point point, double seconds)
	{
		LaneState* laneState = findOrAllocateLane(lane);
		if (laneState == nullptr)
			return;

		Slot& slot = slotFor(laneIndexOf(*laneState), stream, tick);
		if (slot.live && slot.tick != tick)
		{
			// ⛔G-02  docs/LatencyBudgetProbe-guards.md
			if (tick < slot.tick)
			{
				++m_staleStamps;
				return;
			}
			finalize(stream, slot);
		}
		if (!slot.live)
		{
			slot = Slot{};
			slot.live = true;
			slot.tick = tick;
		}

		const std::size_t pointIndex = static_cast<std::size_t>(point);
		// ⛔G-03  docs/LatencyBudgetProbe-guards.md
		if (slot.hasPoint(pointIndex))
		{
			++m_duplicateStamps;
			return;
		}
		slot.at[pointIndex] = seconds;
		slot.pointMask |= static_cast<uint16_t>(1u << pointIndex);
		resolveRoutes(stream, slot);
	}

	void stampPending(Stream stream, Point from, Point to, double seconds)
	{
		forEachLiveSlot(stream, [&](Slot& slot) {
			if (isPending(slot, from, to, seconds))
				setPoint(stream, slot, to, seconds);
		});
	}

	void stampNewestPending(Stream stream, Point from, Point to, double seconds)
	{
		for (std::size_t laneIndex = 0; laneIndex < m_lanes.size(); ++laneIndex)
		{
			if (!m_lanes[laneIndex].used)
				continue;

			Slot* newest = nullptr;
			for (uint32_t ring = 0; ring < m_config.ticksPerLane; ++ring)
			{
				Slot& slot = slotAt(laneIndex, stream, ring);
				if (slot.live && isPending(slot, from, to, seconds)
					&& (newest == nullptr || slot.tick > newest->tick))
				{
					newest = &slot;
				}
			}
			if (newest != nullptr)
				setPoint(stream, *newest, to, seconds);
		}
	}

	void addSample(Hop hop, double durationSeconds)
	{
		if (!isEnabled(hop))
			return;
		HopState& state = m_hops[static_cast<std::size_t>(hop)];
		if (!std::isfinite(durationSeconds) || durationSeconds < 0.0)
		{
			++state.outOfOrder;
			return;
		}
		if (state.samples.size() >= m_config.maxSamplesPerHop)
		{
			++state.overflow;
			return;
		}
		state.samples.push_back(durationSeconds);
	}

	void forgetLane(uint32_t lane)
	{
		for (std::size_t laneIndex = 0; laneIndex < m_lanes.size(); ++laneIndex)
		{
			if (!m_lanes[laneIndex].used || m_lanes[laneIndex].id != lane)
				continue;
			for (std::size_t streamIndex = 0; streamIndex < kStreamCount; ++streamIndex)
			{
				const Stream stream = static_cast<Stream>(streamIndex);
				for (uint32_t ring = 0; ring < m_config.ticksPerLane; ++ring)
				{
					Slot& slot = slotAt(laneIndex, stream, ring);
					if (slot.live)
						finalize(stream, slot);
				}
			}
			m_lanes[laneIndex].used = false;
			return;
		}
	}

	bool closeWindowIfDue(double nowSeconds, WindowReport& out)
	{
		if (!m_windowStarted)
		{
			m_windowStarted = true;
			m_windowStartSeconds = nowSeconds;
			return false;
		}
		if (nowSeconds - m_windowStartSeconds < m_config.windowSeconds)
			return false;

		out = WindowReport{};
		out.windowStartSeconds = m_windowStartSeconds;
		out.windowEndSeconds   = nowSeconds;
		out.staleStamps        = m_staleStamps;
		out.laneOverflow       = m_laneOverflow;
		out.duplicateStamps    = m_duplicateStamps;

		for (std::size_t hopIndex = 0; hopIndex < kHopCount; ++hopIndex)
		{
			const Hop hop = static_cast<Hop>(hopIndex);
			if (!isEnabled(hop))
				continue;
			out.hops[out.hopCount++] = summarize(hop, m_hops[hopIndex]);
			m_hops[hopIndex].resetWindow();
		}

		m_staleStamps        = 0u;
		m_laneOverflow       = 0u;
		m_duplicateStamps    = 0u;
		m_windowStartSeconds = nowSeconds;
		return true;
	}

private:
	struct Slot
	{
		uint32_t                           tick         = 0u;
		bool                               live         = false;
		uint16_t                           pointMask    = 0u;
		uint16_t                           resolvedMask = 0u;
		std::array<double, kPointCount>    at{};

		bool hasPoint(std::size_t pointIndex) const
		{
			return (pointMask & static_cast<uint16_t>(1u << pointIndex)) != 0u;
		}
	};

	static_assert(kPointCount <= 16u, "Slot::pointMask is 16 bits wide");
	static_assert(kHopRoutes.size() <= 16u, "Slot::resolvedMask is 16 bits wide");

	struct LaneState
	{
		uint32_t id   = 0u;
		bool     used = false;
	};

	struct HopState
	{
		std::vector<double> samples;
		uint32_t            noStart    = 0u;
		uint32_t            noEnd      = 0u;
		uint32_t            outOfOrder = 0u;
		uint32_t            overflow   = 0u;

		void resetWindow()
		{
			samples.clear();
			noStart    = 0u;
			noEnd      = 0u;
			outOfOrder = 0u;
			overflow   = 0u;
		}
	};

	LaneState* findOrAllocateLane(uint32_t lane)
	{
		LaneState* freeLane = nullptr;
		for (LaneState& state : m_lanes)
		{
			if (state.used && state.id == lane)
				return &state;
			if (!state.used && freeLane == nullptr)
				freeLane = &state;
		}
		if (freeLane == nullptr)
		{
			++m_laneOverflow;
			return nullptr;
		}
		freeLane->used = true;
		freeLane->id   = lane;
		const std::size_t laneIndex = laneIndexOf(*freeLane);
		for (std::size_t streamIndex = 0; streamIndex < kStreamCount; ++streamIndex)
		{
			for (uint32_t ring = 0; ring < m_config.ticksPerLane; ++ring)
				slotAt(laneIndex, static_cast<Stream>(streamIndex), ring) = Slot{};
		}
		return freeLane;
	}

	std::size_t laneIndexOf(const LaneState& state) const
	{
		return static_cast<std::size_t>(&state - m_lanes.data());
	}

	Slot& slotAt(std::size_t laneIndex, Stream stream, uint32_t ring)
	{
		const std::size_t streamBase =
			(laneIndex * kStreamCount + static_cast<std::size_t>(stream)) * m_config.ticksPerLane;
		return m_slots[streamBase + ring];
	}

	Slot& slotFor(std::size_t laneIndex, Stream stream, uint32_t tick)
	{
		return slotAt(laneIndex, stream, tick % m_config.ticksPerLane);
	}

	template <typename Fn>
	void forEachLiveSlot(Stream stream, Fn&& fn)
	{
		for (std::size_t laneIndex = 0; laneIndex < m_lanes.size(); ++laneIndex)
		{
			if (!m_lanes[laneIndex].used)
				continue;
			for (uint32_t ring = 0; ring < m_config.ticksPerLane; ++ring)
			{
				Slot& slot = slotAt(laneIndex, stream, ring);
				if (slot.live)
					fn(slot);
			}
		}
	}

	static bool isPending(const Slot& slot, Point from, Point to, double seconds)
	{
		const std::size_t fromIndex = static_cast<std::size_t>(from);
		const std::size_t toIndex   = static_cast<std::size_t>(to);
		return slot.hasPoint(fromIndex) && !slot.hasPoint(toIndex) && slot.at[fromIndex] <= seconds;
	}

	void setPoint(Stream stream, Slot& slot, Point point, double seconds)
	{
		const std::size_t pointIndex = static_cast<std::size_t>(point);
		slot.at[pointIndex] = seconds;
		slot.pointMask |= static_cast<uint16_t>(1u << pointIndex);
		resolveRoutes(stream, slot);
	}

	void resolveRoutes(Stream stream, Slot& slot)
	{
		for (std::size_t routeIndex = 0; routeIndex < kHopRoutes.size(); ++routeIndex)
		{
			const HopRoute& route = kHopRoutes[routeIndex];
			const uint16_t routeBit = static_cast<uint16_t>(1u << routeIndex);
			if (route.stream != stream || !isEnabled(route.hop) || (slot.resolvedMask & routeBit) != 0u)
				continue;
			const std::size_t fromIndex = static_cast<std::size_t>(route.from);
			const std::size_t toIndex   = static_cast<std::size_t>(route.to);
			if (!slot.hasPoint(fromIndex) || !slot.hasPoint(toIndex))
				continue;
			slot.resolvedMask |= routeBit;
			addSample(route.hop, slot.at[toIndex] - slot.at[fromIndex]);
		}
	}

	void finalize(Stream stream, Slot& slot)
	{
		for (std::size_t routeIndex = 0; routeIndex < kHopRoutes.size(); ++routeIndex)
		{
			const HopRoute& route = kHopRoutes[routeIndex];
			const uint16_t routeBit = static_cast<uint16_t>(1u << routeIndex);
			if (route.stream != stream || !isEnabled(route.hop) || (slot.resolvedMask & routeBit) != 0u)
				continue;
			const bool hasFrom = slot.hasPoint(static_cast<std::size_t>(route.from));
			const bool hasTo   = slot.hasPoint(static_cast<std::size_t>(route.to));
			HopState& state = m_hops[static_cast<std::size_t>(route.hop)];
			if (hasTo && !hasFrom)
				++state.noStart;
			else if (hasFrom && !hasTo)
				++state.noEnd;
		}
		slot = Slot{};
	}

	static HopSummary summarize(Hop hop, HopState& state)
	{
		HopSummary summary;
		summary.hop        = hop;
		summary.n          = static_cast<uint32_t>(state.samples.size());
		summary.noStart    = state.noStart;
		summary.noEnd      = state.noEnd;
		summary.outOfOrder = state.outOfOrder;
		summary.overflow   = state.overflow;
		if (summary.n == 0u)
			return summary;

		std::sort(state.samples.begin(), state.samples.end());
		summary.p50 = state.samples[nearestRankIndex(summary.n, 500u)];
		summary.p95 = state.samples[nearestRankIndex(summary.n, 950u)];
		summary.p99 = state.samples[nearestRankIndex(summary.n, 990u)];
		summary.max = state.samples.back();
		return summary;
	}

	ProbeConfig                      m_config;
	HopMask                          m_enabledHops;
	std::vector<LaneState>           m_lanes;
	std::vector<Slot>                m_slots;
	std::array<HopState, kHopCount>  m_hops{};
	bool                             m_windowStarted      = false;
	double                           m_windowStartSeconds = 0.0;
	uint32_t                         m_staleStamps        = 0u;
	uint32_t                         m_laneOverflow       = 0u;
	uint32_t                         m_duplicateStamps    = 0u;
};

struct ScalarWindowStats
{
	uint32_t samples = 0u;
	double   min     = 0.0;
	double   max     = 0.0;
	double   sum     = 0.0;

	void add(double value)
	{
		if (!std::isfinite(value))
			return;
		if (samples == 0u)
		{
			min = value;
			max = value;
		}
		else
		{
			min = std::min(min, value);
			max = std::max(max, value);
		}
		sum += value;
		++samples;
	}

	double mean() const { return samples == 0u ? 0.0 : sum / static_cast<double>(samples); }

	void reset() { *this = ScalarWindowStats{}; }
};

} // namespace latencyBudget
