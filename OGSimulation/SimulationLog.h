#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdio>
#include <functional>

// Dev-only structured log helper shared by SimulationManager, SimulationNetSync,
// and SimulationReconciliation. Expands to a cheap null-pointer test when the
// logger is unset (tests, constructors before the manager wires loggers). At
// the UE instantiation site (ASimulationManagerUImpl::BeginPlay), the logger
// routes messages through UE_LOG(LogPCTM, ...). Prefix a message with
// "[Verbose]" or "[Warning]" to pick a non-default verbosity.
#define SIMLOG(logger, fmt, ...) \
    do { \
        if ((logger)) { \
            char _simlog_buf[256]; \
            std::snprintf(_simlog_buf, sizeof(_simlog_buf), (fmt) __VA_OPT__(,) __VA_ARGS__); \
            (logger)(_simlog_buf); \
        } \
    } while (0)

// Process-global logger sink — used by deeply-nested simulation templates that
// don't receive a logger via parameter plumbing (DAttackMachineSimulation,
// DAttackRadialSimulation integrate paths). Set once from the UE instantiation
// site. SIMLOG_G is a no-op if unset.
namespace simlog
{
    inline std::function<void(const char*)> g_sink;

    inline void setGlobal(std::function<void(const char*)> fn) { g_sink = std::move(fn); }
}

#define SIMLOG_G(fmt, ...) \
    do { \
        if (::simlog::g_sink) { \
            char _simlog_g_buf[256]; \
            std::snprintf(_simlog_g_buf, sizeof(_simlog_g_buf), (fmt) __VA_OPT__(,) __VA_ARGS__); \
            ::simlog::g_sink(_simlog_g_buf); \
        } \
    } while (0)
