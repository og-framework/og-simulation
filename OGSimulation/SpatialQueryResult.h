#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "glm/vec3.hpp"
#include <vector>
#include <cstdint>
#include "OGSimulation/BodyId.h"
#include "OGSimulation/QueryGeometry.h"

// Engine-independent query result — one per overlapping object.
//
// Two-level identity (see current_state.md §D10):
//   • bodyId     — the SHAPE body reported by the overlap. Use for shape-level physics
//                  access (e.g. physics.getBodyTransform(hit.bodyId) to read a guard
//                  sphere's aim-facing rotation).
//   • rootBodyId — the root of the body hierarchy the shape body belongs to. Equal to
//                  bodyId when the shape body is standalone (projectile bodies,
//                  environment). For character shapes (hurtbox, guard), this is the
//                  character capsule's body id — the id that identifies the whole
//                  character regardless of which shape was actually hit. This is what
//                  actor-hit mergers, cross-character hit routing, and projectile
//                  hitRootBodyId use. Populated by the query adapter from a shape→root
//                  map built at registerShape time; falls back to bodyId when no parent
//                  was registered for the shape body.
struct SpatialQueryHit
{
	glm::vec3 objectPosition{0.f};           // world position of colliding object
	BodyId bodyId;                           // SHAPE body reported by the overlap
	BodyId rootBodyId;                       // root of the body hierarchy (== bodyId for standalone)
	CollisionCategories objectCategories;    // which collision categories this object belongs to
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
