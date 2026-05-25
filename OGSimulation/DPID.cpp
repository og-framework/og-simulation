// SPDX-License-Identifier: MPL-2.0
#include "DPID.h"
#include "OGAssert.h"
#include <algorithm>
#include "glm/geometric.hpp"


DPIDState::DPIDState()
	: m_previousError(0.f)
	, m_integral(0.f)
{
}

DPIDSettings::DPIDSettings(float p, float i, float d)
	: m_p(p)
	, m_i(i)
	, m_d(d)
	, m_maxAdjustment(1337.f)
{
	OG_CHECK(p >= 0.f, "p must be non-negative");
	OG_CHECK(i >= 0.f, "i must be non-negative");
	OG_CHECK(d >= 0.f, "d must be non-negative");
}

namespace dPID
{
	void update(float targetValue, float currentValue, float deltaTime, const DPIDSettings& settings, DPIDState& state)
	{
		const float error = targetValue - currentValue;

		const float p = settings.getP();
		const float i = settings.getI();
		const float d = settings.getD();

		state.setIntegral(state.getIntegral() + error * deltaTime);
		state.setIntegral(std::clamp(state.getIntegral(), -1.f, 1.f));
		const float derivative = (error - state.getPreviousError()) / deltaTime;
		state.setPreviousError(error);

		const float adjustment = p * error + i * state.getIntegral() + d * derivative;
		state.setAdjustment(std::clamp(adjustment, -settings.getMaxAdjustment(), settings.getMaxAdjustment()));
	}
}
