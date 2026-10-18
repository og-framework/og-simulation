#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/TickDriverStepHooks-rationale.md · docs/TickDriverStepHooks-guards.md

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "OGSimulation/OGAssert.h"
#include "OGSimulation/PhysicsWorldAdapter.h"
#include "OGSimulation/StepHooks.h"
#include "OGSimulation/TickDriverConcepts.h"

struct StepCostSample
{
	double preSeconds = 0.0;
	double stepSeconds = 0.0;
	uint32_t replayedTicks = 0u;
	bool resimRefused = false;
};

template <typename D>
concept DriverStepDoors = requires(D& d, const D& cd, int64_t offset, const StepCostSample& sample)
{
	{ d.applyOccupancyCommands_Step() }  -> std::same_as<void>;
	{ d.publishTickOffset_Step(offset) } -> std::same_as<void>;
	{ d.pushStepCost_Step(sample) }      -> std::same_as<void>;
	{ cd.isStepRunningOnThisThread() }   -> std::same_as<bool>;
	{ cd.isOwnerThread() }               -> std::same_as<bool>;
};

namespace driverStepDoorsSelfCheck
{
struct Conforming
{
	void applyOccupancyCommands_Step();
	void publishTickOffset_Step(int64_t);
	void pushStepCost_Step(const StepCostSample&);
	bool isStepRunningOnThisThread() const;
	bool isOwnerThread() const;
};

struct MissingPushStepCost
{
	void applyOccupancyCommands_Step();
	void publishTickOffset_Step(int64_t);
	bool isStepRunningOnThisThread() const;
	bool isOwnerThread() const;
};

struct MissingOwnerThreadTest
{
	void applyOccupancyCommands_Step();
	void publishTickOffset_Step(int64_t);
	void pushStepCost_Step(const StepCostSample&);
	bool isStepRunningOnThisThread() const;
};

struct MutatingStepContextTest
{
	void applyOccupancyCommands_Step();
	void publishTickOffset_Step(int64_t);
	void pushStepCost_Step(const StepCostSample&);
	bool isStepRunningOnThisThread();
	bool isOwnerThread() const;
};

struct PushTakesMutableSample
{
	void applyOccupancyCommands_Step();
	void publishTickOffset_Step(int64_t);
	void pushStepCost_Step(StepCostSample&);
	bool isStepRunningOnThisThread() const;
	bool isOwnerThread() const;
};

static_assert(DriverStepDoors<Conforming>,
	"DriverStepDoors self-check: the reference shape must satisfy the concept");
static_assert(!DriverStepDoors<MissingPushStepCost>,
	"DriverStepDoors self-check: pushStepCost_Step (the step-cost ring, J6) is required");
static_assert(!DriverStepDoors<MissingOwnerThreadTest>,
	"DriverStepDoors self-check: isOwnerThread (the authority's inline-step check) is required");
static_assert(!DriverStepDoors<MutatingStepContextTest>,
	"DriverStepDoors self-check: the step-context test must be callable on const doors; it only reads");
static_assert(!DriverStepDoors<PushTakesMutableSample>,
	"DriverStepDoors self-check: pushStepCost_Step copies the sample and cannot rewrite it");
} // namespace driverStepDoorsSelfCheck

template <DriverStepDoors DriverT, TickDriverGame GameT, TickDriverPresenterSteps PresenterT, TickDriverWorldHost WorldHostT,
	TickDriverEngine EngineT>
struct StepDoors
{
	DriverT& driver;
	GameT& game;
	PresenterT& presenter;
	WorldHostT& worldHost;
	const EngineT& engine;
};

template <typename T>
inline constexpr bool isStepDoors = false;

template <typename DriverT, typename GameT, typename PresenterT, typename WorldHostT, typename EngineT>
inline constexpr bool isStepDoors<StepDoors<DriverT, GameT, PresenterT, WorldHostT, EngineT>> = true;

template <typename DoorsT>
	requires isStepDoors<DoorsT>
class TickDriverStepHooks
{
public:
	explicit TickDriverStepHooks(const DoorsT& doors) : m_doors(doors) {}

	TickDriverStepHooks(const TickDriverStepHooks&) = delete;
	TickDriverStepHooks& operator=(const TickDriverStepHooks&) = delete;

	void beforeTick(const UpcomingTick& upcoming)
	{
		m_tickStartSeconds = m_doors.engine.nowSeconds();
		checkInStep();
		m_doors.driver.applyOccupancyCommands_Step();
		if (!upcoming.authority)
			return;

		OG_CHECK(m_doors.driver.isOwnerThread(),
			"TickDriverStepHooks::beforeTick: an authority step ran off the owner thread. The authority's input release "
			"delivers into game objects that only the owner thread may touch, so the authority always steps inline on the "
			"owner thread. Was the game-thread check of the engine-hosted step hooks");
		OG_CHECK(upcoming.authorityTick.has_value(),
			"TickDriverStepHooks::beforeTick: an authority UpcomingTick carries no authorityTick");
		// ⛔G-01  docs/TickDriverStepHooks-guards.md
		m_doors.game.releaseInputsForAuthorityTick(*upcoming.authorityTick);
	}

	void beforeSimulate(const UpcomingTick& upcoming)
	{
		checkInStep();
		m_doors.presenter.captureCorrection_Step(upcoming.physicsStep);
		// ⛔G-03  docs/TickDriverStepHooks-guards.md
		m_stepStartSeconds = m_doors.engine.nowSeconds();
	}

	void beforePhysics(SimTick)
	{
		checkInStep();
		m_doors.game.onStepStarted(m_stepStartSeconds);
	}

	void afterTick(const TickOutcome& outcome)
	{
		const double tickEndSeconds = m_doors.engine.nowSeconds();
		checkInStep();
		m_doors.game.onStepEnded();
		m_doors.presenter.publish_Step(outcome, currentStepDeadline);
		m_doors.worldHost.afterTick_Step(outcome);
		// ⛔G-02  docs/TickDriverStepHooks-guards.md
		m_doors.driver.publishTickOffset_Step(static_cast<int64_t>(outcome.tick) - static_cast<int64_t>(outcome.physicsStep));
		m_doors.driver.pushStepCost_Step(StepCostSample{ m_stepStartSeconds - m_tickStartSeconds,
			tickEndSeconds - m_stepStartSeconds, outcome.replayedTicks, outcome.resimRefused });
	}

	double currentStepDeadline = 0.0;

private:
	void checkInStep() const
	{
		OG_CHECK(m_doors.driver.isStepRunningOnThisThread(),
			"TickDriverStepHooks: a hook was called outside a step of this driver. The hooks reach the step driver, the "
			"world and the step-owned mailboxes, which only the step (under the world lock and the step scope) may touch; "
			"from anywhere else they race the running step. Was the step-context check of the engine-hosted step hooks");
	}

	DoorsT m_doors;
	double m_tickStartSeconds = 0.0;
	double m_stepStartSeconds = 0.0;
};

namespace tickDriverStepHooksSelfCheck
{
using Doors = StepDoors<driverStepDoorsSelfCheck::Conforming, tickDriverGameSelfCheck::Conforming,
	tickDriverPresenterStepsSelfCheck::Conforming, tickDriverWorldHostSelfCheck::Conforming, tickDriverEngineSelfCheck::Conforming>;

template <typename DoorsT>
concept HooksExistFor = requires { typename TickDriverStepHooks<DoorsT>; };

static_assert(StepHooks<TickDriverStepHooks<Doors>>,
	"TickDriverStepHooks must satisfy og-simulation's StepHooks concept: all four hooks, each returning void, afterTick "
	"reading a const outcome");
static_assert(std::is_aggregate_v<Doors>,
	"StepDoors is a plain aggregate of references; the hooks copy it and own nothing it names");
static_assert(!HooksExistFor<driverStepDoorsSelfCheck::Conforming>,
	"TickDriverStepHooks takes a StepDoors aggregate, never a single collaborator");
static_assert(!std::is_copy_constructible_v<TickDriverStepHooks<Doors>>,
	"TickDriverStepHooks is not copyable: the step driver holds it by reference, and a copy would split the step's times");
} // namespace tickDriverStepHooksSelfCheck
