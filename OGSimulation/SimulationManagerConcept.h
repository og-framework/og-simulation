#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <functional>
#include <cstdint>

#include "OGSimulation/PCTimeManagement/ServerTickClock.h"

template <typename BufferT>
concept SyncedTimingBufferConcept = requires(BufferT& b, const ServerTickClock& clock)
{
    { b.write(clock) };
};

// Owner-layer concept for ASimulationManagerUImpl.
// Server side: getSyncedTimingBuffer() is writable; SimulationManager writes the server
// clock into it each tick via write(). Client side: setOnTimingInfoReceivedCallback
// registers the deserialized-tick+RTT handler; OnRep_TimingInfo fires it.
template <typename OwnerT>
concept SimulationManagerOwnerConcept =
    requires(OwnerT& owner,
             std::function<void(uint32_t authorityTick, double rtt)> timingFn)
    {
        { owner.getSyncedTimingBuffer() } -> SyncedTimingBufferConcept;
        { owner.setOnTimingInfoReceivedCallback(timingFn) };
    };
