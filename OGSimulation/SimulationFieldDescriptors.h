#pragma once
// SPDX-License-Identifier: MPL-2.0
#include <algorithm>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#include "OGSimulation/SimulationSerialization.h"

// ---------------------------------------------------------------------------
// SimulationFieldDescriptors — field-descriptor types for
// SerializableFields<T> specializations.
//
// Usage summary:
//  1. Define SerializableFields<MyType> specialization with a static constexpr
//     get() returning std::make_tuple(SIM_MEMBER(MyType, member), ...).
//  2. writeToSyncedBuffer / readFromSyncedBuffer / fieldwiseIsSimilarTo
//     in SimulationComposite.h handle serialization automatically.
//
// Rationale: docs/SimulationComparison-rationale.md §3 — this file has no rationale
// doc of its own, and every `§3` below is a section of THAT document.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Internal trait helpers
// ---------------------------------------------------------------------------

namespace serializationDetail {

template <typename T> struct GetterTraits;

template <typename Ret, typename Class>
struct GetterTraits<Ret (Class::*)() const>
{
	using ClassType = Class;
	using ValueType = std::decay_t<Ret>;
};

template <typename T> struct MemberPtrTraits;

template <typename Class, typename Value>
struct MemberPtrTraits<Value Class::*>
{
	using ClassType = Class;
	using ValueType = std::remove_cvref_t<Value>;
};

} // namespace serializationDetail

// ---------------------------------------------------------------------------
// FieldDesc<Getter, Setter> — describes a readable+writable field.
// Getter must be a const member function returning Value.
// Setter must be a member function taking Value (or const Value&).
// ---------------------------------------------------------------------------

template <auto Getter, auto Setter>
struct FieldDesc
{
	using Owner = typename serializationDetail::GetterTraits<decltype(Getter)>::ClassType;
	using Value = typename serializationDetail::GetterTraits<decltype(Getter)>::ValueType;

	// Use syncSize when Value is a nested Serializable aggregate.
	static constexpr std::uint32_t Size = []() -> std::uint32_t {
		if constexpr (Serializable<Value>)
			return syncSize<Value>();
		else
			return static_cast<std::uint32_t>(sizeof(Value));
	}();

	static Value read(const Owner& obj) { return (obj.*Getter)(); }
	static void write(Owner& obj, Value v) { (obj.*Setter)(v); }

	// ⛔ A NON-STATIC MEMBER, NEVER A TEMPLATE PARAMETER. Making the spelling a
	// second NTTP would change the DESCRIPTOR'S TYPE, and a dozen APPEND-ONLY wire
	// fences across the games pin `decltype(SerializableFields<S>::get())` against
	// an exact `std::tuple<MemberFieldDesc<&S::f>, ...>`. Every one of them would
	// fire. As a member the type is byte-for-byte the same one they name. §3
	const char* name = nullptr;
};

// ---------------------------------------------------------------------------
// FieldDescReadOnly<Getter> — describes a readable-only field (inputs).
// ---------------------------------------------------------------------------

template <auto Getter>
struct FieldDescReadOnly
{
	using Owner = typename serializationDetail::GetterTraits<decltype(Getter)>::ClassType;
	using Value = typename serializationDetail::GetterTraits<decltype(Getter)>::ValueType;

	// Use syncSize when Value is a nested Serializable aggregate.
	static constexpr std::uint32_t Size = []() -> std::uint32_t {
		if constexpr (Serializable<Value>)
			return syncSize<Value>();
		else
			return static_cast<std::uint32_t>(sizeof(Value));
	}();

	static Value read(const Owner& obj) { return (obj.*Getter)(); }

	// See FieldDesc::name — a member, not a template parameter, and for the same reason.
	const char* name = nullptr;
};

// ---------------------------------------------------------------------------
// Convenience macros
// ---------------------------------------------------------------------------

// THE FIELD SPELLING IS CARRIED BY THE MACRO, AND ONLY BY THE MACRO. §3
//
// ⛔ IT CANNOT BE RECOVERED FROM THE MEMBER POINTER. MEASURED on this tree's
// toolchain (MSVC 14.38.33130, the version UBT selects): `__FUNCSIG__` for a
// function template taking a pointer-to-member NTTP prints
// `sigOf<pointer-to-member(0x0)>` — the SPELLING IS GONE. The `nameof` trick that
// works for TYPES (`tsigOf<struct dAttackRadialSimulation::State>`, which
// SimulationComparison.h does use) does not work for MEMBERS here.
//
// ⚠ sigOf AND tsigOf ARE NOT IN THIS TREE. They were throwaway probe
// templates, never committed; §3 writes both of them out beside the output quoted
// above, and that is the only place they exist. Do not go looking for them here.
//
// ⛔ SO A DESCRIPTOR WRITTEN AS A BARE TYPE — `MemberFieldDesc<&S::f>{}`, which
// four sub-simulations still use — HAS NO NAME, by construction, and the
// divergence walk falls back to `#<index>`. That is the design's honest edge, not
// a defect to patch by editing those call sites.
//
// SIM_FIELD(Type, getter, setter) — creates a FieldDesc instance (read+write).
#define SIM_FIELD(Type, getter, setter) FieldDesc<&Type::getter, &Type::setter>{#getter}

// SIM_FIELD_RO(Type, getter) — creates a FieldDescReadOnly instance (read only).
#define SIM_FIELD_RO(Type, getter) FieldDescReadOnly<&Type::getter>{#getter}

// ---------------------------------------------------------------------------
// MemberFieldDesc<MemberPtr> — describes a plain aggregate data member.
// Works with any non-const data member pointer.
//
// Value types that work:
//   - Scalar (float, bool, uint32, …)                — Size = sizeof(T), safe.
//   - std::array<T, N> (trivially copyable T)        — Size = sizeof(array),
//     raw byte-copy round-trips correctly.
//   - glm::vec2 / glm::vec3                          — trivially copyable.
//   - Nested Serializable aggregate                  — Size = syncSize<Value>(),
//     serialized field-by-field via recursion. Ordering constraint: specialize
//     SerializableFields<Inner> before SerializableFields<Outer>.
//
// For std::vector members, use VectorMemberFieldDesc instead.
// ---------------------------------------------------------------------------

template <auto MemberPtr>
struct MemberFieldDesc
{
	using Owner = typename serializationDetail::MemberPtrTraits<decltype(MemberPtr)>::ClassType;
	using Value = typename serializationDetail::MemberPtrTraits<decltype(MemberPtr)>::ValueType;

	// Use syncSize when Value is a nested Serializable aggregate.
	static constexpr std::uint32_t Size = []() -> std::uint32_t {
		if constexpr (Serializable<Value>)
			return syncSize<Value>();
		else
			return static_cast<std::uint32_t>(sizeof(Value));
	}();

	static Value read(const Owner& obj) { return obj.*MemberPtr; }
	static void write(Owner& obj, Value v) { obj.*MemberPtr = std::move(v); }

	// The member's SPELLING, for `describeFirstDivergingField`'s path. See FieldDesc::name.
	//
	// ⛔ `nullptr` IS A REACHABLE VALUE, not a "can't happen": `forEachField`
	// default-constructs each descriptor (`std::tuple_element_t<Is, Fields>{}`) and
	// four sub-simulations spell their descriptors as bare types. Every reader must
	// have an index fallback. §3
	const char* name = nullptr;
};

// SIM_MEMBER(Class, member) — creates a MemberFieldDesc instance for a plain data member.
#define SIM_MEMBER(Class, member) MemberFieldDesc<&Class::member>{#member}

// ---------------------------------------------------------------------------
// VectorMemberFieldDesc<MemberPtr, MaxCount> — fixed-max-capacity std::vector
// serialization descriptor.
//
// Wire layout: [uint32_t count][Element × MaxCount]
//   Size is a compile-time constant = sizeof(uint32_t) + MaxCount * ElementSize.
//
// Truncation: if vec.size() > MaxCount, only the first MaxCount elements are
// written. The remaining wire slots are zero-filled for deterministic output.
// This is a known lossy operation — callers must ensure vec.size() ≤ MaxCount.
//
// Element types that work:
//   - Trivially copyable scalars (float, int, …) — raw writeToBuffer per element.
//   - Serializable aggregates            — recursive writeToSyncedBuffer per element.
// ---------------------------------------------------------------------------

template <auto MemberPtr, std::uint32_t MaxCount>
struct VectorMemberFieldDesc
{
	using Owner = typename serializationDetail::MemberPtrTraits<decltype(MemberPtr)>::ClassType;
	using VectorType = typename serializationDetail::MemberPtrTraits<decltype(MemberPtr)>::ValueType;
	using Element = typename VectorType::value_type;
	using Value = VectorType; // for consistent FD::Value interface

	static constexpr std::uint32_t ElementSize = []() -> std::uint32_t {
		if constexpr (Serializable<Element>)
			return syncSize<Element>();
		else
			return static_cast<std::uint32_t>(sizeof(Element));
	}();

	// Wire layout: [uint32 count][Element × MaxCount]
	static constexpr std::uint32_t Size =
		static_cast<std::uint32_t>(sizeof(std::uint32_t)) + MaxCount * ElementSize;

	static const VectorType& read(const Owner& obj) { return obj.*MemberPtr; }
	static void write(Owner& obj, VectorType v) { obj.*MemberPtr = std::move(v); }

	// See FieldDesc::name.
	const char* name = nullptr;

	// Tier-1 custom serialization — detected by writeToSyncedBuffer via requires.
	template <typename SyncedBuffer>
	static void serializeToBuffer(const Owner& obj, SyncedBuffer& buffer, std::uint32_t offset)
	{
		const VectorType& vec = obj.*MemberPtr;
		const std::uint32_t count = static_cast<std::uint32_t>(
			std::min(static_cast<std::size_t>(MaxCount), vec.size()));
		buffer.writeToBuffer(offset, count);
		std::uint32_t off = offset + static_cast<std::uint32_t>(sizeof(std::uint32_t));
		for (std::uint32_t i = 0; i < count; ++i)
		{
			if constexpr (Serializable<Element>)
				writeToSyncedBuffer(vec[i], buffer, off);
			else
				buffer.writeToBuffer(off, vec[i]);
			off += ElementSize;
		}
		// Zero-fill remaining slots for deterministic wire format.
		const Element zero{};
		for (std::uint32_t i = count; i < MaxCount; ++i)
		{
			if constexpr (Serializable<Element>)
				writeToSyncedBuffer(zero, buffer, off);
			else
				buffer.writeToBuffer(off, zero);
			off += ElementSize;
		}
	}

	// Tier-1 custom deserialization — detected by readFromSyncedBuffer via requires.
	template <typename SyncedBuffer>
	static void deserializeFromBuffer(Owner& obj, const SyncedBuffer& buffer, std::uint32_t offset)
	{
		const std::uint32_t count = buffer.template readFromBuffer<std::uint32_t>(offset);
		const std::uint32_t safeCount = std::min(count, MaxCount);
		std::uint32_t off = offset + static_cast<std::uint32_t>(sizeof(std::uint32_t));
		VectorType& vec = obj.*MemberPtr;
		vec.resize(safeCount);
		for (std::uint32_t i = 0; i < safeCount; ++i)
		{
			if constexpr (Serializable<Element>)
				readFromSyncedBuffer(vec[i], buffer, off);
			else
				vec[i] = buffer.template readFromBuffer<Element>(off);
			off += ElementSize;
		}
	}
};

// SIM_VECTOR(Class, member, MaxCount) — fixed-max-capacity vector descriptor.
#define SIM_VECTOR(Class, member, MaxCount) VectorMemberFieldDesc<&Class::member, MaxCount>{#member}
