#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <memory>
#include <tuple>
#include <type_traits>

#include "OGSimulation/SimulatableList.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize("", off)

// Forward declaration for the friend grant inside StorageView — the sole
// constructor of a StorageView is SimulationObjectStorage::projectTo<>()
// (design §4.1). Declared here (not #included) to avoid a header cycle:
// SimulationObjectStorage.h includes THIS header.
template <typename... AllTs>
class SimulationObjectStorage;

// ---------------------------------------------------------------------------
// StorageView<...> — a projected, non-owning slice of a SimulationObjectStorage.
//
// A system declares a RequiredSimulatables = SimulatableList<...> and receives a
// StorageView<WantedTs...> (by value) in its hook methods instead of the game's
// full storage. The view forwards typed accessors (get<T>, forEachSimulatable<T>)
// to the underlying storage; naming a simulatable type NOT in WantedTs... is a
// compile error gated by a legible requires-clause. See system_api_design.md
// §3.11 / §4.1 for the full rationale.
//
// Global namespace per lead decision D12 (see SimulatableList.h).
//
// ---- IMPLEMENTATION STRATEGY (design §4.1) --------------------------------
//
// The view must reach into a SimulationObjectStorage<AllTs...> whose full pack it
// does NOT know (only WantedTs..., a subset). It therefore holds an OPAQUE handle
// to the storage (void* m_storage) plus, per WantedT, a typed thunk pair — plain
// function pointers produced at projection time by the concretely-typed storage
// (which alone knows AllTs... and can emit a `T& (*)(void*, unsigned int)` etc.).
// This keeps a system's method signature naming only StorageView<WantedTs...>
// (never the game's full pack) while costing nothing at runtime: get<T> is one
// indirect call, and forEachSimulatable<T> threads an arbitrary callable through
// a plain function pointer via a stack-local type-erasure trampoline — no heap.
//
// The view is a trivially-destructible value type (opaque pointer + a tuple of
// function-pointer PODs); it owns no heap memory, so passing it by value — as the
// SimulationSystem hooks do — is cheap and allocation-free.
//
// PRODUCTION CONSTRUCTION: SimulationObjectStorage::projectTo<>() builds the
// thunks and constructs the view. The constructor is PRIVATE; the sole grant is
// `friend class SimulationObjectStorage` (all specializations) per design §4.1 —
// a StorageView is "not directly constructible by user code".
//
// ---- ITERATION ORDER (§3.11) ----------------------------------------------
//
// forEachSimulatable<T> delegates to the underlying storage's unordered_map
// iteration, whose order is unspecified across builds/runs. Systems whose logic
// depends on cross-character ordering MUST sort by id themselves inside the hook.
// A future forEachSimulatableSorted<T> is possible but not v1.
// ---------------------------------------------------------------------------

// Per-WantedT typed thunk pair. Grouped one-struct-per-type so StorageView can
// std::get<> the right thunk by type. Populated at projection time; a default-
// constructed (all-null) thunk is only ever seen on a default-constructed view.
template <typename T>
struct StorageViewThunk
{
	// Resolve id -> T& in the underlying storage.
	T& (*get)(void* storage, unsigned int id) = nullptr;

	// Type-erased iteration. The view supplies a stack-local trampoline
	// (ctx + invoke fn ptr) so any callable can be threaded through this plain
	// function pointer without a heap allocation.
	void (*forEach)(void* storage, void* ctx,
		void (*invoke)(void* ctx, unsigned int id, T& item)) = nullptr;
};

template <typename... WantedTs>
class StorageView
{
	// The view is minted ONLY by SimulationObjectStorage::projectTo<>(), which
	// alone knows the storage's full pack and can bind one StorageViewThunk per
	// WantedT to its concrete accessors (design §4.1). Befriend every
	// SimulationObjectStorage specialization so any game's storage can project.
	template <typename... AllTs>
	friend class SimulationObjectStorage;

	// PRIVATE ctor — not directly constructible by user code (design §4.1). The
	// projecting storage populates the opaque handle + typed thunks.
	StorageView(void* storage, StorageViewThunk<WantedTs>... thunks)
		: m_storage(storage)
		, m_thunks(thunks...)
	{
	}

public:
	// Typed single-object access — only compiles for T in WantedTs... . The
	// requires-clause produces a legible unsatisfied-constraint diagnostic (not a
	// deep template-instantiation error) when a system asks for a type it did not
	// declare in RequiredSimulatables.
	template <typename T>
		requires (std::is_same_v<T, WantedTs> || ...)
	T& get(unsigned int id)
	{
		return std::get<StorageViewThunk<T>>(m_thunks).get(m_storage, id);
	}

	// Per-type iteration — calls fn(id, T&) for every T of that type. Only
	// compiles for T in WantedTs... (same gate as get<T>).
	template <typename T, typename F>
		requires (std::is_same_v<T, WantedTs> || ...)
	void forEachSimulatable(F&& fn)
	{
		using Callable = std::remove_reference_t<F>;
		void* ctx = static_cast<void*>(std::addressof(fn));
		void (*invoke)(void*, unsigned int, T&) =
			[](void* c, unsigned int id, T& item)
			{
				(*static_cast<Callable*>(c))(id, item);
			};
		std::get<StorageViewThunk<T>>(m_thunks).forEach(m_storage, ctx, invoke);
	}

	// Uniform iteration across EVERY type in WantedTs... — folds the per-type
	// forEachSimulatable<T> over the pack, invoking a generic lambda once per
	// entity per type. The body must compile for every type in the view; use
	// `if constexpr (requires { ... })` gates for optional per-type behaviour.
	// Only visits the types the view declared; the game's full pack is not
	// observable here. Zero overhead vs. writing the per-type calls by hand.
	template <typename F>
	void forEachAny(F&& fn)
	{
		(forEachSimulatable<WantedTs>(fn), ...);
	}

private:
	void* m_storage = nullptr;
	std::tuple<StorageViewThunk<WantedTs>...> m_thunks;
};

#pragma optimize("", on)
// pragma optimize on.
