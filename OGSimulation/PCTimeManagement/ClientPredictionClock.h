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

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize( "", off )

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
// The OGSimulation module is engine-independent; the TestYo layer bridges this
// to UE_LOG via a lambda passed at construction.
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
    // Tier-transition rollback (T11, D5.3)
    // -----------------------------------------------------------------------

    // Register the effective-input-delay change produced by an authoritative
    // RTT tier transition, as reported by `tierDelayDeltaTicks(oldTier, newTier)`
    // (ConnectionTierTable.h). Called on the CLIENT from the replicated-tier
    // OnRep handler — under Option A that OnRep delta is the only transition
    // signal the client has.
    //
    // WHY A ROLLBACK AT ALL. An upward transition RAISES the effective input
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
    OGSIMULATION_API void requestTierTransitionRollback(int32_t deltaDelayTicks);

    // Ticks of rollback still owed. Nonzero means advancePrediction() will
    // return Stall on the next call(s) instead of applying ordinary drift
    // correction. Exposed for tests and telemetry.
    OGSIMULATION_API unsigned int getPendingTierRollbackTicks() const;

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

private:
    void fireResyncCallbacks(unsigned int newTick);

    const TimeConfig&           m_config;
    const NetworkTimeEstimator& m_estimator;

    unsigned int m_predictionTick           = 0;
    unsigned int m_resimulationTick         = 0;
    unsigned int m_gradualCorrectionCounter = 0;  // cycles 0 .. gradualCorrectionRate-1

    // [T11] Unpaid tier-transition rollback, in ticks. Paid down one tick per
    // advancePrediction() call (as a Stall), cleared by a hard resync.
    unsigned int m_pendingRollbackTicks = 0;

    std::vector<ResyncCallback> m_resyncCallbacks;
    PCClockLoggerFn m_logger;
};

#pragma optimize( "", on )
// pragma optimize on.
