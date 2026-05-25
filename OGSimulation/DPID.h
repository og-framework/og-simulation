#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGExport.h"
#include <algorithm>

class DPIDState
{
public:
	OGSIMULATION_API DPIDState();

	float getPreviousError() const { return m_previousError; }
	float getIntegral() const { return m_integral; }
	float getAdjustment() const { return m_adjustment; }

	void setPreviousError(float previousError) { m_previousError = previousError; }
	void setIntegral(float integral) { m_integral = integral; }
	void setAdjustment(float adjustment) { m_adjustment = adjustment; }

private:
	float m_previousError = 0.f;
	float m_integral = 0.f;
	float m_adjustment = 0.f;
};

class DPIDSettings
{
public:
 	OGSIMULATION_API DPIDSettings(float p, float i, float d);
	float getMaxAdjustment() const { return m_maxAdjustment; }
	float getP() const { return m_p; }
	float getI() const { return m_i; }
	float getD() const { return m_d; }

private:
	DPIDSettings() = delete;

	const float m_maxAdjustment;
	const float m_p;
	const float m_i;
	const float m_d;
};

namespace dPID
{
	OGSIMULATION_API void update(float targetValue, float currentValue, float deltaTime, const DPIDSettings& settings, DPIDState& state);
}

