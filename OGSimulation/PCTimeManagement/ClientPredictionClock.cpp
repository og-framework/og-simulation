// SPDX-License-Identifier: MPL-2.0
#include "ClientPredictionClock.h"

#include <cstdio>

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

    // Startup guard: skip drift correction until both clocks are past the warm-up period.
    const bool pastGuard = (m_predictionTick >= m_config.minTicksBeforeDriftCheck)
                        && (targetTick        >= m_config.minTicksBeforeDriftCheck);

    if (!pastGuard)
    {
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
        return AdvanceResult::Normal;
    }

    const unsigned int absDrift = drift >= 0 ? static_cast<unsigned int>(drift) : static_cast<unsigned int>(-drift);

    if (absDrift <= m_config.softDriftThresholdTicks)
    {
        // Dead-band — no correction.
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
        return AdvanceResult::Normal;
    }
    else if (absDrift <= m_config.hardResyncThresholdTicks)
    {
        // Graduated correction zone.
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
    else
    {
        // Hard resync: jump straight to target, reset counters, fire callbacks.
        // Both clocks jump together so isResimulating() returns false after the jump
        // (the cache wipe from callbacks makes previous resim state irrelevant).
        const unsigned int oldTick = m_predictionTick;
        m_predictionTick          = targetTick;
        m_resimulationTick        = targetTick;
        m_gradualCorrectionCounter = 0;

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

    const bool pastGuard = (m_predictionTick >= m_config.minTicksBeforeDriftCheck)
                        && (targetTick        >= m_config.minTicksBeforeDriftCheck);

    if (!pastGuard)
        return DriftAction::None;

    const unsigned int absDrift = drift >= 0 ? static_cast<unsigned int>(drift) : static_cast<unsigned int>(-drift);

    if (absDrift <= m_config.softDriftThresholdTicks)
        return DriftAction::None;

    if (absDrift <= m_config.hardResyncThresholdTicks)
        return drift > 0 ? DriftAction::Skip : DriftAction::Stall;

    return DriftAction::HardResync;
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
