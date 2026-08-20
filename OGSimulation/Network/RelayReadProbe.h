#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// ---------------------------------------------------------------------------
// RelayReadProbe / RelayArrivalProbe — the CLIENT-side relay telemetry.
// (og-netcode-v2-input-relay T19; unblocks T9 scenarios 4, 5 and 6.)
//
// WHAT THESE ARE FOR. Two questions T9 asks that the shipped code could not
// answer, because neither emitted anything at all:
//
//   * "is the SCHEDULE being CONSUMED?" — `resolveScheduledRelayedInput` had zero
//     log calls, so a proxy that missed on every single probe and a proxy that hit
//     on every single probe produced byte-identical logs. `[CollectInput] …
//     source=RemoteInputCache hasStore=1` reports that a store EXISTS, which is a
//     different claim and is present either way.
//   * "how often does the relay ring actually ARRIVE?" — the OnRep was unlogged, so
//     T9's `depth >= gap_p99 + margin` rule had no gap distribution to be derived
//     from and could only ever have been guessed.
//
// INSTRUMENTATION ONLY. Nothing here feeds back into resolution: the probes are
// counters plus a windowed summary, and every code path that consults them is a
// log call. Removing this header would change no simulated value.
//
// ---------------------------------------------------------------------------
// TWO OBJECTS BECAUSE THERE ARE TWO THREADS. THIS IS THE HEADER STATEMENT THE
// TASK ASKS FOR (T19 review F3), and it is a correctness property, not tidiness.
//
//   RelayReadProbe     PHYSICS thread. Fed from SimulationNetSync::collectInputAll
//                      (T7's prediction proxy branch) and ::collectResimInputAll
//                      (T6's resim frontier row). Window driven by the PREDICTION
//                      tick.
//   RelayArrivalProbe  GAME thread. Fed from the relay-ring arrival callback bound
//                      in registerPredictionOwner (USimmableUpdateComponent::
//                      OnRep_RelayedInputRing -> populateRemoteInputCache).
//                      Window driven by SAMPLE COUNT — the game thread has no
//                      simulation tick to hand, and inventing one would be a
//                      second clock to keep honest.
//
//   FrameHealthProbe   [T20; renamed T49] GAME thread, on EITHER role. Fed from the
//                      game-thread hook that precedes each physics frame. Window
//                      driven by SAMPLE COUNT. Originally SERVER-only (named
//                      `ServerFrameProbe`) because only the server had a call site
//                      that fed it; T49 gives the client a call site too, since the
//                      math here was always role-neutral (see its own banner at the
//                      bottom of this file). The two roles emit under DIFFERENT log
//                      tags and categories — see the call site in
//                      SimulationManagerUImpl.cpp — because a line that cannot be
//                      told apart from the other role's by grep alone has already
//                      cost this initiative one mis-attributed analysis (item 49).
//
// They are SEPARATE OBJECTS with SEPARATE WINDOWS. A single shared window would
// have one thread reset counters the other thread is mid-increment on, corrupting
// a whole window's totals — strictly worse than the torn-SLOT debt
// RemoteInputCache.h documents and accepts, because a torn slot costs one input
// on one proxy for one tick while a torn window reset costs the entire
// measurement. Neither object is touched by the other's thread, so no atomics and
// no seam are needed; that is the whole reason for the split.
//
// NEITHER OBJECT LOGS. They accumulate and hand back a summary struct; the caller
// owns the logger and does the SIMLOG. Same convention `populateRemoteInputCache`
// and `RemoteMoveQueue` already follow (core containers do not log), and it is
// what lets the Low-Level-Tests assert on the numbers rather than on strings.
//
// ENGINE-AGNOSTIC. STL only — no UE types, no OGTypes, no other OGSimulation
// header. That is deliberate: it makes the whole of this file testable from
// og-simulation-tests without a simulatable, an owner-traits specialization or a
// logger.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PROBE 1 — the four outcomes of the scheduled read.
//
// FOUR, NOT TWO, and the collapse to hit/miss is the mistake this enum exists to
// prevent (T19 description). `resolveScheduledRelayedInput`'s three-rung ladder
// serves `fallback()` from THREE different situations that mean completely
// different things while behaving identically:
//
//   NoProbe     rung 0 — `!findLatest().valid`, nothing has EVER arrived for this
//               character. The pre-registration / join window. NOT starvation:
//               there is no data yet because the channel has not started, and
//               counting it as one would make the join window look like a fault
//               on every single join.
//   Hit         the probe at `tick - dLatest` found a candidate AND that
//               candidate's own stamp equals `dLatest`. The proxy is consuming
//               the server's actual schedule — the whole claim of the scheduled
//               regime, and what T9 scenario 4 exists to confirm at floor > 0.
//   Miss        the probe found NOTHING resident at the scheduled capture tick.
//               STARVATION: the entry we should be running on has not arrived (or
//               has been evicted). Also covers the `tick < dA` early-session case,
//               where the probe tick would underflow — see the classification note
//               on `resolveScheduledRelayedInput`.
//   VerifyFail  a candidate WAS resident but its stamp differs from `dLatest`: the
//               delay REGIME shifted under the reader. TRANSITION, not starvation.
//               The data is arriving fine; the schedule it was stamped against is
//               no longer the current one, so replaying it would reproduce a
//               schedule the authority is not using.
//
// Miss and VerifyFail are the pair that must never be merged. A window that is all
// Miss says the wire is starving the proxy; a window that is all VerifyFail says
// the wire is healthy and the delay is thrashing. Same fallback behaviour, opposite
// diagnosis, and distinguishing them is precisely what T9 scenario 4 needs in order
// to tell "the schedule is working" from "the schedule is thrashing".
// ---------------------------------------------------------------------------
enum class ScheduledRelayedReadOutcome : std::uint8_t
{
    NoProbe = 0,
    Hit,
    Miss,
    VerifyFail,
};

// ---------------------------------------------------------------------------
// [T20] PROBE B — WHY the miss happened. THE ENUM ABOVE IS DELIBERATELY UNCHANGED.
//
// T19's four outcomes record THAT a scheduled read missed. They cannot say why, and
// that is precisely the gap that left one measured client at 0.8% hit-rate
// unexplained while the coverage model predicted ~50% (RelayDepthCoverageHypothesis
// §3, architect response §8.4). The three whys have three DIFFERENT remedies, and
// picking the wrong one is the failure mode this enum exists to prevent:
//
//   InSpan       probeTick lies between the store's oldest and newest resident
//                capture ticks and is ABSENT. A COVERAGE HOLE: the sender produced
//                that tick and it was clobbered in the replace-latest relay ring
//                before UE replicated it. THIS is the class the depth hypothesis
//                predicts, and the only one raising the ring's retention depth
//                would move (item 63 / RN-13, 2026-08-16: that was a session-
//                configurable knob; it is retired — see RN-13, ReviewNotes.md).
//   AboveNewest  probeTick is NEWER than anything the store holds. The receiver is
//                asking for a capture that has not been produced or has not landed.
//                DEPTH IS IRRELEVANT here — no amount of ring redundancy delivers a
//                capture that does not exist yet. This is the design's own deficit
//                condition (D_A >= lead_B + downlink_B, spectrum §3.2) failing for
//                that particular sender/receiver pair.
//   BelowOldest  probeTick is OLDER than the store retains. Clock misalignment, or
//                the store's 64-tick capacity being outrun.
//   NoProbeTick  no probe tick could be formed at all (the `tick < dA` underflow
//                guard: a session younger than the delay). Kept SEPARATE rather than
//                folded into BelowOldest, which it superficially resembles — folding
//                an early-session artefact into a clock-misalignment bucket would
//                corrupt the exact discrimination this probe exists to provide.
//
// WHY A SECOND ENUM RATHER THAN SIX OUTCOMES. Splitting `Miss` into three
// enumerators would have rewritten every existing use of
// `ScheduledRelayedReadOutcome::Miss` — the T19 test suite, the stale-run rule
// (`isStaleFallbackOutcome`), and the two shipped call sites — to buy nothing the
// orthogonal field does not. `outcome` answers "which rung of the ladder answered",
// which is a property of the LADDER; `missClass` answers "where was the receiver
// asking relative to what it has", which is a property of the STORE. They are
// genuinely different questions, and the pair is strictly more informative than a
// flattened six-way enum: a window can report six classes AND still be compared
// against every T19-era measurement.
// ---------------------------------------------------------------------------
enum class ScheduledRelayedReadMissClass : std::uint8_t
{
    NotAMiss = 0,
    InSpan,
    AboveNewest,
    BelowOldest,
    NoProbeTick,
};

// True iff this outcome served `fallback()` from a store that HAS data — i.e. the
// D4 stale-hold situation probe 3 measures. Deliberately excludes NoProbe (see
// RelayReadProbe::notePredictionRead) and, obviously, Hit.
inline bool isStaleFallbackOutcome(ScheduledRelayedReadOutcome outcome)
{
    return outcome == ScheduledRelayedReadOutcome::Miss
        || outcome == ScheduledRelayedReadOutcome::VerifyFail;
}

// What `resolveScheduledRelayedInput` reports back to its CALLER, so the caller can
// count and log without the ladder itself gaining any state. Every field except
// `outcome` is diagnostic detail for the per-event Verbose line; `probeTick` and
// `candidateDA` are meaningless on the outcomes that never computed them, and the
// comments say which.
struct ScheduledRelayedReadReport
{
    ScheduledRelayedReadOutcome outcome = ScheduledRelayedReadOutcome::NoProbe;
    // The capture tick the ladder probed, i.e. `tick - dLatest`. Meaningless on
    // NoProbe (no probe was formed) and on the tick < dA underflow guard.
    std::uint32_t probeTick = 0u;
    // The stamp the verify step compared AGAINST — `findLatest().dA`. Meaningless
    // on NoProbe.
    std::uint8_t dLatest = 0u;
    // The resident candidate's OWN stamp. Meaningful on Hit and VerifyFail only;
    // on VerifyFail this is the value that differed from `dLatest`, which is the
    // single most useful number in a delay-transition trace.
    std::uint8_t candidateDA = 0u;

    // --- [T20] PROBE B ------------------------------------------------------

    // Why the miss happened. `NotAMiss` on every non-Miss outcome.
    ScheduledRelayedReadMissClass missClass = ScheduledRelayedReadMissClass::NotAMiss;

    // THE SIGNED DISTANCE `probeTick - newestResident`, and at depth 1 this is the
    // richer half of Probe B. The three miss classes are buckets over exactly this
    // quantity, so the distribution separates them CONTINUOUSLY: a window whose
    // deltas cluster at +2 is a receiver reading ahead of what it has been sent (no
    // depth will help), and one whose deltas cluster at -3 with misses is a receiver
    // reading inside a span full of holes (depth will).
    //
    // Set on every outcome that formed a probe tick — Hit and VerifyFail included,
    // because "how far behind the newest is a HIT" is the calibration the miss
    // deltas are read against. Costs nothing: `newestResident` on those arms is
    // `findLatest().captureTick`, which the ladder has already computed.
    bool         deltaToNewestValid = false;
    std::int32_t deltaToNewest      = 0;

    // The store's resident span at the moment of the read. Filled ONLY on a miss
    // (the classification needs it and nothing else does), so `spanValid` is false
    // on Hit / VerifyFail / NoProbe even though a span exists there too.
    bool          spanValid      = false;
    std::uint32_t oldestResident = 0u;
    std::uint32_t newestResident = 0u;
    std::uint32_t residentCount  = 0u;
};

// ---------------------------------------------------------------------------
// [T20] The signed-delta histogram — `probeTick - newestResident`, per call site.
//
// Exact for |delta| <= kRelayDeltaHistogramRange, with one saturating bucket at each
// end; the exact min and max are tracked alongside, so a saturated percentile is
// always accompanied by a real number (the same contract RelayArrivalProbe's gap
// histogram uses, and for the same reason).
//
// THE RANGE IS THE STORE'S CAPACITY ON PURPOSE. Beyond +/-64 capture ticks the
// receiver is asking outside anything the store could ever have held, so the exact
// value stops carrying diagnostic information that the min/max does not already
// carry — it only says "far outside", which is what the saturating bucket says.
// ---------------------------------------------------------------------------
inline constexpr std::int32_t kRelayDeltaHistogramRange = 64;

struct RelayDeltaSummary
{
    std::uint32_t samples = 0u;

    // Nearest-rank percentiles of the SIGNED delta. p10 and p90 are reported rather
    // than a mean because the distribution is expected to be bimodal (hits clustered
    // just below zero, above-newest misses clustered above it) and a mean of a
    // bimodal distribution names a value that never occurs.
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
    // Bucket 0 absorbs everything below -range; bucket kBucketCount-1 everything
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
    // The delta a bucket index represents. The two saturating buckets report the
    // first value OUTSIDE the exact range, which is a floor/ceiling on the truth and
    // never a claim of precision it does not have.
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

    // NEAREST-RANK over the buckets in ASCENDING delta order — the same definition
    // RelayArrivalProbe::percentile uses.
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
    // [T20] The MISS TOTAL, unchanged in meaning. The four sub-counters below
    // partition it and always sum to it exactly; `miss` is kept as the total rather
    // than being replaced so every T19-era number stays directly comparable.
    std::uint32_t miss       = 0u;
    std::uint32_t verifyFail = 0u;

    // [T20] The miss partition — see ScheduledRelayedReadMissClass.
    std::uint32_t missInSpan      = 0u;
    std::uint32_t missAboveNewest = 0u;
    std::uint32_t missBelowOldest = 0u;
    std::uint32_t missNoProbeTick = 0u;

    // [T20] `probeTick - newestResident` over every read that formed a probe tick.
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

    // [T20] The full tally. Kept as an overload of the outcome-only form above so
    // every T19 call site and test compiles and counts identically — the added
    // fields are pure refinement, never a reinterpretation of the four totals.
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
    // THE TWO CALL SITES ARE COUNTED SEPARATELY, NEVER SUMMED (T19 description).
    // Prediction and resim run the SAME ladder over the SAME store, so a divergence
    // in their hit rates is a real signal about the frontier — the resim resolves
    // ticks the prediction already ran, so entries that were missing then may have
    // landed since. Summing them would average that signal away, and it is a signal
    // nothing else in the system reports.
    RelayReadCounters prediction;
    RelayReadCounters resim;

    // PROBE 3 — the D4 stale window. Longest consecutive run of fallback-serving
    // reads on ONE character within this window, and which character owned it.
    // This is what sets `K` for the deferred stale-hold rule (T5 / spectrum §8.7)
    // from data instead of a guess.
    std::uint32_t maxConsecutiveFallbackRun = 0u;
    unsigned int  maxConsecutiveFallbackId  = 0u;

    std::uint32_t windowStartTick = 0u;
    std::uint32_t windowEndTick   = 0u;
};

// ~2 s at 60 Hz, matching the cadence ServerReceptionCoordinator::maybeEmitInputStats
// already emits `[InputStats]` on. Deliberately the same feel so an operator reading
// a PIE log sees the two channels tick at a comparable rate; it is NOT derived from
// TimeConfig because SimulationNetSync holds no TimeConfig, and threading one in for
// a log cadence would be a real dependency bought for a cosmetic gain.
inline constexpr std::uint32_t kRelayReadProbeWindowTicks = 120u;

// ---------------------------------------------------------------------------
// RelayReadProbe — PHYSICS THREAD ONLY. Probes 1 and 3.
// ---------------------------------------------------------------------------
class RelayReadProbe
{
public:
    explicit RelayReadProbe(std::uint32_t windowTicks = kRelayReadProbeWindowTicks)
        : m_windowTicks(windowTicks == 0u ? kRelayReadProbeWindowTicks : windowTicks)
    {
    }

    // A scheduled read on the PREDICTION path (collectInputAll's proxy branch).
    //
    // THIS IS THE ONLY CALL SITE THAT FEEDS THE STALE RUN, and that is deliberate:
    // the run is "consecutive ticks this character was served a fallback", which is
    // only meaningful over a MONOTONIC per-tick stream. Resim replays ticks the
    // prediction has already run, out of order and repeatedly, so interleaving it
    // would produce a "consecutive run" that describes nothing.
    //
    // RUNG 0 IS EXCLUDED FROM THE RUN (T19 review F5). Rung 0 also serves
    // `fallback()`, but it is the pre-registration / join window — "no data has
    // ever arrived" — not staleness, which is "data arrived and then stopped
    // scheduling". Counting it would inflate every session's maximum run by the
    // whole length of its join window and bias `K` upward, and setting `K` from
    // data is the entire reason this probe exists.
    //
    // It neither increments the run NOR resets it. Once anything has been pushed
    // into a store, `findLatest().valid` stays true forever (slots are reclaimed by
    // overwrite, never cleared), so NoProbe can only ever be a LEADING prefix and
    // the two treatments are observationally identical. Not-incrementing is the
    // load-bearing half; not-resetting is the literal reading of "exclude rung-0
    // from the run" — the sample is excluded, rather than being treated as a break.
    // [T20] THE SHIPPED OVERLOAD — takes the whole report, so the miss class and the
    // signed delta are tallied alongside the outcome. The outcome-only overload
    // below is kept because it is what the T19 tests drive and because a caller that
    // has only an outcome must not be forced to fabricate a report.
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

    // A scheduled read on the RESIM path (collectResimInputAll's NoRef/remote row).
    // Counted, but it drives neither the window nor the stale run — see above and
    // maybeCloseWindow. Takes no id because nothing per-id is derived from it.
    void noteResimRead(ScheduledRelayedReadOutcome outcome)
    {
        m_resim.note(outcome);
    }

    // Close the window if `predictionTick` has advanced past it, filling `out` and
    // resetting the counters. Returns true ONLY when a window closed AND carried at
    // least one read, so an authority (which allocates no relay stores and therefore
    // never reaches either call site) and an idle client never heartbeat a line.
    //
    // DRIVEN BY THE PREDICTION TICK, and only by it: it is the one monotonic
    // per-frame clock either call site has. A HARD RESYNC can move it BACKWARDS,
    // which is not an error here — the window simply restarts at the new tick
    // rather than staying open for the ~4 billion ticks an unsigned subtraction
    // would compute.
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
        // The per-id CURRENT runs deliberately survive the window boundary — a
        // starvation that straddles two windows is one run, not two. Only the
        // window's MAXIMUM is reset, which is the value being reported.
        m_windowMaxRun    = 0u;
        m_windowMaxRunId  = 0u;
        m_windowStartTick = predictionTick;

        return carriedSomething;
    }

    // Drop per-id state for a character that has unregistered. Without this the run
    // map would grow with every character that has ever existed in the session.
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
    // [T20] The stale-run half of notePredictionRead, factored out so the two
    // overloads cannot drift. The rule itself is T19's, unchanged: see the block
    // above notePredictionRead for why rung 0 neither increments nor resets.
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
// PROBE 2 — replication cadence, IN CAPTURE TICKS (T19 review F4).
//
// THIS IS A CORRECTNESS REQUIREMENT, NOT A UNIT PREFERENCE. The rule this feeds is
// `depth >= gap_p99 + margin`, and `depth` is denominated in CAPTURE ticks — a ring
// entry IS a capture tick. A gap measured in the RECEIVER's local ticks conflates
// the sender's cadence with the receiver's clock geometry (two different leads over
// the server, plus stalls and resyncs), so the resulting p99 would be in the wrong
// units and could not legitimately be compared against `depth` at all.
//
// The measurement is therefore: the newest `captureTick` in the ring on THIS
// arrival minus the newest on the PREVIOUS arrival, per component. Same units as
// depth, immune to local clock skew, and free — `populateRemoteInputCache` already
// walks every entry, so T19 added the newest-captureTick field to the report it
// already returned rather than adding a second walk.
//
// STRUCTURALLY LOCAL-TICK-PROOF: `noteArrival` is not given a local tick and there
// is no clock in this file, so a local-tick implementation cannot be written here
// by accident. The Catch2 case that pins this drives arrivals whose local spacing
// differs from their capture spacing and asserts the CAPTURE answer.
//
// A HISTOGRAM, NOT A MEAN — and not a mean plus a max either. A mean hides exactly
// the tail that sets `depth`: a channel that delivers every tick except for one
// 20-tick stall per second has a mean gap near 1 and a p99 near 20, and it is the
// 20 that the depth rule must clear. Buckets are exact for gaps 1..kMaxTrackedGap
// (the whole range that can ever matter, since the store holds only 64 capture
// ticks); anything larger lands in one saturating bucket AND is still reported
// exactly through `maxGap`, so a saturated p99 is always accompanied by a real
// number.
// ---------------------------------------------------------------------------

// Gaps 1..64 are counted exactly; index 0 is unused (a zero gap is not a sample —
// see noteArrival) and index kGapOverflowBucket absorbs everything larger.
inline constexpr std::uint32_t kRelayArrivalMaxTrackedGap = 64u;
inline constexpr std::size_t   kGapOverflowBucket         = kRelayArrivalMaxTrackedGap + 1u;

// One summary per this many GAP SAMPLES. Sample-driven rather than tick-driven
// because the game thread has no simulation tick; at depth 1 and a healthy wire a
// component replicates about once per tick, so 120 samples is ~2 s per component —
// the same feel as the physics-side window without pretending to share its clock.
inline constexpr std::uint32_t kRelayArrivalProbeWindowSamples = 120u;

// ---------------------------------------------------------------------------
// ⭐ [T34 rework] THE DISCONTINUITY GUARD — the same guard, for the same reason,
// that `FrameHealthProbe` (`kFrameHealthDiscontinuityTicks`) and `RelayWriteProbe`
// (`kRelayWriteDiscontinuityTicks`) already carry. A capture-tick jump larger than
// this is a SINGLE CORRELATED EVENT — a connection hiccup, a host stall, a
// relevancy pause, a re-join, the client's game thread blocking so OnReps queue —
// not `gap - 1` independently lost inputs. Charging it to the per-input loss rate
// mixes two distributions and reports catastrophic failure on a working relay.
//
// WHY THIS MATTERS MORE HERE THAN ON EITHER SIBLING: `lostCaptureTicksX1000` is the
// discriminating term of item 34's acceptance gate, whose pass condition is ~11 per
// mille. A single 47-tick gap in a 120-sample window computes to ~355 per mille —
// 30x the pass condition, on a window whose other 118 samples are healthy.
//
// ⚠ THE VALUE IS 16 BECAUSE THE ARCHIVES FORCE IT, not because it is round. Every
// `[RelayProbe.Arrival]` window in `runs/t39_runA_3char`, `runs/t39_runB_2char` and
// `runs/t33_depth1_control` was re-read; the observed gap distribution is BIMODAL
// with a completely empty band:
//
//   healthy   every window's `max=` is in 1..6, and p99 never exceeds 4
//   EMPTY     no sample anywhere in 7..17, across ~28,000 samples in three runs
//   outliers  18, 20, 20, 22, 46, 47 and four samples >= 64 (max 229)
//
// 16 sits at the top of that empty band: 2.6x the worst healthy `max` and 4x the
// worst healthy p99 below it, 2 ticks of margin below the smallest outlier above
// it. It is also exactly 2 * `relayedInputRing::kMaxDepth` (8) — one flush round
// can publish at most kMaxDepth capture ticks, so a gap of more than two full
// rounds cannot be produced by the flush path at all; something upstream stopped.
//
// The reviewer's suggested 15-30 range is only satisfiable at its very bottom: a
// threshold of 20 or 24 would let the 18/20/22 class through, and the window
// carrying 18 and 22 still computes to ~250 per mille. 16 is therefore not a taste
// call — anything above 17 fails to fix the defect.
//
// And it cannot be produced by ordinary wire loss: at the measured 1.122 % per
// capture tick, 16 consecutive losses has probability ~1e-31.
//
// A gap above this RE-SEEDS the watermark, is counted in `discontinuities`, and is
// a sample of nothing — it enters neither loss accumulator, neither the histogram
// nor `maxGap`. Its magnitude is preserved exactly in `maxDiscontinuityGap` so
// nothing is hidden, only re-classified.
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
    // already-resident tick is the realistic cause). NOT cadence samples — they
    // delivered no new capture tick, so counting them as a gap of 0 would drag the
    // percentiles down and understate the depth requirement. Reported separately
    // because a window that is mostly these is itself worth seeing.
    std::uint32_t noAdvance = 0u;

    // Samples that landed in the saturating bucket.
    //
    // ⚠ [T34 rework] STRUCTURALLY UNREACHABLE SINCE THE DISCONTINUITY GUARD, and
    // this is deliberate rather than an oversight. The bucket sits at
    // kRelayArrivalMaxTrackedGap = 64; the guard fires at 16. Every gap big enough
    // to saturate is re-classified as a discontinuity before it is ever sampled, so
    // `saturatedSamples` and `p99Saturated` now read 0/false forever.
    //
    // THEY ARE KEPT because every archived T22/T33/T39 window carries `saturated=`,
    // and runB's two poisoned windows were found by it — deleting the field would
    // break comparability with exactly the logs that motivated the guard. But an
    // operator reading a NEW log must read `discontinuities` instead: `saturated=`
    // never saw the 46/47 class at all, which is precisely why it was not enough.
    std::uint32_t saturatedSamples = 0u;

    // -----------------------------------------------------------------------
    // ⭐ [og-netcode-v2-input-relay T34] THE R = 0 LOSS COUNTER — MANDATORY, not
    // diagnostic garnish, and the reason is structural: with bare C1 flush-on-poll
    // at R = 0 an input is sent exactly ONCE, and there is NO send-success signal
    // anywhere in Iris that game code can read (T38 §4.3 — `FReplicationWriter::
    // HandleDroppedRecord` recovers the changemask, not the values). So permanent
    // input loss is INVISIBLE on the server by construction.
    //
    // THE COUNTABLE END IS THE CLIENT. Capture ticks are per-character monotonic at
    // ~60/s, so every permanently lost input is a visible ARITHMETIC GAP in the
    // received stream, and nothing else can produce one: a gap of g means g-1
    // capture ticks were produced by the sender and never arrived here.
    //
    // ⛔ [T34 loss-counter fix] THE NUMERATOR IS `gap - delivered`, NOT `gap - 1`,
    // AND THE DIFFERENCE IS THE WHOLE INSTRUMENT. `gap - 1` measures ADVANCE OF THE
    // NEWEST WATERMARK MINUS ONE, which equals the lost count only while an arrival
    // carries exactly ONE new capture tick — the retired replace-latest regime.
    // Under flush-on-poll one arrival publishes the whole staged burst, so a 2-entry
    // burst advances the watermark by 2 and `gap - 1` charges 1 as lost WHILE BOTH
    // ENTRIES ARRIVED. Measured on `runs/t34_run1_2char_2026-08-09_1938`, that read
    // 122 / 129 per mille against a ~11 per mille pass condition — and it was
    // measuring the burst rate: the run's `[RelayFlush] entriesPerRoundX100` was
    // 112-113, and 1 - 1/1.13 = 115 per mille. The instrument was reporting, as
    // loss, precisely the thing the flush now delivers instead of losing.
    //
    //   lostCaptureTicks     Sum(gap - delivered) over the window's accepted
    //                        arrivals, where `delivered` is how many NEW capture
    //                        ticks that arrival actually carried (the caller's
    //                        count; see noteArrival). EXACT, not estimated: the
    //                        watermark advanced by `gap`, `delivered` of those ticks
    //                        arrived, so the remainder provably never did. Wire loss
    //                        + scheduler-skip loss + everything, i.e. exactly the
    //                        total the user's acceptance is priced on.
    //                        ⭐ IT REDUCES TO Sum(gap - 1) AT DEPTH 1, so every
    //                        archived comparison and the replace-latest
    //                        counterfactual stay meaningful.
    //   deliveredCaptureTicks
    //                        Sum(delivered), clamped per sample to `gap`. Reported
    //                        so the window is SELF-CHECKING: `lost + delivered ==
    //                        expected` must hold exactly, on the log line as well as
    //                        in a test. Without it a reader cannot tell a window
    //                        that lost nothing from a window whose delivered count
    //                        was never plumbed through.
    //   expectedCaptureTicks Sum(gap) — the capture ticks the senders PRODUCED over
    //                        the span this window covers. The only honest
    //                        denominator: it is derived from the same samples, so
    //                        it stays correct across window edges, across a varying
    //                        number of remote characters, and on a client whose own
    //                        frame rate has nothing to do with the senders'.
    //   lostCaptureTicksX1000
    //                        per mille of expected. STEADY-STATE EXPECTATION ~ 11
    //                        (the measured 1.122 % wire loss). Sustained material
    //                        excess over the server's `[RelayProbe.Budget] lost=`
    //                        rate means scheduler SKIPS are happening on top of wire
    //                        loss, which is the evidence-driven trigger to re-check
    //                        the diet margins or put R = 1 back on the table
    //                        (T38 §13.4). It must be reported for the join-settling
    //                        window SEPARATELY from steady state (T43 finding 3).
    //
    // WHY IT RIDES THE EXISTING WINDOW RATHER THAN A NEW ONE: it is a ratio of two
    // quantities this probe already computes per sample, and a second window over
    // the same samples would be a second clock to keep honest for no new
    // information. The window is shared across remote ids (as it always has been),
    // and the ratio aggregates correctly across them because both numerator and
    // denominator are sums over the same sample set.
    //
    // ⚠ A no-advance arrival is NOT a sample and contributes to NONE of the three
    // terms — it advanced no capture tick, so it says nothing about loss.
    // `noAdvance` above already reports those separately.
    // -----------------------------------------------------------------------
    std::uint32_t lostCaptureTicks      = 0u;
    std::uint32_t deliveredCaptureTicks = 0u;
    std::uint32_t expectedCaptureTicks  = 0u;
    std::uint32_t lostCaptureTicksX1000 = 0u;

    // ⭐ [T34 rework] Gaps this window RE-CLASSIFIED rather than charged, per
    // kRelayArrivalDiscontinuityTicks. Non-zero means the window covers less
    // continuous capture stream than its `samples` suggests.
    //
    // ⚠ THE OPERATOR RULE, and it is the reason this field exists at all: a window
    // reporting `discont=` > 0 is DISCARDED, not averaged in — exactly as
    // `RelayWriteProbe::discontinuities` is. `lostCaptureTicksX1000` is still
    // honest for the samples it kept, but the excluded event was real and the
    // window no longer covers a contiguous span.
    //
    // `maxDiscontinuityGap` is the largest EXCLUDED gap, exact. Without it the guard
    // would silently swallow the one number that says how bad the interruption was —
    // and before the guard existed, `max=` was the only tell those windows had.
    std::uint32_t discontinuities      = 0u;
    std::uint32_t maxDiscontinuityGap  = 0u;
};

// ---------------------------------------------------------------------------
// RelayArrivalProbe — GAME THREAD ONLY. Probe 2.
// ---------------------------------------------------------------------------
class RelayArrivalProbe
{
public:
    explicit RelayArrivalProbe(std::uint32_t windowSamples = kRelayArrivalProbeWindowSamples)
        : m_windowSamples(windowSamples == 0u ? kRelayArrivalProbeWindowSamples : windowSamples)
    {
    }

    // Record one relay-ring arrival for `id`, carrying the newest capture tick the
    // ring held AND how many NEW capture ticks that ring actually delivered.
    // Returns true — filling `outSummary` and resetting the histogram — when this
    // sample completed a window.
    //
    // ⛔ [T34 loss-counter fix] `newCaptureTicksDelivered` IS REQUIRED AND HAS NO
    // DEFAULT, DELIBERATELY. A default of 1 is exactly the retired replace-latest
    // premise — "one arrival carries one entry" — and silently re-defaulting to it
    // is how this instrument came to report ~120 per mille on a flush that lost
    // nothing (see RelayArrivalWindowSummary's loss block). Making it a positional
    // requirement turns every call site into a statement of what that arrival
    // delivered, and makes a pre-fix call site a compile error rather than a
    // silently-wrong number.
    //
    // WHERE THE COUNT COMES FROM at the shipped call site:
    // `RelayedInputIngestReport::newCaptureTicksIngested`, which the ingest already
    // computes while walking the ring. It counts entries whose capture tick was NOT
    // already resident, because re-delivery of a tick this receiver already holds is
    // not new coverage.
    //
    // IT IS CLAMPED TO `gap` before it is used. The exactness argument — "the
    // watermark advanced by `gap`, `delivered` of those ticks arrived, so the rest
    // never did" — is an argument about the half-open interval
    // `(previousNewest, newestCaptureTick]`, which holds exactly `gap` tick slots.
    // An arrival that additionally back-fills a hole BELOW the previous watermark
    // (not reachable under R = 0's monotonic single-publish stream, but not
    // excluded by any type) would otherwise subtract coverage that belongs to an
    // earlier interval, and could underflow the unsigned difference. The clamp
    // makes the counter unable to report negative loss as an enormous positive one.
    //
    // `outGapCaptureTicks` receives the gap this arrival contributed, or 0 when it
    // contributed none (first arrival for this id, or no advance). It exists so the
    // caller can emit its per-event Verbose line without re-deriving the gap.
    //
    // FOUR ARRIVALS ARE NOT SAMPLES, and each is a different thing:
    //   * the FIRST arrival for an id — there is no previous newest to subtract, so
    //     there is no gap. Counted nowhere; it only seeds the watermark.
    //   * an arrival whose newest capture tick did NOT advance — see
    //     RelayArrivalWindowSummary::noAdvance.
    //   * an arrival whose newest capture tick went BACKWARDS. The relay stream is
    //     monotonic in capture tick by construction (the server relays only on
    //     `parked && acceptedNew`), so this should not happen; if it ever does, a
    //     signed-negative "gap" would be nonsense, so it is treated as a no-advance
    //     and the watermark is left alone rather than being rewound.
    //   * [T34 rework] an arrival whose gap EXCEEDS
    //     kRelayArrivalDiscontinuityTicks — a stall or an interruption, not
    //     `gap - 1` lost inputs. Counted in `discontinuities`, magnitude kept in
    //     `maxDiscontinuityGap`, and the watermark IS advanced so the next arrival
    //     measures from the far side of it.
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

        // ⭐ [T34 rework] THE DISCONTINUITY GUARD. See
        // kRelayArrivalDiscontinuityTicks for why 16 and why this is not optional:
        // without it a single stall reports ~355 per mille loss on a window whose
        // other 118 samples are perfect, against a pass condition of ~11.
        //
        // The watermark has already been advanced above, so the NEXT arrival
        // measures from the far side of the interruption rather than re-charging it.
        // This is a sample of nothing: no histogram bucket, no `maxGap`, no
        // `m_samples`, NONE of the three accumulators — and therefore it cannot
        // close a window either, which is deliberate. A window must close on 120
        // real samples.
        //
        // ⚠ [T34 loss-counter fix] It composes with the delivered count the only way
        // that is coherent: `newCaptureTicksDelivered` is discarded here TOO. A
        // discontinuous arrival may well carry a full burst, but the interval it
        // spans is not a contiguous capture stream, so neither its loss nor its
        // coverage is a statement about the wire. Crediting its delivered ticks
        // while not charging its gap would let an interruption IMPROVE the reported
        // rate — an instrument that reads healthier the worse the connection gets.
        if (gap > kRelayArrivalDiscontinuityTicks)
        {
            ++m_discontinuities;
            if (gap > m_maxDiscontinuityGap)
            {
                m_maxDiscontinuityGap = gap;
            }
            // `*outGapCaptureTicks` stays 0: its documented contract is "the gap
            // this arrival CONTRIBUTED", and this one contributed none. The
            // magnitude is not lost — it rides the window line as `discontMax=`.
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

        // [T34] THE R = 0 LOSS COUNTER. Accumulated on the same accepted-arrival
        // path as the histogram, so the two can never disagree about which samples
        // they cover. See RelayArrivalWindowSummary's block for why
        // `gap - delivered` IS the permanently-lost count and why `gap` is the right
        // denominator.
        //
        // ⛔ [T34 loss-counter fix] The clamp is not defensive tidiness: it is what
        // keeps the subtraction below an honest statement about the interval
        // (previousNewest, newestCaptureTick], which holds exactly `gap` ticks. See
        // the block above noteArrival.
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

    // The percentile the window WOULD report right now. Exposed so a test can
    // assert a distribution without having to fill an exact window.
    void peekSummary(RelayArrivalWindowSummary& out) const { fillSummary(out); }

    // [og-netcode-v2-input-relay item 91 part C] Mirrors `RelayReadProbe::
    // trackedOwnerCount()` above (this file, this class's PT sibling) — same
    // reason: a direct, no-window-required proof that `forgetOwner` actually
    // shrinks the id-keyed map, for a leak-freedom test that does not want to
    // depend on `noteArrival`'s first-vs-subsequent-arrival semantics as an
    // indirect signal.
    std::size_t trackedOwnerCount() const { return m_lastNewestCaptureTick.size(); }

private:
    // NEAREST-RANK. p_q is the smallest gap value whose cumulative count reaches
    // ceil(q * n) — the standard definition, chosen over interpolation because the
    // samples are integer tick counts and an interpolated 3.7-tick p99 would have
    // to be rounded up to 4 before it could be compared against `depth` anyway.
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
        // [T34 rework] Per-window, like FrameHealthProbe's: the field's job is to
        // tell an operator whether THIS window was interrupted, and a cumulative
        // count would mark every window after the first as suspect forever.
        m_discontinuities     = 0u;
        m_maxDiscontinuityGap = 0u;
        // The per-id watermark deliberately SURVIVES the window boundary: the gap
        // across a window edge is a real gap, and dropping the watermark would
        // silently discard one sample per component per window.
    }

    std::uint32_t m_windowSamples;

    // Newest capture tick seen per component. Bounded by live ids (forgetOwner).
    std::unordered_map<unsigned int, std::uint32_t> m_lastNewestCaptureTick;

    std::array<std::uint32_t, kGapOverflowBucket + 1u> m_buckets{};
    std::uint32_t m_samples          = 0u;
    std::uint32_t m_maxGap           = 0u;
    std::uint32_t m_noAdvance        = 0u;
    std::uint32_t m_saturatedSamples = 0u;

    // [T34] The R = 0 loss counter's accumulators. All reset with the window; the
    // per-id watermark that produces the gaps deliberately does not.
    // [T34 loss-counter fix] `m_deliveredCaptureTicks` is the third term, and the
    // invariant `lost + delivered == expected` is what makes the window checkable.
    std::uint32_t m_lostCaptureTicks      = 0u;
    std::uint32_t m_deliveredCaptureTicks = 0u;
    std::uint32_t m_expectedCaptureTicks  = 0u;

    // [T34 rework] The discontinuity guard's tally. See
    // kRelayArrivalDiscontinuityTicks.
    std::uint32_t m_discontinuities     = 0u;
    std::uint32_t m_maxDiscontinuityGap = 0u;
};

// ---------------------------------------------------------------------------
// [T20; renamed + extended to both roles T49] PROBE 4 — SIM TICKS PER
// GAME-THREAD FRAME, i.e. FRAME HEALTH.
//
// ORIGIN, SERVER-ONLY. This probe was built to measure the CAUSE of the quantity
// probe 2 measures the EFFECT of. Probe 2 reports `gapCaptureTicks` — how many
// capture ticks a client's relay ring skipped between two arrivals. This probe
// reports how many sim ticks a game thread advanced between two frames. On the
// SERVER the two are predicted to be the same number, and that prediction was the
// whole T20 experiment:
//
//   * UE property replication runs once per server GAME-THREAD FRAME, and
//     NetServerMaxTickRate is a CAP on that rate, never a floor.
//   * A 60 Hz async sim on a 30 fps server therefore advances TWO sim ticks per
//     replication, and a replace-latest ring at depth 1 can only carry the newer
//     one. `G ~ 60 / serverFrameRate` — every measured number in
//     RelayDepthCoverageHypothesis falls out of that one relation.
//   * So if ticks-per-frame p50 EQUALS the measured gap p50, G is a HOST
//     PERFORMANCE artefact (the observed sessions ran a dedicated server, two
//     clients and the editor on one machine) and there is no netcode defect. If
//     ticks-per-frame is 1 while the gap is still 2, replication is being skipped
//     for some other reason and the depth hypothesis is not the explanation.
//
// [T49] EXTENDED TO THE CLIENT. Nothing above is server-specific — it is a
// hook-independent ratio over two caller-supplied counters (see below) — and item
// 49 named the gap directly: `[RelayProbe.Frame]` was the ONLY wall-clock timing
// instrument anywhere in this netcode surface, and it was server-only by
// construction even though the server never resims and the client does. On a
// client this same ratio, plus its own `meanFrameMicros`/p99/max, is frame HEALTH
// under resim load — the quantity every client cost figure in
// `finding_task43_resim_gate_live.md` §4 had to be DERIVED from `ResimGateProbe`
// cadence instead of measured, because nothing sampled wall-clock time on that
// thread. The type was renamed from `ServerFrameProbe` (its T20 name) to
// `FrameHealthProbe` accordingly — nothing role-specific survives in its math, so
// nothing role-specific should survive in its name.
//
// TWO ROUTES TO A RATIO ABOVE 1, AND THEY ARE NOT THE SAME DEFECT. Chaos may run
// several fixed sub-steps inside one game frame (`numSteps > 1`), or the game
// thread may simply be slow. Both show up as ticks-per-frame > 1 and they need
// different fixes, so this probe reports the sub-step count ALONGSIDE the ratio and
// never collapses them. ON A RESIMMING CLIENT THIS SEPARATION IS THE WHOLE POINT:
// a resim burst folds extra sim ticks into `numSteps` at the SAME hook, so a ratio
// above 1 with `numStepsAboveOne > 0` reads as "resim ran", not "the game thread
// hitched" — the two client-cost questions item 49 exists to keep apart.
//
// HOOK-INDEPENDENT BY CONSTRUCTION. The caller supplies a GLOBAL frame counter, not
// an invocation count, so the ratio is correct no matter which game-thread hook
// feeds it and no matter how often that hook fires — the same property that let
// this probe move to a second role without a second implementation. The probe
// additionally reports how its own invocations distributed over frames —
// `dFrame == 1` (once per frame, the assumed case), `dFrame == 0` (fired more than
// once in a frame, i.e. per sub-step) and `dFrame > 1` (frames it did not fire on).
// THAT IS THE VERIFICATION THE TASK ASKS FOR: the hook's cadence is measured, never
// assumed — on EITHER role, from its own counters, independent of which hook the
// caller chose.
//
// GAME THREAD ONLY, on WHICHEVER ROLE the owning object runs on. Like the other two
// objects here it holds no lock and needs none — it is touched from exactly one
// thread. Each role gets its OWN instance (SimulationManagerUImpl's
// `m_frameHealthProbe` is a plain member, constructed once per actor, and both a
// server-role and a client-role actor exist as separate UObjects in the same
// process under PIE) — never shared, so there is no cross-role synchronization
// question either.
//
// TICK SOURCE IS THE CALLER'S PROBLEM, and there is only one right answer on the
// game thread: the Chaos tick mapper's atomic offset. The underlying clock (the
// server tick or the client's prediction tick) is written on the physics thread and
// must not be read here on either role. This probe takes a plain number and
// therefore cannot make that mistake on the caller's behalf — the call site carries
// the comment, including item 49's argument for why the SAME atomic read that was
// already proven safe for the server call site is equally safe for the client one.
// ---------------------------------------------------------------------------

// Exact buckets for 0..64 sim ticks per frame; anything larger saturates. 0 is a
// real observation here (a frame in which physics did not step), which is why the
// range starts at 0 rather than at 1 as the arrival histogram's does.
inline constexpr std::uint32_t kFrameHealthMaxTrackedTicks = 64u;
inline constexpr std::size_t   kFrameHealthOverflowBucket  =
    static_cast<std::size_t>(kFrameHealthMaxTrackedTicks) + 1u;

// One summary per this many samples. At an intended 60 fps that is ~2 s, the same
// feel as the other two windows; on a 30 fps server it is ~4 s, which is itself
// informative — the summaries thin out exactly when the measured thing is bad.
inline constexpr std::uint32_t kFrameHealthProbeWindowSamples = 120u;

// A sim-tick jump larger than this is a DISCONTINUITY (the mapper offset being
// established, a level transition, a multi-second editor stall), not a hitch.
// Sampling it would put a five-digit outlier in `max` and hide every real number
// behind it. Counted and reported, never silently dropped.
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
    // Cadence-independent: the totals are differences of the CALLER's own counters,
    // so this stays right even if the hook fired twice in a frame or skipped frames.
    // x100 to keep the log line integer — 250 means 2.50 sim ticks per frame.
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
    // Chaos's own count of fixed steps the frame will advance. numSteps > 1 is
    // SUB-STEPPING; a ratio above 1 with numSteps == 1 throughout is FRAME-RATE
    // SHORTFALL. Reported side by side so the two are never conflated.
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

    // Record one game-thread sample. Returns true — filling `out` and resetting the
    // window — when this sample completed a window.
    //
    //   frameCounter  a GLOBAL, monotonic frame number (UE's GFrameCounter). NOT an
    //                 invocation count: the whole hook-independence property rests
    //                 on this being the engine's own counter.
    //   simTick       the sim tick this frame is about to advance to, from a source
    //                 that is safe to read on the GAME thread.
    //   numSteps      Chaos's sub-step count for this frame.
    //   nowMicros     a monotonic wall-clock reading, for the mean frame time.
    bool noteFrame(std::uint64_t frameCounter,
                   std::uint32_t simTick,
                   std::uint32_t numSteps,
                   std::uint64_t nowMicros,
                   FrameHealthWindowSummary& out)
    {
        // The sub-step cross-check is per INVOCATION and is accumulated before any
        // of the frame-delta reasoning, so it stays correct whatever the cadence
        // turns out to be.
        m_totalNumSteps += numSteps;
        if (numSteps > m_maxNumSteps) { m_maxNumSteps = numSteps; }
        if (numSteps > 1u)            { ++m_numStepsAboveOne; }

        if (!m_seeded)
        {
            m_seeded = true;
            reseedAnchors(frameCounter, simTick, nowMicros);
            return false;
        }

        // A backwards or implausible jump is a discontinuity, not a measurement.
        // Re-seed from it rather than recording it.
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
    // counters. Exposed so a test can assert a distribution without filling a window.
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

    // NEAREST-RANK, and the bucket index IS the tick count — same definition as
    // RelayArrivalProbe::percentile, except that bucket 0 is a real observation.
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
