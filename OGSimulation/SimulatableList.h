#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <type_traits>

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize("", off)

// ---------------------------------------------------------------------------
// SimulatableList<...> marker + apply_t + list_contains_v + IsSimulatableList
//
// The compile-time toolkit the OGSim system-API stack (StorageView, the
// SimulationSystem concept, and SimulationSystemsExecutor) is built on. See
// system_api_design.md §3.10 / §4.1 for the full design rationale.
//
// NAMESPACE NOTE: the design sketch wraps these in `namespace ogsim`, but the
// entire existing OGSim core (SimulationObjectStorage, SimulationTimeStep,
// BodyId, every executor) lives in the GLOBAL namespace, and task 1's
// acceptance-criteria static_asserts reference these symbols UNQUALIFIED.
// This header therefore matches the established codebase convention (global
// namespace). The `ogsim::` qualification in the design corpus / downstream
// backlog tasks is schematic; downstream tasks reference these as global.
// ---------------------------------------------------------------------------

// Marker type: wraps a pack of simulatable types so a template that already
// has one variadic pack (the systems pack) can accept a second one via a
// single parameter. Empty struct — no ABI cost.
template <typename... Ts>
struct SimulatableList {};

// apply_t<F, SimulatableList<Ts...>> = F<Ts...>.
// Lets a game define its sim pack ONCE (e.g. BrawlerSimulatables) and derive
// every executor from it: apply_t<SimulationObjectStorage, BrawlerSimulatables>.
template <template <typename...> class F, typename List>
struct apply;

template <template <typename...> class F, typename... Ts>
struct apply<F, SimulatableList<Ts...>>
{
    using type = F<Ts...>;
};

template <template <typename...> class F, typename List>
using apply_t = typename apply<F, List>::type;

// is_type_in_pack_v<T, Ts...> — true if T appears anywhere in the pack Ts... .
template <typename T, typename... Ts>
inline constexpr bool is_type_in_pack_v = (std::is_same_v<T, Ts> || ...);

// list_contains<Superset, Subset> — compile-time subset check over two
// SimulatableList<...>s. True if every T in Subset appears in Superset. Used
// by the systems executor's requires-clause to verify each system's declared
// RequiredSimulatables is available in the game's full sim pack.
//
// Primary is intentionally ill-formed on instantiation: a hard static_assert
// on non-SimulatableList<> inputs produces a legible diagnostic pointing at
// the caller's mistake (typically a system's RequiredSimulatables not wrapped
// in SimulatableList<>). The IsSimulatableList concept gate on the callers
// fires FIRST so this static_assert shouldn't be reached from those paths —
// this belt-and-braces the case where list_contains_v is used directly.
template <typename Superset, typename Subset>
struct list_contains
{
    static_assert(!std::is_same_v<Superset, Superset>,
        "list_contains<A, B>: A and B must both be SimulatableList<...>");
};

template <typename... SupTs, typename... SubTs>
struct list_contains<SimulatableList<SupTs...>, SimulatableList<SubTs...>>
    : std::bool_constant<(is_type_in_pack_v<SubTs, SupTs...> && ...)> {};

template <typename Superset, typename Subset>
inline constexpr bool list_contains_v = list_contains<Superset, Subset>::value;

// Detection trait + concept for the "T is a SimulatableList<...>" gate.
// Used by the SimulationSystem concept to fail fast with a clear diagnostic
// if a system's RequiredSimulatables isn't wrapped.
template <typename T>
struct is_simulatable_list : std::false_type {};

template <typename... Ts>
struct is_simulatable_list<SimulatableList<Ts...>> : std::true_type {};

template <typename T>
concept IsSimulatableList = is_simulatable_list<T>::value;

#pragma optimize("", on)
// pragma optimize on.
