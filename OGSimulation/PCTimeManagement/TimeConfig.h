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
// ORIENTATION — WHAT THIS STRUCT IS, WHO WRITES IT, AND HOW TO READ A DEFAULT
//
// Read this first. Every fence below states one rule at the field it guards;
// none of them restates this map, and this map states no rule.
//
// The narrative, the derivations, the measurement records and the retired
// mechanisms are in `docs/TimeConfig-rationale.md`; `§N` points into it.
//
//   * WHAT IT IS. Every tunable parameter of the PCTM time-management system,
//     as a plain POD struct — copy freely. One instance per session, set once
//     at startup and passed by const-ref into `NetworkTimeEstimator`,
//     `ClientPredictionClock` and the tier helpers.
//
//   * ⛔ A DEFAULT IN THIS FILE IS THE COMPILED DEFAULT. IT IS NOT WHAT SHIPS.
//     The composition root may override a field from the host application's
//     configuration file, and for two fields it does. The two facts are stated
//     TOGETHER in exactly ONE place — at `resimTriggerPolicy` — and nowhere
//     else; every other field states only its compiled default, which is the
//     initialiser you can already see. §6
//
//   * THE TWO KEYS THE SHIPPED CONFIGURATION OVERRIDES, both under the
//     `[OGNetcode]` section, both read at the composition root, both consumed
//     ONCE per session so a change needs a restart:
//
//       `ResimTriggerPolicy`    -> `resimTriggerPolicy`    arms a depth ceiling §3
//       `RelayDelayFloorTicks`  -> `relayDelayFloorTicks`  uniform-D fairness   §7
//
//     ⛔ Read the KEY for the current value — never a comment, and never a line
//     number in that file, which any edit above it silently severs.
//
//   * BOTH ROLES CARRY THE SAME STRUCT. The intake is role-agnostic so the two
//     TimeConfigs stay identical; a field simply has no reader on the role it
//     does not serve. Which role reads what is stated at each field.
//
//   * FOUR MECHANISMS BOUND CLIENT PREDICTION, and they are easy to conflate:
//
//       Stall           client AHEAD of the server: pause one sim sub-step.
//       Skip            client BEHIND: advance several ticks in one frame.
//       RollbackWindow  SOFT depth cap, `rollbackWindowTicks`. The client SKIPS
//                       a too-deep resim anchor; the CLAMP is UNBUILT. §2 §3
//       HardResync      absolute failsafe, `hardResyncThresholdTicks`: snap the
//                       clock, wipe the cache. Expected very rarely.
//
//     ⛔ Ordering invariant: `hardResyncThresholdTicks > rollbackWindowHardCap`,
//     so the failsafe fires strictly LATER than the soft cap — clamp before
//     snap. `TimeConfigOrderingTest.cpp` asserts the strict inequality.
//
//   * WHAT A TEST PINS, and therefore what breaks if you retune:
//
//       `TimeConfigDefaultsTest.cpp`           every default here, by value
//       `TimeConfigOrderingTest.cpp`           the hardResync ordering invariant
//       `TimeConfigTierArrayOrderingTest.cpp`  all four tier-array orderings
//
//   * ⚠ SEVERAL FIELDS HAVE NO PRODUCTION READER, deliberately — see §10 for the
//     rule that puts them here. Each says so at its own declaration. Do not
//     delete one because a grep came back empty.
// ===========================================================================

struct TimeConfig
{
	// -------------------------------------------------------------------------
	// Network estimation (NetworkTimeEstimator)
	// -------------------------------------------------------------------------

	// EMA smoothing factor for RTT, range (0, 1]. Higher reacts faster, lower smoother.
	double rttSmoothingAlpha = 0.15;

	// EMA smoothing factor for jitter (absolute RTT deviation), same range.
	double jitterSmoothingAlpha = 0.15;

	// Safety margin: the offset grows by `jitterMultiplier * smoothedJitter`.
	// ⚠ Larger hides more late arrivals and costs exactly that much input lag.
	double jitterMultiplier = 2.0;

	// Floor, in ticks, for `NetworkTimeEstimator::getPredictionOffsetTicks()`.
	// ⛔ A STRUCTURAL INVARIANT, NOT AN ESTIMATE — "the client predicts forward".
	// Without it a sub-ms RTT locks the client at or behind authority forever. §5
	// ⛔ ALSO the no-RTT-sample return of `getPredictionOffsetTicks()`. §5
	// ⚠ It is `softDriftThresholdTicks + 1`: that puts the dead band above authority.
	uint32_t predOffsetFloorTicks = 4;

	// -------------------------------------------------------------------------
	// Outlier RTT rejection (NetworkTimeEstimator) — the next five fields
	//
	// ⛔ THE HOST STAMPS RTT AT FRAME-START, so a frame hitch lands in the sample —
	// a loopback session can report a ~1-SECOND RTT. These five disbelieve it. §4
	// ⛔ AN IMPLAUSIBLE SAMPLE IS REJECTED, NOT CLAMPED — it moves neither EMA. §4
	// -------------------------------------------------------------------------

	// Multiplicative half of the plausibility bound: implausible when the sample
	// exceeds `rttOutlierMultiplier * smoothedRTT + rttOutlierMarginSeconds`.
	// ⚠ Deliberately loose: aimed at 100x hitch artifacts, not at ordinary jitter. §4
	double rttOutlierMultiplier = 4.0;

	// Additive half of the same bound, in seconds.
	// ⛔ Without it 4x a 0.5 ms LAN RTT is 2 ms, rejecting an ordinary 10 ms reading.
	double rttOutlierMarginSeconds = 0.030;

	// COLD START, in seconds: the absolute ceiling on the FIRST sample.
	// ⛔ The first sample seeds `m_smoothedRTT` VERBATIM, so only this bound gates it. §4
	// ⚠ A slower link is not locked out: `rttOutlierConsecutiveLimit` re-seeds it.
	double rttOutlierColdStartCeilingSeconds = 0.5;

	// THE ESCAPE HATCH — consecutive implausible samples after which the estimator
	// RE-SEEDS (smoothedRTT = the sample, jitter = 0), exactly as on a first sample.
	// ⛔ Without it the filter could never follow a GENUINE step change. §4
	// ⛔ 0 or 1 DISABLES the filter — the degenerate setting for A/B measurement.
	uint32_t rttOutlierConsecutiveLimit = 30;

	// OBSERVABILITY: rejections are counted per window of this many samples and
	// summarised in ONE log line per window that contains any.
	// ⛔ A silent reject hides a real RTT step change as well as it hides a hitch.
	// ⛔ 0 disables the summary; the running totals stay on the estimator's accessors.
	uint32_t rttOutlierLogWindowSamples = 600;

	// -------------------------------------------------------------------------
	// Drift correction (ClientPredictionClock)
	// -------------------------------------------------------------------------

	// Dead band: ticks of drift below which no CLIENT CLOCK correction is applied.
	uint32_t softDriftThresholdTicks = 3;

	// Ticks of drift above which the CLIENT CLOCK jumps straight to the target tick.
	// ⛔ FAILSAFE BACKSTOP ONLY — it fires when the depth policy has already failed.
	// ⛔ MUST satisfy `hardResyncThresholdTicks > rollbackWindowHardCap`. §1
	// ⚠ NOT the only shipped bound: `rollbackWindowTicks` SKIPS today. §2 §3
	uint32_t hardResyncThresholdTicks = 21;

	// In the graduated zone (soft < drift <= hard): one CLIENT-CLOCK-ONLY tick of
	// skip or stall every N frames.
	uint32_t gradualCorrectionRate = 4;

	// Prediction ticks that must elapse before drift correction is evaluated.
	// ⛔ Prevents spurious corrections during startup.
	uint32_t minTicksBeforeDriftCheck = 60;

	// -------------------------------------------------------------------------
	// Tick frequency
	// -------------------------------------------------------------------------

	// Physics ticks per second. Set at construction from the host's fixed physics
	// timestep: `tickFrequency = 1.0 / asyncDeltaTime`.
	double tickFrequency = 60.0;

	// -------------------------------------------------------------------------
	// Bounded-depth prediction (RollbackWindow) — see the ORIENTATION block
	// -------------------------------------------------------------------------

	// SOFT cap on client resim depth, in ticks — the primary circuit-breaker.
	// ⛔ THE CLAMP IS UNBUILT: a deeper anchor is SKIPPED AND COUNTED
	// (`ResimGateWindowSummary::deepAnchorExclusions`), never clamped. §3
	// ⛔ TWO PRODUCTION CONSUMERS — the SERVER late-input future guard
	// (`SimulationManager.h`) and `resimGate::isAnchorWithinDepthPolicy` on the client.
	// ⛔ Consulted ONLY under `resimTriggerPolicy == OnDisagreement`, which ships. §3
	int32_t rollbackWindowTicks = 12;

	// Degraded-mobile ceiling that tier escalation may raise the soft cap to.
	// ⛔ THAT ESCALATION IS INTENDED, NOT IMPLEMENTED — nothing raises the soft cap. §2
	// ⛔ STILL READ ON BOTH ROLES, never as a resim clamp: `CorrectionCache.h`'s
	// log gate and flat release gate, `relayDelayFloorHardCapTicks` (the floor's
	// derived ceiling), and `ServerReceptionCoordinator`'s staleness bounds.
	int32_t rollbackWindowHardCap = 20;

	// -------------------------------------------------------------------------
	// THE RESIM GATE — trigger policy
	//
	// EDGE-TRIGGERED: a landed correction sets a per-character pending resim ANCHOR
	// TICK, and the resim-completion edge consumes it with a CAS. `resimGate::`
	// (OGSimulation/ResimGatePolicy.h) holds the predicates, `StateCorrectionCache`
	// holds the anchor, and ONE field is the whole configurable surface. §6
	// -------------------------------------------------------------------------

	// WHICH LANDED CORRECTIONS SET THE PENDING ANCHOR.
	//   FrontierExact   only a correction landing exactly on the prediction
	//                   frontier. Reproduces the LEGACY gate, and deliberately does
	//                   NOT consult the divergence verdict — the legacy gate never did.
	//   OnDisagreement  any landing whose authority state disagrees with the local
	//                   prediction, wherever it lands. THE DESIGNED TRIGGER.
	enum class ResimTriggerPolicy { FrontierExact, OnDisagreement };

	// ⛔ THE COMPILED DEFAULT AND THE SHIPPED CONFIGURATION ARE TWO SEPARATE FACTS,
	// THEY DIFFER, AND CONFLATING THEM IS A DEFECT THIS TREE HAS REPEATED. Seven
	// sites elsewhere point HERE rather than restate the pair. Both read 2026-08-23:
	//
	//   CODE DEFAULT    `FrontierExact`    anchor: the declaration at the foot of
	//                                      this block. It is what a build with NO
	//                                      configuration override runs.
	//   SHIPPED CONFIG  `OnDisagreement`   anchor: the `ResimTriggerPolicy` KEY
	//                                      under the `[OGNetcode]` section of the
	//                                      host application's configuration file,
	//                                      uncommented. It overrides the code
	//                                      default at runtime, so it is what every
	//                                      run of this project actually uses.
	//                                      (One adapter's binding: that file is
	//                                      `Config/DefaultEngine.ini`.)
	//
	// ⛔ Check that KEY for the current value — never this comment, never a line number.
	// ⛔ Do not restate either value elsewhere in this header; name which one you mean.
	// ⛔ DO NOT FLIP THE COMPILED DEFAULT EARLY "to see what happens" — use §6's arm.
	// ⚠ IT SELECTS A REGIME, NOT A CONDITION: `resimGate::policyEnforcesDepthCeiling`
	// arms `rollbackWindowTicks`' depth ceiling ONLY under `OnDisagreement`. §3
	// ⚠ CLIENT-SIDE ONLY IN EFFECT — `SimulationReconciliation::checkDivergenceAll` is
	// the only consultation, and an authority allocates no correction cache. §6
	// ⚠ Read and applied on BOTH roles so the two TimeConfigs stay identical. §6
	//
	// Sequencing, the measured cost of today's degenerate verdict, and the full
	// argument: `docs/ResimGatePolicy-rationale.md` §3 point 3, §5, §10.
	ResimTriggerPolicy resimTriggerPolicy = ResimTriggerPolicy::FrontierExact;

	// ⛔ THERE IS NO `resimCooldownTicks`, AND ITS ABSENCE IS A RULING, NOT AN
	// OMISSION. (User ruling, 2026-08-11, during the edge-triggered gate's work.)
	//
	// The input-relay design named a trigger-rate ceiling in ticks (placeholder
	// 12). It was built on the four-step config path and then REMOVED, because a
	// rate ceiling defers acting on a correction already KNOWN to disagree with
	// prediction — which is the defect the edge-triggered gate repairs, with a
	// constant. The throttle that replaces it is STRUCTURAL and demand-driven: the
	// gate is consulted only on non-resim physics frames and the anchor is consumed
	// only on the resim-completion edge, so at most one resim is in flight and at
	// most one more is pending, and corrections arriving mid-replay COALESCE into the
	// pending anchor and fire once as a single deeper replay.
	//
	// A correction arriving mid-replay therefore RE-ANCHORS rather than restarting or
	// waiting. Full argument, including why the cost concern is handled by the
	// structural one-resim-per-completed-resim throttle rather than by a rate
	// ceiling (ruled 2026-08-11), and why per-class triggering (not a
	// delay) is the escalation if a measured rate is unaffordable, is at
	// `resimGate::policyEnforcesDepthCeiling`'s neighbouring block in
	// OGSimulation/ResimGatePolicy.h. If a future task reintroduces a cooldown,
	// it must argue against that block, not merely cite the original design.

	// -------------------------------------------------------------------------
	// Input redundancy — the bundle of recent inputs a client re-sends with each
	// capture so a single dropped packet does not lose a tick's input
	// -------------------------------------------------------------------------

	// Slot count for the redundancy bundle; tracks the runtime tick rate (3 at
	// 60 Hz, 5 at 100 Hz).
	// ⛔ That tick rate is the host's physics timestep, set OUTSIDE this library. §9
	int32_t redundancyDepthTicks = 3;

	// -------------------------------------------------------------------------
	// Outbound input relay — the per-character ring of recently captured inputs
	// the authority replicates on to the other peers
	// -------------------------------------------------------------------------

	// ⛔ RETIRED (2026-08-16): there is deliberately no session-configurable
	// retention-depth field here any more (its old identifier is on record at
	// RN-13 in ReviewNotes.md, if a future reader needs it), and its absence is
	// a ruling, not an omission.
	//
	// It used to size the OUTBOUND relay ring's replace-latest write path: how
	// many of a character's most recent (captureTick, dA, input) entries the
	// server kept replicated. Bare-C1 flush-on-poll replaced that write path
	// (`relayedInputRing::stageArrival` / `flushStagedInto`), whose
	// stage capacity is `relayedInputRing::kMaxDepth` — a CONSTANT, taken
	// directly — so the field had named a quantity that no longer existed on the
	// live relay path, and stayed only as an inert, correctly-clamped knob. The
	// retirement removed it, its configuration key, its intake chain and its
	// startup proof line outright.
	//
	// THE TRAP THIS RETIREMENT MUST NOT REOPEN, carried forward rather than
	// re-discovered: wiring ANY depth knob into the flush stage's capacity
	// degenerates bare-C1 back into replace-latest — silently, no assert, no log
	// line — and it cost two separate investigations to find the first time. The
	// retirement ruling declined a tombstone comment at `kMaxDepth` itself
	// (RelayedInputRingCodec.h); the
	// machine-checked fence that replaces it lives in
	// `Network/RelayRedundancyDepthTest.cpp` instead, and it is a compile error
	// (not a silent regression) if a future change gives `stageArrival` a depth
	// parameter to receive one.
	//
	// A genuine redundancy successor is filed and gated on the wire diet — see
	// RN-13 in ReviewNotes.md for the sizing argument. If it ships,
	// it is a NEW, differently-named knob: "entries retained by a write path"
	// and "already-sent ticks re-included in a flush" are different quantities,
	// and reusing this field's old identifier would make every archived
	// measurement that quotes it ambiguous.
	//
	// If a depth knob is ever reintroduced here, argue against this block, not
	// merely against its absence.

	// THE FLOOR LEVER — the session-scoped minimum effective Layer-1 input delay,
	// in ticks. §7
	// ⛔ IT IS A MAX, NEVER A SUM: `max(relayDelayFloorTicks, tier-or-fallback)`, via
	// the ONE shared helper `applyRelayDelayFloor` (ConnectionTierTable.h).
	// ⛔ A NONZERO FLOOR DOMINATES `lanZeroDelayOverride` — see that field.
	// ⛔ RAISING IT RAISES EVERY PLAYER'S OWN FELT INPUT LAG. Tune by playtest. §7
	// ⛔ NO PRESET SHIPS: values wait on the relay-cadence probe and playtest. §7
	// ⛔ CLAMPED at a DERIVED ceiling, `relayDelayFloorHardCapTicks` — beyond it a
	// capture is evicted before its scheduled tick and the regime degenerates silently.
	// ⛔ That clamp runs at BOTH intakes and once more on every read. §7
	// ⛔ THE SERVER OWNS THE VALUE and replicates it as a session-scoped uint8; a
	// client's copy is written from that property's callback, never derived. §7
	// ⚠ UNIFORM-D FAIRNESS MODE is a config VALUE, not a feature: once the floor reaches
	// `max(rttTierInputDelays)` every derivation path collapses to it. §7
	// ⚠ The shipped configuration reaches it; `classifyRelayDelayFloor` logs that. §7
	// ⚠ IT OWNS THE HICCUP-ABSORPTION RATIONALE the retired no-tier field carried. §7
	int32_t relayDelayFloorTicks = 0;

	// THE STATE CADENCE LEVER — how many characters' correction-state buffers
	// `SimulationNetSync::sendCorrectionAll` writes per tick, round-robin. §8
	// ⛔ THE COMPILED DEFAULT IS 1 FOR THE PRE-DIET WINDOW ONLY — the wire diet
	// restores 2 in the same change that deletes `kPreDietCharacterCap`. §8
	// ⛔ A REPLICATION-IMPLEMENTATION PROPERTY, NOT A BYTE BUDGET — a port must
	// RE-CHECK the reason it was lowered, never inherit it. §8
	// ⚠ Each character corrects at `tickFrequency * K / N` Hz; K=1 halves it. §8
	// ⛔ Do NOT special-case K by character count.
	// ⚠ IT REOPENS A DOCUMENTED PREMISE: the retirement block at
	// `SimulationNetSync::sendCorrectionAll` assumes corrections ship EVERY FRAME. §8
	// ⛔ SESSION-SCOPED, SERVER-ONLY, NEVER REPLICATED: a client copy has no reader.
	// ⛔ CLAMPED to [1,16] by `correctionRotation::clampK` — intake, setter, predicate.
	// ⛔ 0 and negatives clamp UP to 1: K=0 is a channel that never publishes, not "off".
	int32_t correctionRotationK = 1;

	// -------------------------------------------------------------------------
	// Test harness mode selector (Catch2 determinism harness)
	// -------------------------------------------------------------------------

	// Catch2 determinism-harness mode.
	//   Production    the production-shipped test surface (the default).
	//   DevTest       opt-in, heavier CI-only determinism runs.
	//   KU1CrossArch  opt-in cross-architecture hash-log verification.
	enum class HarnessMode { Production, DevTest, KU1CrossArch };

	// Active harness mode.
	// ⚠ NO READER IN THIS TREE — only `TimeConfigDefaultsTest.cpp` names it. §10
	HarnessMode harnessMode = HarnessMode::Production;

	// -------------------------------------------------------------------------
	// Tiered input delay (Layer-1 latency hiding)
	//
	// ⛔ THE RTT TIER IS SERVER-AUTHORITATIVE: the server derives it from its own RTT
	// sample and replicates the index; the client CONSUMES it, never computes it. §9
	// ⛔ ALL FOUR TIER ARRAYS SHARE THE 0..3 INDEX — entry N is the same bucket. §9
	// ⛔ EVERY EFFECTIVE DELAY BELOW IS FLOORED by `relayDelayFloorTicks` through
	// `applyRelayDelayFloor`. A floor of 0 is the identity — and 0 is the COMPILED one.
	// -------------------------------------------------------------------------

	// ⛔ RETIRED (2026-08-16): there is
	// deliberately no dedicated no-tier-baseline field here any more (its old
	// identifier is on record at RN-12 in ReviewNotes.md, if a future reader
	// needs it), and its absence is a ruling, not an omission. It used to hold the
	// baseline Layer-1 input-delay applied when NO per-connection tier is
	// available (before the first
	// authoritative tier replicates to a client, and for any Address the server
	// has never sampled). The window it served is bounded to a handful of
	// frames — `ConnectionTierTable::onRttSample` seeds a brand-new connection at
	// tier 0 on its FIRST RTT sample (it does not sit tierless accumulating
	// statistics; `tierMinDwellTicks` gates TRANSITIONS, not the initial
	// assignment) — so carrying a whole separate constant, lint entry and pair of
	// fallback call sites for that window was not worth it.
	//
	// THE REPLACEMENT: both no-tier fallback sites (`ServerInputDelayQueue::
	// effectiveDelay` and `ReplicatedTierConsumer::effectiveInputDelayTicks`) now
	// read `rttTierInputDelays[kMaxConnectionTierIndex]` — the WORST tier —
	// still wrapped by `applyRelayDelayFloor`. Chosen over the best tier (1) or a
	// bare floor (0) because the costs are asymmetric: under-estimating in the
	// join window schedules inputs too early and they are MISSED (the missed-input
	// population `aboveNewest` counts), while over-estimating only costs a couple of
	// extra ticks of lag in a window the player is not yet in combat. A bare
	// floor of 0 was rejected too — it reintroduces the two-ends-disagree shape
	// `relayDelayFloorTicks` exists to prevent. See RN-12 in ReviewNotes.md for
	// the full argument, including the reversal recorded there (domination by a
	// reversible session lever is NOT the same thing as redundancy).
	//
	// If this field is ever reintroduced, argue against this block, not merely
	// against its absence.

	// Inclusive UPPER bound, in milliseconds, of each RTT tier bucket: entry N is
	// the highest smoothed RTT that still counts as tier N.
	// ⛔ The last entry is a SENTINEL: tier 3 must be an open-ended catch-all. §9
	// ⛔ MUST be strictly increasing, or a connection can be stranded in a tier. §9
	int32_t rttTierBoundariesMs[4] = { 30, 80, 150, 999 };

	// Per-tier Layer-1 input delay in ticks, indexed by tier 0..3 — THE effective
	// delay once a tier is known, subject to the session floor.
	// ⛔ MUST be non-decreasing — a worse connection must never get a SHORTER delay. §9
	// ⛔ THE LAST ENTRY IS ALSO THE NO-TIER FALLBACK (sites named in the block above).
	// ⛔ Monotonicity is what makes that fallback pessimistic, not arbitrary. §9
	int32_t rttTierInputDelays[4] = { 1, 2, 3, 4 };

	// Per-tier ceiling that `rollbackWindowTicks` may escalate to, indexed by tier
	// 0..3. Worse connections are permitted a deeper resim.
	// ⛔ MUST be non-decreasing AND the last entry <= `rollbackWindowHardCap`, or the
	// soft cap overtakes the hard cap and clamp-before-snap silently inverts. §9
	// ⛔ INTENDED API, NO PRODUCTION CALLER — the client soft clamp that would read it
	// does not exist, so this array and `tierRollbackCeiling` / `lookupRollbackCeiling` /
	// `effectiveRollbackCeiling` have zero production call sites. NOT dead code. §2 §10
	int32_t rttTierRollbackCeilings[4] = { 6, 9, 12, 20 };

	// TIER-FLAP MITIGATION. Directional dead-band, in ms, around each tier boundary:
	// promote only above (boundary + this), demote only below (boundary - this).
	// ⛔ Without it an RTT on a boundary flaps every sample, and the player feels it. §9
	int32_t tierHysteresisMs = 10;

	// TIER-FLAP MITIGATION, COMPANION. Minimum ticks in the current tier before ANY
	// further transition, however far the RTT has moved.
	// (60 = 1 second at the 60 Hz `tickFrequency` target.)
	// ⛔ Hysteresis stops boundary noise but not a genuinely oscillating connection. §9
	int32_t tierMinDwellTicks = 60;

	// When true, the render-side input echo is suppressed in the WORST tier, where
	// the echo predicts far enough ahead to be visibly wrong more often than right.
	// ⛔ NO PRODUCTION CALLER, AND THE WIRING TASK IS OPTIONAL. `tierShouldMuteEcho` and
	// the two `shouldMuteEcho` members read it, but only the Catch2 suite calls those —
	// so the value has no gameplay effect and keep-true-vs-flip-false stays open. §10
	bool muteEchoOnDegradedTier = true;

	// LAN / arcade-cabinet escape hatch: when true a tier-0 connection gets ZERO
	// input delay instead of `rttTierInputDelays[0]`. Only tier 0 is affected.
	// ⛔ DOMINATED BY A NONZERO `relayDelayFloorTicks`: the floor is applied AFTER this
	// branch, so `max(floor, 0) == floor` wins. Correct precedence, not a conflict. §7
	// ⛔ The ordering is implemented in `tierInputDelayTicks` (ConnectionTierTable.h).
	bool lanZeroDelayOverride = false;

	// -------------------------------------------------------------------------
	// OBSERVABILITY — FIELDS ONLY, NO RUNTIME CONSUMER IN THIS LIBRARY YET
	//
	// ⛔ EVERY CONSTANT THE DESIGN NAMES MUST EXIST AS A FIELD HERE BEFORE ITS CONSUMER
	// SHIPS — otherwise the work hardcodes the literal and the lint cannot see it. §10
	// ⛔ "NO RUNTIME CONSUMER" MEANS THE SAME THING FOR EVERY FIELD BELOW: nothing here
	// reads it today, and its consumer arrives with a work package tracked outside this
	// repository. No field below repeats that sentence; they inherit it from here.
	// -------------------------------------------------------------------------

	// How often the server broadcasts the SN-1 (last-known-authoritative) state.
	//   EveryTick   every server tick; highest bandwidth.
	//   SparseIdle  every `sn1IdleBroadcastIntervalTicks` while idle, every tick
	//               otherwise.
	//   RttTiered   cadence derived from the connection's RTT tier.
	enum class Sn1BroadcastPolicy { EveryTick, SparseIdle, RttTiered };

	// Active SN-1 broadcast policy.
	Sn1BroadcastPolicy sn1BroadcastPolicy = Sn1BroadcastPolicy::RttTiered;

	// Tick interval between SN-1 broadcasts while idle under
	// `Sn1BroadcastPolicy::SparseIdle`.
	int32_t sn1IdleBroadcastIntervalTicks = 6;

	// How often the server broadcasts the state checksum used for desync detection.
	//   EveryTick    checksum every tick; fastest divergence detection.
	//   EveryNTicks  checksum every `hashBroadcastIntervalTicks` ticks.
	//   Off          no checksum broadcast (bandwidth-constrained sessions).
	enum class HashBroadcastPolicy { EveryTick, EveryNTicks, Off };

	// Active hash broadcast policy.
	HashBroadcastPolicy hashBroadcastPolicy = HashBroadcastPolicy::EveryTick;

	// Tick interval between checksum broadcasts under
	// `HashBroadcastPolicy::EveryNTicks`.
	int32_t hashBroadcastIntervalTicks = 1;

	// CONSECUTIVE mismatching checksum ticks before a run counts as a confirmed
	// divergence rather than transient noise — one mismatch can be an in-flight
	// ordering artifact, a sustained run cannot.
	// ⚠ Read by `shouldEscalateToLayer2`, which only the Catch2 suite calls. §10
	int32_t hashMismatchTickThreshold = 5;

	// What to do once `hashMismatchTickThreshold` consecutive mismatches confirm a
	// divergence.
	//   LogOnly             record it and keep running (the safest).
	//   EscalateToLayer2    fall back to the heavier correction layer.
	//   DisconnectAfterRun  drop the diverged client.
	enum class HashMismatchReaction { LogOnly, EscalateToLayer2, DisconnectAfterRun };

	// Active hash-mismatch reaction.
	// ⚠ `shouldEscalateToLayer2` deliberately does NOT read this — it answers WHETHER
	// to escalate; this selects WHAT to do, and that consumer has not shipped. §10
	HashMismatchReaction hashMismatchReaction = HashMismatchReaction::LogOnly;

	// When true, state snapshots are saved sparsely (only at correction-relevant
	// ticks) instead of every tick, trading resim cost for memory.
	bool sparseSaveMode = false;

	// Telemetry recording toggles for the four bounded-depth / input events.
	// ⚠ The three defaulting true are cheap and diagnostically load-bearing; redundancy
	// HITS are high-volume and off, or a healthy session would drown the log.
	bool recordSkipEvents = true;

	// See `recordSkipEvents`.
	bool recordStallEvents = true;

	// See `recordSkipEvents`.
	bool recordSubstitutionEvents = true;

	// See `recordSkipEvents` — high-volume, hence off by default.
	bool recordRedundancyHits = false;

	// When true, input bundles from sibling local players on one connection are
	// aggregated into a single wire payload instead of one bundle each. Phase-2
	// bandwidth optimisation; config-ready flag only.
	// ⚠ This one's consumer lands later than the rest of this section's.
	bool aggregateSiblingInputBundles = false;

	// Capacity, in ticks, of the rolling checksum ring log kept for post-mortem
	// desync analysis. Sized to hold the last 10 seconds at the 60 Hz
	// `tickFrequency` target — the window a human noticing a desync can still catch.
	// ⛔ The field is here so the ring-log reads it instead of hardcoding a literal. §10
	int32_t hashLogRingCapacity = 600;
};
