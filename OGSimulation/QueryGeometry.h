#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "glm/vec3.hpp"
#include "glm/mat4x4.hpp"
#include <variant>
#include <vector>
#include <cstdint>
#include <compare>

// Handle types

struct QueryVolumeId
{
	uint32_t value = 0;
	bool operator==(const QueryVolumeId&) const = default;
	auto operator<=>(const QueryVolumeId&) const = default;
};

struct ShapeId
{
	uint32_t value = 0;
	bool operator==(const ShapeId&) const = default;
	auto operator<=>(const ShapeId&) const = default;
};

// Geometry primitives

struct SphereGeometry
{
	float radius = 0.f;
};

struct BoxGeometry
{
	glm::vec3 halfExtents{0.f};
};

struct CapsuleGeometry
{
	float radius = 0.f;
	float halfHeight = 0.f;
};

// Tagged union for geometry
using QueryGeometry = std::variant<SphereGeometry, BoxGeometry, CapsuleGeometry>;

// CollisionCategories

// Engine-independent bitmask of collision categories.
// Each bit represents a consumer-local category ID (0, 1, 2, ...) — NOT engine channel numbers.
// Each adapter holds an explicit mapping from consumer category <-> engine channel.
struct CollisionCategories
{
	uint32_t bits = 0;

	constexpr CollisionCategories() = default;
	constexpr explicit CollisionCategories(uint32_t bits) : bits(bits) {}

	static constexpr CollisionCategories single(uint32_t category) { return CollisionCategories{1u << category}; }

	constexpr CollisionCategories operator|(CollisionCategories other) const { return CollisionCategories{bits | other.bits}; }
	constexpr CollisionCategories& operator|=(CollisionCategories other) { bits |= other.bits; return *this; }

	constexpr bool contains(uint32_t category) const { return (bits & (1u << category)) != 0; }
};

// Descriptors (setup-time, not simulation state)

// Describes a query volume to register with the adapter.
struct QueryVolumeDescriptor
{
	QueryGeometry geometry;
	CollisionCategories searchCategories;    // which categories of objects to find
	glm::mat4 offsetTransform{1.f};         // local offset from parent
	uint32_t traceCategory = 0;             // adapter resolves to engine channel via mapping
};

// How the engine treats a body during a rollback REPLAY.
enum class BodyResimPolicy : uint8_t
{
	// The engine replays this body's recorded motion. Correct for FOLLOWER bodies —
	// ones the simulation snaps every tick anyway, so re-solving them would only
	// discard the placement the sim is about to redo. Every body today is one of these.
	ReplayRecordedHistory,

	// The engine re-solves this body from the pushed anchor state. For bodies the
	// engine genuinely integrates from corrected input.
	Resimulate
};

// Describes physics body properties — simulation flags only, no geometry.
// The fields after `enableGravity` are all DEFAULTED so every existing descriptor
// keeps today's behaviour without being edited.
struct BodyDescriptor
{
	bool simulatePhysics = true;
	bool enableGravity = false;
	// The engine factory ADOPTS the owner's pre-existing root body instead of
	// creating one. Exactly one per simulatable.
	bool isRoot = false;
	// Freeze all three rotational DOF (the upright capsule).
	bool lockRotation = false;
	// ReplayRecordedHistory: the engine replays this body's recorded motion during a
	// rollback replay (the siblings — snapped every tick anyway). Resimulate: the
	// engine re-solves it from the pushed anchor state (bodies the engine integrates
	// from corrected input).
	BodyResimPolicy resimPolicy = BodyResimPolicy::ReplayRecordedHistory;
};

// Describes a collision shape that can be found by spatial queries.
struct ShapeDescriptor
{
	QueryGeometry geometry;                  // shape dimensions
	CollisionCategories categories;          // what categories this shape belongs to
	// Categories this shape BLOCKS with contacts; every other category merely
	// overlaps. Default none — which is exactly what every existing shape does today.
	CollisionCategories blockingCategories{};
};

// Maps a body to the shapes attached to it.
// Passed to the adapter factory so it can create the engine component,
// register the body, and register all shapes from a single source of truth.
struct PhysicalObjectDescriptor
{
	BodyDescriptor body;
	std::vector<ShapeDescriptor> shapes;
};
