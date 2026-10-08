#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/PhysicsWorldAdapter-rationale.md

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "OGSimulation/BodySlotOccupancy.h"

using SimTick = uint32_t;

struct SnapshotCoverage
{
	bool bodySetAndIds = false;
	bool broadphase = false;
	bool staticShapes = false;
};

template <typename W>
concept PhysicsWorldAdapter = requires(W w, const W cw, float dt, SimTick t, const BodySlotOccupancy& occ)
{
	{ w.step(dt) }             -> std::same_as<void>;
	{ w.saveTick(t) }          -> std::same_as<void>;
	{ w.restoreTick(t) }       -> std::same_as<bool>;
	{ cw.hasTick(t) }          -> std::same_as<bool>;
	{ cw.oldestHeldTick() }    -> std::same_as<std::optional<SimTick>>;
	{ w.invalidateAllTicks() } -> std::same_as<void>;
	{ w.saveScratch() }        -> std::same_as<void>;
	{ w.commitScratch(t) }     -> std::same_as<void>;
	{ w.applyOccupancy(occ) }  -> std::same_as<void>;
	{ cw.stateHash(t) }        -> std::same_as<std::optional<uint64_t>>;
	{ W::coverage }            -> std::same_as<const SnapshotCoverage&>;
	typename std::integral_constant<bool, W::coverage.bodySetAndIds>;
};

namespace physicsWorldAdapterSelfCheck
{
struct Conforming
{
	static constexpr SnapshotCoverage coverage{};

	void step(float);
	void saveTick(SimTick);
	bool restoreTick(SimTick);
	bool hasTick(SimTick) const;
	std::optional<SimTick> oldestHeldTick() const;
	void invalidateAllTicks();
	void saveScratch();
	void commitScratch(SimTick);
	void applyOccupancy(const BodySlotOccupancy&);
	std::optional<uint64_t> stateHash(SimTick) const;
};

struct MissingInvalidateAllTicks
{
	static constexpr SnapshotCoverage coverage{};

	void step(float);
	void saveTick(SimTick);
	bool restoreTick(SimTick);
	bool hasTick(SimTick) const;
	std::optional<SimTick> oldestHeldTick() const;
	void saveScratch();
	void commitScratch(SimTick);
	void applyOccupancy(const BodySlotOccupancy&);
	std::optional<uint64_t> stateHash(SimTick) const;
};

struct MutatingHasTick : Conforming
{
	bool hasTick(SimTick);
};

struct RawStateHash : Conforming
{
	uint64_t stateHash(SimTick) const;
};

struct NonStaticCoverage : Conforming
{
	SnapshotCoverage coverage{};
};

struct RuntimeCoverage : Conforming
{
	static const SnapshotCoverage coverage;
};

static_assert(PhysicsWorldAdapter<Conforming>,
	"PhysicsWorldAdapter self-check: the reference shape must satisfy the concept");
static_assert(!PhysicsWorldAdapter<MissingInvalidateAllTicks>,
	"PhysicsWorldAdapter self-check: invalidateAllTicks (the HardResync wipe) is required");
static_assert(!PhysicsWorldAdapter<MutatingHasTick>,
	"PhysicsWorldAdapter self-check: hasTick must be callable on a const world");
static_assert(!PhysicsWorldAdapter<RawStateHash>,
	"PhysicsWorldAdapter self-check: stateHash must return std::optional<uint64_t>, empty for an unheld tick");
static_assert(!PhysicsWorldAdapter<NonStaticCoverage>,
	"PhysicsWorldAdapter self-check: coverage must be a static member");
static_assert(!PhysicsWorldAdapter<RuntimeCoverage>,
	"PhysicsWorldAdapter self-check: coverage must be usable in a constant expression");
} // namespace physicsWorldAdapterSelfCheck
