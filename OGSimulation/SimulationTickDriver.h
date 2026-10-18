#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationTickDriver-rationale.md · docs/SimulationTickDriver-guards.md

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "OGSimulation/Mailbox.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/SimulationScheduler.h"
#include "OGSimulation/SimulationStepDriver.h"
#include "OGSimulation/StepHooks.h"
#include "OGSimulation/TickDriverConcepts.h"
#include "OGSimulation/TickDriverLog.h"
#include "OGSimulation/TickDriverStepHooks.h"
#include "OGSimulation/TickDriverThreadContext.h"
#include "OGSimulation/TickDriverWindow.h"

struct OccupancyCommand
{
	uint32_t slot     = 0u;
	bool     occupied = false;
};

namespace simulationTickDriver
{
inline constexpr uint32_t kMaxStepsPerFrame         = 240u;
inline constexpr size_t   kOccupancyCommandCapacity = 64u;
inline constexpr size_t   kStepCostSampleCapacity   = 256u;
} // namespace simulationTickDriver

template <TickDriverGame GameT, TickDriverWorldHost WorldHostT, TickDriverPresenterSteps PresenterT, TickDriverEngine EngineT>
class SimulationTickDriver
{
public:
	using Handle     = typename EngineT::Handle;
	using Completion = typename EngineT::Completion;

	struct Config
	{
		double        dtSeconds;
		bool          isAuthority;
		StepThreading threading;
		int32_t       requestedMaxCatchUpSteps;
		double        minRateScale = 0.75;
		double        maxRateScale = 1.25;
	};

	SimulationTickDriver(GameT& game, WorldHostT& worldHost, PresenterT& presenter, EngineT& engine, const Config& config)
		: m_game(&game)
		, m_worldHost(&worldHost)
		, m_engine(&engine)
		, m_config(config)
		, m_window(config.isAuthority, config.threading == StepThreading::Inline)
		, m_firstStepLine(config.isAuthority)
		, m_doors(*this)
	{
		static_assert(__builtin_offsetof(SimulationTickDriver, m_config) < __builtin_offsetof(SimulationTickDriver, m_scheduler)
			&& __builtin_offsetof(SimulationTickDriver, m_scheduler) < __builtin_offsetof(SimulationTickDriver, m_window)
			&& __builtin_offsetof(SimulationTickDriver, m_window) < __builtin_offsetof(SimulationTickDriver, m_occupancyCommands)
			&& __builtin_offsetof(SimulationTickDriver, m_occupancyCommands) < __builtin_offsetof(SimulationTickDriver, m_stepCosts)
			&& __builtin_offsetof(SimulationTickDriver, m_stepCosts) < __builtin_offsetof(SimulationTickDriver, m_tickOffset)
			&& __builtin_offsetof(SimulationTickDriver, m_tickOffset) < __builtin_offsetof(SimulationTickDriver, m_pendingSteps)
			&& __builtin_offsetof(SimulationTickDriver, m_pendingSteps) < __builtin_offsetof(SimulationTickDriver, m_blockingSteps),
			"SimulationTickDriver's owned state is declared in a fixed order: the config, the scheduler, the window, J1, J6, "
			"J3, then the pending and the blocking step handle. Reorder it only together with the rationale's member table");
		static_assert(__builtin_offsetof(SimulationTickDriver, m_blockingSteps) < __builtin_offsetof(SimulationTickDriver, m_doors)
			&& __builtin_offsetof(SimulationTickDriver, m_doors) < __builtin_offsetof(SimulationTickDriver, m_hooks)
			&& __builtin_offsetof(SimulationTickDriver, m_hooks) < __builtin_offsetof(SimulationTickDriver, m_stepDriver),
			"SimulationTickDriver members destruct in REVERSE declaration order. The step driver holds the hooks by reference "
			"and unregisters its resync callback from the game's manager when it goes; the hooks hold the doors, and the "
			"doors reach J1, J3 and J6. Keep the mailboxes < m_doors < m_hooks < m_stepDriver, last. Was the frame host < "
			"hooks < step driver row of the engine-hosted step's composition");

		OG_CHECK(config.threading != StepThreading::FreeRunning,
			"SimulationTickDriver: FreeRunning (the runner thread owns the pump and the batch) is declared but not yet "
			"implemented; choose Inline or FrameBounded");
		OG_CHECK(!config.isAuthority || config.threading == StepThreading::Inline,
			"SimulationTickDriver: the authority must step inline on the owning thread. Its input release delivers into "
			"the game's objects. Was the authority check of the engine-hosted frame host's start of play");
		OG_CHECK(config.isAuthority == (worldHost.threadContext().roleBit() == tickDriverThread::kAuthorityRoleBit),
			"SimulationTickDriver: the driver's role must be the world host's thread-context role; both are fixed at world build");
		OG_CHECK((config.threading == StepThreading::Inline) == worldHost.threadContext().stepsInline(),
			"SimulationTickDriver: the driver's threading must match the world host's stepping mode, which chose the access "
			"predicates at world build");

		{
			const auto worldLock = m_worldHost->lockForStep();
			m_hooks.emplace(Doors{ m_doors, game, presenter, worldHost, engine });
			m_stepDriver.emplace(game.manager(), worldHost.stepWorld(), *m_hooks,
				StepDriverConfig{ static_cast<float>(config.dtSeconds), ResimPolicy::OnRequest, 0u });
		}

		begin();
	}

	~SimulationTickDriver()
	{
		OG_CHECK(!isPendingHandle(m_pendingSteps) && !isPendingHandle(m_blockingSteps),
			"SimulationTickDriver: destroyed with a step task outstanding; the task would run on a destroyed driver. Call "
			"stop() first, as the engine-hosted frame host's end of play waited for its outstanding steps");
	}

	SimulationTickDriver(const SimulationTickDriver&) = delete;
	SimulationTickDriver& operator=(const SimulationTickDriver&) = delete;

	void startFrame(double frameDeltaSeconds)
	{
		const double startSeconds = m_engine->nowSeconds();
		if (m_lastFrameStartSeconds > 0.0)
			m_window.noteFramePeriod(startSeconds - m_lastFrameStartSeconds);
		m_lastFrameStartSeconds = startSeconds;

		const uint32_t numSteps = pumpAndRunSteps(frameDeltaSeconds);

		m_window.noteStartPhysics(numSteps, m_engine->nowSeconds() - startSeconds);
	}

	void endFrame(Completion* completion, int32_t blockMode)
	{
		OG_CHECK(!m_worldHost->threadContext().isWorldLockHeldOnThisThread(),
			"SimulationTickDriver::endFrame: this thread holds the world lock (J2) at end of physics, where it waits for "
			"step tasks that take it: a deadlock. Was frame-host G-05");

		const double endStartSeconds = m_engine->nowSeconds();
		const std::optional<Handle> waitFor = blockMode == 1 ? m_pendingSteps : m_blockingSteps;
		if (!isPendingHandle(waitFor))
		{
			runPostPhysics(std::nullopt);
			return;
		}

		if (completion != nullptr && deferPostPhysics(*completion, *waitFor, endStartSeconds))
			return;

		m_engine->waitFor(*waitFor);
		runPostPhysics(endStartSeconds);
	}

	void stop()
	{
		m_running = false;
		waitForOutstandingSteps();
	}

	void pushOccupancy(uint32_t slot, bool occupied)
	{
		[[maybe_unused]] const bool pushed = m_occupancyCommands.tryPush(OccupancyCommand{ slot, occupied });
		OG_CHECK(pushed,
			"SimulationTickDriver::pushOccupancy: the occupancy ring (J1) is full; the command would be lost and the step "
			"world would keep the wrong body set. No step has drained it");
	}

	void noteWorldLockWait(double waitSeconds) { m_window.noteWorldLockWait(waitSeconds); }

	void noteShadowRestore(double restoreSeconds, bool refused) { m_window.noteShadowRestore(restoreSeconds, refused); }

	double hostNowSeconds() const
	{
		OG_CHECK(m_worldHost->threadContext().isOwnerThreadForRole(),
			"SimulationTickDriver::hostNowSeconds: the host time is the owning thread's accumulated frame delta, written "
			"in startFrame; read off the owning thread it races that write");
		return m_hostTimeSeconds;
	}

	double stepIntervalSeconds()
	{
		const SimulationScheduler& scheduler = schedulerOnOwningThread();
		return scheduler.config().dtSeconds / scheduler.rateScale();
	}

	int64_t tickOffset() const { return m_tickOffset.load(std::memory_order_relaxed); }

	bool savesTicks() const { return m_stepDriver->savesTicks(); }

	uint64_t grantedResims() const { return m_stepDriver->getDiagnostics().grantedResims(); }

	class Diagnostics
	{
	public:
		explicit Diagnostics(const SimulationTickDriver& driver) : m_driver(driver) {}

		const TickDriverWindowCounts& windowCounts() const { return m_driver.m_window.counts(); }
		uint32_t windowSampleCount(TickDriverWindowSeries series) const { return m_driver.m_window.samples(series).count(); }
		LostTime lostTime() const { return m_driver.m_scheduler->lostTime(); }
		uint64_t physicsStepCount() const { return m_driver.m_scheduler->physicsStepCount(); }
		double rateScale() const { return m_driver.m_scheduler->rateScale(); }
		bool isRunning() const { return m_driver.m_running; }
		bool hasPendingSteps() const { return m_driver.isPendingHandle(m_driver.m_pendingSteps); }

	private:
		const SimulationTickDriver& m_driver;
	};

	Diagnostics getDiagnostics() const { return Diagnostics(*this); }

private:
	struct StepBatch
	{
		uint64_t first = 0u;
		uint32_t count = 0u;
		std::array<double, simulationTickDriver::kMaxStepsPerFrame> deadlines{};
	};

	class DriverDoors
	{
	public:
		explicit DriverDoors(SimulationTickDriver& driver) : m_driver(&driver) {}

		void applyOccupancyCommands_Step() { m_driver->applyOccupancyCommands_Step(); }
		void publishTickOffset_Step(int64_t offset) { m_driver->m_tickOffset.store(offset, std::memory_order_relaxed); }
		void pushStepCost_Step(const StepCostSample& sample) { m_driver->m_stepCosts.tryPush(sample); }
		bool isStepRunningOnThisThread() const { return m_driver->m_worldHost->threadContext().isStepRunningOnThisThread(); }
		bool isOwnerThread() const { return m_driver->m_worldHost->threadContext().isOwnerThreadForRole(); }

	private:
		SimulationTickDriver* m_driver;
	};

	using Doors      = StepDoors<DriverDoors, GameT, PresenterT, WorldHostT, EngineT>;
	using Hooks      = TickDriverStepHooks<Doors>;
	using StepDriver = SimulationStepDriver<typename GameT::Manager, typename WorldHostT::World, Hooks>;

	void begin()
	{
		const int32_t requestedCap = m_config.requestedMaxCatchUpSteps;
		const uint32_t cap = static_cast<uint32_t>(
			std::clamp<int32_t>(requestedCap, 1, static_cast<int32_t>(simulationTickDriver::kMaxStepsPerFrame)));
		if (static_cast<int32_t>(cap) != requestedCap)
		{
			m_engine->log(HostLogLevel::Warning,
				m_window.formatCatchUpCapWarningLine(requestedCap, simulationTickDriver::kMaxStepsPerFrame, cap));
		}

		m_hostTimeSeconds       = 0.0;
		m_lastFrameStartSeconds = 0.0;
		m_scheduler.emplace(
			SchedulerConfig{ m_config.dtSeconds, MaxCatchUpSteps{ cap }, m_config.minRateScale, m_config.maxRateScale },
			m_hostTimeSeconds);
		m_window.restart(m_engine->nowSeconds(), windowTotals());

		m_running = true;

		m_engine->log(HostLogLevel::Warning, m_window.formatFrameLine(m_config.dtSeconds, cap));
	}

	uint32_t pumpAndRunSteps(double frameDeltaSeconds)
	{
		m_blockingSteps = m_pendingSteps;

		SimulationScheduler& scheduler = schedulerOnOwningThread();
		m_hostTimeSeconds += frameDeltaSeconds;
		TickRateDoor rate(scheduler);
		m_game->beforePump(m_hostTimeSeconds, rate);
		// ⛔G-04  docs/SimulationTickDriver-guards.md
		const uint32_t numSteps = scheduler.pump(m_hostTimeSeconds);
		if (numSteps == 0u)
			return 0u;

		StepBatch batch;
		batch.first = scheduler.physicsStepCount() - numSteps;
		batch.count = numSteps;
		for (uint32_t index = 0u; index < numSteps; ++index)
			batch.deadlines[index] = scheduler.stepDeadline(batch.first + index);

		// ⛔G-03  docs/SimulationTickDriver-guards.md
		const int32_t firstUpcomingSimTick =
			static_cast<int32_t>(static_cast<int64_t>(batch.first) + m_tickOffset.load(std::memory_order_relaxed));

		// ⛔G-02  docs/SimulationTickDriver-guards.md
		m_game->onFrameStepsDue(firstUpcomingSimTick, static_cast<int32_t>(numSteps));

		// ⛔G-01  docs/SimulationTickDriver-guards.md
		if (m_config.threading == StepThreading::Inline)
		{
			runBatch(batch);
			// ⛔G-06  docs/SimulationTickDriver-guards.md
			m_game->afterInlineBatch(firstUpcomingSimTick);
			return numSteps;
		}

		m_pendingSteps = m_engine->dispatchAfter(isPendingHandle(m_pendingSteps) ? *m_pendingSteps : Handle{},
			[this, batch]() { runBatch(batch); });
		return numSteps;
	}

	void runBatch(const StepBatch& batch)
	{
		if (!m_loggedFirstStep)
		{
			m_loggedFirstStep = true;
			m_engine->log(HostLogLevel::Warning,
				m_firstStepLine.format(m_worldHost->threadContext().isOwnerThreadForRole(), batch.first, batch.count));
		}

		for (uint32_t index = 0u; index < batch.count; ++index)
			runStep(batch.first + index, batch.deadlines[index]);
	}

	void runStep(uint64_t physicsStep, double stepDeadlineSeconds)
	{
		OG_CHECK(m_config.threading != StepThreading::Inline || m_worldHost->threadContext().isOwnerThreadForRole(),
			"SimulationTickDriver: an inline step ran off the owning thread. In the inline mode (the authority, and a client "
			"stepping inline) the owning thread owns the step driver: the authority's input release delivers into the "
			"game's objects, and the inline access predicates accept the owning thread. Was the inline-step check of the "
			"engine-hosted per-step body");
		const auto worldLock = m_worldHost->lockForStep();
		const StepScope stepScope(m_worldHost->threadContext());
		m_hooks->currentStepDeadline = stepDeadlineSeconds;
		const TickOutcome outcome = m_stepDriver->runTick(physicsStep);
		m_worldHost->afterRunTick_Step(physicsStep, outcome);
	}

	bool deferPostPhysics(Completion& completion, const Handle& waitFor, double endStartSeconds)
	{
		return m_engine->deferUntil(completion, waitFor,
			[this, endStartSeconds]()
			{
				if (!m_running)
					return;
				runPostPhysics(endStartSeconds);
			});
	}

	void runPostPhysics(std::optional<double> waitStartSeconds)
	{
		const double postStartSeconds = m_engine->nowSeconds();
		if (waitStartSeconds.has_value())
			m_window.noteEndWait(postStartSeconds - *waitStartSeconds);

		m_game->onPostPhysics();

		const double nowSeconds = m_engine->nowSeconds();
		m_window.notePostPhysics(nowSeconds - postStartSeconds);
		drainStepCosts();
		if (m_window.isDue(nowSeconds))
			logWindow(nowSeconds);
	}

	void drainStepCosts()
	{
		while (std::optional<StepCostSample> sample = m_stepCosts.tryPop())
			m_window.noteStepCost(sample->preSeconds, sample->stepSeconds, sample->replayedTicks, sample->resimRefused);
	}

	void logWindow(double nowSeconds)
	{
		const TickDriverWindowTotals totals = windowTotals();
		m_engine->log(HostLogLevel::Warning, m_window.formatWindowLine(nowSeconds, totals));
		m_window.restart(nowSeconds, totals);
	}

	TickDriverWindowTotals windowTotals()
	{
		const SimulationScheduler& scheduler = schedulerOnOwningThread();
		return TickDriverWindowTotals{ scheduler.lostTime(), scheduler.ignoredTimeSamples(), m_occupancyCommands.drops(),
			m_stepCosts.drops() };
	}

	void waitForOutstandingSteps()
	{
		OG_CHECK(!m_worldHost->threadContext().isWorldLockHeldOnThisThread(),
			"SimulationTickDriver::stop: this thread holds the world lock (J2) and is about to wait for a step task that "
			"takes it: a deadlock. Was frame-host G-05");

		const std::array<const std::optional<Handle>*, 2> outstanding{ &m_pendingSteps, &m_blockingSteps };
		for (const std::optional<Handle>* steps : outstanding)
		{
			if (steps->has_value())
			{
				// ⛔G-05  docs/SimulationTickDriver-guards.md
				m_engine->waitFor(**steps);
			}
		}

		m_pendingSteps.reset();
		m_blockingSteps.reset();
	}

	void applyOccupancyCommands_Step()
	{
		OG_CHECK(m_worldHost->threadContext().isStepRunningOnThisThread(),
			"SimulationTickDriver: the occupancy door was called outside a step of this driver. Only the step (under the "
			"world lock and the step scope) may touch the step driver: from anywhere else it races the running step, and "
			"the occupancy timeline no longer records the body set the world stepped with. Was the occupancy-door check "
			"of the engine-hosted frame host");
		while (std::optional<OccupancyCommand> command = m_occupancyCommands.tryPop())
			m_stepDriver->noteOccupancy(command->slot, command->occupied);
	}

	SimulationScheduler& schedulerOnOwningThread()
	{
		OG_CHECK(m_worldHost->threadContext().isOwnerThreadForRole(),
			"SimulationTickDriver: the scheduler was reached off the owning thread. The owning thread owns it in Inline "
			"and FrameBounded: it pumps it in startFrame, and the step reads only the batch it was handed by value. It has "
			"no synchronization, so a call from the step or any other thread (a rate change included) races the pump");
		OG_CHECK(m_scheduler.has_value(), "SimulationTickDriver: the scheduler was reached before it was built");
		return *m_scheduler;
	}

	bool isPendingHandle(const std::optional<Handle>& steps) const
	{
		return steps.has_value() && m_engine->isPending(*steps);
	}

	GameT*      m_game;
	WorldHostT* m_worldHost;
	EngineT*    m_engine;

	const Config m_config;
	bool         m_running               = false;
	bool         m_loggedFirstStep       = false;
	double       m_hostTimeSeconds       = 0.0;
	double       m_lastFrameStartSeconds = 0.0;

	std::optional<SimulationScheduler>                                         m_scheduler;
	TickDriverWindow                                                           m_window;
	TickDriverFirstStepLine                                                    m_firstStepLine;
	SpscRing<OccupancyCommand, simulationTickDriver::kOccupancyCommandCapacity> m_occupancyCommands;
	SpscRing<StepCostSample, simulationTickDriver::kStepCostSampleCapacity>     m_stepCosts;
	std::atomic<int64_t>                                                       m_tickOffset{ 0 };
	std::optional<Handle>                                                      m_pendingSteps;
	std::optional<Handle>                                                      m_blockingSteps;
	DriverDoors                                                                m_doors;
	std::optional<Hooks>                                                       m_hooks;
	std::optional<StepDriver>                                                  m_stepDriver;
};
