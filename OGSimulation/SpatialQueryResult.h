#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SpatialQueryResult-rationale.md · docs/SpatialQueryResult-guards.md

#include "glm/vec3.hpp"
#include <vector>
#include <cstdint>
#include "OGSimulation/BodyId.h"
#include "OGSimulation/QueryGeometry.h"

struct SpatialQueryHit
{
	glm::vec3 objectPosition{0.f};
	BodyId bodyId;
	BodyId rootBodyId;
	CollisionCategories objectCategories;
};

struct SpatialQueryReport
{
	std::vector<SpatialQueryHit> hits;

	bool empty() const { return hits.empty(); }
	size_t size() const { return hits.size(); }
	const SpatialQueryHit& operator[](size_t index) const { return hits[index]; }

	auto begin() const { return hits.begin(); }
	auto end() const { return hits.end(); }
};

// ⛔G-01  docs/SpatialQueryResult-guards.md
struct SweepHit
{
	bool blocked = false;
	// ⛔G-02  docs/SpatialQueryResult-guards.md
	float fraction = 1.f;
	glm::vec3 normal{0.f};
	glm::vec3 impactPoint{0.f};
	bool startPenetrating = false;
	// ⛔G-03  docs/SpatialQueryResult-guards.md
	float penetrationDepth = 0.f;
	BodyId bodyId;
	BodyId rootBodyId;
	CollisionCategories objectCategories;
};
