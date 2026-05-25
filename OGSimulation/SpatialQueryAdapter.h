#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/QueryGeometry.h"
#include "OGSimulation/SpatialQueryResult.h"
#include "glm/mat4x4.hpp"
#include <concepts>
#include <vector>

template <typename T>
concept SpatialQueryAdapter = requires(
	T adapter,
	const T cadapter,
	QueryVolumeId volumeId,
	ShapeId shapeId,
	const glm::mat4& transform,
	const std::vector<QueryVolumeId>& volumeIds)
{
	// Run an overlap query against a set of registered volumes.
	{ adapter.overlap(volumeIds) } -> std::convertible_to<SpatialQueryReport>;

	// Update the parent transform of a registered query volume.
	{ adapter.setVolumeParentTransform(volumeId, transform) };

	// Enable/disable a registered shape (for guard shield toggling).
	{ adapter.enableShape(shapeId) };
	{ adapter.disableShape(shapeId) };
};
