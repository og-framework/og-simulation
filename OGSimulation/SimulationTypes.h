#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationTypes-rationale.md

#include <cmath>
#include <concepts>

template <typename T>
using PlainType = std::remove_cv_t<std::remove_reference_t<T>>;

inline constexpr float kDefaultSimilarityEpsilon = 0.0001f;

namespace simulationTypesDetail {

template <typename T>
concept GlmFloatType = requires {
	typename T::value_type;
	T::length();
} && std::floating_point<typename T::value_type>;

} // namespace simulationTypesDetail

template <typename T>
bool isSimilarToField(const T& a, const T& b)
{
	static_assert(!simulationTypesDetail::GlmFloatType<T>,
		"isSimilarToField: a glm float vector, quaternion or matrix reached the generic exact compare. Its epsilon "
		"overload is in namespace glm in OGSimulation/SimulationComparisonGlm.h, found by ADL: include that header, or "
		"add the overload there for a new glm type. Was: vec3/vec2/quat fields compared exactly (og-input-latency task "
		"2e); glm::mat fields passed this check and compared exactly (task 2g)");
	if constexpr (std::floating_point<T>)
		return std::abs(a - b) < kDefaultSimilarityEpsilon;
	else
		return a == b;
}
