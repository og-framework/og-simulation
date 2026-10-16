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
// can change rotation bits. Optional so that a PhysicsBodyAdapter model without it (the test
// mocks) compiles unchanged; an adapter that implements it should
// static_assert it, so a signature slip fails the build instead of silently falling back.
template <typename T>
concept PhysicsBodyPoseWriter = requires(T adapter, BodyId bodyId,
                                         const glm::vec3& position, const glm::quat& rotation)
{
	{ adapter.setBodyPose(bodyId, position, rotation) };
};

// OPTIONAL capability, deliberately NOT part of PhysicsBodyAdapter: read the state the body's NEXT
// step will start integrating from. An adapter needs it only when captureBodyState reads a
// different phase. SimulationIntegrationExecutor's restore push reads the restored body through it
// when the adapter models it (`if constexpr`), and through captureBodyState otherwise; the push
// runs between a rollback and the first replayed step, so it needs the pose the replay starts
// from. Example: the Chaos adapter's captureBodyState reads the SOLVED pose (P/Q) that its
// post-solve capture needs, while a Chaos rewind restores X/R/V/W and leaves P/Q stale; its
// captureStartOfStepBodyState reads X/R/V/W. Same static_assert advice as PhysicsBodyPoseWriter.
template <typename T>
concept PhysicsBodyStartOfStepReader = requires(const T adapter, BodyId bodyId)
{
	{ adapter.captureStartOfStepBodyState(bodyId) } -> std::convertible_to<PhysicsBodyState>;
};
