#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <memory>
#include <tuple>
#include <unordered_map>

#include "OGAssert.h"
#include "OGSimulation/SimulatableList.h"  // SimulatableList, apply_t, IsSimulatableList, list_contains_v
#include "OGSimulation/StorageView.h"      // StorageView, StorageViewThunk (projectTo<> return + thunks)

// pragma optimize off — debugger-friendliness across all build configs (breakpoints hit,
// locals visible, call-stack intact). OGSim-core convention.
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

    // Per-type iteration — the PUBLIC per-type entry point promoted from the
    // formerly-private forEachOfType<T> (design §5.3: "public per-type overload
    // wins"). Calls func(id, T&) for every simulatable of type T. This is what the
    // StorageView projection thunks bind to. Coexists unambiguously with the
    // all-types overloads above: an unqualified forEachSimulatable(fn) deduces Func
    // only, so this overload (whose T is non-deducible without an explicit argument)
    // drops out; an explicit forEachSimulatable<T>(fn) selects this one.
    template <typename T, typename Func>
    void forEachSimulatable(Func&& func)
    {
        forEachOfType<T>(func);
    }

    template <typename T, typename Func>
    void forEachSimulatable(Func&& func) const
    {
        forEachOfTypeConst<T>(func);
    }

    // Projects this storage down to the subset a system declared, returning a
    // StorageView over just WantedList's types (design §3.11 / §4.1). The returned
    // view holds an opaque handle to *this plus, per WantedT, typed function-pointer
    // thunks bound to this storage's concrete get<T> / forEachSimulatable<T>.
    //
    // Constraint ordering is load-bearing (NEW-2 / SB-4): IsSimulatableList is the
    // FIRST conjunct, so a caller that forgets the wrap — projectTo<SomeType>() —
    // fails on that concept with a legible diagnostic BEFORE list_contains_v would
    // otherwise instantiate list_contains<>'s primary template and hit its hard
    // static_assert. A well-formed-but-not-a-subset request —
    // projectTo<SimulatableList<Unrelated>>() where Unrelated is not in
    // SimulatableTs... — fails on the list_contains_v conjunct instead.
    template <typename WantedList>
        requires IsSimulatableList<WantedList>
              && list_contains_v<SimulatableList<SimulatableTs...>, WantedList>
    auto projectTo() -> apply_t<StorageView, WantedList>
    {
        return makeView(WantedList{});
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

    // Unpacks WantedList's pack and mints the StorageView with per-WantedT thunks.
    // Constructs the view through its PRIVATE ctor (this storage is a friend of
    // StorageView per design §4.1).
    template <typename... WantedTs>
    StorageView<WantedTs...> makeView(SimulatableList<WantedTs...>)
    {
        return StorageView<WantedTs...>(
            static_cast<void*>(this),
            StorageViewThunk<WantedTs>{ &getThunk<WantedTs>, &forEachThunk<WantedTs> }...);
    }

    // Typed thunks bound into the view at projection time. Each recovers the
    // concrete storage from the opaque handle and forwards to a public accessor;
    // both are stack-only (no heap), so view access is allocation-free.
    template <typename T>
    static T& getThunk(void* storage, unsigned int id)
    {
        return static_cast<SimulationObjectStorage*>(storage)->template get<T>(id);
    }

    template <typename T>
    static void forEachThunk(void* storage, void* ctx,
        void (*invoke)(void* ctx, unsigned int id, T& item))
    {
        static_cast<SimulationObjectStorage*>(storage)->template forEachSimulatable<T>(
            [ctx, invoke](unsigned int id, T& item) { invoke(ctx, id, item); });
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
// pragma optimize on — restore command-line optimization settings.
