#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/OGExport.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>

#include "TimeConfig.h"

#pragma optimize( "", off )

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
//                       Called from OnRep_TimingInfo (UObject replication).
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
// The OGSimulation module is engine-independent; the TestYo layer bridges this
// to UE_LOG via a lambda passed at construction.
using LoggerFn = std::function<void(const char*)>;

    OGSIMULATION_API explicit NetworkTimeEstimator(const TimeConfig& config, LoggerFn logger);

    // -----------------------------------------------------------------------
    // Mutators — called from game thread (OnRep_TimingInfo)
    // -----------------------------------------------------------------------

    // Feed a new raw RTT sample (seconds).  Updates smoothed RTT and jitter.
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

    // Number of ticks added on top of authorityTick to form the target.
    OGSIMULATION_API unsigned int  getPredictionOffsetTicks() const;

private:
    const TimeConfig& m_config;

    std::atomic<unsigned int> m_authorityTick{0}; // GT writes, PT reads — explicit atomic
    double m_smoothedRTT          = 0.0;
    double m_smoothedJitter       = 0.0;
    bool   m_hasFirstSample       = false;
    LoggerFn m_logger;
};

#pragma optimize( "", on )
