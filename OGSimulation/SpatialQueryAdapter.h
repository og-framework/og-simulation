#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/QueryGeometry.h"
#include "OGSimulation/SpatialQueryResult.h"
#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"
#include <concepts>
#include <vector>

template <typename T>
concept SpatialQueryAdapter = requires(
	T adapter,
	const T cadapter,
	QueryVolumeId volumeId,
	ShapeId shapeId,
	const glm::mat4& transform,
	const glm::vec3& delta,
	const std::vector<QueryVolumeId>& volumeIds)
{
	// Run an overlap query against a set of registered volumes.
	{ adapter.overlap(volumeIds) } -> std::convertible_to<SpatialQueryReport>;

	// Sweep the registered volume from world pose `transform` (its `offsetTransform`
	// applied on top, as `overlap` does) through displacement `delta`; returns the
	// NEAREST BLOCKING hit only. Does not read or write the volume's stored parent
	// transform -- the pose is an argument -- so a mover can issue several sub-sweeps
	// per tick (wall-slide iterations) without setVolumeParentTransform churn.
	{ adapter.sweep(volumeId, transform, delta) } -> std::convertible_to<SweepHit>;

	// Update the parent transform of a registered query volume.
	{ adapter.setVolumeParentTransform(volumeId, transform) };

	// Enable/disable a registered shape (for guard shield toggling).
	{ adapter.enableShape(shapeId) };
	{ adapter.disableShape(shapeId) };
};
