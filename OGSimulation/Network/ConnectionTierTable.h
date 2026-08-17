#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <type_traits>
#include <unordered_map>

#include "OGSimulation/Network/LocalInputCache.h"
#include "OGSimulation/Network/RemoteInputCache.h"
#include "OGSimulation/PCTimeManagement/TimeConfig.h"

// ---------------------------------------------------------------------------
// ConnectionTierTable<Address> — per-connection RTT tier state.
// (Stage 3 / D3.4; proposal_ogbrawler_netcode.md §1.2 + §8.1;
//  risks_and_plan.md Stage 5 D5.1 + R-A2 mitigation.)
//
// Holds, for every connection the owner has sampled, a smoothed RTT and the
// RTT tier (0..3) derived from it. The tier is the single input to three
// downstream Layer-1 quantities: the forced input delay, the rollback-window
// soft ceiling, and (optionally, task T15) whether the render-side input echo
// is muted.
//
// OWNERSHIP — Option A, locked 2026-07-19 (backlog C1). This table is owned by
// the SERVER only. The authority derives each connection's tier from its own
// per-connection RTT and replicates the resulting tier to the owning client;
// clients do NOT run a second instance of this table and do NOT derive their
// own tier. That keeps the codebase's existing single-source-of-truth shape
// (server owns the authoritative quantity, client consumes it) instead of
// introducing a second, independently-drifting estimator. Nothing in this
// header assumes or enforces that placement — it is stated so a future reader
// does not mistake the type for a symmetric client/server component.
//
// ENGINE-AGNOSTIC. This header lives in the sim core and may include ONLY
// other `OGSimulation/` headers and the STL — no UE types, no engine headers.
// Wire identity therefore arrives as the opaque template parameter `Address`,
// bound in production to `FUEConnectionHandle` (OGSimulationUnreal) and in the
// Catch2 suite to `FStandaloneTestHandle`.
//
// NO CONSUMER YET. As of Stage 3 nothing outside this directory constructs or
// calls a ConnectionTierTable; the wiring lands in Phase B (T9/T10). The type
// is delivered standalone with test coverage so the escalation policy can be
// reviewed on its own, ahead of the tick-loop integration that depends on it.
//
// R-A2 — WHY TWO INDEPENDENT ANTI-FLAPPING GATES. Every tier transition
// changes the player's effective input delay, which is felt directly as a
// change in control latency. A naive `tier = bucketOf(rtt)` lookup flaps on
// ordinary jitter, so transitions pass two gates that fail for different
// reasons:
//   1. Directional hysteresis (`tierHysteresisMs`) — a connection promotes only
//      above `boundary + hysteresis` and demotes only below `boundary -
//      hysteresis`. Kills fast oscillation *around a boundary*.
//   2. Minimum dwell (`tierMinDwellTicks`) — however far the RTT moves, a
//      connection may not leave a tier it entered fewer than N ticks ago. Bounds
//      the transition RATE for a connection that is genuinely oscillating over
//      a wide range, which hysteresis alone cannot do.
// Both must pass. Neither subsumes the other.
//
// NAMESPACE NOTE: declared in the GLOBAL namespace, matching the rest of the
// OGSim core (see the same note on `NetConfig` in SimulationManagerConcept.h
// and on SimulatableList.h). The design corpus writes `ogsim::` but no such
// namespace exists in this tree.
// ---------------------------------------------------------------------------

// The engine-agnostic minimum contract on a connection handle: a hashable,
// regular value type with a liveness probe. This is deliberately the SAME
// requirement set that `NetConfig<C>` enforces on `C::Address`
// (SimulationManagerConcept.h) — restated here as a standalone concept so this
// header constrains its template parameter directly rather than requiring an
// entire NetConfig to be threaded through. Any `C` satisfying `NetConfig`
// therefore has a `C::Address` satisfying `ConnectionAddress`; the static_assert
// in the Catch2 suite pins that relationship so the two cannot silently drift.
template <typename A>
concept ConnectionAddress =
    std::regular<A> &&                      // copyable + equality-comparable + ...
    std::default_initializable<A> &&        // ... has a null/sentinel state
    requires(const A& a)
    {
        { std::hash<A>{}(a) } -> std::convertible_to<std::size_t>;
        { a.isAlive() } -> std::convertible_to<bool>;
    };

// ---------------------------------------------------------------------------
// SHARED TIER -> BEHAVIOUR LOOKUPS (T9, additive to T4's file)
//
// Under Option A the tier is derived ONCE, on the server, and the resulting
// index is replicated to the owning client. Both ends must then turn that one
// integer into the same three Layer-1 quantities — input delay, rollback
// ceiling, echo mute — or the client predicts against a rule the server is not
// applying and every tick mispredicts by the difference.
//
// These free functions are the single source for that math. `ConnectionTierTable`
// (server, address-keyed) delegates to them, and the client's
// `ReplicatedTierConsumer` (ReplicatedTierConsumer.h, tier-index-keyed) calls
// them directly. Neither end owns a private copy of the rule, so the two cannot
// drift apart. They take a bare `(tierIndex, cfg)` precisely so the client can
// use them with NO ConnectionTierTable instance and NO Address.
//
// TIER-INDEX CLAMPING. `clampConnectionTierIndex` exists because the client's
// tier arrives OFF THE WIRE as a uint8. A corrupt, hostile, or
// version-mismatched value would otherwise index the TimeConfig arrays out of
// bounds. The server path is unaffected: ConnectionTierTable only ever produces
// in-range indices, so clamping is the identity there.
// ---------------------------------------------------------------------------

// Tier count derived from the TimeConfig array extent rather than written as a
// literal 4 — the tier count and the config arrays cannot drift apart, and
// adding a tier is a one-line TimeConfig change.
inline constexpr std::size_t kConnectionTierCount =
    std::extent_v<decltype(TimeConfig::rttTierBoundariesMs)>;

inline constexpr int32_t kMaxConnectionTierIndex =
    static_cast<int32_t>(kConnectionTierCount) - 1;

// Clamp an arbitrary integer into [0, kMaxConnectionTierIndex].
inline constexpr int32_t clampConnectionTierIndex(int32_t tierIndex)
{
    if (tierIndex < 0)
        return 0;
    if (tierIndex > kMaxConnectionTierIndex)
        return kMaxConnectionTierIndex;
    return tierIndex;
}

// ---------------------------------------------------------------------------
// THE RELAY DELAY FLOOR (og-netcode-v2-input-relay T11;
// RelayDelaySpectrumDesign.md §6, §10, §11 Q1/Q5)
//
// `TimeConfig::relayDelayFloorTicks` is a SESSION-scoped minimum on the
// effective Layer-1 input delay, independent of the per-wire tier. It exists
// because the tier answers the wrong question for input relay: the tier covers
// the SENDER's uplink (arrival margin at the server), while a peer scheduling a
// relayed input needs the RECEIVER's round trip covered (§3.2). The floor is
// that receiver-coverage budget, and it is session-wide because every receiver
// must be able to schedule every sender.
//
// THE RULE IS A MAX, NEVER A SUM, AND IT LIVES IN ONE PLACE. Every production
// site that derives an effective input delay routes through
// `applyRelayDelayFloor`. There are FOUR such sites (the count was corrected by
// the 2026-08-03 review — the design doc and the backlog each named a different,
// incomplete THREE, which is exactly the mistake this single helper prevents):
//
//   1. `tierInputDelayTicks` below — the sole production reader of
//      `rttTierInputDelays[]`, serving BOTH the server's ConnectionTierTable and
//      the client's ReplicatedTierConsumer.
//   2. `ServerInputDelayQueue::effectiveDelay`'s no-tier fallback.
//   3. `ReplicatedTierConsumer::effectiveInputDelayTicks`'s no-tier fallback.
//   4. The composition-root pre-tier baseline publish, which since the T10 tier
//      channel migration no longer derives anything of its own — it publishes
//      site 3's answer, so it is floored by construction.
//
// A MISSED SITE IS A DIVERGENCE BUG, not a cosmetic gap: the server parks the
// input at its own effective delay while the client predicts at a different one,
// so every tick mispredicts by the difference.
//
// [item 62 / RN-12] The dedicated no-tier baseline field sites 2 and 3 used to
// read (its old identifier is on record in RN-12, ReviewNotes.md) is RETIRED.
// Both fallback sites now read `rttTierInputDelays[kMaxConnectionTierIndex]`
// (the WORST tier) directly, so the completeness guard that used to be "a grep
// on the retired field's READS" no longer applies: there is no second field to
// grep for. The guard is now structural instead — count the sites that route
// through
// `applyRelayDelayFloor` (still 4: sites 1-3 call it directly, site 4
// transitively through site 3) — see `classifyRelayDelayFloor` below for the
// companion startup-advisory this item also adds.
// ---------------------------------------------------------------------------

// The absolute ceiling for an effective floor, in ticks (review finding A5,
// widened to BOTH capacities by T5's AM-3, 2026-08-04).
//
// DERIVED, not a literal, and derived from the SMALLER of the two ring capacities
// the floor has to survive — because a configured floor delays the read on BOTH
// ends of the relay and each end holds the value in a ring of its own:
//
//   SENDER SIDE (`LocalInputCache`, capacity
//   `kLocalInputCacheCapacityTicks`): the local capture is pushed AT capture,
//   read at `capture + floor`, and revisited by a resim up to
//   `rollbackWindowHardCap` ticks further back. It must therefore survive
//   `floor + rollbackWindowHardCap` ticks of pushes.
//
//   RECEIVER SIDE (`RemoteInputCache`, capacity
//   `kRemoteInputCacheCapacityTicks`): the relayed entry for capture tick `c`
//   enters the store at arrival (~`c + wire`), is consumed by the scheduled read
//   at `c + dA`, and must stay resident for T6's resim reads until the frontier
//   passes `c + dA + rollbackWindowHardCap`. The store is fed at up to one entry
//   per tick, so `c` is evicted at ~`c + capacity + wire`. Residency therefore
//   needs `dA + rollbackWindowHardCap <= capacity + wire` — THE SAME INEQUALITY as
//   the delay line's, with `wire = 0`. It is not a different quantity; it is a
//   bound that merely happens not to bind while both capacities are 64 and the
//   wire slack is non-negative.
//
// SO THE MIN IS THE POINT, NOT A FLOURISH. Lower `kRemoteInputCacheCapacityTicks`
// to 32 and a delay-line-only derivation would still admit a floor of 44 into a
// store that evicts scheduled entries before their resim window closes — the
// scheduled regime degenerating SILENTLY into permanent fallback reads, which is
// precisely the failure this cap exists to make impossible. A comment coupling the
// two would have been the silent-drift hole; this derivation closes it.
//
// 64 - 20 = 44 at current defaults; lowering `rollbackWindowHardCap` raises it,
// lowering EITHER capacity lowers it.
constexpr int32_t relayDelayFloorHardCapForCapacities(std::size_t delayLineCapacityTicks,
                                                      std::size_t storeCapacityTicks,
                                                      int32_t     rollbackWindowHardCap)
{
    // Split out from `relayDelayFloorHardCapTicks` so the derivation is TESTABLE
    // against capacities other than the shipped ones: both capacities are
    // `constexpr` constants, so a test can only prove "the cap follows the store
    // capacity down" by feeding a different one in here.
    const std::size_t smallerCapacity =
        delayLineCapacityTicks < storeCapacityTicks ? delayLineCapacityTicks : storeCapacityTicks;

    const int32_t cap = static_cast<int32_t>(smallerCapacity) - rollbackWindowHardCap;
    return cap < 0 ? 0 : cap;   // a pathological hardCap must not produce a negative cap
}

inline int32_t relayDelayFloorHardCapTicks(const TimeConfig& cfg)
{
    return relayDelayFloorHardCapForCapacities(kLocalInputCacheCapacityTicks,
                                               kRemoteInputCacheCapacityTicks,
                                               cfg.rollbackWindowHardCap);
}

// Clamp a REQUESTED floor into [0, relayDelayFloorHardCapTicks(cfg)].
//
// Called at BOTH intake points — the ini override at the composition root and
// the client's floor OnRep — and again on every read inside
// `applyRelayDelayFloor`. That belt-and-braces shape is deliberate and is the
// same one `clampConnectionTierIndex` uses for the replicated tier: the value
// arrives off the wire as a uint8, so "clamped at intake" alone would leave a
// corrupt or version-mismatched byte able to break the regime if any future
// intake point forgot to clamp.
inline int32_t clampRelayDelayFloorTicks(int32_t requestedFloorTicks, const TimeConfig& cfg)
{
    if (requestedFloorTicks < 0)
    {
        return 0;
    }
    const int32_t cap = relayDelayFloorHardCapTicks(cfg);
    return requestedFloorTicks > cap ? cap : requestedFloorTicks;
}

// THE shared floor application: `max(relayDelayFloorTicks, base)`.
//
// `base` is whatever the caller's own rule produced (a tier delay, or — since
// item 62 / RN-12 retired the dedicated field — the no-tier fallback, which is
// itself `rttTierInputDelays[kMaxConnectionTierIndex]`). At the shipped default
// floor of 0 this is the identity for every non-negative base, which is what
// makes the whole feature ship degenerate — floor 0 behaves exactly as the
// pre-T11 build.
inline int32_t applyRelayDelayFloor(int32_t baseDelayTicks, const TimeConfig& cfg)
{
    const int32_t floorTicks = clampRelayDelayFloorTicks(cfg.relayDelayFloorTicks, cfg);
    return baseDelayTicks > floorTicks ? baseDelayTicks : floorTicks;
}

// ---------------------------------------------------------------------------
// FLOOR VALIDATION (og-netcode-v2-input-relay item 62 / RN-12) — ADVISORY ONLY,
// NEVER AN ASSERT. `relayDelayFloorTicks == 0` is the documented "scheduled
// regime OFF" mode (see the field's own comment: "Default: 0 (degenerate —
// today's behaviour, byte-for-byte)"), so a hard check on `floor < 2` would
// forbid a mode this codebase deliberately supports. This classifies the
// CONFIGURED floor for STARTUP LOGGING; callers (the composition-root ini
// intake and the client's floor OnRep — the same two sites `clampRelayDelayFloorTicks`
// is called from) decide how loudly to log each case. Never used to reject or
// correct a value — that is `clampRelayDelayFloorTicks`'s job, and it always
// runs first.
//
//   0                          -> None (documented off mode; silent)
//   1                          -> BelowHiccupBaseline (below the hiccup-
//                                 absorption baseline `relayDelayFloorTicks` now
//                                 documents, above off — the one value that is
//                                 neither; WARN)
//   2 .. < max(rttTierInputDelays) -> None (an ordinary scheduled-regime floor;
//                                 silent)
//   >= max(rttTierInputDelays) -> UniformDFairnessActive (every derivation path
//                                 — tier, LAN override, and the no-tier fallback
//                                 — collapses to this one value; NOTE, not a
//                                 warning: this is ACTIVE on the shipped config
//                                 today — floor 6 vs max tier delay 4 — and was,
//                                 until this item, invisible in the logs)
// ---------------------------------------------------------------------------
enum class RelayDelayFloorAdvisory
{
    None,
    BelowHiccupBaseline,
    UniformDFairnessActive,
};

// The worst-connection per-tier delay — the threshold at which a floor
// dominates every tiered derivation path. Computed rather than assumed to be
// the last array entry so this stays correct independent of the
// monotonic-non-decreasing invariant `TimeConfigTierArrayOrderingTest` pins
// (today it is exactly `rttTierInputDelays[kMaxConnectionTierIndex]`, and that
// equivalence is itself asserted by a test rather than assumed here).
inline int32_t maxRttTierInputDelay(const TimeConfig& cfg)
{
    int32_t maxDelay = cfg.rttTierInputDelays[0];
    for (std::size_t i = 1; i < kConnectionTierCount; ++i)
    {
        if (cfg.rttTierInputDelays[i] > maxDelay)
        {
            maxDelay = cfg.rttTierInputDelays[i];
        }
    }
    return maxDelay;
}

// Classify `cfg.relayDelayFloorTicks` per the table above. UniformDFairnessActive
// is checked BEFORE BelowHiccupBaseline: the two conditions cannot both hold
// under the shipped tier table (`rttTierInputDelays[0] == 1`, so
// `maxRttTierInputDelay >= 1` always, which makes floor 1 read as
// UniformDFairnessActive only in the degenerate case where every tier delay is
// <= 1) — the ordering below simply decides that degenerate case in favour of
// the stronger, more informative advisory rather than leaving it unspecified.
inline RelayDelayFloorAdvisory classifyRelayDelayFloor(const TimeConfig& cfg)
{
    const int32_t floorTicks = cfg.relayDelayFloorTicks;
    if (floorTicks <= 0)
    {
        return RelayDelayFloorAdvisory::None;   // documented off mode
    }
    if (floorTicks >= maxRttTierInputDelay(cfg))
    {
        return RelayDelayFloorAdvisory::UniformDFairnessActive;
    }
    if (floorTicks == 1)
    {
        return RelayDelayFloorAdvisory::BelowHiccupBaseline;
    }
    return RelayDelayFloorAdvisory::None;
}

// Effective Layer-1 input delay in ticks for `tierIndex`.
//
// `lanZeroDelayOverride` collapses tier 0 to zero delay: on a sub-millisecond
// local link there is no round trip left to hide, so the configured delay is
// pure added lag. Only tier 0 is affected — a bad connection inside a LAN
// session still gets its own tier's delay.
//
// THE FLOOR IS APPLIED AFTER THE OVERRIDE BRANCH, so a nonzero floor DOMINATES
// the override (`max(floor, 0) == floor`). That ordering is the point, not an
// accident: on a mixed session a LAN sender must still be schedulable by WAN
// receivers. Documented at `lanZeroDelayOverride`'s definition too.
//
// NOTE this is the C2-locked REPLACES value: it IS the effective delay, and is
// never added to the no-tier fallback. The fallback applies only when NO tier is
// available at all (see ReplicatedTierConsumer and
// ServerInputDelayQueue::effectiveDelay, which each own that fallback for their
// own "no tier" condition) and — since item 62 / RN-12 retired the dedicated
// no-tier-baseline field this used to read — the fallback value itself is
// `rttTierInputDelays[kMaxConnectionTierIndex]` (the WORST tier). AMENDED
// 2026-08-03: the replacement value is `max(relayDelayFloorTicks,
// tier-or-fallback)` at every one of those sites.
inline int32_t tierInputDelayTicks(int32_t tierIndex, const TimeConfig& cfg)
{
    const int32_t tier = clampConnectionTierIndex(tierIndex);
    const int32_t tierDelay =
        (tier == 0 && cfg.lanZeroDelayOverride) ? 0 : cfg.rttTierInputDelays[tier];
    return applyRelayDelayFloor(tierDelay, cfg);
}

// Per-tier SOFT ceiling for `rollbackWindowTicks`. The hard cap
// (`rollbackWindowHardCap`) is enforced elsewhere and is unaffected by tier.
//
// INTENDED API, NO PRODUCTION CALLER — the client-side soft resim clamp this
// would feed is INTENDED, NOT IMPLEMENTED (TimeConfig.h ADR status note; ruling
// in RelayDelaySpectrumDesign.md §7). Defined and tested, called only from the
// Catch2 suite and from `lookupRollbackCeiling` / `effectiveRollbackCeiling`,
// which are unconsumed for the same reason. Wiring is deferred to the
// sparse-state increment. Not dead code to delete: keeping the lookup here is
// what stops a future consumer re-deriving the tier math at its call site.
inline int32_t tierRollbackCeiling(int32_t tierIndex, const TimeConfig& cfg)
{
    return cfg.rttTierRollbackCeilings[clampConnectionTierIndex(tierIndex)];
}

// C.4 render-side input echo suppression, worst tier only.
inline bool tierShouldMuteEcho(int32_t tierIndex, const TimeConfig& cfg)
{
    return cfg.muteEchoOnDegradedTier
        && clampConnectionTierIndex(tierIndex) == kMaxConnectionTierIndex;
}

// Change in effective Layer-1 input delay when moving `fromTier` -> `toTier`.
// POSITIVE = the transition INCREASES delay (an upward/degrading transition);
// negative = it decreases delay; zero = no delay change.
//
// MUST be expressed through `tierInputDelayTicks`, never as a bare
// `cfg.rttTierInputDelays[to] - cfg.rttTierInputDelays[from]`: with
// `lanZeroDelayOverride` set, tier 0's effective delay is 0 rather than
// `rttTierInputDelays[0]`, so the raw-array form reports the wrong delta for
// every transition that touches tier 0 — which is the most common transition
// there is. The suite pins this with a dedicated case + verified negative.
//
// BENIGN SIDE EFFECT OF THE FLOOR (T11): because this routes through
// `tierInputDelayTicks`, a floor that dominates both endpoints collapses the
// delta to 0 — and that is CORRECT, not a swallowed transition: with the floor
// dominating, the player's felt delay does not change across the transition, so
// there is no prediction-stall debt to pay. The server's publish predicate
// compares tier INDICES, not deltas (ServerReceptionCoordinator), so publishes
// are unaffected and the client still learns the new tier.
inline int32_t tierDelayDeltaTicks(int32_t fromTier, int32_t toTier, const TimeConfig& cfg)
{
    return tierInputDelayTicks(toTier, cfg) - tierInputDelayTicks(fromTier, cfg);
}

// THE CLIENT PREDICTION-STALL DECISION for a wire tier transition (item 69 /
// og-netcode-v2-input-relay, filed from item 62's review finding 1).
//
// `oldTier` in `ISimulationConnectionRelayListener::onConnectionTierReceived`
// is the replicated property's value BEFORE this OnRep — but on a fresh
// connection's FIRST real OnRep, that "before" value is the property's
// compiled default (0), not a tier the client was ever actually running.
// Before that first arrival the client runs the pre-arrival no-tier fallback
// (`rttTierInputDelays[kMaxConnectionTierIndex]`, floored — see
// ReplicatedTierConsumer's PRE-ARRIVAL FALLBACK note), never tier 0's delay.
// Feeding `tierDelayDeltaTicks(0, newTier, cfg)` into that case therefore
// stalls by the wrong quantity, in the wrong direction, for every newTier
// except 0 (SimulationManagerUImpl.cpp's PRESERVED QUIRK note has the full
// worked table). `hadAnyTier` is what makes the two cases distinguishable:
// pass whether the caller had ALREADY received an authoritative tier before
// THIS call — never infer it from `oldTier` itself, since 0 is both the
// property default and a legal tier.
//
// FIRST-EVER RESOLUTION -> ALWAYS ZERO, regardless of newTier. This is
// exactly `onConnectionTierReplayed`'s reasoning (no stall: nothing was
// predicted against a previous TIER's delay, only against the fallback) —
// a first real OnRep is the same situation, just arriving through the other
// entry point.
//
// The `oldTier == newTier` early-out is preserved: an OnRep can fire for an
// unchanged value, and nothing transitioned.
//
// Only a genuine (hadAnyTier == true) transition reaches `tierDelayDeltaTicks`,
// and only a POSITIVE delta (an upward/degrading move) is ever returned — a
// downward or delay-neutral transition needs no correction, matching
// `applyTierTransitionStall`'s pre-existing "non-positive deltas are dropped"
// contract (ClientPredictionClock::requestInputDelayIncreaseStall is also a
// no-op below zero, so this is belt-and-braces, not the only guard).
inline int32_t shouldStallForTierTransition(
    int32_t oldTier, int32_t newTier, bool hadAnyTier, const TimeConfig& cfg)
{
    if (oldTier == newTier)
        return 0;

    if (!hadAnyTier)
        return 0;

    const int32_t deltaDelayTicks = tierDelayDeltaTicks(oldTier, newTier, cfg);
    return deltaDelayTicks > 0 ? deltaDelayTicks : 0;
}

// Outcome of a single ConnectionTierTable::onRttSample call.
//
// LOCKED SHAPE (backlog T11): the caller needs the resulting tier and the delay
// delta and nothing else — no separate up/down booleans, since the sign of
// `deltaDelayTicks` already carries the direction and `> 0` is exactly the
// "delay increased" predicate the transition consumers test.
//
// A sample that produces no transition reports the UNCHANGED current tier with
// `deltaDelayTicks == 0`. Callers therefore never need to remember the previous
// tier themselves to tell "no change" from "changed"; `deltaDelayTicks != 0` is
// the transition signal. Note a transition CAN legitimately carry a zero delta
// (two adjacent tiers configured with the same input delay) — such a transition
// changes the rollback ceiling and echo-mute behaviour but requires no
// delay-driven reaction, which is precisely why the delta rather than the index
// change is the thing reported.
struct TierSampleResult
{
    int32_t newTierIndex    = 0;
    int32_t deltaDelayTicks = 0;
};

template <ConnectionAddress Address>
class ConnectionTierTable
{
public:
    // Aliases of the free constants above so the class and the shared lookups
    // cannot describe different tier counts.
    static constexpr std::size_t kTierCount = kConnectionTierCount;

    static constexpr int32_t kMaxTierIndex = kMaxConnectionTierIndex;

    // Per-connection state. Public so tests and (later) the Stage 4 telemetry
    // sink can read a snapshot; mutated only through onRttSample / reapDeadHandles.
    struct TierState
    {
        // Current RTT tier, 0 (best) .. kMaxTierIndex (worst).
        int32_t currentTierIndex = 0;

        // EMA of observed round-trip time, milliseconds. Seeded with the first
        // sample rather than with 0 — seeding from zero would drag every fresh
        // connection through a spurious tier-0 phase while the average climbed,
        // and on a genuinely bad link that phase hands the player a delay far
        // too short for their actual RTT.
        double smoothedRttMs = 0.0;

        // R-A2 dwell counter: samples observed since entering `currentTierIndex`.
        // Reset to 0 on every transition.
        int32_t ticksInCurrentTier = 0;

        // Tick of the most recent sample, for staleness-based reaping.
        int32_t lastSampleTick = 0;
    };

    // The config is BORROWED, not owned — one TimeConfig instance is shared by
    // the whole time-management stack, and a copy here could silently diverge
    // from the one the clocks read. Caller must outlive the table.
    explicit ConnectionTierTable(const TimeConfig& cfg)
        : m_config(cfg)
    {
    }

    // Feed one RTT observation for `addr`, observed at `currentTick`.
    // Creates the entry on first sight. Updates the EMA, advances the dwell
    // counter, and applies at most ONE tier transition per call (see R-A2 note
    // above for why both gates exist).
    //
    // Returns the resulting tier and the change in effective input delay (T11).
    // Deliberately NOT [[nodiscard]]: the great majority of call sites — the
    // production sampler and every arrange-phase loop in the suite — drive the
    // table for its state and have no use for the per-sample outcome. Only a
    // consumer reacting to transitions reads the result.
    TierSampleResult onRttSample(Address addr, int32_t currentTick, double rttMs)
    {
        auto it = m_entries.find(addr);
        if (it == m_entries.end())
        {
            TierState fresh;
            fresh.currentTierIndex = 0;
            fresh.smoothedRttMs = rttMs;    // seed, not blend — see TierState
            fresh.ticksInCurrentTier = 0;
            fresh.lastSampleTick = currentTick;
            it = m_entries.emplace(addr, fresh).first;
        }
        else
        {
            const double alpha = m_config.rttSmoothingAlpha;
            it->second.smoothedRttMs += alpha * (rttMs - it->second.smoothedRttMs);
            it->second.lastSampleTick = currentTick;
        }

        TierState& state = it->second;

        // Counted on EVERY sample, including the one that creates the entry, so
        // a brand-new connection must also serve the dwell period before its
        // first transition.
        ++state.ticksInCurrentTier;

        const int32_t desired = desiredTierWithHysteresis(state);
        if (desired == state.currentTierIndex)
        {
            return TierSampleResult{ state.currentTierIndex, 0 };
        }

        // Gate 2: dwell. The candidate transition is DISCARDED, not deferred —
        // there is no pending-transition memory. If the condition is real it
        // still holds on the next sample after the gate opens; if it was a
        // transient it correctly evaporates.
        //
        // A dwell-blocked sample reports NO transition (unchanged tier, zero
        // delta) — the discard is invisible to the caller by design, exactly as
        // it is invisible to lookupTierIndex.
        if (state.ticksInCurrentTier < m_config.tierMinDwellTicks)
        {
            return TierSampleResult{ state.currentTierIndex, 0 };
        }

        const int32_t previousTier = state.currentTierIndex;
        state.currentTierIndex = desired;
        state.ticksInCurrentTier = 0;

        return TierSampleResult{
            desired,
            tierDelayDeltaTicks(previousTier, desired, m_config)
        };
    }

    // 0..kMaxTierIndex. An Address never sampled reports the BEST tier: before
    // any evidence of a bad link exists, the optimistic assumption costs the
    // player the least input delay, and the first sample corrects it.
    int32_t lookupTierIndex(const Address& addr) const
    {
        const auto it = m_entries.find(addr);
        return it == m_entries.end() ? 0 : it->second.currentTierIndex;
    }

    // The three tier -> behaviour lookups. Each is a thin address-keyed wrapper
    // over the corresponding SHARED free function above — the address-keyed and
    // tier-index-keyed forms therefore compute the identical answer BY
    // CONSTRUCTION, which is what lets the client (which has only a replicated
    // tier index, no table and no Address) stay in lockstep with this server-side
    // table. Do not re-inline the math here.

    // Effective Layer-1 input delay in ticks for this connection's tier.
    int32_t lookupInputDelayTicks(const Address& addr) const
    {
        return tierInputDelayTicks(lookupTierIndex(addr), m_config);
    }

    // Per-tier SOFT ceiling for `rollbackWindowTicks`.
    // INTENDED API, NO PRODUCTION CALLER — see `tierRollbackCeiling` above: the
    // client-side soft resim clamp that would consume this is INTENDED, NOT
    // IMPLEMENTED (TimeConfig.h ADR status note; RelayDelaySpectrumDesign.md §7).
    int32_t lookupRollbackCeiling(const Address& addr) const
    {
        return tierRollbackCeiling(lookupTierIndex(addr), m_config);
    }

    // C.4 render-side input echo suppression, worst tier only.
    // NOTE: no production caller until the OPTIONAL task T15 — the query ships
    // here so the policy lives with the tier state rather than being re-derived
    // at the call site later.
    bool shouldMuteEcho(const Address& addr) const
    {
        return tierShouldMuteEcho(lookupTierIndex(addr), m_config);
    }

    // Evict entries whose connection has died OR that have gone silent for
    // longer than `deadlineTicks`. Without this the map grows for the lifetime
    // of the process, one entry per connection ever seen.
    //
    // Two conditions because they catch different failures: `isAlive()` is the
    // prompt signal for a clean disconnect, while the deadline catches a
    // connection that stopped being sampled without its handle reporting dead
    // (half-open socket, a handle whose liveness cannot go stale — see the
    // FStandaloneTestHandle note in the test file).
    void reapDeadHandles(int32_t currentTick, int32_t deadlineTicks)
    {
        for (auto it = m_entries.begin(); it != m_entries.end();)
        {
            const bool dead = !it->first.isAlive();
            const bool stale = (it->second.lastSampleTick + deadlineTicks) < currentTick;
            it = (dead || stale) ? m_entries.erase(it) : std::next(it);
        }
    }

    bool hasEntry(const Address& addr) const
    {
        return m_entries.find(addr) != m_entries.end();
    }

    std::size_t entryCount() const
    {
        return m_entries.size();
    }

    // Read-only snapshot; nullptr when unknown. Lets tests assert on the EMA and
    // dwell counter without those becoming settable from outside.
    const TierState* findState(const Address& addr) const
    {
        const auto it = m_entries.find(addr);
        return it == m_entries.end() ? nullptr : &it->second;
    }

private:
    // Gate 1: directional hysteresis. Returns the tier this state SHOULD be in,
    // or its current tier when neither band is exceeded.
    //
    // Asymmetric by construction: promotion tests the CURRENT tier's upper
    // boundary while demotion tests the tier-BELOW's boundary. That leaves a
    // 2 * hysteresis dead-band straddling every boundary in which neither
    // direction fires, which is precisely what stops boundary-adjacent jitter
    // from flapping the tier.
    //
    // At most one step per call — a genuine multi-tier RTT jump walks up one
    // tier per dwell period rather than teleporting. Deliberate: each step is a
    // player-visible input-delay change, and stepping keeps that change bounded
    // and monotone.
    int32_t desiredTierWithHysteresis(const TierState& state) const
    {
        const int32_t tier = state.currentTierIndex;
        const double hysteresis = static_cast<double>(m_config.tierHysteresisMs);

        if (tier < kMaxTierIndex)
        {
            const double upperBoundary = static_cast<double>(m_config.rttTierBoundariesMs[tier]);
            if (state.smoothedRttMs > upperBoundary + hysteresis)
            {
                return tier + 1;
            }
        }

        if (tier > 0)
        {
            const double lowerBoundary = static_cast<double>(m_config.rttTierBoundariesMs[tier - 1]);
            if (state.smoothedRttMs < lowerBoundary - hysteresis)
            {
                return tier - 1;
            }
        }

        return tier;
    }

    const TimeConfig& m_config;
    std::unordered_map<Address, TierState> m_entries;
};
