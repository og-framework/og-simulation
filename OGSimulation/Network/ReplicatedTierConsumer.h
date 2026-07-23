#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

#include "OGSimulation/Network/ConnectionTierTable.h"
#include "OGSimulation/PCTimeManagement/TimeConfig.h"

// ---------------------------------------------------------------------------
// ReplicatedTierConsumer — the CLIENT half of C.2 (Stage 5 / D5.1, T9).
//
// OPTION A, locked 2026-07-19 (backlog C1). The RTT tier is derived exactly
// once, by the SERVER, from the server's own per-connection FNetPing RoundTrip
// (ASimulationManagerUImpl::sampleAndDeriveConnectionTier), and the resulting
// index is replicated COND_OwnerOnly to the owning client. This class is the
// client's entire share of the tier system:
//
//   * it does NOT own a ConnectionTierTable,
//   * it does NOT call onRttSample,
//   * it does NOT sample, smooth, bucket, or otherwise derive a tier.
//
// It stores the replicated integer and turns it into behaviour via the SHARED
// lookups in ConnectionTierTable.h — the same functions the server's table
// delegates to. With one producer there is no second estimator to disagree with,
// which is precisely what removes the boundary-RTT tier split (and the recurring
// one-tick corrections it caused) that motivated the C1 decision.
//
// ANY client-side tier derivation added here is a bug, not an optimisation.
//
// RELATIONSHIP TO THE PREDICTION OFFSET. This is ADDITIVE to, and independent
// of, the existing NetworkTimeEstimator / ClientPredictionClock prediction-offset
// path. The tier supplies a Layer-1 INPUT DELAY; the estimator supplies the
// prediction offset that decides WHICH tick the client is simulating. The two
// coexist; neither replaces the other, and nothing in this header touches the
// estimator.
//
// PRE-ARRIVAL FALLBACK. Until the first OnRep actually lands, there is no
// authoritative tier and `TimeConfig::forcedInputLatencyTicks` — the documented
// "no per-connection tier is available" baseline — is used instead. This is
// deliberately keyed on ARRIVAL, not on the tier VALUE: the replicated property
// defaults to 0, which is also a perfectly legal tier, so a value test could not
// tell "server says tier 0" from "server has not spoken yet", and a client that
// had never heard from the server would silently adopt tier-0 timing.
//
// ENGINE-AGNOSTIC. Sim core: STL plus `OGSimulation/` headers only. The
// replicated value arrives as a plain integer; the UE transport
// (USimmableUpdateComponent's COND_OwnerOnly uint8 + OnRep_ConnectionTier) stays
// entirely on the UE side.
//
// THREADING. Not thread-safe, and single-threaded by construction: the only
// mutator (`onReplicatedTierReceived`) is called from OnRep_ConnectionTier, which
// is a GAME-THREAD UObject callback. A reader on the physics thread would be an
// unsynchronised cross-thread read — see the THREADING CONTRACT block in
// ServerInputDelayQueue.h for why adding a lock is the wrong fix for that shape
// of problem.
//
// NAMESPACE NOTE: declared in the GLOBAL namespace, matching the rest of the
// OGSim core (same note as ConnectionTierTable / NetConfig / SimulatableList).
// The design corpus writes `ogsim::` but no such namespace exists in this tree.
// ---------------------------------------------------------------------------

class ReplicatedTierConsumer
{
public:
    // The config is BORROWED, not owned — one TimeConfig instance is shared by
    // the whole time-management stack, and a copy here could silently diverge
    // from the one the clocks read. Caller must outlive the consumer.
    explicit ReplicatedTierConsumer(const TimeConfig& cfg)
        : m_config(cfg)
    {
    }

    // Feed one authoritative tier value, as received from the server. Call from
    // OnRep_ConnectionTier (game thread).
    //
    // The incoming value came off the wire, so it is CLAMPED into the legal tier
    // range rather than trusted: a corrupt or version-mismatched byte must not
    // index the TimeConfig tier arrays out of bounds. Clamping is the identity
    // for every value the server can legitimately produce.
    void onReplicatedTierReceived(int32_t tierIndex)
    {
        m_tierIndex = clampConnectionTierIndex(tierIndex);
        m_hasReceivedTier = true;
    }

    // True once at least one authoritative tier has arrived. Everything below
    // falls back to the no-tier baseline while this is false.
    bool hasReceivedTier() const
    {
        return m_hasReceivedTier;
    }

    // The last authoritative tier. Reports 0 before arrival; callers that need to
    // distinguish "tier 0" from "nothing yet" must ask hasReceivedTier().
    int32_t currentTierIndex() const
    {
        return m_tierIndex;
    }

    // THE C2 FORMULA, client side. Once a tier has arrived the effective delay is
    // `rttTierInputDelays[tier]` — it REPLACES the baseline and is NOT added to
    // it (locked 2026-07-19, backlog C2; the earlier additive draft algebraically
    // cancelled to a constant). Before arrival it is `forcedInputLatencyTicks`.
    //
    // This is the same value the server's ServerInputDelayQueue::effectiveDelay
    // computes for this connection, reached through the same shared lookup, so
    // the two ends delay by the identical number of ticks.
    int32_t effectiveInputDelayTicks() const
    {
        if (!m_hasReceivedTier)
        {
            return m_config.forcedInputLatencyTicks;
        }
        return tierInputDelayTicks(m_tierIndex, m_config);
    }

    // Per-tier SOFT rollback ceiling. Before any tier arrives the client keeps
    // the unescalated configured window — the tier can only ever RAISE the
    // ceiling, so the pre-arrival state must be the un-escalated one.
    int32_t effectiveRollbackCeiling() const
    {
        if (!m_hasReceivedTier)
        {
            return m_config.rollbackWindowTicks;
        }
        return tierRollbackCeiling(m_tierIndex, m_config);
    }

    // C.4 echo mute. NO PRODUCTION CALLER until the OPTIONAL task T15 — exposed
    // here so the client's mute decision is derived from the same shared lookup
    // as the server's rather than being re-implemented at a render-side call
    // site later. Never mutes before a tier has arrived: muting is a degraded-tier
    // response and "no information yet" is not evidence of a degraded link.
    bool shouldMuteEcho() const
    {
        return m_hasReceivedTier && tierShouldMuteEcho(m_tierIndex, m_config);
    }

    // Drop back to the pre-arrival state. For seamless travel / reconnect, where
    // the next session's tier must be re-established from scratch rather than
    // inherited from the previous server.
    void reset()
    {
        m_tierIndex = 0;
        m_hasReceivedTier = false;
    }

private:
    const TimeConfig& m_config;

    // 0 is the pre-arrival placeholder AND a legal tier; m_hasReceivedTier is
    // what separates the two. See the PRE-ARRIVAL FALLBACK note above.
    int32_t m_tierIndex = 0;
    bool    m_hasReceivedTier = false;
};
