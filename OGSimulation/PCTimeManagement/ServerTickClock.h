#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/OGExport.h"
#include <functional>
#include <type_traits>

#include "OGSimulation/OGAssert.h"
#include "OGSimulation/SimulationTimeContext.h"

#pragma optimize( "", off )

// ServerTickClock — server-side monotonic tick counter with sync-buffer serialization.
// Replaces AuthoritySimulationTimeManager (renamed for clarity).
// The serialization byte layout is identical to AuthoritySimulationTimeManager so that
// existing replicated FSmallSimulationStateSyncBuffer data is fully backward-compatible.
class ServerTickClock
{
public:
    using LoggerFn = std::function<void(const char*)>;
    // deltaSeconds is the fixed physics step length (seconds per tick).
    // It is baked into every SimulationTimeStep this clock produces so the
    // simulation's per-tick timer arithmetic gets the right dt.
    OGSIMULATION_API ServerTickClock(float deltaSeconds, LoggerFn logger = nullptr);

    OGSIMULATION_API unsigned int  getTick()           const;
    OGSIMULATION_API void         advanceTick();
    OGSIMULATION_API SimulationTimeStep getSimulationStep() const;

    // -----------------------------------------------------------------
    // Serialization — byte-compatible with AuthoritySimulationTimeManager
    // -----------------------------------------------------------------

    // Helper: strips const/ref so we can use sizeof on the underlying type.
    template<typename T>
    using PlainType = std::remove_cv_t<std::remove_reference_t<T>>;

    // Number of bytes occupied in the synced buffer: sizeof(unsigned int) = 4.
    static constexpr unsigned int SyncSize()
    {
        return sizeof(PlainType<decltype(std::declval<ServerTickClock>().getTick())>);
    }

    // Write m_tick into buffer at byteIterator; returns number of bytes written.
    template <typename SyncedBuffer>
    static unsigned int writeToSyncedBuffer(const ServerTickClock& clock, SyncedBuffer& buffer, unsigned int byteIterator)
    {
        unsigned int it = byteIterator;
        buffer.writeToBuffer(it, clock.getTick());
        it += sizeof(PlainType<decltype(clock.getTick())>);

        OG_CHECK(SyncSize() == it - byteIterator, "ServerTickClock: wrote to syncedBuffer incorrectly");

        return it - byteIterator;
    }

    // Read a tick from buffer at byteIterator and return as SimulationTimeStep(tick, false).
    template <typename SyncedBuffer>
    static SimulationTimeStep readFromSyncedBuffer(const SyncedBuffer& buffer, unsigned int byteIterator)
    {
        typedef PlainType<decltype(std::declval<ServerTickClock>().getTick())> TickType;

        unsigned int it = byteIterator;
        const TickType tick = buffer.template readFromBuffer<TickType>(it);
        it += sizeof(TickType);

        OG_CHECK(SyncSize() == it - byteIterator, "ServerTickClock: read from syncedBuffer incorrectly");

        return SimulationTimeStep(tick, false);
    }

private:
    unsigned int m_tick = 0;
    float m_deltaSeconds = 0.f;
    LoggerFn m_logger;
};

#pragma optimize( "", on )
