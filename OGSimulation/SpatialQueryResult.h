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

// Engine-independent SWEEP result — the NEAREST BLOCKING hit only (v1), or a miss.
// Produced by SpatialQueryAdapter::sweep.
//
// Field validity — read the two flags FIRST; the rest are conditional on them:
//   • fraction / normal / impactPoint are valid IFF `blocked`. When `blocked` is
//     false the volume travelled the full delta unobstructed, `fraction` is 1, and
//     normal/impactPoint carry nothing.
//   • penetrationDepth together with `normal` is the PUSH-OUT IFF `startPenetrating`
//     — the volume was already overlapping at fraction 0, and displacing it
//     `penetrationDepth` along `normal` separates it. `fraction` is 0 in that case
//     and carries no distance information.
//
// Two-level identity, exactly as SpatialQueryHit above:
//   • bodyId     — the SHAPE body the sweep hit.
//   • rootBodyId — the root of that shape body's hierarchy; equal to bodyId when the
//                  shape body is standalone. This is the id that identifies a whole
//                  character regardless of which of its shapes the sweep struck.
struct SweepHit
{
	bool blocked = false;                    // false => moved the full delta unobstructed
	float fraction = 1.f;                    // [0,1] of delta travelled before contact
	glm::vec3 normal{0.f};                   // surface normal at contact (unit, world)
	glm::vec3 impactPoint{0.f};              // world
	bool startPenetrating = false;           // already overlapping at fraction 0
	float penetrationDepth = 0.f;            // valid iff startPenetrating; push-out along `normal`
	BodyId bodyId;                           // SHAPE body hit
	BodyId rootBodyId;                       // root of the body hierarchy (== bodyId for standalone)
	CollisionCategories objectCategories;    // which collision categories the hit object belongs to
};
