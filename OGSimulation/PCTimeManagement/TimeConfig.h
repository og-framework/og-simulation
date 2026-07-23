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
// An ordering test asserts this strict inequality.
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
	// satisfy `hardResyncThresholdTicks > rollbackWindowHardCap`.
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
	// Derived from the Quantum formula on OGBrawler's cellular profile.
	// Default: 12
	int32_t rollbackWindowTicks = 12;

	// Degraded-mobile maximum that C.2 tier escalation can raise the soft cap to.
	// `rollbackWindowTicks` may grow up to this ceiling on poor connections.
	// For the failsafe backstop, see `hardResyncThresholdTicks`.
	// Default: 20
	int32_t rollbackWindowHardCap = 20;

	// -------------------------------------------------------------------------
	// Input redundancy (FInputRedundancyBundle)
	// -------------------------------------------------------------------------

	// Slot count for FInputRedundancyBundle; tracks the runtime tick rate
	// (3 at 60 Hz, 5 at 100 Hz). The runtime tick rate is set in
	// Config/DefaultEngine.ini AsyncFixedTimeStepSize.
	// Default: 3
	int32_t redundancyDepthTicks = 3;

	// -------------------------------------------------------------------------
	// Test harness mode selector (Catch2 determinism harness)
	// -------------------------------------------------------------------------

	// Catch2 determinism-harness mode.
	//   Production   — default; the production-shipped test surface.
	//   DevTest      — opt-in, heavier CI-only determinism runs.
	//   KU1CrossArch — opt-in cross-architecture hash-log verification.
	enum class HarnessMode { Production, DevTest, KU1CrossArch };

	// Active harness mode.
	// Default: HarnessMode::Production
	HarnessMode harnessMode = HarnessMode::Production;

	// -------------------------------------------------------------------------
	// C.2 tiered input delay (Layer-1 latency hiding) — Stage 5
	//
	// The RTT tier is SERVER-AUTHORITATIVE (C1 decision, Option A, 2026-07-19):
	// the server derives each connection's tier from its own per-connection RTT
	// sample and replicates the tier index to the owning client. The client does
	// NOT compute its own tier — it consumes the replicated value. This preserves
	// the codebase's existing single-source-of-truth pattern (server owns the
	// authority tick; client derives from it) rather than introducing a second,
	// independently-drifting estimator on the client.
	//
	// All four tier arrays are indexed by the SAME tier index 0..3, so entry N of
	// every array describes the same connection quality bucket. Keeping them as
	// parallel arrays (rather than an array-of-struct) matches the proposal §11
	// appendix layout and keeps each row independently tunable from config.
	// -------------------------------------------------------------------------

	// Baseline Layer-1 input-delay in ticks, applied when NO per-connection tier
	// is available: before the first authoritative tier replicates to a client,
	// and for any Address the server has never sampled. Once a tier IS known, the
	// per-tier value in `rttTierInputDelays` REPLACES this baseline — the two are
	// NOT additive (locked 2026-07-19, backlog C2; the earlier additive draft
	// algebraically cancelled to a constant).
	// Deliberately trading a small constant input lag for the ability to absorb
	// short network hiccups without a visible re-simulation pop.
	// Default: 2
	int32_t forcedInputLatencyTicks = 2;

	// Inclusive UPPER bounds, in milliseconds, of each RTT tier bucket. Entry N is
	// the highest smoothed RTT that still counts as tier N, so the buckets are
	// [0,30] [31,80] [81,150] [151,999]. The final entry is a SENTINEL chosen far
	// above any playable RTT — a connection worse than tier 3 has no worse tier to
	// escalate into, so tier 3 must be an open-ended catch-all.
	// MUST be strictly increasing; TimeConfigTierArrayOrderingTest asserts this.
	// A non-monotonic table would make the tier lookup order-dependent and could
	// strand a connection in a tier it can never leave.
	// Default: { 30, 80, 150, 999 }
	int32_t rttTierBoundariesMs[4] = { 30, 80, 150, 999 };

	// Per-tier Layer-1 input delay, in ticks. Indexed by tier index 0..3. This IS
	// the effective input delay once a tier is known (see `forcedInputLatencyTicks`
	// for the no-tier fallback). Worse tiers buy more delay, which hides more of
	// the network round-trip behind the local input latency.
	// MUST be monotonically non-decreasing — a worse connection must never get a
	// SHORTER delay, which would defeat the escalation entirely.
	// Default: { 1, 2, 3, 4 }
	int32_t rttTierInputDelays[4] = { 1, 2, 3, 4 };

	// Per-tier ceiling that `rollbackWindowTicks` (the SOFT cap — see the ADR block
	// at the top of this file) may escalate to. Indexed by tier index 0..3. Worse
	// connections are permitted a deeper resim before the window clamps.
	// MUST be monotonically non-decreasing, and the LAST entry must be <=
	// `rollbackWindowHardCap` — tier 3 cannot escalate past the absolute failsafe,
	// otherwise the soft cap would overtake the hard cap and the bounded-depth
	// ordering invariant (clamp before snap) would silently invert.
	// Default: { 6, 9, 12, 20 }
	int32_t rttTierRollbackCeilings[4] = { 6, 9, 12, 20 };

	// R-A2 mitigation. Directional dead-band, in milliseconds, applied around each
	// tier boundary: a connection promotes only when smoothed RTT exceeds
	// (boundary + this), and demotes only when it falls below (boundary - this).
	// Without the band, an RTT hovering exactly on a boundary would flap between
	// two tiers every sample, and each flip changes the effective input delay —
	// which the player feels directly as stuttering control latency.
	// Default: 10
	int32_t tierHysteresisMs = 10;

	// R-A2 mitigation companion. Minimum ticks a connection must remain in its
	// current tier before ANY further transition is allowed, however far the RTT
	// has moved. The hysteresis band alone stops boundary-noise flapping but not a
	// genuinely oscillating connection; this dwell floor bounds how often the
	// player-visible input delay is allowed to change at all.
	// Default: 60 (= 1 second at the 60 Hz `tickFrequency` target)
	int32_t tierMinDwellTicks = 60;

	// When true, the render-side input echo (C.4) is suppressed for connections in
	// the WORST tier (tier 3), where the echo would be predicting far enough ahead
	// to be visibly wrong more often than it is right.
	// NO CONSUMER UNTIL OPTIONAL TASK T15 — the field exists now purely so the R-P1
	// configurability lint has a home for the proposal-named constant and Stage-5
	// work cannot hardcode it. Nothing reads this value today, so the default is
	// NOT load-bearing; the keep-true-vs-flip-false question is deliberately
	// deferred to T15, when it can be judged against real gameplay feel.
	// Default: true
	bool muteEchoOnDegradedTier = true;

	// LAN / arcade-cabinet escape hatch: when true, a tier-0 connection gets ZERO
	// input delay instead of `rttTierInputDelays[0]`. On a sub-millisecond local
	// link there is no round-trip to hide, so any forced delay is pure added input
	// lag with no benefit. Only tier 0 is affected — a bad connection on a LAN
	// session still gets its tier's delay.
	// Default: false
	bool lanZeroDelayOverride = false;

	// -------------------------------------------------------------------------
	// Stage 4 observability — FIELDS ONLY, NO RUNTIME CONSUMER THIS INITIATIVE
	//
	// Every constant the proposal names must exist as a TimeConfig field even
	// before its consumer ships (risks_and_plan §6 configurability rule). If the
	// field did not exist here, the Stage 4 initiative would inevitably hardcode
	// the literal at its use site and the R-P1 lint could not catch it — which is
	// exactly the second-source-of-truth failure this rule exists to prevent.
	// The consumers for everything in this section land in the Stage 4
	// observability initiative unless noted otherwise.
	// -------------------------------------------------------------------------

	// How often the server broadcasts the SN-1 (last-known-authoritative) state.
	//   EveryTick  — broadcast on every server tick; highest bandwidth.
	//   SparseIdle — broadcast every `sn1IdleBroadcastIntervalTicks` while the
	//                simulation is idle, every tick otherwise.
	//   RttTiered  — broadcast cadence derived from the connection's RTT tier.
	enum class Sn1BroadcastPolicy { EveryTick, SparseIdle, RttTiered };

	// Active SN-1 broadcast policy. No runtime consumer until Stage 4.
	// Default: Sn1BroadcastPolicy::RttTiered
	Sn1BroadcastPolicy sn1BroadcastPolicy = Sn1BroadcastPolicy::RttTiered;

	// Tick interval between SN-1 broadcasts while idle under
	// `Sn1BroadcastPolicy::SparseIdle`. No runtime consumer until Stage 4.
	// Default: 6
	int32_t sn1IdleBroadcastIntervalTicks = 6;

	// How often the server broadcasts the state checksum used for desync
	// detection.
	//   EveryTick   — checksum every tick; fastest divergence detection.
	//   EveryNTicks — checksum every `hashBroadcastIntervalTicks` ticks.
	//   Off         — no checksum broadcast (bandwidth-constrained sessions).
	enum class HashBroadcastPolicy { EveryTick, EveryNTicks, Off };

	// Active hash broadcast policy. No runtime consumer until Stage 4.
	// Default: HashBroadcastPolicy::EveryTick
	HashBroadcastPolicy hashBroadcastPolicy = HashBroadcastPolicy::EveryTick;

	// Tick interval between checksum broadcasts under
	// `HashBroadcastPolicy::EveryNTicks`. No runtime consumer until Stage 4.
	// Default: 1
	int32_t hashBroadcastIntervalTicks = 1;

	// Number of CONSECUTIVE mismatching checksum ticks before a run of hash
	// mismatches is treated as a confirmed divergence rather than transient noise.
	// A single mismatch can be an in-flight ordering artifact; a sustained run
	// cannot. Consumed by `shouldEscalateToLayer2` / IDesyncDiagnosticSink (T8).
	// Default: 5
	int32_t hashMismatchTickThreshold = 5;

	// What to do once `hashMismatchTickThreshold` consecutive mismatches confirm a
	// divergence.
	//   LogOnly            — record it and keep running (default; safest).
	//   EscalateToLayer2   — fall back to the heavier correction layer.
	//   DisconnectAfterRun — drop the diverged client.
	enum class HashMismatchReaction { LogOnly, EscalateToLayer2, DisconnectAfterRun };

	// Active hash-mismatch reaction. Consumed by T8's sink boundary.
	// Default: HashMismatchReaction::LogOnly
	HashMismatchReaction hashMismatchReaction = HashMismatchReaction::LogOnly;

	// When true, state snapshots are saved sparsely (only at correction-relevant
	// ticks) instead of every tick, trading resim cost for memory.
	// No runtime consumer until Stage 4.
	// Default: false
	bool sparseSaveMode = false;

	// Telemetry recording toggles for the four bounded-depth / input events. The
	// three that default true are cheap and diagnostically load-bearing; redundancy
	// HITS are high-volume and off by default because a healthy session produces
	// them constantly, which would drown the log.
	// No runtime consumer until Stage 4.
	// Default: true
	bool recordSkipEvents = true;

	// See `recordSkipEvents`. No runtime consumer until Stage 4.
	// Default: true
	bool recordStallEvents = true;

	// See `recordSkipEvents`. No runtime consumer until Stage 4.
	// Default: true
	bool recordSubstitutionEvents = true;

	// See `recordSkipEvents` — high-volume, hence off by default.
	// No runtime consumer until Stage 4.
	// Default: false
	bool recordRedundancyHits = false;

	// When true, input bundles from sibling local players on the same connection
	// are aggregated into a single wire payload instead of one bundle each.
	// Phase-2 bandwidth optimisation; config-ready flag only.
	// No runtime consumer until Stage 4 / Stage 7.
	// Default: false
	bool aggregateSiblingInputBundles = false;

	// Capacity, in ticks, of the rolling checksum ring log kept for post-mortem
	// desync analysis. Sized to hold the last 10 seconds at the 60 Hz
	// `tickFrequency` target — long enough that a human noticing a desync can
	// still capture the window in which it started.
	// NO RUNTIME CONSUMER UNTIL THE STAGE 4 INITIATIVE — the field lands here so
	// the Stage 4 ring-log implementation reads it instead of hardcoding 600.
	// Default: 600
	int32_t hashLogRingCapacity = 600;
};
