#pragma once
// SPDX-License-Identifier: MPL-2.0

// SimulationDependencies — tuple utilities, ValidDependencies,
// makeDependencies, execution order validation, ownership validation,
// structured violation diagnostics, and ValidExecutionOrder concept.

#include "OGSimulation/SimulationComposite.h"

#include <type_traits>

// ---------------------------------------------------------------------------
// Dependency infrastructure — tuple utilities,
// OwnedDeps/ExternalDeps templates, ValidDependencies, makeDependencies,
// and ValidExecutionOrder for nested Owned/External layout.
// ---------------------------------------------------------------------------

namespace compositeDetail {

// TupleCat_t — concatenate two std::tuple types.
template <typename T1, typename T2>
struct TupleCat;

template <typename... T1s, typename... T2s>
struct TupleCat<std::tuple<T1s...>, std::tuple<T2s...>> {
	using type = std::tuple<T1s..., T2s...>;
};

template <typename T1, typename T2>
using TupleCat_t = typename TupleCat<T1, T2>::type;

// AddMutableRef_t — transforms tuple<T1, T2, ...> → tuple<T1&, T2&, ...>.
template <typename Tuple>
struct AddMutableRef;

template <typename... Ts>
struct AddMutableRef<std::tuple<Ts...>> {
	using type = std::tuple<Ts&...>;
};

template <typename Tuple>
using AddMutableRef_t = typename AddMutableRef<Tuple>::type;

// indexOfBareType — find the index of the Ref whose remove_cvref_t matches T.
template <typename T, typename... Refs>
consteval std::size_t indexOfBareType()
{
	constexpr bool matches[] = { std::is_same_v<T, std::remove_cvref_t<Refs>>... };
	for (std::size_t i = 0; i < sizeof...(Refs); ++i)
		if (matches[i]) return i;
	return std::size_t(-1); // not found — will cause compilation error at use site
}

// resolveRef — dispatches const T& → get<T>(), T& → edit<T>().
template <typename Ref, typename Composite>
auto resolveRef(Composite& composite) -> Ref
{
	using T = std::remove_reference_t<Ref>;
	if constexpr (std::is_const_v<T>)
		return composite.template get<std::remove_const_t<T>>();
	else
		return composite.template edit<T>();
}

// makeOwnedImpl — constructs OwnedDeps from composite.
// OwnedDeps::Types = tuple<T1, T2, ...> (bare value types); resolved as T1&, T2&.
template <typename Sub, typename Composite, std::size_t... Is>
Sub makeOwnedImpl(Composite& composite, std::index_sequence<Is...>)
{
	using Refs = AddMutableRef_t<typename Sub::Types>;
	return Sub{ { resolveRef<std::tuple_element_t<Is, Refs>, Composite>(composite)... } };
}

// makeExternalImpl — constructs ExternalDeps from composite.
// ExternalDeps::Types = tuple<const T1&, T2&, ...> (already ref-qualified).
template <typename Sub, typename Composite, std::size_t... Is>
Sub makeExternalImpl(Composite& composite, std::index_sequence<Is...>)
{
	using Refs = typename Sub::Types;
	return Sub{ { resolveRef<std::tuple_element_t<Is, Refs>, Composite>(composite)... } };
}

} // namespace compositeDetail

// ---------------------------------------------------------------------------
// OwnedDeps<Ts...> — stores mutable references to owned types.
// Types = tuple<Ts...> (bare value types).
// ---------------------------------------------------------------------------

template <typename... Ts>
struct OwnedDeps {
	using Types = std::tuple<Ts...>;
	std::tuple<Ts&...> data;

	template <typename T>
	T& edit() { return std::get<T&>(data); }

	template <typename T>
	const T& get() const { return std::get<T&>(data); }
};

// ---------------------------------------------------------------------------
// ExternalDeps<Refs...> — stores references with original qualification.
// Refs are e.g. const T&, T& — preserved as-is in the tuple.
// ---------------------------------------------------------------------------

template <typename... Refs>
struct ExternalDeps {
	using Types = std::tuple<Refs...>;
	std::tuple<Refs...> data;

	template <typename T>
	T& edit() {
		constexpr auto idx = compositeDetail::indexOfBareType<T, Refs...>();
		static_assert(!std::is_const_v<std::remove_reference_t<
			std::tuple_element_t<idx, Types>>>,
			"Cannot edit a const external dependency");
		return std::get<idx>(data);
	}

	template <typename T>
	const T& get() const {
		constexpr auto idx = compositeDetail::indexOfBareType<T, Refs...>();
		return std::get<idx>(data);
	}
};

// ComputeRefTypes — flat ref-type list for aggregate init (owned refs + external refs).
template <typename Deps>
using ComputeRefTypes = compositeDetail::TupleCat_t<
	compositeDetail::AddMutableRef_t<typename Deps::Owned::Types>,
	typename Deps::External::Types>;

// ComputeFullOwnedTypes — all types this simulation owns (owned types + InputType).
template <typename Deps>
using ComputeFullOwnedTypes = compositeDetail::TupleCat_t<
	typename Deps::Owned::Types,
	std::tuple<typename Deps::InputType>>;

// ValidDependencies — checks that Deps has Owned::Types, External::Types, InputType.
template <typename Deps>
concept ValidDependencies = requires {
	typename Deps::Owned;
	typename Deps::Owned::Types;
	typename Deps::External;
	typename Deps::External::Types;
	typename Deps::InputType;
};

// makeDependencies — two-phase construction: owned + external → Deps{ owned, external }.
template <typename Deps, typename Composite>
Deps makeDependencies(Composite& composite)
{
	static_assert(ValidDependencies<Deps>,
		"Dependencies must expose Owned::Types, External::Types and InputType.");
	using OwnedTypes = typename Deps::Owned::Types;
	using ExternalTypes = typename Deps::External::Types;
	auto owned = compositeDetail::makeOwnedImpl<typename Deps::Owned>(
		composite, std::make_index_sequence<std::tuple_size_v<OwnedTypes>>{});
	auto external = compositeDetail::makeExternalImpl<typename Deps::External>(
		composite, std::make_index_sequence<std::tuple_size_v<ExternalTypes>>{});
	return Deps{ owned, external };
}

// ---------------------------------------------------------------------------
// Compile-time execution order validation (updated for Owned/External).
// Edge detection iterates External::Types only — all entries are cross-namespace.
// ---------------------------------------------------------------------------

namespace compositeDetail {

template <typename T, typename Tuple>
constexpr bool isInTuple = false;

template <typename T, typename... Ts>
constexpr bool isInTuple<T, std::tuple<Ts...>> = (std::is_same_v<T, Ts> || ...);

template <typename T, typename ExecutionOrder, std::size_t I = 0>
consteval std::size_t findOwnerIndex()
{
	if constexpr (I >= std::tuple_size_v<ExecutionOrder>)
		return std::size_t(-1);
	else if constexpr (isInTuple<T, ComputeFullOwnedTypes<std::tuple_element_t<I, ExecutionOrder>>>)
		return I;
	else
		return findOwnerIndex<T, ExecutionOrder, I + 1>();
}

// Check if WriterDeps has a hard edge to TargetDeps (non-const write in External::Types
// whose bare type is owned by TargetDeps).
template <typename WriterDeps, typename TargetDeps, typename Ref>
consteval bool isHardEdgeTo()
{
	using T = std::remove_reference_t<Ref>;
	using BareT = std::remove_const_t<T>;
	if constexpr (std::is_const_v<T>)
		return false;
	else
		return isInTuple<BareT, ComputeFullOwnedTypes<TargetDeps>>;
}

template <typename WriterDeps, typename TargetDeps, typename Refs, std::size_t... Is>
consteval bool hasHardEdgeInRefs(std::index_sequence<Is...>)
{
	return (isHardEdgeTo<WriterDeps, TargetDeps, std::tuple_element_t<Is, Refs>>() || ...);
}

template <typename WriterDeps, typename TargetDeps>
consteval bool hasHardEdge()
{
	using Refs = typename WriterDeps::External::Types;
	return hasHardEdgeInRefs<WriterDeps, TargetDeps, Refs>(
		std::make_index_sequence<std::tuple_size_v<Refs>>{});
}

// ---------------------------------------------------------------------------
// Unique ownership validation + structured violation diagnostics
// ---------------------------------------------------------------------------

template <typename...>
inline constexpr bool always_false = false;

enum class ViolationKind : uint8_t {
	None, OwnershipOverlap, UnownedExternalRef, ExecutionOrderViolation
};

struct Violation {
	ViolationKind kind = ViolationKind::None;
	std::size_t simA = 0;     // consumer or first owner index
	std::size_t simB = 0;     // owner or second owner index
	std::size_t typeIdx = 0;  // index into relevant type-list
};

// Find index of bare type T in Deps::External::Types (stripping cv-ref).
template <typename T, typename Deps, std::size_t I = 0>
consteval std::size_t findTypeInExternalTypes()
{
	using Refs = typename Deps::External::Types;
	if constexpr (I >= std::tuple_size_v<Refs>)
		return std::size_t(-1);
	else if constexpr (std::is_same_v<T,
		std::remove_const_t<std::remove_reference_t<std::tuple_element_t<I, Refs>>>>)
		return I;
	else
		return findTypeInExternalTypes<T, Deps, I + 1>();
}

// Validate one external ref against execution order.
// Returns Violation on failure, Violation{} (None) on success.
template <typename Deps, typename ExecutionOrder, std::size_t DepsIndex, typename Ref>
consteval Violation validateOneExternalRef()
{
	using T = std::remove_reference_t<Ref>;
	using BareT = std::remove_const_t<T>;

	constexpr std::size_t ownerIdx = findOwnerIndex<BareT, ExecutionOrder>();
	if constexpr (ownerIdx == std::size_t(-1))
	{
		// Find the index of BareT in the consumer's External::Types for typeIdx.
		constexpr std::size_t refIdx = findTypeInExternalTypes<BareT, Deps>();
		return Violation{ ViolationKind::UnownedExternalRef, DepsIndex, 0, refIdx };
	}
	else if constexpr (std::is_const_v<T>)
	{
		// Soft edge: owner should run before this deps.
		// Drop if a hard edge this→owner exists (conflict).
		if constexpr (hasHardEdge<Deps, std::tuple_element_t<ownerIdx, ExecutionOrder>>())
			return Violation{};
		else if constexpr (ownerIdx < DepsIndex)
			return Violation{};
		else
		{
			constexpr std::size_t refIdx = findTypeInExternalTypes<BareT, Deps>();
			return Violation{ ViolationKind::ExecutionOrderViolation, DepsIndex, ownerIdx, refIdx };
		}
	}
	else
	{
		// Hard edge: this deps runs before owner.
		if constexpr (DepsIndex < ownerIdx)
			return Violation{};
		else
		{
			constexpr std::size_t refIdx = findTypeInExternalTypes<BareT, Deps>();
			return Violation{ ViolationKind::ExecutionOrderViolation, DepsIndex, ownerIdx, refIdx };
		}
	}
}

template <typename Deps, typename ExecutionOrder, std::size_t DepsIndex, typename Refs, std::size_t I, std::size_t N>
consteval Violation validateExternalRefsLoop()
{
	if constexpr (I >= N)
		return Violation{};
	else
	{
		Violation result = validateOneExternalRef<Deps, ExecutionOrder, DepsIndex, std::tuple_element_t<I, Refs>>();
		if (result.kind != ViolationKind::None)
			return result;
		return validateExternalRefsLoop<Deps, ExecutionOrder, DepsIndex, Refs, I + 1, N>();
	}
}

template <typename Deps, typename ExecutionOrder, std::size_t DepsIndex, typename Refs, std::size_t... Is>
consteval Violation validateExternalRefs(std::index_sequence<Is...>)
{
	return validateExternalRefsLoop<Deps, ExecutionOrder, DepsIndex, Refs, 0, sizeof...(Is)>();
}

template <typename Deps, typename ExecutionOrder, std::size_t DepsIndex>
consteval Violation validateOneDeps()
{
	using Refs = typename Deps::External::Types;
	return validateExternalRefs<Deps, ExecutionOrder, DepsIndex, Refs>(
		std::make_index_sequence<std::tuple_size_v<Refs>>{});
}

template <typename ExecutionOrder, std::size_t I, std::size_t N>
consteval Violation validateAllDepsLoop()
{
	if constexpr (I >= N)
		return Violation{};
	else
	{
		Violation result = validateOneDeps<std::tuple_element_t<I, ExecutionOrder>, ExecutionOrder, I>();
		if (result.kind != ViolationKind::None)
			return result;
		return validateAllDepsLoop<ExecutionOrder, I + 1, N>();
	}
}

template <typename ExecutionOrder, std::size_t... Is>
consteval Violation validateAllDeps(std::index_sequence<Is...>)
{
	return validateAllDepsLoop<ExecutionOrder, 0, sizeof...(Is)>();
}

template <typename T, typename ExecutionOrder, std::size_t OwnerIndex, std::size_t J = 0>
consteval Violation isUniquelyOwned()
{
	if constexpr (J >= std::tuple_size_v<ExecutionOrder>)
		return Violation{};
	else if constexpr (J == OwnerIndex)
		return isUniquelyOwned<T, ExecutionOrder, OwnerIndex, J + 1>();
	else if constexpr (isInTuple<T, ComputeFullOwnedTypes<std::tuple_element_t<J, ExecutionOrder>>>)
		return Violation{ ViolationKind::OwnershipOverlap, OwnerIndex, J, 0 };
	else
		return isUniquelyOwned<T, ExecutionOrder, OwnerIndex, J + 1>();
}

template <typename ExecutionOrder, std::size_t I, typename OwnedTuple, std::size_t K, std::size_t N>
consteval Violation allTypesUniquelyOwnedLoop()
{
	if constexpr (K >= N)
		return Violation{};
	else
	{
		Violation result = isUniquelyOwned<std::tuple_element_t<K, OwnedTuple>, ExecutionOrder, I>();
		if (result.kind != ViolationKind::None)
		{
			// Store the typeIdx within the owned tuple.
			result.typeIdx = K;
			return result;
		}
		return allTypesUniquelyOwnedLoop<ExecutionOrder, I, OwnedTuple, K + 1, N>();
	}
}

template <typename ExecutionOrder, std::size_t I, typename OwnedTuple, std::size_t... Ks>
consteval Violation allTypesUniquelyOwned(std::index_sequence<Ks...>)
{
	return allTypesUniquelyOwnedLoop<ExecutionOrder, I, OwnedTuple, 0, sizeof...(Ks)>();
}

template <typename ExecutionOrder, std::size_t I>
consteval Violation validateOwnershipForOneDeps()
{
	using Deps = std::tuple_element_t<I, ExecutionOrder>;
	using Owned = ComputeFullOwnedTypes<Deps>;
	return allTypesUniquelyOwned<ExecutionOrder, I, Owned>(
		std::make_index_sequence<std::tuple_size_v<Owned>>{});
}

template <typename ExecutionOrder, std::size_t I, std::size_t N>
consteval Violation noOwnershipOverlapLoop()
{
	if constexpr (I >= N)
		return Violation{};
	else
	{
		Violation result = validateOwnershipForOneDeps<ExecutionOrder, I>();
		if (result.kind != ViolationKind::None)
			return result;
		return noOwnershipOverlapLoop<ExecutionOrder, I + 1, N>();
	}
}

template <typename ExecutionOrder, std::size_t... Is>
consteval Violation noOwnershipOverlap(std::index_sequence<Is...>)
{
	return noOwnershipOverlapLoop<ExecutionOrder, 0, sizeof...(Is)>();
}

template <typename ExecutionOrder>
consteval Violation findFirstViolation()
{
	auto v = noOwnershipOverlap<ExecutionOrder>(
		std::make_index_sequence<std::tuple_size_v<ExecutionOrder>>{});
	if (v.kind != ViolationKind::None)
		return v;
	return validateAllDeps<ExecutionOrder>(
		std::make_index_sequence<std::tuple_size_v<ExecutionOrder>>{});
}

template <typename ExecutionOrder>
consteval bool validateExecutionOrder()
{
	return findFirstViolation<ExecutionOrder>().kind == ViolationKind::None;
}

// ---------------------------------------------------------------------------
// Phase 2 — DecodeViolation maps Violation indices back to types
// ---------------------------------------------------------------------------

template <ViolationKind Kind, typename Type, typename SimADeps, typename SimBDeps = void>
struct ExecutionOrderError;

template <typename Type, typename SimADeps, typename SimBDeps>
struct ExecutionOrderError<ViolationKind::OwnershipOverlap, Type, SimADeps, SimBDeps>
{
	static_assert(always_false<Type>,
		"OWNERSHIP OVERLAP: type is owned by both simulations "
		"(see template arguments in the note below).");
};

template <typename Type, typename SimADeps>
struct ExecutionOrderError<ViolationKind::UnownedExternalRef, Type, SimADeps, void>
{
	static_assert(always_false<Type>,
		"UNOWNED EXTERNAL REF: type in External::Types is not owned by any simulation "
		"(see template arguments in the note below).");
};

template <typename Type, typename SimADeps, typename SimBDeps>
struct ExecutionOrderError<ViolationKind::ExecutionOrderViolation, Type, SimADeps, SimBDeps>
{
	static_assert(always_false<Type>,
		"EXECUTION ORDER VIOLATION: declared order violates dependency constraint "
		"(see template arguments in the note below).");
};

template <typename ExecutionOrder, Violation V>
struct DecodeViolation {};

template <typename ExecutionOrder, Violation V>
	requires (V.kind == ViolationKind::OwnershipOverlap)
struct DecodeViolation<ExecutionOrder, V> : ExecutionOrderError<
	ViolationKind::OwnershipOverlap,
	std::tuple_element_t<V.typeIdx,
		ComputeFullOwnedTypes<std::tuple_element_t<V.simA, ExecutionOrder>>>,
	std::tuple_element_t<V.simA, ExecutionOrder>,
	std::tuple_element_t<V.simB, ExecutionOrder>
> {};

template <typename ExecutionOrder, Violation V>
	requires (V.kind == ViolationKind::UnownedExternalRef)
struct DecodeViolation<ExecutionOrder, V> : ExecutionOrderError<
	ViolationKind::UnownedExternalRef,
	std::remove_const_t<std::remove_reference_t<
		std::tuple_element_t<V.typeIdx,
			typename std::tuple_element_t<V.simA, ExecutionOrder>::External::Types>>>,
	std::tuple_element_t<V.simA, ExecutionOrder>
> {};

template <typename ExecutionOrder, Violation V>
	requires (V.kind == ViolationKind::ExecutionOrderViolation)
struct DecodeViolation<ExecutionOrder, V> : ExecutionOrderError<
	ViolationKind::ExecutionOrderViolation,
	std::remove_const_t<std::remove_reference_t<
		std::tuple_element_t<V.typeIdx,
			typename std::tuple_element_t<V.simA, ExecutionOrder>::External::Types>>>,
	std::tuple_element_t<V.simA, ExecutionOrder>,
	std::tuple_element_t<V.simB, ExecutionOrder>
> {};

} // namespace compositeDetail

template <typename ExecutionOrder>
concept ValidExecutionOrder = compositeDetail::validateExecutionOrder<ExecutionOrder>();
