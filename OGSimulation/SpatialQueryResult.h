#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "glm/vec3.hpp"
#include <vector>
#include <cstdint>
#include "OGSimulation/BodyId.h"
#include "OGSimulation/QueryGeometry.h"

// Engine-independent query result — one per overlapping object.
struct SpatialQueryHit
{
	glm::vec3 objectPosition{0.f};           // world position of colliding object
	int objectIndex = 0;                     // instance index of colliding object
	CollisionCategories objectCategories;    // which collision categories this object belongs to
	BodyId bodyId;                           // physics body of the colliding object
};

// Engine-independent query report — collection of hits.
struct SpatialQueryReport
{
	std::vector<SpatialQueryHit> hits;

	bool empty() const { return hits.empty(); }
	size_t size() const { return hits.size(); }
	const SpatialQueryHit& operator[](size_t index) const { return hits[index]; }

	auto begin() const { return hits.begin(); }
	auto end() const { return hits.end(); }
};
