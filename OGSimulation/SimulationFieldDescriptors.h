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
// [Task 50] SimulationFieldDescriptors — field-descriptor types for
// SerializableFields<T> specializations.
//
// Usage summary:
//  1. Define SerializableFields<MyType> specialization with a static constexpr
//     get() returning std::make_tuple(SIM_MEMBER(MyType, member), ...).
//  2. writeToSyncedBuffer / readFromSyncedBuffer / fieldwiseIsSimilarTo
//     in SimulationComposite.h handle serialization automatically.
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

	// [Task 51] Use syncSize when Value is a nested Serializable aggregate.
	static constexpr std::uint32_t Size = []() -> std::uint32_t {
		if constexpr (Serializable<Value>)
			return syncSize<Value>();
		else
			return static_cast<std::uint32_t>(sizeof(Value));
	}();

	static Value read(const Owner& obj) { return (obj.*Getter)(); }
	static void write(Owner& obj, Value v) { (obj.*Setter)(v); }
};

// ---------------------------------------------------------------------------
// FieldDescReadOnly<Getter> — describes a readable-only field (inputs).
// ---------------------------------------------------------------------------

template <auto Getter>
struct FieldDescReadOnly
{
	using Owner = typename serializationDetail::GetterTraits<decltype(Getter)>::ClassType;
	using Value = typename serializationDetail::GetterTraits<decltype(Getter)>::ValueType;

	// [Task 51] Use syncSize when Value is a nested Serializable aggregate.
	static constexpr std::uint32_t Size = []() -> std::uint32_t {
		if constexpr (Serializable<Value>)
			return syncSize<Value>();
		else
			return static_cast<std::uint32_t>(sizeof(Value));
	}();

	static Value read(const Owner& obj) { return (obj.*Getter)(); }
};

// ---------------------------------------------------------------------------
// Convenience macros
// ---------------------------------------------------------------------------

// SIM_FIELD(Type, getter, setter) — creates a FieldDesc instance (read+write).
#define SIM_FIELD(Type, getter, setter) FieldDesc<&Type::getter, &Type::setter>{}

// SIM_FIELD_RO(Type, getter) — creates a FieldDescReadOnly instance (read only).
#define SIM_FIELD_RO(Type, getter) FieldDescReadOnly<&Type::getter>{}

// ---------------------------------------------------------------------------
// MemberFieldDesc<MemberPtr> — describes a plain aggregate data member.
// Works with any non-const data member pointer.
//
// Value types that work:
//   - Scalar (float, bool, uint32, …)                — Size = sizeof(T), safe.
//   - std::array<T, N> (trivially copyable T)        — Size = sizeof(array),
//     raw byte-copy round-trips correctly.
//   - glm::vec2 / glm::vec3                          — trivially copyable.
//   - Nested Serializable aggregate (Task 51)        — Size = syncSize<Value>(),
//     serialized field-by-field via recursion. Ordering constraint: specialize
//     SerializableFields<Inner> before SerializableFields<Outer>.
//
// For std::vector members, use VectorMemberFieldDesc (Task 52) instead.
// ---------------------------------------------------------------------------

template <auto MemberPtr>
struct MemberFieldDesc
{
	using Owner = typename serializationDetail::MemberPtrTraits<decltype(MemberPtr)>::ClassType;
	using Value = typename serializationDetail::MemberPtrTraits<decltype(MemberPtr)>::ValueType;

	// [Task 51] Use syncSize when Value is a nested Serializable aggregate.
	static constexpr std::uint32_t Size = []() -> std::uint32_t {
		if constexpr (Serializable<Value>)
			return syncSize<Value>();
		else
			return static_cast<std::uint32_t>(sizeof(Value));
	}();

	static Value read(const Owner& obj) { return obj.*MemberPtr; }
	static void write(Owner& obj, Value v) { obj.*MemberPtr = std::move(v); }
};

// SIM_MEMBER(Class, member) — creates a MemberFieldDesc instance for a plain data member.
#define SIM_MEMBER(Class, member) MemberFieldDesc<&Class::member>{}

// ---------------------------------------------------------------------------
// VectorMemberFieldDesc<MemberPtr, MaxCount> — fixed-max-capacity std::vector
// serialization descriptor (Task 52).
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
#define SIM_VECTOR(Class, member, MaxCount) VectorMemberFieldDesc<&Class::member, MaxCount>{}
