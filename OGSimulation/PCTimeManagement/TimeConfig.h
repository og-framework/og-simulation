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

// ===========================================================================
// ADR — Bounded-depth prediction: Stall / Skip / RollbackWindow / HardResync
// ===========================================================================
// These four mechanisms together bound how far client prediction may diverge
// from the authoritative server. They are easy to conflate, so the 4-way
// interaction is spelled out explicitly (proposal §4.3):
//
//   * Stall   — client too far AHEAD of the server tick. The client pauses one
//               sim sub-step to let the server catch up. (Existing OGSim
//               behaviour; bounded stalls are preferred over unbounded
//               rollback under cellular packet-loss bursts.)
//   * Skip    — client BEHIND the server tick. The client advances multiple
//               ticks in one display frame to catch up. (Existing OGSim
//               behaviour, kept as a binary catch-up.)
//   * RollbackWindow — SOFT cap on client prediction/resim depth (the primary
//               circuit-breaker). When a server correction would require
//               resimulating more than `rollbackWindowTicks` ticks, the client
//               clamps to the window and accepts a partial resim (older ticks
//               beyond the ring-buffer window are not corrected). Degraded
//               mobile may raise this up to `rollbackWindowHardCap`.
//   * HardResync — ABSOLUTE FAILSAFE BACKSTOP. The legacy drift threshold,
//               repurposed: it fires only when the soft cap has failed and the
//               client ends up further adrift than `rollbackWindowHardCap`
//               (off-the-rails packet loss, multi-second freeze, dev pause).
//               It snaps the clock and wipes the cache; expected very rarely.
//
// Ordering invariant: `hardResyncThresholdTicks > rollbackWindowHardCap` so the
// failsafe always fires strictly LATER than the soft cap (clamp before snap).
// T6's R-D3 ordering test asserts this strict inequality.
//
// See OGBrawlerNetworkModelResearch/arch/proposal_ogbrawler_netcode.md §4 for
// the bounded-depth design rationale.
// ===========================================================================

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

	// Minimum floor for NetworkTimeEstimator::getPredictionOffsetTicks(). Guarantees
	// the client's prediction target sits at least this far ahead of the last known
	// authority tick, keeping the dead-band lower bound at or above authorityTick + 1
	// and preserving the "client predicts forward" invariant on LAN. Without this
	// floor, sub-millisecond RTT rounds predOffset to 0-1 ticks and the softDrift
	// dead band locks the client at or behind authority in perpetuity. Prevents the
	// LAN late-connect corner case documented in
	// ../og-brawler-hit-resolution/netcode_finding_pred_offset_floor.md. On WAN /
	// cellular with real RTT the natural rawOffset already exceeds this floor and the
	// floor is a no-op (verified: at 50 ms RTT + 5 ms jitter → rawOffset = 3.6 → ceil = 4
	// = floor; at 150 ms cellular → rawOffset = 12.6 → ceil = 13, floor irrelevant).
	// Default: 4 (= softDriftThresholdTicks + 1 at current defaults; guarantees
	// dead-band lower bound sits at authorityTick + 1).
	uint32_t predOffsetFloorTicks = 4;

	// -------------------------------------------------------------------------
	// Drift correction (ClientPredictionClock)
	// -------------------------------------------------------------------------

	// Ticks of drift below which no CLIENT CLOCK correction is applied (dead-band).
	// Default: 3
	uint32_t softDriftThresholdTicks = 3;

	// Ticks of drift above which a hard resync is triggered:
	//   CLIENT CLOCK  — prediction tick jumps to the target tick immediately.
	// FAILSAFE BACKSTOP ONLY — primary clamping is `rollbackWindowTicks` /
	// `rollbackWindowHardCap`; this fires only when the soft cap fails. MUST
	// satisfy `hardResyncThresholdTicks > rollbackWindowHardCap` (T6 asserts this).
	// See `rollbackWindowTicks` / `rollbackWindowHardCap` for the primary caps.
	// Default: 21
	uint32_t hardResyncThresholdTicks = 21;

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

	// -------------------------------------------------------------------------
	// Bounded-depth prediction (RollbackWindow) — see ADR block at top of file
	// -------------------------------------------------------------------------

	// Soft cap on client resim depth — the primary prediction circuit-breaker.
	// When a server correction would require resimulating more than this many
	// ticks, the client clamps to the window and accepts a partial resim.
	// Derived from the Quantum formula on OGBrawler's cellular profile
	// (proposal §4.2, §11; Synthesis §B Axis b Gate).
	// Default: 12
	int32_t rollbackWindowTicks = 12;

	// Degraded-mobile maximum that C.2 tier escalation can raise the soft cap to.
	// `rollbackWindowTicks` may grow up to this ceiling on poor connections
	// (proposal §4.2, §11). For the failsafe backstop, see `hardResyncThresholdTicks`.
	// Default: 20
	int32_t rollbackWindowHardCap = 20;

	// -------------------------------------------------------------------------
	// Input redundancy (FInputRedundancyBundle)
	// -------------------------------------------------------------------------

	// Slot count for FInputRedundancyBundle. Default tracks the runtime tick rate:
	// 3 at the 60 Hz ratified target (active default), 5 at the 100 Hz interim
	// (historical, pre-Stage-2). Per proposal §11.
	// (Stage 2 flipped this default 5 -> 3 when the runtime moved to 60 Hz; the
	// runtime tick rate is set in Config/DefaultEngine.ini AsyncFixedTimeStepSize.)
	// Default: 3
	int32_t redundancyDepthTicks = 3;

	// -------------------------------------------------------------------------
	// Test harness mode selector (Catch2 determinism harness)
	// -------------------------------------------------------------------------

	// Catch2 determinism-harness mode (proposal §9.1, §11).
	//   Production   — default; the production-shipped test surface.
	//   DevTest      — opt-in, heavier CI-only determinism runs.
	//   KU1CrossArch — opt-in cross-architecture hash-log verification.
	enum class HarnessMode { Production, DevTest, KU1CrossArch };

	// Active harness mode.
	// Default: HarnessMode::Production
	HarnessMode harnessMode = HarnessMode::Production;
};
