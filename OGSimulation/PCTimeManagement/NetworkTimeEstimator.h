#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/OGExport.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>

#include "TimeConfig.h"

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// NetworkTimeEstimator — client-only network estimation component.
// Tracks smoothed RTT and jitter via EMA, stores the last known authority tick,
// and computes the target prediction tick that ClientPredictionClock should aim for.
//
// ============================================================
// THREAD-SAFETY CONTRACT
// ============================================================
// This class is accessed from two threads without a mutex:
//
//   Game thread (GT)  — writes via: updateRTT(), recordAuthorityTick()
//                       Called from the adapter's timing-buffer arrival callback,
//                       on the thread replication delivers on. One adapter binds
//                       that to `ASimulationManagerUImpl::onTimingInfoReceived`,
//                       fed from `ASimulationTimingRelay::OnRep_Buffer`.
//
//   Physics thread (PT) — reads via: getTargetPredictionTick(), getLastAuthorityTick(),
//                          getSmoothedRTT(), getSmoothedJitter(), getPredictionOffsetTicks()
//                          Called inside ClientPredictionClock::advancePrediction().
//
// m_authorityTick is declared std::atomic<unsigned int> so that PT reads of
// the server tick are always coherent, regardless of platform.
//
// m_smoothedRTT and m_smoothedJitter are plain double.  On x86-64, naturally-
// aligned 8-byte stores and loads are single-bus-cycle operations and cannot
// tear.  This holds for MSVC/Clang on Windows x64 (UE's primary PT target).
// If this class is ever ported to ARM or compiled with unaligned-access enabled,
// wrap these fields in std::atomic<double> as well.
//
// m_hasFirstSample is written exactly once (GT, on the first updateRTT call)
// and thereafter only read (PT).  On x86-64 this is safe; on weaker memory
// models a one-time publish fence or atomic<bool> would be needed.
// ============================================================
class NetworkTimeEstimator
{
public:
// Injected logger type. Pass nullptr (the default) to disable all logging.
// Messages carry a level prefix: "[Verbose] ..."
// The OGSimulation module is engine-independent; the adapter layer bridges this
// to the engine's own logging macro via a lambda passed at construction.
using LoggerFn = std::function<void(const char*)>;

    OGSIMULATION_API explicit NetworkTimeEstimator(const TimeConfig& config, LoggerFn logger);

    // -----------------------------------------------------------------------
    // Mutators — called from the game thread (see the THREAD-SAFETY CONTRACT above)
    // -----------------------------------------------------------------------

    // Feed a new raw RTT sample (seconds).  Updates smoothed RTT and jitter.
    //
    // TOTAL — callers may pass the engine's "no reading" sentinel straight
    // through. A sample that is negative (the ping source's "not set" reading is
    // -1.0 when the ping type is disabled or unsampled; the implementation names
    // one adapter's symbol for it), NaN or infinite is REJECTED
    // outright: it does not latch, does not move either EMA, and does not set
    // the first-sample flag. It is counted and logged ONCE at [Warning].
    // Rationale — including why rejecting beats clamping, and the live defect
    // this closed — is in the implementation (og-netcode-v2-input-relay T21).
    //
    // A VALID sample is additionally screened for PLAUSIBILITY against the
    // current estimate (T26b): a frame hitch inflates UE's frame-start-based RTT
    // measurement by orders of magnitude, and such samples are rejected rather
    // than believed. Sustained implausible samples — a genuine step change in
    // network conditions — force a re-seed after
    // `rttOutlierConsecutiveLimit` of them, so the filter can always follow the
    // network. Full derivation at the gate in the implementation.
    OGSIMULATION_API void updateRTT(double rawRTTSeconds);

    // Store the latest tick received from the server.
    OGSIMULATION_API void recordAuthorityTick(unsigned int serverTick);

    // -----------------------------------------------------------------------
    // Accessors — called by ClientPredictionClock / SimulationManager
    // -----------------------------------------------------------------------

    // authorityTick + ceil((smoothedRTT + jitterMultiplier * smoothedJitter) * tickFrequency)
    OGSIMULATION_API unsigned int  getTargetPredictionTick()  const;

    OGSIMULATION_API unsigned int  getLastAuthorityTick()     const;
    OGSIMULATION_API double        getSmoothedRTT()           const;
    OGSIMULATION_API double        getSmoothedJitter()        const;

    // Has any VALID RTT sample ever landed? False means the offset accessors
    // are reporting the configured floor and nothing else — the distinction
    // that the pre-T21 silent-zero defect erased. (T21.)
    //
    // Since T26a the floor IS what the offset accessors report while this is
    // false, so the flag is the ONLY way to tell "floored because no estimate
    // exists" from "floored because the estimate is below the floor". That is
    // deliberate: the floor is a structural invariant, not an estimate, and the
    // honesty requirement is served by the log rather than by a wrong offset.
    OGSIMULATION_API bool          hasFirstRTTSample()        const;

    // Running total of samples rejected by updateRTT's validity gate. Non-zero
    // on a healthy session only during warm-up; sustained growth means the
    // engine ping source is misconfigured. Paired with the one-shot warning so
    // the magnitude is recoverable after the log line has scrolled. (T21.)
    OGSIMULATION_API unsigned int  getRejectedRTTSampleCount() const;

    // Running total of VALID samples rejected as implausible by the T26b
    // outlier gate. Distinct from getRejectedRTTSampleCount() above, which
    // counts samples the ENGINE said do not exist: these are well-formed
    // readings the ESTIMATOR judged to be frame-hitch artifacts. Keeping the two
    // separate is what lets a PIE trace tell "the ping source is broken" from
    // "the game thread is hitching".
    OGSIMULATION_API unsigned int  getOutlierRejectedSampleCount() const;

    // How many times the outlier gate's escape hatch fired — i.e. how many times
    // a sustained run of implausible samples was judged to be a GENUINE step
    // change and forced a re-seed. A session with rejections but no escapes saw
    // transients; a session with escapes saw the network actually move. This is
    // the counter that resolves the gate's central ambiguity.
    OGSIMULATION_API unsigned int  getOutlierEscapeCount() const;

    // Number of ticks added on top of authorityTick to form the target.
    OGSIMULATION_API unsigned int  getPredictionOffsetTicks() const;

private:
    // True when `rawRTTSeconds` is plausible given the current estimate. Cold
    // start (no first sample yet) uses the absolute ceiling; afterwards the
    // multiplicative-plus-margin bound. One-sided by design — see the gate.
    bool isPlausibleRTTSample(double rawRTTSeconds) const;

    // Window bookkeeping for the outlier gate's per-window summary line. Emits
    // at most one [Warning][RttSample.Outlier] per full window, and only when
    // that window contained a rejection or an escape.
    void noteOutlierWindowSample();

    const TimeConfig& m_config;

    std::atomic<unsigned int> m_authorityTick{0}; // GT writes, PT reads — explicit atomic
    double m_smoothedRTT          = 0.0;
    double m_smoothedJitter       = 0.0;
    bool   m_hasFirstSample       = false;

    // T21 validity-gate bookkeeping. GT-only, like every other non-atomic
    // member here: both are touched exclusively inside updateRTT (a GT writer
    // per the contract above). The accessors are diagnostics — a torn read of
    // either would misreport a count, never corrupt the estimator.
    unsigned int m_rejectedRTTSamples = 0;
    bool         m_rejectedRTTLogged  = false;

    // T26b outlier-gate bookkeeping. GT-only on exactly the same terms as the
    // T21 counters above: every one of these is touched solely inside updateRTT
    // and its two private helpers, which only updateRTT calls. The accessors are
    // diagnostics — a torn read would misreport a count, never corrupt the
    // estimator.
    unsigned int m_outlierRejectedSamples = 0; // running total, all windows
    unsigned int m_outlierEscapes         = 0; // running total of forced re-seeds
    unsigned int m_consecutiveOutliers    = 0; // run length feeding the escape hatch
    unsigned int m_outlierWindowSamples   = 0; // samples seen in the current window
    unsigned int m_outlierWindowRejects   = 0; // rejections in the current window
    unsigned int m_outlierWindowEscapes   = 0; // escapes in the current window

    LoggerFn m_logger;
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
