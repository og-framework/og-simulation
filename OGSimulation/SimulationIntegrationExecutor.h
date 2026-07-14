#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <tuple>
#include <unordered_map>
#include "OGSimulation/PhysicsBodyAdapter.h"
#include "OGSimulation/SpatialQueryAdapter.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationTimeContext.h"

// pragma optimize off — debugger-friendliness across all build configs (breakpoints hit,
// locals visible, call-stack intact). OGSim-core convention.
#pragma optimize("", off)

// Adapter-dependent side of a simulatable: integrate() and firstResimStep().
// Required by SimulationIntegrationExecutor.
template <typename T, typename PhysAdapterT, typename QueryAdapterT, typename StaticDataT>
concept SimulatableIntegration =
    PhysicsBodyAdapter<PhysAdapterT> &&
    SpatialQueryAdapter<QueryAdapterT> &&
    requires(
        T& t,
        PhysAdapterT& phys,
        QueryAdapterT& query,
        const SimulationTimeStep& step,
        const typename T::InputType& input,
        const StaticDataT& sd,
        int32_t physStep)
    {
        typename T::InputType;
        { t.integrate(step, input, phys, query, sd) };
        { t.firstResimStep(phys, physStep) };
    };

// Storage and StaticData are EXTERNALLY OWNED (by the engine adapter's
// composition root); the executor holds only references to them. This makes the
// object storage a genuinely shared resource across all simulation peers
// (integration executor, net-sync, reconciliation, systems executor) rather than
// being owned by one peer and bridged to the others via accessors. See the
// adapter's m_staticData declaration for the StaticData internal-reference
// invariant that mandates the const& (never by-value) handling.
template <
    typename StaticDataT,
    PhysicsBodyAdapter PhysAdapterT,
    SpatialQueryAdapter QueryAdapterT,
    typename... SimulatableTs>
    requires (SimulatableIntegration<SimulatableTs, PhysAdapterT, QueryAdapterT, StaticDataT> && ...)
class SimulationIntegrationExecutor
{
public:
    // storage and staticData are externally owned; the executor stores references.
    // staticData MUST be constructed in place at its owner and never copied — its
    // nested fields hold references bound to its own members (see the adapter's
    // m_staticData doc-comment), so the const& binding is load-bearing, not
    // stylistic.
    SimulationIntegrationExecutor(
        SimulationObjectStorage<SimulatableTs...>& storage,
        const StaticDataT& staticData,
        PhysAdapterT& physAdapter,
        QueryAdapterT& queryAdapter)
        : m_storage(storage)
        , m_staticData(staticData)
        , m_physicsAdapter(physAdapter)
        , m_queryAdapter(queryAdapter)
    {}

    using ResolvedInputsType = ResolvedInputs<SimulatableTs...>;

    void integrateAll(const SimulationTimeStep& step, const ResolvedInputsType& inputs)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using SimulatableT = std::remove_reference_t<decltype(simulatable)>;
            const auto& map = std::get<
                std::unordered_map<unsigned int, typename SimulatableT::InputType>>(inputs);
            if (auto it = map.find(id); it != map.end())
                simulatable.integrate(step, it->second, m_physicsAdapter, m_queryAdapter, m_staticData);
        });
    }

    void firstResimStepAll(int32_t physicsStep)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            simulatable.firstResimStep(m_physicsAdapter, physicsStep);
        });
    }

    void captureBodyStatesAll()
    {
        m_storage.forEachSimulatable([&](unsigned int /*id*/, auto& sim) {
            sim.editPhysicsComposite().forEach([&](auto& decl) {
                using D = std::decay_t<decltype(decl)>;
                using S = typename D::StateType;
                D::bodyStateOf(sim.editAllState().editState().template edit<S>()) =
                    m_physicsAdapter.captureBodyState(decl.bindings.ownBodyId);
            });
        });
    }

private:
    // Externally-owned references (see class + ctor doc-comments). Declaration
    // order matches the ctor init-list to keep -Wreorder clean.
    SimulationObjectStorage<SimulatableTs...>& m_storage;
    const StaticDataT&                         m_staticData;
    PhysAdapterT&                              m_physicsAdapter;
    QueryAdapterT&                             m_queryAdapter;
};

// Concept for types that implement the SimulationIntegrationExecutor interface.
// Lives here so task 6 can static_assert ASimulationManagerUImpl's member against it.
template <typename T>
concept SimulationIntegrationExecutorConcept = requires(
    T& t, const SimulationTimeStep& step, int32_t physStep)
{
    { t.firstResimStepAll(physStep) };
    { t.captureBodyStatesAll() };
};

#pragma optimize("", on)
// pragma optimize on — restore command-line optimization settings.
