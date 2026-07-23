// SPDX-License-Identifier: MPL-2.0
#include "ClientPredictionClock.h"

#include <cstdio>

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize( "", off )

namespace
{
    // TimeConfig stores tickFrequency as Hz (ticks-per-second); dt is the reciprocal.
    inline float dtFromConfig(const TimeConfig& cfg)
    {
        return static_cast<float>(1.0 / cfg.tickFrequency);
    }
}

ClientPredictionClock::ClientPredictionClock(const TimeConfig& config, const NetworkTimeEstimator& estimator, PCClockLoggerFn logger)
    : m_config(config)
    , m_estimator(estimator)
    , m_predictionTick(0)
    , m_resimulationTick(0)
    , m_gradualCorrectionCounter(0)
    , m_logger(std::move(logger))
{
}

// ---------------------------------------------------------------------------
// Internal helper: the standard one-tick forward advance.
// Also advances resimTick when the clocks are in sync (not resimulating).
// ---------------------------------------------------------------------------
static void doNormalAdvance(unsigned int& predictionTick, unsigned int& resimulationTick, ClientPredictionClock::PCClockLoggerFn* logger)
{
    if (predictionTick == resimulationTick)
        ++resimulationTick;
    ++predictionTick;

    // Per-tick "normal advance" log silenced to reduce log noise during rework.
    (void)logger;
}

// ---------------------------------------------------------------------------
// Prediction
// ---------------------------------------------------------------------------

ClientPredictionClock::AdvanceResult ClientPredictionClock::advancePrediction()
{
    const unsigned int targetTick = m_estimator.getTargetPredictionTick();
    const int          drift      = static_cast<int>(targetTick) - static_cast<int>(m_predictionTick);

    // Startup guard: skip drift correction until the *authority* clock is past
    // the warm-up period. The client's own tick counter is NOT gated — a
    // late-connect client that has never seen tick 60 on its own local counter
    // should still be allowed to HardResync forward as soon as authority is
    // available. This closes the "10-20 s late-start walks predictionTick 0..59
    // while server is 600+" corner case documented in
    // ../og-brawler-hit-resolution/netcode_finding_pred_offset_floor.md §3.2.
    const bool pastGuard = (targetTick >= m_config.minTicksBeforeDriftCheck);

    const unsigned int absDrift = drift >= 0 ? static_cast<unsigned int>(drift) : static_cast<unsigned int>(-drift);

    // [T11] PRIORITY ORDER: hard resync > tier-transition rollback debt >
    // ordinary graduated drift correction. Hoisted above the guard/dead-band
    // chain so the three cannot fight each other:
    //
    //  * A hard resync TELEPORTS the frontier onto authority and wipes the
    //    caches through the resync callbacks. Any rollback debt describes the
    //    PRE-teleport frontier, which no longer exists, so it is discarded
    //    rather than applied on top of the jump (applying both would overshoot
    //    backwards past authority).
    //  * Otherwise debt is paid BEFORE drift correction. Both are expressed as
    //    Stalls, and interleaving them would let a Skip cancel a rollback the
    //    tier change requires — the tier delay and the prediction offset are
    //    independent quantities and must not net each other out.
    if (pastGuard && absDrift > m_config.hardResyncThresholdTicks)
    {
        const unsigned int oldTick  = m_predictionTick;
        m_predictionTick           = targetTick;
        m_resimulationTick         = targetTick;
        m_gradualCorrectionCounter = 0;
        m_pendingRollbackTicks     = 0;

        if (m_logger)
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "[Warning] PCTM CPC: hard resync oldTick=%u -> newTick=%u drift=%d",
                oldTick, targetTick, drift);
            m_logger(buf);
        }

        fireResyncCallbacks(targetTick);
        return AdvanceResult::HardResync;
    }

    // [T11] Pay down one tick of tier-transition rollback. The mechanism is the
    // SAME Stall the soft-drift path uses: do not advance this call, so the
    // frontier falls one tick further behind where it would otherwise have been.
    //
    // WHY A STALL AND NOT `m_predictionTick -= delta`. The prediction frontier
    // is monotonically non-decreasing everywhere except hard resync, and hard
    // resync fires resync callbacks precisely BECAUSE moving it backwards
    // invalidates every cache slot above the new value. A silent decrement here
    // would leave already-populated slots ahead of the frontier that the resim
    // path would then consume as if they were fresh. Stalling reaches the same
    // relative position without ever breaking that invariant.
    if (m_pendingRollbackTicks > 0)
    {
        --m_pendingRollbackTicks;

        if (m_logger)
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "[Log] PCTM CPC: tier rollback Stall predTick=%u remainingRollback=%u",
                m_predictionTick, m_pendingRollbackTicks);
            m_logger(buf);
        }
        return AdvanceResult::Stall;
    }

    if (!pastGuard)
    {
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
        return AdvanceResult::Normal;
    }

    if (absDrift <= m_config.softDriftThresholdTicks)
    {
        // Dead-band — no correction.
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
        return AdvanceResult::Normal;
    }
    else
    {
        // Graduated correction zone. The hard-resync zone was already handled
        // and returned above (T11 priority hoist), so reaching here means
        // softDriftThresholdTicks < absDrift <= hardResyncThresholdTicks.
        ++m_gradualCorrectionCounter;
        if (m_gradualCorrectionCounter >= m_config.gradualCorrectionRate)
        {
            m_gradualCorrectionCounter = 0;

            if (drift > 0)
            {
                // Client is BEHIND target → skip: apply an extra advance before the normal one.
                // Mirror the resim tick if the clocks are in sync, so isResimulating() stays false.
                if (m_predictionTick == m_resimulationTick)
                    ++m_resimulationTick;
                ++m_predictionTick;

                if (m_logger)
                {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                        "[Log] PCTM CPC: graduated Skip  predTick=%u targetTick=%u drift=+%d",
                        m_predictionTick, targetTick, drift);
                    m_logger(buf);
                }
                doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
                return AdvanceResult::Skip;
            }
            else
            {
                // Client is AHEAD of target → stall: do NOT advance the tick at all.
                // Return early without calling doNormalAdvance.
                if (m_logger)
                {
                    char buf[192];
                    std::snprintf(buf, sizeof(buf),
                        "[Log] PCTM CPC: graduated Stall predTick=%u targetTick=%u drift=%d",
                        m_predictionTick, targetTick, drift);
                    m_logger(buf);
                }
                return AdvanceResult::Stall;
            }
        }
        // Counter not yet triggered — normal advance within the graduated zone.
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
        return AdvanceResult::Normal;
    }
}

// ---------------------------------------------------------------------------
// Tier-transition rollback (T11)
// ---------------------------------------------------------------------------

void ClientPredictionClock::requestTierTransitionRollback(int32_t deltaDelayTicks)
{
    // Downward / no-change transitions need no correction — see the header.
    if (deltaDelayTicks <= 0)
        return;

    m_pendingRollbackTicks += static_cast<unsigned int>(deltaDelayTicks);

    if (m_logger)
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "[Log] PCTM CPC: tier rollback requested delta=%d totalPending=%u predTick=%u",
            static_cast<int>(deltaDelayTicks), m_pendingRollbackTicks, m_predictionTick);
        m_logger(buf);
    }
}

unsigned int ClientPredictionClock::getPendingTierRollbackTicks() const
{
    return m_pendingRollbackTicks;
}

unsigned int ClientPredictionClock::getPredictionTick() const
{
    return m_predictionTick;
}

SimulationTimeStep ClientPredictionClock::getPredictionStep() const
{
    return SimulationTimeStep(m_predictionTick, /*isResimulating=*/false, StepKind::Normal, dtFromConfig(m_config));
}

// ---------------------------------------------------------------------------
// Resimulation
// ---------------------------------------------------------------------------

void ClientPredictionClock::startResimulation(unsigned int tick)
{
    m_resimulationTick = tick;

    if (m_logger)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "[Log] ClientPredictionClock::starting resim resimTick=%u predictionTick=%u",
            m_resimulationTick, m_predictionTick);
        m_logger(buf);
    }
}

void ClientPredictionClock::advanceResimulation()
{
    ++m_resimulationTick;
    
    if (m_logger)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "[Log] ClientPredictionClock::advance resim resimTick=%u",
            m_resimulationTick);
        m_logger(buf);
    }
}

void ClientPredictionClock::finishResimulation()
{

    m_resimulationTick = m_predictionTick;

    if (m_logger)
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "[Log] ClientPredictionClock::finishResimulation resimTick=%u predictionTick=%u",
            m_resimulationTick, m_predictionTick);
        m_logger(buf);
    }
}

unsigned int ClientPredictionClock::getResimulationTick() const
{
    return m_resimulationTick;
}

SimulationTimeStep ClientPredictionClock::getResimulationStep() const
{
    return SimulationTimeStep(m_resimulationTick, /*isResimulating=*/true, StepKind::Normal, dtFromConfig(m_config));
}

bool ClientPredictionClock::isResimulating() const
{
    return m_resimulationTick < m_predictionTick;
}

// ---------------------------------------------------------------------------
// Drift evaluation — pure query, no side effects
// Reports which correction ZONE we are in based on current drift magnitude.
// advancePrediction() may still suppress the action this frame if the
// gradualCorrectionCounter has not yet reached gradualCorrectionRate.
// ---------------------------------------------------------------------------

ClientPredictionClock::DriftAction ClientPredictionClock::evaluateDrift() const
{
    const unsigned int targetTick = m_estimator.getTargetPredictionTick();
    const int          drift      = static_cast<int>(targetTick) - static_cast<int>(m_predictionTick);

    // Startup guard: authority-only form — MUST match advancePrediction()'s guard
    // exactly (see the fuller comment there +
    // ../og-brawler-hit-resolution/netcode_finding_pred_offset_floor.md §3.2). If the
    // two guards diverge, this query reports a different drift zone than
    // advancePrediction() would act on for the same tick. The
    // WarmupGuard.EvaluateDriftConsistency test pins the two occurrences together.
    const bool pastGuard = (targetTick >= m_config.minTicksBeforeDriftCheck);

    const unsigned int absDrift = drift >= 0 ? static_cast<unsigned int>(drift) : static_cast<unsigned int>(-drift);

    // [T11] Mirror advancePrediction()'s priority order EXACTLY — hard resync,
    // then rollback debt, then the guard/dead-band chain. This query's whole
    // contract is "what advancePrediction() would do right now", so a pending
    // rollback has to be visible here too or the two answers diverge for every
    // tick between a tier transition and its last paid-down Stall.
    if (pastGuard && absDrift > m_config.hardResyncThresholdTicks)
        return DriftAction::HardResync;

    if (m_pendingRollbackTicks > 0)
        return DriftAction::Stall;

    if (!pastGuard)
        return DriftAction::None;

    if (absDrift <= m_config.softDriftThresholdTicks)
        return DriftAction::None;

    return drift > 0 ? DriftAction::Skip : DriftAction::Stall;
}

// ---------------------------------------------------------------------------
// Resync callbacks
// ---------------------------------------------------------------------------

unsigned int ClientPredictionClock::registerResyncCallback(ResyncCallback cb)
{
    m_resyncCallbacks.push_back(std::move(cb));
    return static_cast<uint32>(m_resyncCallbacks.size() - 1);
}

void ClientPredictionClock::unregisterResyncCallback(unsigned int id)
{
    std::swap(m_resyncCallbacks[id], m_resyncCallbacks.back());
    m_resyncCallbacks.pop_back();
}

void ClientPredictionClock::fireResyncCallbacks(unsigned int newTick)
{
    for (auto& cb : m_resyncCallbacks)
        cb(newTick);
}

#pragma optimize( "", on )
// pragma optimize on.
