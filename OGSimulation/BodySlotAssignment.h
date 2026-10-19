#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/BodySlotAssignment-rationale.md

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

#include "OGSimulation/BodySlotOccupancy.h"
#include "OGSimulation/OGAssert.h"

enum class BodySlotOutcome : uint8_t
{
	Assigned,
	WaitForReplication,
	WaitForRelease,
	Full
};

struct BodySlotDecision
{
	BodySlotOutcome outcome;
	uint32_t slot;

	bool operator==(const BodySlotDecision&) const = default;
};

inline BodySlotDecision decideBodySlot(bool isAuthority,
                                       std::optional<uint32_t> replicatedSlot,
                                       const std::array<bool, kMaxSimulatableSlots>& takenLocally)
{
	if (isAuthority)
	{
		const auto freeSlot = std::find(takenLocally.begin(), takenLocally.end(), false);
		if (freeSlot == takenLocally.end())
			return { BodySlotOutcome::Full, 0u };
		return { BodySlotOutcome::Assigned, static_cast<uint32_t>(freeSlot - takenLocally.begin()) };
	}

	if (!replicatedSlot.has_value())
		return { BodySlotOutcome::WaitForReplication, 0u };

	if (*replicatedSlot >= kMaxSimulatableSlots)
	{
		OG_CHECK(false, "decideBodySlot: the replicated slot is not a slot of this world (>= kMaxSimulatableSlots)");
		return { BodySlotOutcome::WaitForReplication, 0u };
	}

	if (takenLocally[*replicatedSlot])
		return { BodySlotOutcome::WaitForRelease, *replicatedSlot };

	return { BodySlotOutcome::Assigned, *replicatedSlot };
}
