#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/StepHooks-rationale.md

#include <concepts>
#include <cstdint>
#include <optional>

#include "OGSimulation/PhysicsWorldAdapter.h"
#include "OGSimulation/SimulationTimeContext.h"

struct UpcomingTick
{
	bool authority = false;
	std::optional<SimTick> authorityTick;
	uint64_t physicsStep = 0;
};

struct TickOutcome
{
	SimTick tick = 0;
	StepKind kind = StepKind::Normal;
	bool hardResync = false;
	uint32_t replayedTicks = 0;
	bool resimRefused = false;
	uint64_t physicsStep = 0;
};

template <typename H>
concept StepHooks = requires(H h, const UpcomingTick& upcoming, SimTick tick, const TickOutcome& outcome)
{
	{ h.beforeTick(upcoming) }     -> std::same_as<void>;
	{ h.beforeSimulate(upcoming) } -> std::same_as<void>;
	{ h.beforePhysics(tick) }      -> std::same_as<void>;
	{ h.afterTick(outcome) }       -> std::same_as<void>;
};

namespace stepHooksSelfCheck
{
struct Conforming
{
	void beforeTick(const UpcomingTick&);
	void beforeSimulate(const UpcomingTick&);
	void beforePhysics(SimTick);
	void afterTick(const TickOutcome&);
};

struct MissingBeforeSimulate
{
	void beforeTick(const UpcomingTick&);
	void beforePhysics(SimTick);
	void afterTick(const TickOutcome&);
};

struct MissingBeforePhysics
{
	void beforeTick(const UpcomingTick&);
	void beforeSimulate(const UpcomingTick&);
	void afterTick(const TickOutcome&);
};

struct BeforeTickReturnsBool
{
	bool beforeTick(const UpcomingTick&);
	void beforeSimulate(const UpcomingTick&);
	void beforePhysics(SimTick);
	void afterTick(const TickOutcome&);
};

struct AfterTickTakesMutableOutcome
{
	void beforeTick(const UpcomingTick&);
	void beforeSimulate(const UpcomingTick&);
	void beforePhysics(SimTick);
	void afterTick(TickOutcome&);
};

static_assert(StepHooks<Conforming>,
	"StepHooks self-check: the reference shape must satisfy the concept");
static_assert(!StepHooks<MissingBeforeSimulate>,
	"StepHooks self-check: beforeSimulate (the hook after any replay, before the normal step consumes input) is required");
static_assert(!StepHooks<MissingBeforePhysics>,
	"StepHooks self-check: beforePhysics (the hook between the integrate and the physics step) is required");
static_assert(!StepHooks<BeforeTickReturnsBool>,
	"StepHooks self-check: a hook returns void, so it cannot veto or reorder the tick");
static_assert(!StepHooks<AfterTickTakesMutableOutcome>,
	"StepHooks self-check: afterTick reads the outcome and cannot rewrite it");
} // namespace stepHooksSelfCheck
