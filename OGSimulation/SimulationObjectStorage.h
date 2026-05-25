#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <memory>
#include <tuple>
#include <unordered_map>

#include "OGAssert.h"

#pragma optimize("", off)

// ---------------------------------------------------------------------------
// SimulatableState concept — adapter-agnostic side of a simulatable.
// Required by SimulationObjectStorage and SimulationReconciliation.
// ---------------------------------------------------------------------------
template <typename T>
concept SimulatableState = requires(T& t)
{
    typename T::StateType;
    typename T::InputType;
    { t.getAllState() };
    { t.editAllState() };
    { t.updateVizState() };
    { t.getVizState() };
};

// Canonical free alias for per-type id→input maps.
// Stack-scoped; passed by const ref to SimulationIntegrationExecutor::integrateAll
// and returned from SimulationNetSync::collectInputAll / SimulationReconciliation::collectResimInputAll.
template <typename... Ts>
using ResolvedInputs = std::tuple<
    std::unordered_map<unsigned int, typename Ts::InputType>...>;

// Variadic tuple-of-maps owning simulatable instances via unique_ptr for stable addresses.
// SimulationIntegrationExecutor, SimulationReconciliation, and SimulationNetSync all
// hold non-owning refs to this storage; ASimulationManagerUImpl owns it by value.
template <typename... SimulatableTs>
class SimulationObjectStorage
{
public:
    template <typename T>
    void add(unsigned int id, T&& simulatable)
    {
        auto& map = std::get<MapFor<T>>(m_maps);
        map.emplace(id, std::make_unique<T>(std::forward<T>(simulatable)));
    }

    template <typename T>
    T& get(unsigned int id)
    {
        auto& map = std::get<MapFor<T>>(m_maps);
        auto it = map.find(id);
        OG_CHECK(it != map.end(), "SimulationObjectStorage::get — id not found");
        return *it->second;
    }

    template <typename T>
    const T& get(unsigned int id) const
    {
        const auto& map = std::get<MapFor<T>>(m_maps);
        auto it = map.find(id);
        OG_CHECK(it != map.end(), "SimulationObjectStorage::get — id not found");
        return *it->second;
    }

    template <typename T>
    bool has(unsigned int id) const
    {
        const auto& map = std::get<MapFor<T>>(m_maps);
        return map.find(id) != map.end();
    }

    template <typename T>
    void remove(unsigned int id)
    {
        std::get<MapFor<T>>(m_maps).erase(id);
    }

    // Iterates all simulatables of all types, calling func(id, simulatable&).
    // func receives the simulatable by ref; type is deduced as the concrete type.
    template <typename Func>
    void forEachSimulatable(Func&& func)
    {
        (forEachOfType<SimulatableTs>(func), ...);
    }

    template <typename Func>
    void forEachSimulatable(Func&& func) const
    {
        (forEachOfTypeConst<SimulatableTs>(func), ...);
    }

private:
    template <typename T>
    using MapFor = std::unordered_map<unsigned int, std::unique_ptr<T>>;

    std::tuple<MapFor<SimulatableTs>...> m_maps;

    template <typename T, typename Func>
    void forEachOfType(Func& func)
    {
        for (auto& [id, ptr] : std::get<MapFor<T>>(m_maps))
            func(id, *ptr);
    }

    template <typename T, typename Func>
    void forEachOfTypeConst(Func& func) const
    {
        for (const auto& [id, ptr] : std::get<MapFor<T>>(m_maps))
            func(id, *ptr);
    }
};

// Free function — drives visualization update for all simulatables.
// Called by ASimulationManagerUImpl::OnPostPhysicsStep directly;
// not a networking or cache concern.
template <typename... Ts>
void updateVisualizationAll(SimulationObjectStorage<Ts...>& storage)
{
    storage.forEachSimulatable([](unsigned int, auto& simulatable) {
        simulatable.updateVizState();
    });
}

#pragma optimize("", on)
