#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimInbox-rationale.md · docs/SimInbox-guards.md

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>

#include "OGSimulation/Network/SimTransport.h"
#include "OGSimulation/OGAssert.h"

inline constexpr size_t kMaxSimMessageBytes = 512u;
inline constexpr size_t kReservedEventSlots = 64u;
inline constexpr uint16_t kNoInboxTag = 0xFFFFu;
inline constexpr size_t kMaxRingSlots = size_t{1} << 29;

static_assert(kMaxSimMessageBytes <= std::numeric_limits<uint16_t>::max(),
	"kMaxSimMessageBytes: a record's size field is a uint16_t, so the payload limit must fit it");
static_assert(3u * kMaxRingSlots < (size_t{1} << 31),
	"kMaxRingSlots: records are ordered by a 32-bit sequence compared by its signed difference, so fewer than 2^31 may wait "
	"across the three rings (shared, Control, events). Was 2^30 per ring with two rings");

enum class InboundKind : uint8_t { Message, Connected, Disconnected };
enum class InboundCause : uint8_t { Unstated, ControlQuota };

struct InboundSimRecord
{
	InboundKind kind = InboundKind::Message;
	SimLane lane = SimLane::Control;
	uint16_t size = 0u;
	SimConnectionId conn = kInvalidSimConnection;
	double recvSeconds = 0.0;
	uint32_t sequence = 0u;
	uint16_t tag = kNoInboxTag;
	InboundCause cause = InboundCause::Unstated;
	std::array<std::byte, kMaxSimMessageBytes> bytes{};

	std::span<const std::byte> payload() const { return std::span<const std::byte>(bytes.data(), size); }
};

static_assert(sizeof(InboundSimRecord) <= kMaxSimMessageBytes + 24u,
	"InboundSimRecord: at most 536 B (512 B of payload and a 24 B header), the size the inbox's memory budget was set from");

struct SimInboxConfig
{
	size_t controlSlots = 0u;
	size_t tags = 0u;
	uint32_t tagQuota = 0u;
	uint32_t tagControlQuota = 0u;
	size_t eventSlots = 0u;
};

enum class SimInboxPush : uint8_t { Pushed, TooLarge, Dropped, QuotaRefused, ControlQuotaRefused, EventRefused };

struct SimInboxStats
{
	std::array<uint64_t, kSimLaneCount> pushed{};
	std::array<uint64_t, kSimLaneCount> dropped{};
	std::array<uint64_t, kSimLaneCount> quotaRefused{};
	uint64_t tooLarge = 0u;
	uint64_t events = 0u;
	uint64_t maxOccupancy = 0u;
	uint64_t controlHighWater = 0u;
	uint64_t eventsRefused = 0u;
	uint64_t eventHighWater = 0u;
};

template <size_t N>
class SimInbox
{
	static_assert(std::has_single_bit(N), "SimInbox: capacity N must be a power of two");
	static_assert(N > kReservedEventSlots, "SimInbox: capacity N must exceed the 64 slots reserved for connection events");
	static_assert(std::atomic<size_t>::is_always_lock_free, "SimInbox: needs lock-free size_t atomics");
	static_assert(std::atomic<uint64_t>::is_always_lock_free, "SimInbox: needs lock-free uint64_t atomics");
	static_assert(std::atomic<uint32_t>::is_always_lock_free, "SimInbox: needs lock-free uint32_t atomics for the sequence and the tags");
	static_assert(std::atomic<bool>::is_always_lock_free, "SimInbox: needs a lock-free bool atomic for the consumer guard");
	static_assert(N <= kMaxRingSlots,
		"SimInbox: records are ordered by a 32-bit sequence compared by its signed difference, so fewer than 2^31 may wait: "
		"2^29 in each of the three rings");

public:
	static constexpr size_t kCapacity = N;
	static constexpr size_t kCacheLineBytes = 64;

	explicit SimInbox(SimInboxConfig config = {})
		: records_(std::make_unique<InboundSimRecord[]>(N))
		, control_(config.controlSlots != 0u ? std::make_unique<InboundSimRecord[]>(config.controlSlots) : nullptr)
		, controlCapacity_(config.controlSlots)
		, eventRing_(config.eventSlots != 0u ? std::make_unique<InboundSimRecord[]>(config.eventSlots) : nullptr)
		, eventCapacity_(config.eventSlots)
		, tags_(config.tags != 0u ? std::make_unique<TagState[]>(config.tags) : nullptr)
		, tagCount_(config.tags)
		, tagQuota_(config.tagQuota)
		, tagControlQuota_(config.tagControlQuota)
	{
		OG_CHECK(config.tags < kNoInboxTag, "SimInbox: a tag is a uint16_t below kNoInboxTag, so an inbox has at most 65,535 tags");
		OG_CHECK(config.controlSlots <= kMaxRingSlots && config.eventSlots <= kMaxRingSlots,
			"SimInbox: records are ordered by a 32-bit sequence compared by its signed difference, so fewer than 2^31 may wait: "
			"2^29 in each of the three rings");
		OG_CHECK(config.tags == 0u || config.controlSlots == 0u
				|| (config.tagControlQuota != 0u && config.tags * config.tagControlQuota <= config.controlSlots),
			"SimInbox: a tagged inbox with a Control ring gives each tag a Control quota of at least one record, and the tags' "
			"quotas together fit the ring, so no tagged Control push can find the ring full");
		OG_CHECK(config.eventSlots != 1u,
			"SimInbox: an event ring holds a paired Connected and the slot it keeps for its Disconnected, so it has at least two "
			"slots, or none");
	}

	SimInbox(const SimInbox&) = delete;
	SimInbox& operator=(const SimInbox&) = delete;
	SimInbox(SimInbox&&) = delete;
	SimInbox& operator=(SimInbox&&) = delete;

	bool pushMessage(SimConnectionId conn, SimLane lane, double recvSeconds, std::span<const std::byte> bytes, uint16_t tag = kNoInboxTag)
	{
		return push(conn, lane, recvSeconds, bytes, tag) == SimInboxPush::Pushed;
	}

	SimInboxPush push(SimConnectionId conn, SimLane lane, double recvSeconds, std::span<const std::byte> bytes, uint16_t tag = kNoInboxTag)
	{
		const size_t laneIndex = indexOf(lane);
		if (bytes.size() > kMaxSimMessageBytes)
		{
			tooLarge_.fetch_add(1u, std::memory_order_relaxed);
			return SimInboxPush::TooLarge;
		}
		TagState* const owner = tagState(tag);
		if (lane == SimLane::Control && controlCapacity_ != 0u)
		{
			return pushControl(conn, recvSeconds, bytes, owner != nullptr ? tag : kNoInboxTag);
		}
		if (owner != nullptr && lane != SimLane::Control && tagQuota_ != 0u && observe(owner->held) >= tagQuota_)
		{
			refuse(*owner, laneIndex);
			return SimInboxPush::QuotaRefused;
		}
		const size_t write = writeIndex_.load(std::memory_order_relaxed);
		const size_t freeSlots = N - (write - observe(readIndex_));
		if (freeSlots <= kReservedEventSlots)
		{
			dropped_[laneIndex].fetch_add(1u, std::memory_order_relaxed);
			return SimInboxPush::Dropped;
		}
		if (owner != nullptr)
		{
			owner->held.fetch_add(1u, std::memory_order_relaxed);
		}
		fill(records_[write & (N - 1)], InboundKind::Message, lane, conn, recvSeconds, bytes, owner != nullptr ? tag : kNoInboxTag);
		commit(writeIndex_, write + 1);
		pushed_[laneIndex].fetch_add(1u, std::memory_order_relaxed);
		return SimInboxPush::Pushed;
	}

	SimInboxPush pushEvent(InboundKind kind, SimConnectionId conn, double recvSeconds, InboundCause cause = InboundCause::Unstated)
	{
		OG_CHECK(kind != InboundKind::Message, "SimInbox::pushEvent: an event is Connected or Disconnected; a message goes through pushMessage");
		if (eventCapacity_ != 0u)
		{
			return pushRingEvent(kind, conn, recvSeconds, cause, 1u);
		}
		const size_t write = writeIndex_.load(std::memory_order_relaxed);
		const size_t freeSlots = N - (write - observe(readIndex_));
		OG_CHECK(freeSlots > 0u,
			"SimInbox::pushEvent: no slot left for a connection event, even in the reserve; a lost connection event leaks a connection");
		// ⛔G-05  docs/SimInbox-guards.md
		if (freeSlots == 0u)
		{
			return SimInboxPush::Dropped;
		}
		fill(records_[write & (N - 1)], kind, SimLane::Control, conn, recvSeconds, std::span<const std::byte>(), kNoInboxTag);
		records_[write & (N - 1)].cause = cause;
		commit(writeIndex_, write + 1);
		events_.fetch_add(1u, std::memory_order_relaxed);
		return SimInboxPush::Pushed;
	}

	SimInboxPush pushPairedConnected(SimConnectionId conn, double recvSeconds)
	{
		if (eventCapacity_ == 0u)
		{
			return pushEvent(InboundKind::Connected, conn, recvSeconds);
		}
		const SimInboxPush result = pushRingEvent(InboundKind::Connected, conn, recvSeconds, InboundCause::Unstated, 2u);
		if (result == SimInboxPush::Pushed)
		{
			++eventsPaired_;
		}
		return result;
	}

	SimInboxPush pushPairedDisconnected(SimConnectionId conn, double recvSeconds, InboundCause cause = InboundCause::Unstated)
	{
		if (eventCapacity_ == 0u)
		{
			return pushEvent(InboundKind::Disconnected, conn, recvSeconds, cause);
		}
		const bool paired = eventsPaired_ != 0u;
		OG_CHECK(paired,
			"SimInbox::pushPairedDisconnected: no paired Connected is open, so no event slot was kept for this Disconnected; the "
			"producer pairs a Disconnected only with a connection whose pushPairedConnected returned Pushed");
		// ⛔G-11  docs/SimInbox-guards.md
		if (paired)
		{
			--eventsPaired_;
		}
		const SimInboxPush result = pushRingEvent(InboundKind::Disconnected, conn, recvSeconds, cause, 1u);
		OG_CHECK(!paired || result == SimInboxPush::Pushed,
			"SimInbox::pushPairedDisconnected: a paired Disconnected found no slot. Every paired Connected keeps one for its "
			"Disconnected, so a refusal here means the producer paired a Disconnected its Connected never paired");
		return result;
	}

	template <class Fn>
	size_t drain(Fn&& fn)
	{
		bool idle = false;
		const bool entered = enterDrain(idle);
		OG_CHECK(entered, "SimInbox::drain: one consumer at a time; the owner may change between drains, never during one");
		// ⛔G-06  docs/SimInbox-guards.md
		if (!entered)
		{
			return 0u;
		}
		const uint32_t end = observe(pushedSequence_);
		size_t read = readIndex_.load(std::memory_order_relaxed);
		const size_t readEnd = observe(writeIndex_);
		uint64_t controlRead = controlRead_.load(std::memory_order_relaxed);
		const uint64_t controlEnd = observe(controlWrite_);
		uint64_t eventRead = eventRead_.load(std::memory_order_relaxed);
		const uint64_t eventEnd = observe(eventWrite_);
		uint64_t fromMain = 0u;
		size_t count = 0u;
		for (;;)
		{
			const InboundSimRecord* next = nullptr;
			Ring ring = Ring::None;
			consider(read != readEnd ? &records_[read & (N - 1)] : nullptr, Ring::Main, end, next, ring);
			consider(controlRead != controlEnd ? &control_[controlRead % controlCapacity_] : nullptr, Ring::Control, end, next, ring);
			consider(eventRead != eventEnd ? &eventRing_[eventRead % eventCapacity_] : nullptr, Ring::Event, end, next, ring);
			if (ring == Ring::None)
			{
				break;
			}
			const InboundSimRecord& record = *next;
			TagState* const owner = tagState(record.tag);
			fn(static_cast<const InboundSimRecord&>(record));
			if (ring == Ring::Main)
			{
				publish(readIndex_, ++read);
				++fromMain;
			}
			else if (ring == Ring::Control)
			{
				publish(controlRead_, ++controlRead);
			}
			else
			{
				publish(eventRead_, ++eventRead);
			}
			if (owner != nullptr)
			{
				giveBack(ring == Ring::Main ? owner->held : owner->controlHeld);
			}
			++count;
		}
		if (fromMain > maxOccupancy_.load(std::memory_order_relaxed))
		{
			maxOccupancy_.store(fromMain, std::memory_order_relaxed);
		}
		leaveDrain();
		return count;
	}

	SimInboxStats stats() const
	{
		SimInboxStats out;
		for (size_t lane = 0; lane < kSimLaneCount; ++lane)
		{
			out.pushed[lane] = pushed_[lane].load(std::memory_order_relaxed);
			out.dropped[lane] = dropped_[lane].load(std::memory_order_relaxed);
			out.quotaRefused[lane] = quotaRefused_[lane].load(std::memory_order_relaxed);
		}
		out.tooLarge = tooLarge_.load(std::memory_order_relaxed);
		out.events = events_.load(std::memory_order_relaxed);
		out.maxOccupancy = maxOccupancy_.load(std::memory_order_relaxed);
		out.controlHighWater = controlHighWater_.load(std::memory_order_relaxed);
		out.eventsRefused = eventsRefused_.load(std::memory_order_relaxed);
		out.eventHighWater = eventHighWater_.load(std::memory_order_relaxed);
		return out;
	}

	size_t controlCapacity() const { return controlCapacity_; }
	size_t eventCapacity() const { return eventCapacity_; }
	size_t tagCount() const { return tagCount_; }
	uint32_t tagQuota() const { return tagQuota_; }
	uint32_t tagControlQuota() const { return tagControlQuota_; }

	uint32_t undrained(uint16_t tag) const
	{
		const TagState* const owner = tagState(tag);
		return owner != nullptr ? owner->held.load(std::memory_order_relaxed) : 0u;
	}

	uint32_t controlUndrained(uint16_t tag) const
	{
		const TagState* const owner = tagState(tag);
		return owner != nullptr ? owner->controlHeld.load(std::memory_order_relaxed) : 0u;
	}

	std::array<uint64_t, kSimLaneCount> quotaRefusedFor(uint16_t tag) const
	{
		std::array<uint64_t, kSimLaneCount> out{};
		if (const TagState* const owner = tagState(tag))
		{
			for (size_t lane = 0; lane < kSimLaneCount; ++lane)
			{
				out[lane] = owner->refused[lane].load(std::memory_order_relaxed);
			}
		}
		return out;
	}

	void resetTagCounters(uint16_t tag)
	{
		if (TagState* const owner = tagState(tag))
		{
			for (std::atomic<uint64_t>& refused : owner->refused)
			{
				refused.store(0u, std::memory_order_relaxed);
			}
		}
	}

private:
	struct TagState
	{
		std::atomic<uint32_t> held{0};
		std::atomic<uint32_t> controlHeld{0};
		std::array<std::atomic<uint64_t>, kSimLaneCount> refused{};
	};

	enum class Ring : uint8_t { None, Main, Control, Event };

	static size_t indexOf(SimLane lane)
	{
		const size_t index = static_cast<size_t>(lane);
		OG_CHECK(index < kSimLaneCount, "SimInbox: a lane outside SimLane would index past the per-lane counters");
		// ⛔G-07  docs/SimInbox-guards.md
		return std::min(index, kSimLaneCount - 1u);
	}

	static bool sequenceBefore(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

	static void consider(const InboundSimRecord* candidate, Ring candidateRing, uint32_t end, const InboundSimRecord*& next, Ring& ring)
	{
		if (candidate != nullptr && sequenceBefore(candidate->sequence, end)
			&& (next == nullptr || sequenceBefore(candidate->sequence, next->sequence)))
		{
			next = candidate;
			ring = candidateRing;
		}
	}

	template <class IndexT>
	static IndexT observe(const std::atomic<IndexT>& otherSidesIndex)
	{
		// ⛔G-01  docs/SimInbox-guards.md
		return otherSidesIndex.load(std::memory_order_acquire);
	}

	template <class IndexT>
	static void publish(std::atomic<IndexT>& ownIndex, IndexT next)
	{
		// ⛔G-02  docs/SimInbox-guards.md
		ownIndex.store(next, std::memory_order_release);
	}

	static void giveBack(std::atomic<uint32_t>& held)
	{
		// ⛔G-10  docs/SimInbox-guards.md
		held.fetch_sub(1u, std::memory_order_release);
	}

	TagState* tagState(uint16_t tag) const { return tag < tagCount_ ? &tags_[tag] : nullptr; }

	void refuse(TagState& owner, size_t laneIndex)
	{
		owner.refused[laneIndex].fetch_add(1u, std::memory_order_relaxed);
		quotaRefused_[laneIndex].fetch_add(1u, std::memory_order_relaxed);
	}

	SimInboxPush pushControl(SimConnectionId conn, double recvSeconds, std::span<const std::byte> bytes, uint16_t tag)
	{
		const size_t laneIndex = indexOf(SimLane::Control);
		TagState* const owner = tagState(tag);
		if (owner != nullptr && observe(owner->controlHeld) >= tagControlQuota_)
		{
			refuse(*owner, laneIndex);
			return SimInboxPush::ControlQuotaRefused;
		}
		const uint64_t write = controlWrite_.load(std::memory_order_relaxed);
		const uint64_t occupancy = write - observe(controlRead_);
		const bool hasSlot = occupancy < controlCapacity_;
		OG_CHECK(hasSlot,
			"SimInbox::pushMessage: the Control ring is full. The tags' Control quotas together fit the ring, so only an untagged "
			"Control push (a local caller; a transport tags every message) can fill it, and a refused Control message is lost for "
			"good: the transport has already delivered and acknowledged it");
		// ⛔G-09  docs/SimInbox-guards.md
		if (!hasSlot)
		{
			dropped_[laneIndex].fetch_add(1u, std::memory_order_relaxed);
			return SimInboxPush::Dropped;
		}
		if (owner != nullptr)
		{
			owner->controlHeld.fetch_add(1u, std::memory_order_relaxed);
		}
		fill(control_[write % controlCapacity_], InboundKind::Message, SimLane::Control, conn, recvSeconds, bytes, tag);
		commit(controlWrite_, write + 1);
		pushed_[laneIndex].fetch_add(1u, std::memory_order_relaxed);
		if (occupancy + 1u > controlHighWater_.load(std::memory_order_relaxed))
		{
			controlHighWater_.store(occupancy + 1u, std::memory_order_relaxed);
		}
		return SimInboxPush::Pushed;
	}

	SimInboxPush pushRingEvent(InboundKind kind, SimConnectionId conn, double recvSeconds, InboundCause cause, uint64_t room)
	{
		const uint64_t write = eventWrite_.load(std::memory_order_relaxed);
		const uint64_t held = write - observe(eventRead_);
		if (held + eventsPaired_ + room > eventCapacity_)
		{
			eventsRefused_.fetch_add(1u, std::memory_order_relaxed);
			return SimInboxPush::EventRefused;
		}
		InboundSimRecord& record = eventRing_[write % eventCapacity_];
		fill(record, kind, SimLane::Control, conn, recvSeconds, std::span<const std::byte>(), kNoInboxTag);
		record.cause = cause;
		commit(eventWrite_, write + 1);
		events_.fetch_add(1u, std::memory_order_relaxed);
		if (held + 1u > eventHighWater_.load(std::memory_order_relaxed))
		{
			eventHighWater_.store(held + 1u, std::memory_order_relaxed);
		}
		return SimInboxPush::Pushed;
	}

	void fill(InboundSimRecord& record, InboundKind kind, SimLane lane, SimConnectionId conn, double recvSeconds,
		std::span<const std::byte> bytes, uint16_t tag)
	{
		record.kind = kind;
		record.lane = lane;
		record.size = static_cast<uint16_t>(bytes.size());
		record.conn = conn;
		record.recvSeconds = recvSeconds;
		record.sequence = nextSequence_;
		record.tag = tag;
		record.cause = InboundCause::Unstated;
		if (!bytes.empty())
		{
			std::memcpy(record.bytes.data(), bytes.data(), bytes.size());
		}
	}

	template <class IndexT>
	void commit(std::atomic<IndexT>& ringWriteIndex, IndexT next)
	{
		publish(ringWriteIndex, next);
		++nextSequence_;
		// ⛔G-08  docs/SimInbox-guards.md
		publish(pushedSequence_, nextSequence_);
	}

	bool enterDrain(bool& expected)
	{
		// ⛔G-03  docs/SimInbox-guards.md
		return draining_.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
	}

	void leaveDrain()
	{
		// ⛔G-04  docs/SimInbox-guards.md
		draining_.store(false, std::memory_order_release);
	}

	std::unique_ptr<InboundSimRecord[]> records_;
	std::unique_ptr<InboundSimRecord[]> control_;
	size_t controlCapacity_;
	std::unique_ptr<InboundSimRecord[]> eventRing_;
	size_t eventCapacity_;
	std::unique_ptr<TagState[]> tags_;
	size_t tagCount_;
	uint32_t tagQuota_;
	uint32_t tagControlQuota_;
	alignas(kCacheLineBytes) std::atomic<size_t> writeIndex_{0};
	std::atomic<uint64_t> controlWrite_{0};
	std::atomic<uint64_t> eventWrite_{0};
	uint64_t eventsPaired_ = 0u;
	uint32_t nextSequence_ = 0u;
	std::atomic<uint32_t> pushedSequence_{0};
	std::array<std::atomic<uint64_t>, kSimLaneCount> pushed_{};
	std::array<std::atomic<uint64_t>, kSimLaneCount> dropped_{};
	std::array<std::atomic<uint64_t>, kSimLaneCount> quotaRefused_{};
	std::atomic<uint64_t> tooLarge_{0};
	std::atomic<uint64_t> events_{0};
	std::atomic<uint64_t> controlHighWater_{0};
	std::atomic<uint64_t> eventsRefused_{0};
	std::atomic<uint64_t> eventHighWater_{0};
	alignas(kCacheLineBytes) std::atomic<size_t> readIndex_{0};
	std::atomic<uint64_t> controlRead_{0};
	std::atomic<uint64_t> eventRead_{0};
	std::atomic<bool> draining_{false};
	std::atomic<uint64_t> maxOccupancy_{0};
};

using DefaultSimInbox = SimInbox<2048>;
