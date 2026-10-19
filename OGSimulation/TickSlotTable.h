#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/TickSlotTable-rationale.md · docs/TickSlotTable-guards.md

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "OGSimulation/BodySlotAssignment.h"
#include "OGSimulation/BodySlotOccupancy.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/TickDriverLog.h"

template <typename E>
concept TickSlotLogTarget = requires(E& e, const E& ce, HostLogLevel level, const char* line)
{
	{ ce.logEnabled(level) } -> std::same_as<bool>;
	{ e.log(level, line) }   -> std::same_as<void>;
};

namespace tickSlotLogTargetSelfCheck
{
struct Conforming
{
	bool logEnabled(HostLogLevel) const;
	void log(HostLogLevel, const char*);
};

struct MutatingEnabled
{
	bool logEnabled(HostLogLevel);
	void log(HostLogLevel, const char*);
};

struct MissingLog
{
	bool logEnabled(HostLogLevel) const;
};

static_assert(TickSlotLogTarget<Conforming>,
	"TickSlotLogTarget self-check: the reference shape (TickDriverEngine's two log members) must satisfy the concept");
static_assert(!TickSlotLogTarget<MutatingEnabled>,
	"TickSlotLogTarget self-check: logEnabled must be callable on a const target");
static_assert(!TickSlotLogTarget<MissingLog>,
	"TickSlotLogTarget self-check: log(level, line) is required");
} // namespace tickSlotLogTargetSelfCheck

class TickSlotTable
{
public:
	static constexpr size_t kReservedLineChars = 256u;

	template <TickSlotLogTarget E>
	explicit TickSlotTable(E& logTarget)
		: m_logTarget(&logTarget)
		, m_logEnabled([](const void* target, HostLogLevel level) { return static_cast<const E*>(target)->logEnabled(level); })
		, m_log([](void* target, HostLogLevel level, const char* line) { static_cast<E*>(target)->log(level, line); })
	{
		m_line.reserve(kReservedLineChars);
	}

	std::optional<uint32_t> acquire(uint64_t key, const char* name, bool isAuthority, std::optional<uint32_t> replicatedSlot)
	{
		if (const auto found = m_holders.find(key); found != m_holders.end())
			return found->second.slot;

		std::array<bool, kMaxSimulatableSlots> taken{};
		for (const auto& entry : m_holders)
			taken[entry.second.slot] = true;
		const auto lowestFree = std::find(taken.begin(), taken.end(), false);

		// ⛔G-01  docs/TickSlotTable-guards.md
		const BodySlotDecision decision = decideBodySlot(isAuthority, replicatedSlot, taken);

		if (decision.outcome == BodySlotOutcome::WaitForReplication || decision.outcome == BodySlotOutcome::WaitForRelease)
		{
			const bool forReplication = decision.outcome == BodySlotOutcome::WaitForReplication;
			uint8_t& loggedWaits = m_waitsLoggedByOwner[key];
			const uint8_t waitBit = forReplication ? kReplicationWaitLogged : kReleaseWaitLogged;
			if ((loggedWaits & waitBit) == 0u)
			{
				loggedWaits |= waitBit;
				const char* heldBy = "-";
				for (const auto& entry : m_holders)
				{
					if (!forReplication && entry.second.slot == decision.slot)
						heldBy = entry.second.name.c_str();
				}
				if (m_logEnabled(m_logTarget, HostLogLevel::Log))
					m_log(m_logTarget, HostLogLevel::Log, formatHostLine(m_line, "[SimHost.Slot] wait=%s owner=%s slot=%d heldBy=%s",
						forReplication ? "WaitForReplication" : "WaitForRelease", name,
						forReplication ? -1 : static_cast<int32_t>(decision.slot), heldBy));
			}
			// ⛔G-02  docs/TickSlotTable-guards.md
			return std::nullopt;
		}

		OG_CHECK(decision.outcome == BodySlotOutcome::Assigned,
			"TickSlotTable::acquire: every slot is taken, so this owner cannot get one. The world has a fixed body set "
			"of kMaxSimulatableSlots characters. Was the Full checkf of the brawler's Jolt slot acquire.");
		if (decision.outcome != BodySlotOutcome::Assigned)
			return std::nullopt;

		m_holders.emplace(key, Holder{ decision.slot, std::string(name) });
		m_waitsLoggedByOwner.erase(key);

		if (m_logEnabled(m_logTarget, HostLogLevel::Log))
			m_log(m_logTarget, HostLogLevel::Log, formatHostLine(m_line, "[SimHost.Slot] assigned role=%s owner=%s slot=%u lowestFreeLocally=%d",
				isAuthority ? "Authority" : "Client", name, decision.slot,
				lowestFree != taken.end() ? static_cast<int32_t>(lowestFree - taken.begin()) : -1));
		return decision.slot;
	}

	std::optional<uint32_t> release(uint64_t key)
	{
		m_waitsLoggedByOwner.erase(key);

		const auto found = m_holders.find(key);
		if (found == m_holders.end())
			return std::nullopt;

		const uint32_t slot = found->second.slot;
		m_holders.erase(found);
		return slot;
	}

	std::optional<uint32_t> slotOf(uint64_t key) const
	{
		if (const auto found = m_holders.find(key); found != m_holders.end())
			return found->second.slot;
		return std::nullopt;
	}

	size_t size() const { return m_holders.size(); }

private:
	struct Holder
	{
		uint32_t slot;
		std::string name;
	};

	static constexpr uint8_t kReplicationWaitLogged = 1u;
	static constexpr uint8_t kReleaseWaitLogged = 2u;

	void* m_logTarget;
	bool (*m_logEnabled)(const void*, HostLogLevel);
	void (*m_log)(void*, HostLogLevel, const char*);
	HostLine m_line;
	std::unordered_map<uint64_t, Holder> m_holders;
	std::unordered_map<uint64_t, uint8_t> m_waitsLoggedByOwner;
};
