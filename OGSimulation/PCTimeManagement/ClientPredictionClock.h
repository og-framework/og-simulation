#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/OGExport.h"
#include <functional>
#include <limits>
#include <vector>

#include "NetworkTimeEstimator.h"
#include "OGSimulation/SimulationTimeContext.h"
#include "TimeConfig.h"

// pragma optimize off — debugger-friendliness across all build configs (breakpoints hit,
// locals visible, call-stack intact). OGSim-core convention.
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

    std::vector<ResyncCallback> m_resyncCallbacks;
    PCClockLoggerFn m_logger;
};

#pragma optimize( "", on )
// pragma optimize on — restore command-line optimization settings.
