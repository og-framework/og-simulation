#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <type_traits>
#include <unordered_map>

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

// Effective Layer-1 input delay in ticks for `tierIndex`.
//
// `lanZeroDelayOverride` collapses tier 0 to zero delay: on a sub-millisecond
// local link there is no round trip left to hide, so the configured delay is
// pure added lag. Only tier 0 is affected — a bad connection inside a LAN
// session still gets its own tier's delay.
//
// NOTE this is the C2-locked REPLACES value: it IS the effective delay, and is
// never added to `TimeConfig::forcedInputLatencyTicks`. The baseline applies
// only when NO tier is available at all (see ReplicatedTierConsumer and
// ServerInputDelayQueue::effectiveDelay, which each own that fallback for their
// own "no tier" condition).
inline int32_t tierInputDelayTicks(int32_t tierIndex, const TimeConfig& cfg)
{
    const int32_t tier = clampConnectionTierIndex(tierIndex);
    if (tier == 0 && cfg.lanZeroDelayOverride)
    {
        return 0;
    }
    return cfg.rttTierInputDelays[tier];
}

// Per-tier SOFT ceiling for `rollbackWindowTicks`. The hard cap
// (`rollbackWindowHardCap`) is enforced elsewhere and is unaffected by tier.
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
inline int32_t tierDelayDeltaTicks(int32_t fromTier, int32_t toTier, const TimeConfig& cfg)
{
    return tierInputDelayTicks(toTier, cfg) - tierInputDelayTicks(fromTier, cfg);
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
