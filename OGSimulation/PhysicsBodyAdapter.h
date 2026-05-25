#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include "PhysicsBodyState.h"
#include "BodyId.h"
#include "glm/glm.hpp"

template <typename T>
concept PhysicsBodyAdapter = requires(
	T adapter, const T cadapter,
	BodyId bodyId,
	const glm::mat4& transform,
	const glm::vec3& vec)
{
	{ cadapter.getBodyTransform(bodyId) }     -> std::convertible_to<glm::mat4>;
	{ adapter.setBodyTransform(bodyId, transform) };
	{ adapter.addBodyTorque(bodyId, vec) };
	{ adapter.setBodyAngularVelocity(bodyId, vec) };
	{ adapter.setBodyLinearVelocity(bodyId, vec) };
	{ cadapter.getBodyInertiaTensor(bodyId) } -> std::convertible_to<glm::vec3>;
	{ cadapter.captureBodyState(bodyId) }     -> std::convertible_to<PhysicsBodyState>;
};
