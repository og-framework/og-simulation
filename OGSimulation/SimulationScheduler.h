#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationScheduler-rationale.md · docs/SimulationScheduler-guards.md

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <type_traits>

#include "OGSimulation/OGAssert.h"

struct MaxCatchUpSteps
{
	constexpr MaxCatchUpSteps(uint32_t steps) : value(steps) {}
	template <typename T>
	MaxCatchUpSteps(T) = delete;
	constexpr operator uint32_t() const { return value; }

	uint32_t value;
};

static_assert(!std::is_default_constructible_v<MaxCatchUpSteps>,
	"SchedulerConfig::maxCatchUpSteps has no default: the host passes the catch-up cap per role");
static_assert(std::is_convertible_v<uint32_t, MaxCatchUpSteps>,
	"MaxCatchUpSteps: a uint32_t cap converts implicitly");
static_assert(!std::is_convertible_v<int, MaxCatchUpSteps> && !std::is_constructible_v<MaxCatchUpSteps, int64_t>
	&& !std::is_constructible_v<MaxCatchUpSteps, uint64_t> && !std::is_constructible_v<MaxCatchUpSteps, double>,
	"MaxCatchUpSteps: only a uint32_t cap is accepted, so {dt, -1} cannot narrow to a cap of 4294967295. "
	"Was task 5 review note N1");

namespace simulationSchedulerSelfCheck
{
template <typename C, typename CapT>
concept ParenthesisedFromCap = requires(CapT cap) { C(1.0, cap); };
} // namespace simulationSchedulerSelfCheck

struct SchedulerConfig
{
	double dtSeconds;
	MaxCatchUpSteps maxCatchUpSteps;
	double minRateScale = 0.9;
	double maxRateScale = 1.1;
};

static_assert(!std::is_default_constructible_v<SchedulerConfig>,
	"SchedulerConfig::maxCatchUpSteps has no default: the host passes the catch-up cap per role");
static_assert(simulationSchedulerSelfCheck::ParenthesisedFromCap<SchedulerConfig, uint32_t>,
	"SchedulerConfig(dt, cap) compiles for a uint32_t cap: the positive control that keeps the signed check below non-vacuous");
static_assert(!simulationSchedulerSelfCheck::ParenthesisedFromCap<SchedulerConfig, int>,
	"SchedulerConfig(dt, -1) must not compile: a signed cap would narrow to 4294967295. Was task 5 review note N1");

struct LostTime
{
	uint64_t steps = 0;
	double seconds = 0.0;
};

class SimulationScheduler
{
public:
	static constexpr size_t kDeadlineHistorySegments = 64;

	SimulationScheduler(const SchedulerConfig& config, double startSeconds)
		: config_(config)
		, latestSeconds_(startSeconds)
	{
		OG_CHECK(std::isfinite(config.dtSeconds) && config.dtSeconds > 0.0,
			"SimulationScheduler: dtSeconds must be finite and positive");
		OG_CHECK(config.maxCatchUpSteps.value >= 1u,
			"SimulationScheduler: maxCatchUpSteps must be at least 1");
		OG_CHECK(std::isfinite(config.minRateScale) && std::isfinite(config.maxRateScale)
			&& config.minRateScale > 0.0 && config.minRateScale <= 1.0 && config.maxRateScale >= 1.0,
			"SimulationScheduler: rate-scale bounds must satisfy 0 < min <= 1 <= max");
		OG_CHECK(std::isfinite(startSeconds), "SimulationScheduler: startSeconds must be finite");
		segments_.push_back(DeadlineSegment{0u, startSeconds, config.dtSeconds});
	}

	uint32_t pump(double nowSeconds)
	{
		if (!acceptTimeSample(nowSeconds))
		{
			return 0u;
		}

		const uint64_t due = stepsDueAt(nowSeconds);
		const uint64_t run = std::min<uint64_t>(due, config_.maxCatchUpSteps.value);
		const uint64_t dropped = due - run;
		if (dropped > 0u)
		{
			const double droppedFrom = deadlineAt(physicsSteps_);
			const double droppedUntil = deadlineAt(physicsSteps_ + dropped);
			// ⛔G-03  docs/SimulationScheduler-guards.md
			recordLostTime(dropped, droppedUntil - droppedFrom);
			beginSegment(droppedUntil, currentInterval());
		}
		physicsSteps_ += run;
		return static_cast<uint32_t>(run);
	}

	void reanchor(double nowSeconds)
	{
		if (!acceptTimeSample(nowSeconds))
		{
			return;
		}

		const double next = nextDeadline();
		if (nowSeconds <= next)
		{
			return;
		}
		// ⛔G-04  docs/SimulationScheduler-guards.md
		recordLostTime(stepsDueAt(nowSeconds) - 1u, nowSeconds - next);
		beginSegment(nowSeconds, currentInterval());
	}

	void setRateScale(double scale)
	{
		if (!std::isfinite(scale))
		{
			return;
		}
		const double clamped = std::clamp(scale, config_.minRateScale, config_.maxRateScale);
		if (clamped == rateScale_)
		{
			return;
		}
		rateScale_ = clamped;
		beginSegment(nextDeadline(), config_.dtSeconds / rateScale_);
	}

	double rateScale() const { return rateScale_; }

	double nextDeadline() const { return deadlineAt(physicsSteps_); }

	double stepDeadline(uint64_t step) const { return deadlineAt(step); }

	uint64_t physicsStepCount() const { return physicsSteps_; }

	LostTime lostTime() const { return lost_; }

	uint64_t ignoredTimeSamples() const { return ignoredTimeSamples_; }

	const SchedulerConfig& config() const { return config_; }

private:
	struct DeadlineSegment
	{
		uint64_t firstStep;
		double firstDeadline;
		double interval;
	};

	bool acceptTimeSample(double nowSeconds)
	{
		if (!std::isfinite(nowSeconds) || nowSeconds < latestSeconds_)
		{
			++ignoredTimeSamples_;
			return false;
		}
		latestSeconds_ = nowSeconds;
		return true;
	}

	void recordLostTime(uint64_t steps, double seconds)
	{
		lost_.steps += steps;
		lost_.seconds += seconds;
	}

	double currentInterval() const { return segments_.back().interval; }

	const DeadlineSegment& segmentFor(uint64_t step) const
	{
		for (auto it = segments_.rbegin(); it != segments_.rend(); ++it)
		{
			if (it->firstStep <= step)
			{
				return *it;
			}
		}
		return segments_.front();
	}

	double deadlineAt(uint64_t step) const
	{
		const DeadlineSegment& segment = segmentFor(step);
		const double offset = static_cast<double>(step) - static_cast<double>(segment.firstStep);
		// ⛔G-02  docs/SimulationScheduler-guards.md
		return segment.firstDeadline + offset * segment.interval;
	}

	uint64_t stepsDueAt(double nowSeconds) const
	{
		const double first = nextDeadline();
		if (nowSeconds < first)
		{
			return 0u;
		}
		constexpr double kMaxEstimate = 4503599627370496.0;
		const double estimate = std::min(std::floor((nowSeconds - first) / currentInterval()) + 1.0, kMaxEstimate);
		uint64_t due = static_cast<uint64_t>(estimate);
		for (int fix = 0; fix < 4 && due > 1u && deadlineAt(physicsSteps_ + due - 1u) > nowSeconds; ++fix)
		{
			--due;
		}
		for (int fix = 0; fix < 4 && deadlineAt(physicsSteps_ + due) <= nowSeconds; ++fix)
		{
			++due;
		}
		return due;
	}

	void beginSegment(double firstDeadline, double interval)
	{
		if (segments_.back().firstStep == physicsSteps_)
		{
			segments_.back() = DeadlineSegment{physicsSteps_, firstDeadline, interval};
			return;
		}
		segments_.push_back(DeadlineSegment{physicsSteps_, firstDeadline, interval});
		if (segments_.size() > kDeadlineHistorySegments)
		{
			segments_.pop_front();
		}
	}

	SchedulerConfig config_;
	std::deque<DeadlineSegment> segments_;
	// ⛔G-01  docs/SimulationScheduler-guards.md
	uint64_t physicsSteps_ = 0u;
	double rateScale_ = 1.0;
	double latestSeconds_;
	LostTime lost_;
	uint64_t ignoredTimeSamples_ = 0u;
};
