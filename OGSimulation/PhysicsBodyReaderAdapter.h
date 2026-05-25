#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include "PhysicsBodyState.h"
#include "BodyId.h"
#include "glm/glm.hpp"

// Read-only adapter for body state queries.
// Safe for game-thread visualization use — no write operations.
// Complements PhysicsBodyAdapter (physics-thread, read+write).
template <typename T>
concept PhysicsBodyReaderAdapter = requires(const T cadapter, BodyId bodyId)
{
	{ cadapter.getBodyTransform(bodyId) }     -> std::convertible_to<glm::mat4>;
	{ cadapter.getBodyInertiaTensor(bodyId) }  -> std::convertible_to<glm::vec3>;
	{ cadapter.captureBodyState(bodyId) }      -> std::convertible_to<PhysicsBodyState>;
};
