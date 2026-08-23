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
// ConnectionTierTable<Address> -- per-connection RTT tier state.
//
// ORIENTATION
//
// It holds, for every connection the owner has sampled, a smoothed RTT and the RTT
// tier (0 best .. 3 worst) derived from it. The tier is the single input to three
// Layer-1 quantities: the forced input delay, the rollback-window SOFT ceiling, and
// whether the render-side input echo is muted.
//
// OWNERSHIP -- Option A, locked 2026-07-19. THE SERVER OWNS THIS TABLE. The authority
// derives each connection's tier from its own per-connection RTT and replicates the
// resulting index to the owning client; clients do NOT run a second instance and do
// NOT derive their own tier. Nothing here enforces that placement -- it is stated so a
// reader does not mistake the type for a symmetric client/server component.
//
// LIVE IN PRODUCTION, by this route:
//   the per-character adapter component (GAME thread)
//     -> `ServerReceptionCoordinator::noteRttSample`
//     -> `ConnectionTierTable::onRttSample`
// `ServerReceptionCoordinator` owns the only production instance, by value, and reaps
// it; `ServerInputDelayQueue` borrows a const pointer to it. On the CLIENT the same
// math is reached with no table at all, through `ReplicatedTierConsumer`.
//
// THIS FILE HOLDS TWO THINGS, and the split is the point:
//   the SHARED free functions  `tierInputDelayTicks`, `tierRollbackCeiling`,
//                              `tierShouldMuteEcho`, `tierDelayDeltaTicks`,
//                              `shouldStallForTierTransition`, and the relay delay
//                              floor helpers. Bare `(tierIndex, cfg)` -- no table and
//                              no Address -- so the client can call them directly.
//   the TABLE                  address-keyed state plus thin wrappers over those same
//                              functions. It owns no copy of the rule.
//
// TWO INDEPENDENT ANTI-FLAP GATES, and neither subsumes the other. Every transition
// changes the player's effective input delay, felt directly as control latency:
//   1. Directional hysteresis (`tierHysteresisMs`) -- promote only above
//      `boundary + h`, demote only below `boundary - h`. Kills oscillation AROUND a
//      boundary.
//   2. Minimum dwell (`tierMinDwellTicks`) -- however far the RTT moves, a connection
//      may not leave a tier it entered fewer than N ticks ago. Bounds the transition
//      RATE for a connection oscillating over a wide range, which hysteresis cannot.
// Both must pass. §3
//
// WHAT PINS WHAT:
//   the tier ladder, both gates, reaping   `ConnectionTierTableTest.cpp`
//   client/server lockstep on one index    `ReplicatedTierConsumptionTest.cpp`
//   the relay delay floor and its cap      `RelayDelayFloorTest.cpp`
//   the array ordering invariants          `TimeConfigTierArrayOrderingTest.cpp`
//   the delay-queue integration            `ServerInputDelayQueueTest.cpp`,
//                                          `ServerInputDelayIntegrationTest.cpp`
//
// ENGINE-AGNOSTIC. Sim core: only other `OGSimulation/` headers and the STL. Wire
// identity arrives as the opaque template parameter `Address`, bound in production to
// `FUEConnectionHandle` (OGSimulationUnreal) and in the Catch2 suite to
// `FStandaloneTestHandle` (`Network/StandaloneTestHandle.h`).
// GLOBAL NAMESPACE, matching the rest of the OGSim core (same note as `NetConfig` in
// SimulationManagerConcept.h and SimulatableList.h). The design corpus writes
// `ogsim::`; no such namespace exists in this tree.
//
// Derivations, history and deferred work: `docs/ConnectionTierTable-rationale.md`.
// ---------------------------------------------------------------------------

// The engine-agnostic minimum contract on a connection handle: a hashable, regular
// value type with a liveness probe.
// ⛔ DELIBERATELY THE SAME SET `NetConfig<C>` ENFORCES ON `C::Address`; a suite pins it. §1
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
// SHARED TIER -> BEHAVIOUR LOOKUPS
//
// ⛔⛔ THE SINGLE SOURCE OF THE TIER MATH -- NEITHER END MAY OWN A PRIVATE COPY. The
//   server table delegates here and `ReplicatedTierConsumer` calls here; if the two
//   drift, the client predicts against a rule the server is not applying. §2
// ⛔ They take a bare `(tierIndex, cfg)` so the client needs no table and no Address. §2
// ⛔ CLAMPING EXISTS BECAUSE THE CLIENT'S TIER ARRIVES OFF THE WIRE as a uint8. §2
// ---------------------------------------------------------------------------

// ⛔ DERIVED FROM THE TimeConfig ARRAY EXTENT, never a literal 4, so it cannot drift. §2
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
// THE RELAY DELAY FLOOR
//
// `TimeConfig::relayDelayFloorTicks` is a SESSION-scoped minimum on the effective
// Layer-1 input delay, independent of the per-wire tier. The tier answers the wrong
// question for input relay: it covers the SENDER's uplink, while a peer scheduling a
// relayed input needs the RECEIVER's round trip covered. The floor is that
// receiver-coverage budget, and it is session-wide because every receiver must be able
// to schedule every sender. §4
//
// ⛔⛔ THE RULE IS A MAX, NEVER A SUM, AND IT LIVES IN ONE PLACE: every production site
//   that derives an effective input delay routes through `applyRelayDelayFloor`. A
//   MISSED SITE IS A DIVERGENCE BUG -- the server parks the input at its own effective
//   delay while the client predicts at a different one. §4
//
// FOUR SITES. The count was corrected by the 2026-08-03 review, which found TWO prior
// records each naming a different, incomplete THREE -- exactly the mistake the single
// helper prevents:
//   1. `tierInputDelayTicks` -- the sole production reader of `rttTierInputDelays[]`,
//      serving BOTH the server's table and the client's ReplicatedTierConsumer.
//   2. `ServerInputDelayQueue::effectiveDelay`'s no-tier fallback.
//   3. `ReplicatedTierConsumer::effectiveInputDelayTicks`'s no-tier fallback.
//   4. The composition-root pre-tier baseline publish, which derives nothing of its
//      own -- it publishes site 3's answer, so it is floored by construction.
//
// ⛔ THE COMPLETENESS GUARD IS STRUCTURAL: COUNT THE SITES ROUTING THROUGH IT. §4
// ---------------------------------------------------------------------------

// The absolute ceiling for an effective floor, in ticks.
//
// ⛔ DERIVED, NOT A LITERAL, and from the SMALLER of the two ring capacities. §5
//
//   SENDER SIDE (`LocalInputCache`, `kLocalInputCacheCapacityTicks`): the capture is
//   pushed at capture, read at `capture + floor`, and revisited by a resim up to
//   `rollbackWindowHardCap` ticks further back, so it must survive
//   `floor + rollbackWindowHardCap` ticks of pushes.
//   RECEIVER SIDE (`RemoteInputCache`, `kRemoteInputCacheCapacityTicks`): the entry for
//   capture tick `c` arrives at ~`c + wire`, is consumed at `c + dA`, must stay resident
//   until the frontier passes `c + dA + rollbackWindowHardCap`, and is evicted at
//   ~`c + capacity + wire`. THE SAME INEQUALITY, with `wire = 0`.
//
// ⛔⛔ THE MIN IS THE POINT, NOT A FLOURISH: a delay-line-only derivation would admit a
//   floor into a store that evicts entries before their resim window closes, and the
//   scheduled regime would degenerate SILENTLY into permanent fallback reads. §5
//
// 64 - 20 = 44 at current defaults; lowering `rollbackWindowHardCap` raises it,
// lowering EITHER capacity lowers it.
constexpr int32_t relayDelayFloorHardCapForCapacities(std::size_t localInputCacheCapacityTicks,
                                                      std::size_t remoteInputCacheCapacityTicks,
                                                      int32_t     rollbackWindowHardCap)
{
    // ⛔ SPLIT OUT SO THE DERIVATION IS TESTABLE against non-shipped capacities. §5
    const std::size_t smallerCapacity = localInputCacheCapacityTicks < remoteInputCacheCapacityTicks
        ? localInputCacheCapacityTicks
        : remoteInputCacheCapacityTicks;

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
// ⛔ CALLED AT BOTH INTAKES AND AGAIN ON EVERY READ -- it arrives off the wire. §5
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
// `base` is whatever the caller's own rule produced -- a tier delay, or the no-tier
// fallback, which is itself `rttTierInputDelays[kMaxConnectionTierIndex]`.
// ⛔ AT A FLOOR OF 0 THIS IS THE IDENTITY -- which is how the feature ships degenerate. §4
// ⛔ THE COMPILED DEFAULT AND THE SHIPPED CONFIGURATION ARE TWO FACTS -- read the key. §4
inline int32_t applyRelayDelayFloor(int32_t baseDelayTicks, const TimeConfig& cfg)
{
    const int32_t floorTicks = clampRelayDelayFloorTicks(cfg.relayDelayFloorTicks, cfg);
    return baseDelayTicks > floorTicks ? baseDelayTicks : floorTicks;
}

// ---------------------------------------------------------------------------
// FLOOR VALIDATION -- ADVISORY ONLY.
//
// ⛔⛔ NEVER AN ASSERT, AND NEVER USED TO REJECT OR CORRECT A VALUE -- a floor of 0 is
//   the documented "scheduled regime OFF" mode, so a hard check would forbid a mode
//   this codebase supports. Correcting is `clampRelayDelayFloorTicks`'s job. §6
// ⛔ THE CALLERS DECIDE HOW LOUDLY TO LOG -- the same two sites that clamp. §6
//
//   0                              None (documented off mode; silent)
//   1                              BelowHiccupBaseline -- below the hiccup-absorption
//                                  baseline, above off. The one value that is neither;
//                                  WARN.
//   2 .. < max(rttTierInputDelays) None (an ordinary scheduled-regime floor; silent)
//   >= max(rttTierInputDelays)     UniformDFairnessActive -- every derivation path
//                                  (tier, LAN override, no-tier fallback) collapses to
//                                  this one value. NOTE, not a warning. It IS reached
//                                  by the shipped configuration -- read the
//                                  `RelayDelayFloorTicks` KEY against
//                                  `max(rttTierInputDelays)`, never this sentence, for
//                                  the values. Invisible in the logs until this
//                                  advisory existed.
// ---------------------------------------------------------------------------
enum class RelayDelayFloorAdvisory
{
    None,
    BelowHiccupBaseline,
    UniformDFairnessActive,
};

// The worst-connection per-tier delay -- the threshold at which a floor dominates every
// tiered derivation path.
// ⛔ COMPUTED, NOT ASSUMED TO BE THE LAST ARRAY ENTRY, so no ordering is assumed. §3
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

// Classify `cfg.relayDelayFloorTicks` per the FLOOR VALIDATION table.
// ⛔ UniformDFairnessActive IS CHECKED FIRST -- it decides only the degenerate case. §6
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
// `lanZeroDelayOverride` collapses tier 0 to zero delay: on a sub-millisecond local
// link there is no round trip left to hide. Only tier 0 is affected -- a bad connection
// inside a LAN session keeps its own tier's delay.
// ⛔ THE FLOOR IS APPLIED AFTER THE OVERRIDE BRANCH, so a nonzero floor DOMINATES it --
//   on a mixed session a LAN sender must still be schedulable by WAN receivers. §4
// ⛔ THIS IS THE REPLACES VALUE, NEVER ADDED TO THE NO-TIER FALLBACK. §2
inline int32_t tierInputDelayTicks(int32_t tierIndex, const TimeConfig& cfg)
{
    const int32_t tier = clampConnectionTierIndex(tierIndex);
    const int32_t tierDelay =
        (tier == 0 && cfg.lanZeroDelayOverride) ? 0 : cfg.rttTierInputDelays[tier];
    return applyRelayDelayFloor(tierDelay, cfg);
}

// Per-tier SOFT ceiling for `rollbackWindowTicks`. The hard cap
// (`rollbackWindowHardCap`) is enforced elsewhere and is unaffected by tier.
// ⛔ INTENDED API, NO PRODUCTION CALLER -- the soft resim clamp is not implemented. §7
// ⛔ NOT DEAD CODE: it stops a future consumer re-deriving the tier math at its site. §7
inline int32_t tierRollbackCeiling(int32_t tierIndex, const TimeConfig& cfg)
{
    return cfg.rttTierRollbackCeilings[clampConnectionTierIndex(tierIndex)];
}

// Render-side input echo suppression, worst tier only.
inline bool tierShouldMuteEcho(int32_t tierIndex, const TimeConfig& cfg)
{
    return cfg.muteEchoOnDegradedTier
        && clampConnectionTierIndex(tierIndex) == kMaxConnectionTierIndex;
}

// Change in effective Layer-1 input delay when moving `fromTier` -> `toTier`. POSITIVE
// = the transition INCREASES delay (upward/degrading); negative decreases; zero means
// no delay change.
// ⛔⛔ MUST GO THROUGH `tierInputDelayTicks`, NEVER THE BARE ARRAY DIFFERENCE: with
//   `lanZeroDelayOverride` set tier 0's effective delay is 0, so the raw form is wrong
//   for every transition touching tier 0 -- the most common one there is. §3
// ⛔ A FLOOR DOMINATING BOTH ENDPOINTS COLLAPSES THE DELTA TO 0, AND THAT IS CORRECT:
//   felt delay did not change, so there is no prediction-stall debt to pay. §4
// ⛔ Publishes are unaffected -- the server's predicate compares tier INDICES. §4
inline int32_t tierDelayDeltaTicks(int32_t fromTier, int32_t toTier, const TimeConfig& cfg)
{
    return tierInputDelayTicks(toTier, cfg) - tierInputDelayTicks(fromTier, cfg);
}

// THE CLIENT PREDICTION-STALL DECISION for a wire tier transition.
//
// ⛔⛔ PASS `hadAnyTier`, NEVER INFER IT FROM `oldTier` -- 0 is both the replicated
//   property's compiled default and a legal tier. On a fresh connection's FIRST real
//   arrival the "before" value is that default, not a tier the client ever ran at: until
//   then it runs the pre-arrival no-tier fallback (see `ReplicatedTierConsumer`). §8
// ⛔ SO `tierDelayDeltaTicks(0, newTier, cfg)` STALLS BY THE WRONG QUANTITY IN THE WRONG
//   DIRECTION for every newTier except 0 -- at the shipped table the fallback is 4 and
//   tier 0 is 1, so a first arrival of tier 1 asks for a 1-tick stall where the true
//   delta is -2: a stall for a transition that made the client's delay SHORTER. §8
// ⛔ FIRST-EVER RESOLUTION -> ALWAYS ZERO, whatever newTier is. §8
// ⛔ The `oldTier == newTier` early-out is preserved: a callback can fire unchanged. §8
// ⛔ Only a POSITIVE delta is returned; the clock is also a no-op below zero. §8
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

// Outcome of a single `ConnectionTierTable::onRttSample` call.
//
// ⛔ LOCKED SHAPE -- tier and delta only; the delta's sign carries the direction. §3
// ⛔ NO TRANSITION REPORTS THE UNCHANGED TIER WITH A ZERO DELTA -- `!= 0` is the signal. §3
// ⛔ BUT A REAL TRANSITION CAN CARRY A ZERO DELTA and still move the ceiling / mute. §3
struct TierSampleResult
{
    int32_t newTierIndex    = 0;
    int32_t deltaDelayTicks = 0;
};

template <ConnectionAddress Address>
class ConnectionTierTable
{
public:
    // ⛔ Aliases of the free constants, so the two cannot describe different counts. §2
    static constexpr std::size_t kTierCount = kConnectionTierCount;

    static constexpr int32_t kMaxTierIndex = kMaxConnectionTierIndex;

    // Per-connection state. Public so tests and telemetry can read a snapshot;
    // ⛔ MUTATED ONLY THROUGH `onRttSample` / `reapDeadHandles`. §3
    struct TierState
    {
        // Current RTT tier, 0 (best) .. kMaxTierIndex (worst).
        int32_t currentTierIndex = 0;

        // EMA of observed round-trip time, milliseconds.
        // ⛔ SEEDED WITH THE FIRST SAMPLE, NEVER WITH 0 -- zero drags every fresh
        //   connection through a tier-0 phase at a delay far too short for its RTT. §3
        double smoothedRttMs = 0.0;

        // Dwell counter: samples observed since entering `currentTierIndex`.
        // ⛔ RESET TO 0 ON EVERY TRANSITION. §3
        int32_t ticksInCurrentTier = 0;

        // Tick of the most recent sample, for staleness-based reaping.
        int32_t lastSampleTick = 0;
    };

    // ⛔ THE CONFIG IS BORROWED, NOT OWNED; the caller must outlive the table. §1
    explicit ConnectionTierTable(const TimeConfig& cfg)
        : m_config(cfg)
    {
    }

    // Feed one RTT observation for `addr`, observed at `currentTick`. Creates the entry
    // on first sight, updates the EMA, advances the dwell counter, and applies at most
    // ONE tier transition per call. Returns the resulting tier and the change in
    // effective input delay.
    // ⛔ DELIBERATELY NOT [[nodiscard]] -- most call sites drive it for its state. §3
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

        // ⛔ COUNTED ON EVERY SAMPLE, so a new connection also serves the dwell. §3
        ++state.ticksInCurrentTier;

        const int32_t desired = desiredTierWithHysteresis(state);
        if (desired == state.currentTierIndex)
        {
            return TierSampleResult{ state.currentTierIndex, 0 };
        }

        // Gate 2: dwell.
        // ⛔ DISCARDED, NOT DEFERRED -- there is no pending-transition memory. §3
        // ⛔ A DWELL-BLOCKED SAMPLE REPORTS NO TRANSITION -- invisible by design. §3
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

    // 0..kMaxTierIndex.
    // ⛔ AN ADDRESS NEVER SAMPLED REPORTS THE BEST TIER, and the first sample corrects it. §3
    int32_t lookupTierIndex(const Address& addr) const
    {
        const auto it = m_entries.find(addr);
        return it == m_entries.end() ? 0 : it->second.currentTierIndex;
    }

    // The three tier -> behaviour lookups, each a thin address-keyed wrapper over the
    // corresponding SHARED free function at file scope.
    // ⛔ DO NOT RE-INLINE THE MATH HERE -- identical BY CONSTRUCTION is the whole point. §2

    // Effective Layer-1 input delay in ticks for this connection's tier.
    int32_t lookupInputDelayTicks(const Address& addr) const
    {
        return tierInputDelayTicks(lookupTierIndex(addr), m_config);
    }

    // Per-tier SOFT ceiling for `rollbackWindowTicks`.
    // ⛔ INTENDED API, NO PRODUCTION CALLER -- see `tierRollbackCeiling`. §7
    int32_t lookupRollbackCeiling(const Address& addr) const
    {
        return tierRollbackCeiling(lookupTierIndex(addr), m_config);
    }

    // Render-side input echo suppression, worst tier only.
    // ⛔ NO PRODUCTION CALLER; the wiring task is OPTIONAL. §7
    bool shouldMuteEcho(const Address& addr) const
    {
        return tierShouldMuteEcho(lookupTierIndex(addr), m_config);
    }

    // Evict entries whose connection has died OR that have gone silent for longer than
    // `deadlineTicks`.
    // ⛔ WITHOUT THIS THE MAP GROWS FOR THE PROCESS LIFETIME, one entry per connection. §3
    // ⛔ TWO CONDITIONS, TWO FAILURES: `isAlive()` catches a clean disconnect promptly;
    //   the deadline catches a half-open socket, or a handle that cannot go stale. §3
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

    // Read-only snapshot; nullptr when unknown.
    // ⛔ BY POINTER, so the EMA and dwell counter stay unsettable from outside. §3
    const TierState* findState(const Address& addr) const
    {
        const auto it = m_entries.find(addr);
        return it == m_entries.end() ? nullptr : &it->second;
    }

private:
    // Gate 1: directional hysteresis. Returns the tier this state SHOULD be in, or its
    // current tier when neither band is exceeded.
    // ⛔ ASYMMETRIC: promotion tests the CURRENT tier's upper boundary, demotion the
    //   tier-BELOW's -- leaving a `2 * hysteresis` dead-band that absorbs jitter. §3
    // ⛔ AT MOST ONE STEP PER CALL -- each step is a player-visible delay change. §3
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
