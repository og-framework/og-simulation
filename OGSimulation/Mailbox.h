#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/Mailbox-rationale.md · docs/Mailbox-guards.md

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

template <typename T, size_t N>
class SpscRing
{
	static_assert(std::has_single_bit(N), "SpscRing: capacity N must be a power of two (1, 2, 4, ...)");
	static_assert(std::atomic<size_t>::is_always_lock_free, "SpscRing: needs lock-free size_t atomics");
	static_assert(std::atomic<uint64_t>::is_always_lock_free, "SpscRing: needs lock-free uint64_t atomics");
	static_assert(std::is_nothrow_destructible_v<T>, "SpscRing: T must be nothrow destructible");

public:
	static constexpr size_t kCapacity = N;
	static constexpr size_t kCacheLineBytes = 64;

	SpscRing() = default;

	~SpscRing()
	{
		const size_t write = writeIndex_.load(std::memory_order_relaxed);
		for (size_t read = readIndex_.load(std::memory_order_relaxed); read != write; ++read)
		{
			slotAt(read)->~T();
		}
	}

	SpscRing(const SpscRing&) = delete;
	SpscRing& operator=(const SpscRing&) = delete;
	SpscRing(SpscRing&&) = delete;
	SpscRing& operator=(SpscRing&&) = delete;

	bool tryPush(T&& value) { return emplaceIfRoom(std::move(value)); }

	bool tryPush(const T& value) { return emplaceIfRoom(value); }

	std::optional<T> tryPop()
	{
		const size_t read = readIndex_.load(std::memory_order_relaxed);
		if (read == observe(writeIndex_))
		{
			return std::nullopt;
		}
		T* slot = slotAt(read);
		std::optional<T> value(std::move(*slot));
		slot->~T();
		publish(readIndex_, read + 1);
		return value;
	}

	uint64_t drops() const { return drops_.load(std::memory_order_relaxed); }

	size_t sizeApprox() const
	{
		const size_t read = readIndex_.load(std::memory_order_acquire);
		const size_t write = writeIndex_.load(std::memory_order_acquire);
		const size_t used = write - read;
		return used < N ? used : N;
	}

private:
	struct alignas(T) SlotStorage
	{
		std::byte bytes[sizeof(T)];
	};

	template <typename U>
	bool emplaceIfRoom(U&& value)
	{
		const size_t write = writeIndex_.load(std::memory_order_relaxed);
		if (write - observe(readIndex_) == N)
		{
			drops_.fetch_add(1u, std::memory_order_relaxed);
			return false;
		}
		::new (static_cast<void*>(storage_[write & (N - 1)].bytes)) T(std::forward<U>(value));
		publish(writeIndex_, write + 1);
		return true;
	}

	static size_t observe(const std::atomic<size_t>& otherSidesIndex)
	{
		// ⛔G-01  docs/Mailbox-guards.md
		return otherSidesIndex.load(std::memory_order_acquire);
	}

	static void publish(std::atomic<size_t>& ownIndex, size_t next)
	{
		// ⛔G-02  docs/Mailbox-guards.md
		ownIndex.store(next, std::memory_order_release);
	}

	T* slotAt(size_t index) { return std::launder(reinterpret_cast<T*>(storage_[index & (N - 1)].bytes)); }

	alignas(kCacheLineBytes) std::atomic<size_t> writeIndex_{0};
	std::atomic<uint64_t> drops_{0};
	alignas(kCacheLineBytes) std::atomic<size_t> readIndex_{0};
	alignas(kCacheLineBytes) SlotStorage storage_[N];
};

template <typename T>
class TripleBuffer
{
	static_assert(std::atomic<uint32_t>::is_always_lock_free, "TripleBuffer: needs lock-free uint32_t atomics");
	static_assert(std::is_default_constructible_v<T>, "TripleBuffer: T must be default constructible");

public:
	static constexpr size_t kCacheLineBytes = 64;

	TripleBuffer() = default;

	TripleBuffer(const TripleBuffer&) = delete;
	TripleBuffer& operator=(const TripleBuffer&) = delete;
	TripleBuffer(TripleBuffer&&) = delete;
	TripleBuffer& operator=(TripleBuffer&&) = delete;

	T& write() { return slots_[back_].value; }

	void publish() { back_ = swapMiddle(back_ | kDirty) & kIndexMask; }

	const T* acquireLatest()
	{
		if (dirty())
		{
			front_ = swapMiddle(front_) & kIndexMask;
			hasFront_ = true;
		}
		return hasFront_ ? &slots_[front_].value : nullptr;
	}

	bool dirty() const { return (middle_.load(std::memory_order_relaxed) & kDirty) != 0u; }

private:
	static constexpr uint32_t kIndexMask = 0x3u;
	static constexpr uint32_t kDirty = 0x4u;

	struct alignas(kCacheLineBytes) Slot
	{
		T value{};
	};

	uint32_t swapMiddle(uint32_t handOver)
	{
		// ⛔G-03  docs/Mailbox-guards.md
		return middle_.exchange(handOver, std::memory_order_acq_rel);
	}

	Slot slots_[3];
	alignas(kCacheLineBytes) uint32_t back_ = 0u;
	alignas(kCacheLineBytes) std::atomic<uint32_t> middle_{1u};
	alignas(kCacheLineBytes) uint32_t front_ = 2u;
	bool hasFront_ = false;
};

template <typename T, size_t N>
class SnapshotChannel
{
	static_assert(N >= 2, "SnapshotChannel: N must be at least 2, so a reader holding one snapshot never stops the writer");
	static_assert(N <= 12, "SnapshotChannel: N must be at most 12: the newest-first order, its count and the held set share one 64-bit atomic");
	static_assert(std::atomic<uint64_t>::is_always_lock_free, "SnapshotChannel: needs lock-free uint64_t atomics");
	static_assert(std::is_default_constructible_v<T>, "SnapshotChannel: T must be default constructible: the N slots are built with the channel");

public:
	static constexpr size_t kCapacity = N;
	static constexpr size_t kMaxHeldKeepingNewest = N - 2;
	static constexpr size_t kCacheLineBytes = 64;

	SnapshotChannel() = default;

	SnapshotChannel(const SnapshotChannel&) = delete;
	SnapshotChannel& operator=(const SnapshotChannel&) = delete;
	SnapshotChannel(SnapshotChannel&&) = delete;
	SnapshotChannel& operator=(SnapshotChannel&&) = delete;

	T* beginWrite()
	{
		if (writing_ != kNoSlot)
		{
			return &slots_[writing_].value;
		}
		State current = state_.load(std::memory_order_relaxed);
		for (;;)
		{
			const size_t count = countOf(current);
			if (count < N)
			{
				writing_ = firstUnlistedSlot(current);
				return &slots_[writing_].value;
			}
			const size_t victimPosition = oldestUnheldPosition(current);
			const bool canRecycle = victimPosition < count;
			const size_t victim = canRecycle ? slotAt(current, victimPosition) : kNoSlot;
			if (exchangeState(current, canRecycle ? withoutPosition(current, victimPosition) : current))
			{
				if (!canRecycle)
				{
					drops_.fetch_add(1u, std::memory_order_relaxed);
					return nullptr;
				}
				writing_ = victim;
				return &slots_[writing_].value;
			}
		}
	}

	void commit()
	{
		if (writing_ == kNoSlot)
		{
			return;
		}
		State current = state_.load(std::memory_order_relaxed);
		while (!exchangeState(current, withNewest(current, writing_)))
		{
		}
		writing_ = kNoSlot;
	}

	const T* peekNewest(size_t back = 0)
	{
		if (back >= N)
		{
			return nullptr;
		}
		State current = state_.load(std::memory_order_relaxed);
		for (;;)
		{
			const size_t position = viewPosition(current, back);
			const bool found = position < countOf(current);
			const size_t slot = found ? slotAt(current, position) : kNoSlot;
			if (exchangeState(current, found ? (current | heldBit(slot)) : current))
			{
				if (!found)
				{
					return nullptr;
				}
				if (!isHeld(current, slot))
				{
					heldBack_[slot] = back;
				}
				return &slots_[slot].value;
			}
		}
	}

	void release(const T* snapshot)
	{
		const size_t slot = slotOf(snapshot);
		if (slot == kNoSlot)
		{
			return;
		}
		State current = state_.load(std::memory_order_relaxed);
		if (!isHeld(current, slot))
		{
			return;
		}
		while (!exchangeState(current, current & ~heldBit(slot)))
		{
		}
	}

	uint64_t drops() const { return drops_.load(std::memory_order_relaxed); }

private:
	using State = uint64_t;

	static constexpr size_t kNoSlot = N;
	static constexpr unsigned kCountShift = 12u;
	static constexpr unsigned kOrderShift = 16u;
	static constexpr unsigned kEntryBits = 4u;
	static constexpr State kEntryMask = 0xFu;
	static constexpr State kHeldMask = (State{1} << kCountShift) - 1u;

	struct alignas(kCacheLineBytes) Slot
	{
		T value{};
	};

	static size_t countOf(State state) { return static_cast<size_t>((state >> kCountShift) & kEntryMask); }

	static size_t slotAt(State state, size_t position)
	{
		return static_cast<size_t>((state >> (kOrderShift + kEntryBits * position)) & kEntryMask);
	}

	static State heldBit(size_t slot) { return State{1} << slot; }

	static bool isHeld(State state, size_t slot) { return (state & heldBit(slot)) != 0u; }

	static State compose(State order, size_t count, State held)
	{
		return (order << kOrderShift) | (static_cast<State>(count) << kCountShift) | (held & kHeldMask);
	}

	static State withNewest(State state, size_t slot)
	{
		return compose(((state >> kOrderShift) << kEntryBits) | slot, countOf(state) + 1u, state);
	}

	static State withoutPosition(State state, size_t position)
	{
		const State order = state >> kOrderShift;
		const State newer = order & ((State{1} << (kEntryBits * position)) - 1u);
		const State older = order >> (kEntryBits * (position + 1u));
		return compose(newer | (older << (kEntryBits * position)), countOf(state) - 1u, state);
	}

	static size_t positionOf(State state, size_t slot)
	{
		const size_t count = countOf(state);
		for (size_t position = 0; position < count; ++position)
		{
			if (slotAt(state, position) == slot)
			{
				return position;
			}
		}
		return count;
	}

	static size_t firstUnlistedSlot(State state)
	{
		for (size_t slot = 0; slot < N; ++slot)
		{
			if (positionOf(state, slot) == countOf(state))
			{
				return slot;
			}
		}
		return kNoSlot;
	}

	static size_t oldestUnheldPosition(State state)
	{
		const size_t count = countOf(state);
		for (size_t position = count; position > 0; --position)
		{
			if (!isHeld(state, slotAt(state, position - 1u)))
			{
				return position - 1u;
			}
		}
		return count;
	}

	size_t viewPosition(State state, size_t back) const
	{
		size_t anchor = kNoSlot;
		for (size_t slot = 0; slot < N; ++slot)
		{
			if (isHeld(state, slot) && (anchor == kNoSlot || heldBack_[slot] < heldBack_[anchor]))
			{
				anchor = slot;
			}
		}
		if (anchor == kNoSlot)
		{
			return back;
		}
		const size_t anchorPosition = positionOf(state, anchor) + back;
		return anchorPosition >= heldBack_[anchor] ? anchorPosition - heldBack_[anchor] : kNoSlot;
	}

	size_t slotOf(const T* snapshot) const
	{
		for (size_t slot = 0; slot < N; ++slot)
		{
			if (snapshot == &slots_[slot].value)
			{
				return slot;
			}
		}
		return kNoSlot;
	}

	bool exchangeState(State& expected, State desired)
	{
		// ⛔G-04  docs/Mailbox-guards.md
		return state_.compare_exchange_weak(expected, desired, std::memory_order_acq_rel, std::memory_order_relaxed);
	}

	alignas(kCacheLineBytes) std::atomic<State> state_{0u};
	alignas(kCacheLineBytes) size_t writing_ = kNoSlot;
	std::atomic<uint64_t> drops_{0};
	alignas(kCacheLineBytes) size_t heldBack_[N] = {};
	Slot slots_[N];
};
