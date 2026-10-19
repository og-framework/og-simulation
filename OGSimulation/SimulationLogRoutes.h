#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationLogRoutes-rationale.md · docs/SimulationLogRoutes-guards.md

#include "OGSimulation/TickDriverLog.h"

#include <cstddef>
#include <cstdint>

enum class SimulationLogRoute : uint8_t
{
	SimTick,
	Sim,
	Net,
	DivergenceProbe,
};

struct SimulationLogRouteEntry
{
	const char*        prefix;
	SimulationLogRoute route;
	HostLogLevel       plainLevel;
};

inline constexpr SimulationLogRouteEntry kSimulationLogRoutes[] = {
	{ "[Resim.Input]",                  SimulationLogRoute::SimTick,         HostLogLevel::Verbose },
	{ "[TimeResync.",                   SimulationLogRoute::Sim,             HostLogLevel::Log },
	{ "[Resim.",                        SimulationLogRoute::Sim,             HostLogLevel::Log },
	{ "[ResimCheck.Divergence]",        SimulationLogRoute::Sim,             HostLogLevel::Log },
	{ "[ResimCheck.PrepareRestore]",    SimulationLogRoute::Sim,             HostLogLevel::Log },
	{ "[ResimCheck.Check]",             SimulationLogRoute::SimTick,         HostLogLevel::Verbose },
	{ "[ResimCheck.IsSimilar]",         SimulationLogRoute::SimTick,         HostLogLevel::Verbose },
	{ "[AuthoritySimulation]",          SimulationLogRoute::SimTick,         HostLogLevel::Verbose },
	{ "[PredictionSimulation]",         SimulationLogRoute::SimTick,         HostLogLevel::Verbose },
	{ "[PostPrediction]",               SimulationLogRoute::SimTick,         HostLogLevel::Verbose },
	{ "[CollectInput]",                 SimulationLogRoute::SimTick,         HostLogLevel::Verbose },
	{ "[ReceiveLocalInput] DISCARD",    SimulationLogRoute::Net,             HostLogLevel::Log },
	{ "[ReceiveLocalInput]",            SimulationLogRoute::Net,             HostLogLevel::Verbose },
	{ "[SendCorrectionStateToClients]", SimulationLogRoute::Net,             HostLogLevel::Verbose },
	{ "[SendLocalInputToServer]",       SimulationLogRoute::Net,             HostLogLevel::Verbose },
	{ "[InjectCorrectionState]",        SimulationLogRoute::Net,             HostLogLevel::Verbose },
	{ "[InputGap]",                     SimulationLogRoute::Net,             HostLogLevel::Log },
	{ "[InputDrop]",                    SimulationLogRoute::Net,             HostLogLevel::Log },
	{ "[DelayShift]",                   SimulationLogRoute::Net,             HostLogLevel::Log },
	{ "[InputStats]",                   SimulationLogRoute::Net,             HostLogLevel::Log },
	{ "[Park]",                         SimulationLogRoute::Net,             HostLogLevel::Verbose },
	{ "[Release]",                      SimulationLogRoute::Net,             HostLogLevel::Verbose },
	{ "[InputDomain]",                  SimulationLogRoute::Net,             HostLogLevel::Log },
	{ "[RelaySkip]",                    SimulationLogRoute::Net,             HostLogLevel::Log },
	{ "[DivergenceProbe",               SimulationLogRoute::DivergenceProbe, HostLogLevel::Log }
};

namespace simulationLogRoutes
{
constexpr char foldCase(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool startsWithIgnoringCase(const char* text, const char* prefix)
{
	for (; *prefix != 0; ++text, ++prefix)
	{
		if (*text == 0 || foldCase(*text) != foldCase(*prefix))
			return false;
	}
	return true;
}

template <std::size_t RowCount>
constexpr bool allReachable(const SimulationLogRouteEntry (&rows)[RowCount])
{
	for (std::size_t later = 0; later < RowCount; ++later)
	{
		for (std::size_t earlier = 0; earlier < later; ++earlier)
		{
			if (startsWithIgnoringCase(rows[later].prefix, rows[earlier].prefix))
				return false;
		}
	}
	return true;
}

static_assert(allReachable(kSimulationLogRoutes),
	"kSimulationLogRoutes: a route's prefix BEGINS WITH an earlier route's prefix, so the earlier "
	"route always wins and the later one can never be taken. A host matches prefixes ignoring "
	"case and so does this check. Was the ORDER IS LOAD-BEARING fence: [Resim.Input] must be "
	"matched ahead of the [Resim. catch-all, and widening that catch-all to [Resim would "
	"swallow every [ResimCheck. line.");

template <std::size_t RowCount>
constexpr bool plainLevelsAreLogOrVerbose(const SimulationLogRouteEntry (&rows)[RowCount])
{
	for (const SimulationLogRouteEntry& entry : rows)
	{
		if (entry.plainLevel != HostLogLevel::Log && entry.plainLevel != HostLogLevel::Verbose)
			return false;
	}
	return true;
}

static_assert(plainLevelsAreLogOrVerbose(kSimulationLogRoutes),
	"kSimulationLogRoutes: a row's plainLevel is the level of a line that carries no [Verbose] or "
	"[Warning] token, and it may only be Log (rare events) or Verbose (per tick, per packet, "
	"per relay). A line that must reach Warning says so with its own [Warning] token.");
} // namespace simulationLogRoutes
