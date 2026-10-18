#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationStepDriver-rationale.md · docs/SimulationStepDriver-guards.md

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>

#include "OGSimulation/BodySlotOccupancy.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/PhysicsWorldAdapter.h"
#include "OGSimulation/SimulationManager.h"
#include "OGSimulation/SimulationTimeContext.h"
#include "OGSimulation/StepHooks.h"

enum class ResimPolicy : uint8_t
{
	OnRequest,
	Always
};

struct StepDriverConfig
{
	float dtSeconds = 0.f;
	ResimPolicy policy = ResimPolicy::OnRequest;
	uint32_t alwaysDepthTicks = 0;
};

template <typename ManagerT, PhysicsWorldAdapter WorldT, StepHooks HooksT>
class SimulationStepDriver
{
public:
	SimulationStepDriver(ManagerT& manager, WorldT& world, HooksT& hooks, const StepDriverConfig& config)
		: m_manager(manager)
		, m_world(world)
		, m_hooks(hooks)
		, m_config(config)
		, m_isClient(manager.runsPrediction())
	{
		OG_CHECK(std::isfinite(config.dtSeconds) && config.dtSeconds > 0.f,
			"SimulationStepDriver: dtSeconds must be finite and positive");
		OG_CHECK(config.policy != ResimPolicy::Always || config.alwaysDepthTicks >= 1u,
			"SimulationStepDriver: ResimPolicy::Always needs a depth of at least one tick");
		OG_CHECK(config.policy != ResimPolicy::Always || m_isClient,
			"SimulationStepDriver: ResimPolicy::Always is a client policy; the authority never resims");
		applyBodySlotOccupancy(m_bodySlotOccupancy);
		if (m_isClient)
		{
			m_resyncCallbackId = m_manager.editClientClock().registerResyncCallback(
				[this](unsigned int /*newPredictionTick*/) { onHardResync(); });
		}
	}

	~SimulationStepDriver()
	{
		if (m_isClient)
		{
			m_manager.editClientClock().unregisterResyncCallback(m_resyncCallbackId);
		}
	}

	SimulationStepDriver(const SimulationStepDriver&) = delete;
	SimulationStepDriver& operator=(const SimulationStepDriver&) = delete;

	TickOutcome runTick(uint64_t physicsStep)
	{
		return m_isClient ? runClientTick(physicsStep) : runAuthorityTick(physicsStep);
	}

	void noteBodySlotOccupancy(uint32_t slot, bool occupied)
	{
		OG_CHECK(slot < kMaxSimulatableSlots, "SimulationStepDriver: occupancy slot out of range");
		m_bodySlotOccupancy.occupied.set(slot, occupied);
		if (!occupied)
		{
			// ⛔G-05  docs/SimulationStepDriver-guards.md
			for (auto& [tick, held] : m_bodySlotOccupancyTimeline)
			{
				held.occupied.reset(slot);
			}
		}
		applyBodySlotOccupancy(m_bodySlotOccupancy);
	}

	const BodySlotOccupancy& bodySlotOccupancy() const { return m_bodySlotOccupancy; }

	bool savesTicks() const { return m_isClient; }

	class Diagnostics
	{
	public:
		explicit Diagnostics(const SimulationStepDriver& driver) : m_driver(driver) {}

		uint64_t grantedResims() const { return m_driver.m_grantedResims; }
		uint64_t refusedResims() const { return m_driver.m_refusedResims; }
		uint64_t replayedTicks() const { return m_driver.m_replayedTicks; }
		uint64_t skipBackfills() const { return m_driver.m_skipBackfills; }
		uint64_t hardResyncInvalidations() const { return m_driver.m_hardResyncInvalidations; }
		uint64_t ignoredAnchors() const { return m_driver.m_ignoredAnchors; }
		uint64_t bodySlotOccupancyTimelineMisses() const { return m_driver.m_bodySlotOccupancyTimelineMisses; }

		std::optional<BodySlotOccupancy> bodySlotOccupancyAt(SimTick tick) const
		{
			const auto it = m_driver.m_bodySlotOccupancyTimeline.find(tick);
			return it != m_driver.m_bodySlotOccupancyTimeline.end() ? std::optional<BodySlotOccupancy>(it->second)
			                                                : std::nullopt;
		}

		std::size_t bodySlotOccupancyTimelineSize() const { return m_driver.m_bodySlotOccupancyTimeline.size(); }

	private:
		const SimulationStepDriver& m_driver;
	};

	Diagnostics getDiagnostics() const { return Diagnostics(*this); }

private:
	TickOutcome runClientTick(uint64_t physicsStep)
	{
		TickOutcome outcome;
		outcome.physicsStep = physicsStep;
		m_hardResyncThisTick = false;

		const UpcomingTick upcoming{false, std::nullopt, physicsStep};
		m_hooks.beforeTick(upcoming);

		resimulateIfRequested(physicsStep, outcome);

		syncLiveBodySlotOccupancy();
		// ⛔G-07  docs/SimulationStepDriver-guards.md
		m_hooks.beforeSimulate(upcoming);
		m_world.saveScratch();
		m_manager.onGameSimulation(SimulationUpdateInfo(false, false));

		const std::optional<SimulationTimeStep>& step = m_manager.lastIntegratedStep();
		OG_CHECK(step.has_value(), "SimulationStepDriver: onGameSimulation integrated no step");
		const SimTick tick = step->getTick();
		const StepKind kind = step->getStepKind();

		if (kind == StepKind::Skip)
		{
			// ⛔G-02  docs/SimulationStepDriver-guards.md
			m_world.commitScratch(tick - 1u);
			recordBodySlotOccupancy(tick - 1u, m_appliedBodySlotOccupancy);
			++m_skipBackfills;
		}

		m_hooks.beforePhysics(tick);
		m_world.step(m_config.dtSeconds);
		m_manager.onPostGameSimulation(SimulationUpdateInfo(false, false));

		// ⛔G-03  docs/SimulationStepDriver-guards.md
		if (stepAllocatesFrontierSlot(kind))
		{
			m_world.saveTick(tick);
			recordBodySlotOccupancy(tick, m_appliedBodySlotOccupancy);
		}

		outcome.tick = tick;
		outcome.kind = kind;
		// ⛔G-01  docs/SimulationStepDriver-guards.md
		outcome.hardResync = m_hardResyncThisTick;
		m_hooks.afterTick(outcome);
		return outcome;
	}

	TickOutcome runAuthorityTick(uint64_t physicsStep)
	{
		TickOutcome outcome;
		outcome.physicsStep = physicsStep;

		const SimTick upcomingTick = m_manager.getServerClock().getTick() + 1u;
		const UpcomingTick upcoming{true, upcomingTick, physicsStep};
		m_hooks.beforeTick(upcoming);
		m_hooks.beforeSimulate(upcoming);

		m_manager.onGameSimulation(SimulationUpdateInfo(false, false));
		const std::optional<SimulationTimeStep>& step = m_manager.lastIntegratedStep();
		OG_CHECK(step.has_value() && step->getTick() == upcomingTick,
			"SimulationStepDriver: the authority must simulate exactly serverTick + 1, the tick beforeTick released "
			"inputs for. Was the + 1 of the engine-hosted input release");

		m_hooks.beforePhysics(upcomingTick);
		m_world.step(m_config.dtSeconds);
		m_manager.onPostGameSimulation(SimulationUpdateInfo(false, false));

		outcome.tick = upcomingTick;
		outcome.kind = step->getStepKind();
		m_hooks.afterTick(outcome);
		return outcome;
	}

	void resimulateIfRequested(uint64_t physicsStep, TickOutcome& outcome)
	{
		const SimTick present = m_manager.getClientClock().getPredictionTick();
		const std::optional<SimTick> anchor = requestedAnchor(present);
		if (!anchor.has_value())
		{
			return;
		}

		ResimGateProbe& probe = m_manager.editResimGateProbe();
		probe.noteRequest(*anchor, static_cast<int32_t>(present), static_cast<int32_t>(*anchor));
		if (!m_world.hasTick(*anchor))
		{
			++m_refusedResims;
			outcome.resimRefused = true;
			return;
		}
		probe.noteGrant(static_cast<int32_t>(*anchor));

		const bool restored = m_world.restoreTick(*anchor);
		OG_CHECK(restored, "SimulationStepDriver: restoreTick failed for a tick hasTick reported held");
		applyBodySlotOccupancy(bodySlotOccupancyAt(*anchor));
		m_manager.prepareResimulation(static_cast<int32_t>(physicsStep), *anchor);

		// ⛔G-04  docs/SimulationStepDriver-guards.md
		const uint32_t replayTicks = present - *anchor;
		for (uint32_t index = 0u; index < replayTicks; ++index)
		{
			const SimTick replayTick = *anchor + 1u + index;
			const bool first = index == 0u;
			const BodySlotOccupancy replayOccupancy = bodySlotOccupancyAt(replayTick);
			applyBodySlotOccupancy(replayOccupancy);
			m_manager.onGameSimulation(SimulationUpdateInfo(true, first));
			m_world.step(m_config.dtSeconds);
			m_manager.onPostGameSimulation(SimulationUpdateInfo(true, first));
			const SimTick savedTick = m_manager.currentIntegratedTick();
			OG_CHECK(savedTick == replayTick,
				"SimulationStepDriver: a replay step must integrate exactly the next tick (one physics step per sim tick)");
			m_world.saveTick(savedTick);
			// ⛔G-06  docs/SimulationStepDriver-guards.md
			recordBodySlotOccupancy(savedTick, replayOccupancy);
		}

		outcome.replayedTicks = replayTicks;
		m_replayedTicks += replayTicks;
		++m_grantedResims;
	}

	std::optional<SimTick> requestedAnchor(SimTick present)
	{
		if (m_config.policy == ResimPolicy::Always)
		{
			if (present <= m_config.alwaysDepthTicks)
			{
				return std::nullopt;
			}
			return present - m_config.alwaysDepthTicks;
		}

		const unsigned int correctionTick = m_manager.onCheckIsSimilar();
		if (correctionTick == std::numeric_limits<unsigned int>::max() || correctionTick == 0u)
		{
			return std::nullopt;
		}
		if (correctionTick >= present)
		{
			++m_ignoredAnchors;
			return std::nullopt;
		}
		return correctionTick;
	}

	void onHardResync()
	{
		m_world.invalidateAllTicks();
		m_bodySlotOccupancyTimeline.clear();
		m_hardResyncThisTick = true;
		++m_hardResyncInvalidations;
	}

	void applyBodySlotOccupancy(const BodySlotOccupancy& occupancy)
	{
		m_world.applyBodySlotOccupancy(occupancy);
		m_appliedBodySlotOccupancy = occupancy;
	}

	void syncLiveBodySlotOccupancy()
	{
		if (m_appliedBodySlotOccupancy != m_bodySlotOccupancy)
		{
			applyBodySlotOccupancy(m_bodySlotOccupancy);
		}
	}

	BodySlotOccupancy bodySlotOccupancyAt(SimTick tick)
	{
		const auto it = m_bodySlotOccupancyTimeline.find(tick);
		if (it != m_bodySlotOccupancyTimeline.end())
		{
			return it->second;
		}
		++m_bodySlotOccupancyTimelineMisses;
		return m_bodySlotOccupancy;
	}

	void recordBodySlotOccupancy(SimTick tick, const BodySlotOccupancy& occupancy)
	{
		m_bodySlotOccupancyTimeline[tick] = occupancy;
		const std::optional<SimTick> oldest = m_world.oldestHeldTick();
		if (!oldest.has_value())
		{
			m_bodySlotOccupancyTimeline.clear();
			return;
		}
		m_bodySlotOccupancyTimeline.erase(m_bodySlotOccupancyTimeline.begin(), m_bodySlotOccupancyTimeline.lower_bound(*oldest));
	}

	ManagerT& m_manager;
	WorldT& m_world;
	HooksT& m_hooks;
	const StepDriverConfig m_config;
	const bool m_isClient;
	unsigned int m_resyncCallbackId = ClientPredictionClock::InvalidCallbackId;

	BodySlotOccupancy m_bodySlotOccupancy;
	BodySlotOccupancy m_appliedBodySlotOccupancy;
	std::map<SimTick, BodySlotOccupancy> m_bodySlotOccupancyTimeline;
	bool m_hardResyncThisTick = false;

	uint64_t m_grantedResims = 0u;
	uint64_t m_refusedResims = 0u;
	uint64_t m_replayedTicks = 0u;
	uint64_t m_skipBackfills = 0u;
	uint64_t m_hardResyncInvalidations = 0u;
	uint64_t m_ignoredAnchors = 0u;
	uint64_t m_bodySlotOccupancyTimelineMisses = 0u;
};
