#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

// ---------------------------------------------------------------------------
// CorrectionVerdictProbe — the CLIENT-side prediction-versus-authority tally.
// (og-netcode-v2-input-relay T24; feeds T23 scenario 4.)
//
// EXPOSURE, NOT MEASUREMENT. This file adds NO comparison. The verdict it counts
// is `m_stateBuffer[i].isSimilarTo(state)`, which `StateCorrectionCache::
// tryInsertingCorrectState` has always computed on every correction, for every
// character, on every tick, and has always stored in `m_predictionWasCorrect`.
// What did not exist was any way to SEE it: the cache's log line for that event
// carries no tag, so it falls through RouteOGMessage to the `LogOG` fallback at
// `Log` severity, and `LogOG=Warning` suppresses it. The clean floor-0 run
// (RelayDepthCoverageHypothesis §9.11) contains ZERO occurrences of it. The
// signal was invisible by ROUTING, not absent.
//
// ⭐ WHAT THIS CAN AND CANNOT SHOW — read before drawing a conclusion from it.
// The same clean run produced only 8 and 17 resim triggers across ~45 s, i.e.
// roughly 0.2-0.4 per second. CORRECTIONS THAT DISAGREE ARE ALREADY RARE at
// floor 0. So a higher relay delay floor cannot demonstrate much improvement in
// the disagreement RATE — the rate has almost no room left to fall. If raising
// the floor helps, it helps by making the corrections that DO still occur
// SMALLER, and that is a MAGNITUDE, which nothing in this system records:
// `isSimilarTo` is a boolean fold over per-field tolerances and discards the
// distance it folded over. Magnitude is backlog T28 and is DEFERRED.
//
// Consequently a T23 finding of "the disagreement rate did not change" is
// UNINFORMATIVE rather than negative. This header says so because that is
// exactly the conclusion a future reader is most likely to misread, and the
// place they will be standing when they do is a log line this file produced.
//
// ---------------------------------------------------------------------------
// TWO CLASSES, COUNTED SEPARATELY, AND THAT SPLIT IS THE WHOLE POINT.
//
//   LocallyPredicted  this client owns the character's input. Its prediction is
//                     driven by the player's own captures through the client
//                     input delay line; the relay never carries them, because
//                     the server does not relay a character's input back to the
//                     client that produced it. RAISING THE RELAY DELAY FLOOR
//                     CANNOT MOVE THIS NUMBER except through second-order
//                     interaction effects.
//   RemoteProxy       this client predicts the character from relayed input.
//                     Its prediction quality is precisely what the scheduled
//                     regime is meant to improve, and it is the ONLY half whose
//                     movement would evidence T23 scenario 4(b).
//
// Pooled into one counter, a large and immobile local population would dilute
// whatever the small remote population does — in the tested 3-character
// topology by roughly a third — and the signal T23 needs would arrive already
// halved. So they are never summed.
//
// THE CLASS TEST IS PROVIDER-PRESENCE, and it is deliberately not a new one:
// `SimulationNetSync` decides local-vs-remote by looking up the id in
// `m_inputProviders` at `registerPredictionOwner` (the delay-line / relay-store
// fork) and again in `collectInputAll` (the provider / queue fork). This probe's
// caller reuses that same lookup. A second, independently-derived notion of
// "remote" is the thing most likely to disagree with the first one after a
// future edit, and then the two halves of this summary would describe different
// populations while looking authoritative.
//
// ---------------------------------------------------------------------------
// ONE OBJECT, ONE THREAD. Unlike RelayReadProbe/RelayArrivalProbe (T19), which
// had to split because their two call sites live on different threads, every
// correction reaches this probe from the SAME place: the OnRep-dispatched
// correction-state callback bound in `SimulationNetSync::registerPredictionOwner`,
// which runs on the GAME thread. There is no physics-thread feeder, so there is
// no window to tear and no split to make.
//
// NO PER-ID STATE, hence no `forgetOwner`. The summary is a per-CLASS aggregate
// across characters — that is what "corrections seen on remote proxies" means —
// so unregistering a character leaves nothing behind to erase. This is a real
// difference from the relay probes, whose per-id watermarks and fallback runs
// DO have to be dropped at `unregisterSimulatable`; do not copy that pattern
// here looking for symmetry.
//
// NEITHER DOES IT LOG. It accumulates and hands back a summary struct; the
// caller owns the logger and does the SIMLOG. Same convention as RelayReadProbe,
// `populateRelayedInputStore` and `RemoteMoveQueue` — and it is what lets the
// Low-Level Tests assert on numbers rather than on strings.
//
// ENGINE-AGNOSTIC. `<cstdint>` only — no UE types, no OGTypes, no other
// OGSimulation header. Testable from og-simulation-tests with no simulatable, no
// owner-traits specialization and no logger.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// ---------------------------------------------------------------------------

enum class PredictedCharacterClass : std::uint8_t
{
    LocallyPredicted = 0,
    RemoteProxy,
};

// One summary per this many corrections, counting BOTH classes.
//
// TOTAL-DRIVEN RATHER THAN PER-CLASS-DRIVEN, on purpose. If each class closed on
// its own count, the two summary lines would describe different and unstated
// time intervals — a rare-correction class would emit a line covering ten
// seconds next to one covering two, and comparing their rates would be comparing
// different sessions. Closing both on one shared counter makes the pair of lines
// a single observation of one interval, which is the form T23 has to read them
// in.
//
// 120 matches kRelayReadProbeWindowTicks / kRelayArrivalProbeWindowSamples so
// the probe families feel alike in a log. At the measured correction cadence
// (~35-60 per second per character, three characters) that is well under a
// second per window, i.e. ~2 Warning lines per second — three orders of
// magnitude below the volume class T19 was filed to fix.
//
// [T24] Free constant beside the probe rather than a TimeConfig field: it is a
// telemetry knob with no simulation meaning, and TimeConfig is the wrong place
// for a value that no integrator, clock or gate ever reads. This also mirrors
// RelayReadProbe.h exactly.
inline constexpr std::uint32_t kCorrectionVerdictProbeWindowSamples = 120u;

struct CorrectionVerdictClassSummary
{
    // Corrections that LANDED in a cache slot for this class in the window. A
    // correction whose tick had no slot is discarded by the cache and never
    // reaches this probe — it produced no verdict, so counting it here would put
    // a denominator under a comparison that never happened.
    std::uint32_t corrections = 0u;

    // Of those, the ones where the prediction did NOT match authority, i.e.
    // `isSimilarTo` returned false and the cache overwrote the slot.
    std::uint32_t disagreements = 0u;

    // disagreements / corrections, in PARTS PER THOUSAND, rounded to nearest.
    // Integer rather than a float so the number is exactly reproducible in a
    // test and in a log parser; per-mille rather than per-cent because the
    // interesting rates here are single-digit percentages and a percentage would
    // round most windows to 0 and throw the signal away.
    //
    // Zero when `corrections` is zero — no observation, not a perfect record.
    // The caller is expected to skip a class block with no corrections rather
    // than print `ratePerMille=0`, which would read as the opposite of "no data".
    std::uint32_t disagreementRatePerMille = 0u;
};

struct CorrectionVerdictWindowSummary
{
    // Corrections across BOTH classes — the window's own size. Always equals
    // local.corrections + remote.corrections; reported so a log line carries the
    // denominator of the class MIX as well as of each class's rate.
    std::uint32_t samples = 0u;

    CorrectionVerdictClassSummary local;
    CorrectionVerdictClassSummary remote;
};

// ---------------------------------------------------------------------------
// CorrectionVerdictProbe — GAME THREAD ONLY.
// ---------------------------------------------------------------------------
class CorrectionVerdictProbe
{
public:
    explicit CorrectionVerdictProbe(
        std::uint32_t windowSamples = kCorrectionVerdictProbeWindowSamples)
        : m_windowSamples(windowSamples == 0u
              ? kCorrectionVerdictProbeWindowSamples
              : windowSamples)
    {
    }

    // Record one LANDED correction's verdict. Returns true — filling
    // `outSummary` and resetting the window — when this sample completed a
    // window.
    //
    // Only landed corrections belong here; see
    // CorrectionVerdictClassSummary::corrections.
    bool noteCorrection(PredictedCharacterClass characterClass,
                        bool                    predictionWasCorrect,
                        CorrectionVerdictWindowSummary& outSummary)
    {
        ClassCounters& counters = countersFor(characterClass);

        ++counters.corrections;
        if (!predictionWasCorrect)
        {
            ++counters.disagreements;
        }

        ++m_samples;
        if (m_samples < m_windowSamples)
        {
            return false;
        }

        fillSummary(outSummary);
        resetWindow();
        return true;
    }

    // --- introspection; tests and diagnostics only -------------------------
    std::uint32_t sampleCount() const { return m_samples; }

    std::uint32_t correctionsFor(PredictedCharacterClass characterClass) const
    {
        return countersFor(characterClass).corrections;
    }

    std::uint32_t disagreementsFor(PredictedCharacterClass characterClass) const
    {
        return countersFor(characterClass).disagreements;
    }

    std::uint32_t windowSamples() const { return m_windowSamples; }

    // Snapshot of the window IN PROGRESS, without closing or resetting it. The
    // shipped code never calls this — it exists so a test (and a future
    // on-demand diagnostic) can read a partial window, which is the only way to
    // observe a rate in a session whose windows never complete.
    void fillSummary(CorrectionVerdictWindowSummary& outSummary) const
    {
        outSummary.samples = m_samples;
        fillClassSummary(m_local,  outSummary.local);
        fillClassSummary(m_remote, outSummary.remote);
    }

private:
    struct ClassCounters
    {
        std::uint32_t corrections   = 0u;
        std::uint32_t disagreements = 0u;
    };

    static void fillClassSummary(const ClassCounters& counters,
                                 CorrectionVerdictClassSummary& out)
    {
        out.corrections   = counters.corrections;
        out.disagreements = counters.disagreements;
        out.disagreementRatePerMille =
            (counters.corrections == 0u)
                ? 0u
                : static_cast<std::uint32_t>(
                      (static_cast<std::uint64_t>(counters.disagreements) * 1000u
                       + counters.corrections / 2u)
                      / counters.corrections);
    }

    ClassCounters& countersFor(PredictedCharacterClass characterClass)
    {
        return (characterClass == PredictedCharacterClass::RemoteProxy)
            ? m_remote
            : m_local;
    }

    const ClassCounters& countersFor(PredictedCharacterClass characterClass) const
    {
        return (characterClass == PredictedCharacterClass::RemoteProxy)
            ? m_remote
            : m_local;
    }

    void resetWindow()
    {
        m_local   = ClassCounters{};
        m_remote  = ClassCounters{};
        m_samples = 0u;
    }

    ClassCounters m_local;
    ClassCounters m_remote;

    std::uint32_t m_samples      = 0u;
    std::uint32_t m_windowSamples;
};

// Human-readable class name for a log line. Deliberately here rather than at the
// call site so the two spellings that end up in an operator's grep are defined
// once.
inline const char* predictedCharacterClassName(PredictedCharacterClass characterClass)
{
    return (characterClass == PredictedCharacterClass::RemoteProxy)
        ? "RemoteProxy"
        : "LocallyPredicted";
}
