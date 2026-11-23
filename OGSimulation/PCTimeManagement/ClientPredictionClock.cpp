// SPDX-License-Identifier: MPL-2.0
#include "ClientPredictionClock.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "OGSimulation/OGAssert.h"

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

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
    , m_recentreBaseSamples(estimator.getMeasuredRTTSampleCount())
    , m_logger(std::move(logger))
{
    // [og-time-dilation task 5] The re-centre is armed from here, so publish before any advance. §18
    publishCorrectionPhase();
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

// [og-time-dilation task 5] The single exit: every return of advanceAndCorrect() reaches
// the two stores below, which are this advance's last statements. §18
ClientPredictionClock::AdvanceResult ClientPredictionClock::advancePrediction()
{
    const AdvanceResult result = advanceAndCorrect();
    publishCorrectionPhase();
    publishTargetRead();  // [og-time-dilation task 10c] after the body: this advance's read. §18
    return result;
}

ClientPredictionClock::AdvanceResult ClientPredictionClock::advanceAndCorrect()
{
    // [P3-10] The advance's one read of the first-RTT flag, then its one offset (readTarget).
    const TargetRead   estimate   = readTarget();
    m_lastReadTargetTick          = estimate.targetTick;  // [og-time-dilation task 10c] for the published read, §18
    m_firstRttSeenLastAdvance     = estimate.hasFirstRtt;
    const unsigned int targetTick = estimate.targetTick;
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

    // [T11] PRIORITY ORDER: hard resync > input-delay transition debt (Stalls
    // or, since og-input-latency P6-2, Skips) > ordinary graduated drift
    // correction. Hoisted above the guard/dead-band chain so the three cannot
    // fight each other:
    //
    //  * A hard resync TELEPORTS the frontier onto authority and wipes the
    //    caches through the resync callbacks. Any transition debt describes the
    //    PRE-teleport frontier, which no longer exists, so it is discarded
    //    rather than applied on top of the jump (applying both would overshoot
    //    past authority, backwards for Stalls, forwards for Skips). The jump
    //    itself breaks the capture stream the debt was meant to shift.
    //  * Otherwise debt is paid BEFORE drift correction. Both are expressed as
    //    Stalls and Skips, and interleaving them would let a drift correction
    //    cancel one the delay change requires — the input delay and the
    //    prediction offset are independent quantities and must not net each
    //    other out.
    //
    // [P3-10] A hard resync taken before the estimator had any RTT reading landed
    // on the floor target. When the first reading arrives and moves the offset,
    // the clock resyncs once more, onto the measured target (firstRttResyncDue).
    // The wait ends only on an advance whose previous advance already read the
    // flag true: a flag read before its seed is visible reads the floor.
    const bool firstRttResync = firstRttResyncDue(estimate, pastGuard, drift);
    if (m_resyncAwaitsFirstRtt && pastGuard && estimate.firstRttSettled)
        m_resyncAwaitsFirstRtt = false;

    if (firstRttResync || (pastGuard && absDrift > m_config.hardResyncThresholdTicks))
    {
        const unsigned int oldTick  = m_predictionTick;
        m_predictionTick           = targetTick;
        m_resimulationTick         = targetTick;
        m_gradualCorrectionCounter = 0;
        m_pendingInputDelayTransitionTicks.store(0, std::memory_order_relaxed);

        // [P3-10] Only a resync that landed on the floor offset with no settled RTT
        // reading waits for the first one (with no reading the offset IS the floor;
        // a flag read before its seed also reads it). Gated with the re-centre: at
        // recentreSettleSamples == 0 the clock is the one before both.
        const unsigned int landingOffset = m_resyncOffsetTicks;
        m_resyncAwaitsFirstRtt = m_config.recentreSettleSamples > 0u && !estimate.firstRttSettled
            && estimate.offsetTicks == m_config.predOffsetFloorTicks;
        m_resyncOffsetTicks    = estimate.offsetTicks;

        // [task 27] A hard resync clears the re-centre's debt and re-arms it: the
        // settle window counts from here, so it includes this frame's catch-up
        // steps, and the median run restarts at the new frontier. Before the
        // first decision it is still the join's arm.
        armRecentre(m_recentreDecidedOnce ? RecentreCause::Resync : RecentreCause::Join, targetTick);
        ++m_hardResyncCount;  // [og-time-dilation task 5] the published phase's count, §18

        // ⛔ Event seam - the two ticks before the count, in that order.
        m_eventSeamDiagnostics.lastHardResyncFromTick = oldTick;
        m_eventSeamDiagnostics.lastHardResyncToTick   = targetTick;
        ++m_eventSeamDiagnostics.hardResyncCount;

        if (m_logger)
        {
            char cause[64] = "";
            if (firstRttResync)
                std::snprintf(cause, sizeof(cause), " cause=firstRtt offset=%u->%u", landingOffset, m_resyncOffsetTicks);
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "[Warning] PCTM CPC: hard resync oldTick=%u -> newTick=%u drift=%d%s",
                oldTick, targetTick, drift, cause);
            m_logger(buf);
        }

        fireResyncCallbacks(targetTick);
        return AdvanceResult::HardResync;
    }

    // [task 27] Every other advance of an armed re-centre is one reading of its
    // median run, taken before any branch below moves the frontier. A decided
    // one re-arms first if the RTT-only offset has moved away from its decision.
    // [P3-11] The advance counts in the settle window first, so a re-arm on it
    // starts the window at the next advance, as a hard resync's does.
    countRecentreWindow(estimate.hasFirstRtt);
    trackRecentreRearm(pastGuard);
    recordRecentreReading(pastGuard);

    // [T11] Pay down one tick of an input-delay increase. The mechanism is the
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
    //
    // [P6-2] Pay down one tick of an input-delay DECREASE as a Skip: the SAME
    // shape as the graduated Skip below (one extra advance, then the normal
    // one), so the tick in between is never stepped and no input is captured
    // for it. That missing capture tick is what drains the server's backlog
    // (see requestInputDelayTransition in the header).
    const int32_t paid = consumeOneInputDelayTransitionTick();
    if (paid > 0)
    {
        // ⛔ Event seam - tick before count. This is a Stall like the graduated one.
        m_eventSeamDiagnostics.lastStallTick = m_predictionTick;
        ++m_eventSeamDiagnostics.stallCount;

        if (m_logger)
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "[Log] PCTM CPC: input delay increase Stall predTick=%u remainingStalls=%d",
                m_predictionTick, m_pendingInputDelayTransitionTicks.load(std::memory_order_relaxed));
            m_logger(buf);
        }
        return AdvanceResult::Stall;
    }
    if (paid < 0)
    {
        if (m_predictionTick == m_resimulationTick)
            ++m_resimulationTick;
        ++m_predictionTick;
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);

        // ⛔ Event seam - tick before count; the tick is the one JUMPED TO, as the graduated Skip's.
        m_eventSeamDiagnostics.lastSkipTick = m_predictionTick;
        ++m_eventSeamDiagnostics.skipCount;

        if (m_logger)
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "[Log] PCTM CPC: input delay decrease Skip predTick=%u remainingSkips=%d",
                m_predictionTick, -m_pendingInputDelayTransitionTicks.load(std::memory_order_relaxed));
            m_logger(buf);
        }
        return AdvanceResult::Skip;
    }

    if (!pastGuard)
    {
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
        return AdvanceResult::Normal;
    }

    // [task 27] The one-shot re-centre: decided once per arm; its debt keeps the
    // graduated path running inside the dead band until every owed tick is paid.
    decideRecentre(targetTick);

    const bool inDeadBand = absDrift <= m_config.softDriftThresholdTicks;
    if (inDeadBand && m_recentreOwedTicks == 0u)
    {
        // Dead-band — no correction.
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
        return AdvanceResult::Normal;
    }
    else
    {
        // Graduated correction zone. The hard-resync zone was already handled
        // and returned above (T11 priority hoist), so reaching here means
        // softDriftThresholdTicks < absDrift <= hardResyncThresholdTicks, or a
        // re-centre still owes a correction inside the dead band. Inside the band
        // the debt sets the direction; outside it the drift does, as before.
        ++m_gradualCorrectionCounter;
        if (m_gradualCorrectionCounter >= m_config.gradualCorrectionRate)
        {
            m_gradualCorrectionCounter = 0;

            // [task 27] Every graduated correction fired while a debt is owed pays
            // one tick of it, so the episode ends after exactly the owed corrections.
            const bool skip = inDeadBand ? !m_recentreAhead : drift > 0;
            if (m_recentreOwedTicks > 0u)
                --m_recentreOwedTicks;

            if (skip)
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
                        "[Log] PCTM CPC: graduated Skip  predTick=%u targetTick=%u drift=%+d",
                        m_predictionTick, targetTick, drift);
                    m_logger(buf);
                }
                doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);

                // ⛔ Event seam - tick before count; the tick is the one JUMPED TO.
                m_eventSeamDiagnostics.lastSkipTick = m_predictionTick;
                ++m_eventSeamDiagnostics.skipCount;
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

                // ⛔ Event seam - tick before count; the frontier did not move.
                m_eventSeamDiagnostics.lastStallTick = m_predictionTick;
                ++m_eventSeamDiagnostics.stallCount;
                return AdvanceResult::Stall;
            }
        }
        // Counter not yet triggered — normal advance within the graduated zone.
        doNormalAdvance(m_predictionTick, m_resimulationTick, &m_logger);
        return AdvanceResult::Normal;
    }
}

// ---------------------------------------------------------------------------
// Input-delay transition debt (T11; signed since og-input-latency P6-2)
// ---------------------------------------------------------------------------

void ClientPredictionClock::requestInputDelayTransition(int32_t deltaDelayTicks)
{
    if (deltaDelayTicks != 0)
    {
        const int32_t total =
            m_pendingInputDelayTransitionTicks.fetch_add(deltaDelayTicks, std::memory_order_relaxed) + deltaDelayTicks;

        if (m_logger)
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "[Log] PCTM CPC: input delay %s requested delta=%d totalPending=%d predTick=%u",
                deltaDelayTicks > 0 ? "increase stall" : "decrease skip",
                static_cast<int>(deltaDelayTicks), static_cast<int>(total), m_predictionTick);
            m_logger(buf);
        }
    }

    // [og-time-dilation task 5] The single exit publishes, the no-op too. §18
    publishCorrectionPhase();
}

void ClientPredictionClock::requestInputDelayIncreaseStall(int32_t deltaDelayTicks)
{
    // Positive deltas only (the T11 contract); the signed entry point is
    // requestInputDelayTransition, which this forwards to. A non-positive delta
    // still publishes, as the no-op always did.
    requestInputDelayTransition(deltaDelayTicks > 0 ? deltaDelayTicks : 0);
}

unsigned int ClientPredictionClock::getRequiredInputDelayIncreaseStallTicks() const
{
    const int32_t pending = m_pendingInputDelayTransitionTicks.load(std::memory_order_relaxed);
    return pending > 0 ? static_cast<unsigned int>(pending) : 0u;
}

int32_t ClientPredictionClock::getPendingInputDelayTransitionTicks() const
{
    return m_pendingInputDelayTransitionTicks.load(std::memory_order_relaxed);
}

// One tick toward 0 by compare-exchange, so a request landing between the load
// and the store (a frame-locked client requests on the game thread) is never lost.
int32_t ClientPredictionClock::consumeOneInputDelayTransitionTick()
{
    int32_t pending = m_pendingInputDelayTransitionTicks.load(std::memory_order_relaxed);
    while (pending != 0)
    {
        const int32_t step = pending > 0 ? 1 : -1;
        if (m_pendingInputDelayTransitionTicks.compare_exchange_weak(pending, pending - step, std::memory_order_relaxed))
            return step;
    }
    return 0;
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
    const TargetRead   estimate   = readTarget();
    const unsigned int targetTick = estimate.targetTick;
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
    // then the transition debt, then the guard/dead-band chain. This query's
    // whole contract is "what advancePrediction() would do right now", so a
    // pending Stall or Skip (P6-2) has to be visible here too or the two
    // answers diverge for every tick between a delay transition and its last
    // paid-down correction.
    if (firstRttResyncDue(estimate, pastGuard, drift) || (pastGuard && absDrift > m_config.hardResyncThresholdTicks))
        return DriftAction::HardResync;

    const int32_t pendingTransition = m_pendingInputDelayTransitionTicks.load(std::memory_order_relaxed);
    if (pendingTransition > 0)
        return DriftAction::Stall;
    if (pendingTransition < 0)
        return DriftAction::Skip;

    if (!pastGuard)
        return DriftAction::None;

    if (absDrift <= m_config.softDriftThresholdTicks)
    {
        // [task 27] Inside the band only a re-centre's debt corrects, in its own direction.
        const int pending = recentrePendingTicks(estimate.hasFirstRtt);
        if (pending == 0)
            return DriftAction::None;
        return pending > 0 ? DriftAction::Stall : DriftAction::Skip;
    }

    return drift > 0 ? DriftAction::Skip : DriftAction::Stall;
}

// [P3-10] The advance's estimator read: the first-RTT flag once, BEFORE the offset,
// and the target from that one offset, so the due check, the target a resync lands
// on and the offset it records are one value (getTargetPredictionTick's arithmetic).
// Rationale: docs/TimeConfig-rationale.md §16; ThreadingCrossings.md row 10.
ClientPredictionClock::TargetRead ClientPredictionClock::readTarget() const
{
    TargetRead estimate;
    estimate.hasFirstRtt     = m_estimator.hasFirstRTTSample();
    estimate.firstRttSettled = estimate.hasFirstRtt && m_firstRttSeenLastAdvance;
    estimate.offsetTicks     = m_estimator.getPredictionOffsetTicks();
    estimate.targetTick      = m_estimator.getLastAuthorityTick() + estimate.offsetTicks;
    return estimate;
}

// [P3-10] Due on the first advance past the guard whose previous advance already
// read the estimator's first RTT flag, when that reading moved the prediction offset
// off the one the resync landed with and the clock is not already on the target
// (drift 1). Only one try: advancePrediction clears the wait on that advance either way.
// Rationale: docs/TimeConfig-rationale.md §16.
bool ClientPredictionClock::firstRttResyncDue(const TargetRead& estimate, bool pastGuard, int drift) const
{
    return m_resyncAwaitsFirstRtt && pastGuard && estimate.firstRttSettled
        && estimate.offsetTicks != m_resyncOffsetTicks && drift != 1;
}

// ---------------------------------------------------------------------------
// One-shot re-centre (og-input-latency task 27)
// ---------------------------------------------------------------------------

bool ClientPredictionClock::isRecentreArmed() const
{
    return m_config.recentreSettleSamples > 0u && m_recentreArmed;
}

bool ClientPredictionClock::isRecentring() const
{
    return m_recentreOwedTicks > 0u;
}

// ---------------------------------------------------------------------------
// The published correction phase (og-time-dilation task 5). Encoded from the
// clock's own accessors, so the word cannot disagree with them; one relaxed
// store, one relaxed load. Rationale: docs/TimeConfig-rationale.md §18.
// ---------------------------------------------------------------------------

void ClientPredictionClock::publishCorrectionPhase()
{
    uint32_t word = (m_hardResyncCount & kPhaseHardResyncMask) << kPhaseHardResyncShift;
    if (isRecentreArmed())
        word |= kPhaseRecentreArmedBit;
    if (isRecentring())
        word |= kPhaseRecentringBit;
    if (m_pendingInputDelayTransitionTicks.load(std::memory_order_relaxed) != 0)
        word |= kPhaseTransitionDebtBit;
    if (m_resyncAwaitsFirstRtt)
        word |= kPhaseAwaitsFirstRttBit;
    m_publishedPhase.store(word, std::memory_order_relaxed);
}

ClockCorrectionPhase ClientPredictionClock::publishedCorrectionPhase() const
{
    const uint32_t word = m_publishedPhase.load(std::memory_order_relaxed);
    ClockCorrectionPhase phase;
    phase.recentreArmed  = (word & kPhaseRecentreArmedBit) != 0u;
    phase.recentring     = (word & kPhaseRecentringBit) != 0u;
    phase.transitionDebt = (word & kPhaseTransitionDebtBit) != 0u;
    phase.awaitsFirstRtt = (word & kPhaseAwaitsFirstRttBit) != 0u;
    phase.hardResyncs    = word >> kPhaseHardResyncShift;
    return phase;
}

// ---------------------------------------------------------------------------
// The published target read (og-time-dilation task 10c). The count and the
// target share one word, so a reader never pairs one advance's count with
// another's target; one relaxed store, one relaxed load. The count wraps at
// 2^32, which a reader comparing it for a change never notices.
// Rationale: docs/TimeConfig-rationale.md §18.
// ---------------------------------------------------------------------------

void ClientPredictionClock::publishTargetRead()
{
    ++m_advances;
    const uint64_t word = (static_cast<uint64_t>(m_advances) << 32u) | static_cast<uint64_t>(m_lastReadTargetTick);
    m_publishedTargetRead.store(word, std::memory_order_relaxed);
}

ClockTargetRead ClientPredictionClock::publishedTargetRead() const
{
    const uint64_t word = m_publishedTargetRead.load(std::memory_order_relaxed);
    ClockTargetRead read;
    read.advances   = static_cast<uint32_t>(word >> 32u);
    read.targetTick = static_cast<uint32_t>(word & 0xFFFFFFFFull);
    return read;
}

// [P3-11] The settle window is time, not samples: recentreSettleSamples advances
// since the arm on which the estimator had a reading. One advance is one tick, so
// at the engine source's one reading per authority tick it spans what the sample
// count did; at SimClock's 20 Hz pongs it no longer takes three times as long.
// [P3-12] It also needs kRecentreMinSamples measured samples since the arm: a
// catch-up batch runs many advances in one frame, so there advances are not time.
// Rationale: docs/TimeConfig-rationale.md §14.
bool ClientPredictionClock::recentreWindowFull(unsigned int windowAdvances) const
{
    return m_config.recentreSettleSamples > 0u && windowAdvances >= m_config.recentreSettleSamples
        && m_estimator.getMeasuredRTTSampleCount() - m_recentreBaseSamples >= kRecentreMinSamples;
}

void ClientPredictionClock::countRecentreWindow(bool hasReading)
{
    if (m_recentreArmed && hasReading)
        ++m_recentreWindowAdvances;
}

// Arms a re-centre: no debt, the settle window counting from the estimator's
// count now, and the median run restarting at frontierTick (the tick the clock
// last stepped, so the next advance's reading is the tick it steps).
void ClientPredictionClock::armRecentre(RecentreCause cause, unsigned int frontierTick)
{
    m_recentreArmed           = true;
    m_recentreCause           = cause;
    m_recentreOwedTicks       = 0;
    m_recentreBaseSamples     = m_estimator.getMeasuredRTTSampleCount();
    m_recentreWindowAdvances  = 0;
    m_recentreUncorrectedTick = frontierTick;
    m_recentreReadingCount    = 0;
    m_recentreReadingNext     = 0;
    m_recentreRearmRun        = 0;
}

// [task 27 reworks 3, 4] The re-arm's offset: getPredictionOffsetTicks() without its
// jitter term, max(predOffsetFloorTicks, ceil(smoothedRTT * tickFrequency)). The full
// offset carries jitterMultiplier times the jitter EWMA, which a heavy-tailed
// steady link moves by several ticks with no RTT step (review_27 R1). The floor
// is the target's own (m_config is the estimator's TimeConfig), so an RTT move
// entirely under it, which leaves the target where it is, cannot re-arm (Ph3 R13).
unsigned int ClientPredictionClock::recentreRttOffsetTicks() const
{
    const double ceiled = std::ceil(m_estimator.getSmoothedRTT() * m_config.tickFrequency);
    const unsigned int rttTicks = ceiled > 0.0 ? static_cast<unsigned int>(ceiled) : 0u;
    return std::max(rttTicks, static_cast<unsigned int>(m_config.predOffsetFloorTicks));
}

bool ClientPredictionClock::recentreOffsetMoved() const
{
    const unsigned int offset = recentreRttOffsetTicks();
    const unsigned int moved  = offset >= m_recentreDecisionRttOffset ? offset - m_recentreDecisionRttOffset
                                                                      : m_recentreDecisionRttOffset - offset;
    return moved >= kRecentreRearmTicks;
}

// True when this advance completes the run that re-arms a decided re-centre.
bool ClientPredictionClock::recentreRearmDue() const
{
    return !m_recentreArmed && recentreOffsetMoved() && m_recentreRearmRun + 1u >= kRecentreMedianAdvances;
}

// [task 27 reworks 2-4] A decided re-centre re-arms once the floored RTT-only offset
// has stood kRecentreRearmTicks or more from its value at the decision on every one
// of the last kRecentreMedianAdvances advances (an RTT step after the decision).
// The jitter term is left out of this test, so only the RTT EWMA can make the
// run, and only above the floor; an outstanding debt is cleared, the next decision measures from the
// current state (the full offset). Never runs while armed, so never at 0.
void ClientPredictionClock::trackRecentreRearm(bool pastGuard)
{
    if (m_recentreArmed)
        return;
    if (!pastGuard || !recentreOffsetMoved())
    {
        m_recentreRearmRun = 0;
        return;
    }
    if (++m_recentreRearmRun < kRecentreMedianAdvances)
        return;

    m_recentreRearmFromOffset = m_recentreDecisionRttOffset;
    m_recentreRearmToOffset   = recentreRttOffsetTicks();
    armRecentre(RecentreCause::Rearm, m_predictionTick);
}

// The re-centre's zero is drift == 1, not 0: drift is the target minus the tick
// the clock last stepped, so a Normal advance at drift 1 steps the target tick
// itself, which is also where a hard resync lands. A lead error is 1 - drift,
// which is the lead over the authority tick minus the prediction offset.
//
// A reading is the lead over the authority tick the clock would read had every
// advance since the arm been Normal: m_recentreUncorrectedTick moves one tick
// per advance, the frontier 0 (Stall), 1 or 2 (Skip), so the clock's own
// corrections inside the run do not move its median. The median filters the
// authority tick's arrival phase; the offset is the decision's own, so an RTT
// step inside the run (an outlier re-seed lands on the window's last sample)
// counts in full.
void ClientPredictionClock::recordRecentreReading(bool pastGuard)
{
    if (!m_recentreArmed)
        return;

    ++m_recentreUncorrectedTick;
    if (pastGuard)
    {
        m_recentreReadings[m_recentreReadingNext] =
            static_cast<int>(m_recentreUncorrectedTick - m_estimator.getLastAuthorityTick());
        m_recentreReadingNext  = (m_recentreReadingNext + 1u) % kRecentreMedianAdvances;
        m_recentreReadingCount = std::min(m_recentreReadingCount + 1u, kRecentreMedianAdvances);
    }
}

// N = round(median lead over authority of the run, moved to today's frontier,
// minus today's offset). uncorrectedTick is m_recentreUncorrectedTick as of
// this advance; extraReading, when given, joins the run without being stored
// (evaluateDrift's query). An even run's median is the midpoint of its two
// middle readings, rounded half away from zero.
int ClientPredictionClock::recentreCorrectionTicks(unsigned int uncorrectedTick, const int* extraReading) const
{
    std::array<int, kRecentreMedianAdvances> run = m_recentreReadings;
    unsigned int count = m_recentreReadingCount;
    if (extraReading)
    {
        run[m_recentreReadingNext] = *extraReading;
        count = std::min(count + 1u, kRecentreMedianAdvances);
    }
    if (count == 0u)
        return 0;

    std::sort(run.begin(), run.begin() + count);
    const int twiceMedian    = (count % 2u == 1u) ? 2 * run[count / 2u] : run[count / 2u - 1u] + run[count / 2u];
    const int ownCorrections = static_cast<int>(uncorrectedTick - (m_predictionTick + 1u));
    const int offset         = static_cast<int>(m_estimator.getPredictionOffsetTicks());
    const int twice          = twiceMedian - 2 * (ownCorrections + offset);
    return twice >= 0 ? (twice + 1) / 2 : -((1 - twice) / 2);
}

// What a re-centre would make the next advance correct, signed as a lead error
// (> 0: the client is ahead, Stall). The owed debt if one runs; otherwise, on
// the advance that decides, the decision itself; otherwise 0. hasReading is the
// advance's flag read, which that advance counts in the window (P3-11).
int ClientPredictionClock::recentrePendingTicks(bool hasReading) const
{
    if (recentreRearmDue())
        return 0;
    if (m_recentreOwedTicks > 0u)
        return m_recentreAhead ? static_cast<int>(m_recentreOwedTicks) : -static_cast<int>(m_recentreOwedTicks);
    if (!m_recentreArmed || !recentreWindowFull(m_recentreWindowAdvances + (hasReading ? 1u : 0u)))
        return 0;
    const unsigned int uncorrectedTick = m_recentreUncorrectedTick + 1u;
    const int          readingNow      = static_cast<int>(uncorrectedTick - m_estimator.getLastAuthorityTick());
    return recentreCorrectionTicks(uncorrectedTick, &readingNow);
}

void ClientPredictionClock::decideRecentre(unsigned int targetTick)
{
    if (!m_recentreArmed || !recentreWindowFull(m_recentreWindowAdvances))
        return;

    const int owed = recentreCorrectionTicks(m_recentreUncorrectedTick, /*extraReading=*/nullptr);
    m_recentreArmed          = false;
    m_recentreAhead          = owed > 0;
    m_recentreOwedTicks      = static_cast<unsigned int>(owed >= 0 ? owed : -owed);
    m_recentreDecidedOnce    = true;
    m_recentreDecisionRttOffset = recentreRttOffsetTicks();
    m_recentreRearmRun          = 0;

    if (m_logger)
    {
        const int toLead = static_cast<int>(targetTick) - static_cast<int>(m_estimator.getLastAuthorityTick());
        const char* const cause = m_recentreCause == RecentreCause::Join   ? "join"
                                : m_recentreCause == RecentreCause::Resync ? "resync"
                                                                           : "rearm";
        char rearm[64] = "";
        if (m_recentreCause == RecentreCause::Rearm)
            std::snprintf(rearm, sizeof(rearm), " fromTarget=%u toTarget=%u", m_recentreRearmFromOffset, m_recentreRearmToOffset);
        char buf[288];
        std::snprintf(buf, sizeof(buf),
            "[Warning][Clock.Recentre] fromLead=%d toLead=%d owed=%d target=%u rttMs=%.3f samples=%u cause=%s%s%s",
            toLead + owed,
            toLead,
            owed,
            targetTick,
            m_estimator.getSmoothedRTT() * 1000.0,
            m_estimator.getMeasuredRTTSampleCount() - m_recentreBaseSamples,
            cause,
            rearm,
            owed != 0 ? "" : " skipped=within1");
        m_logger(buf);
    }
}

// ---------------------------------------------------------------------------
// Resync callbacks
// ---------------------------------------------------------------------------

unsigned int ClientPredictionClock::registerResyncCallback(ResyncCallback cb)
{
    OG_CHECK(m_nextResyncCallbackId != InvalidCallbackId,
        "ClientPredictionClock: resync callback ids exhausted");
    const unsigned int id = m_nextResyncCallbackId++;
    m_resyncCallbacks.push_back(RegisteredResyncCallback{id, std::move(cb)});
    return id;
}

void ClientPredictionClock::unregisterResyncCallback(unsigned int id)
{
    // Erase, not swap-with-back: the survivors keep their ids and their firing order.
    const auto it = std::find_if(m_resyncCallbacks.begin(), m_resyncCallbacks.end(),
        [id](const RegisteredResyncCallback& registered) { return registered.id == id; });
    if (it != m_resyncCallbacks.end())
        m_resyncCallbacks.erase(it);
}

void ClientPredictionClock::fireResyncCallbacks(unsigned int newTick)
{
    for (auto& registered : m_resyncCallbacks)
        registered.callback(newTick);
}

OGSIM_OPTIMIZE_ON
// pragma optimize on.
