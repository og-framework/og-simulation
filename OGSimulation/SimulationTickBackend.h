#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationTickBackend-rationale.md · docs/SimulationTickBackend-guards.md

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "OGSimulation/BodySlotOccupancy.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/QueryGeometry.h"
#include "OGSimulation/SimulationTickDriver.h"
#include "OGSimulation/StaticGeometry.h"
#include "OGSimulation/TickDriverConcepts.h"
#include "OGSimulation/TickDriverPresenter.h"

namespace tickBackendWorldHost
{
struct SimulatableArchetype
{
};

struct StaticDataArchetype
{
};

struct ManagerArchetype
{
	uint32_t currentIntegratedTick() const;
};

struct PublishSlotArchetype
{
	void operator()(uint32_t slot) const;
};

struct WorldLockWaitArchetype
{
	void operator()(double waitSeconds) const;
};

struct PushBodySlotOccupancyArchetype
{
	void operator()(uint32_t slot, bool occupied) const;
};
} // namespace tickBackendWorldHost

template <typename H, typename SimulatableT>
concept TickBackendWorldHost = TickDriverWorldHost<H> && requires(H& h, const H& ch,
	const tickBackendWorldHost::ManagerArchetype& manager, uint64_t key, const char* name, bool isAuthority,
	std::optional<uint32_t> replicatedSlot, tickBackendWorldHost::PublishSlotArchetype publish, uint32_t slot,
	unsigned int id, SimulatableT& simulatable, const tickBackendWorldHost::StaticDataArchetype& staticData,
	tickBackendWorldHost::WorldLockWaitArchetype onWorldLockWait, tickBackendWorldHost::PushBodySlotOccupancyArchetype pushBodySlotOccupancy,
	const QueryVolumeDescriptor& descriptor)
{
	typename H::Config;
	{ h.bindIntegratedTickSource(manager) }                                              -> std::same_as<void>;
	{ ch.rootDeclarationIndex() }                                                        -> std::same_as<uint8_t>;
	{ h.acquire(key, name, isAuthority, replicatedSlot, publish) }                       -> std::same_as<std::optional<uint32_t>>;
	{ h.bind(slot, id, simulatable, staticData, onWorldLockWait) }                       -> std::same_as<void>;
	{ h.release(key, id, pushBodySlotOccupancy) }                                        -> std::same_as<std::optional<uint32_t>>;
	{ ch.slotOf(key) }                                                                   -> std::same_as<std::optional<uint32_t>>;
	h.editVizQuery();
	ch.getVizReader();
	{ h.registerVizVolume(descriptor, key, name, isAuthority, replicatedSlot, publish) } -> std::same_as<std::optional<QueryVolumeId>>;
};

template <typename H, typename... AdapterOptionalTs>
concept TickBackendWorldBuild = requires(H& h, const typename H::Config& config, const StaticWorldDescription& statics,
	AdapterOptionalTs&... adapterOptionals)
{
	{ h.build(config, statics, statics, adapterOptionals...) } -> std::same_as<void>;
};

namespace tickBackendWorldHostSelfCheck
{
using tickBackendWorldHost::SimulatableArchetype;

struct Conforming : tickDriverWorldHostSelfCheck::Conforming
{
	struct Config
	{
	};

	template <typename ManagerT>
	void bindIntegratedTickSource(const ManagerT&);
	uint8_t rootDeclarationIndex() const;
	template <typename PublishF>
	std::optional<uint32_t> acquire(uint64_t, const char*, bool, std::optional<uint32_t>, PublishF&&);
	template <typename StaticDataT, typename OnWorldLockWaitF>
	void bind(uint32_t, unsigned int, SimulatableArchetype&, const StaticDataT&, OnWorldLockWaitF&&);
	template <typename PushBodySlotOccupancyF>
	std::optional<uint32_t> release(uint64_t, unsigned int, PushBodySlotOccupancyF&&);
	std::optional<uint32_t> slotOf(uint64_t) const;
	int& editVizQuery();
	const int& getVizReader() const;
	template <typename PublishF>
	std::optional<QueryVolumeId> registerVizVolume(const QueryVolumeDescriptor&, uint64_t, const char*, bool,
		std::optional<uint32_t>, PublishF&&);
	void build(const Config&, const StaticWorldDescription&, const StaticWorldDescription&, std::optional<int>&);
};

struct ReleaseWithoutBodySlotOccupancyPush : Conforming
{
	std::optional<uint32_t> release(uint64_t, unsigned int);
};

struct BindWithoutLockWaitNote : Conforming
{
	template <typename StaticDataT>
	void bind(uint32_t, unsigned int, SimulatableArchetype&, const StaticDataT&);
};

struct MutatingSlotOf : Conforming
{
	std::optional<uint32_t> slotOf(uint64_t);
};

struct AcquireWithoutReplicatedSlot : Conforming
{
	template <typename PublishF>
	std::optional<uint32_t> acquire(uint64_t, const char*, bool, PublishF&&);
};

static_assert(TickBackendWorldHost<Conforming, SimulatableArchetype>,
	"TickBackendWorldHost self-check: the reference shape must satisfy the concept");
static_assert(!TickBackendWorldHost<ReleaseWithoutBodySlotOccupancyPush, SimulatableArchetype>,
	"TickBackendWorldHost self-check: release takes the occupancy push, so a vacated slot reaches the driver's ring");
static_assert(!TickBackendWorldHost<BindWithoutLockWaitNote, SimulatableArchetype>,
	"TickBackendWorldHost self-check: bind takes the world-lock wait note, so the window keeps the bind's lock wait");
static_assert(!TickBackendWorldHost<MutatingSlotOf, SimulatableArchetype>,
	"TickBackendWorldHost self-check: slotOf must be callable on a const host; it only reads the slot table");
static_assert(!TickBackendWorldHost<AcquireWithoutReplicatedSlot, SimulatableArchetype>,
	"TickBackendWorldHost self-check: acquire takes the replicated slot, the only slot a client may bind");
static_assert(!TickBackendWorldHost<tickDriverWorldHostSelfCheck::Conforming, SimulatableArchetype>,
	"TickBackendWorldHost self-check: the concept refines TickDriverWorldHost; the driver's members alone do not satisfy it");
static_assert(TickBackendWorldBuild<Conforming, std::optional<int>>,
	"TickBackendWorldBuild self-check: the reference shape must satisfy the concept with its adapter optional");
static_assert(!TickBackendWorldBuild<Conforming>,
	"TickBackendWorldBuild self-check: build takes the session's adapter optionals, which it emplaces");
static_assert(!TickBackendWorldBuild<Conforming, std::optional<long>>,
	"TickBackendWorldBuild self-check: the adapter optionals are the world host's own adapter types");
} // namespace tickBackendWorldHostSelfCheck

template <TickDriverGame GameT, typename WorldHostT, typename SimulatableT, TickDriverEngine EngineT>
	requires TickBackendWorldHost<WorldHostT, SimulatableT>
class SimulationTickBackend
{
public:
	class RenderSource
	{
	public:
		using Storage          = typename GameT::Storage;
		using PhysicsComposite = std::remove_cvref_t<decltype(std::declval<const SimulatableT&>().getPhysicsComposite())>;
		static constexpr size_t kMaxSimulatableSlots = ::kMaxSimulatableSlots;

		explicit RenderSource(SimulationTickBackend& backend) : m_backend(backend) {}

		RenderSource(const RenderSource&) = delete;
		RenderSource& operator=(const RenderSource&) = delete;

		Storage& storage() { return m_backend.m_game.storage(); }
		bool savesTicks() const { return m_backend.m_driver->savesTicks(); }
		uint64_t grantedResims() const { return m_backend.m_driver->grantedResims(); }
		uint32_t integratedTick() const { return m_backend.m_game.manager().currentIntegratedTick(); }

	private:
		SimulationTickBackend& m_backend;
	};

	using Presenter = TickDriverPresenter<typename GameT::RenderSnapshot, EngineT, RenderSource>;
	using Driver    = SimulationTickDriver<GameT, WorldHostT, Presenter, EngineT>;

	SimulationTickBackend(GameT& game, WorldHostT& worldHost, EngineT& engine)
		: m_game(game)
		, m_worldHost(worldHost)
		, m_engine(engine)
		, m_renderSource(*this)
	{
		static_assert(__builtin_offsetof(SimulationTickBackend, m_renderSource) < __builtin_offsetof(SimulationTickBackend, m_presenter)
			&& __builtin_offsetof(SimulationTickBackend, m_presenter) < __builtin_offsetof(SimulationTickBackend, m_driver),
			"SimulationTickBackend members destruct in REVERSE declaration order. The driver holds the presenter by pointer, "
			"and its step driver unregisters its resync callback from the game's manager when it goes; the presenter holds the "
			"render source by reference. Keep m_renderSource < m_presenter < m_driver. Was the presenter < driver and render "
			"source < presenter rows of the engine-hosted backend");
	}

	SimulationTickBackend(const SimulationTickBackend&) = delete;
	SimulationTickBackend& operator=(const SimulationTickBackend&) = delete;

	template <typename... AdapterOptionalTs>
		requires TickBackendWorldBuild<WorldHostT, AdapterOptionalTs...>
	void build(const typename WorldHostT::Config& config, const StaticWorldDescription& queryableStatics,
		const StaticWorldDescription& physicsOnlyStatics, AdapterOptionalTs&... adapterOptionals)
	{
		m_worldHost.build(config, queryableStatics, physicsOnlyStatics, adapterOptionals...);
	}

	Driver& start(double dtSeconds, bool isAuthority, int32_t requestedMaxCatchUpSteps)
	{
		m_worldHost.bindIntegratedTickSource(m_game.manager());
		m_presenter.emplace(m_engine, m_renderSource, TickDriverPresenterConfig{ m_game.manager().runsPrediction(),
			static_cast<float>(dtSeconds), m_worldHost.rootDeclarationIndex() });
		OG_CHECK(m_presenter.has_value(),
			"SimulationTickBackend::start: m_presenter has no value when the driver is built. The driver holds the presenter "
			"by pointer from its constructor on, so the presenter is emplaced first. Was the order of the engine-hosted "
			"backend's start of stepping");
		m_driver.emplace(m_game, m_worldHost, *m_presenter, m_engine, typename Driver::Config{ dtSeconds, isAuthority,
			m_worldHost.threadContext().stepsInline() ? StepThreading::Inline : StepThreading::FrameBounded,
			requestedMaxCatchUpSteps });
		return *m_driver;
	}

	void stop()
	{
		if (m_driver.has_value())
			m_driver->stop();
	}

	template <typename PublishF, typename StaticDataT>
	bool acquireAndBind(uint64_t key, const char* name, bool isAuthority, std::optional<uint32_t> replicatedSlot,
		PublishF&& publish, unsigned int id, SimulatableT& simulatable, const StaticDataT& staticData)
	{
		const std::optional<uint32_t> slot = m_worldHost.acquire(key, name, isAuthority, replicatedSlot, publish);
		if (!slot.has_value())
			return false;

		m_worldHost.bind(*slot, id, simulatable, staticData,
			[this](double waitSeconds) { driver().noteWorldLockWait(waitSeconds); });
		return true;
	}

	void onRegistered(unsigned int id, uint64_t key)
	{
		const std::optional<uint32_t> slot = m_worldHost.slotOf(key);
		OG_CHECK(slot.has_value(),
			"SimulationTickBackend::onRegistered: slotOf(key) has no value: the id is Ready but its owner holds no slot; the "
			"first pass binds one. Was the engine-hosted backend's registration check");
		driver().pushBodySlotOccupancy(*slot, true);
		presenter().onRegistered(id, *slot);
	}

	void forgetPresenterTarget(unsigned int id, uint64_t key)
	{
		if (m_presenter.has_value())
			m_presenter->onUnregistered(id, m_worldHost.slotOf(key).value_or(0u));
	}

	void releaseSlot(uint64_t key, unsigned int id)
	{
		m_worldHost.release(key, id, [this](uint32_t slot, bool occupied) { driver().pushBodySlotOccupancy(slot, occupied); });
	}

	template <typename SinkT>
	void present(const PresentControls& controls, SinkT& sink)
	{
		presenter().present_GameThread(driver(), controls, sink, m_worldHost);
	}

	void updateVisualization()
	{
		presenter().updateVisualization_GameThread(m_game.storage());
	}

	decltype(auto) editVizQuery() { return m_worldHost.editVizQuery(); }

	decltype(auto) getVizReader() const { return m_worldHost.getVizReader(); }

	template <typename PublishF>
	std::optional<QueryVolumeId> registerVizVolume(const QueryVolumeDescriptor& descriptor, uint64_t key, const char* name,
		bool isAuthority, std::optional<uint32_t> replicatedSlot, PublishF&& publish)
	{
		return m_worldHost.registerVizVolume(descriptor, key, name, isAuthority, replicatedSlot,
			std::forward<PublishF>(publish));
	}

	Driver& driver()
	{
		OG_CHECK(m_driver.has_value(),
			"SimulationTickBackend::driver: m_driver has no value: a door reached the tick driver before start() built it. "
			"Was the engine-hosted backend's tick-driver door check");
		return *m_driver;
	}

	Presenter& presenter()
	{
		OG_CHECK(m_presenter.has_value(),
			"SimulationTickBackend::presenter: m_presenter has no value: a door reached the presenter before start() built "
			"it. Was the engine-hosted backend's presenter door check");
		return *m_presenter;
	}

private:
	GameT&                   m_game;
	WorldHostT&              m_worldHost;
	EngineT&                 m_engine;
	RenderSource             m_renderSource;
	std::optional<Presenter> m_presenter;
	std::optional<Driver>    m_driver;
};
