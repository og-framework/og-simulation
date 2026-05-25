#pragma once
// SPDX-License-Identifier: MPL-2.0

// [Task 35] GLM overloads for isSimilarToField.
// Kept in a separate header so that SimulationComposite.h stays GLM-free.

#include "OGSimulation/SimulationTypes.h"
#include "glm/vec3.hpp"
#include "glm/vec2.hpp"
#include "glm/gtc/quaternion.hpp"

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

inline bool isSimilarToField(const glm::quat& a, const glm::quat& b)
{
	const float dot = glm::dot(a, b);
	return std::abs(std::abs(dot) - 1.f) < kDefaultSimilarityEpsilon;
}
