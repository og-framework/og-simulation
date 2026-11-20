#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationTickDriver-rationale.md · docs/SimulationTickDriver-guards.md

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
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

struct BodySlotOccupancyCommand
{
	uint32_t slot     = 0u;
	bool     occupied = false;
};

enum class FrameTimeBase : uint8_t
{
	HostDelta,
	Monotonic,
};

enum class FrameWait : uint8_t
{
	EarlierFrames,
	ThisFramesSingleStep,
};

namespace simulationTickDriver
{
inline constexpr uint32_t kMaxStepsPerFrame                 = 240u;
inline constexpr size_t   kBodySlotOccupancyCommandCapacity = 64u;
inline constexpr size_t   kStepCostSampleCapacity           = 256u;
inline constexpr uint32_t kEarlyWakesBeforeFallback         = 3u;

inline constexpr bool isFiniteSeconds(double seconds)
{
	constexpr uint64_t kExponentBits = 0x7ff0000000000000ull;
	return (std::bit_cast<uint64_t>(seconds) & kExponentBits) != kExponentBits;
}

static_assert(isFiniteSeconds(0.0) && isFiniteSeconds(-0.0) && isFiniteSeconds(std::numeric_limits<double>::max())
		&& isFiniteSeconds(-std::numeric_limits<double>::max()) && isFiniteSeconds(std::numeric_limits<double>::denorm_min())
		&& !isFiniteSeconds(std::numeric_limits<double>::infinity()) && !isFiniteSeconds(-std::numeric_limits<double>::infinity())
		&& !isFiniteSeconds(std::numeric_limits<double>::quiet_NaN())
		&& !isFiniteSeconds(-std::numeric_limits<double>::quiet_NaN())
		&& !isFiniteSeconds(std::numeric_limits<double>::signaling_NaN()),
	"simulationTickDriver::isFiniteSeconds reads the exponent bits, so every NaN and both infinities fail it whatever the "
	"floating-point model; a comparison (x == x, x <= bound) can be folded to true for a NaN");
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
		double        minRateScale     = 0.75;
		double        maxRateScale     = 1.25;
		double        idleSlackSeconds = 0.0;
		FrameTimeBase frameTimeBase    = FrameTimeBase::HostDelta;
		double        monotonicFrameClampSeconds = 0.4;
		FrameWait     frameWait        = FrameWait::EarlierFrames;
	};

	SimulationTickDriver(GameT& game, WorldHostT& worldHost, PresenterT& presenter, EngineT& engine, const Config& config)
		: m_game(&game)
		, m_worldHost(&worldHost)
		, m_engine(&engine)
		, m_config(config)
		, m_window(config.isAuthority, config.threading)
		, m_firstStepLine(config.isAuthority)
		, m_doors(*this)
	{
		static_assert(__builtin_offsetof(SimulationTickDriver, m_config) < __builtin_offsetof(SimulationTickDriver, m_scheduler)
			&& __builtin_offsetof(SimulationTickDriver, m_scheduler) < __builtin_offsetof(SimulationTickDriver, m_window)
			&& __builtin_offsetof(SimulationTickDriver, m_window) < __builtin_offsetof(SimulationTickDriver, m_bodySlotOccupancyCommands)
			&& __builtin_offsetof(SimulationTickDriver, m_bodySlotOccupancyCommands) < __builtin_offsetof(SimulationTickDriver, m_stepCosts)
			&& __builtin_offsetof(SimulationTickDriver, m_stepCosts) < __builtin_offsetof(SimulationTickDriver, m_tickOffset)
			&& __builtin_offsetof(SimulationTickDriver, m_tickOffset) < __builtin_offsetof(SimulationTickDriver, m_publishedRateScale)
			&& __builtin_offsetof(SimulationTickDriver, m_publishedRateScale) < __builtin_offsetof(SimulationTickDriver, m_loopOriginSeconds)
			&& __builtin_offsetof(SimulationTickDriver, m_loopOriginSeconds) < __builtin_offsetof(SimulationTickDriver, m_loopActive)
			&& __builtin_offsetof(SimulationTickDriver, m_loopActive) < __builtin_offsetof(SimulationTickDriver, m_pendingSteps)
			&& __builtin_offsetof(SimulationTickDriver, m_pendingSteps) < __builtin_offsetof(SimulationTickDriver, m_blockingSteps),
			"SimulationTickDriver's owned state is declared in a fixed order: the config, the scheduler, the window, J1, J6, "
			"J3, J8, the loop origin, the loop-active flag, then the pending and the blocking step handle. Reorder it only "
			"together with the rationale's member table");
		static_assert(__builtin_offsetof(SimulationTickDriver, m_blockingSteps) < __builtin_offsetof(SimulationTickDriver, m_doors)
			&& __builtin_offsetof(SimulationTickDriver, m_doors) < __builtin_offsetof(SimulationTickDriver, m_hooks)
			&& __builtin_offsetof(SimulationTickDriver, m_hooks) < __builtin_offsetof(SimulationTickDriver, m_stepDriver),
			"SimulationTickDriver members destruct in REVERSE declaration order. The step driver holds the hooks by reference "
			"and unregisters its resync callback from the game's manager when it goes; the hooks hold the doors, and the "
			"doors reach J1, J3 and J6. Keep the mailboxes < m_doors < m_hooks < m_stepDriver, last. Was the frame host < "
			"hooks < step driver row of the engine-hosted step's composition");

		OG_CHECK(config.threading != StepThreading::FreeRunning || config.isAuthority,
			"SimulationTickDriver: FreeRunning is the authority's only; a client stays Inline or FrameBounded. Was the refusal "
			"of FreeRunning as declared but not yet implemented");
		OG_CHECK(!config.isAuthority || config.threading == StepThreading::Inline || config.threading == StepThreading::FreeRunning,
			"SimulationTickDriver: the authority steps inline on its owning thread: Inline on the frame thread or FreeRunning on "
			"its runner. Was the authority check of the engine-hosted frame host's start of play");
		OG_CHECK(config.isAuthority == (worldHost.threadContext().roleBit() == tickDriverThread::kAuthorityRoleBit),
			"SimulationTickDriver: the driver's role must be the world host's thread-context role; both are fixed at world build");
		OG_CHECK(config.threading == worldHost.threadContext().threading(),
			"SimulationTickDriver: the driver's threading must be the world host's thread-context threading, which chose the "
			"access predicates and the owner thread at world build. Was the match of Inline against the context's stepsInline()");
		OG_CHECK(simulationTickDriver::isFiniteSeconds(config.idleSlackSeconds) && config.idleSlackSeconds >= 0.0
				&& config.idleSlackSeconds < config.dtSeconds,
			"SimulationTickDriver: idleSlackSeconds must be finite and in [0, dtSeconds). The FreeRunning runner idles to each "
			"deadline minus the slack and polls the rest; a slack of a whole step or more would poll through every idle");
		OG_CHECK(config.frameTimeBase == FrameTimeBase::HostDelta || config.threading != StepThreading::FreeRunning,
			"SimulationTickDriver: frameTimeBase Monotonic is a time base for the frame-driven modes, Inline and FrameBounded. "
			"FreeRunning already pumps the runner's engine clock minus the loop's origin, with no frame clamp");
		OG_CHECK(config.frameTimeBase == FrameTimeBase::HostDelta
				|| (simulationTickDriver::isFiniteSeconds(config.monotonicFrameClampSeconds)
					&& config.monotonicFrameClampSeconds * config.minRateScale >= config.dtSeconds),
			"SimulationTickDriver: monotonicFrameClampSeconds must be finite and at least the longest step interval, "
			"dtSeconds / minRateScale. The clamp re-anchors the next deadline by the frame's excess, and a clamp shorter "
			"than one interval would move it past the frame's own time");

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
		OG_CHECK(!m_loopActive.load(std::memory_order_acquire),
			"SimulationTickDriver: destroyed while the authority loop runs on its runner. Set the loop's stop flag, join the "
			"runner, then call stop()");
		OG_CHECK(!isPendingHandle(m_pendingSteps) && !isPendingHandle(m_blockingSteps),
			"SimulationTickDriver: destroyed with a step task outstanding; the task would run on a destroyed driver. Call "
			"stop() first, as the engine-hosted frame host's end of play waited for its outstanding steps");
	}

	SimulationTickDriver(const SimulationTickDriver&) = delete;
	SimulationTickDriver& operator=(const SimulationTickDriver&) = delete;

	void startFrame(double frameDeltaSeconds)
	{
		if (m_config.threading == StepThreading::FreeRunning)
			return;

		sampleFrameInput();

		const double startSeconds = m_engine->nowSeconds();
		if (m_lastFrameStartSeconds > 0.0)
			m_window.noteFramePeriod(startSeconds - m_lastFrameStartSeconds);
		m_lastFrameStartSeconds = startSeconds;

		if (m_config.frameTimeBase == FrameTimeBase::Monotonic)
		{
			advanceMonotonicHostTime(startSeconds);
		}
		else
		{
			// ⛔G-04  docs/SimulationTickDriver-guards.md
			m_hostTimeSeconds += frameDeltaSeconds;
		}
		const uint32_t numSteps = pumpAndRunSteps();
		m_frameSteps = numSteps;

		m_window.noteStartPhysics(numSteps, m_engine->nowSeconds() - startSeconds);
	}

	void endFrame(Completion* completion, int32_t blockMode)
	{
		if (m_config.threading == StepThreading::FreeRunning)
		{
			OG_CHECK(!m_worldHost->threadContext().isOwnerThreadForRole(),
				"SimulationTickDriver::endFrame: the FreeRunning frame-thread pass ran on the runner. The runner owns the pump, "
				"the step-cost drain and the window; the frame thread runs only the post-physics hook");
			// ⛔G-08  docs/SimulationTickDriver-guards.md
			m_game->onPostPhysics();
			return;
		}

		OG_CHECK(!m_worldHost->threadContext().isWorldLockHeldOnThisThread(),
			"SimulationTickDriver::endFrame: this thread holds the world lock (J2) at end of physics, where it waits for "
			"step tasks that take it: a deadlock. Was frame-host G-05");

		const double endStartSeconds = m_engine->nowSeconds();
		const std::optional<Handle> waitFor = waitsForThisFrame(blockMode) ? m_pendingSteps : m_blockingSteps;
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

	template <TickDriverRunnerHost RunnerT>
	void runAuthorityLoop(RunnerT& runner, const std::atomic<bool>& stopRequested)
		requires TickDriverIdleGame<GameT>
	{
		OG_CHECK(m_config.threading == StepThreading::FreeRunning,
			"SimulationTickDriver::runAuthorityLoop: only a FreeRunning driver runs the authority loop; Inline and "
			"FrameBounded are driven by startFrame and endFrame");
		OG_CHECK(m_worldHost->threadContext().isOwnerThreadForRole(),
			"SimulationTickDriver::runAuthorityLoop: the loop runs on the runner, the thread that built the world host and "
			"owns the scheduler, the batch and the window");
		OG_CHECK(!m_loopActive.load(std::memory_order_acquire),
			"SimulationTickDriver::runAuthorityLoop: the authority loop is already running");

		const double originSeconds = m_engine->nowSeconds();
		OG_CHECK(originSeconds >= 0.0,
			"SimulationTickDriver::runAuthorityLoop: the engine clock read below zero at the loop's start; the present clock "
			"reads a negative loop origin as a loop that has not started");
		m_loopActive.store(true, std::memory_order_release);
		m_loopOriginSeconds.store(originSeconds, std::memory_order_release);

		std::optional<double> lastSteppingWakeSeconds;
		while (!stopRequested.load(std::memory_order_acquire))
		{
			const double wakeSeconds = m_engine->nowSeconds() - originSeconds;
			m_hostTimeSeconds = wakeSeconds;
			const uint32_t numSteps = pumpAndRunSteps();
			if (numSteps > 0u)
			{
				if (lastSteppingWakeSeconds.has_value())
					m_window.noteFramePeriod(wakeSeconds - *lastSteppingWakeSeconds);
				lastSteppingWakeSeconds = wakeSeconds;
				m_window.noteStartPhysics(numSteps, (m_engine->nowSeconds() - originSeconds) - wakeSeconds);
			}

			drainStepCosts();
			const double windowNowSeconds = m_engine->nowSeconds();
			if (m_window.isDue(windowNowSeconds))
				logWindow(windowNowSeconds);

			idleUntilNextDeadline(runner, stopRequested, originSeconds);
		}

		m_running = false;
		m_loopActive.store(false, std::memory_order_release);
	}

	void stop()
	{
		OG_CHECK(!m_loopActive.load(std::memory_order_acquire),
			"SimulationTickDriver::stop: the authority loop is still running on its runner. Set its stop flag and join the "
			"runner before stop(); the loop would step on after the session ended");
		m_running = false;
		waitForOutstandingSteps();
	}

	void reanchor()
	{
		OG_CHECK(m_config.frameTimeBase == FrameTimeBase::Monotonic,
			"SimulationTickDriver::reanchor: only a Monotonic driver re-anchors on resume. A HostDelta driver's pause is the "
			"engine's: its frame deltas stop and nothing is owed on resume");
		SimulationScheduler& scheduler = schedulerOnOwningThread();
		const double hostSeconds = m_engine->nowSeconds() - m_monotonicOriginSeconds;
		scheduler.reanchor(hostSeconds);
		m_hostTimeSeconds = hostSeconds;
	}

	void pushBodySlotOccupancy(uint32_t slot, bool occupied)
	{
		OG_CHECK(m_config.threading != StepThreading::FreeRunning || m_worldHost->threadContext().isOwnerThreadForRole(),
			"SimulationTickDriver::pushBodySlotOccupancy: in FreeRunning the occupancy ring (J1) is pushed on the runner, "
			"where the binds and releases run; it is a single-producer ring");
		[[maybe_unused]] const bool pushed = m_bodySlotOccupancyCommands.tryPush(BodySlotOccupancyCommand{ slot, occupied });
		OG_CHECK(pushed,
			"SimulationTickDriver::pushBodySlotOccupancy: the occupancy ring (J1) is full; the command would be lost and the step "
			"world would keep the wrong body set. No step has drained it");
	}

	void noteWorldLockWait(double waitSeconds)
	{
		OG_CHECK(m_config.threading != StepThreading::FreeRunning || m_worldHost->threadContext().isOwnerThreadForRole(),
			"SimulationTickDriver::noteWorldLockWait: in FreeRunning the runner owns the window; the wait is noted by the "
			"bind that waited, on the runner");
		m_window.noteWorldLockWait(waitSeconds);
	}

	void noteShadowRestore(double restoreSeconds, bool refused)
	{
		OG_CHECK(m_config.threading != StepThreading::FreeRunning,
			"SimulationTickDriver::noteShadowRestore: a FreeRunning authority has no shadow world to restore");
		m_window.noteShadowRestore(restoreSeconds, refused);
	}

	double hostNowSeconds() const
	{
		if (m_config.threading == StepThreading::FreeRunning)
		{
			const double originSeconds = m_loopOriginSeconds.load(std::memory_order_acquire);
			return originSeconds < 0.0 ? 0.0 : m_engine->nowSeconds() - originSeconds;
		}
		OG_CHECK(m_worldHost->threadContext().isOwnerThreadForRole(),
			"SimulationTickDriver::hostNowSeconds: the host time is written by the owning thread in startFrame (the "
			"accumulated frame delta, or in Monotonic the engine clock minus the origin); read off the owning thread it "
			"races that write");
		return m_hostTimeSeconds;
	}

	double stepIntervalSeconds()
	{
		if (m_config.threading == StepThreading::FreeRunning)
			return m_config.dtSeconds / m_publishedRateScale.load(std::memory_order_acquire);
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
		LostTimeByCause lostTimeByCause() const { return m_driver.m_scheduler->lostTimeByCause(); }
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

		void applyBodySlotOccupancyCommands_Step() { m_driver->applyBodySlotOccupancyCommands_Step(); }
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
		const double beginSeconds = m_engine->nowSeconds();
		m_monotonicOriginSeconds  = beginSeconds;
		m_window.restart(beginSeconds, windowTotals());

		m_running = true;

		m_engine->log(HostLogLevel::Warning, m_window.formatFrameLine(m_config.dtSeconds, cap, m_config.idleSlackSeconds,
			m_config.frameTimeBase == FrameTimeBase::Monotonic, m_config.frameWait == FrameWait::ThisFramesSingleStep));
	}

	void advanceMonotonicHostTime(double clockSeconds)
	{
		const double hostSeconds   = clockSeconds - m_monotonicOriginSeconds;
		const double excessSeconds = (hostSeconds - m_hostTimeSeconds) - m_config.monotonicFrameClampSeconds;
		if (excessSeconds > 0.0)
		{
			SimulationScheduler& scheduler = schedulerOnOwningThread();
			// ⛔G-09  docs/SimulationTickDriver-guards.md
			scheduler.reanchor(scheduler.nextDeadline() + excessSeconds);
		}
		m_hostTimeSeconds = hostSeconds;
	}

	void sampleFrameInput()
	{
		if constexpr (TickDriverSampleGame<GameT>)
		{
			OG_CHECK(m_config.threading != StepThreading::FreeRunning,
				"SimulationTickDriver: sampleInput ran in FreeRunning. The sample is taken once per startFrame in Inline and "
				"FrameBounded, first; a free-running authority has no local input to sample, and pumpAndRunSteps and the "
				"loop run on the runner at every wake");
			m_game->sampleInput();
		}
	}

	uint32_t pumpAndRunSteps()
	{
		m_blockingSteps = m_pendingSteps;

		SimulationScheduler& scheduler = schedulerOnOwningThread();
		TickRateDoor rate(scheduler);
		m_game->beforePump(m_hostTimeSeconds, rate);
		if (m_config.threading == StepThreading::FreeRunning)
			m_publishedRateScale.store(scheduler.rateScale(), std::memory_order_release);
		const uint32_t numSteps = scheduler.pump(m_hostTimeSeconds);
		if (numSteps == 0u)
			return 0u;

		StepBatch batch;
		batch.first = scheduler.physicsStepCount() - numSteps;
		batch.count = numSteps;
		for (uint32_t index = 0u; index < numSteps; ++index)
			batch.deadlines[index] = scheduler.stepDeadline(batch.first + index);
		if (m_config.threading == StepThreading::FreeRunning)
		{
			for (uint32_t index = 0u; index < numSteps; ++index)
				m_window.noteLateness(m_hostTimeSeconds - batch.deadlines[index]);
		}

		// ⛔G-03  docs/SimulationTickDriver-guards.md
		const int32_t firstUpcomingSimTick =
			static_cast<int32_t>(static_cast<int64_t>(batch.first) + m_tickOffset.load(std::memory_order_relaxed));

		// ⛔G-02  docs/SimulationTickDriver-guards.md
		m_game->onFrameStepsDue(firstUpcomingSimTick, static_cast<int32_t>(numSteps));

		// ⛔G-01  docs/SimulationTickDriver-guards.md
		if (m_config.threading != StepThreading::FrameBounded)
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

	template <typename RunnerT>
	void idleUntilNextDeadline(RunnerT& runner, const std::atomic<bool>& stopRequested, double originSeconds)
	{
		const double targetSeconds   = schedulerOnOwningThread().nextDeadline();
		const double approachSeconds = targetSeconds - m_config.idleSlackSeconds;

		uint32_t earlyReturns = 0u;
		for (;;)
		{
			const IdleWake wake = m_game->idleUntil(originSeconds + approachSeconds);
			if (wake == IdleWake::Activity)
			{
				m_window.noteActivityWake();
				return;
			}
			if (wake == IdleWake::Unavailable)
			{
				runner.sleepUntil(originSeconds + approachSeconds);
				m_window.noteIdleFallback();
				break;
			}
			if (m_engine->nowSeconds() - originSeconds >= approachSeconds)
				break;
			m_window.noteEarlyWake();
			// ⛔G-07  docs/SimulationTickDriver-guards.md
			if (++earlyReturns == simulationTickDriver::kEarlyWakesBeforeFallback)
			{
				runner.sleepUntil(originSeconds + approachSeconds);
				m_window.noteIdleFallback();
				break;
			}
		}

		const double approachedSeconds = m_engine->nowSeconds() - originSeconds;
		while (!stopRequested.load(std::memory_order_acquire))
		{
			const double clockSeconds = m_engine->nowSeconds();
			if (clockSeconds - originSeconds >= targetSeconds)
				break;
			m_game->idleUntil(clockSeconds);
		}
		if (m_config.idleSlackSeconds > 0.0)
			m_window.noteApproach((m_engine->nowSeconds() - originSeconds) - approachedSeconds);
	}

	void runBatch(const StepBatch& batch)
	{
		if (!m_loggedFirstStep)
		{
			m_loggedFirstStep = true;
			m_engine->log(HostLogLevel::Warning,
				m_firstStepLine.format(m_config.threading, m_worldHost->threadContext().isOwnerThreadForRole(), batch.first,
					batch.count));
		}

		for (uint32_t index = 0u; index < batch.count; ++index)
			runStep(batch.first + index, batch.deadlines[index]);

		if constexpr (TickDriverDisplayGame<GameT>)
		{
			if (!m_config.isAuthority)
				publishAfterBatch();
		}
	}

	void publishAfterBatch()
		requires TickDriverDisplayGame<GameT>
	{
		const auto worldLock = m_worldHost->lockForStep();
		const StepScope stepScope(m_worldHost->threadContext());
		m_hooks->afterBatch();
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

	bool waitsForThisFrame(int32_t blockMode) const
	{
		// ⛔G-10  docs/SimulationTickDriver-guards.md
		return blockMode == 1 || (m_config.frameWait == FrameWait::ThisFramesSingleStep && m_frameSteps == 1u);
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
		return TickDriverWindowTotals{ scheduler.lostTime(), scheduler.ignoredTimeSamples(), m_bodySlotOccupancyCommands.drops(),
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

	void applyBodySlotOccupancyCommands_Step()
	{
		OG_CHECK(m_worldHost->threadContext().isStepRunningOnThisThread(),
			"SimulationTickDriver: the occupancy door was called outside a step of this driver. Only the step (under the "
			"world lock and the step scope) may touch the step driver: from anywhere else it races the running step, and "
			"the occupancy timeline no longer records the body set the world stepped with. Was the occupancy-door check "
			"of the engine-hosted frame host");
		while (std::optional<BodySlotOccupancyCommand> command = m_bodySlotOccupancyCommands.tryPop())
			m_stepDriver->noteBodySlotOccupancy(command->slot, command->occupied);
	}

	SimulationScheduler& schedulerOnOwningThread()
	{
		OG_CHECK(m_worldHost->threadContext().isOwnerThreadForRole(),
			"SimulationTickDriver: the scheduler was reached off the owning thread. The owning thread owns it: it pumps it in "
			"startFrame in Inline and FrameBounded, and in the authority loop on the runner in FreeRunning, and the step reads "
			"only the batch it was handed by value. It has no synchronization, so a call from the step or any other thread (a "
			"rate change or the FreeRunning frame thread included) races the pump");
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
	double       m_monotonicOriginSeconds = 0.0;
	uint32_t     m_frameSteps            = 0u;

	std::optional<SimulationScheduler>                                         m_scheduler;
	TickDriverWindow                                                           m_window;
	TickDriverFirstStepLine                                                    m_firstStepLine;
	SpscRing<BodySlotOccupancyCommand, simulationTickDriver::kBodySlotOccupancyCommandCapacity> m_bodySlotOccupancyCommands;
	SpscRing<StepCostSample, simulationTickDriver::kStepCostSampleCapacity>     m_stepCosts;
	std::atomic<int64_t>                                                       m_tickOffset{ 0 };
	std::atomic<double>                                                        m_publishedRateScale{ 1.0 };
	std::atomic<double>                                                        m_loopOriginSeconds{ -1.0 };
	std::atomic<bool>                                                          m_loopActive{ false };
	std::optional<Handle>                                                      m_pendingSteps;
	std::optional<Handle>                                                      m_blockingSteps;
	DriverDoors                                                                m_doors;
	std::optional<Hooks>                                                       m_hooks;
	std::optional<StepDriver>                                                  m_stepDriver;
};
