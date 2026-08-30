#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/OGExport.h"
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
// does it in `SimulationManagerUImpl.cpp`'s `RouteOGMessage`.
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
    // (ConnectionTierTable.h). Called on the CLIENT from the replicated-tier
    // OnRep handler — under Option A that OnRep delta is the only transition
    // signal the client has.
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
    // Resync callbacks
    // Called with the new predictionTick when a hard resync fires.
    // Same swap-with-back pattern as the existing system.
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

        // ⛔ The full contract and the fence test's name are at the seven members' declaration below.
        unsigned int skipCount()              const { return m_clock.m_skipCount; }
        unsigned int lastSkipTick()           const { return m_clock.m_lastSkipTick; }
        unsigned int stallCount()             const { return m_clock.m_stallCount; }
        unsigned int lastStallTick()          const { return m_clock.m_lastStallTick; }
        unsigned int hardResyncCount()        const { return m_clock.m_hardResyncCount; }
        unsigned int lastHardResyncFromTick() const { return m_clock.m_lastHardResyncFromTick; }
        unsigned int lastHardResyncToTick()   const { return m_clock.m_lastHardResyncToTick; }

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
            const unsigned int aboveFrontier = m_clock.m_predictionTick + seed + 1u;
            m_clock.m_skipCount              = seed + 1u;
            m_clock.m_lastSkipTick           = aboveFrontier;
            m_clock.m_stallCount             = seed + 2u;
            m_clock.m_lastStallTick          = aboveFrontier + 1u;
            m_clock.m_hardResyncCount        = seed + 3u;
            m_clock.m_lastHardResyncFromTick = aboveFrontier + 2u;
            m_clock.m_lastHardResyncToTick   = aboveFrontier + 3u;
        }

    private:
        ClientPredictionClock& m_clock;
    };

    Diagnostics        getDiagnostics()  const { return Diagnostics(*this); }
    MutableDiagnostics editDiagnostics()       { return MutableDiagnostics(*this); }

private:
    void fireResyncCallbacks(unsigned int newTick);

    const TimeConfig&           m_config;
    const NetworkTimeEstimator& m_estimator;

    unsigned int m_predictionTick           = 0;
    unsigned int m_resimulationTick         = 0;
    unsigned int m_gradualCorrectionCounter = 0;  // cycles 0 .. gradualCorrectionRate-1

    // [T11] Unpaid input-delay-increase stall, in ticks. Paid down one tick per
    // advancePrediction() call (as a Stall), cleared by a hard resync.
    unsigned int m_requiredInputDelayIncreaseStallTicks = 0;

    std::vector<ResyncCallback> m_resyncCallbacks;
    PCClockLoggerFn m_logger;

    // THE CLOCK'S EVENT SEAM - seven plain words, written PER EVENT on the physics thread and
    // read as `clock.getDiagnostics().skipCount()` and its six siblings.
    //
    // ⛔ DIAGNOSTIC ONLY: three write sites in `advancePrediction`, no production reader, no decision.
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
    //
    // ⛔ Gate state is NOT in this block: `m_predictionTick`, `m_gradualCorrectionCounter`,
    // `m_requiredInputDelayIncreaseStallTicks`.
    unsigned int m_skipCount              = 0;
    unsigned int m_lastSkipTick           = 0;
    unsigned int m_stallCount             = 0;
    unsigned int m_lastStallTick          = 0;
    unsigned int m_hardResyncCount        = 0;
    unsigned int m_lastHardResyncFromTick = 0;
    unsigned int m_lastHardResyncToTick   = 0;
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
