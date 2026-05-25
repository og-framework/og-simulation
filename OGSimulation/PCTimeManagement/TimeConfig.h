#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

// ---------------------------------------------------------------------------
// Acronym legend
//   EMA   — Exponential Moving Average
//   POD   — Plain Old Data
//   PCTM  — Prediction / Correction Time Management
//   RTT   — Round Trip Time (network latency: client → server → client)
// ---------------------------------------------------------------------------

// TimeConfig — all tunable parameters for the PCTM time management system.
// Plain POD struct; copy freely. Set once at startup (e.g., in SimulationManager
// constructor) and pass by const-ref into NetworkTimeEstimator / ClientPredictionClock.
struct TimeConfig
{
	// -------------------------------------------------------------------------
	// Network estimation (NetworkTimeEstimator)
	// -------------------------------------------------------------------------

	// EMA smoothing factor for RTT. Range (0, 1].
	// Higher = reacts faster to changes; lower = smoother.
	// Default: 0.15
	double rttSmoothingAlpha = 0.15;

	// EMA smoothing factor for jitter (absolute RTT deviation).
	// Default: 0.15
	double jitterSmoothingAlpha = 0.15;

	// Safety margin: prediction offset is increased by jitterMultiplier * smoothedJitter.
	// Larger values reduce late-arrival miss-predictions at the cost of extra input lag.
	// Default: 2.0
	double jitterMultiplier = 2.0;

	// -------------------------------------------------------------------------
	// Drift correction (ClientPredictionClock)
	// -------------------------------------------------------------------------

	// Ticks of drift below which no CLIENT CLOCK correction is applied (dead-band).
	// Default: 3
	uint32_t softDriftThresholdTicks = 3;

	// Ticks of drift above which a hard resync is triggered:
	//   CLIENT CLOCK  — prediction tick jumps to the target tick immediately.
	// Default: 15
	uint32_t hardResyncThresholdTicks = 15;

	// In the graduated correction zone (soft < drift <= hard):
	// apply a CLIENT CLOCK-only correction (one tick skip or stall) every N frames.
	// Default: 4
	uint32_t gradualCorrectionRate = 4;

	// Number of prediction ticks that must elapse before drift correction
	// is evaluated. Prevents spurious corrections during startup.
	// Default: 60
	uint32_t minTicksBeforeDriftCheck = 60;

	// -------------------------------------------------------------------------
	// Tick frequency
	// -------------------------------------------------------------------------

	// Physics ticks per second. Set from solver->GetAsyncDeltaTime() at construction:
	//   config.tickFrequency = 1.0 / asyncDeltaTime
	// Default: 60.0
	double tickFrequency = 60.0;
};
