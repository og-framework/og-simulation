#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/TickDriverGameTypes-rationale.md

#include <cstdint>

#include "OGSimulation/SimulationScheduler.h"

class TickRateDoor
{
public:
	TickRateDoor(SimulationScheduler& scheduler, int64_t tickOffset, bool refusesRateChange, uint64_t& refusedRateChanges)
		: m_scheduler(&scheduler)
		, m_refusedRateChanges(&refusedRateChanges)
		, m_tickOffset(tickOffset)
		, m_refusesRateChange(refusesRateChange)
	{
	}

	void setRateScale(double scale)
	{
		if (m_refusesRateChange)
		{
			++*m_refusedRateChanges;
			return;
		}
		m_scheduler->setRateScale(scale);
	}

	double rateScale() const { return m_scheduler->rateScale(); }
	LostTime lostTime() const { return m_scheduler->lostTime(); }
	uint64_t physicsStepCount() const { return m_scheduler->physicsStepCount(); }
	double nextDeadline() const { return m_scheduler->nextDeadline(); }
	double currentInterval() const { return m_scheduler->currentStepInterval(); }
	int64_t tickOffset() const { return m_tickOffset; }
	uint64_t refusedRateChanges() const { return *m_refusedRateChanges; }

private:
	SimulationScheduler* m_scheduler;
	uint64_t*            m_refusedRateChanges;
	int64_t              m_tickOffset;
	bool                 m_refusesRateChange;
};

enum class IdleWake : uint8_t
{
	Deadline,
	Activity,
	Unavailable,
};
