#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <cstdint>
#include <functional>

#include "OGSimulation/PCTimeManagement/TimeConfig.h"
#include "OGSimulation/SimulationLog.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize("", off)

// ---------------------------------------------------------------------------
// Desync diagnostic sink boundary (proposal §2.4 / D3.8).
//
// Layer: OGSimulation. Engine-agnostic — no UE, no Chaos, no game types.
//
// This header ships the BOUNDARY only. There is deliberately NO production
// consumer yet: the hash-broadcast wire hookup (the OnRep handler that compares
// the authoritative checksum against the local one and fires these callbacks)
// belongs to the Stage 4 observability initiative. Landing the interface first
// means Stage 4 wires a broadcast into an already-tested seam instead of
// inventing the seam and the transport in the same change.
//
// Split from SimulationReconciliation.h (the task named that file as the
// alternative) for three reasons:
//   1. SimulationReconciliation.h declares one variadic class template plus its
//      concept; these are non-template plain types with a different lifetime and
//      no dependency on the reconciliation template parameters.
//   2. It would drag TimeConfig.h into SimulationReconciliation.h's include
//      graph purely for the threshold read — SimulationReconciliation.h has no
//      TimeConfig dependency today and does not need one.
//   3. tools/lint/configurability_lint.ps1 already pre-allows this exact
//      filename for hashMismatchTickThreshold / hashMismatchReaction (added by
//      task 1), i.e. the sibling-header layout is what the lint scaffold was
//      built to expect.
//
// Header-only (no .cpp) — matching CorrectionCache.h / SimulationReconciliation.h
// in this same directory. Both the standalone CMake build and the UE module glob
// this directory, so nothing needs registering; and keeping the type header-only
// avoids exporting a class with std::atomic members across the DLL boundary.
// ---------------------------------------------------------------------------

// One desync observation. Produced by whatever compares a local state checksum
// against an authoritative one; consumed by an IDesyncDiagnosticSink.
struct DesyncDiagnosticEvent
{
    // Simulation tick the two checksums describe.
    int32_t  tick                    = 0;

    // Checksum computed locally for `tick`.
    uint32_t localChecksum           = 0;

    // Authoritative checksum for `tick`. 0 when the diagnostic is local-only
    // (no remote checksum was available to compare against).
    uint32_t remoteChecksum          = 0;

    // Length of the current unbroken run of mismatching ticks, INCLUDING this
    // one. A run of 1 is routine in-flight ordering noise; a sustained run is
    // what `shouldEscalateToLayer2` turns into a confirmed divergence.
    int32_t  consecutiveMismatchRun  = 0;
};

// Diagnostic sink boundary. Implementations decide what a desync observation
// MEANS (log it, ship it to telemetry, snapshot a replay reproducer, drop the
// client); the detection path only decides that one happened.
//
// Two distinct signals, deliberately not collapsed into one:
//   onHashMismatch        — every mismatching tick, including transient noise.
//   onConfirmedDivergence — fired once a run crosses the configured threshold,
//                           i.e. the mismatch is no longer plausibly transient.
class IDesyncDiagnosticSink
{
public:
    virtual void onHashMismatch(const DesyncDiagnosticEvent& ev) = 0;
    virtual void onConfirmedDivergence(const DesyncDiagnosticEvent& ev) = 0;
    virtual ~IDesyncDiagnosticSink() = default;
};

// Default sink: records what happened and logs it. Never mutates simulation
// state, never disconnects anyone — safe to install unconditionally, which is
// why it is the default.
//
// The injected-logger shape mirrors NetworkTimeEstimator exactly: the same
// `LoggerFn = std::function<void(const char*)>` alias, taken by value at
// construction and moved into the member, null meaning "logging disabled". The
// OGSimulation layer is engine-independent; the UE instantiation site passes a
// lambda that routes into UE_LOG, and the tests pass a capturing lambda.
//
// Threading: the counters are std::atomic because the detection path may run on
// the physics thread while a test or debug UI reads the counts from the game
// thread. relaxed ordering is sufficient — these are standalone tallies, they
// publish no other state.
class LogOnlyDesyncDiagnosticSink final : public IDesyncDiagnosticSink
{
public:
    // Injected logger type. Pass nullptr to disable all logging.
    // Messages carry a level prefix: "[Warning] ..."
    using LoggerFn = std::function<void(const char*)>;

    explicit LogOnlyDesyncDiagnosticSink(LoggerFn logger)
        : m_logger(std::move(logger))
    {}

    void onHashMismatch(const DesyncDiagnosticEvent& ev) override
    {
        m_hashMismatchCount.fetch_add(1, std::memory_order_relaxed);

        SIMLOG(m_logger,
            "[Warning] Desync.HashMismatch tick=%d local=0x%08x remote=0x%08x run=%d",
            ev.tick, ev.localChecksum, ev.remoteChecksum, ev.consecutiveMismatchRun);
    }

    void onConfirmedDivergence(const DesyncDiagnosticEvent& ev) override
    {
        m_confirmedDivergenceCount.fetch_add(1, std::memory_order_relaxed);

        SIMLOG(m_logger,
            "[Warning] Desync.ConfirmedDivergence tick=%d local=0x%08x remote=0x%08x run=%d",
            ev.tick, ev.localChecksum, ev.remoteChecksum, ev.consecutiveMismatchRun);
    }

    // Test-observable state. Also useful from a debug HUD.
    int32_t getHashMismatchCount() const
    {
        return m_hashMismatchCount.load(std::memory_order_relaxed);
    }

    int32_t getConfirmedDivergenceCount() const
    {
        return m_confirmedDivergenceCount.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int32_t> m_hashMismatchCount{0};
    std::atomic<int32_t> m_confirmedDivergenceCount{0};
    LoggerFn             m_logger;
};

// THE single place `hashMismatchTickThreshold` is consulted.
//
// Every consumer that wants to know "has this run of mismatches become a
// confirmed divergence?" must ask through here rather than comparing against
// cfg.hashMismatchTickThreshold itself. One call site for the comparison means
// one place to change if the escalation rule ever grows a second input (a time
// window, a per-simulatable weighting), and it keeps the >= -vs- > boundary
// from being re-litigated at each consumer.
//
// Boundary semantics: escalate ON reaching the threshold, not after exceeding
// it — a run of exactly `hashMismatchTickThreshold` ticks IS a confirmed
// divergence. With the default of 5 that means the 5th consecutive mismatching
// tick escalates.
//
// Note: this helper answers WHETHER to escalate. It deliberately does not read
// cfg.hashMismatchReaction — that field selects WHAT the consumer does about a
// confirmed divergence (log / fall back to layer 2 / disconnect), which is the
// consumer's policy call and has no consumer until Stage 4.
inline bool shouldEscalateToLayer2(int32_t consecutiveMismatchRun, const TimeConfig& cfg)
{
    return consecutiveMismatchRun >= cfg.hashMismatchTickThreshold;
}

#pragma optimize("", on)
// pragma optimize on.
