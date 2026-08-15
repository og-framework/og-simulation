#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <functional>
#include <cstddef>
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

// ---------------------------------------------------------------------------
// NetConfig<C> — session-level compile-time netcode parameterization.
// (Stage 3 / D3.2; proposal_ogbrawler_netcode.md §2.1. GGRS "Pattern B" adapted
// from P2P to server-authoritative and from single- to variadic-simulatable.)
//
// A NetConfig is the ONE type a session is monomorphised over. It supplies the
// opaque wire-identity handle type (`Address`) and the session tick rate. Every
// engine-agnostic per-connection structure in this core — ConnectionTierTable,
// ServerInputDelayQueue, and (Stage 4) ServerSubstitutionTracker — is templated
// on `Address`, so the sim core never names a UE / Godot / engine type. Engine
// adapters supply the concrete handle: `OGSimulationUnreal` provides
// `FUEConnectionHandle` (wrapping `TWeakObjectPtr<UNetConnection>`), and the
// engine-free Catch2 harness provides `FStandaloneTestHandle`.
//
// WHY `NetConfig` DOES NOT CARRY `Input` / `State` TYPES (proposal §2.1
// Correction 3): GGRS models a session over a single simulatable, so its config
// can name one Input and one State. OGSim does not — a session is variadic over
// a simulatable pack (`SimulationObjectStorage<SimulatableTs...>`,
// `SimulationReconciliation<SimulatableTs...>`, `SimulationNetSync<SimulatableTs...>`),
// and a session running `<SimulatableBrawler, SimulatableVehicle>` therefore has
// TWO distinct Input types and TWO distinct State types with no meaningful
// "the" Input. Those types are owned by the simulatable pack and reach the
// template stack through it. Forcing a single Input/State pair into NetConfig
// would collapse that pack to arity one and break the existing variadic design.
// NetConfig is deliberately session-level ONLY: wire identity + tick rate.
//
// NAMESPACE NOTE: the design corpus (proposal §2.1) writes this concept as
// `ogsim::NetConfig`, and this initiative's backlog quotes that spelling in its
// acceptance criteria. The entire existing OGSim core — including the two
// concepts above in this very file — lives in the GLOBAL namespace, and there
// is no `namespace ogsim` anywhere in the tree. This declaration therefore
// matches the established codebase convention, exactly as SimulatableList.h
// already resolved the same design-corpus-vs-codebase mismatch. Downstream
// tasks (T4/T5) should reference `NetConfig` unqualified.
// ---------------------------------------------------------------------------
template <typename C>
concept NetConfig = requires
{
    typename C::Address;                                     // opaque wire-identity handle
    { C::tickFrequencyHz } -> std::convertible_to<int>;      // session tick rate

    // Engine-agnostic minimum contract on Address: a hashable regular value
    // type with a default/sentinel state and a liveness probe used to reap
    // dropped connections from the per-connection tables.
    requires std::regular<typename C::Address>;              // copyable + equality-comparable
    requires std::default_initializable<typename C::Address>; // null/sentinel state
    requires requires(const typename C::Address& a)
    {
        { std::hash<typename C::Address>{}(a) } -> std::convertible_to<std::size_t>;
        { a.isAlive() } -> std::convertible_to<bool>;
    };
};
