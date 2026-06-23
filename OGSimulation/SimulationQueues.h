#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include <array>
#include <atomic>
#include <optional>
#include <cstddef>

#pragma optimize("", off)

// Outcome of RemoteMoveQueue::queueMove — Stage 1 (Task 10) receive-side capture-tick
// guard (proposal §3.3 step 10). Returned (rather than logged here) so the queue stays
// a pure container with no logger dependency; the caller (SimulationNetSync) emits the
// too-far-future warning.
enum class QueueMoveResult
{
    Enqueued,               // accepted and stored
    DuplicateDiscarded,     // capture_tick already pending — R-T5 first-writer-wins
    TooFarFutureDiscarded,  // capture_tick > serverAuthorityTick + rollbackWindowTicks
};

// Generic circular move queue — physics thread writes on RPC arrival (game thread),
// integration reads on physics thread. Mirrors the concrete RemoteMoveQueue pattern
// but is templated on InputType so SimulationNetSync can instantiate one per type.
template <typename InputType>
class RemoteMoveQueue
{
public:
    static constexpr size_t Capacity = 30;

    RemoteMoveQueue() : m_head(0), m_tail(0), m_count(0) {}

    // Enqueue one inbound (captureTick, input) slot with Stage 1 (Task 10) dedup.
    //
    // 1. R-T5 dedup (first-writer-wins): if a still-pending entry already carries this
    //    captureTick, silently discard the new one. This is correct ONLY under the
    //    FInputRedundancyBundle append-only/immutable-per-capture-tick invariant — the
    //    client never revises a sent tick's input, so the first arrival is authoritative
    //    and the redundant re-sends in the overlap window are duplicates to drop.
    // 2. Too-far-future guard: reject a captureTick beyond rollbackWindowTicks ahead of
    //    the server authority tick. rollbackWindowTicks is supplied by the caller from
    //    TimeConfig::rollbackWindowTicks (no hardcoded literal here — R-P1). A negative
    //    value disables the guard (unconfigured SimulationNetSync, e.g. isolated tests).
    // 3. Otherwise enqueue.
    //
    // NOTE (scope): dedup is over the live pending window only. A captureTick that was
    // already dequeued/applied is not tracked here; the bundle's bounded overlap window
    // plus the future guard keep replays out of scope of this task (proposal §3.3 step 10).
    QueueMoveResult queueMove(InputType&& input, uint32 captureTick,
                              uint32 serverAuthorityTick, int32 rollbackWindowTicks)
    {
        for (size_t i = 0; i < m_count; ++i)
        {
            const size_t idx = (m_head + i) % Capacity;
            if (m_tickBuffer[idx] == captureTick)
                return QueueMoveResult::DuplicateDiscarded;
        }

        if (rollbackWindowTicks >= 0)
        {
            const uint32 maxAcceptableTick =
                serverAuthorityTick + static_cast<uint32>(rollbackWindowTicks);
            if (captureTick > maxAcceptableTick)
                return QueueMoveResult::TooFarFutureDiscarded;
        }

        m_inputBuffer[m_tail].emplace(std::move(input));
        m_tickBuffer[m_tail] = captureTick;
        if (m_count == Capacity)
            m_head = (m_head + 1) % Capacity;
        else
            ++m_count;
        m_tail = (m_tail + 1) % Capacity;
        return QueueMoveResult::Enqueued;
    }

    // Returns a zero-initialized move if empty.
    struct Move
    {
        InputType input;
        uint32 tick = 0;
    };

    Move dequeueMove()
    {
        if (m_count == 0)
            return Move{};
        auto entry = Move{ std::move(*m_inputBuffer[m_head]), m_tickBuffer[m_head] };
        m_head = (m_head + 1) % Capacity;
        --m_count;
        return entry;
    }

    bool empty() const { return m_count == 0; }
    size_t size() const { return m_count; }

private:
    size_t m_head, m_tail, m_count;
    std::array<std::optional<InputType>, Capacity> m_inputBuffer;
    std::array<uint32, Capacity> m_tickBuffer{};
};

// SPSC queue for buffering locally-produced inputs between the physics thread
// (producer: collectInputAll) and the game thread (consumer: sendLocalInputToAuthorityAll).
// Lamport ring: head/tail are atomic, no count. Full = next(tail)==head (wastes
// one slot, hence RingSize = Capacity + 1 to preserve 60 usable entries).
template <typename InputType>
class PendingInputQueue
{
public:
    static constexpr size_t Capacity = 60;

    struct Entry
    {
        uint32 tick;
        InputType input;
    };

    void enqueue(uint32 tick, const InputType& input)
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t nextTail = (tail + 1) % RingSize;
        if (nextTail == m_head.load(std::memory_order_acquire))
            return; // drop if full — should not happen under normal operation
        m_buffer[tail] = Entry{ tick, input };
        m_tail.store(nextTail, std::memory_order_release);
    }

    std::optional<Entry> dequeue()
    {
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (head == m_tail.load(std::memory_order_acquire))
            return std::nullopt;
        Entry e = std::move(m_buffer[head]);
        m_head.store((head + 1) % RingSize, std::memory_order_release);
        return e;
    }

    // Const peek of up to `maxSlots` most-recent still-pending entries whose tick
    // is <= upToTick, invoking cb(uint32 tick, const InputType& input) oldest-first.
    // Does NOT advance head/tail — used by the Stage 1 redundancy-bundle producer
    // (buildRedundancyBundle) to re-send the last N ticks every frame so a
    // dropped unreliable datagram self-heals on the next send. Consumer-thread only
    // (the game thread that also calls dequeue / releaseAllButRecent).
    template <typename Callback>
    void forEachRecent(uint32 upToTick, size_t maxSlots, Callback&& cb) const
    {
        const size_t head  = m_head.load(std::memory_order_relaxed);
        const size_t tail  = m_tail.load(std::memory_order_acquire);
        const size_t count = (tail + RingSize - head) % RingSize;
        const size_t take  = count < maxSlots ? count : maxSlots;
        const size_t start = (tail + RingSize - take) % RingSize;
        for (size_t i = 0; i < take; ++i)
        {
            const Entry& e = m_buffer[(start + i) % RingSize];
            if (e.tick <= upToTick)
                cb(e.tick, e.input);
        }
    }

    // Consumer-side: advance head until at most `keep` entries remain, releasing the
    // older ones. The redundancy send path retains a sliding window of the most-recent
    // `keep` inputs (so consecutive bundles overlap) instead of draining fully. SPSC-safe
    // — only the consumer thread moves head.
    void releaseAllButRecent(size_t keep)
    {
        const size_t tail = m_tail.load(std::memory_order_acquire);
        size_t head  = m_head.load(std::memory_order_relaxed);
        size_t count = (tail + RingSize - head) % RingSize;
        while (count > keep)
        {
            head = (head + 1) % RingSize;
            --count;
        }
        m_head.store(head, std::memory_order_release);
    }

    bool empty() const
    {
        return m_head.load(std::memory_order_acquire)
            == m_tail.load(std::memory_order_acquire);
    }

    // Drops all pending entries. Safe only when neither producer nor consumer
    // is concurrently accessing the queue — i.e. from the clock-resync wipe
    // path, which runs on the physics thread inside advancePrediction while
    // TG_EndPhysics is still blocked and the consumer cannot touch the queue.
    void clear()
    {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr size_t RingSize = Capacity + 1;
    std::atomic<size_t> m_head{ 0 };
    std::atomic<size_t> m_tail{ 0 };
    std::array<Entry, RingSize> m_buffer{};
};

#pragma optimize("", on)
