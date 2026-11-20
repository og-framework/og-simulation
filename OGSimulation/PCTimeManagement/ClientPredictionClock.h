#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/OGExport.h"
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include "NetworkTimeEstimator.h"
#include "OGSimulation/SimulationTimeContext.h"
#include "TimeConfig.h"

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// THE CLOCK'S EVENT SEAM - seven plain words the clock owns as ONE member, written PER EVENT
// on the physics thread and read as `clock.getDiagnostics().skipCount()` and its six siblings.
//
// ⛔ DIAGNOSTIC ONLY: four write sites in `advancePrediction`, no production reader, no decision.
//
// ⭐ STRUCTURAL, not merely asserted: clock logic cannot name a word without `m_eventSeamDiagnostics.`.
//
// ⭐ MACHINE-CHECKED, by the case named here so it cannot be quietly dropped:
// `ClientPredictionClock.Events.TheEventCountersCannotReachAnyClockDecision`.
//
// That case garbage-fills these seven words mid-lifecycle and asserts every clock output
// byte-identical to an unscribbled run: every AdvanceResult, both cursors, `evaluateDrift`,
// the stall debt and the resync callback's ticks.
//
// ⛔ EVERY BRANCH WRITES ITS TICK BEFORE ITS COUNT - a count-then-tick reader tears one way only.
//
// ⚠ Both stall branches write, debt and graduated; instrumenting one under-counts silently.
//
// ⛔ Nothing here reaches the wire, the correction payload or `compute_checksum`.
struct EventSeamDiagnostics
{
    unsigned int skipCount              = 0;
    unsigned int lastSkipTick           = 0;
    unsigned int stallCount             = 0;
    unsigned int lastStallTick          = 0;
    unsigned int hardResyncCount        = 0;
    unsigned int lastHardResyncFromTick = 0;
    unsigned int lastHardResyncToTick   = 0;
};

// ClientPredictionClock — client-only tick counter with graduated drift correction
// and resimulation cursor.
//
// Replaces PredictedAndCorrectionSimulationTimeManager.  The caller just calls
// advancePrediction() each non-resim physics step; drift correction is applied
// internally so the caller never needs to know about it.
//
// Lifetime contract: m_config and m_estimator must outlive this object
// (both owned by SimulationManager).
class ClientPredictionClock
{
public:
// Injected logger type. Pass nullptr (the default) to disable all logging.
// Messages carry a level prefix: "[Log] ..." or "[Warning] ..."
// The OGSimulation module is engine-independent; an adapter bridges this callback
// to the host application's log via a lambda passed at construction. One adapter
// does it in `OGBrawlerSimulationManager.cpp`'s `RouteOGMessage`.
using PCClockLoggerFn = std::function<void(const char*)>;

    OGSIMULATION_API ClientPredictionClock(const TimeConfig& config, const NetworkTimeEstimator& estimator, PCClockLoggerFn logger);

    // -----------------------------------------------------------------------
    // Prediction
    // -----------------------------------------------------------------------

    // Result of a single advancePrediction() call.
    enum class AdvanceResult { Normal, Skip, Stall, HardResync };

    // Advance the prediction tick by one (plus any drift correction).
    // Returns what kind of advance occurred so callers can annotate the step.
    // Also advances the resimulation tick when not resimulating
    // (m_resimulationTick == m_predictionTick → advance both, preserving the
    // "resim cursor stays at the frontier" invariant).
    OGSIMULATION_API AdvanceResult advancePrediction();

    OGSIMULATION_API unsigned int getPredictionTick()  const;
    OGSIMULATION_API SimulationTimeStep getPredictionStep() const;

    // -----------------------------------------------------------------------
    // Input-delay-increase stall (T11, D5.3)
    //
    // NOTE ON THE NAME: this mechanism never resimulates anything — it only
    // declines to advance the prediction frontier for N calls (an AdvanceResult
    // Stall). It is unrelated to the resimulation-depth machinery in TimeConfig,
    // which owns the codebase's other vocabulary for "give ticks back".
    // -----------------------------------------------------------------------

    // Register the effective-input-delay change produced by an authoritative
    // RTT tier transition, as reported by `tierDelayDeltaTicks(oldTier, newTier)`
    // (ConnectionTierTable.h). Called on the CLIENT when the connection tier
    // arrives (og-sim-net's ConnectionTier message, through
    // UESimulationSession::onConnectionTierReceived) — under Option A that tier
    // delta is the only tier-transition signal the client has. The relay-delay
    // floor's arrival calls it too.
    //
    // WHY A STALL AT ALL. An upward transition RAISES the effective input
    // delay: an input captured at tick T now lands at T + newDelay instead of
    // T + oldDelay. Every already-predicted tick ahead of the frontier was
    // computed against the OLD, smaller delay, so the frontier now sits
    // `delta` ticks further ahead of where the new delay says it should. The
    // client must give those ticks back.
    //
    // POSITIVE DELTAS ONLY. `deltaDelayTicks <= 0` is a no-op: a DOWNWARD
    // transition lets the client predict FURTHER ahead, which the ordinary
    // drift path reaches on its own by advancing normally. There is nothing to
    // undo, so forcing a forward jump here would be an unnecessary
    // discontinuity.
    //
    // ACCUMULATES. Consecutive upward transitions add their deltas: two
    // unpaid transitions owe the sum, never just the most recent one.
    OGSIMULATION_API void requestInputDelayIncreaseStall(int32_t deltaDelayTicks);

    // Ticks of stall still owed. Nonzero means advancePrediction() will
    // return Stall on the next call(s) instead of applying ordinary drift
    // correction. Exposed for tests and telemetry.
    OGSIMULATION_API unsigned int getRequiredInputDelayIncreaseStallTicks() const;

    // -----------------------------------------------------------------------
    // Resimulation
    // -----------------------------------------------------------------------

    // Begin a resimulation pass: set the resimulation cursor to 'tick'.
    OGSIMULATION_API void         startResimulation(unsigned int tick);

    // Advance the resimulation cursor by one.
    OGSIMULATION_API void         advanceResimulation();

    // Mark resimulation as finished: move cursor back to the prediction frontier.
    OGSIMULATION_API void         finishResimulation();

    OGSIMULATION_API unsigned int getResimulationTick()  const;
    OGSIMULATION_API SimulationTimeStep getResimulationStep() const;

    // True if the resimulation cursor is behind the prediction frontier.
    OGSIMULATION_API bool         isResimulating() const;

    // -----------------------------------------------------------------------
    // Drift evaluation (pure query — no side effects)
    // -----------------------------------------------------------------------

    enum class DriftAction { None, Skip, Stall, HardResync };

    // Returns the drift action that advancePrediction() *would* apply right now.
    // Does not mutate any state.
    OGSIMULATION_API DriftAction evaluateDrift() const;

    // -----------------------------------------------------------------------
    // One-shot re-centre (og-input-latency task 27; TimeConfig::recentreSettleSamples)
    //
    // Armed at construction and by every hard resync. Once the clock has made
    // recentreSettleSamples advances with an RTT reading since the arm (a time
    // window, og-input-latency P3-11) and the estimator has taken
    // kRecentreMinSamples measured samples since it (P3-12), the next
    // advancePrediction() past the warm-up guard decides ONCE and logs one
    // [Clock.Recentre] line. It owes N graduated corrections, N the rounded
    // median lead error of the last kRecentreMedianAdvances advances, and pays
    // them one per gradualCorrectionRate advances, inside the dead band too;
    // no later reading ends the debt, a hard resync clears it. Then the dead
    // band applies again, until the RTT-only offset (the prediction offset
    // without its jitter term, floored at predOffsetFloorTicks as it is) has
    // stood kRecentreRearmTicks or more from its value at the decision on every
    // one of the last kRecentreMedianAdvances advances: that re-arms it
    // (reworks 2-4). The decision itself still aims at the full prediction offset.
    // recentreSettleSamples == 0 never decides.
    // Rationale: docs/TimeConfig-rationale.md §14.
    // -----------------------------------------------------------------------

    static constexpr unsigned int kRecentreMedianAdvances = 15u;
    static_assert(kRecentreMedianAdvances % 2u == 1u,
        "ClientPredictionClock: the re-centre's median run must be odd, so a full run's median is one reading "
        "(og-input-latency task 27 rework 1, review_27 F3)");

    static constexpr unsigned int kRecentreRearmTicks = 2u;
    static_assert(kRecentreRearmTicks >= 2u,
        "ClientPredictionClock: a re-arm on a 1-tick offset move would chase the offset's ceil() wobble on a steady "
        "link (og-input-latency task 27 rework 2, Ph3 R10)");

    // [P3-12] The settle window's sample floor. Rationale: docs/TimeConfig-rationale.md §14.
    static constexpr unsigned int kRecentreMinSamples = 8u;
    static_assert(kRecentreMinSamples <= TimeConfig{}.recentreSettleSamples,
        "ClientPredictionClock: the re-centre's sample floor must not exceed the default window's advances, so at one "
        "reading per advance (the engine source) the advances still bind and the floor binds only when advances outrun "
        "time, as in a catch-up batch (og-input-latency P3-12, Ph4 gate R3: joins decided on 1-6 samples)");

    // True while a re-centre is armed and has not yet decided.
    OGSIMULATION_API bool isRecentreArmed() const;

    // True while a re-centre still owes corrections.
    OGSIMULATION_API bool isRecentring() const;

    // -----------------------------------------------------------------------
    // Resync callbacks
    // Called with the new predictionTick when a hard resync fires, in
    // registration order.
    // Ids are STABLE: each registration gets the next value of a monotonic
    // counter, and unregistering one id leaves every other id valid. (Ids used
    // to be vector indices with a swap-with-back unregister, which silently
    // re-indexed the moved callback, so a later unregister of that id removed
    // the wrong one.) Unregistering an unknown id is a no-op.
    // -----------------------------------------------------------------------

    using ResyncCallback = std::function<void(unsigned int newTick)>;
    static constexpr unsigned int InvalidCallbackId = std::numeric_limits<unsigned int>::max();

    OGSIMULATION_API unsigned int registerResyncCallback(ResyncCallback cb);
    OGSIMULATION_API void         unregisterResyncCallback(unsigned int id);

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------

    // ⛔ The grouping rule these follow lives in `docs/DiagnosticsConventions.md`; not re-derived here.
    //
    // ⛔ THE VIEW'S TYPE IS THE MARKER: members carry no `get` prefix and no `Diagnostic` infix.
    class Diagnostics
    {
    public:
        explicit Diagnostics(const ClientPredictionClock& clock) : m_clock(clock) {}

        // ⛔ The full contract and the fence test's name are at `EventSeamDiagnostics`.
        unsigned int skipCount()              const { return m_clock.m_eventSeamDiagnostics.skipCount; }
        unsigned int lastSkipTick()           const { return m_clock.m_eventSeamDiagnostics.lastSkipTick; }
        unsigned int stallCount()             const { return m_clock.m_eventSeamDiagnostics.stallCount; }
        unsigned int lastStallTick()          const { return m_clock.m_eventSeamDiagnostics.lastStallTick; }
        unsigned int hardResyncCount()        const { return m_clock.m_eventSeamDiagnostics.hardResyncCount; }
        unsigned int lastHardResyncFromTick() const { return m_clock.m_eventSeamDiagnostics.lastHardResyncFromTick; }
        unsigned int lastHardResyncToTick()   const { return m_clock.m_eventSeamDiagnostics.lastHardResyncToTick; }

    private:
        const ClientPredictionClock& m_clock;
    };

    class MutableDiagnostics
    {
    public:
        explicit MutableDiagnostics(ClientPredictionClock& clock) : m_clock(clock) {}

        // ⛔ THE FENCE'S OWN INSTRUMENT: it has no production caller and must never acquire one.
        //
        // Every word takes a distinct non-zero value that moves with the seed, and all four tick
        // words land ABOVE the prediction frontier, a value no real event can leave behind.
        void scribbleEventCountersForFenceTest(unsigned int seed)
        {
            EventSeamDiagnostics& seam       = m_clock.m_eventSeamDiagnostics;
            const unsigned int aboveFrontier = m_clock.m_predictionTick + seed + 1u;
            seam.skipCount                   = seed + 1u;
            seam.lastSkipTick                = aboveFrontier;
            seam.stallCount                  = seed + 2u;
            seam.lastStallTick               = aboveFrontier + 1u;
            seam.hardResyncCount             = seed + 3u;
            seam.lastHardResyncFromTick      = aboveFrontier + 2u;
            seam.lastHardResyncToTick        = aboveFrontier + 3u;
        }

    private:
        ClientPredictionClock& m_clock;
    };

    Diagnostics        getDiagnostics()  const { return Diagnostics(*this); }
    MutableDiagnostics editDiagnostics()       { return MutableDiagnostics(*this); }

private:
    void fireResyncCallbacks(unsigned int newTick);

    // [P3-10] One advance's estimator read (readTarget) and the resync onto the first
    // RTT reading's target. See advancePrediction.
    struct TargetRead
    {
        bool         hasFirstRtt     = false;  // the flag, read once
        bool         firstRttSettled = false;  // ... and already true on the previous advance
        unsigned int offsetTicks     = 0;
        unsigned int targetTick      = 0;
    };
    TargetRead readTarget() const;
    bool firstRttResyncDue(const TargetRead& estimate, bool pastGuard, int drift) const;

    // [task 27] The re-centre's pieces. With recentreSettleSamples == 0 the
    // window is never full, so the decision never fires and nothing corrects.
    // A reading is the lead over the authority tick, stored UNCORRECTED (as if
    // the clock had taken no Skip or Stall since the arm), so the clock's own
    // corrections inside the run do not move its median.
    // [P3-11] The window is time: it counts the advances since the arm on which
    // the estimator had a reading (hasFirstRtt), not the samples it took.
    enum class RecentreCause { Join, Resync, Rearm };

    bool recentreWindowFull(unsigned int windowAdvances) const;
    void countRecentreWindow(bool hasReading);
    void armRecentre(RecentreCause cause, unsigned int frontierTick);
    unsigned int recentreRttOffsetTicks() const;
    bool recentreOffsetMoved() const;
    bool recentreRearmDue() const;
    void trackRecentreRearm(bool pastGuard);
    void recordRecentreReading(bool pastGuard);
    int  recentreCorrectionTicks(unsigned int uncorrectedTick, const int* extraReading) const;
    int  recentrePendingTicks(bool hasReading) const;
    void decideRecentre(unsigned int targetTick);

    const TimeConfig&           m_config;
    const NetworkTimeEstimator& m_estimator;

    unsigned int m_predictionTick           = 0;
    unsigned int m_resimulationTick         = 0;
    unsigned int m_gradualCorrectionCounter = 0;  // cycles 0 .. gradualCorrectionRate-1

    // [T11] Unpaid input-delay-increase stall, in ticks. Paid down one tick per
    // advancePrediction() call (as a Stall), cleared by a hard resync.
    unsigned int m_requiredInputDelayIncreaseStallTicks = 0;

    // [P3-10] Set by a hard resync that landed on the floor offset with no settled RTT
    // reading (with the re-centre on), with the offset it landed on; cleared on the
    // first advance past the guard whose reading is settled. The flag as the last
    // advance read it: a reading is settled once two advances in a row read it.
    bool         m_resyncAwaitsFirstRtt    = false;
    unsigned int m_resyncOffsetTicks       = 0;
    bool         m_firstRttSeenLastAdvance = false;

    // [task 27] The one-shot re-centre: armed at construction and by a hard
    // resync, with the estimator's measured-sample count at that moment.
    bool         m_recentreArmed       = true;
    bool         m_recentreAhead       = false;  // the debt's direction: true = client ahead, Stall
    unsigned int m_recentreOwedTicks   = 0;      // corrections still owed; only a correction or a hard resync lowers it
    unsigned int m_recentreBaseSamples = 0;      // for the line's samples= field
    unsigned int m_recentreWindowAdvances = 0;   // [P3-11] advances with a reading since the arm
    unsigned int m_recentreUncorrectedTick = 0;  // the frontier had every advance since the arm been Normal
    std::array<int, kRecentreMedianAdvances> m_recentreReadings{};
    unsigned int m_recentreReadingCount = 0;
    unsigned int m_recentreReadingNext  = 0;

    // [task 27 reworks 2-4] The re-arm: the floored RTT-only offset at the last decision, and how
    // many advances in a row it has stood kRecentreRearmTicks or more from that.
    RecentreCause m_recentreCause          = RecentreCause::Join;
    bool          m_recentreDecidedOnce    = false;
    unsigned int  m_recentreDecisionRttOffset = 0;
    unsigned int  m_recentreRearmRun       = 0;
    unsigned int  m_recentreRearmFromOffset = 0;  // the decision's RTT-only offset, for the re-armed decision's line
    unsigned int  m_recentreRearmToOffset   = 0;  // the RTT-only offset when it re-armed

    struct RegisteredResyncCallback
    {
        unsigned int   id;
        ResyncCallback callback;
    };
    std::vector<RegisteredResyncCallback> m_resyncCallbacks;
    unsigned int                          m_nextResyncCallbackId = 0;
    PCClockLoggerFn m_logger;

    // ⛔ THE EVENT SEAM, whole and behind its own type; the contract is at `EventSeamDiagnostics`.
    EventSeamDiagnostics m_eventSeamDiagnostics;
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
