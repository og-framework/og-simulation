// SPDX-License-Identifier: MPL-2.0
#include "SimulationTimeContext.h"
#include <algorithm>
#include "glm/geometric.hpp"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize( "", off )


AuthoritySimulationTimeManager::AuthoritySimulationTimeManager()
	: m_currentTick(0.f)
{

}

SimulationTimeStep AuthoritySimulationTimeManager::getAuthoritySimulationStep() const
{
	return SimulationTimeStep(m_currentTick, false);
}

PredictedAndCorrectionSimulationTimeManager::PredictedAndCorrectionSimulationTimeManager(double tickFrequency)
	: m_predictionTick(0)
	, m_resimulationTick(0)
	, m_authorityTick(0)
	, m_roundTripTime(0.0)
	, m_tickFrequency(tickFrequency)
{
}

void PredictedAndCorrectionSimulationTimeManager::recordAuthorityTick(uint32 tick)
{ 
	m_authorityTick = tick;

	const uint32 expectedPredictionTick = getExpectedPredictionTick();

	if (expectedPredictionTick > m_predictionTick + 15) //fast forward
	{
		m_predictionTick = expectedPredictionTick;
		for (const auto& callback : m_onPredictionResyncWithAuthority)
			callback(expectedPredictionTick);
	}
	else if (m_predictionTick > 60 && tick > 60 && expectedPredictionTick < m_predictionTick - 15) //rewind
	{
		m_predictionTick = expectedPredictionTick;
		for (const auto& callback : m_onPredictionResyncWithAuthority)
			callback(expectedPredictionTick);
	}

}

uint32 PredictedAndCorrectionSimulationTimeManager::getExpectedPredictionTick()
{
	return m_authorityTick + static_cast<uint32>(glm::ceil((m_roundTripTime)*m_tickFrequency));
}


SimulationTimeStep PredictedAndCorrectionSimulationTimeManager::getPredictionSimulationStep() const
{
	return SimulationTimeStep(m_predictionTick, false);
}

SimulationTimeStep PredictedAndCorrectionSimulationTimeManager::getReSimulationStep() const
{
	return SimulationTimeStep(m_resimulationTick, true);
}

#pragma optimize( "", on )
// pragma optimize on.
