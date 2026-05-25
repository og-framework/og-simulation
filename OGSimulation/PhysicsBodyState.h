#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"
#include "OGSimulation/SimulationSerialization.h"
#include "OGSimulation/SimulationFieldDescriptors.h"
#include "OGSimulation/SimulationComparisonGlm.h"

// Pure wire-shape — every field is serialized and compared. The body identifier
// is NOT stored here; it lives in DerivedState's RuntimeBindings, which is
// local-only and immune to correction-state overwrites.
struct PhysicsBodyState
{
	glm::vec3 position        {0.f};                // 12 bytes
	glm::quat rotation        {1.f, 0.f, 0.f, 0.f}; // 16 bytes
	glm::vec3 linearVelocity  {0.f};                // 12 bytes
	glm::vec3 angularVelocity {0.f};                // 12 bytes
	// Wire size: 52 bytes
};

template <>
struct SerializableFields<PhysicsBodyState>
{
	static constexpr auto get()
	{
		return std::make_tuple(
			SIM_MEMBER(PhysicsBodyState, position),
			SIM_MEMBER(PhysicsBodyState, rotation),
			SIM_MEMBER(PhysicsBodyState, linearVelocity),
			SIM_MEMBER(PhysicsBodyState, angularVelocity));
	}
};
