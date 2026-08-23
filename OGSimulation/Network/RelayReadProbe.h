#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// ---------------------------------------------------------------------------
// RelayReadProbe / RelayArrivalProbe / FrameHealthProbe -- the CLIENT-side relay
// telemetry, plus the role-neutral frame-health probe. Companion:
// Network/RelayWritePathProbe.h (SERVER-side write and budget probes).
// Tests: Network/RelayReadProbeTest.cpp.
// Every `§N` mark below resolves in docs/RelayProbes-rationale.md.
//
// ---------------------------------------------------------------------------
// ORIENTATION -- THREE OBJECTS BECAUSE THERE ARE TWO THREADS.
//
//   object             thread   fed from                     window closes on
//   -----------------  -------  ---------------------------  ------------------
//   RelayReadProbe     PHYSICS  InputResolutionTelemetry's    the PREDICTION
//     PROBES 1, B, 3            two scheduled-read emitters,  TICK advancing
//                               off SimulationInputResolution past the window
//                               collectInputAll / collectResimInputAll
//   RelayArrivalProbe  GAME     NetSyncTelemetry::            120 GAP SAMPLES
//     PROBE 2                   emitRelayArrival, off the     (the game thread
//                               replicated-ring arrival       has no sim tick)
//                               callback registerPredictionOwner binds
//   FrameHealthProbe   GAME     the adapter's per-frame hook, 120 SAMPLES
//     PROBE 4                   on EITHER role
//
//   PROBE 1 which rung of the scheduled-read ladder replied; PROBE B why a miss
//   missed; PROBE 3 how long one character was served stale; PROBE 2 how often
//   the ring arrives, in CAPTURE ticks; PROBE 4 sim ticks per frame.
//   §2 §3 §5 §6 §9
//
// ⛔ THREE OBJECTS, THREE WINDOWS, AND THAT IS A CORRECTNESS PROPERTY: one shared window would have a thread reset counters the other is mid-increment on, costing a whole window. §13
// ⛔ NEITHER THREAD TOUCHES THE OTHER'S OBJECT, so no atomics and no seam are needed. That is the whole reason for the split. §13
// ⛔ INSTRUMENTATION ONLY -- counters plus a windowed summary, every consumer a log call; removing this header changes no simulated value. §13
// ⛔ NEITHER OBJECT LOGS: each hands back a summary struct and the caller does the SIMLOG, which lets the tests assert on numbers rather than strings. §13
// ⛔ ENGINE-AGNOSTIC, STL ONLY -- no UE types, no OGTypes, no other OGSimulation header, so the file is testable without a simulatable or a logger. §13
// ⚠ Global namespace, matching the rest of the OGSim core.
//
// ---------------------------------------------------------------------------
// ONE ADAPTER'S BINDING FOR EVERY ENGINE NAME BELOW. The body names ROLES ONLY:
// the physics engine, the replication system, the replicated-ring arrival
// callback, the server frame-rate cap, the global game-thread frame counter. For
// one adapter (Unreal/Chaos) those are Chaos, Iris, the relay actor's OnRep
// `ASimulationInputRelay::OnRep_RelayedInputRing`, the `NetServerMaxTickRate`
// setting and `GFrameCounter`; there the missing send-success signal is
// `FReplicationWriter::HandleDroppedRecord`, which recovers the changemask and
// not the values. Another adapter substitutes its own. §15
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PROBE 1 -- the four outcomes of the scheduled read.
//
//   NoProbe     rung 0, `!findLatest().valid`: nothing has EVER arrived here.
//               The pre-registration / join window.
//   Hit         the probe at `tick - dLatest` found a candidate whose own stamp
//               equals `dLatest` -- the proxy is consuming the server's schedule.
//   Miss        nothing resident at the scheduled capture tick. STARVATION. Also
//               covers the `tick < dA` early-session underflow.
//   VerifyFail  a candidate WAS resident, its stamp differs: the delay REGIME
//               shifted under the reader. TRANSITION, not starvation.
//
// ⛔ FOUR, NOT TWO: the ladder serves `fallback()` from three situations that behave identically and mean different things. §2
// ⛔ NoProbe IS NOT STARVATION -- counting it as one makes the join window look like a fault on every join. §2
// ⛔ Miss AND VerifyFail MUST NEVER BE MERGED: all-Miss says the wire is starving the proxy, all-VerifyFail says the wire is healthy and the delay is thrashing. §2
// ---------------------------------------------------------------------------
enum class ScheduledRelayedReadOutcome : std::uint8_t
{
    NoProbe = 0,
    Hit,
    Miss,
    VerifyFail,
};

// ---------------------------------------------------------------------------
// PROBE B -- WHY the miss happened. THE ENUM ABOVE IS DELIBERATELY UNCHANGED.
//
//   InSpan       probeTick lies between the store's oldest and newest resident
//                capture ticks and is ABSENT. A COVERAGE HOLE.
//   AboveNewest  probeTick is NEWER than anything the store holds.
//   BelowOldest  probeTick is OLDER than the store retains: clock misalignment,
//                or its 64-tick capacity outrun.
//   NoProbeTick  no probe tick could be formed (the `tick < dA` underflow guard:
//                a session younger than the delay).
//
// ⛔ THREE WHYS, THREE DIFFERENT REMEDIES, and picking the wrong one is the failure this enum prevents; only InSpan moves with ring retention depth. §3
// ⛔ DEPTH IS IRRELEVANT TO AboveNewest -- no redundancy delivers a capture that does not exist yet; that is the design's own deficit condition failing. §3
// ⛔ NoProbeTick IS KEPT SEPARATE from BelowOldest, which it resembles: folding an early-session artefact into a clock-misalignment bucket destroys the discrimination. §3
// ⛔ A SECOND ENUM RATHER THAN SIX OUTCOMES: `outcome` answers which rung of the LADDER replied, `missClass` where the receiver asked relative to what the STORE holds. §3
// ---------------------------------------------------------------------------
enum class ScheduledRelayedReadMissClass : std::uint8_t
{
    NotAMiss = 0,
    InSpan,
    AboveNewest,
    BelowOldest,
    NoProbeTick,
};

// True iff this outcome served `fallback()` from a store that HAS data -- the D4
// stale-hold situation PROBE 3 measures. §5
// ⛔ DELIBERATELY EXCLUDES NoProbe (see `RelayReadProbe::notePredictionRead`) and, obviously, Hit. §5
inline bool isStaleFallbackOutcome(ScheduledRelayedReadOutcome outcome)
{
    return outcome == ScheduledRelayedReadOutcome::Miss
        || outcome == ScheduledRelayedReadOutcome::VerifyFail;
}

// What `resolveScheduledRelayedInput` reports back to its CALLER, so the caller
// can count and log without the ladder itself gaining any state. §2
// ⚠ EVERY FIELD BUT `outcome` IS DIAGNOSTIC DETAIL, and several are meaningless on the outcomes that never computed them -- the field comments say which. §2
struct ScheduledRelayedReadReport
{
    ScheduledRelayedReadOutcome outcome = ScheduledRelayedReadOutcome::NoProbe;
    // The capture tick the ladder probed, `tick - dLatest`. Meaningless on NoProbe
    // and on the `tick < dA` underflow guard.
    std::uint32_t probeTick = 0u;
    // The stamp the verify step compared AGAINST, `findLatest().dA`. Meaningless
    // on NoProbe.
    std::uint8_t dLatest = 0u;
    // The resident candidate's OWN stamp; meaningful on Hit and VerifyFail only.
    // On VerifyFail it is the value that differed from `dLatest`, which is the
    // single most useful number in a delay-transition trace.
    std::uint8_t candidateDA = 0u;

    // --- [T20] PROBE B ------------------------------------------------------

    // Why the miss happened. `NotAMiss` on every non-Miss outcome.
    ScheduledRelayedReadMissClass missClass = ScheduledRelayedReadMissClass::NotAMiss;

    // The SIGNED DISTANCE `probeTick - newestResident`. The three miss classes are
    // buckets over exactly this quantity, so the distribution separates them
    // CONTINUOUSLY: deltas at +2 are a receiver reading ahead of what it was sent
    // (no depth helps); at -3 with misses, one reading inside a holed span (depth
    // does). §4
    // ⛔ SET ON HIT AND VerifyFail TOO -- how far behind the newest a HIT sits is the calibration the miss deltas are read against, and it costs nothing. §4
    bool         deltaToNewestValid = false;
    std::int32_t deltaToNewest      = 0;

    // The store's resident span at the moment of the read, filled ONLY on a miss:
    // the classification needs it and nothing else does, so `spanValid` is false on
    // Hit / VerifyFail / NoProbe even though a span exists there too. §3
    bool          spanValid      = false;
    std::uint32_t oldestResident = 0u;
    std::uint32_t newestResident = 0u;
    std::uint32_t residentCount  = 0u;
};

// ---------------------------------------------------------------------------
// The signed-delta histogram -- `probeTick - newestResident`, per call site.
// Exact for |delta| <= kRelayDeltaHistogramRange with one saturating bucket at
// each end; the exact min and max are tracked alongside. §4
//
// ⛔ A SATURATED PERCENTILE IS ALWAYS ACCOMPANIED BY A REAL NUMBER -- the same contract RelayArrivalProbe's gap histogram carries. §13
// ⛔ THE RANGE IS THE STORE'S CAPACITY ON PURPOSE: past +/-64 ticks the receiver is asking outside anything the store could hold, so the exact value adds nothing. §4
// ---------------------------------------------------------------------------
inline constexpr std::int32_t kRelayDeltaHistogramRange = 64;

struct RelayDeltaSummary
{
    std::uint32_t samples = 0u;

    // ⛔ p10 AND p90 RATHER THAN A MEAN, deliberately: the distribution is bimodal -- hits just below zero, above-newest misses above it -- so a mean names a value that never occurs. §4
    std::int32_t p10 = 0;
    std::int32_t p50 = 0;
    std::int32_t p90 = 0;

    // EXACT, never saturated.
    std::int32_t minDelta = 0;
    std::int32_t maxDelta = 0;

    // Samples that fell outside the exactly-tracked range in each direction.
    std::uint32_t saturatedLow  = 0u;
    std::uint32_t saturatedHigh = 0u;
};

class RelaySignedDeltaHistogram
{
public:
    // Bucket 0 absorbs everything below -range, bucket kBucketCount-1 everything
    // above +range; the middle 2*range+1 buckets are exact.
    static constexpr std::size_t kBucketCount =
        static_cast<std::size_t>(2 * kRelayDeltaHistogramRange + 3);

    void note(std::int32_t delta)
    {
        if (m_samples == 0u)
        {
            m_min = delta;
            m_max = delta;
        }
        else
        {
            if (delta < m_min) { m_min = delta; }
            if (delta > m_max) { m_max = delta; }
        }

        if (delta < -kRelayDeltaHistogramRange)
        {
            ++m_buckets[0];
            ++m_saturatedLow;
        }
        else if (delta > kRelayDeltaHistogramRange)
        {
            ++m_buckets[kBucketCount - 1u];
            ++m_saturatedHigh;
        }
        else
        {
            ++m_buckets[static_cast<std::size_t>(delta + kRelayDeltaHistogramRange) + 1u];
        }

        ++m_samples;
    }

    void reset()
    {
        m_buckets.fill(0u);
        m_samples       = 0u;
        m_min           = 0;
        m_max           = 0;
        m_saturatedLow  = 0u;
        m_saturatedHigh = 0u;
    }

    std::uint32_t sampleCount() const { return m_samples; }

    void fillSummary(RelayDeltaSummary& out) const
    {
        out.samples       = m_samples;
        out.p10           = percentile(10u);
        out.p50           = percentile(50u);
        out.p90           = percentile(90u);
        out.minDelta      = m_min;
        out.maxDelta      = m_max;
        out.saturatedLow  = m_saturatedLow;
        out.saturatedHigh = m_saturatedHigh;
    }

private:
    // ⛔ THE TWO SATURATING BUCKETS REPORT THE FIRST VALUE OUTSIDE THE EXACT RANGE -- a floor/ceiling on the truth, never a claim of precision they do not have. §4
    static std::int32_t valueForBucket(std::size_t bucket)
    {
        if (bucket == 0u)
        {
            return -kRelayDeltaHistogramRange - 1;
        }
        if (bucket == kBucketCount - 1u)
        {
            return kRelayDeltaHistogramRange + 1;
        }
        return static_cast<std::int32_t>(bucket) - 1 - kRelayDeltaHistogramRange;
    }

    // ⛔ NEAREST-RANK in ASCENDING delta order, the same definition RelayArrivalProbe::percentile uses. §13
    std::int32_t percentile(std::uint32_t numeratorPercent) const
    {
        if (m_samples == 0u)
        {
            return 0;
        }

        const std::uint64_t scaled = static_cast<std::uint64_t>(numeratorPercent)
                                   * static_cast<std::uint64_t>(m_samples);
        const std::uint64_t rank = (scaled + 99u) / 100u;

        std::uint64_t cumulative = 0u;
        for (std::size_t bucket = 0u; bucket < kBucketCount; ++bucket)
        {
            cumulative += m_buckets[bucket];
            if (cumulative >= rank)
            {
                return valueForBucket(bucket);
            }
        }
        return m_max;       // unreachable while rank <= m_samples
    }

    std::array<std::uint32_t, kBucketCount> m_buckets{};
    std::uint32_t m_samples       = 0u;
    std::int32_t  m_min           = 0;
    std::int32_t  m_max           = 0;
    std::uint32_t m_saturatedLow  = 0u;
    std::uint32_t m_saturatedHigh = 0u;
};

// Per-window outcome tallies for ONE call site.
struct RelayReadCounters
{
    std::uint32_t noProbe    = 0u;
    std::uint32_t hit        = 0u;
    // ⛔ THE MISS TOTAL, UNCHANGED IN MEANING: the four sub-counters below partition it and sum to it exactly; `miss` stays the total so earlier numbers remain comparable. §3
    std::uint32_t miss       = 0u;
    std::uint32_t verifyFail = 0u;

    // The miss partition -- see ScheduledRelayedReadMissClass.
    std::uint32_t missInSpan      = 0u;
    std::uint32_t missAboveNewest = 0u;
    std::uint32_t missBelowOldest = 0u;
    std::uint32_t missNoProbeTick = 0u;

    // `probeTick - newestResident` over every read that formed a probe tick. §4
    RelaySignedDeltaHistogram delta;

    std::uint32_t total() const
    {
        return noProbe + hit + miss + verifyFail;
    }

    // Every miss is classified, so this must equal `miss`. Exposed so a test can
    // assert the partition rather than trusting it.
    std::uint32_t missClassTotal() const
    {
        return missInSpan + missAboveNewest + missBelowOldest + missNoProbeTick;
    }

    void note(ScheduledRelayedReadOutcome outcome)
    {
        switch (outcome)
        {
        case ScheduledRelayedReadOutcome::NoProbe:    ++noProbe;    break;
        case ScheduledRelayedReadOutcome::Hit:        ++hit;        break;
        case ScheduledRelayedReadOutcome::Miss:       ++miss;       break;
        case ScheduledRelayedReadOutcome::VerifyFail: ++verifyFail; break;
        }
    }

    // ⛔ AN OVERLOAD, NOT A REPLACEMENT, so every earlier call site and test compiles and counts identically -- the added fields are pure refinement of the four totals. §3
    void note(const ScheduledRelayedReadReport& report)
    {
        note(report.outcome);

        switch (report.missClass)
        {
        case ScheduledRelayedReadMissClass::InSpan:      ++missInSpan;      break;
        case ScheduledRelayedReadMissClass::AboveNewest: ++missAboveNewest; break;
        case ScheduledRelayedReadMissClass::BelowOldest: ++missBelowOldest; break;
        case ScheduledRelayedReadMissClass::NoProbeTick: ++missNoProbeTick; break;
        case ScheduledRelayedReadMissClass::NotAMiss:                       break;
        }

        if (report.deltaToNewestValid)
        {
            delta.note(report.deltaToNewest);
        }
    }

    void reset()
    {
        *this = RelayReadCounters{};
    }
};

struct RelayReadWindowSummary
{
    // ⛔ THE TWO CALL SITES ARE COUNTED SEPARATELY, NEVER SUMMED: both run the SAME ladder over the SAME store, so a hit-rate divergence is a real frontier signal nothing else reports. §5
    RelayReadCounters prediction;
    RelayReadCounters resim;

    // PROBE 3 -- the D4 stale window: the longest consecutive run of
    // fallback-serving reads on ONE character in this window, and whose it was.
    // This sets `K` for the deferred stale-hold rule from data instead of a guess. §5
    std::uint32_t maxConsecutiveFallbackRun = 0u;
    unsigned int  maxConsecutiveFallbackId  = 0u;

    std::uint32_t windowStartTick = 0u;
    std::uint32_t windowEndTick   = 0u;
};

// ~2 s at 60 Hz, matching the cadence ServerReceptionCoordinator::maybeEmitInputStats
// already emits `[InputStats]` on, so an operator sees the two channels tick at a
// comparable rate. §13
// ⛔ DELIBERATELY NOT DERIVED FROM TimeConfig: none is held here, and threading one in for a log cadence buys a real dependency for a cosmetic gain. §13
inline constexpr std::uint32_t kRelayReadProbeWindowTicks = 120u;

// ---------------------------------------------------------------------------
// RelayReadProbe -- PHYSICS THREAD ONLY. Probes 1, B and 3.
// ---------------------------------------------------------------------------
class RelayReadProbe
{
public:
    explicit RelayReadProbe(std::uint32_t windowTicks = kRelayReadProbeWindowTicks)
        : m_windowTicks(windowTicks == 0u ? kRelayReadProbeWindowTicks : windowTicks)
    {
    }

    // A scheduled read on the PREDICTION path. Takes the whole report, so the miss
    // class and the signed delta are tallied alongside the outcome. §5
    //
    // ⛔ THE ONLY CALL SITE THAT FEEDS THE STALE RUN: the run is consecutive ticks on a MONOTONIC per-tick stream, and resim replays ticks out of order and repeatedly. §5
    // ⛔ RUNG 0 IS EXCLUDED FROM THE RUN -- it is the join window, not staleness, and counting it would inflate every session's maximum run and bias `K` upward. §5
    // ⛔ IT NEITHER INCREMENTS THE RUN NOR RESETS IT: `findLatest().valid` stays true forever once anything is pushed, so NoProbe can only be a LEADING prefix. §5
    // ⚠ The outcome-only overload below is kept because a caller holding only an outcome must not be forced to fabricate a report. §5
    void notePredictionRead(unsigned int id, const ScheduledRelayedReadReport& report)
    {
        m_prediction.note(report);
        noteRun(id, report.outcome);
    }

    void noteResimRead(const ScheduledRelayedReadReport& report)
    {
        m_resim.note(report);
    }

    void notePredictionRead(unsigned int id, ScheduledRelayedReadOutcome outcome)
    {
        m_prediction.note(outcome);
        noteRun(id, outcome);
    }

    // A scheduled read on the RESIM path. Counted, but it drives neither the window
    // nor the stale run, and takes no id because nothing per-id is derived from it. §5
    void noteResimRead(ScheduledRelayedReadOutcome outcome)
    {
        m_resim.note(outcome);
    }

    // Close the window if `predictionTick` has advanced past it, filling `out` and
    // resetting the counters. §5
    // ⛔ TRUE ONLY WHEN A WINDOW CLOSED AND CARRIED A READ, so an authority -- which allocates no relay stores and reaches neither call site -- never heartbeats a line. §5
    // ⛔ DRIVEN BY THE PREDICTION TICK AND ONLY BY IT: it is the one monotonic per-frame clock either call site has. §5
    // ⚠ A HARD RESYNC CAN MOVE IT BACKWARDS, which is not an error -- the window restarts rather than staying open for the ~4 billion ticks an unsigned subtraction computes. §5
    bool maybeCloseWindow(std::uint32_t predictionTick, RelayReadWindowSummary& out)
    {
        if (!m_windowStarted)
        {
            m_windowStarted   = true;
            m_windowStartTick = predictionTick;
            return false;
        }

        if (predictionTick < m_windowStartTick)
        {
            m_windowStartTick = predictionTick;     // resync jumped us backwards
            return false;
        }

        if (predictionTick - m_windowStartTick < m_windowTicks)
        {
            return false;                           // window still open
        }

        const bool carriedSomething = (m_prediction.total() + m_resim.total()) > 0u;

        out.prediction                = m_prediction;
        out.resim                     = m_resim;
        out.maxConsecutiveFallbackRun = m_windowMaxRun;
        out.maxConsecutiveFallbackId  = m_windowMaxRunId;
        out.windowStartTick           = m_windowStartTick;
        out.windowEndTick             = predictionTick;

        m_prediction.reset();
        m_resim.reset();
        // ⛔ THE PER-ID CURRENT RUNS DELIBERATELY SURVIVE THE WINDOW BOUNDARY -- a starvation straddling two windows is one run, not two. Only the window's MAXIMUM is reset. §5
        m_windowMaxRun    = 0u;
        m_windowMaxRunId  = 0u;
        m_windowStartTick = predictionTick;

        return carriedSomething;
    }

    // ⛔ DROP PER-ID STATE FOR AN UNREGISTERED CHARACTER -- without it the run map grows with every character the session has ever had. §13
    void forgetOwner(unsigned int id)
    {
        m_fallbackRuns.erase(id);
    }

    // --- introspection; tests and diagnostics only -------------------------
    const RelayReadCounters& predictionCounters() const { return m_prediction; }
    const RelayReadCounters& resimCounters()      const { return m_resim; }
    std::uint32_t windowMaxFallbackRun()          const { return m_windowMaxRun; }

    std::uint32_t currentFallbackRun(unsigned int id) const
    {
        const auto it = m_fallbackRuns.find(id);
        return it == m_fallbackRuns.end() ? 0u : it->second;
    }

    std::size_t trackedOwnerCount() const { return m_fallbackRuns.size(); }

private:
    // ⛔ THE STALE-RUN HALF OF notePredictionRead, factored out so the two overloads cannot drift. The rule itself is unchanged -- see the block above notePredictionRead. §5
    void noteRun(unsigned int id, ScheduledRelayedReadOutcome outcome)
    {
        if (outcome == ScheduledRelayedReadOutcome::NoProbe)
        {
            return;             // excluded from the run entirely — see above
        }

        if (!isStaleFallbackOutcome(outcome))
        {
            m_fallbackRuns[id] = 0u;        // a Hit breaks the run
            return;
        }

        const std::uint32_t run = ++m_fallbackRuns[id];
        if (run > m_windowMaxRun)
        {
            m_windowMaxRun   = run;
            m_windowMaxRunId = id;
        }
    }

    std::uint32_t m_windowTicks;

    RelayReadCounters m_prediction;
    RelayReadCounters m_resim;

    // Live consecutive-fallback run per character. Bounded by live ids (forgetOwner).
    std::unordered_map<unsigned int, std::uint32_t> m_fallbackRuns;

    std::uint32_t m_windowMaxRun   = 0u;
    unsigned int  m_windowMaxRunId = 0u;

    bool          m_windowStarted   = false;
    std::uint32_t m_windowStartTick = 0u;
};

// ---------------------------------------------------------------------------
// PROBE 2 -- replication cadence, IN CAPTURE TICKS.
//
// The measurement is the newest `captureTick` in the ring on THIS arrival minus
// the newest on the PREVIOUS one, per component -- same units as `depth`, immune
// to local clock skew, and free: `populateRemoteInputCache` already walks every
// entry. §6
//
// ⛔ CAPTURE TICKS ARE A CORRECTNESS REQUIREMENT, NOT A PREFERENCE: the rule this feeds is `depth >= gap_p99 + margin`, and a gap in LOCAL ticks is not comparable to `depth`. §6
// ⛔ STRUCTURALLY LOCAL-TICK-PROOF: `noteArrival` is given no local tick and this file holds no clock, so a local-tick implementation cannot be written here by accident. §6
// ⛔ A HISTOGRAM, NOT A MEAN: a channel with one 20-tick stall per second has a mean gap near 1 and a p99 near 20, and it is the 20 the depth rule must clear. §6
// ---------------------------------------------------------------------------

// Gaps 1..64 are counted exactly; index 0 is unused (a zero gap is not a sample --
// see noteArrival) and index kGapOverflowBucket absorbs everything larger.
inline constexpr std::uint32_t kRelayArrivalMaxTrackedGap = 64u;
inline constexpr std::size_t   kGapOverflowBucket         = kRelayArrivalMaxTrackedGap + 1u;

// One summary per this many GAP SAMPLES -- sample-driven, not tick-driven, because
// the game thread has no simulation tick. At depth 1 on a healthy wire that is ~2 s
// per component, the same feel as the physics-side window. §6
inline constexpr std::uint32_t kRelayArrivalProbeWindowSamples = 120u;

// ---------------------------------------------------------------------------
// THE DISCONTINUITY GUARD -- the same guard, same reason, that `FrameHealthProbe`
// (`kFrameHealthDiscontinuityTicks`) and `RelayWriteProbe`
// (`kRelayWriteDiscontinuityTicks`) already carry. §7
//
// ⛔ A JUMP LARGER THAN THIS IS ONE CORRELATED EVENT -- a hiccup, a host stall, a relevancy pause, a re-join -- NOT `gap - 1` independently lost inputs. §7
// ⛔ IT MATTERS MORE HERE THAN ON EITHER SIBLING: `lostCaptureTicksX1000` gates acceptance at ~11 per mille, and two outliers in one 120-sample window compute to 358. §7
// ⚠ THE VALUE IS 16 BECAUSE THE ARCHIVES FORCE IT: the observed gap distribution is bimodal with an empty band at 7..17, and anything above 17 fails to fix the defect. §7
// ⚠ It is also exactly 2 * `relayedInputRing::kMaxDepth`, so no flush round can produce it; and at the measured 1.122 % per-tick loss, 16 consecutive losses has probability ~1e-31. §7
// ⛔ A GAP ABOVE THIS IS A SAMPLE OF NOTHING: it re-seeds the watermark and enters no accumulator, no bucket and not `maxGap`; its magnitude survives in `maxDiscontinuityGap`. §7
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kRelayArrivalDiscontinuityTicks = 16u;

struct RelayArrivalWindowSummary
{
    std::uint32_t samples = 0u;

    // All in CAPTURE ticks. p50/p99 are nearest-rank over the histogram.
    std::uint32_t p50    = 0u;
    std::uint32_t p99    = 0u;
    std::uint32_t maxGap = 0u;

    // True when p99 fell in the saturating bucket, i.e. the real p99 is
    // ">= kRelayArrivalMaxTrackedGap". `maxGap` is still exact when this is set.
    bool p99Saturated = false;

    // Arrivals that replicated but advanced no capture tick (a dA re-stamp of an
    // already-resident tick is the realistic cause). §6
    // ⛔ NOT CADENCE SAMPLES -- they delivered no new capture tick, so counting them as a gap of 0 would drag the percentiles down and understate the depth requirement. §6
    std::uint32_t noAdvance = 0u;

    // Samples that landed in the saturating bucket. §7
    // ⚠ STRUCTURALLY UNREACHABLE SINCE THE DISCONTINUITY GUARD, deliberately: the bucket sits at 64 and the guard fires at 16, so this and `p99Saturated` read 0/false forever. §7
    // ⛔ KEPT ANYWAY -- every archived window carries `saturated=`, and deleting the field breaks comparability with the logs that motivated the guard. §7
    // ⛔ AN OPERATOR READING A NEW LOG MUST READ `discontinuities` INSTEAD: `saturated=` never saw the 46/47-tick class at all, which is why it was not enough. §7
    std::uint32_t saturatedSamples = 0u;

    // -----------------------------------------------------------------------
    // THE R = 0 LOSS COUNTER.
    //
    //   lostCaptureTicks       Sum(gap - delivered) over accepted arrivals. EXACT:
    //                          the watermark advanced by `gap`, `delivered` of those
    //                          arrived, the remainder provably did not. Reduces to
    //                          Sum(gap - 1) at depth 1.
    //   deliveredCaptureTicks  Sum(delivered), clamped per sample to `gap`.
    //   expectedCaptureTicks   Sum(gap) -- what the senders PRODUCED over the span.
    //   lostCaptureTicksX1000  per mille of expected; steady state ~11.
    //
    // ⛔ MANDATORY, NOT GARNISH: at R = 0 an input is sent ONCE and the replication system exposes no send-success signal game code can read, so loss is INVISIBLE on the server. §8
    // ⛔ THE COUNTABLE END IS THE CLIENT -- capture ticks are per-character monotonic at ~60/s, so every lost input is a visible ARITHMETIC GAP and nothing else produces one. §8
    // ⛔ THE NUMERATOR IS `gap - delivered`, NEVER `gap - 1`: under flush-on-poll one arrival publishes a whole staged burst, and `gap - 1` charged 122 per mille while BOTH entries arrived. §8
    // ⛔ `deliveredCaptureTicks` MAKES THE WINDOW SELF-CHECKING: `lost + delivered == expected` must hold exactly, or a lossless window reads like one never plumbed through. §8
    // ⛔ `expectedCaptureTicks` IS THE ONLY HONEST DENOMINATOR: same samples, so it survives window edges, a varying character count and an unrelated client frame rate. §8
    // ⛔ SUSTAINED EXCESS OVER THE SERVER'S OWN LOSS RATE MEANS SCHEDULER SKIPS on top of wire loss; report the join-settling window SEPARATELY from steady state. §8
    // ⚠ IT RIDES THE EXISTING WINDOW: a second window over the same samples would be a second clock to keep honest for no new information. §8
    // ⚠ A NO-ADVANCE ARRIVAL IS NOT A SAMPLE and contributes to none of the three terms -- it advanced no capture tick, so it says nothing about loss. §8
    // -----------------------------------------------------------------------
    std::uint32_t lostCaptureTicks      = 0u;
    std::uint32_t deliveredCaptureTicks = 0u;
    std::uint32_t expectedCaptureTicks  = 0u;
    std::uint32_t lostCaptureTicksX1000 = 0u;

    // Gaps this window RE-CLASSIFIED rather than charged, per
    // kRelayArrivalDiscontinuityTicks; `maxDiscontinuityGap` is the largest
    // EXCLUDED gap, exact. §7
    // ⛔ A WINDOW REPORTING discont > 0 IS DISCARDED, NOT AVERAGED IN, exactly as `RelayWriteWindowSummary::discontinuities` is: the span is no longer contiguous. §7
    // ⛔ WITHOUT `maxDiscontinuityGap` THE GUARD WOULD SWALLOW the one number saying how bad the interruption was -- before it existed, `max=` was those windows' only tell. §7
    std::uint32_t discontinuities      = 0u;
    std::uint32_t maxDiscontinuityGap  = 0u;
};

// ---------------------------------------------------------------------------
// RelayArrivalProbe -- GAME THREAD ONLY. Probe 2.
// ---------------------------------------------------------------------------
class RelayArrivalProbe
{
public:
    explicit RelayArrivalProbe(std::uint32_t windowSamples = kRelayArrivalProbeWindowSamples)
        : m_windowSamples(windowSamples == 0u ? kRelayArrivalProbeWindowSamples : windowSamples)
    {
    }

    // Record one relay-ring arrival for `id`, carrying the newest capture tick the
    // ring held AND how many NEW capture ticks it delivered. Returns true -- filling
    // `outSummary`, resetting the histogram -- when this sample completed a window.
    // `outGapCaptureTicks` receives the gap this arrival contributed, or 0 when it
    // contributed none. §8
    //
    //   FOUR ARRIVALS ARE NOT SAMPLES: the FIRST for an id (no previous newest, so
    //   it only seeds the watermark); one that did NOT advance (see `noAdvance`);
    //   one that went BACKWARDS; one whose gap exceeds the discontinuity guard.
    //   §6 §7
    //
    // ⛔ `newCaptureTicksDelivered` IS REQUIRED AND HAS NO DEFAULT: a default of 1 is the retired replace-latest premise, and is how this reported ~120 per mille on a lossless flush. §8
    // ⛔ IT COMES FROM `RelayedInputIngestReport::newCaptureTicksIngested`, which counts entries NOT already resident -- re-delivery of a held tick is not new coverage. §8
    // ⛔ IT IS CLAMPED TO `gap` BEFORE USE: the exactness argument is about the interval `(previousNewest, newestCaptureTick]`, so the clamp stops negative loss reading as a huge positive. §8
    // ⚠ A BACKWARDS ARRIVAL SHOULD NOT HAPPEN (the relay stream is monotonic by construction), so it is treated as a no-advance and the watermark is left alone rather than rewound. §6
    bool noteArrival(unsigned int   id,
                     std::uint32_t  newestCaptureTick,
                     std::uint32_t  newCaptureTicksDelivered,
                     RelayArrivalWindowSummary& outSummary,
                     std::uint32_t* outGapCaptureTicks = nullptr)
    {
        if (outGapCaptureTicks != nullptr)
        {
            *outGapCaptureTicks = 0u;
        }

        const auto it = m_lastNewestCaptureTick.find(id);
        if (it == m_lastNewestCaptureTick.end())
        {
            m_lastNewestCaptureTick.emplace(id, newestCaptureTick);
            return false;                       // first arrival — seeds, samples nothing
        }

        if (newestCaptureTick <= it->second)
        {
            ++m_noAdvance;
            return false;
        }

        const std::uint32_t gap = newestCaptureTick - it->second;
        it->second = newestCaptureTick;                 // re-seeds either way

        // ⛔ THE DISCONTINUITY GUARD, AND IT IS NOT OPTIONAL: without it a single stall reports ~358 per mille loss on a window whose other 118 samples are perfect. §7
        // ⛔ THIS IS A SAMPLE OF NOTHING -- no bucket, no `maxGap`, no `m_samples`, no accumulator -- so it cannot close a window either. A window closes on 120 REAL samples. §7
        // ⛔ `newCaptureTicksDelivered` IS DISCARDED HERE TOO: crediting its delivered ticks while not charging its gap would let an interruption IMPROVE the reported rate. §8
        // ⚠ The watermark is advanced above, so the NEXT arrival measures from the far side of the interruption rather than re-charging it. §7
        if (gap > kRelayArrivalDiscontinuityTicks)
        {
            ++m_discontinuities;
            if (gap > m_maxDiscontinuityGap)
            {
                m_maxDiscontinuityGap = gap;
            }
            // ⛔ `*outGapCaptureTicks` STAYS 0 -- its contract is the gap this arrival CONTRIBUTED, and this one contributed none; the magnitude rides the window line instead. §7
            return false;
        }

        if (outGapCaptureTicks != nullptr)
        {
            *outGapCaptureTicks = gap;
        }

        const std::size_t bucket = (gap >= kRelayArrivalMaxTrackedGap)
            ? kGapOverflowBucket
            : static_cast<std::size_t>(gap);
        ++m_buckets[bucket];
        if (bucket == kGapOverflowBucket)
        {
            ++m_saturatedSamples;
        }

        ++m_samples;
        if (gap > m_maxGap)
        {
            m_maxGap = gap;
        }

        // ⛔ ACCUMULATED ON THE SAME ACCEPTED-ARRIVAL PATH AS THE HISTOGRAM, so the two can never disagree about which samples they cover. §8
        // ⛔ THE CLAMP IS NOT DEFENSIVE TIDINESS: it is what keeps the subtraction an honest statement about the interval `(previousNewest, newestCaptureTick]`. §8
        const std::uint32_t delivered = (newCaptureTicksDelivered > gap)
            ? gap
            : newCaptureTicksDelivered;

        m_lostCaptureTicks      += (gap - delivered);
        m_deliveredCaptureTicks += delivered;
        m_expectedCaptureTicks  += gap;

        if (m_samples < m_windowSamples)
        {
            return false;
        }

        fillSummary(outSummary);
        resetWindow();
        return true;
    }

    void forgetOwner(unsigned int id)
    {
        m_lastNewestCaptureTick.erase(id);
    }

    // --- introspection; tests and diagnostics only -------------------------
    std::uint32_t sampleCount()     const { return m_samples; }
    std::uint32_t maxGap()          const { return m_maxGap; }
    std::uint32_t noAdvance()       const { return m_noAdvance; }
    std::uint32_t discontinuities() const { return m_discontinuities; }

    // The percentile the window WOULD report right now, so a test can assert a
    // distribution without having to fill an exact window.
    void peekSummary(RelayArrivalWindowSummary& out) const { fillSummary(out); }

    // ⛔ MIRRORS `RelayReadProbe::trackedOwnerCount()` -- direct proof that `forgetOwner` shrinks the id-keyed map, for a leak test that must not lean on `noteArrival` semantics. §13
    std::size_t trackedOwnerCount() const { return m_lastNewestCaptureTick.size(); }

private:
    // ⛔ NEAREST-RANK, over interpolation: the samples are integer ticks, and an interpolated 3.7-tick p99 would be rounded up to 4 before comparison against `depth`. §13
    std::uint32_t percentile(std::uint32_t numeratorPercent, bool& outSaturated) const
    {
        outSaturated = false;
        if (m_samples == 0u)
        {
            return 0u;
        }

        // ceil(numeratorPercent * n / 100) without floating point.
        const std::uint64_t scaled = static_cast<std::uint64_t>(numeratorPercent)
                                   * static_cast<std::uint64_t>(m_samples);
        const std::uint64_t rank = (scaled + 99u) / 100u;

        std::uint64_t cumulative = 0u;
        for (std::size_t bucket = 1u; bucket <= kGapOverflowBucket; ++bucket)
        {
            cumulative += m_buckets[bucket];
            if (cumulative >= rank)
            {
                outSaturated = (bucket == kGapOverflowBucket);
                return static_cast<std::uint32_t>(bucket);
            }
        }
        // Unreachable while `rank <= m_samples`, which the arithmetic above
        // guarantees; falling through to the exact max is the harmless answer.
        outSaturated = false;
        return m_maxGap;
    }

    void fillSummary(RelayArrivalWindowSummary& out) const
    {
        bool p50Saturated = false;
        out.samples          = m_samples;
        out.p50              = percentile(50u, p50Saturated);
        out.p99              = percentile(99u, out.p99Saturated);
        out.maxGap           = m_maxGap;
        out.noAdvance        = m_noAdvance;
        out.saturatedSamples = m_saturatedSamples;

        out.lostCaptureTicks      = m_lostCaptureTicks;
        out.deliveredCaptureTicks = m_deliveredCaptureTicks;
        out.expectedCaptureTicks  = m_expectedCaptureTicks;
        out.lostCaptureTicksX1000 = (m_expectedCaptureTicks == 0u)
            ? 0u
            : static_cast<std::uint32_t>(
                  (static_cast<std::uint64_t>(m_lostCaptureTicks) * 1000u)
                  / m_expectedCaptureTicks);

        out.discontinuities     = m_discontinuities;
        out.maxDiscontinuityGap = m_maxDiscontinuityGap;
    }

    void resetWindow()
    {
        m_buckets.fill(0u);
        m_samples             = 0u;
        m_maxGap              = 0u;
        m_noAdvance           = 0u;
        m_saturatedSamples    = 0u;
        m_lostCaptureTicks    = 0u;
        m_deliveredCaptureTicks = 0u;
        m_expectedCaptureTicks = 0u;
        // ⛔ PER-WINDOW, LIKE FrameHealthProbe'S: the field says whether THIS window was interrupted, and a cumulative count would mark every later window suspect forever. §7
        m_discontinuities     = 0u;
        m_maxDiscontinuityGap = 0u;
        // ⛔ THE PER-ID WATERMARK DELIBERATELY SURVIVES THE WINDOW BOUNDARY: a gap across an edge is a real gap, and dropping it discards one sample per component per window. §6
    }

    std::uint32_t m_windowSamples;

    // Newest capture tick seen per component. Bounded by live ids (forgetOwner).
    std::unordered_map<unsigned int, std::uint32_t> m_lastNewestCaptureTick;

    std::array<std::uint32_t, kGapOverflowBucket + 1u> m_buckets{};
    std::uint32_t m_samples          = 0u;
    std::uint32_t m_maxGap           = 0u;
    std::uint32_t m_noAdvance        = 0u;
    std::uint32_t m_saturatedSamples = 0u;

    // The R = 0 loss counter's accumulators; all reset with the window, while the
    // per-id watermark that produces the gaps deliberately does not. §8
    // ⛔ THE INVARIANT `lost + delivered == expected` IS WHAT MAKES THE WINDOW CHECKABLE. §8
    std::uint32_t m_lostCaptureTicks      = 0u;
    std::uint32_t m_deliveredCaptureTicks = 0u;
    std::uint32_t m_expectedCaptureTicks  = 0u;

    // The discontinuity guard's tally. See kRelayArrivalDiscontinuityTicks. §7
    std::uint32_t m_discontinuities     = 0u;
    std::uint32_t m_maxDiscontinuityGap = 0u;
};

// ---------------------------------------------------------------------------
// PROBE 4 -- SIM TICKS PER GAME-THREAD FRAME, i.e. FRAME HEALTH.
//
// Built server-only, to measure the CAUSE of what PROBE 2 measures the EFFECT of:
// property replication runs once per server GAME-THREAD FRAME, so a 60 Hz sim on a
// 30 fps server advances TWO sim ticks per replication. Later extended to the
// CLIENT, where the same ratio plus `meanFrameMicros` is frame health under resim
// load. §9
//
// ⛔ THE SERVER FRAME-RATE SETTING IS A CAP ON THAT RATE, NEVER A FLOOR -- the load-bearing half of the relation every measured number falls out of. §9
// ⛔ TWO ROUTES TO A RATIO ABOVE 1, AND NOT THE SAME DEFECT: the physics engine may sub-step, or the game thread may be slow. The sub-step count rides ALONGSIDE the ratio, never inside it. §9
// ⛔ ON A RESIMMING CLIENT THAT SEPARATION IS THE WHOLE POINT: a resim burst folds extra ticks into `numSteps` at the SAME hook, so `numStepsAboveOne > 0` means resim, not a hitch. §9
// ⛔ HOOK-INDEPENDENT BY CONSTRUCTION: the caller supplies a GLOBAL frame counter, not an invocation count -- which is what let this probe gain a second role for free. §9
// ⛔ THE HOOK'S CADENCE IS MEASURED, NEVER ASSUMED: the probe reports how its own invocations fell across frames (`dFrame == 1`, `== 0`, `> 1`) on either role. §9
// ⛔ GAME THREAD ONLY, ON WHICHEVER ROLE THE OWNER RUNS ON, and each role gets its OWN instance -- never shared, so there is no cross-role synchronization question. §9
// ⛔ TICK SOURCE IS THE CALLER'S PROBLEM, and there is one right answer on the game thread: the tick-to-physics-frame mapper's atomic offset, never the physics-thread clock. §9
// ⚠ The two roles emit under DIFFERENT log tags and categories -- a line indistinguishable from the other role's by grep has already cost one mis-attributed analysis. §9
// ---------------------------------------------------------------------------

// Exact buckets for 0..64 sim ticks per frame; anything larger saturates. 0 is a
// real observation here (a frame in which physics did not step), which is why the
// range starts at 0 rather than at 1 as the arrival histogram's does.
inline constexpr std::uint32_t kFrameHealthMaxTrackedTicks = 64u;
inline constexpr std::size_t   kFrameHealthOverflowBucket  =
    static_cast<std::size_t>(kFrameHealthMaxTrackedTicks) + 1u;

// One summary per this many samples -- ~2 s at 60 fps, ~4 s on a 30 fps server,
// which is itself informative: the summaries thin out exactly when the measured
// thing is bad. §9
inline constexpr std::uint32_t kFrameHealthProbeWindowSamples = 120u;

// A sim-tick jump larger than this is a DISCONTINUITY (the mapper offset being
// established, a level transition, a multi-second editor stall), not a hitch. §9
// ⛔ COUNTED AND REPORTED, NEVER SILENTLY DROPPED -- sampling it would put a five-digit outlier in `max` and hide every real number behind it. §9
inline constexpr std::uint32_t kFrameHealthDiscontinuityTicks = 600u;

struct FrameHealthWindowSummary
{
    // --- the ratio, over samples where EXACTLY ONE frame elapsed -------------
    std::uint32_t samples          = 0u;
    std::uint32_t p50              = 0u;
    std::uint32_t p99              = 0u;
    std::uint32_t maxTicksPerFrame = 0u;
    bool          p99Saturated     = false;

    // --- the ratio, aggregated over the WHOLE window ------------------------
    // ⛔ CADENCE-INDEPENDENT: the totals are differences of the CALLER's own counters, so this holds if the hook fired twice in a frame or skipped one. x100 keeps the log integer. §9
    std::uint32_t totalSimTicks         = 0u;
    std::uint32_t totalFrames           = 0u;
    std::uint32_t meanTicksPerFrameX100 = 0u;
    std::uint32_t meanFrameMicros       = 0u;

    // --- the HOOK CADENCE verification --------------------------------------
    std::uint32_t oncePerFrameSamples = 0u;   // dFrame == 1 (the assumed case)
    std::uint32_t sameFrameSamples    = 0u;   // dFrame == 0 (fires per SUB-STEP)
    std::uint32_t skippedFrameSamples = 0u;   // dFrame  > 1 (fires less often)
    std::uint32_t discontinuities     = 0u;   // re-seeded; see the constant above

    // --- the SUB-STEP cross-check -------------------------------------------
    // ⛔ REPORTED SIDE BY SIDE SO THE TWO ARE NEVER CONFLATED: numSteps > 1 is SUB-STEPPING, while a ratio above 1 with numSteps == 1 throughout is FRAME-RATE SHORTFALL. §9
    std::uint32_t totalNumSteps    = 0u;
    std::uint32_t maxNumSteps      = 0u;
    std::uint32_t numStepsAboveOne = 0u;
};

class FrameHealthProbe
{
public:
    explicit FrameHealthProbe(std::uint32_t windowSamples = kFrameHealthProbeWindowSamples)
        : m_windowSamples(windowSamples == 0u ? kFrameHealthProbeWindowSamples : windowSamples)
    {
    }

    // Record one game-thread sample. Returns true -- filling `out` and resetting the
    // window -- when this sample completed a window. `simTick` must come from a
    // source safe to read on the GAME thread; `nowMicros` is a monotonic wall-clock
    // reading, for the mean frame time. §9
    // ⛔ `frameCounter` MUST BE THE ENGINE'S OWN FRAME COUNTER, NOT AN INVOCATION COUNT -- the whole hook-independence property rests on that. §9
    bool noteFrame(std::uint64_t frameCounter,
                   std::uint32_t simTick,
                   std::uint32_t numSteps,
                   std::uint64_t nowMicros,
                   FrameHealthWindowSummary& out)
    {
        // ⚠ PER INVOCATION, accumulated before any frame-delta reasoning, so it stays correct whatever the cadence turns out to be. §9
        m_totalNumSteps += numSteps;
        if (numSteps > m_maxNumSteps) { m_maxNumSteps = numSteps; }
        if (numSteps > 1u)            { ++m_numStepsAboveOne; }

        if (!m_seeded)
        {
            m_seeded = true;
            reseedAnchors(frameCounter, simTick, nowMicros);
            return false;
        }

        // ⛔ A BACKWARDS OR IMPLAUSIBLE JUMP IS A DISCONTINUITY, NOT A MEASUREMENT -- re-seed from it rather than recording it. §9
        if (simTick < m_lastSimTick
            || frameCounter < m_lastFrameCounter
            || (simTick - m_lastSimTick) > kFrameHealthDiscontinuityTicks)
        {
            ++m_discontinuities;
            reseedAnchors(frameCounter, simTick, nowMicros);
            return false;
        }

        const std::uint64_t frameDelta = frameCounter - m_lastFrameCounter;
        const std::uint32_t tickDelta  = simTick - m_lastSimTick;

        if (frameDelta == 0u)
        {
            ++m_sameFrameSamples;       // this hook fires more than once per frame
        }
        else if (frameDelta == 1u)
        {
            ++m_oncePerFrameSamples;
            const std::size_t bucket = (tickDelta > kFrameHealthMaxTrackedTicks)
                ? kFrameHealthOverflowBucket
                : static_cast<std::size_t>(tickDelta);
            ++m_buckets[bucket];
            ++m_ratioSamples;
            if (tickDelta > m_maxTicksPerFrame) { m_maxTicksPerFrame = tickDelta; }
        }
        else
        {
            ++m_skippedFrameSamples;    // this hook did not fire on every frame
        }

        m_lastFrameCounter = frameCounter;
        m_lastSimTick      = simTick;
        m_lastMicros       = nowMicros;
        ++m_samplesThisWindow;

        if (m_samplesThisWindow < m_windowSamples)
        {
            return false;
        }

        fillSummary(out, frameCounter, simTick, nowMicros);
        resetWindow(frameCounter, simTick, nowMicros);
        return true;
    }

    // --- introspection; tests and diagnostics only -------------------------
    std::uint32_t ratioSampleCount() const { return m_ratioSamples; }
    std::uint32_t sameFrameSamples() const { return m_sameFrameSamples; }
    std::uint32_t skippedFrames()    const { return m_skippedFrameSamples; }
    std::uint32_t discontinuities()  const { return m_discontinuities; }

    // The summary the window WOULD report right now, given the caller's current
    // counters, so a test can assert a distribution without filling a window.
    void peekSummary(FrameHealthWindowSummary& out,
                     std::uint64_t frameCounter,
                     std::uint32_t simTick,
                     std::uint64_t nowMicros) const
    {
        fillSummary(out, frameCounter, simTick, nowMicros);
    }

private:
    void reseedAnchors(std::uint64_t frameCounter, std::uint32_t simTick,
                       std::uint64_t nowMicros)
    {
        m_lastFrameCounter   = frameCounter;
        m_lastSimTick        = simTick;
        m_lastMicros         = nowMicros;
        m_windowStartFrame   = frameCounter;
        m_windowStartSimTick = simTick;
        m_windowStartMicros  = nowMicros;
    }

    // ⛔ NEAREST-RANK, and the bucket index IS the tick count -- same definition as RelayArrivalProbe::percentile, except that bucket 0 is a real observation. §13
    std::uint32_t percentile(std::uint32_t numeratorPercent, bool& outSaturated) const
    {
        outSaturated = false;
        if (m_ratioSamples == 0u)
        {
            return 0u;
        }

        const std::uint64_t scaled = static_cast<std::uint64_t>(numeratorPercent)
                                   * static_cast<std::uint64_t>(m_ratioSamples);
        const std::uint64_t rank = (scaled + 99u) / 100u;

        std::uint64_t cumulative = 0u;
        for (std::size_t bucket = 0u; bucket <= kFrameHealthOverflowBucket; ++bucket)
        {
            cumulative += m_buckets[bucket];
            if (cumulative >= rank)
            {
                outSaturated = (bucket == kFrameHealthOverflowBucket);
                return static_cast<std::uint32_t>(bucket);
            }
        }
        outSaturated = false;
        return m_maxTicksPerFrame;
    }

    void fillSummary(FrameHealthWindowSummary& out,
                     std::uint64_t frameCounter,
                     std::uint32_t simTick,
                     std::uint64_t nowMicros) const
    {
        bool p50Saturated = false;
        out.samples          = m_ratioSamples;
        out.p50              = percentile(50u, p50Saturated);
        out.p99              = percentile(99u, out.p99Saturated);
        out.maxTicksPerFrame = m_maxTicksPerFrame;

        const std::uint64_t frames = (frameCounter >= m_windowStartFrame)
            ? (frameCounter - m_windowStartFrame) : 0u;
        const std::uint32_t ticks = (simTick >= m_windowStartSimTick)
            ? (simTick - m_windowStartSimTick) : 0u;
        const std::uint64_t micros = (nowMicros >= m_windowStartMicros)
            ? (nowMicros - m_windowStartMicros) : 0u;

        out.totalFrames           = static_cast<std::uint32_t>(frames);
        out.totalSimTicks         = ticks;
        out.meanTicksPerFrameX100 = (frames == 0u)
            ? 0u
            : static_cast<std::uint32_t>((static_cast<std::uint64_t>(ticks) * 100u) / frames);
        out.meanFrameMicros = (frames == 0u)
            ? 0u
            : static_cast<std::uint32_t>(micros / frames);

        out.oncePerFrameSamples = m_oncePerFrameSamples;
        out.sameFrameSamples    = m_sameFrameSamples;
        out.skippedFrameSamples = m_skippedFrameSamples;
        out.discontinuities     = m_discontinuities;

        out.totalNumSteps    = m_totalNumSteps;
        out.maxNumSteps      = m_maxNumSteps;
        out.numStepsAboveOne = m_numStepsAboveOne;
    }

    void resetWindow(std::uint64_t frameCounter, std::uint32_t simTick,
                     std::uint64_t nowMicros)
    {
        m_buckets.fill(0u);
        m_ratioSamples        = 0u;
        m_maxTicksPerFrame    = 0u;
        m_oncePerFrameSamples = 0u;
        m_sameFrameSamples    = 0u;
        m_skippedFrameSamples = 0u;
        m_discontinuities     = 0u;
        m_totalNumSteps       = 0u;
        m_maxNumSteps         = 0u;
        m_numStepsAboveOne    = 0u;
        m_samplesThisWindow   = 0u;

        m_windowStartFrame   = frameCounter;
        m_windowStartSimTick = simTick;
        m_windowStartMicros  = nowMicros;
    }

    std::uint32_t m_windowSamples;

    bool          m_seeded           = false;
    std::uint64_t m_lastFrameCounter = 0u;
    std::uint32_t m_lastSimTick      = 0u;
    std::uint64_t m_lastMicros       = 0u;

    std::uint64_t m_windowStartFrame   = 0u;
    std::uint32_t m_windowStartSimTick = 0u;
    std::uint64_t m_windowStartMicros  = 0u;

    std::array<std::uint32_t, kFrameHealthOverflowBucket + 1u> m_buckets{};
    std::uint32_t m_ratioSamples      = 0u;
    std::uint32_t m_maxTicksPerFrame  = 0u;
    std::uint32_t m_samplesThisWindow = 0u;

    std::uint32_t m_oncePerFrameSamples = 0u;
    std::uint32_t m_sameFrameSamples    = 0u;
    std::uint32_t m_skippedFrameSamples = 0u;
    std::uint32_t m_discontinuities     = 0u;

    std::uint32_t m_totalNumSteps    = 0u;
    std::uint32_t m_maxNumSteps      = 0u;
    std::uint32_t m_numStepsAboveOne = 0u;
};
