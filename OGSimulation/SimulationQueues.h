#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include <array>
#include <atomic>
#include <optional>
#include <cstddef>

#pragma optimize("", off)

// Generic circular move queue — physics thread writes on RPC arrival (game thread),
// integration reads on physics thread. Mirrors the concrete RemoteMoveQueue pattern
// but is templated on InputType so SimulationNetSync can instantiate one per type.
template <typename InputType>
class RemoteMoveQueue
{
public:
    static constexpr size_t Capacity = 30;

    RemoteMoveQueue() : m_head(0), m_tail(0), m_count(0) {}

    void queueMove(InputType&& input, uint32 tick)
    {
        m_inputBuffer[m_tail].emplace(std::move(input));
        m_tickBuffer[m_tail] = tick;
        if (m_count == Capacity)
            m_head = (m_head + 1) % Capacity;
        else
            ++m_count;
        m_tail = (m_tail + 1) % Capacity;
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
