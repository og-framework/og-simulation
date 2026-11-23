#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationDisplaySnapshot-rationale.md · docs/SimulationDisplaySnapshot-guards.md

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "OGSimulation/Mailbox.h"
#include "OGSimulation/Network/LocalInputCache.h"
#include "OGSimulation/Network/ReplicatedTierConsumer.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/PCTimeManagement/ClientPredictionClock.h"
#include "OGSimulation/PCTimeManagement/NetworkTimeEstimator.h"
#include "OGSimulation/PCTimeManagement/TimeConfig.h"
#include "OGSimulation/SimulationInputResolution.h"
#include "OGSimulation/SimulationReconciliation.h"
#include "OGSimulation/SlotStateProvenance.h"

namespace simulationDisplay
{
inline constexpr size_t kChannelSlots = 4u;
} // namespace simulationDisplay

struct DisplayClockReading
{
	unsigned int                       predictionTick        = 0u;
	float                              stepDeltaSeconds      = 0.f;
	unsigned int                       targetTick            = 0u;
	unsigned int                       authorityTick         = 0u;
	unsigned int                       predictionOffsetTicks = 0u;
	ClientPredictionClock::DriftAction pendingAction         = ClientPredictionClock::DriftAction::None;
	unsigned int                       stallDebtTicks        = 0u;
	EventSeamDiagnostics               events{};
};

struct DisplayTierReading
{
	bool    hasReceivedTier                = false;
	int32_t tierIndex                      = 0;
	int32_t tierInputDelayTicks            = 0;
	int32_t relayDelayFloorTicks           = 0;
	int32_t clientEffectiveInputDelayTicks = 0;
};

inline DisplayClockReading readDisplayClock(const ClientPredictionClock& clock, const NetworkTimeEstimator& estimator)
{
	DisplayClockReading reading;
	const SimulationTimeStep step = clock.getPredictionStep();
	reading.predictionTick        = step.getTick();
	reading.stepDeltaSeconds      = step.getDeltaSeconds();
	reading.targetTick            = estimator.getTargetPredictionTick();
	reading.authorityTick         = estimator.getLastAuthorityTick();
	reading.predictionOffsetTicks = estimator.getPredictionOffsetTicks();
	reading.pendingAction         = clock.evaluateDrift();
	reading.stallDebtTicks        = clock.getRequiredInputDelayIncreaseStallTicks();

	const ClientPredictionClock::Diagnostics diagnostics = clock.getDiagnostics();
	reading.events.skipCount              = diagnostics.skipCount();
	reading.events.lastSkipTick           = diagnostics.lastSkipTick();
	reading.events.stallCount             = diagnostics.stallCount();
	reading.events.lastStallTick          = diagnostics.lastStallTick();
	reading.events.hardResyncCount        = diagnostics.hardResyncCount();
	reading.events.lastHardResyncFromTick = diagnostics.lastHardResyncFromTick();
	reading.events.lastHardResyncToTick   = diagnostics.lastHardResyncToTick();
	return reading;
}

inline DisplayTierReading readDisplayTier(const ReplicatedTierConsumer& consumer, const TimeConfig& config,
	int32_t clientEffectiveInputDelayTicks)
{
	DisplayTierReading reading;
	reading.hasReceivedTier                = consumer.hasReceivedTier();
	reading.tierIndex                      = consumer.currentTierIndex();
	reading.tierInputDelayTicks            = consumer.effectiveInputDelayTicks();
	reading.relayDelayFloorTicks           = config.relayDelayFloorTicks;
	reading.clientEffectiveInputDelayTicks = clientEffectiveInputDelayTicks;
	return reading;
}

template <typename InputT>
struct DisplayCapture
{
	int32_t tick    = -1;
	bool    present = false;
	InputT  input{};
};

template <typename InputT, size_t Window>
class DisplayCaptureWindow
{
	static_assert(Window >= 1u, "DisplayCaptureWindow: the window holds at least the newest tick");
	static_assert(Window <= kLocalInputCacheCapacityTicks,
		"DisplayCaptureWindow: a window wider than LocalInputCache's capacity would copy ticks the cache has already evicted");

public:
	static constexpr size_t kWindow = Window;

	void fill(const LocalInputCache<InputT>& cache, uint32_t newestTick)
	{
		for (size_t back = 0u; back < Window; ++back)
		{
			const int64_t tick = static_cast<int64_t>(newestTick) - static_cast<int64_t>(back);
			DisplayCapture<InputT>& slot = m_slots[slotIndexFor(tick)];
			slot = DisplayCapture<InputT>{};
			if (tick < 0)
				continue;
			const int32_t signedTick = static_cast<int32_t>(tick);
			slot.tick = signedTick;
			if (!cache.has(signedTick))
				continue;
			slot.present = true;
			slot.input   = cache.at(signedTick);
		}
	}

	void clear() { m_slots.fill(DisplayCapture<InputT>{}); }

	const InputT* at(uint32_t tick) const
	{
		const DisplayCapture<InputT>& slot = m_slots[slotIndexFor(static_cast<int64_t>(tick))];
		return (slot.present && slot.tick == static_cast<int32_t>(tick)) ? &slot.input : nullptr;
	}

private:
	static size_t slotIndexFor(int64_t tick)
	{
		const int64_t window = static_cast<int64_t>(Window);
		return static_cast<size_t>(((tick % window) + window) % window);
	}

	std::array<DisplayCapture<InputT>, Window> m_slots{};
};

struct DisplayAppliedSlot
{
	uint32_t                           simTick = 0u;
	bool                               filled  = false;
	AppliedCaptureRef                  ref{};
	std::optional<SlotStateProvenance> provenance{};
};

template <size_t Window>
class DisplayAppliedWindow
{
	static_assert(Window >= 1u, "DisplayAppliedWindow: the window holds at least the newest tick");

public:
	static constexpr size_t kWindow = Window;

	template <typename SimulatableT, typename ReconciliationT>
	void fill(const ReconciliationT& reconciliation, unsigned int id, uint32_t newestTick)
	{
		m_hasCorrectionCache = reconciliation.template findCorrectionCache<SimulatableT>(id) != nullptr;
		const auto diagnostics = reconciliation.getDiagnostics();
		for (size_t back = 0u; back < Window; ++back)
		{
			DisplayAppliedSlot& slot = m_slots[back];
			slot = DisplayAppliedSlot{};
			if (back > newestTick)
				continue;
			slot.simTick    = newestTick - static_cast<uint32_t>(back);
			slot.filled     = true;
			slot.ref        = reconciliation.template getAppliedCaptureTickRef<SimulatableT>(id, slot.simTick);
			slot.provenance = diagnostics.template slotStateProvenance<SimulatableT>(id, slot.simTick);
		}
	}

	void clear()
	{
		m_hasCorrectionCache = false;
		m_slots.fill(DisplayAppliedSlot{});
	}

	bool hasCorrectionCache() const { return m_hasCorrectionCache; }

	AppliedCaptureRef appliedCaptureRef(uint32_t simTick) const
	{
		const DisplayAppliedSlot* const slot = find(simTick);
		return slot != nullptr ? slot->ref : AppliedCaptureRef{};
	}

	std::optional<SlotStateProvenance> slotProvenance(uint32_t simTick) const
	{
		const DisplayAppliedSlot* const slot = find(simTick);
		return slot != nullptr ? slot->provenance : std::nullopt;
	}

private:
	const DisplayAppliedSlot* find(uint32_t simTick) const
	{
		const uint32_t newest = m_slots[0].simTick;
		if (!m_slots[0].filled || simTick > newest)
			return nullptr;
		const uint32_t back = newest - simTick;
		if (back >= Window || !m_slots[back].filled)
			return nullptr;
		return &m_slots[back];
	}

	bool                                   m_hasCorrectionCache = false;
	std::array<DisplayAppliedSlot, Window> m_slots{};
};

template <typename SimulatableT, size_t CaptureWindow, size_t AppliedWindow>
struct SimulationDisplayEntry
{
	using Input    = typename SimulatableT::InputType;
	using VizState = std::remove_cvref_t<decltype(std::declval<const SimulatableT&>().getAllState())>;

	unsigned int                               simId = 0u;
	VizState                                   vizState{};
	std::optional<Input>                       lastRelayedInput{};
	bool                                       hasLocalInputCache = false;
	DisplayCaptureWindow<Input, CaptureWindow> localCaptures{};
	bool                                       hasRelayedReadObservations = false;
	RelayedReadObservationRing                 relayedReadObservations{};
	bool                                       hasRelayedInputArrivals = false;
	RelayedInputArrivalRing                    relayedInputArrivals{};
	DisplayAppliedWindow<AppliedWindow>        applied{};
};

template <typename SimulatableT, size_t MaxSimulatables, size_t CaptureWindow, size_t AppliedWindow>
struct SimulationDisplaySnapshotT
{
	static_assert(MaxSimulatables >= 1u, "SimulationDisplaySnapshotT: MaxSimulatables must be at least 1");

	using Simulatable = SimulatableT;
	using Entry       = SimulationDisplayEntry<SimulatableT, CaptureWindow, AppliedWindow>;

	static constexpr size_t kMaxSimulatables = MaxSimulatables;

	uint64_t                             publishSequence = 0u;
	DisplayClockReading                  clock{};
	DisplayTierReading                   tier{};
	size_t                               entryCount = 0u;
	std::array<Entry, MaxSimulatables>   entries{};

	const Entry* find(unsigned int simId) const
	{
		for (size_t index = 0u; index < entryCount; ++index)
		{
			if (entries[index].simId == simId)
				return &entries[index];
		}
		return nullptr;
	}
};

template <typename SimulatableT, size_t MaxSimulatables, size_t CaptureWindow, size_t AppliedWindow,
	typename StorageT, typename ResolutionT, typename ReconciliationT, typename RelaySourceT>
void fillSimulationDisplaySnapshot(
	SimulationDisplaySnapshotT<SimulatableT, MaxSimulatables, CaptureWindow, AppliedWindow>& snapshot,
	const StorageT&            storage,
	const ResolutionT&         resolution,
	const ReconciliationT&     reconciliation,
	const DisplayClockReading& clock,
	const DisplayTierReading&  tier,
	const RelaySourceT&        relay)
{
	snapshot.clock      = clock;
	snapshot.tier       = tier;
	snapshot.entryCount = 0u;

	const auto     diagnostics = resolution.getDiagnostics();
	const uint32_t newestTick  = clock.predictionTick;

	storage.template forEachSimulatable<SimulatableT>([&](unsigned int id, const SimulatableT& simulatable)
	{
		OG_CHECK(snapshot.entryCount < MaxSimulatables,
			"fillSimulationDisplaySnapshot: the storage holds more simulatables than the snapshot's MaxSimulatables; the excess "
			"is not shown");
		if (snapshot.entryCount >= MaxSimulatables)
			return;

		auto& entry = snapshot.entries[snapshot.entryCount++];
		entry.simId = id;
		// ⛔G-01  docs/SimulationDisplaySnapshot-guards.md
		entry.vizState         = simulatable.getAllState();
		entry.lastRelayedInput = relay.lastRelayedInput(id);

		const auto* const captures = diagnostics.template localInputCache<SimulatableT>(id);
		entry.hasLocalInputCache = captures != nullptr;
		if (captures != nullptr)
			entry.localCaptures.fill(*captures, newestTick);
		else
			entry.localCaptures.clear();

		const RelayedReadObservationRing* const reads = diagnostics.template relayedReadObservations<SimulatableT>(id);
		entry.hasRelayedReadObservations = reads != nullptr;
		entry.relayedReadObservations    = reads != nullptr ? *reads : RelayedReadObservationRing{};

		const RelayedInputArrivalRing* const arrivals = relay.relayedInputArrivals(id);
		entry.hasRelayedInputArrivals = arrivals != nullptr;
		entry.relayedInputArrivals    = arrivals != nullptr ? *arrivals : RelayedInputArrivalRing{};

		entry.applied.template fill<SimulatableT>(reconciliation, id, newestTick);
	});
}

template <typename SimulatableT, typename ResolutionT>
class LiveDisplayRelaySource
{
public:
	using Input = typename SimulatableT::InputType;

	explicit LiveDisplayRelaySource(const ResolutionT& resolution) : m_resolution(&resolution) {}

	std::optional<Input> lastRelayedInput(unsigned int simId) const
	{
		return m_resolution->template getLastRelayedInput<SimulatableT>(simId);
	}

	const RelayedInputArrivalRing* relayedInputArrivals(unsigned int simId) const
	{
		return m_resolution->getDiagnostics().template relayedInputArrivals<SimulatableT>(simId);
	}

private:
	const ResolutionT* m_resolution;
};

template <typename SimulatableT, size_t MaxSimulatables, size_t CaptureWindow, size_t AppliedWindow,
	typename StorageT, typename ResolutionT, typename ReconciliationT>
void fillSimulationDisplaySnapshot(
	SimulationDisplaySnapshotT<SimulatableT, MaxSimulatables, CaptureWindow, AppliedWindow>& snapshot,
	const StorageT&            storage,
	const ResolutionT&         resolution,
	const ReconciliationT&     reconciliation,
	const DisplayClockReading& clock,
	const DisplayTierReading&  tier)
{
	fillSimulationDisplaySnapshot(snapshot, storage, resolution, reconciliation, clock, tier,
		LiveDisplayRelaySource<SimulatableT, ResolutionT>(resolution));
}

template <typename InputT, size_t MaxSimulatables>
struct DisplayRelayStage
{
	static_assert(MaxSimulatables >= 1u, "DisplayRelayStage: MaxSimulatables must be at least 1");

	size_t                                               count = 0u;
	std::array<unsigned int, MaxSimulatables>            ids{};
	std::array<std::optional<InputT>, MaxSimulatables>   lastRelayedInputs{};
	std::array<bool, MaxSimulatables>                    hasArrivals{};
	std::array<RelayedInputArrivalRing, MaxSimulatables> arrivals{};

	std::optional<InputT> lastRelayedInput(unsigned int simId) const
	{
		const size_t index = indexOf(simId);
		return index < count ? lastRelayedInputs[index] : std::nullopt;
	}

	const RelayedInputArrivalRing* relayedInputArrivals(unsigned int simId) const
	{
		const size_t index = indexOf(simId);
		return index < count && hasArrivals[index] ? &arrivals[index] : nullptr;
	}

private:
	size_t indexOf(unsigned int simId) const
	{
		for (size_t index = 0u; index < count; ++index)
		{
			if (ids[index] == simId)
				return index;
		}
		return count;
	}
};

template <typename SimulatableT, size_t MaxSimulatables, typename StorageT, typename ResolutionT>
void stageDisplayRelay(DisplayRelayStage<typename SimulatableT::InputType, MaxSimulatables>& stage, const StorageT& storage,
	const ResolutionT& resolution)
{
	const LiveDisplayRelaySource<SimulatableT, ResolutionT> live(resolution);
	stage.count = 0u;
	storage.template forEachSimulatable<SimulatableT>([&](unsigned int id, const SimulatableT&)
	{
		if (stage.count >= MaxSimulatables)
			return;
		const size_t index = stage.count++;
		stage.ids[index]               = id;
		stage.lastRelayedInputs[index] = live.lastRelayedInput(id);
		const RelayedInputArrivalRing* const ring = live.relayedInputArrivals(id);
		stage.hasArrivals[index] = ring != nullptr;
		stage.arrivals[index]    = ring != nullptr ? *ring : RelayedInputArrivalRing{};
	});
}

template <typename SnapshotT>
class SimulationDisplayPublisher
{
public:
	using Channel = SnapshotChannel<SnapshotT, simulationDisplay::kChannelSlots>;

	static constexpr size_t kMaxHeldByReader = Channel::kMaxHeldKeepingNewest;

	SimulationDisplayPublisher() = default;

	SimulationDisplayPublisher(const SimulationDisplayPublisher&) = delete;
	SimulationDisplayPublisher& operator=(const SimulationDisplayPublisher&) = delete;
	SimulationDisplayPublisher(SimulationDisplayPublisher&&) = delete;
	SimulationDisplayPublisher& operator=(SimulationDisplayPublisher&&) = delete;

	template <typename FillF>
	bool publish_Step(FillF&& fill)
	{
		SnapshotT* const snapshot = m_channel.beginWrite();
		OG_CHECK(snapshot != nullptr,
			"SimulationDisplayPublisher::publish_Step: the display snapshot channel has no free slot. A game-thread reader holds "
			"at most kMaxHeldByReader snapshots, and a 4-slot channel keeps a slot free while two are held");
		if (snapshot == nullptr)
			return false;

		std::forward<FillF>(fill)(*snapshot);
		snapshot->publishSequence = ++m_publishedSequence;
		m_channel.commit();
		return true;
	}

	const SnapshotT* acquire_GameThread(size_t back = 0u) { return m_channel.peekNewest(back); }

	void release_GameThread(const SnapshotT* snapshot) { m_channel.release(snapshot); }

	const SnapshotT* refreshView_GameThread(const SnapshotT* held)
	{
		if (held != nullptr)
			m_channel.release(held);
		return m_channel.peekNewest(0u);
	}

	uint64_t drops() const { return m_channel.drops(); }

private:
	Channel  m_channel;
	uint64_t m_publishedSequence = 0u;
};
