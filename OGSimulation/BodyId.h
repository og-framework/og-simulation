#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <compare>
#include "OGSimulation/SimulationSerialization.h"
#include "OGSimulation/SimulationFieldDescriptors.h"

struct BodyId
{
	uint32_t value = 0;
	bool operator==(const BodyId&) const = default;
	auto operator<=>(const BodyId&) const = default;
};

template <>
struct SerializableFields<BodyId>
{
	static constexpr auto get()
	{
		return std::make_tuple(
			SIM_MEMBER(BodyId, value));
	}
};
