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
	{ adapter.addBodyAcceleration(bodyId, vec) };     // accumulate an acceleration for THIS step; engine applies force = a * mass
	{ adapter.addBodyVelocityChange(bodyId, vec) };   // instantaneous dv before this step's solve; engine applies impulse = dv * mass
	{ cadapter.getBodyInertiaTensor(bodyId) } -> std::convertible_to<glm::vec3>;
	{ cadapter.captureBodyState(bodyId) }     -> std::convertible_to<PhysicsBodyState>;
};

// OPTIONAL capability, deliberately NOT part of PhysicsBodyAdapter: write a body's pose from a
// position and a quaternion, with no matrix in between. SimulationIntegrationExecutor's restore
// push uses it when the adapter models it (`if constexpr`) and otherwise keeps the
// setBodyTransform matrix path, which costs a quaternion -> matrix -> quaternion round trip and so
// can change rotation bits. Optional so that every existing PhysicsBodyAdapter model (the Chaos
// adapter and the test mocks) compiles unchanged; an adapter that implements it should
// static_assert it, so a signature slip fails the build instead of silently falling back.
template <typename T>
concept PhysicsBodyPoseWriter = requires(T adapter, BodyId bodyId,
                                         const glm::vec3& position, const glm::quat& rotation)
{
	{ adapter.setBodyPose(bodyId, position, rotation) };
};
