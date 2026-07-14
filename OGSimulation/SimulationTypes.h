#pragma once
// SPDX-License-Identifier: MPL-2.0

// SimulationTypes — PlainType alias and base isSimilarToField overloads.
// No project dependencies — only standard library headers.

#include <cmath>
#include <concepts>

// PlainType — strips const and references. Defined once here; all simulation
// headers that previously defined their own copy now include this file instead.
template <typename T>
using PlainType = std::remove_cv_t<std::remove_reference_t<T>>;

// ---------------------------------------------------------------------------
// isSimilarToField — per-field similarity helper
// Float fields use epsilon comparison; all other types use operator==.
// A glm::vec3 overload lives in OGSimulation/SimulationComparisonGlm.h to keep
// GLM out of this header.
//
// Constrained overloads for Serializable<T> and std::vector<T> live in
// SimulationSerialization.h (they depend on fieldwiseIsSimilarTo).
// ---------------------------------------------------------------------------

inline constexpr float kDefaultSimilarityEpsilon = 0.0001f;

template <typename T>
bool isSimilarToField(const T& a, const T& b)
{
	if constexpr (std::floating_point<T>)
		return std::abs(a - b) < kDefaultSimilarityEpsilon;
	else
		return a == b;
}
