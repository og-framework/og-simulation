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
//               *** THIS BULLET IS INTENDED DESIGN, NOT SHIPPED BEHAVIOUR —
//               see the status note below before relying on it. ***
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
// ---------------------------------------------------------------------------
// STATUS NOTE — the RollbackWindow CLAMP is INTENDED, NOT IMPLEMENTED
// (recorded 2026-08-04; og-netcode-v2-input-relay T13)
//
// "The client clamps to the window and accepts a partial resim" describes an
// INTENDED mechanism that does not exist in the shipped code. Established by an
// exhaustive code trace (input-relay review finding A1; ruling recorded in
// RelayDelaySpectrumDesign.md §7):
//
//   - `SimulationReconciliation` contains NO window or clamp logic. The resim
//     span is "newest landed correction -> prediction frontier"; depth is
//     bounded only implicitly, by correction-ring capacity.
//   - The only PRODUCTION consumer of `rollbackWindowTicks` is the SERVER-side
//     late-input future-guard context (`SimulationManager.h`) — a different
//     mechanism from the client clamp described above.
//   - Client-side, `rollbackWindowHardCap` appears solely as a LOG GATE
//     (`CorrectionCache.h`). It gates a diagnostic; it clamps nothing.
//
// The three Stall / Skip / HardResync bullets DO describe shipped behaviour;
// only the RollbackWindow clamp is aspirational.
//
// Building the clamp is genuine DESIGN work, not wiring — the site (naturally
// the resim-anchor selection) and the partial-resim semantics both have to be
// specified. It is deliberately DEFERRED to the sparse-state increment, where
// corrections stop landing every tick and deep resims become real. While state
// replicates every frame the need is theoretical: resim depth stays ~= client
// lead + downlink, comfortably under every configured ceiling.
// ---------------------------------------------------------------------------
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
	//
	// NOTE (T26a): this floor is ALSO what the no-RTT-sample path of
	// getPredictionOffsetTicks() returns. It is a structural invariant guard
	// ("the client predicts forward"), not an estimate, so it holds whether or
	// not an RTT estimate exists yet. See NetworkTimeEstimator.cpp.
	uint32_t predOffsetFloorTicks = 4;

	// -------------------------------------------------------------------------
	// Outlier RTT rejection (NetworkTimeEstimator) — og-netcode-v2-input-relay T26b
	// -------------------------------------------------------------------------
	// WHAT THESE FIVE FIELDS EXIST FOR. UE measures RTT as
	// `CurrentTime - OutLagTime[Index]` with `CurrentTime = FApp::GetCurrentTime()`
	// — FRAME-START time. A frame hitch delays ack processing and that delay lands
	// straight in the sample, so a loopback session with 5-10 ms emulated lag can
	// report a ~1-SECOND RTT purely because the game thread stalled. Pre-T26b
	// updateRTT believed it unconditionally: jitterMultiplier doubled the
	// excursion into the offset and jitterSmoothingAlpha made recovery take
	// seconds. The engine hands us an obviously-bad number; the estimator's job is
	// to not believe it. Full rationale, including why the gate is one-sided and
	// what it costs, lives at the gate itself in NetworkTimeEstimator.cpp.
	//
	// The mechanism is NOT a clamp: an implausible sample is REJECTED (it does not
	// move either EMA), matching the T21 validity gate's doctrine in the same
	// function. `rttOutlierConsecutiveLimit` is what stops that being a bug — see
	// its comment.

	// Multiplicative half of the plausibility bound: a sample is implausible when
	// it exceeds `rttOutlierMultiplier * smoothedRTT + rttOutlierMarginSeconds`.
	// 4x is deliberately loose — this gate is aimed at 100x frame-hitch artifacts,
	// not at trimming ordinary jitter, and anything under 4x the current estimate
	// is inside what jitterMultiplier * smoothedJitter is already there to cover.
	// Default: 4.0
	double rttOutlierMultiplier = 4.0;

	// Additive half of the same bound. Without it the multiplicative term collapses
	// on LAN: at a 0.5 ms smoothed RTT, 4x is 2 ms, which would reject an entirely
	// ordinary 10 ms reading. 30 ms is above any plausible loopback/LAN excursion
	// and far below the ~1 s artifacts being filtered.
	// Default: 0.030 (seconds)
	double rttOutlierMarginSeconds = 0.030;

	// COLD START. The first sample seeds m_smoothedRTT VERBATIM, so the relative
	// bound above has nothing to compare against; the seed is instead gated by
	// this absolute ceiling. Chosen over accept-then-correct because T21 showed the
	// seed is the single most damaging value in the estimator — it is latched, and
	// every subsequent jitter delta is measured against it. A genuinely >500 ms
	// link is NOT locked out: rttOutlierConsecutiveLimit re-seeds it after a short
	// run of consistent readings, so this ceiling costs a fraction of a second on
	// such links and rejects a hitch-inflated first reading outright.
	// Default: 0.5 (seconds)
	double rttOutlierColdStartCeilingSeconds = 0.5;

	// THE ESCAPE HATCH — the field that makes this a filter rather than a bug.
	// A filter that never lets the estimate move cannot follow a GENUINE step
	// change in network conditions. After this many CONSECUTIVE implausible
	// samples the estimator concludes the level really has moved and RE-SEEDS
	// (smoothedRTT = the sample, jitter = 0), exactly as it would on a first
	// sample. A single hitch is isolated and never reaches the limit; a real step
	// is sustained and always does. ~30 samples is ~0.3 s at the 100 Hz timing
	// relay — longer than a hitch's inflated-ack backlog, short enough that a real
	// step is absorbed in well under a second.
	// 0 or 1 DISABLES the filter (the first implausible sample is accepted
	// immediately), which is the degenerate setting for A/B measurement.
	// Default: 30
	uint32_t rttOutlierConsecutiveLimit = 30;

	// OBSERVABILITY. A silent reject hides a genuine RTT step change exactly as
	// well as it hides a hitch artifact, so rejections are counted per window and
	// summarised in ONE log line per window that contains any. Sized to bound log
	// volume at roughly one line per 6 s at the 100 Hz timing relay even if the
	// ping source misbehaves continuously. 0 disables the window summary; the
	// running totals stay available through the estimator's accessors either way.
	// Default: 600 (samples)
	uint32_t rttOutlierLogWindowSamples = 600;

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
	// (That "primary clamping" is INTENDED, NOT IMPLEMENTED — see the ADR status
	// note at the top of this file. This backstop IS live and, today, the only
	// shipped bound of the three.)
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
	//
	// THE CLIENT-SIDE CLAMP IS INTENDED, NOT IMPLEMENTED (ADR status note at the
	// top of this file; ruling in RelayDelaySpectrumDesign.md §7). The field IS
	// live — but its only production consumer is the SERVER-side late-input
	// future-guard context, not a client resim clamp. Deferred to the
	// sparse-state increment.
	// Default: 12
	int32_t rollbackWindowTicks = 12;

	// Degraded-mobile maximum that C.2 tier escalation can raise the soft cap to.
	// `rollbackWindowTicks` may grow up to this ceiling on poor connections.
	// For the failsafe backstop, see `hardResyncThresholdTicks`.
	//
	// THE ESCALATION THIS BOUNDS IS INTENDED, NOT IMPLEMENTED (ADR status note at
	// the top of this file; RelayDelaySpectrumDesign.md §7): nothing raises
	// `rollbackWindowTicks` toward this ceiling today. Client-side this value is
	// read only as a LOG GATE (`CorrectionCache.h`) and as T26's deliberate flat
	// release gate — both real, neither a clamp.
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
	// Outbound input relay (FRelayedInputRing) — og-netcode-v2-input-relay
	// -------------------------------------------------------------------------

	// Entry count of the OUTBOUND relay ring — how many of a character's most
	// recent (captureTick, dA, input) entries the server keeps replicated to the
	// other clients. NOT the same knob as `redundancyDepthTicks` above: that one
	// sizes the INBOUND client->server redundancy bundle. The two directions have
	// different payloads and different failure modes and are tuned separately.
	//
	// SIZING RULE (RelayDelaySpectrumDesign.md §8.2): once the delay floor is
	// above zero, a peer READS this ring on a schedule (`captureTick + dA`), so
	// every capture tick that never made it onto the wire between two successful
	// replications is a scheduled MISS. Therefore
	//
	//     depth >= (measured ticks between successful relay replications) + margin
	//
	// That measurement does not exist yet — the T9 relay-cadence probe produces it
	// (its acceptance criterion is to emit a measured `depth >= gap_p99 + margin`
	// recommendation). UNTIL THEN THE DEFAULT IS 1: at floor 0 the scheduled read
	// nearly always misses anyway and falls back to the peer's last-known input,
	// which is the degenerate, no-bandwidth-increase behaviour this increment
	// ships. Do not raise this on intuition — raise it against the probe's number.
	//
	// DELIBERATELY INDEPENDENT OF `relayDelayFloorTicks` (§11 Q3): the floor sets
	// how far ahead inputs are scheduled, this sets how much history survives a
	// replication gap. A `depth = f(floor)` derivation was considered and REJECTED
	// for now — it cannot be written honestly before the T9 cadence measurement
	// exists. Raising the floor without raising this simply means more scheduled
	// misses (self-healed by the next correction), not a correctness break.
	//
	// Clamped to [1, relayedInputRing::kMaxDepth = 8] at the write site: 0 would
	// silently disable the relay, and the upper bound is the per-character wire
	// budget (RelayedInputRingCodec.h).
	// Default: 1
	int32_t relayRedundancyDepthTicks = 1;

	// THE FLOOR LEVER — the session-scoped minimum effective Layer-1 input delay,
	// in ticks. (RelayDelaySpectrumDesign.md §3, §6, §10, §11 Q1/Q2/Q5.)
	//
	// WHAT IT BUYS. A peer can only simulate a relayed input AT its scheduled tick
	// if the input reached that peer before the peer's frontier got there. The
	// trip left to cover is a property of the RECEIVER (§3.2):
	//
	//     D >= lead_B + downlink_B  ~=  RTT_B (+ jitter/wobble margin)
	//
	// The per-connection tier derives its delay from the SENDER's wire, which is
	// the wrong variable for receiver coverage — hence this separate, session-wide
	// quantity. Raising it moves the whole session along the spectrum from
	// "extrapolate + correct" toward "everyone applies the same input on the same
	// tick"; the price is that EVERY player's own felt input lag rises to at least
	// this many ticks (§6). That is why the knob defaults to 0 and is tuned by
	// playtest, not by intuition.
	//
	// SIZING GUIDE at 60 Hz (§3.3; m = jitter + frontier-wobble margin, ~2-3):
	//
	//     cover-to-RTT:  30 ms -> >= 2+m    80 ms -> >= 5+m    150 ms -> >= 9+m
	//
	// so "the floor covers decent connections" lands around 7-8. NO PRESET SHIPS:
	// exact values wait on the T9 relay-cadence probe and playtest (§11 Q2).
	//
	// HOW IT COMPOSES. It is a MAX, never a sum, and it is applied at every site
	// that derives an effective input delay, through the ONE shared helper
	// `applyRelayDelayFloor` (ConnectionTierTable.h):
	//
	//     effective = max(relayDelayFloorTicks, <tier-or-fallback value>)
	//
	// A nonzero floor therefore DOMINATES `lanZeroDelayOverride` as well — see the
	// note at that field.
	//
	// UNIFORM-D FAIRNESS MODE (§11 Q5) is a config VALUE, not a feature: once
	// `relayDelayFloorTicks >= max(rttTierInputDelays)` every derivation path —
	// tier, LAN override, and the no-tier `forcedInputLatencyTicks` fallback —
	// collapses to exactly this value, so every sender is scheduled with the same
	// D and no player is advantaged by their connection.
	//
	// CLAMPED, and the ceiling is DERIVED, not a literal (review finding A5):
	// effective values are capped at
	// `kClientInputDelayLineCapacityTicks - rollbackWindowHardCap` (= 64 - 20 = 44
	// at current defaults). Beyond that cap the client's own capture for the
	// scheduled tick has already been evicted from `ClientInputDelayLine` before
	// it can be consumed, and the whole scheduled regime silently degenerates
	// instead of failing loudly. The clamp is enforced at BOTH intake points (the
	// ini override at the composition root and the client's floor OnRep) and once
	// more on every read inside `applyRelayDelayFloor` — the same belt-and-braces
	// shape `clampConnectionTierIndex` uses for the replicated tier.
	//
	// SOURCE OF TRUTH + DISTRIBUTION. The SERVER owns the value (optionally
	// overridden from `[OGNetcode] RelayDelayFloorTicks` in the ini at the
	// composition root) and replicates it to every client as its own uint8
	// UPROPERTY on `ASimulationTimingRelay` — session-scoped state on the
	// session-scoped vehicle. Clients never derive it locally; a client's copy of
	// this field is written from that OnRep. §11 Q6's deferred dynamic-floor policy
	// needs no new mechanism: it writes this field and the property again.
	//
	// RELATED KNOB: `relayRedundancyDepthTicks` above. Deliberately independent
	// (§11 Q3) — the floor sets how far ahead inputs are scheduled, the depth sets
	// how much history survives a replication gap.
	// Default: 0 (degenerate — today's behaviour, byte-for-byte)
	int32_t relayDelayFloorTicks = 0;

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
	//
	// C2 AMENDMENT (2026-08-03, RelayDelaySpectrumDesign.md §6/§9): every effective
	// input delay derived below is FLOORED by `relayDelayFloorTicks` —
	// `max(floor, tier-or-fallback)` — through the single shared helper
	// `applyRelayDelayFloor`. The tier remains the per-wire quantity; the floor is
	// the session-wide receiver-coverage minimum. At the shipped floor of 0 the
	// max is the identity, so everything documented in this section is unchanged.
	// -------------------------------------------------------------------------

	// Baseline Layer-1 input-delay in ticks, applied when NO per-connection tier
	// is available: before the first authoritative tier replicates to a client,
	// and for any Address the server has never sampled. Once a tier IS known, the
	// per-tier value in `rttTierInputDelays` REPLACES this baseline — the two are
	// NOT additive (locked 2026-07-19, backlog C2; the earlier additive draft
	// algebraically cancelled to a constant).
	//
	// C2 AMENDED (RelayDelaySpectrumDesign.md §6 / §9, 2026-08-03). The replacement
	// rule is now floored by the session lever:
	//
	//     effectiveDelay = max(relayDelayFloorTicks, <tier-or-fallback value>)
	//
	// so THIS baseline is itself floored on the no-tier path. Both no-tier
	// fallback sites (`ServerInputDelayQueue::effectiveDelay` and
	// `ReplicatedTierConsumer::effectiveInputDelayTicks`) route this field through
	// `applyRelayDelayFloor`; with the default floor of 0 the max is the identity
	// and the pre-amendment value is preserved exactly.
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
	// for the no-tier fallback), subject to the session floor: the C2 replacement
	// rule reads `max(relayDelayFloorTicks, rttTierInputDelays[tier])` since the
	// 2026-08-03 amendment, applied inside `tierInputDelayTicks`. Worse tiers buy
	// more delay, which hides more of
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
	//
	// INTENDED API, NO CONSUMER — the escalation described above is INTENDED, NOT
	// IMPLEMENTED (ADR status note at the top of this file; ruling in
	// RelayDelaySpectrumDesign.md §7). This array is defined, ordering-tested and
	// replicated to the client, but the client-side soft clamp that would read it
	// does not exist, so it has ZERO production call sites. Its three lookup
	// helpers — `tierRollbackCeiling` / `lookupRollbackCeiling`
	// (ConnectionTierTable.h) and `effectiveRollbackCeiling`
	// (ReplicatedTierConsumer.h) — are unconsumed for the same reason. Wiring is
	// deferred to the sparse-state increment; this is kept (not deleted) because
	// the R-P1 configurability rule requires every proposal-named constant to
	// have a TimeConfig home before its consumer ships.
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
	//
	// DOMINATED BY A NONZERO `relayDelayFloorTicks` (RelayDelaySpectrumDesign.md
	// §6). The floor is applied AFTER this branch, so `max(floor, 0) == floor`
	// wins. That is the correct precedence, not a conflict: on a pure-LAN session
	// the floor is configured 0 and this override behaves exactly as before, while
	// on a MIXED session a LAN sender must still be schedulable by WAN receivers —
	// and a sender applying its own input at capture+0 while its peers schedule it
	// at capture+floor is precisely the two-ends-disagree bug the floor exists to
	// prevent. See `tierInputDelayTicks` in ConnectionTierTable.h, where the
	// ordering is implemented.
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
