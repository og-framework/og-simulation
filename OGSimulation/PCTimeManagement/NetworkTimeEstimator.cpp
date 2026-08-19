// SPDX-License-Identifier: MPL-2.0
#include "NetworkTimeEstimator.h"

#include <algorithm>
#include <cstdio>

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

NetworkTimeEstimator::NetworkTimeEstimator(const TimeConfig& config, LoggerFn logger)
    : m_config(config)
    , m_smoothedRTT(0.0)
    , m_smoothedJitter(0.0)
    , m_hasFirstSample(false)
    , m_logger(std::move(logger))
{
}

void NetworkTimeEstimator::updateRTT(double rawRTTSeconds)
{
    // -----------------------------------------------------------------------
    // VALIDITY GATE (og-netcode-v2-input-relay T21).
    //
    // THE DEFECT THIS CLOSES — it was LIVE, not hypothetical. The UE binding
    // reads the client's RTT with `FNetPing::GetPingValues(...).Current`, which
    // the engine documents as **-1.0 when the ping type is disabled or has zero
    // samples** (NetPing.h: "The current/latest ping value, or -1.0 if not
    // set"). That value reached here UNGUARDED, and because the branch below
    // latches the FIRST sample verbatim, a single pre-warm-up OnRep would set
    // m_smoothedRTT = -1.0 and m_hasFirstSample = true.
    //
    // TWO CONSEQUENCES, THE SECOND MUCH WORSE THAN THE FIRST.
    //
    //   (1) MASKED. The negative reaches getPredictionOffsetTicks, whose
    //       `ceiled > 0.0` test clamps it to 0, whereupon
    //       `std::max(computed, predOffsetFloorTicks)` substitutes the floor.
    //       The estimator reports a plausible-looking offset while running on
    //       NOTHING — the floor is what makes the failure invisible.
    //
    //   (2) POISONED, AND FOR A LONG TIME. The latch is not merely a lost
    //       sample; it seeds the EMA at -1.0 s. The first REAL sample then
    //       computes its jitter delta against that fiction — for a 30 ms LAN
    //       reading, |0.03 - (-1.0)| = 1.03 s of "jitter" — and with
    //       jitterSmoothingAlpha = 0.15 that lands ~154 ms of phantom jitter in
    //       m_smoothedJitter, which jitterMultiplier = 2 then DOUBLES into the
    //       offset. Both EMAs need tens of samples to relax. So one sentinel
    //       reading at session start produces exactly the inflated-offset
    //       transient this initiative spent T21 chasing — from a bug, not from
    //       the network. The regression cases in NetworkTimeEstimatorTest.cpp
    //       pin both halves.
    //
    // That is precisely why the rejection below is LOUD rather than a quiet
    // `return`.
    //
    // REJECT, DO NOT CLAMP. Clamping a -1.0 to 0.0 would be worse than useless:
    // it would latch m_hasFirstSample against a reading that never happened and
    // then drag the EMA toward zero from a fabricated baseline. A sample the
    // engine says does not exist must not participate at all — so this returns
    // WITHOUT touching m_smoothedRTT, m_smoothedJitter or m_hasFirstSample.
    //
    // WHAT THE CALLER SEES INSTEAD, STATED EXACTLY. The estimator stays
    // `hasFirstRTTSample() == false`, and getPredictionOffsetTicks then returns
    // predOffsetFloorTicks — see the RULING recorded at that function. T21
    // originally left this path returning 0 and escalated the question; the
    // architect ruled in T26a that the floor is a STRUCTURAL INVARIANT rather
    // than an estimate, so it must hold on the no-sample path too. The honesty
    // requirement that motivated the 0 is served by THIS warning, not by making
    // the offset wrong.
    //
    // `!(x >= 0.0)` rather than `x < 0.0` so NaN — which compares false against
    // everything — is rejected too; the isfinite test then also catches +inf.
    //
    // CALLER CONTRACT IS UNCHANGED. `updateRTT` remains total: callers may pass
    // the engine sentinel straight through and need no guard of their own. The
    // UE read sites still log their own one-shot diagnostic naming the specific
    // cause (no FNetPing, ping type not enabled, accumulator empty), which this
    // layer cannot know.
    // -----------------------------------------------------------------------
    if (!(rawRTTSeconds >= 0.0) || !std::isfinite(rawRTTSeconds))
    {
        ++m_rejectedRTTSamples;

        // ONE-SHOT. OnRep_Buffer fires at the timing relay's replication rate
        // (100 Hz), so an unpopulated accumulator would otherwise emit hundreds
        // of lines a second. The running total stays available through
        // getRejectedRTTSampleCount() for anyone who needs the magnitude.
        if (!m_rejectedRTTLogged && m_logger)
        {
            m_rejectedRTTLogged = true;
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                "[Warning][RttSample] NetworkTimeEstimator REJECTED an invalid RTT sample "
                "(raw=%.6f s) - the engine reports NO usable ping reading. hasFirstRTTSample=%d. "
                "While that is 0 the prediction offset is NOT tracking the network - it reports "
                "the structural floor (predOffsetFloorTicks=%u) and nothing else. THIS LINE is the "
                "only signal that the offset is unmeasured; do not read the offset value as evidence. "
                "One-shot; running total via getRejectedRTTSampleCount().",
                rawRTTSeconds,
                m_hasFirstSample ? 1 : 0,
                static_cast<unsigned int>(m_config.predOffsetFloorTicks));
            m_logger(buf);
        }
        return;
    }

    // -----------------------------------------------------------------------
    // PLAUSIBILITY GATE (og-netcode-v2-input-relay T26b).
    //
    // The sample is well-formed — the engine says it is a real reading. This
    // second gate asks a different question: is it BELIEVABLE?
    //
    // WHY IT HAS TO EXIST. UE computes RTT as
    // `CurrentTime - OutLagTime[Index]`, where `CurrentTime` is
    // `FApp::GetCurrentTime()` — FRAME-START time. When a frame hitches, ack
    // processing is delayed by up to the hitch duration and that delay lands
    // DIRECTLY in the measurement. On a loopback session with 5-10 ms of
    // emulated lag this produces readings near ONE SECOND: a hundredfold error
    // caused by the local game thread, not by the network. Pre-T26b updateRTT
    // believed them, jitterMultiplier = 2 doubled the excursion into the offset,
    // and jitterSmoothingAlpha = 0.15 dragged recovery out over seconds — the
    // ~8 s post-hitch offset transient this initiative kept measuring. This gate
    // is the cheap half of the answer (the causal fix, moving the timestamp off
    // frame-start via net.PingUsePacketRecvTime, is backlog T25 and costs a
    // receive thread).
    //
    // THE BOUND. `raw <= rttOutlierMultiplier * smoothedRTT + rttOutlierMarginSeconds`,
    // with the absolute `rttOutlierColdStartCeilingSeconds` standing in on the
    // very first sample, where there is no estimate to compare against. The
    // additive margin is not decoration: on LAN the multiplicative term alone
    // collapses (4 x 0.5 ms = 2 ms would reject an ordinary 10 ms reading).
    //
    // ONE-SIDED, DELIBERATELY. Only samples that are too HIGH are screened.
    // A hitch can only ever inflate the measurement — it delays the ack, it
    // cannot deliver it early — so there is no low-side artifact to filter. More
    // importantly, screening the low side would block RECOVERY: after a genuine
    // congestion episode the estimate must be free to fall as fast as the real
    // readings do.
    //
    // REJECT, NOT CLAMP — the same doctrine as the T21 gate above, for the same
    // reason. A clamped sample is a fabricated number that still moves both EMAs
    // and still injects a jitter delta; the whole point is that we do not know
    // what the RTT was during the hitch, only that it was not this.
    //
    // THE ESCAPE HATCH IS THE PART THAT MATTERS. A filter that never lets the
    // estimate move is not a filter, it is a bug: a genuine step change in
    // network conditions (a route flap, a handover to cellular) looks exactly
    // like an outlier on its first sample. The discriminator is PERSISTENCE. A
    // hitch is isolated — the very next sample is normal and resets the run — so
    // it never reaches `rttOutlierConsecutiveLimit`. A real step is sustained and
    // always does, at which point the estimator concludes the level has genuinely
    // moved and RE-SEEDS from the offending sample exactly as it would from a
    // first sample. Re-seed rather than blend: a step change makes the old
    // estimate wrong, not stale, and re-seeding also zeroes the jitter EMA
    // instead of recording the step itself as ~1 s of phantom jitter.
    //
    // WHAT THIS COSTS, STATED PLAINLY. The gate cannot distinguish a hitch
    // artifact from a burst of real jitter larger than 4x the current estimate,
    // and it suppresses the latter. That is accepted: an excursion that large is
    // already beyond what `jitterMultiplier * smoothedJitter` was sized to cover,
    // and believing hitch readings demonstrably costs more. It is also why every
    // rejection is COUNTED and summarised per window rather than dropped
    // silently — a window full of rejections with no escape is the signature of
    // exactly this ambiguity, and it is the operator's cue to widen the bound.
    //
    // DEGENERATE BY CONSTRUCTION: with no implausible sample, nothing below runs
    // except the two `m_consecutiveOutliers = 0` / window-count statements, and
    // the EMA arithmetic is bit-for-bit what it was pre-T26b.
    // -----------------------------------------------------------------------
    if (!isPlausibleRTTSample(rawRTTSeconds))
    {
        ++m_consecutiveOutliers;

        // A limit of 0 or 1 means "escape on the first implausible sample", i.e.
        // the filter is off — the documented degenerate setting for A/B runs.
        const unsigned int escapeAfter = m_config.rttOutlierConsecutiveLimit > 1u
                                           ? m_config.rttOutlierConsecutiveLimit
                                           : 1u;

        if (m_consecutiveOutliers >= escapeAfter)
        {
            m_smoothedRTT         = rawRTTSeconds;
            m_smoothedJitter      = 0.0;
            m_hasFirstSample      = true;
            m_consecutiveOutliers = 0;
            ++m_outlierEscapes;
            ++m_outlierWindowEscapes;
        }
        else
        {
            ++m_outlierRejectedSamples;
            ++m_outlierWindowRejects;
        }

        noteOutlierWindowSample();
        return;
    }

    m_consecutiveOutliers = 0;

    if (!m_hasFirstSample)
    {
        m_smoothedRTT    = rawRTTSeconds;
        m_smoothedJitter = 0.0;
        m_hasFirstSample = true;
    }
    else
    {
        const double alpha  = m_config.rttSmoothingAlpha;
        const double alphaJ = m_config.jitterSmoothingAlpha;

        const double delta  = rawRTTSeconds >= m_smoothedRTT
                                ? rawRTTSeconds - m_smoothedRTT
                                : m_smoothedRTT - rawRTTSeconds;

        m_smoothedRTT    = alpha  * rawRTTSeconds + (1.0 - alpha)  * m_smoothedRTT;
        m_smoothedJitter = alphaJ * delta          + (1.0 - alphaJ) * m_smoothedJitter;
    }

    noteOutlierWindowSample();
}

// COLD START IS THE SUBTLE CASE and the rule is recorded here rather than left
// implicit. The first sample is latched VERBATIM into m_smoothedRTT, so a
// relative bound has nothing to measure it against. The choice made — and the
// alternative rejected — is:
//
//   CHOSEN: an ABSOLUTE CEILING on the seed (rttOutlierColdStartCeilingSeconds).
//   REJECTED: accept-then-correct, i.e. seed with whatever arrives and let the
//   EMA walk it back.
//
// Accept-then-correct is wrong here for the reason T21 established with
// evidence: the seed is the single most consequential value in this class. It is
// not smoothed, and every subsequent jitter delta is measured against it, so a
// bad seed produces phantom jitter that jitterMultiplier doubles into the offset
// and both EMAs then need tens of samples to shed. A session-start hitch is
// precisely when a bad seed is most likely.
//
// The ceiling does NOT lock out a genuinely slow link: a sustained run of
// readings above it trips the escape hatch and re-seeds. The cost of the ceiling
// on such a link is `rttOutlierConsecutiveLimit` samples — a fraction of a
// second — and the benefit is that a hitch-inflated first reading never becomes
// the baseline.
bool NetworkTimeEstimator::isPlausibleRTTSample(double rawRTTSeconds) const
{
    if (!m_hasFirstSample)
        return rawRTTSeconds <= m_config.rttOutlierColdStartCeilingSeconds;

    return rawRTTSeconds <= m_config.rttOutlierMultiplier * m_smoothedRTT
                            + m_config.rttOutlierMarginSeconds;
}

// PER-WINDOW REPORTING. Rejections must not be silent: a silent reject hides a
// genuine RTT step change exactly as well as it hides a hitch artifact, and that
// ambiguity is this gate's main risk. But the T21 one-shot pattern is wrong for
// this one — a single line at session start would say nothing about a hitch that
// happens forty seconds in, and the whole point is to be able to place
// rejections against the measurement windows a PIE trace is bucketed into.
//
// So: one summary line per WINDOW, emitted only when the window actually
// contained something, carrying rejects, escapes and the surviving estimate.
// That bounds volume at ~1 line / 6 s at the 100 Hz timing relay even under a
// continuously misbehaving ping source, while keeping the per-window resolution
// the analysis needs. The two totals are separate on purpose: rejects WITHOUT
// escapes means transients were filtered; escapes mean the network genuinely
// moved and the estimator followed it.
void NetworkTimeEstimator::noteOutlierWindowSample()
{
    ++m_outlierWindowSamples;

    const unsigned int window = m_config.rttOutlierLogWindowSamples;
    if (window == 0u || m_outlierWindowSamples < window)
        return;

    if ((m_outlierWindowRejects > 0u || m_outlierWindowEscapes > 0u) && m_logger)
    {
        char buf[448];
        std::snprintf(buf, sizeof(buf),
            "[Warning][RttSample.Outlier] NetworkTimeEstimator filtered %u implausible RTT "
            "sample(s) and force-re-seeded %u time(s) over the last %u samples. "
            "smoothedRTT=%.3fms smoothedJitter=%.3fms. Rejects WITHOUT re-seeds = transient "
            "(frame-hitch artifacts, filtered as intended); re-seeds = the network genuinely "
            "stepped and the estimate followed. Running totals via "
            "getOutlierRejectedSampleCount() / getOutlierEscapeCount().",
            m_outlierWindowRejects,
            m_outlierWindowEscapes,
            m_outlierWindowSamples,
            m_smoothedRTT    * 1000.0,
            m_smoothedJitter * 1000.0);
        m_logger(buf);
    }

    m_outlierWindowSamples = 0;
    m_outlierWindowRejects = 0;
    m_outlierWindowEscapes = 0;
}

void NetworkTimeEstimator::recordAuthorityTick(unsigned int serverTick)
{
    m_authorityTick.store(serverTick, std::memory_order_relaxed);

    if (m_logger)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[Verbose] PCTM NTE: authorityTick=%u smoothedRTT=%.3fms smoothedJitter=%.3fms targetPredTick=%u",
            m_authorityTick.load(std::memory_order_relaxed),
            m_smoothedRTT    * 1000.0,
            m_smoothedJitter * 1000.0,
            getTargetPredictionTick());
        m_logger(buf);
    }
}

unsigned int NetworkTimeEstimator::getPredictionOffsetTicks() const
{
    // -----------------------------------------------------------------------
    // THE NO-SAMPLE PATH RETURNS THE FLOOR, NOT 0.
    //
    // ARCHITECT'S RULING, og-netcode-v2-input-relay T26a, resolving the
    // behaviour change T21 escalated. IT IS WRITTEN OUT HERE BECAUSE THE NEXT
    // READER WILL OTHERWISE "SIMPLIFY" IT BACK — returning 0 from a function
    // whose estimate does not exist looks obviously right, and is not.
    //
    // (1) predOffsetFloorTicks IS NOT AN ESTIMATE. It is a STRUCTURAL INVARIANT
    //     GUARD. Its derivation (netcode_finding_pred_offset_floor.md, and the
    //     field comment in TimeConfig.h) records the failure it exists to
    //     prevent: without it the softDrift dead band lets the client sit stably
    //     0..softDrift ticks BEHIND the server's live tick, discarding EVERY
    //     correction until HardResync teleports the body. The floor asserts
    //     "the client predicts forward" — a claim that is true whether or not an
    //     RTT estimate exists yet.
    //
    // (2) RETURNING 0 RE-CREATES THAT PATHOLOGY IN EXACTLY THE CASE THE T21 GATE
    //     DETECTS. The timing relay's OnRep delivers authorityTick AND reads the
    //     ping in the SAME handler, so a client can legitimately have a live,
    //     advancing authorityTick while its ping source is still unpopulated.
    //     Then target = authorityTick + 0, pastGuard goes true, and the clock
    //     steers the client to run AT the server tick — perpetually behind,
    //     discarding corrections. The remedy for the silent-zero defect would
    //     have re-introduced the very failure the floor exists to prevent.
    //
    // (3) THE HONESTY ARGUMENT IS ALREADY SATISFIED BY T21'S LOUD LOG. We do not
    //     additionally need the offset to be WRONG in order to know the ping
    //     source is broken. updateRTT above warns by name; hasFirstRTTSample()
    //     answers the question programmatically. T21 shipped the diagnosis half;
    //     this is the behaviour half.
    //
    // (4) IT IS ALSO THE CONSERVATIVE CHOICE. Pre-T21 this path yielded 4 anyway
    //     — the -1.0 sentinel latched, produced a negative rawOffset, clamped to
    //     0, and the floor below substituted. Returning the floor preserves
    //     shipped behaviour while keeping the new diagnostic.
    //
    // (5) TRUE COLD START IS UNAFFECTED EITHER WAY. Before the first OnRep,
    //     authorityTick is 0, so targetTick is below minTicksBeforeDriftCheck,
    //     pastGuard is false, and nothing is steering. The floor only starts
    //     mattering once authority ticks arrive — which is precisely the
    //     harmful case.
    // -----------------------------------------------------------------------
    if (!m_hasFirstSample)
        return static_cast<unsigned int>(m_config.predOffsetFloorTicks);

    const double rawOffset = (m_smoothedRTT + m_config.jitterMultiplier * m_smoothedJitter)
                             * m_config.tickFrequency;

    // ceil rounds up so predictions arrive slightly early rather than late.
    const double ceiled = std::ceil(rawOffset);
    const unsigned int computed = ceiled > 0.0 ? static_cast<unsigned int>(ceiled) : 0u;

    // Floor: keep the client predictively AHEAD of authority even on LAN /
    // near-zero RTT. Without this, on cooked-dedicated + late-connect client,
    // the HardResync sequence deposits the client at exactly authorityTick + 0..1
    // with zero margin ahead, and the softDriftThresholdTicks dead band lets it
    // sit stably 0..softDrift ticks BEHIND server's live tick, discarding every
    // correction. See ../og-brawler-hit-resolution/netcode_finding_pred_offset_floor.md
    // for the full derivation. The floor is a no-op on WAN because rawOffset already
    // exceeds predOffsetFloorTicks at any real RTT + jitter.
    return std::max(computed, static_cast<unsigned int>(m_config.predOffsetFloorTicks));
}

unsigned int NetworkTimeEstimator::getTargetPredictionTick() const
{
    return m_authorityTick.load(std::memory_order_relaxed) + getPredictionOffsetTicks();
}

unsigned int NetworkTimeEstimator::getLastAuthorityTick() const
{
    return m_authorityTick.load(std::memory_order_relaxed);
}

double NetworkTimeEstimator::getSmoothedRTT() const
{
    return m_smoothedRTT;
}

double NetworkTimeEstimator::getSmoothedJitter() const
{
    return m_smoothedJitter;
}

bool NetworkTimeEstimator::hasFirstRTTSample() const
{
    return m_hasFirstSample;
}

unsigned int NetworkTimeEstimator::getRejectedRTTSampleCount() const
{
    return m_rejectedRTTSamples;
}

unsigned int NetworkTimeEstimator::getOutlierRejectedSampleCount() const
{
    return m_outlierRejectedSamples;
}

unsigned int NetworkTimeEstimator::getOutlierEscapeCount() const
{
    return m_outlierEscapes;
}

OGSIM_OPTIMIZE_ON
// pragma optimize on.
