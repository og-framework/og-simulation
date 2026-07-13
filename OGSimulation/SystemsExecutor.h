#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <tuple>
#include <utility>

#include "OGSimulation/SimulatableList.h"           // SimulatableList, apply_t, IsSimulatableList, list_contains_v
#include "OGSimulation/StorageView.h"                // StorageView (apply_t<StorageView, RequiredSimulatables>)
#include "OGSimulation/SimulationObjectStorage.h"    // SimulationObjectStorage (StorageT + projectTo<>)
#include "OGSimulation/SimulationTimeContext.h"      // SimulationTimeStep

#pragma optimize("", off)

// ---------------------------------------------------------------------------
// SimulationSystem concept + SimulationSystemsExecutor + NullSystemsExecutor.
//
// A "system" is a cross-simulatable coordinator — the OGSim analog of an ECS
// "system": per-tick logic that reads/writes across the whole simulatable
// population, in contrast to SimulatableT::integrate (per-entity local state
// work). Concrete systems (brawlerHitRouting::System, brawlerDamage::System,
// …) live in game core; this header is engine-agnostic AND game-agnostic OGSim
// core. See system_api_design.md §3.11 / §4.1 / §8.3 for the full rationale.
//
// NAMESPACE NOTE: the design corpus wraps these in `namespace ogsim`, but the
// entire existing OGSim core (SimulationObjectStorage, SimulationTimeStep,
// BodyId, every executor) plus tasks 1-4's primitives live in the GLOBAL
// namespace. This header matches that convention (lead D12, ratified
// 2026-07-07); the `ogsim::` qualification in the design/backlog is schematic.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// SimulationSystem<T, StaticDataT> — compile-time contract for a system.
//
// T satisfies the concept if it (a) declares a RequiredSimulatables type alias
// (a SimulatableList<...> naming exactly the simulatables it needs to observe)
// and (b) provides the four hook methods, each accepting a StorageView<...>
// matching those requirements plus a const StaticDataT& for game-static config
// lookup. No inheritance requirement — systems are plain classes that duck-type
// into the concept.
//
// Requirement ordering is load-bearing: the "RequiredSimulatables is a
// SimulatableList<>" gate (S9) fires FIRST — as its own requires-block — so a
// system that forgets the wrap gets a legible IsSimulatableList diagnostic
// before the hook checks (which reference apply_t<StorageView, ...> and would
// otherwise produce a noisier failure).
//
// View-parameter shape (NEW-4 honest scope): the requires-expression passes
// `view` as an LVALUE, so a hook may take it by value, by
// `const StorageView<...>&`, or by `StorageView<...>&` — all three satisfy the
// concept. Convention is by-value (matches the view's lightweight-wrapper
// nature; symmetric with std::span); only an rvalue-ref hook would fail. The
// §3.11 C2 fix picks by-value as the intended convention but does not
// compile-block the const-ref / mutable-ref alternatives.
// ---------------------------------------------------------------------------
template <typename T, typename StaticDataT>
concept SimulationSystem = requires
{
    typename T::RequiredSimulatables;
    requires IsSimulatableList<typename T::RequiredSimulatables>;   // S9 — clearer diagnostic than an undefined list_contains hit
} && requires(
    T& t,
    const SimulationTimeStep& step,
    unsigned int id,
    apply_t<StorageView, typename T::RequiredSimulatables> view,    // lvalue — hooks may take by value / const-ref / mutable-ref (see §3.11 / C2)
    const StaticDataT& staticData)
{
    { t.preIntegrate(step, view, staticData)           } -> std::same_as<void>;   // S8 — tighten diagnostic
    { t.postIntegrate(step, view, staticData)          } -> std::same_as<void>;
    { t.onCharacterRegistered(id, view, staticData)    } -> std::same_as<void>;
    { t.onCharacterUnregistered(id, view, staticData)  } -> std::same_as<void>;

    // FUTURE (deferred v1): intraIntegrate — fires DURING integrateAll, between
    // the producer sub-sims and the consumer sub-sims of each per-character
    // integrate. Eliminates the D3 one-tick latency at the cost of a split-phase
    // integrateAll. Additive strategy (§8.3): v1 systems do NOT declare
    // intraIntegrate; when the feature ships, the executor's fireIntraIntegrate
    // uses `if constexpr (requires { ... })` to skip systems missing the method.
    // No concept change, no v1-system source changes. See fireIntraIntegrate
    // block-comment below and §3.3.
};

// ---------------------------------------------------------------------------
// SimulationSystemsExecutor — the fourth peer alongside
// SimulationIntegrationExecutor, SimulationNetSync, and SimulationReconciliation.
// Holds a compile-time-fixed tuple of concrete system objects, all satisfying
// SimulationSystem. Fires hooks via a std::apply fold-expression over the tuple
// in template-parameter order, projecting the full storage down to each system's
// declared view before calling.
//
// Determinism: firing order == template-parameter order. Server and every
// client compile the same type alias, so the order is byte-identical across
// machines by construction.
//
// Ownership: systems are value-members of the tuple. No heap allocations, no
// virtual dispatch.
// ---------------------------------------------------------------------------

// Primary template — UNDEFINED. Forces callers onto the SimulatableList<...>
// marker path (§3.10); an accidental raw-pack instantiation is an incomplete
// type, not a confusing partial match.
template <typename SimulatablesList, typename StaticDataT, typename... SystemTs>
class SimulationSystemsExecutor;

// Specialization on the SimulatableList<...> marker. StaticDataT is threaded so
// systems can look up game-static config (per-attack damage tables, per-character
// tuning, …) inside their hooks — the same StaticDataT that
// SimulationIntegrationExecutor holds.
//
// The requires-clause has two conjunct folds:
//   (1) every SystemT satisfies SimulationSystem<SystemT, StaticDataT>, and
//   (2) every SystemT's RequiredSimulatables is a subset of the game's full
//       SimulatableList<SimulatableTs...> (so projectTo<> is always well-formed).
template <typename... SimulatableTs, typename StaticDataT, typename... SystemTs>
    requires (SimulationSystem<SystemTs, StaticDataT> && ...)
          && (list_contains_v<SimulatableList<SimulatableTs...>,
                              typename SystemTs::RequiredSimulatables> && ...)
class SimulationSystemsExecutor<SimulatableList<SimulatableTs...>, StaticDataT, SystemTs...>
{
    using StorageT = SimulationObjectStorage<SimulatableTs...>;
    std::tuple<SystemTs...> m_systems;

public:
    // Default construction — every system must be default-constructible. For
    // systems needing constructor args, use the piecewise ctor below.
    SimulationSystemsExecutor() = default;

    // Per-system constructor args via std::piecewise_construct: each element of
    // args... is a std::tuple forwarded to the matching system's constructor.
    template <typename... TupleArgs>
    explicit SimulationSystemsExecutor(std::piecewise_construct_t, TupleArgs&&... args)
        : m_systems(std::forward<TupleArgs>(args)...)
    {
    }

    // --- Per-tick firing points ---------------------------------------------
    //
    // Each system receives (a) a projected view over exactly its declared
    // RequiredSimulatables, materialized FRESH per system per fire, and (b) a
    // const reference to the game's StaticData. StorageView is a lightweight
    // wrapper (opaque pointer + a tuple of function-pointer PODs) so per-fire
    // construction is essentially free — no allocation, no runtime type lookup.
    //
    // `view` is materialized into a LOCAL so it is an LVALUE at the hook call
    // site — this is the C2 fix from the reviews: a hook taking the view by
    // `StorageView<...>&` (mutable-ref) would fail to bind to a temporary, so
    // the executor must not pass a prvalue directly into the hook.

    void firePreIntegrate(const SimulationTimeStep& step, StorageT& storage,
                          const StaticDataT& staticData)
    {
        std::apply([&](auto&... systems)
        {
            ([&]{
                auto view = storage.template projectTo<
                    typename std::decay_t<decltype(systems)>::RequiredSimulatables>();
                systems.preIntegrate(step, view, staticData);
            }(), ...);
        }, m_systems);
    }

    void firePostIntegrate(const SimulationTimeStep& step, StorageT& storage,
                           const StaticDataT& staticData)
    {
        std::apply([&](auto&... systems)
        {
            ([&]{
                auto view = storage.template projectTo<
                    typename std::decay_t<decltype(systems)>::RequiredSimulatables>();
                systems.postIntegrate(step, view, staticData);
            }(), ...);
        }, m_systems);
    }

    // --- Character-lifecycle fan-out ----------------------------------------
    // NOTE: NO SimulationTimeStep parameter — lifecycle notifications are not
    // tick-scoped (they fire on spawn/despawn, out of band from integrateAll).

    void notifyCharacterRegistered(unsigned int id, StorageT& storage,
                                   const StaticDataT& staticData)
    {
        std::apply([&](auto&... systems)
        {
            ([&]{
                auto view = storage.template projectTo<
                    typename std::decay_t<decltype(systems)>::RequiredSimulatables>();
                systems.onCharacterRegistered(id, view, staticData);
            }(), ...);
        }, m_systems);
    }

    void notifyCharacterUnregistered(unsigned int id, StorageT& storage,
                                     const StaticDataT& staticData)
    {
        std::apply([&](auto&... systems)
        {
            ([&]{
                auto view = storage.template projectTo<
                    typename std::decay_t<decltype(systems)>::RequiredSimulatables>();
                systems.onCharacterUnregistered(id, view, staticData);
            }(), ...);
        }, m_systems);
    }

    // --- FUTURE (deferred per §3.3): fireIntraIntegrate ---------------------
    //
    // When the interleaved firing point ships, this method uses a per-system
    // detection idiom so v1 systems WITHOUT intraIntegrate remain valid — no
    // concept split, no v1-system source changes:
    //
    //   void fireIntraIntegrate(const SimulationTimeStep& step, StorageT& storage,
    //                           const StaticDataT& staticData)
    //   {
    //       std::apply([&](auto&... systems) {
    //           ([&]{
    //               using S = std::decay_t<decltype(systems)>;
    //               auto view = storage.template projectTo<typename S::RequiredSimulatables>();
    //               if constexpr (requires (S& s) { s.intraIntegrate(step, view, staticData); }) {
    //                   systems.intraIntegrate(step, view, staticData);
    //               }
    //           }(), ...);
    //       }, m_systems);
    //   }
    //
    // See §3.3 + §8.3.

    // --- Direct system access (for testing, debug, cross-system hooks) ------

    template <typename SystemT>
    SystemT& get() { return std::get<SystemT>(m_systems); }

    template <typename SystemT>
    const SystemT& get() const { return std::get<SystemT>(m_systems); }
};

// Zero-systems flavor. Satisfies the specialization's vacuous requires-clause
// (both folds are empty ⇒ true), giving a well-typed executor whose four fire
// methods are empty folds (no-ops). Used by any game/manager wiring that has no
// systems yet — the fourth peer is still present and callable.
template <typename SimulatablesList, typename StaticDataT>
using NullSystemsExecutor = SimulationSystemsExecutor<SimulatablesList, StaticDataT>;

#pragma optimize("", on)
