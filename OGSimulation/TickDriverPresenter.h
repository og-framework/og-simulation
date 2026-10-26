#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/TickDriverPresenter-rationale.md · docs/TickDriverPresenter-guards.md

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "glm/geometric.hpp"
#include "glm/gtc/quaternion.hpp"
#include "glm/mat3x3.hpp"
#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"
#include "OGSimulation/LatencyBudgetProbe.h"
#include "OGSimulation/Mailbox.h"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/PhysicsBodyState.h"
#include "OGSimulation/RenderCorrectionSmoothing.h"
#include "OGSimulation/RenderInterpolation.h"
#include "OGSimulation/RenderSnapshot.h"
#include "OGSimulation/SimulationComposite.h"
#include "OGSimulation/SimulationFrame.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationTimeContext.h"
#include "OGSimulation/StepHooks.h"
#include "OGSimulation/TickDriverConcepts.h"
#include "OGSimulation/TickDriverLog.h"

struct PresentControls
{
	float  delaySteps        = 2.f;
	bool   newestOnly        = false;
	float  correctionSeconds = 0.3f;
	float  maxCorrection     = 2.5f;
	float  desyncSeconds     = 0.6f;
	bool   traceFrames       = false;
	float  parkSnap          = std::numeric_limits<float>::infinity();
	double lengthReportScale = 1.0;
};

struct TickDriverPresenterConfig
{
	bool    runsPrediction       = false;
	float   stepSeconds          = 0.f;
	uint8_t rootDeclarationIndex = 0u;
};

namespace tickDriverPresenter
{
inline constexpr size_t   kSnapshotChannelSlots       = 4u;
inline constexpr size_t   kCorrectionRingSlots        = 16u;
inline constexpr double   kMaxInterpolationDelaySteps = 2.0;
// ⛔G-02  docs/TickDriverPresenter-guards.md
inline constexpr double   kHeldTolerance              = 1e-8;
inline constexpr double   kAppliedTolerance           = 9.999999747378752e-7;
inline constexpr double   kShadowPoseTolerance        = 1e-5;
inline constexpr double   kShadowPoseToleranceRadians = 1.0e-4;
inline constexpr uint32_t kWindowFrames               = 600u;
inline constexpr size_t   kLineReserve                = 1024u;
inline constexpr size_t   kDisplacementFieldReserve   = 128u;

static_assert(std::bit_cast<uint64_t>(kHeldTolerance) == 0x3E45798EE2308C3Aull
		&& std::bit_cast<uint64_t>(kAppliedTolerance) == 0x3EB0C6F79999999Aull
		&& std::bit_cast<uint64_t>(kShadowPoseTolerance) == 0x3EE4F8B588E368F1ull
		&& std::bit_cast<uint32_t>(PresentControls{}.maxCorrection) == 0x40200000u,
	"TickDriverPresenter: the held, applied and shadow tolerances are exactly 1e-8, double(1e-4f) / 100 and 1e-5 m, and the "
	"default maxCorrection is exactly 2.5 m, the metre values flip U gave the presenter. Was task 3's pin of the centimetre "
	"values 1e-6, double(1e-4f), 1e-3 and 250");

template <typename Composite>
struct declarationCountOf;

template <typename... Declarations>
struct declarationCountOf<SimulationComposite<Declarations...>> : std::integral_constant<size_t, sizeof...(Declarations)>
{
};

template <typename T>
inline constexpr bool isRenderSnapshot = false;

template <size_t MaxBodies>
inline constexpr bool isRenderSnapshot<RenderSnapshotT<MaxBodies>> = true;

template <typename T>
constexpr T maxOf(T first, T second)
{
	return (second < first) ? first : second;
}

template <typename T>
constexpr T minOf(T first, T second)
{
	return (first < second) ? first : second;
}

template <typename T>
constexpr T clampOf(T value, T low, T high)
{
	return maxOf(minOf(value, high), low);
}

template <typename T>
constexpr T absOf(T value)
{
	return (value < T(0)) ? -value : value;
}

inline double atan2Of(double y, double x)
{
	if (x == 0.0 && y == 0.0)
		return 0.0;
	return std::atan2(y, x);
}

inline double distanceOf(const glm::dvec3& from, const glm::dvec3& to)
{
	const double x = to.x - from.x;
	const double y = to.y - from.y;
	const double z = to.z - from.z;
	return std::sqrt(x * x + y * y + z * z);
}

inline bool equalWithin(const glm::dvec3& value, const glm::dvec3& other, double tolerance)
{
	return absOf(value.x - other.x) <= tolerance && absOf(value.y - other.y) <= tolerance
		&& absOf(value.z - other.z) <= tolerance;
}

inline bool exactlyEqual(const glm::dvec3& value, const glm::dvec3& other)
{
	return value.x == other.x && value.y == other.y && value.z == other.z;
}

template <typename T>
std::optional<glm::dvec3> positionOf(const T& value)
{
	if constexpr (requires { value.has_value(); })
	{
		if (!value.has_value())
			return std::nullopt;
		return glm::dvec3(*value);
	}
	else
	{
		return glm::dvec3(value);
	}
}

template <typename T>
double reportedLength(const PresentControls& controls, T length)
{
	return static_cast<double>(length) * controls.lengthReportScale;
}

inline double clampedDelaySteps(const PresentControls& controls)
{
	return clampOf(static_cast<double>(controls.delaySteps), 0.0, kMaxInterpolationDelaySteps);
}

inline const char* stepKindText(StepKind kind)
{
	switch (kind)
	{
	case StepKind::Normal:     return "Normal";
	case StepKind::Stall:      return "Stall";
	case StepKind::Skip:       return "Skip";
	case StepKind::HardResync: return "HardResync";
	}
	return "?";
}

inline const char* roleText(bool runsPrediction)
{
	return runsPrediction ? "Client" : "Authority";
}

struct ShadowTargetsArchetype
{
	template <typename EachT>
	void operator()(EachT&& each) const;
};

struct ShadowVisitorArchetype
{
	template <typename ReadBackT>
	void operator()(uint32_t id, uint32_t declarationIndex, const RenderBody& pose, ReadBackT&& readBack) const;
};

struct ShadowRestoreNoteArchetype
{
	void operator()(double restoreSeconds, bool refused) const;
};
} // namespace tickDriverPresenter

template <typename S>
concept TickDriverRenderSource = requires(S& s)
{
	typename S::Storage;
	typename S::PhysicsComposite;
	{ S::kMaxSimulatableSlots } -> std::convertible_to<size_t>;
	typename std::integral_constant<size_t, static_cast<size_t>(S::kMaxSimulatableSlots)>;
	{ s.storage() }        -> std::same_as<typename S::Storage&>;
	{ s.savesTicks() }     -> std::same_as<bool>;
	{ s.grantedResims() }  -> std::same_as<uint64_t>;
	{ s.integratedTick() } -> std::convertible_to<int64_t>;
};

namespace tickDriverRenderSourceSelfCheck
{
struct DeclarationA {};
struct DeclarationB {};

struct Conforming
{
	using Storage = SimulationObjectStorage<>;
	using PhysicsComposite = SimulationComposite<DeclarationA, DeclarationB>;
	static constexpr size_t kMaxSimulatableSlots = 2u;

	Storage& storage();
	bool savesTicks() const;
	uint64_t grantedResims() const;
	int64_t integratedTick() const;
};

struct MissingGrantedResims
{
	using Storage = SimulationObjectStorage<>;
	using PhysicsComposite = SimulationComposite<DeclarationA, DeclarationB>;
	static constexpr size_t kMaxSimulatableSlots = 2u;

	Storage& storage();
	bool savesTicks() const;
	int64_t integratedTick() const;
};

struct RuntimeSlotCount
{
	using Storage = SimulationObjectStorage<>;
	using PhysicsComposite = SimulationComposite<DeclarationA, DeclarationB>;
	static inline size_t kMaxSimulatableSlots = 2u;

	Storage& storage();
	bool savesTicks() const;
	uint64_t grantedResims() const;
	int64_t integratedTick() const;
};

struct StorageByValue
{
	using Storage = SimulationObjectStorage<>;
	using PhysicsComposite = SimulationComposite<DeclarationA, DeclarationB>;
	static constexpr size_t kMaxSimulatableSlots = 2u;

	Storage storage();
	bool savesTicks() const;
	uint64_t grantedResims() const;
	int64_t integratedTick() const;
};

static_assert(TickDriverRenderSource<Conforming>,
	"TickDriverRenderSource self-check: the reference shape must satisfy the concept");
static_assert(!TickDriverRenderSource<MissingGrantedResims>,
	"TickDriverRenderSource self-check: grantedResims (the step driver's granted-resimulation count the capture compares) is required");
static_assert(!TickDriverRenderSource<RuntimeSlotCount>,
	"TickDriverRenderSource self-check: kMaxSimulatableSlots is a compile-time constant; the snapshot size is checked against it");
static_assert(!TickDriverRenderSource<StorageByValue>,
	"TickDriverRenderSource self-check: storage() returns the session's storage by reference, never a copy");
} // namespace tickDriverRenderSourceSelfCheck

template <typename C>
concept TickDriverPresentClock = requires(C& c)
{
	{ c.hostNowSeconds() }                         -> std::same_as<double>;
	{ c.stepIntervalSeconds() }                    -> std::same_as<double>;
	{ c.noteShadowRestore(double{}, bool{}) }      -> std::same_as<void>;
};

namespace tickDriverPresentClockSelfCheck
{
struct Conforming
{
	double hostNowSeconds() const;
	double stepIntervalSeconds();
	void noteShadowRestore(double, bool);
};

struct MissingStepInterval
{
	double hostNowSeconds() const;
	void noteShadowRestore(double, bool);
};

struct FloatHostTime
{
	float hostNowSeconds() const;
	double stepIntervalSeconds();
	void noteShadowRestore(double, bool);
};

struct MissingRestoreNote
{
	double hostNowSeconds() const;
	double stepIntervalSeconds();
};

static_assert(TickDriverPresentClock<Conforming>,
	"TickDriverPresentClock self-check: the reference shape must satisfy the concept");
static_assert(!TickDriverPresentClock<MissingStepInterval>,
	"TickDriverPresentClock self-check: stepIntervalSeconds (the delay's unit) is required");
static_assert(!TickDriverPresentClock<FloatHostTime>,
	"TickDriverPresentClock self-check: the host time is a double, the time base of the snapshots' step deadlines");
static_assert(!TickDriverPresentClock<MissingRestoreNote>,
	"TickDriverPresentClock self-check: noteShadowRestore (the window's shadowRestores/shadowRestoreUs/shadowRefused sample of "
	"every shadow restore) is required");
} // namespace tickDriverPresentClockSelfCheck

template <typename H, typename Snapshot>
concept TickDriverPresentWorldHost = requires(H& h, const H& ch, uint64_t frame, const Snapshot& poses,
	tickDriverPresenter::ShadowTargetsArchetype targetSlots, tickDriverPresenter::ShadowVisitorArchetype visitor,
	tickDriverPresenter::ShadowRestoreNoteArchetype onRestore)
{
	{ ch.hasShadowWorld() }                                               -> std::same_as<bool>;
	{ h.restore_GameThread(frame, onRestore) }                            -> std::same_as<void>;
	{ h.overwriteShadowPoses_GameThread(poses, targetSlots, visitor) }    -> std::same_as<void>;
};

namespace tickDriverPresentWorldHostSelfCheck
{
using Snapshot = RenderSnapshotT<4>;

struct Conforming
{
	bool hasShadowWorld() const;
	template <typename OnRestoreF>
	void restore_GameThread(uint64_t, OnRestoreF&&);
	template <typename TargetsT, typename VisitorT>
	void overwriteShadowPoses_GameThread(const Snapshot&, TargetsT&&, VisitorT&&);
};

struct MissingRestore
{
	bool hasShadowWorld() const;
	template <typename TargetsT, typename VisitorT>
	void overwriteShadowPoses_GameThread(const Snapshot&, TargetsT&&, VisitorT&&);
};

struct RestoreWithoutNote
{
	bool hasShadowWorld() const;
	void restore_GameThread(uint64_t);
	template <typename TargetsT, typename VisitorT>
	void overwriteShadowPoses_GameThread(const Snapshot&, TargetsT&&, VisitorT&&);
};

struct MutatingShadowQuery
{
	bool hasShadowWorld();
	template <typename OnRestoreF>
	void restore_GameThread(uint64_t, OnRestoreF&&);
	template <typename TargetsT, typename VisitorT>
	void overwriteShadowPoses_GameThread(const Snapshot&, TargetsT&&, VisitorT&&);
};

struct OverwriteTakesMutablePoses
{
	bool hasShadowWorld() const;
	template <typename OnRestoreF>
	void restore_GameThread(uint64_t, OnRestoreF&&);
	template <typename TargetsT, typename VisitorT>
	void overwriteShadowPoses_GameThread(Snapshot&, TargetsT&&, VisitorT&&);
};

static_assert(TickDriverPresentWorldHost<Conforming, Snapshot>,
	"TickDriverPresentWorldHost self-check: the reference shape must satisfy the concept");
static_assert(!TickDriverPresentWorldHost<MissingRestore, Snapshot>,
	"TickDriverPresentWorldHost self-check: restore_GameThread (the shadow restore before the apply) is required");
static_assert(!TickDriverPresentWorldHost<RestoreWithoutNote, Snapshot>,
	"TickDriverPresentWorldHost self-check: restore_GameThread takes the restore note, so the window keeps every restore sample");
static_assert(!TickDriverPresentWorldHost<MutatingShadowQuery, Snapshot>,
	"TickDriverPresentWorldHost self-check: hasShadowWorld must be callable on a const host; the window line only reads it");
static_assert(!TickDriverPresentWorldHost<OverwriteTakesMutablePoses, Snapshot>,
	"TickDriverPresentWorldHost self-check: the shadow overwrite reads the render poses and cannot rewrite them");
} // namespace tickDriverPresentWorldHostSelfCheck

template <typename Snapshot, TickDriverEngine EngineT, TickDriverRenderSource SourceT, typename Allocator = std::allocator<char>>
class BasicTickDriverPresenter
{
	static_assert(tickDriverPresenter::isRenderSnapshot<Snapshot>,
		"BasicTickDriverPresenter: Snapshot must be a RenderSnapshotT<N>");

public:
	using RenderSnapshot = Snapshot;
	using Correction     = RenderCorrectionT<Snapshot::kMaxBodies>;
	using Channel        = SnapshotChannel<Snapshot, tickDriverPresenter::kSnapshotChannelSlots>;
	using CorrectionRing = SpscRing<Correction, tickDriverPresenter::kCorrectionRingSlots>;

	static constexpr bool   kHostDiagnostics      = static_cast<bool>(EngineT::kHostDiagnostics);
	static constexpr size_t kMaxSimulatableSlots  = static_cast<size_t>(SourceT::kMaxSimulatableSlots);
	static constexpr size_t kDeclarationsPerSlot  = tickDriverPresenter::declarationCountOf<typename SourceT::PhysicsComposite>::value;

	static_assert(Snapshot::kMaxBodies == kMaxSimulatableSlots * kDeclarationsPerSlot,
		"TickDriverPresenter: a render snapshot holds one body per declaration of the game's physics composite in every slot, "
		"so its N must equal the source's kMaxSimulatableSlots times the composite's declaration count. A declaration added to "
		"or removed from the composite must change N with it, or fillRenderSnapshot drops the excess bodies. Was the engine-hosted "
		"render publish's 'kMaxSimulatableSlots x 6' check.");
	static_assert(std::is_trivially_copyable_v<Snapshot>,
		"TickDriverPresenter: the render snapshot must stay a flat value with no heap-owning member: the J5 channel's four slots "
		"are built with the presenter and filled in place on every step, which allocates nothing only while the snapshot owns "
		"no heap storage. Was task 55's 'no allocation on the publish path after warm-up'.");
	static_assert(tickDriverPresenter::kMaxInterpolationDelaySteps <= static_cast<double>(Channel::kCapacity - 2),
		"TickDriverPresenter: the interpolation delay is clamped to what the J5 channel can bracket: the step before the render "
		"time is at most floor(delay) + 1 snapshots behind the newest, so a delay of N - 2 steps is the most an N-slot channel "
		"reaches. Raise the channel's N with the clamp. Was the engine-hosted render path's delay-clamp check (task 19).");

	BasicTickDriverPresenter(EngineT& engine, SourceT& source, const TickDriverPresenterConfig& config,
		const Allocator& allocator = Allocator())
		: m_engine(engine)
		, m_source(source)
		, m_config(config)
		, m_targets(TargetAllocator(allocator))
		, m_frameDisplacement(FloatAllocator(allocator))
		, m_line(allocator)
		, m_displacementField(allocator)
	{
		if constexpr (kHostDiagnostics)
		{
			m_frameDisplacement.reserve(static_cast<size_t>(tickDriverPresenter::kWindowFrames) * kMaxSimulatableSlots);
			m_line.reserve(tickDriverPresenter::kLineReserve);
			m_displacementField.reserve(tickDriverPresenter::kDisplacementFieldReserve);
		}
	}

	BasicTickDriverPresenter(const BasicTickDriverPresenter&) = delete;
	BasicTickDriverPresenter& operator=(const BasicTickDriverPresenter&) = delete;

	void publish_Step(const TickOutcome& outcome, double stepDeadlineSeconds)
	{
		Snapshot* const snapshot = m_snapshots.beginWrite();
		OG_CHECK(snapshot != nullptr,
			"TickDriverPresenter::publish_Step: the render snapshot channel (J5) has no free slot. The game thread holds at most "
			"two snapshots, the pair that brackets the render time, and a 4-slot channel keeps a slot free while two are held. "
			"Was the engine-hosted render publish's free-slot check");
		if (snapshot == nullptr)
			return;

		fillRenderSnapshot(*snapshot, m_source.storage(), outcome, stepDeadlineSeconds);
		if (m_source.savesTicks())
			m_capture.noteShownSnapshot(*snapshot);
		m_snapshots.commit();
	}

	void captureCorrection_Step(uint64_t physicsStep)
	{
		Correction correction;
		const RenderCorrectionCaptureOutcome outcome = m_capture.onStepAfterReplay(m_source.grantedResims(),
			static_cast<SimTick>(m_source.integratedTick()), physicsStep, m_source.storage(), correction);
		switch (outcome)
		{
		case RenderCorrectionCaptureOutcome::NoNewResim:
			return;
		case RenderCorrectionCaptureOutcome::NoShownHead:
			m_resims.fetch_add(1u, std::memory_order_relaxed);
			return;
		case RenderCorrectionCaptureOutcome::TickMismatch:
			m_resims.fetch_add(1u, std::memory_order_relaxed);
			m_tickMismatches.fetch_add(1u, std::memory_order_relaxed);
			return;
		case RenderCorrectionCaptureOutcome::NoError:
			m_resims.fetch_add(1u, std::memory_order_relaxed);
			m_withoutError.fetch_add(1u, std::memory_order_relaxed);
			return;
		case RenderCorrectionCaptureOutcome::Captured:
			m_resims.fetch_add(1u, std::memory_order_relaxed);
			if (m_corrections.tryPush(correction))
				m_captured.fetch_add(1u, std::memory_order_relaxed);
			return;
		}
	}

	template <TickDriverPresentClock ClockT, TickDriverRenderSink SinkT, TickDriverPresentWorldHost<Snapshot> WorldHostT>
	void present_GameThread(ClockT& clock, const PresentControls& controls, SinkT& sink, WorldHostT& worldHost)
	{
		if (worldHost.hasShadowWorld())
			worldHost.restore_GameThread(m_engine.frameNumber(),
				[&clock](double restoreSeconds, bool refused) { clock.noteShadowRestore(restoreSeconds, refused); });
		applyRenderSnapshot(clock, controls, sink, worldHost);
		overwriteShadowPoses(controls, sink, worldHost);
	}

	template <typename StorageT>
	void updateVisualization_GameThread(StorageT& storage)
	{
		updateVisualizationAll(storage);
		if (!m_renderPosesValid)
			return;

		storage.forEachSimulatable([this](unsigned int id, auto& simulatable)
		{
			auto& vizState = simulatable.editVizState().editState();
			uint8_t declarationIndex = 0u;
			simulatable.getPhysicsComposite().forEach([&](const auto& declaration)
			{
				using D = std::decay_t<decltype(declaration)>;
				using S = typename D::StateType;
				const uint8_t index = declarationIndex++;
				const RenderBody* const pose = findRenderBody(m_renderPoses, static_cast<uint32_t>(id), index);
				if (pose == nullptr || index == m_config.rootDeclarationIndex)
					return;

				auto& bodyState = D::bodyStateOf(vizState.template edit<S>());
				bodyState.position = pose->position;
				if constexpr (std::is_same_v<std::remove_cvref_t<decltype(bodyState)>, PhysicsBodyState>)
					bodyState.rotation = pose->rotation;
			});
		});
	}

	void onRegistered(unsigned int id, uint32_t slot)
	{
		m_targets[id] = Target{ slot };
	}

	void onUnregistered(unsigned int id, [[maybe_unused]] uint32_t slot)
	{
		m_targets.erase(id);
	}

	const Snapshot& renderPoses() const { return m_renderPoses; }
	bool renderPosesValid() const { return m_renderPosesValid; }
	const RenderInterpolationFrame& renderFrame() const { return m_renderFrame; }
	const RenderCorrectionFrame& renderCorrectionFrame() const { return m_correctionFrame; }

private:
	struct Target
	{
		uint32_t   slot = 0u;
		glm::dvec3 position{ 0.0 };
		bool       applied = false;
	};

	struct ApplyWindow
	{
		uint32_t frames               = 0u;
		uint32_t appliedFrames        = 0u;
		uint32_t applied              = 0u;
		uint32_t missing              = 0u;
		uint32_t exact                = 0u;
		uint32_t outOfTolerance       = 0u;
		uint32_t held                 = 0u;
		uint32_t moved                = 0u;
		double   maxError             = 0.0;
		double   maxHeldDrift         = 0.0;
		double   path                 = 0.0;
		uint64_t firstAppliedStep     = 0u;
		uint64_t lastAppliedStep      = 0u;
		uint32_t interpolated         = 0u;
		uint32_t newestMode           = 0u;
		uint32_t atNewest             = 0u;
		uint32_t beyondOldest         = 0u;
		uint32_t snaps                = 0u;
		uint32_t maxNextBack          = 0u;
		uint32_t shadowBodies         = 0u;
		uint32_t shadowExact          = 0u;
		uint32_t shadowOutOfTolerance = 0u;
		double   shadowMaxError       = 0.0;
		double   shadowMaxAngleRad    = 0.0;
	};

	struct CorrectionWindow
	{
		uint32_t corrections         = 0u;
		uint32_t smoothedBodies      = 0u;
		uint32_t snappedBodies       = 0u;
		uint32_t late                = 0u;
		uint32_t pendingDrops        = 0u;
		uint32_t finished            = 0u;
		uint64_t finishedSteps       = 0u;
		uint32_t maxFinishedSteps    = 0u;
		uint32_t activeFrames        = 0u;
		double   maxError            = 0.0;
		double   maxApplied          = 0.0;
		uint32_t policySnaps         = 0u;
		uint32_t rootFramedFrames    = 0u;
		double   maxPolicySnap       = 0.0;
		double   maxPolicySnapDeg    = 0.0;
		uint32_t straddleSnaps       = 0u;
		uint64_t resimsAtStart       = 0u;
		uint64_t capturedAtStart     = 0u;
		uint64_t withoutErrorAtStart = 0u;
		uint64_t tickMismatchAtStart = 0u;
		uint64_t ringDropsAtStart    = 0u;
	};

	using AllocatorTraits  = std::allocator_traits<Allocator>;
	using TargetAllocator  = typename AllocatorTraits::template rebind_alloc<std::pair<const unsigned int, Target>>;
	using FloatAllocator   = typename AllocatorTraits::template rebind_alloc<float>;
	using TargetMap        = std::unordered_map<unsigned int, Target, std::hash<unsigned int>, std::equal_to<unsigned int>,
		TargetAllocator>;
	using Smoother         = RenderCorrectionSmoother<Snapshot::kMaxBodies>;
	using Capture          = RenderCorrectionCapture<Snapshot::kMaxBodies>;

	bool logEnabled(HostLogLevel level) const { return m_engine.logEnabled(level); }

	template <TickDriverPresentClock ClockT>
	bool buildRenderPoses(ClockT& clock, const PresentControls& controls)
	{
		RenderInterpolationParams params;
		params.newestOnly = controls.newestOnly;
		params.snapDistance = controls.parkSnap;
		const double delaySteps = tickDriverPresenter::clampedDelaySteps(controls);
		// ⛔G-01  docs/TickDriverPresenter-guards.md
		params.renderTimeSeconds = clock.hostNowSeconds() - delaySteps * clock.stepIntervalSeconds();

		if (!interpolateRenderPoses(m_snapshots, params, m_renderPoses, m_renderFrame, &m_renderNext))
			return false;

		RenderCorrectionParams correction;
		correction.correctionSeconds = controls.correctionSeconds;
		correction.maxCorrection     = controls.maxCorrection;
		correction.desyncSeconds     = controls.desyncSeconds;
		correction.stepSeconds       = m_config.stepSeconds;
		m_smoother.correctPoses(m_corrections, m_renderFrame, correction, m_renderPoses, m_correctionFrame, &m_renderNext);

		if constexpr (kHostDiagnostics)
		{
			using tickDriverPresenter::maxOf;
			const RenderCorrectionFrame& corrected = m_correctionFrame;
			CorrectionWindow& window = m_correctionWindow;
			window.corrections      += corrected.corrections;
			window.smoothedBodies   += corrected.smoothedBodies;
			window.snappedBodies    += corrected.snappedBodies;
			window.pendingDrops     += corrected.pendingDrops;
			window.finished         += corrected.finished;
			window.finishedSteps    += corrected.finishedSteps;
			window.maxFinishedSteps  = maxOf(window.maxFinishedSteps, corrected.maxFinishedSteps);
			window.activeFrames     += corrected.active > 0u ? 1u : 0u;
			window.maxError          = maxOf(window.maxError, corrected.maxError);
			window.maxApplied        = maxOf(window.maxApplied, corrected.maxApplied);
			window.policySnaps      += corrected.policySnaps;
			window.rootFramedFrames += corrected.rootFramed > 0u ? 1u : 0u;
			window.maxPolicySnap     = maxOf(window.maxPolicySnap, corrected.maxPolicySnap);
			window.maxPolicySnapDeg  = maxOf(window.maxPolicySnapDeg, corrected.maxPolicySnapDeg);
			window.straddleSnaps    += corrected.straddleSnaps;
			if (corrected.late > 0u && window.late == 0u && logEnabled(HostLogLevel::Warning))
			{
				m_engine.log(HostLogLevel::Warning, formatHostLine(m_line,
					"[SimHost.RenderCorrection] LATE frame=%llu next=%llu late=%u: a resimulation's correction reached the game "
					"thread after the corrected step it rides had already been drawn, so that step was drawn uncorrected (a pop)",
					static_cast<unsigned long long>(m_engine.frameNumber()), static_cast<unsigned long long>(m_renderFrame.nextStep),
					corrected.late));
			}
			window.late += corrected.late;
			if (corrected.corrections > 0u && controls.traceFrames && logEnabled(HostLogLevel::Log))
			{
				m_engine.log(HostLogLevel::Log, formatHostLine(m_line,
					"[SimHost.RenderCorrection] event frame=%llu prev=%llu next=%llu alpha=%.4f corrections=%u smoothed=%u "
					"snapped=%u maxError%s=%.*f durationSteps=%d policySnaps=%u rootFramed=%u",
					static_cast<unsigned long long>(m_engine.frameNumber()), static_cast<unsigned long long>(m_renderFrame.prevStep),
					static_cast<unsigned long long>(m_renderFrame.nextStep), m_renderFrame.alpha, corrected.corrections,
					corrected.smoothedBodies, corrected.snappedBodies, simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(4),
					tickDriverPresenter::reportedLength(controls, corrected.maxError),
					renderCorrectionDurationSteps(correction.correctionSeconds, correction.stepSeconds), corrected.policySnaps,
					corrected.rootFramed));
			}
		}

		m_renderPosesValid = true;
		return true;
	}

	template <TickDriverPresentClock ClockT, TickDriverRenderSink SinkT, typename WorldHostT>
	void applyRenderSnapshot(ClockT& clock, const PresentControls& controls, SinkT& sink, const WorldHostT& worldHost)
	{
		if constexpr (kHostDiagnostics)
		{
			++m_applyWindow.frames;
			checkRenderTargetsHeld(controls, sink);
		}

		if (buildRenderPoses(clock, controls))
		{
			for (auto& [id, target] : m_targets)
			{
				const std::optional<glm::dvec3> before = tickDriverPresenter::positionOf(sink.readAnchor(id));
				const RenderBody* const body = findRenderBody(m_renderPoses, static_cast<uint32_t>(id), m_config.rootDeclarationIndex);
				if (!before.has_value() || body == nullptr)
					continue;

				const std::optional<glm::quat> rotation =
					body->hasRotation != 0u ? std::optional<glm::quat>(body->rotation) : std::nullopt;
				const std::optional<glm::dvec3> applied =
					tickDriverPresenter::positionOf(sink.applyRootPose(static_cast<uint32_t>(id), body->position, rotation));
				if (!applied.has_value())
					continue;

				if constexpr (kHostDiagnostics)
				{
					if (target.applied)
					{
						const double displacement = tickDriverPresenter::distanceOf(target.position, *applied);
						m_applyWindow.path += displacement;
						m_frameDisplacement.push_back(static_cast<float>(displacement));
					}
					else if (logEnabled(HostLogLevel::Log))
					{
						m_engine.log(HostLogLevel::Log, formatHostLine(m_line,
							"[SimHost.RenderApply] first role=%s id=%u from=(%.*f,%.*f,%.*f) to=(%.*f,%.*f,%.*f) physicsStep=%llu tick=%u",
							tickDriverPresenter::roleText(m_config.runsPrediction), id, simFrame::lengthDecimals(3), before->x,
							simFrame::lengthDecimals(3), before->y, simFrame::lengthDecimals(3), before->z,
							simFrame::lengthDecimals(3), static_cast<double>(body->position.x),
							simFrame::lengthDecimals(3), static_cast<double>(body->position.y),
							simFrame::lengthDecimals(3), static_cast<double>(body->position.z),
							static_cast<unsigned long long>(m_renderPoses.physicsStep),
							static_cast<uint32_t>(m_renderPoses.tick)));
					}
				}
				target.position = *applied;
				target.applied  = true;
			}

			if constexpr (kHostDiagnostics)
			{
				checkRenderApply(controls, sink);
				traceRenderFrame(clock, controls);
			}
		}

		if constexpr (kHostDiagnostics)
		{
			if (m_applyWindow.frames >= tickDriverPresenter::kWindowFrames)
				logRenderApplyWindow(controls, worldHost);
		}
	}

	template <TickDriverRenderSink SinkT, typename WorldHostT>
	void overwriteShadowPoses(const PresentControls& controls, SinkT& sink, WorldHostT& worldHost)
	{
		if (!worldHost.hasShadowWorld() || !m_renderPosesValid)
			return;

		worldHost.overwriteShadowPoses_GameThread(m_renderPoses,
			[this, &sink](auto&& each)
			{
				for (const auto& [id, target] : m_targets)
				{
					if (!tickDriverPresenter::positionOf(sink.readAnchor(id)).has_value())
						continue;
					each(static_cast<uint32_t>(id), target.slot);
				}
			},
			[this, &controls]([[maybe_unused]] uint32_t id, [[maybe_unused]] uint32_t declarationIndex,
				[[maybe_unused]] const RenderBody& pose, [[maybe_unused]] auto&& readBack)
			{
				if constexpr (kHostDiagnostics)
				{
					using tickDriverPresenter::maxOf;
					ApplyWindow& window = m_applyWindow;
					const glm::mat4 queried = readBack();
					const double error = glm::distance(glm::vec3(queried[3]), pose.position);
					double angleRad = 0.0;
					if (pose.hasRotation != 0u)
					{
						const glm::dquat queriedRotation(glm::normalize(glm::quat_cast(glm::mat3(queried))));
						const glm::dquat relative = glm::conjugate(glm::dquat(pose.rotation)) * queriedRotation;
						angleRad = 2.0 * tickDriverPresenter::atan2Of(glm::length(glm::dvec3(relative.x, relative.y, relative.z)),
							tickDriverPresenter::absOf(relative.w));
					}
					++window.shadowBodies;
					window.shadowExact += error == 0.0 ? 1u : 0u;
					window.shadowMaxError    = maxOf(window.shadowMaxError, error);
					window.shadowMaxAngleRad = maxOf(window.shadowMaxAngleRad, angleRad);
					if ((error > tickDriverPresenter::kShadowPoseTolerance || angleRad > tickDriverPresenter::kShadowPoseToleranceRadians)
						&& window.shadowOutOfTolerance++ == 0u && logEnabled(HostLogLevel::Warning))
					{
						m_engine.log(HostLogLevel::Warning, formatHostLine(m_line,
							"[SimHost.RenderSync] SHADOW id=%u declaration=%u queried=(%.*f,%.*f,%.*f) rendered=(%.*f,%.*f,%.*f) "
							"error%s=%.*f angleRad=%.9f: the shadow world the visualizations query is not at the rendered pose",
							id, declarationIndex, simFrame::lengthDecimals(6), static_cast<double>(queried[3].x),
							simFrame::lengthDecimals(6), static_cast<double>(queried[3].y),
							simFrame::lengthDecimals(6), static_cast<double>(queried[3].z),
							simFrame::lengthDecimals(6), static_cast<double>(pose.position.x),
							simFrame::lengthDecimals(6), static_cast<double>(pose.position.y),
							simFrame::lengthDecimals(6), static_cast<double>(pose.position.z),
							simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(9),
							tickDriverPresenter::reportedLength(controls, error), angleRad));
					}
				}
			});
	}

	template <TickDriverRenderSink SinkT>
	void checkRenderTargetsHeld(const PresentControls& controls, const SinkT& sink)
	{
		using tickDriverPresenter::maxOf;
		for (const auto& [id, target] : m_targets)
		{
			if (!target.applied)
				continue;
			const std::optional<glm::dvec3> planePoint = tickDriverPresenter::positionOf(sink.readAnchor(id));
			if (!planePoint.has_value())
				continue;

			const double drift = tickDriverPresenter::distanceOf(*planePoint, target.position);
			++m_applyWindow.held;
			m_applyWindow.maxHeldDrift = maxOf(m_applyWindow.maxHeldDrift, drift);
			if (drift > tickDriverPresenter::kHeldTolerance && m_applyWindow.moved++ == 0u && logEnabled(HostLogLevel::Warning))
			{
				m_engine.log(HostLogLevel::Warning, formatHostLine(m_line,
					"[SimHost.RenderApply] MOVED role=%s id=%u capsule=(%.*f,%.*f,%.*f) applied=(%.*f,%.*f,%.*f) drift%s=%.*f: "
					"something moved the capsule between the last render apply and this one",
					tickDriverPresenter::roleText(m_config.runsPrediction), id, simFrame::lengthDecimals(6), planePoint->x,
					simFrame::lengthDecimals(6), planePoint->y, simFrame::lengthDecimals(6), planePoint->z,
					simFrame::lengthDecimals(6), target.position.x, simFrame::lengthDecimals(6), target.position.y,
					simFrame::lengthDecimals(6), target.position.z, simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(9),
					tickDriverPresenter::reportedLength(controls, drift)));
			}
		}
	}

	template <TickDriverRenderSink SinkT>
	void checkRenderApply(const PresentControls& controls, const SinkT& sink)
	{
		using tickDriverPresenter::maxOf;
		ApplyWindow& window = m_applyWindow;
		const RenderInterpolationFrame& bracket = m_renderFrame;
		if (window.appliedFrames++ == 0u)
			window.firstAppliedStep = m_renderPoses.physicsStep;
		window.lastAppliedStep = m_renderPoses.physicsStep;
		window.interpolated += bracket.hasPrev ? 1u : 0u;
		window.newestMode   += bracket.newestOnly ? 1u : 0u;
		window.atNewest     += bracket.atNewest ? 1u : 0u;
		window.beyondOldest += bracket.beyondOldest ? 1u : 0u;
		window.snaps        += bracket.snaps;
		window.maxNextBack   = maxOf(window.maxNextBack, bracket.nextBack);

		for (const auto& [id, target] : m_targets)
		{
			const std::optional<glm::dvec3> planePoint = tickDriverPresenter::positionOf(sink.readAnchor(id));
			const RenderBody* const body = findRenderBody(m_renderPoses, static_cast<uint32_t>(id), m_config.rootDeclarationIndex);
			if (!planePoint.has_value() || body == nullptr)
			{
				++window.missing;
				continue;
			}

			const glm::dvec3 pose(body->position);
			const double error = tickDriverPresenter::distanceOf(*planePoint, pose);
			++window.applied;
			window.exact += tickDriverPresenter::exactlyEqual(*planePoint, pose) ? 1u : 0u;
			window.maxError = maxOf(window.maxError, error);
			if (!tickDriverPresenter::equalWithin(*planePoint, pose, tickDriverPresenter::kAppliedTolerance)
				&& window.outOfTolerance++ == 0u && logEnabled(HostLogLevel::Warning))
			{
				m_engine.log(HostLogLevel::Warning, formatHostLine(m_line,
					"[SimHost.RenderApply] MISMATCH role=%s id=%u capsule=(%.*f,%.*f,%.*f) rendered=(%.*f,%.*f,%.*f) error%s=%.*f physicsStep=%llu",
					tickDriverPresenter::roleText(m_config.runsPrediction), id, simFrame::lengthDecimals(6), planePoint->x,
					simFrame::lengthDecimals(6), planePoint->y, simFrame::lengthDecimals(6), planePoint->z,
					simFrame::lengthDecimals(6), pose.x, simFrame::lengthDecimals(6), pose.y, simFrame::lengthDecimals(6), pose.z,
					simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(9), tickDriverPresenter::reportedLength(controls, error),
					static_cast<unsigned long long>(m_renderPoses.physicsStep)));
			}
		}
	}

	template <TickDriverPresentClock ClockT>
	void traceRenderFrame(ClockT& clock, const PresentControls& controls)
	{
		if (!controls.traceFrames)
			return;

		const RenderInterpolationFrame& bracket = m_renderFrame;
		for (const auto& [id, target] : m_targets)
		{
			const RenderBody* const nextBody = findRenderBody(m_renderNext, static_cast<uint32_t>(id), m_config.rootDeclarationIndex);
			if (!target.applied || nextBody == nullptr)
				continue;
			const glm::dvec3 error = m_smoother.appliedErrorPosition(bracket, static_cast<uint32_t>(id), m_config.rootDeclarationIndex);
			if (!logEnabled(HostLogLevel::Log))
				continue;
			m_engine.log(HostLogLevel::Log, formatHostLine(m_line,
				"[SimHost.RenderTrace] role=%s frame=%llu id=%u pos=(%.*f,%.*f,%.*f) nextPos=(%.*f,%.*f,%.*f) host=%.6f "
				"render=%.6f mode=%s prev=%llu/%s next=%llu/%s back=%u alpha=%.4f newest=%llu err=(%.*f,%.*f,%.*f)",
				tickDriverPresenter::roleText(m_config.runsPrediction), static_cast<unsigned long long>(m_engine.frameNumber()), id,
				simFrame::lengthDecimals(4), target.position.x, simFrame::lengthDecimals(4), target.position.y,
				simFrame::lengthDecimals(4), target.position.z,
				simFrame::lengthDecimals(4), static_cast<double>(nextBody->position.x),
				simFrame::lengthDecimals(4), static_cast<double>(nextBody->position.y),
				simFrame::lengthDecimals(4), static_cast<double>(nextBody->position.z),
				clock.hostNowSeconds(), bracket.renderTimeSeconds, bracket.newestOnly ? "newest" : "interp",
				static_cast<unsigned long long>(bracket.prevStep), bracket.hasPrev ? tickDriverPresenter::stepKindText(bracket.prevKind) : "-",
				static_cast<unsigned long long>(bracket.nextStep), tickDriverPresenter::stepKindText(bracket.nextKind), bracket.nextBack,
				bracket.alpha, static_cast<unsigned long long>(bracket.newestStep), simFrame::lengthDecimals(4), error.x,
				simFrame::lengthDecimals(4), error.y, simFrame::lengthDecimals(4), error.z));
		}
	}

	template <typename WorldHostT>
	void logRenderApplyWindow(const PresentControls& controls, const WorldHostT& worldHost)
	{
		ApplyWindow& window = m_applyWindow;
		const char* displacement = "-";
		if (!m_frameDisplacement.empty())
		{
			std::sort(m_frameDisplacement.begin(), m_frameDisplacement.end());
			const uint32_t n = static_cast<uint32_t>(m_frameDisplacement.size());
			displacement = formatHostLine(m_displacementField, "%.*f/%.*f/%.*f",
				simFrame::lengthDecimals(4),
				tickDriverPresenter::reportedLength(controls, m_frameDisplacement[latencyBudget::nearestRankIndex(n, 500u)]),
				simFrame::lengthDecimals(4),
				tickDriverPresenter::reportedLength(controls, m_frameDisplacement[latencyBudget::nearestRankIndex(n, 990u)]),
				simFrame::lengthDecimals(4), tickDriverPresenter::reportedLength(controls, m_frameDisplacement.back()));
		}

		if (logEnabled(HostLogLevel::Log))
		{
			m_engine.log(HostLogLevel::Log, formatHostLine(m_line,
				"[SimHost.RenderApply] role=%s frames=%u appliedFrames=%u characters=%d applied=%u missing=%u exact=%u "
				"outOfTolerance=%u maxError%s=%.*f held=%u moved=%u maxHeldDrift%s=%.*f path%s=%.*f steps=[%llu,%llu] drops=%llu "
				"mode=%s delaySteps=%.2f interpolated=%u atNewest=%u beyondOldest=%u snaps=%u maxNextBack=%u disp%s=%s",
				tickDriverPresenter::roleText(m_config.runsPrediction), window.frames, window.appliedFrames,
				static_cast<int32_t>(m_targets.size()), window.applied, window.missing, window.exact,
				window.outOfTolerance, simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(9),
				tickDriverPresenter::reportedLength(controls, window.maxError), window.held, window.moved,
				simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(9), tickDriverPresenter::reportedLength(controls, window.maxHeldDrift),
				simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(3), tickDriverPresenter::reportedLength(controls, window.path),
				static_cast<unsigned long long>(window.firstAppliedStep), static_cast<unsigned long long>(window.lastAppliedStep),
				static_cast<unsigned long long>(m_snapshots.drops()),
				window.newestMode > 0u ? "newest" : "interp",
				tickDriverPresenter::clampedDelaySteps(controls),
				window.interpolated, window.atNewest, window.beyondOldest, window.snaps, window.maxNextBack,
				simFrame::kLengthFieldSuffix, displacement));
		}

		if (worldHost.hasShadowWorld() && logEnabled(HostLogLevel::Log))
		{
			m_engine.log(HostLogLevel::Log, formatHostLine(m_line,
				"[SimHost.RenderSync] role=Client frames=%u shadowBodies=%u shadowExact=%u shadowOutOfTolerance=%u "
				"shadowMaxError%s=%.*f shadowMaxAngleRad=%.6f",
				window.frames, window.shadowBodies, window.shadowExact, window.shadowOutOfTolerance,
				simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(6),
				tickDriverPresenter::reportedLength(controls, window.shadowMaxError), window.shadowMaxAngleRad));
		}

		if (m_config.runsPrediction)
			logRenderCorrectionWindow(controls);

		m_frameDisplacement.clear();
		window = ApplyWindow{};
	}

	void logRenderCorrectionWindow(const PresentControls& controls)
	{
		CorrectionWindow& window = m_correctionWindow;
		const uint64_t resims       = m_resims.load(std::memory_order_relaxed);
		const uint64_t captured     = m_captured.load(std::memory_order_relaxed);
		const uint64_t withoutError = m_withoutError.load(std::memory_order_relaxed);
		const uint64_t tickMismatch = m_tickMismatches.load(std::memory_order_relaxed);
		const uint64_t ringDrops    = m_corrections.drops();
		const float    seconds      = controls.correctionSeconds;
		const double   stepSeconds  = static_cast<double>(m_config.stepSeconds);
		const double   decayMeanS   = window.finished > 0u
			? static_cast<double>(window.finishedSteps) / static_cast<double>(window.finished) * stepSeconds : 0.0;

		if (logEnabled(HostLogLevel::Log))
		{
			m_engine.log(HostLogLevel::Log, formatHostLine(m_line,
				"[SimHost.RenderCorrection] role=Client frames=%u resims=%llu captured=%llu withoutError=%llu tickMismatches=%llu "
				"ringDrops=%llu corrections=%u smoothedBodies=%u snappedBodies=%u maxError%s=%.*f maxApplied%s=%.*f decays=%u "
				"decayMeanS=%.3f decayMaxS=%.3f activeFrames=%u late=%u pendingDrops=%u seconds=%.3f durationSteps=%d "
				"max%s=%.*f desyncS=%.2f policySnaps=%u maxPolicySnap%s=%.*f maxPolicySnapDeg=%.1f rootFramedFrames=%u "
				"straddleSnaps=%u",
				m_applyWindow.frames, static_cast<unsigned long long>(resims - window.resimsAtStart),
				static_cast<unsigned long long>(captured - window.capturedAtStart),
				static_cast<unsigned long long>(withoutError - window.withoutErrorAtStart),
				static_cast<unsigned long long>(tickMismatch - window.tickMismatchAtStart),
				static_cast<unsigned long long>(ringDrops - window.ringDropsAtStart), window.corrections, window.smoothedBodies,
				window.snappedBodies, simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(3),
				tickDriverPresenter::reportedLength(controls, window.maxError), simFrame::kLengthFieldSuffix,
				simFrame::lengthDecimals(3), tickDriverPresenter::reportedLength(controls, window.maxApplied), window.finished,
				decayMeanS, static_cast<double>(window.maxFinishedSteps) * stepSeconds, window.activeFrames, window.late,
				window.pendingDrops, static_cast<double>(seconds), renderCorrectionDurationSteps(seconds, m_config.stepSeconds),
				simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(1),
				tickDriverPresenter::reportedLength(controls, controls.maxCorrection), static_cast<double>(controls.desyncSeconds),
				window.policySnaps, simFrame::kLengthFieldSuffix, simFrame::lengthDecimals(3),
				tickDriverPresenter::reportedLength(controls, window.maxPolicySnap), window.maxPolicySnapDeg,
				window.rootFramedFrames, window.straddleSnaps));
		}

		window = CorrectionWindow{};
		window.resimsAtStart       = resims;
		window.capturedAtStart     = captured;
		window.withoutErrorAtStart = withoutError;
		window.tickMismatchAtStart = tickMismatch;
		window.ringDropsAtStart    = ringDrops;
	}

	EngineT&                      m_engine;
	SourceT&                      m_source;
	TickDriverPresenterConfig     m_config;

	Channel                       m_snapshots;
	TargetMap                     m_targets;
	Snapshot                      m_renderPoses{};
	bool                          m_renderPosesValid = false;
	RenderInterpolationFrame      m_renderFrame;
	Snapshot                      m_renderNext{};
	ApplyWindow                   m_applyWindow;
	std::vector<float, FloatAllocator> m_frameDisplacement;

	Capture                       m_capture{};
	CorrectionRing                m_corrections;
	std::atomic<uint64_t>         m_resims{ 0u };
	std::atomic<uint64_t>         m_captured{ 0u };
	std::atomic<uint64_t>         m_withoutError{ 0u };
	std::atomic<uint64_t>         m_tickMismatches{ 0u };
	Smoother                      m_smoother;
	RenderCorrectionFrame         m_correctionFrame;
	CorrectionWindow              m_correctionWindow;

	BasicHostLine<Allocator>      m_line;
	BasicHostLine<Allocator>      m_displacementField;
};

template <typename Snapshot, TickDriverEngine EngineT, TickDriverRenderSource SourceT>
using TickDriverPresenter = BasicTickDriverPresenter<Snapshot, EngineT, SourceT>;

namespace tickDriverPresenterSelfCheck
{
using Presenter = TickDriverPresenter<RenderSnapshotT<4>, tickDriverEngineSelfCheck::Conforming,
	tickDriverRenderSourceSelfCheck::Conforming>;

static_assert(TickDriverPresenterSteps<Presenter>,
	"TickDriverPresenter must model TickDriverPresenterSteps: the step hooks call captureCorrection_Step and publish_Step");
static_assert(!std::is_copy_constructible_v<Presenter>,
	"TickDriverPresenter is not copyable: the step hooks and the game-thread doors share one J5 channel and one J7 ring");
} // namespace tickDriverPresenterSelfCheck
