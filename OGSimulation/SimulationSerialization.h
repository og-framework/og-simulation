#pragma once
// SPDX-License-Identifier: MPL-2.0

// [Task 61] SimulationSerialization — SerializableFields, Serializable concept,
// syncSize, write/read/fieldwiseIsSimilarTo, and compositeDetail dispatch helpers.

#include "OGSimulation/SimulationTypes.h"

#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// [Task 47 / Task 50] SerializableFields<T> primary template — intentionally undefined.
// Specialize to expose a static constexpr auto get() returning a std::tuple
// of field descriptors describing all serialized fields.
//
// Supported field types:
//   - Scalar types (float, bool, uint32, etc.)         — works out of the box.
//   - std::array<T, N> where T is trivially copyable   — works: sizeof gives
//     the full array size, operator== compares elements, raw byte-copy is safe.
//   - glm::vec2, glm::vec3                             — works: trivially copyable,
//     isSimilarToField overloads in SimulationComparisonGlm.h.
//
// Nested Serializable aggregates (Task 51):
//   - When MemberFieldDesc<&Outer::inner>::Value is itself Serializable,
//     Size becomes syncSize<Inner>() and serialization recurses field-by-field.
//     Ordering constraint: SerializableFields<Inner> must be specialized
//     before SerializableFields<Outer>.
// Variable-length fields (Task 52):
//   - Use VectorMemberFieldDesc<&Class::vec, MaxCount> for std::vector members.
//     Fixed-max-capacity wire layout: [uint32 count][Element × MaxCount].
// ---------------------------------------------------------------------------

template <typename T>
struct SerializableFields; // No body — must be specialized.

// ---------------------------------------------------------------------------
// [Task 47 / Task 50] Serializable concept — T has a SerializableFields<T> specialization.
// ---------------------------------------------------------------------------

template <typename T>
concept Serializable = requires { { SerializableFields<T>::get() }; };

// ---------------------------------------------------------------------------
// [Task 47 / Task 50] Internal: compile-time sum of field sizes from a tuple type.
// ---------------------------------------------------------------------------

namespace serializationDetail {

template <typename Tuple, std::size_t N>
struct TupleSizeSum
{
	static constexpr std::uint32_t value =
		std::decay_t<decltype(std::get<N - 1>(std::declval<Tuple>()))>::Size
		+ TupleSizeSum<Tuple, N - 1>::value;
};

template <typename Tuple>
struct TupleSizeSum<Tuple, 0>
{
	static constexpr std::uint32_t value = 0;
};

// NDK clang 27.2 crashes (ICE) when instantiating std::apply with IIFE-in-pack-expansion
// containing if constexpr recursive calls.  Replace std::apply with an index_sequence helper
// that accesses each field descriptor type directly via std::tuple_element_t.
// The field descriptor types are stateless so no tuple instance is needed.
template <typename T, typename F, std::size_t... Is>
void forEachFieldImpl(F&& f, std::index_sequence<Is...>)
{
	using Fields = decltype(SerializableFields<T>::get());
	// Call f with a default-constructed (stateless) instance of each field type.
	(f(std::tuple_element_t<Is, Fields>{}), ...);
}

template <typename T, typename F>
void forEachField(F&& f)
{
	using Fields = decltype(SerializableFields<T>::get());
	forEachFieldImpl<T>(std::forward<F>(f), std::make_index_sequence<std::tuple_size_v<Fields>>{});
}

} // namespace serializationDetail

// ---------------------------------------------------------------------------
// [Task 47 / Task 50] syncSize<T>() — compile-time sum of all field sizes.
// ---------------------------------------------------------------------------

template <typename T>
constexpr std::uint32_t syncSize()
{
	using Fields = decltype(SerializableFields<T>::get());
	return serializationDetail::TupleSizeSum<Fields, std::tuple_size_v<Fields>>::value;
}

// ---------------------------------------------------------------------------
// [Task 48 / Task 50] writeToSyncedBuffer — serialize all fields.
// (Moved here from AutoSerialization.h / SimulationFieldDescriptors.h.)
// ---------------------------------------------------------------------------

template <typename T, typename SyncedBuffer>
std::uint32_t writeToSyncedBuffer(const T& obj, SyncedBuffer& buffer, std::uint32_t offset)
{
	std::uint32_t off = offset;
	serializationDetail::forEachField<T>([&]<typename FD>(FD) {
		// [Task 51/52] 3-tier dispatch per field:
		//   Tier 1: descriptor has serializeToBuffer (e.g. VectorMemberFieldDesc)
		//   Tier 2: Value is Serializable — recurse field-by-field
		//   Tier 3: scalar/trivially-copyable — raw writeToBuffer
		using V = typename FD::Value;
		if constexpr (requires { FD::serializeToBuffer(obj, buffer, off); })
			FD::serializeToBuffer(obj, buffer, off);
		else if constexpr (Serializable<V>)
			writeToSyncedBuffer(FD::read(obj), buffer, off);
		else
			buffer.writeToBuffer(off, FD::read(obj));
		off += FD::Size;
	});
	return off - offset;
}

// ---------------------------------------------------------------------------
// [Task 48 / Task 50] readFromSyncedBuffer — deserialize all fields.
// (Moved here from AutoSerialization.h / SimulationFieldDescriptors.h.)
// ---------------------------------------------------------------------------

template <typename T, typename SyncedBuffer>
void readFromSyncedBuffer(T& obj, const SyncedBuffer& buffer, std::uint32_t offset)
{
	std::uint32_t off = offset;
	serializationDetail::forEachField<T>([&]<typename FD>(FD) {
		// [Task 51/52] 3-tier dispatch per field (mirrors writeToSyncedBuffer).
		using V = typename FD::Value;
		if constexpr (requires { FD::deserializeFromBuffer(obj, buffer, off); })
			FD::deserializeFromBuffer(obj, buffer, off);
		else if constexpr (Serializable<V>) {
			V tmp{};
			readFromSyncedBuffer(tmp, buffer, off);
			FD::write(obj, std::move(tmp));
		} else {
			FD::write(obj, buffer.template readFromBuffer<V>(off));
		}
		off += FD::Size;
	});
}

// ---------------------------------------------------------------------------
// [Task 48 / Task 50] fieldwiseIsSimilarTo<T> — compare field by field.
// (Moved here from AutoSerialization.h / SimulationFieldDescriptors.h.)
// Note: unconstrained so it works in class bodies before SerializableFields is declared.
// ---------------------------------------------------------------------------

template <typename T>
bool fieldwiseIsSimilarTo(const T& a, const T& b)
{
	bool result = true;
	serializationDetail::forEachField<T>([&]<typename FD>(FD) {
		if (!result) return;
		// [Task 51] Recurse for Serializable<V> fields; use isSimilarToField otherwise.
		using V = typename FD::Value;
		if constexpr (Serializable<V>)
			result = fieldwiseIsSimilarTo(FD::read(a), FD::read(b));
		else
			result = isSimilarToField(FD::read(a), FD::read(b));
	});
	return result;
}

// ---------------------------------------------------------------------------
// [Task 51] isSimilarToField overload for Serializable types.
// Preferred over the unconstrained template by C++20 partial ordering.
// Placed after fieldwiseIsSimilarTo since it calls it.
// ---------------------------------------------------------------------------

template <typename T>
	requires Serializable<T>
bool isSimilarToField(const T& a, const T& b)
{
	return fieldwiseIsSimilarTo(a, b);
}

// ---------------------------------------------------------------------------
// [Task 52] isSimilarToField overload for std::vector<T>.
// Compares element-wise via isSimilarToField — floats get epsilon compare,
// Serializable elements get fieldwiseIsSimilarTo via the overload above.
// ---------------------------------------------------------------------------

template <typename T>
bool isSimilarToField(const std::vector<T>& a, const std::vector<T>& b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (!isSimilarToField(a[i], b[i])) return false;
	return true;
}

// ---------------------------------------------------------------------------
// [Task 48] compositeDetail — dispatch helpers with if constexpr customization points.
// write/read: fall back to writeToSyncedBuffer/readFromSyncedBuffer unless a
//             customWriteToSyncedBuffer/customReadFromSyncedBuffer ADL override exists.
// compare:    prefer member isSimilarTo if defined, else fieldwiseIsSimilarTo.
// ---------------------------------------------------------------------------

namespace compositeDetail {

template <typename T, typename SyncedBuffer>
std::uint32_t writeElement(const T& obj, SyncedBuffer& buffer, std::uint32_t offset)
{
	if constexpr (requires { customWriteToSyncedBuffer(obj, buffer, offset); })
		return static_cast<std::uint32_t>(customWriteToSyncedBuffer(obj, buffer, offset));
	else
		return writeToSyncedBuffer(obj, buffer, offset);
}

template <typename T, typename SyncedBuffer>
void readElement(T& obj, const SyncedBuffer& buffer, std::uint32_t offset)
{
	if constexpr (requires { customReadFromSyncedBuffer(obj, buffer, offset); })
		customReadFromSyncedBuffer(obj, buffer, offset);
	else
		readFromSyncedBuffer(obj, buffer, offset);
}

template <typename T>
bool compareElement(const T& a, const T& b)
{
	if constexpr (requires { { a.isSimilarTo(b) } -> std::convertible_to<bool>; })
		return a.isSimilarTo(b);
	else
		return fieldwiseIsSimilarTo(a, b);
}

} // namespace compositeDetail
