#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/BodySlotOccupancy-rationale.md

#include <bitset>
#include <cstdint>

inline constexpr uint32_t kMaxSimulatableSlots = 8;

struct BodySlotOccupancy
{
	std::bitset<kMaxSimulatableSlots> occupied;

	bool operator==(const BodySlotOccupancy&) const = default;
};
