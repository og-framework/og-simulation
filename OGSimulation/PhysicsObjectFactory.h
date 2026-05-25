#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/QueryGeometry.h"
#include "OGSimulation/BodyId.h"
#include <concepts>
#include <vector>

template <typename T>
concept PhysicsObjectFactory = requires(
	T factory,
	const PhysicalObjectDescriptor& descriptor,
	const char* name)
{
	// Create an engine physics object from a descriptor.
	// Engine-specific scene context (owner, parent, world) is provided
	// at factory construction time — NOT per call.
	// Return type must expose bodyId and shapeIds members.
	{ factory.createPhysicalObject(descriptor, name).bodyId } -> std::convertible_to<BodyId>;
	{ factory.createPhysicalObject(descriptor, name).shapeIds } -> std::convertible_to<std::vector<ShapeId>>;
};
