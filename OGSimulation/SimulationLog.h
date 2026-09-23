#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>

#include "OGSimulation/OGAssert.h"

// Dev-only structured log helper shared by SimulationManager, SimulationNetSync,
// and SimulationReconciliation. Expands to a cheap null-pointer test when the
// logger is unset (tests, constructors before the manager wires loggers). At
// the adapter's instantiation site the logger routes messages into the host's
// logging macro — one adapter's binding: `ASimulationManagerUImpl::BeginPlay`,
// routing through `UE_LOG(LogPCTM, ...)`. Prefix a message with "[Verbose]" or
// "[Warning]" to pick a non-default verbosity.
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
// DAttackRadialSimulation integrate paths). Set once from the adapter's
// instantiation site. SIMLOG_G is a no-op if unset.
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

// Ambient log context for the duration of ONE simulatable's integrate: the storage
// id being integrated and the tick of the step. SimulationIntegrationExecutor::
// integrateAll opens the scope immediately around each simulatable's integrate call,
// and that is the ONLY place it is opened. A host's log emitter may read it to
// attribute a line to its character and tick without the id being threaded through
// any simulation type, which must never read it: it is log context, not state.
//
// Outside every scope — firstResimStep, body-state capture, systems, game-thread
// code, a test that calls integrate directly — currentIntegrateScope() is nullopt,
// so nothing can print a wrong id.
//
// thread_local, not a plain inline global: integrate runs on the physics thread
// today, and an integrate on another thread must never read this thread's scope.
namespace simulationLog
{
    struct IntegrateScopeInfo
    {
        unsigned int id;
        uint32_t tick;
    };

    namespace detail
    {
        inline thread_local std::optional<IntegrateScopeInfo> t_integrateScope;
    }

    // RAII. Nesting is a programming error: one integrate call is one scope.
    struct IntegrateScope
    {
        IntegrateScope(unsigned int id, uint32_t tick)
        {
            OG_CHECK(!detail::t_integrateScope.has_value(),
                "simulationLog::IntegrateScope - a scope is already active on this thread; "
                "integrate scopes do not nest");
            detail::t_integrateScope = IntegrateScopeInfo{ id, tick };
        }
        ~IntegrateScope() { detail::t_integrateScope.reset(); }

        IntegrateScope(const IntegrateScope&) = delete;
        IntegrateScope& operator=(const IntegrateScope&) = delete;
    };

    inline std::optional<IntegrateScopeInfo> currentIntegrateScope() { return detail::t_integrateScope; }
}
