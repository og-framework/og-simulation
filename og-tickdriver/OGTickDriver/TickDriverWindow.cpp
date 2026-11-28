// SPDX-License-Identifier: MPL-2.0
// docs/TickDriverWindow-rationale.md · docs/TickDriverWindow-guards.md

#include "TickDriverWindow.h"

#include "OGSimulation/LatencyBudgetProbe.h"

#include <algorithm>

TickDriverPercentiles tickDriverPercentilesOf(float* values, uint32_t count)
{
	TickDriverPercentiles result;
	result.n = count;
	if (result.n == 0u)
		return result;

	std::sort(values, values + count);
	result.p50 = values[latencyBudget::nearestRankIndex(result.n, 500u)];
	result.p99 = values[latencyBudget::nearestRankIndex(result.n, 990u)];
	result.max = values[count - 1u];
	return result;
}
