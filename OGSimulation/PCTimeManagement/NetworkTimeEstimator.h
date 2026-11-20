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
//   Game thread (GT)  — writes via: updateRTT(), recordAuthorityTick(), and
//                       since og-input-latency P3-10 noteEngineRttSample(),
//                       noteSimClockSample() and noteRttSourceChanged(), which
//                       must run on that same writer thread.
//                       Called from the adapter's clock arrival callback, on
//                       the game thread. One adapter binds that to
//                       `UESimulationSession::onTimingInfoReceived`, fed since
//                       og-input-latency task P5-7 by og-sim-net's
//                       `SimNetClient::drain` in the frame-locked drain the
//                       og-net driver runs at the end of its `PostTickDispatch`
//                       (by `ASimulationTimingRelay::OnRep_Buffer` before;
//                       deleted by P5-9). Since og-input-latency task 27 that
//                       drain hands over at most one clock sample, the newest.
//                       That is og-sim-net's FRAME-LOCKED mode
//                       (`SimNetConfig::immediate` false, the shipped default).
//                       In its IMMEDIATE mode (og-input-latency P2-15) the
//                       host calls `SimNetClient::drain` at the start of each
//                       step, on the step's thread, so the one writer is the
//                       PT below and the GT/PT crossing becomes same-thread.
//                       The atomics stay: the frame-locked mode keeps the GT
//                       writer, and a game-thread display read is then the
//                       crossing they synchronize.
//
//   Physics thread (PT) — reads via: hasFirstRTTSample(), then
//                          getPredictionOffsetTicks() and getLastAuthorityTick(),
//                          which the clock sums into its target (since P3-10
//                          rework 1, in place of getTargetPredictionTick());
//                          getMeasuredRTTSampleCount(), getLastAuthorityTick() and
//                          getPredictionOffsetTicks() directly for the clock's re-centre.
//                          Called inside ClientPredictionClock::advancePrediction().
//                          The other accessors read the same members and are
//                          safe from either thread.
//
// m_authorityTick, m_smoothedRTT, m_smoothedJitter, m_hasFirstSample,
// m_measuredRTTSamples and (since P3-13, for the offset's per-source jitter
// multiplier) m_activeRttSource are all std::atomic, with ONE writer (GT) and
// memory_order_relaxed on every access. Every crossing read is therefore correctly synchronized on every
// platform — no data race, no torn value — not only on x86-64, where a relaxed
// atomic compiles to the same plain load and store the old fields used.
//
// Each member is coherent on its own; the four are NOT a snapshot. A PT read
// may combine values written by different writer calls (e.g. the first-sample
// flag already true while the seed is not yet visible). The worst outcome is
// one frame's offset computed from that mix (the floor, in the flag example),
// superseded by the next read. That is accepted: the estimate is data.
// The one reader that acts once on the flag, ClientPredictionClock's second
// resync (P3-10), reads it once per advance and decides only on an advance
// whose previous advance already read it true, so a mixed read is superseded
// for it too (ThreadingCrossings.md row 10).
//
// The static_assert on std::atomic<double> below replaces the old caveat
// "if this class is ever ported to ARM ... wrap these fields in
// std::atomic<double>": a platform without lock-free 8-byte atomics now
// fails to compile instead of silently taking a lock on the PT.
// ============================================================

// One SimClock ping/pong, in seconds: the RTT with the server's turnaround removed,
// the NTP offset theta, and the four stamps (t0/t3 on the client's clock, t1/t2 on
// the server's). og-sim names no og-sim-net type: the host copies its sample in.
// Rationale: docs/TimeConfig-rationale.md §15.
struct SimClockRttSample
{
    double rttSeconds   = 0.0;
    double thetaSeconds = 0.0;
    double t0Seconds    = 0.0;
    double t1Seconds    = 0.0;
    double t2Seconds    = 0.0;
    double t3Seconds    = 0.0;
};

// The SimClock shadow, the [ClockSource.Window] line's reading. §15
struct ClockSourceShadow
{
    unsigned int samples            = 0;    // valid SimClock samples so far
    unsigned int invalidSamples     = 0;    // non-finite stamps or an RTT not above 0, ignored
    double       simClockRttSeconds = 0.0;  // EMA of the SimClock RTT at rttSmoothingAlpha, no gate
    double       offsetSeconds      = 0.0;  // theta of the minimum-RTT sample of the last kClockOffsetSamples
    double       owdVarUpSeconds    = 0.0;  // RFC 3550 interarrival jitter of t1 - t0
    double       owdVarDownSeconds  = 0.0;  // RFC 3550 interarrival jitter of t3 - t2
};

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
    // Mutators — called from the game thread in the frame-locked mode, from the step in the
    // immediate mode (see the THREAD-SAFETY CONTRACT above)
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
    //
    // A reading of exactly 0 before the first sample above 0 is VALID but does
    // not seed: it latches nothing and moves nothing (one ping source reads 0
    // before it has measured anything; og-input-latency P5-28). After a seed a
    // 0 is an ordinary low reading.
    OGSIMULATION_API void updateRTT(double rawRTTSeconds);

    // -----------------------------------------------------------------------
    // The clock RTT source (og-input-latency P3-10; TimeConfig::clockRttSource).
    // Same thread as updateRTT. Rationale: docs/TimeConfig-rationale.md §15.
    //
    // The active source starts as Engine. An engine reading reaches updateRTT
    // only while Engine is active. A valid SimClock sample always feeds the
    // shadow; with clockRttSource == SimClock the first one switches the active
    // source (noteRttSourceChanged) and from then on every one reaches updateRTT.
    // -----------------------------------------------------------------------
    OGSIMULATION_API void noteEngineRttSample(double rawRTTSeconds);
    OGSIMULATION_API void noteSimClockSample(const SimClockRttSample& sample);

    // The next sample is the new source's first. NEVER a reset: the estimate,
    // the first-sample flag and the counts stay. That sample moves the RTT EMA
    // but not the jitter EMA, and if it reads above the estimate (and under the
    // cold-start ceiling) the plausibility gate is re-based on it, so a step up
    // does not run into the escape hatch's re-seed. A no-op before the first seed.
    OGSIMULATION_API void noteRttSourceChanged();

    OGSIMULATION_API TimeConfig::ClockRttSource getActiveRttSource() const;

    // The SimClock shadow. Read it on the writer's thread (plain members).
    OGSIMULATION_API ClockSourceShadow getClockSourceShadow() const;

    // NTP's clock-filter register (RFC 5905 §10): the offset is the minimum-RTT
    // sample's theta among this many.
    static constexpr unsigned int kClockOffsetSamples = 8u;

    // Store the latest tick received from the server.
    OGSIMULATION_API void recordAuthorityTick(unsigned int serverTick);

    // -----------------------------------------------------------------------
    // Accessors — called by ClientPredictionClock / SimulationManager
    // -----------------------------------------------------------------------

    // authorityTick + ceil((smoothedRTT + m * smoothedJitter) * tickFrequency), m the active
    // source's multiplier: jitterMultiplier (Engine) or simClockJitterMultiplier (SimClock).
    OGSIMULATION_API unsigned int  getTargetPredictionTick()  const;

    OGSIMULATION_API unsigned int  getLastAuthorityTick()     const;
    OGSIMULATION_API double        getSmoothedRTT()           const;
    OGSIMULATION_API double        getSmoothedJitter()        const;

    // Has any VALID RTT sample above 0 ever landed (a 0 does not seed, P5-28;
    // an outlier escape's re-seed counts)? False means the offset accessors
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

    // Running total of MEASURED samples: past updateRTT's validity gate (plausible
    // or not) and above 0. A 0 reading is counted out because it carries no
    // measurement: one adapter's ping source (an averaged RoundTrip) reads 0 until
    // its first averaging bucket fills, about a second into the connection.
    // ClientPredictionClock reads it on the PT to count its re-centre's settle
    // window (og-input-latency task 27).
    OGSIMULATION_API unsigned int  getMeasuredRTTSampleCount() const;

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

    static_assert(std::atomic<double>::is_always_lock_free,
        "NetworkTimeEstimator: needs lock-free double atomics for its GT->PT estimate. Was the "
        "THREAD-SAFETY CONTRACT's ARM caveat: 'if ported to ARM, wrap these fields in std::atomic<double>'");

    // GT writes, PT reads — relaxed atomics, one writer (see the contract above).
    std::atomic<unsigned int> m_authorityTick{0};
    std::atomic<double>       m_smoothedRTT{0.0};
    std::atomic<double>       m_smoothedJitter{0.0};
    std::atomic<bool>         m_hasFirstSample{false};
    std::atomic<unsigned int> m_measuredRTTSamples{0};

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

    // P3-10 source-switch bookkeeping, writer-thread only like the counters above,
    // except the active source: since P3-13 the offset reads it (each source has its
    // own jitter multiplier), so it is a relaxed atomic with the estimate's one writer.
    std::atomic<TimeConfig::ClockRttSource> m_activeRttSource{TimeConfig::ClockRttSource::Engine};
    static_assert(std::atomic<TimeConfig::ClockRttSource>::is_always_lock_free,
        "NetworkTimeEstimator: the offset reads the active RTT source on the PT (P3-13), so it must be a lock-free atomic.");
    bool         m_rttSourceChangePending = false; // the next sample is the new source's first
    bool         m_skipNextJitterUpdate   = false; // that sample's distance is the bias step, not jitter
    double       m_gateRebaseRTTSeconds   = 0.0;   // 0 = not re-based; else the gate's reference floor
    unsigned int m_gateRebaseSamplesLeft  = 0;     // the re-base lapses after rttOutlierConsecutiveLimit samples

    // P3-10 SimClock shadow, writer-thread only.
    struct ShadowOffsetSample
    {
        double rttSeconds;
        double thetaSeconds;
    };
    ClockSourceShadow m_shadow;
    ShadowOffsetSample m_shadowOffsetRing[kClockOffsetSamples] = {};
    unsigned int      m_shadowOffsetNext   = 0;
    double            m_shadowLastUpTransit   = 0.0;
    double            m_shadowLastDownTransit = 0.0;

    LoggerFn m_logger;

    // P3-10 rework 1: a reader's view of a GT write landing mid-read. Empty in shipped
    // code: only this friend sets it, and it is declared here and defined ONLY in
    // og-simulation-tests (the LocalInputCacheTearProbe pattern), so it adds no surface
    // a consumer can call. hasFirstRTTSample() runs it after its load. ThreadingCrossings.md row 10.
    friend struct NetworkTimeEstimatorReadProbe;
    std::function<void()> m_afterFirstSampleLoad;
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
