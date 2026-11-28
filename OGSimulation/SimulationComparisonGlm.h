#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationComparisonGlm-rationale.md

#include <algorithm>

#include "OGSimulation/SimulationTypes.h"
#include "glm/vec3.hpp"
#include "glm/vec2.hpp"
#include "glm/gtc/quaternion.hpp"

namespace glm {

inline bool isSimilarToField(const glm::vec3& a, const glm::vec3& b)
{
	return std::abs(a.x - b.x) < kDefaultSimilarityEpsilon
		&& std::abs(a.y - b.y) < kDefaultSimilarityEpsilon
		&& std::abs(a.z - b.z) < kDefaultSimilarityEpsilon;
}

inline bool isSimilarToField(const glm::vec2& a, const glm::vec2& b)
{
	return std::abs(a.x - b.x) < kDefaultSimilarityEpsilon
		&& std::abs(a.y - b.y) < kDefaultSimilarityEpsilon;
}

inline glm::quat similaritySignAligned(const glm::quat& a, const glm::quat& b)
{
	return glm::dot(a, b) < 0.f ? -b : b;
}

inline bool isSimilarToField(const glm::quat& a, const glm::quat& b)
{
	const glm::quat aligned = similaritySignAligned(a, b);
	return std::abs(a.x - aligned.x) < kDefaultSimilarityEpsilon
		&& std::abs(a.y - aligned.y) < kDefaultSimilarityEpsilon
		&& std::abs(a.z - aligned.z) < kDefaultSimilarityEpsilon
		&& std::abs(a.w - aligned.w) < kDefaultSimilarityEpsilon;
}

inline double similarityRatioField(const glm::vec3& a, const glm::vec3& b)
{
	const float largest = std::max(std::abs(a.x - b.x), std::max(std::abs(a.y - b.y), std::abs(a.z - b.z)));
	return static_cast<double>(largest) / static_cast<double>(kDefaultSimilarityEpsilon);
}

inline double similarityRatioField(const glm::vec2& a, const glm::vec2& b)
{
	const float largest = std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
	return static_cast<double>(largest) / static_cast<double>(kDefaultSimilarityEpsilon);
}

inline double similarityRatioField(const glm::quat& a, const glm::quat& b)
{
	const glm::quat aligned = similaritySignAligned(a, b);
	const float largest = std::max(std::max(std::abs(a.x - aligned.x), std::abs(a.y - aligned.y)),
		std::max(std::abs(a.z - aligned.z), std::abs(a.w - aligned.w)));
	return static_cast<double>(largest) / static_cast<double>(kDefaultSimilarityEpsilon);
}

} // namespace glm
