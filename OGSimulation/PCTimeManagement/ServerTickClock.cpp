// SPDX-License-Identifier: MPL-2.0
#include "ServerTickClock.h"

#include <cstdio>

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize( "", off )

ServerTickClock::ServerTickClock(float deltaSeconds, LoggerFn logger)
    : m_tick(0)
    , m_deltaSeconds(deltaSeconds)
    , m_logger(std::move(logger))
{
}

unsigned int ServerTickClock::getTick() const
{
    return m_tick;
}

void ServerTickClock::advanceTick()
{
    ++m_tick;
    // Per-tick "advanceTick" log silenced to reduce log noise during rework.
}

SimulationTimeStep ServerTickClock::getSimulationStep() const
{
    return SimulationTimeStep(m_tick, /*isResimulating=*/false, StepKind::Normal, m_deltaSeconds);
}

#pragma optimize( "", on )
// pragma optimize on.
