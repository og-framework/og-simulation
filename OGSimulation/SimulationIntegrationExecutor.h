#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <tuple>
#include <unordered_map>
#include "OGSimulation/PhysicsBodyAdapter.h"
#include "OGSimulation/SpatialQueryAdapter.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationTimeContext.h"

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

template <
    typename StaticDataT,
    PhysicsBodyAdapter PhysAdapterT,
    SpatialQueryAdapter QueryAdapterT,
    typename... SimulatableTs>
    requires (SimulatableIntegration<SimulatableTs, PhysAdapterT, QueryAdapterT, StaticDataT> && ...)
class SimulationIntegrationExecutor
{
public:
    // StaticDataT is default-constructed in place — its inner references bind to
    // m_staticData's own members, avoiding a dangling-reference hazard that arises
    // when copying/moving StaticData (whose nested fields hold refs, not values).
    SimulationIntegrationExecutor(
        PhysAdapterT& physAdapter,
        QueryAdapterT& queryAdapter,
        SimulationObjectStorage<SimulatableTs...>& storage)
        : m_physicsAdapter(physAdapter)
        , m_queryAdapter(queryAdapter)
        , m_staticData{}
        , m_storage(storage)
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

    // Mutable access to the executor's internal object storage. Systems
    // (e.g. the SimulationSystemsExecutor peer) project storage views off
    // this reference to run their pre/post-integrate hooks.
    SimulationObjectStorage<SimulatableTs...>& editStorage()
    {
        return m_storage;
    }

    // Const access to the executor's StaticData.
    //
    // The `const&` return type is load-bearing, NOT stylistic: per the ctor
    // doc-comment above, m_staticData is default-constructed in place so its
    // nested fields hold references bound to m_staticData's own members.
    // Returning by value would copy/move StaticData and silently leave those
    // internal references dangling (pointing at the moved-from original).
    // Every consumer in this initiative therefore takes StaticData by
    // const-reference; do NOT change this to a by-value return.
    const StaticDataT& getStaticData() const
    {
        return m_staticData;
    }

private:
    PhysAdapterT&                              m_physicsAdapter;
    QueryAdapterT&                             m_queryAdapter;
    StaticDataT                                m_staticData;
    SimulationObjectStorage<SimulatableTs...>& m_storage;
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
