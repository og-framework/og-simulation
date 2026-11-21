#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/TickDriverWindow-rationale.md · docs/TickDriverWindow-guards.md

#include "OGSimulation/OGExport.h"
#include "OGSimulation/SimulationScheduler.h"
#include "OGSimulation/TickDriverLog.h"
#include "OGSimulation/TickDriverThreadContext.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

enum class TickDriverWindowSeries : uint8_t
{
	FrameMs,
	StartPhysicsUs,
	EndWaitUs,
	PostPhysicsUs,
	J2WaitUs,
	StepUs,
	PreUs,
	ShadowRestoreUs,
	LateUs,
	ApproachUs,
	Count,
};

namespace tickDriverWindow
{
inline constexpr double   kWindowSeconds           = 10.0;
inline constexpr uint32_t kMaxSamplesPerSeries     = 16384u;
inline constexpr size_t   kSeriesCount             = static_cast<size_t>(TickDriverWindowSeries::Count);
inline constexpr size_t   kPercentileFieldReserve  = 160u;
inline constexpr size_t   kReplayDepthFieldReserve = 64u;
inline constexpr size_t   kWindowLineReserve       = 4096u;
inline constexpr size_t   kWindowSuffixReserve     = 512u;
inline constexpr size_t   kFrameLineReserve        = 1536u;
inline constexpr size_t   kFirstStepLineReserve    = 256u;

static_assert(kSeriesCount == 10u, "the window line carries eight sample series, and FreeRunning's suffix two more");

template <typename T, typename Allocator, size_t... Index>
std::array<T, sizeof...(Index)> makeEach(const Allocator& allocator, std::index_sequence<Index...>)
{
	return { { (static_cast<void>(Index), T(allocator))... } };
}

inline uint32_t roundedMicroseconds(double seconds)
{
	const double microseconds = seconds * 1.0e6;
	if (!(microseconds >= 0.5))
		return 0u;
	if (microseconds >= 4294967294.5)
		return 4294967295u;
	return static_cast<uint32_t>(microseconds + 0.5);
}
} // namespace tickDriverWindow

struct TickDriverPercentiles
{
	uint32_t n   = 0u;
	double   p50 = 0.0;
	double   p99 = 0.0;
	double   max = 0.0;
};

OGSIMULATION_API TickDriverPercentiles tickDriverPercentilesOf(float* values, uint32_t count);

struct TickDriverWindowTotals
{
	LostTime lost;
	uint64_t ignoredTimeSamples = 0u;
	uint64_t j1Drops            = 0u;
	uint64_t j6Drops            = 0u;
};

struct TickDriverWindowCounts
{
	double   startSeconds     = 0.0;
	uint32_t frames           = 0u;
	uint64_t steps            = 0u;
	uint32_t maxStepsPerFrame = 0u;
	uint32_t endWaits         = 0u;
	uint32_t j2Waits          = 0u;
	uint32_t replays          = 0u;
	uint64_t replayedTicks    = 0u;
	uint32_t maxReplayDepth   = 0u;
	uint32_t resimRefused     = 0u;
	uint32_t shadowRefused    = 0u;
	uint32_t activityWakes    = 0u;
	uint32_t earlyWakes       = 0u;
	uint32_t idleFallbacks    = 0u;
	TickDriverWindowTotals atStart;
};

template <typename Allocator = std::allocator<char>>
class BasicTickDriverWindowSamples
{
public:
	using Values = std::vector<float, typename std::allocator_traits<Allocator>::template rebind_alloc<float>>;

	explicit BasicTickDriverWindowSamples(const Allocator& allocator = Allocator())
		: m_values(allocator)
	{
		m_values.reserve(tickDriverWindow::kMaxSamplesPerSeries);
	}

	BasicTickDriverWindowSamples(const BasicTickDriverWindowSamples&) = delete;
	BasicTickDriverWindowSamples& operator=(const BasicTickDriverWindowSamples&) = delete;

	void add(double value)
	{
		if (m_values.size() < tickDriverWindow::kMaxSamplesPerSeries)
			m_values.push_back(static_cast<float>(value));
		else
			++m_overflow;
	}

	void clear()
	{
		m_values.clear();
		m_overflow = 0u;
	}

	uint32_t count() const { return static_cast<uint32_t>(m_values.size()); }
	uint32_t overflow() const { return m_overflow; }
	const Values& values() const { return m_values; }

	TickDriverPercentiles sortAndTakePercentiles() { return tickDriverPercentilesOf(m_values.data(), count()); }

private:
	Values   m_values;
	uint32_t m_overflow = 0u;
};

template <typename Allocator = std::allocator<char>>
class BasicTickDriverWindow
{
public:
	using Samples = BasicTickDriverWindowSamples<Allocator>;
	using Line    = BasicHostLine<Allocator>;
	using Series  = TickDriverWindowSeries;

	BasicTickDriverWindow(bool isAuthority, StepThreading threading, const Allocator& allocator = Allocator())
		: m_isAuthority(isAuthority)
		, m_threading(threading)
		, m_samples(tickDriverWindow::makeEach<Samples>(allocator, std::make_index_sequence<tickDriverWindow::kSeriesCount>{}))
		, m_percentileFields(tickDriverWindow::makeEach<Line>(allocator, std::make_index_sequence<tickDriverWindow::kSeriesCount>{}))
		, m_replayDepthField(allocator)
		, m_windowLine(allocator)
		, m_suffixField(allocator)
		, m_frameLine(allocator)
	{
		for (Line& field : m_percentileFields)
			field.reserve(tickDriverWindow::kPercentileFieldReserve);
		m_replayDepthField.reserve(tickDriverWindow::kReplayDepthFieldReserve);
		m_windowLine.reserve(tickDriverWindow::kWindowLineReserve);
		m_suffixField.reserve(tickDriverWindow::kWindowSuffixReserve);
		m_frameLine.reserve(tickDriverWindow::kFrameLineReserve);
	}

	BasicTickDriverWindow(bool isAuthority, bool stepsInline, const Allocator& allocator = Allocator())
		: BasicTickDriverWindow(isAuthority, stepsInline ? StepThreading::Inline : StepThreading::FrameBounded, allocator)
	{
	}

	BasicTickDriverWindow(const BasicTickDriverWindow&) = delete;
	BasicTickDriverWindow& operator=(const BasicTickDriverWindow&) = delete;

	bool isAuthority() const { return m_isAuthority; }
	bool stepsInline() const { return m_threading == StepThreading::Inline; }
	StepThreading threading() const { return m_threading; }

	void restart(double nowSeconds, const TickDriverWindowTotals& totals)
	{
		m_counts              = TickDriverWindowCounts{};
		m_counts.startSeconds = nowSeconds;
		m_counts.atStart      = totals;
		for (Samples& samples : m_samples)
			samples.clear();
	}

	bool isDue(double nowSeconds) const { return nowSeconds - m_counts.startSeconds >= tickDriverWindow::kWindowSeconds; }

	void noteFramePeriod(double periodSeconds) { samples(Series::FrameMs).add(periodSeconds * 1000.0); }

	void noteStartPhysics(uint32_t numSteps, double startPhysicsSeconds)
	{
		++m_counts.frames;
		m_counts.steps += numSteps;
		m_counts.maxStepsPerFrame = std::max(m_counts.maxStepsPerFrame, numSteps);
		samples(Series::StartPhysicsUs).add(startPhysicsSeconds * 1.0e6);
	}

	void noteEndWait(double waitSeconds)
	{
		++m_counts.endWaits;
		samples(Series::EndWaitUs).add(waitSeconds * 1.0e6);
	}

	void notePostPhysics(double postPhysicsSeconds) { samples(Series::PostPhysicsUs).add(postPhysicsSeconds * 1.0e6); }

	void noteWorldLockWait(double waitSeconds)
	{
		++m_counts.j2Waits;
		samples(Series::J2WaitUs).add(waitSeconds * 1.0e6);
	}

	void noteShadowRestore(double restoreSeconds, bool refused)
	{
		samples(Series::ShadowRestoreUs).add(restoreSeconds * 1.0e6);
		m_counts.shadowRefused += refused ? 1u : 0u;
	}

	void noteStepCost(double preSeconds, double stepSeconds, uint32_t replayedTicks, bool resimRefused)
	{
		samples(Series::StepUs).add(stepSeconds * 1.0e6);
		if (m_isAuthority || replayedTicks > 0u)
			samples(Series::PreUs).add(preSeconds * 1.0e6);
		if (replayedTicks > 0u)
		{
			++m_counts.replays;
			m_counts.replayedTicks += replayedTicks;
			m_counts.maxReplayDepth = std::max(m_counts.maxReplayDepth, replayedTicks);
		}
		m_counts.resimRefused += resimRefused ? 1u : 0u;
	}

	void noteLateness(double lateSeconds) { samples(Series::LateUs).add(lateSeconds * 1.0e6); }

	void noteApproach(double seconds) { samples(Series::ApproachUs).add(seconds * 1.0e6); }

	void noteActivityWake() { ++m_counts.activityWakes; }

	void noteEarlyWake() { ++m_counts.earlyWakes; }

	void noteIdleFallback() { ++m_counts.idleFallbacks; }

	TickDriverWindowCounts&       counts() { return m_counts; }
	const TickDriverWindowCounts& counts() const { return m_counts; }

	Samples&       samples(Series series) { return m_samples[static_cast<size_t>(series)]; }
	const Samples& samples(Series series) const { return m_samples[static_cast<size_t>(series)]; }

	uint32_t sampleOverflow() const
	{
		uint32_t overflow = 0u;
		for (const Samples& samples : m_samples)
			overflow += samples.overflow();
		return overflow;
	}

	const char* formatWindowLine(double nowSeconds, const TickDriverWindowTotals& totals)
	{
		const TickDriverWindowCounts& counts = m_counts;
		const uint32_t overflow = sampleOverflow();

		const char* const replayDepth = formatReplayDepth();
		const uint32_t costSamples = samples(Series::StepUs).count();
		const uint32_t preTicks    = samples(Series::PreUs).count();
		const uint32_t restores    = samples(Series::ShadowRestoreUs).count();

		const char* const line = formatHostLine(m_windowLine,
			"[SimHost.Window] role=%s stepping=%s seconds=%.2f frames=%u frameMs=%s startPhysicsUs=%s endWaits=%u "
			"endWaitUs=%s postPhysicsUs=%s steps=%llu maxStepsPerFrame=%u lostSteps=%llu lostSeconds=%.4f "
			"ignoredTimeSamples=%llu j1Drops=%llu j2Waits=%u j2WaitUs=%s costSamples=%u stepUs=%s preTicks=%u preUs=%s "
			"replayDepth=%s resimRefused=%u j6Drops=%llu shadowRestores=%u shadowRestoreUs=%s shadowRefused=%u "
			"sampleOverflow=%u",
			roleWord(),
			steppingWord(),
			nowSeconds - counts.startSeconds, counts.frames,
			formatPercentiles(Series::FrameMs, true),
			formatPercentiles(Series::StartPhysicsUs, false),
			counts.endWaits,
			formatPercentiles(Series::EndWaitUs, false),
			formatPercentiles(Series::PostPhysicsUs, false),
			static_cast<unsigned long long>(counts.steps), counts.maxStepsPerFrame,
			static_cast<unsigned long long>(totals.lost.steps - counts.atStart.lost.steps),
			totals.lost.seconds - counts.atStart.lost.seconds,
			static_cast<unsigned long long>(totals.ignoredTimeSamples - counts.atStart.ignoredTimeSamples),
			static_cast<unsigned long long>(totals.j1Drops - counts.atStart.j1Drops),
			counts.j2Waits,
			formatPercentiles(Series::J2WaitUs, false),
			costSamples,
			formatPercentiles(Series::StepUs, false),
			preTicks,
			formatPercentiles(Series::PreUs, false),
			replayDepth, counts.resimRefused,
			static_cast<unsigned long long>(totals.j6Drops - counts.atStart.j6Drops),
			restores,
			formatPercentiles(Series::ShadowRestoreUs, false),
			counts.shadowRefused, overflow);
		if (m_threading != StepThreading::FreeRunning)
			return line;

		m_windowLine.append(formatHostLine(m_suffixField,
			" lateUs=%s activityWakes=%u earlyWakes=%u idleFallbacks=%u approachUs=%s",
			formatPercentiles(Series::LateUs, false),
			counts.activityWakes, counts.earlyWakes, counts.idleFallbacks,
			formatPercentiles(Series::ApproachUs, false)));
		return m_windowLine.c_str();
	}

	const char* formatCatchUpCapWarningLine(int32_t requestedCap, uint32_t maxStepsPerFrame, uint32_t cap)
	{
		return formatHostLine(m_frameLine,
			"[SimHost.Frame] og.Sim.MaxCatchUpSteps=%d is outside [1, %u]; using %u",
			requestedCap, maxStepsPerFrame, cap);
	}

	const char* formatFrameLine(double dtSeconds, uint32_t maxCatchUpSteps, double minRateScale, double maxRateScale,
		double idleSlackSeconds = 0.0, bool monotonicTimeBase = false, bool waitsForThisFramesStep = false)
	{
		if (m_threading == StepThreading::FreeRunning)
			return formatHostLine(m_frameLine,
				"[SimHost.Frame] role=%s stepping=runner dt=%.6f maxCatchUpSteps=%u rateScale=[%.2f,%.2f] timeBase=monotonic "
				"idleSlackUs=%u",
				roleWord(),
				dtSeconds, maxCatchUpSteps, minRateScale, maxRateScale, tickDriverWindow::roundedMicroseconds(idleSlackSeconds));
		return formatHostLine(m_frameLine,
			"[SimHost.Frame] role=%s stepping=%s dt=%.6f maxCatchUpSteps=%u rateScale=[%.2f,%.2f] timeBase=%s%s",
			roleWord(),
			steppingWord(),
			dtSeconds, maxCatchUpSteps, minRateScale, maxRateScale, monotonicTimeBase ? "monotonic" : "worldDelta",
			waitsForThisFramesStep ? " frameWait=thisFramesSingleStep" : "");
	}

private:
	const char* roleWord() const { return m_isAuthority ? "Authority" : "Client"; }

	const char* steppingWord() const
	{
		return m_threading == StepThreading::FreeRunning ? "runner" : m_threading == StepThreading::Inline ? "inline" : "worker";
	}

	const char* formatPercentiles(Series series, bool twoDecimals)
	{
		const TickDriverPercentiles p = samples(series).sortAndTakePercentiles();
		if (p.n == 0u)
			return "-";
		Line& field = m_percentileFields[static_cast<size_t>(series)];
		return twoDecimals ? formatHostLine(field, "%.2f/%.2f/%.2f", p.p50, p.p99, p.max)
		                   : formatHostLine(field, "%.1f/%.1f/%.1f", p.p50, p.p99, p.max);
	}

	const char* formatReplayDepth()
	{
		const TickDriverWindowCounts& counts = m_counts;
		return counts.replays == 0u
			? "-"
			: formatHostLine(m_replayDepthField, "%.2f/%u",
				static_cast<double>(counts.replayedTicks) / static_cast<double>(counts.replays), counts.maxReplayDepth);
	}

	const bool                                            m_isAuthority;
	const StepThreading                                   m_threading;
	TickDriverWindowCounts                                m_counts;
	std::array<Samples, tickDriverWindow::kSeriesCount>   m_samples;
	std::array<Line, tickDriverWindow::kSeriesCount>      m_percentileFields;
	Line                                                  m_replayDepthField;
	Line                                                  m_windowLine;
	Line                                                  m_suffixField;
	Line                                                  m_frameLine;
};

template <typename Allocator = std::allocator<char>>
class BasicTickDriverFirstStepLine
{
public:
	using Line = BasicHostLine<Allocator>;

	explicit BasicTickDriverFirstStepLine(bool isAuthority, const Allocator& allocator = Allocator())
		: m_isAuthority(isAuthority)
		, m_line(allocator)
	{
		m_line.reserve(tickDriverWindow::kFirstStepLineReserve);
	}

	BasicTickDriverFirstStepLine(const BasicTickDriverFirstStepLine&) = delete;
	BasicTickDriverFirstStepLine& operator=(const BasicTickDriverFirstStepLine&) = delete;

	const char* format(StepThreading threading, bool onOwnerThread, uint64_t physicsStep, uint32_t batchCount)
	{
		return formatHostLine(m_line,
			"[SimHost.FirstStep] role=%s thread=%s physicsStep=%llu batch=%u",
			m_isAuthority ? "Authority" : "Client",
			threading == StepThreading::FreeRunning ? "runner" : onOwnerThread ? "game" : "worker",
			static_cast<unsigned long long>(physicsStep), batchCount);
	}

	const char* format(bool onOwnerThread, uint64_t physicsStep, uint32_t batchCount)
	{
		return format(StepThreading::Inline, onOwnerThread, physicsStep, batchCount);
	}

private:
	const bool m_isAuthority;
	Line       m_line;
};

using TickDriverWindow        = BasicTickDriverWindow<>;
using TickDriverFirstStepLine = BasicTickDriverFirstStepLine<>;
