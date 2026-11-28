#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/TickDriverConcepts-rationale.md · docs/TickDriverConcepts-guards.md

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "glm/gtc/quaternion.hpp"
#include "glm/vec3.hpp"
#include "OGSimulation/PhysicsWorldAdapter.h"
#include "OGSimulation/StepHooks.h"
#include "OGSimulation/TickDriverGameTypes.h"
#include "OGTickDriver/TickDriverLog.h"
#include "OGTickDriver/TickDriverThreadContext.h"

namespace tickDriverConceptProbe
{
struct Task
{
	void operator()() const {}
};
} // namespace tickDriverConceptProbe

template <typename E>
concept TickDriverEngine = requires(E& e, const E& ce, HostLogLevel level, const char* line,
	const typename E::Handle& handle, typename E::Completion& completion, tickDriverConceptProbe::Task task)
{
	{ E::kHostDiagnostics } -> std::convertible_to<bool>;
	typename std::bool_constant<static_cast<bool>(E::kHostDiagnostics)>;
	{ ce.nowSeconds() }                                -> std::same_as<double>;
	{ ce.frameNumber() }                               -> std::same_as<uint64_t>;
	{ ce.logEnabled(level) }                           -> std::same_as<bool>;
	{ e.log(level, line) }                             -> std::same_as<void>;
	{ ce.isPending(handle) }                           -> std::same_as<bool>;
	{ e.waitFor(handle) }                              -> std::same_as<void>;
	{ e.dispatchAfter(handle, std::move(task)) }       -> std::same_as<typename E::Handle>;
	{ e.deferUntil(completion, handle, std::move(task)) } -> std::same_as<bool>;
	requires std::default_initializable<typename E::Handle> && std::copyable<typename E::Handle>;
};

namespace tickDriverEngineSelfCheck
{
struct Conforming
{
	static constexpr bool kHostDiagnostics = true;
	struct Handle {};
	struct Completion {};

	double nowSeconds() const;
	uint64_t frameNumber() const;
	bool logEnabled(HostLogLevel) const;
	void log(HostLogLevel, const char*);
	bool isPending(const Handle&) const;
	void waitFor(const Handle&);
	template <typename F> Handle dispatchAfter(const Handle&, F&&);
	template <typename F> bool deferUntil(Completion&, const Handle&, F&&);
};

struct RuntimeHostDiagnostics
{
	static inline bool kHostDiagnostics = true;
	struct Handle {};
	struct Completion {};

	double nowSeconds() const;
	uint64_t frameNumber() const;
	bool logEnabled(HostLogLevel) const;
	void log(HostLogLevel, const char*);
	bool isPending(const Handle&) const;
	void waitFor(const Handle&);
	template <typename F> Handle dispatchAfter(const Handle&, F&&);
	template <typename F> bool deferUntil(Completion&, const Handle&, F&&);
};

struct MissingDeferUntil
{
	static constexpr bool kHostDiagnostics = true;
	struct Handle {};
	struct Completion {};

	double nowSeconds() const;
	uint64_t frameNumber() const;
	bool logEnabled(HostLogLevel) const;
	void log(HostLogLevel, const char*);
	bool isPending(const Handle&) const;
	void waitFor(const Handle&);
	template <typename F> Handle dispatchAfter(const Handle&, F&&);
};

struct MutatingClock
{
	static constexpr bool kHostDiagnostics = true;
	struct Handle {};
	struct Completion {};

	double nowSeconds();
	uint64_t frameNumber() const;
	bool logEnabled(HostLogLevel) const;
	void log(HostLogLevel, const char*);
	bool isPending(const Handle&) const;
	void waitFor(const Handle&);
	template <typename F> Handle dispatchAfter(const Handle&, F&&);
	template <typename F> bool deferUntil(Completion&, const Handle&, F&&);
};

struct MoveOnlyHandle
{
	static constexpr bool kHostDiagnostics = true;
	struct Handle
	{
		Handle() = default;
		Handle(Handle&&) = default;
		Handle& operator=(Handle&&) = default;
	};
	struct Completion {};

	double nowSeconds() const;
	uint64_t frameNumber() const;
	bool logEnabled(HostLogLevel) const;
	void log(HostLogLevel, const char*);
	bool isPending(const Handle&) const;
	void waitFor(const Handle&);
	template <typename F> Handle dispatchAfter(const Handle&, F&&);
	template <typename F> bool deferUntil(Completion&, const Handle&, F&&);
};

static_assert(TickDriverEngine<Conforming>,
	"TickDriverEngine self-check: the reference shape must satisfy the concept");
static_assert(!TickDriverEngine<RuntimeHostDiagnostics>,
	"TickDriverEngine self-check: kHostDiagnostics must be a constant expression, because the driver branches on it with if constexpr");
static_assert(!TickDriverEngine<MissingDeferUntil>,
	"TickDriverEngine self-check: deferUntil (the end-of-frame deferral onto a completion event) is required");
static_assert(!TickDriverEngine<MutatingClock>,
	"TickDriverEngine self-check: nowSeconds must be callable on a const engine");
static_assert(!TickDriverEngine<MoveOnlyHandle>,
	"TickDriverEngine self-check: a Handle is copyable, because the driver keeps the pending handle and the blocking handle at once");
} // namespace tickDriverEngineSelfCheck

template <typename G>
concept TickDriverGame = requires(G& g, int32_t tick, int32_t steps, SimTick at, double seconds, TickRateDoor& rate)
{
	typename G::Manager;
	typename G::Storage;
	typename G::RenderSnapshot;
	{ g.manager() }                        -> std::same_as<typename G::Manager&>;
	{ g.storage() }                        -> std::same_as<typename G::Storage&>;
	{ g.beforePump(seconds, rate) }        -> std::same_as<void>;
	{ g.onFrameStepsDue(tick, steps) }     -> std::same_as<void>;
	{ g.afterInlineBatch(tick) }           -> std::same_as<void>;
	{ g.releaseInputsForAuthorityTick(at) } -> std::same_as<void>;
	{ g.onStepStarted(seconds) }           -> std::same_as<void>;
	{ g.onStepEnded() }                    -> std::same_as<void>;
	{ g.onPostPhysics() }                  -> std::same_as<void>;
};

namespace tickDriverGameSelfCheck
{
struct Manager {};
struct Storage {};
struct RenderSnapshot {};

struct Conforming
{
	using Manager = tickDriverGameSelfCheck::Manager;
	using Storage = tickDriverGameSelfCheck::Storage;
	using RenderSnapshot = tickDriverGameSelfCheck::RenderSnapshot;

	Manager& manager();
	Storage& storage();
	void beforePump(double, TickRateDoor&);
	void onFrameStepsDue(int32_t, int32_t);
	void afterInlineBatch(int32_t);
	void releaseInputsForAuthorityTick(SimTick);
	void onStepStarted(double);
	void onStepEnded();
	void onPostPhysics();
};

struct MissingBeforePump
{
	using Manager = tickDriverGameSelfCheck::Manager;
	using Storage = tickDriverGameSelfCheck::Storage;
	using RenderSnapshot = tickDriverGameSelfCheck::RenderSnapshot;

	Manager& manager();
	Storage& storage();
	void onFrameStepsDue(int32_t, int32_t);
	void afterInlineBatch(int32_t);
	void releaseInputsForAuthorityTick(SimTick);
	void onStepStarted(double);
	void onStepEnded();
	void onPostPhysics();
};

struct MissingAfterInlineBatch
{
	using Manager = tickDriverGameSelfCheck::Manager;
	using Storage = tickDriverGameSelfCheck::Storage;
	using RenderSnapshot = tickDriverGameSelfCheck::RenderSnapshot;

	Manager& manager();
	Storage& storage();
	void beforePump(double, TickRateDoor&);
	void onFrameStepsDue(int32_t, int32_t);
	void releaseInputsForAuthorityTick(SimTick);
	void onStepStarted(double);
	void onStepEnded();
	void onPostPhysics();
};

struct HookReturnsBool
{
	using Manager = tickDriverGameSelfCheck::Manager;
	using Storage = tickDriverGameSelfCheck::Storage;
	using RenderSnapshot = tickDriverGameSelfCheck::RenderSnapshot;

	Manager& manager();
	Storage& storage();
	void beforePump(double, TickRateDoor&);
	bool onFrameStepsDue(int32_t, int32_t);
	void afterInlineBatch(int32_t);
	void releaseInputsForAuthorityTick(SimTick);
	void onStepStarted(double);
	void onStepEnded();
	void onPostPhysics();
};

struct ManagerByValue
{
	using Manager = tickDriverGameSelfCheck::Manager;
	using Storage = tickDriverGameSelfCheck::Storage;
	using RenderSnapshot = tickDriverGameSelfCheck::RenderSnapshot;

	Manager manager();
	Storage& storage();
	void beforePump(double, TickRateDoor&);
	void onFrameStepsDue(int32_t, int32_t);
	void afterInlineBatch(int32_t);
	void releaseInputsForAuthorityTick(SimTick);
	void onStepStarted(double);
	void onStepEnded();
	void onPostPhysics();
};

static_assert(TickDriverGame<Conforming>,
	"TickDriverGame self-check: the reference shape must satisfy the concept");
static_assert(!TickDriverGame<MissingBeforePump>,
	"TickDriverGame self-check: beforePump (the rate door before the pump) is required, even as a no-op");
static_assert(!TickDriverGame<MissingAfterInlineBatch>,
	"TickDriverGame self-check: afterInlineBatch (the game's work after an inline batch) is required");
static_assert(!TickDriverGame<HookReturnsBool>,
	"TickDriverGame self-check: a hook returns void, so it cannot veto or reorder the frame");
static_assert(!TickDriverGame<ManagerByValue>,
	"TickDriverGame self-check: manager() returns the game's own manager by reference, never a copy");
} // namespace tickDriverGameSelfCheck

template <typename G>
concept TickDriverSampleGame = TickDriverGame<G> && requires(G& g)
{
	{ g.sampleInput() } -> std::same_as<void>;
};

namespace tickDriverSampleGameSelfCheck
{
struct Conforming : tickDriverGameSelfCheck::Conforming
{
	void sampleInput();
};

struct MissingSampleInput : tickDriverGameSelfCheck::Conforming
{
};

struct SampleReturnsBool : tickDriverGameSelfCheck::Conforming
{
	bool sampleInput();
};

struct SampleTakesAnArgument : tickDriverGameSelfCheck::Conforming
{
	void sampleInput(double);
};

struct SamplesButIsNoGame
{
	void sampleInput();
};

static_assert(TickDriverSampleGame<Conforming>,
	"TickDriverSampleGame self-check: the reference shape must satisfy the concept");
static_assert(TickDriverGame<MissingSampleInput> && !TickDriverSampleGame<MissingSampleInput>,
	"TickDriverSampleGame self-check: sampleInput is required, and a game without it is still a TickDriverGame");
static_assert(!TickDriverSampleGame<SampleReturnsBool>,
	"TickDriverSampleGame self-check: sampleInput returns void, so it cannot veto or skip the frame");
static_assert(!TickDriverSampleGame<SampleTakesAnArgument>,
	"TickDriverSampleGame self-check: sampleInput takes nothing; the frame's time is read after it");
static_assert(!TickDriverSampleGame<SamplesButIsNoGame>,
	"TickDriverSampleGame self-check: it refines TickDriverGame, so a sampleInput alone is not a game");
} // namespace tickDriverSampleGameSelfCheck

template <typename G>
concept TickDriverReceiveGame = TickDriverGame<G> && requires(G& g)
{
	{ g.receiveForStep() } -> std::same_as<void>;
};

namespace tickDriverReceiveGameSelfCheck
{
struct Conforming : tickDriverGameSelfCheck::Conforming
{
	void receiveForStep();
};

struct MissingReceiveForStep : tickDriverGameSelfCheck::Conforming
{
};

struct ReceiveReturnsBool : tickDriverGameSelfCheck::Conforming
{
	bool receiveForStep();
};

struct ReceiveTakesAnArgument : tickDriverGameSelfCheck::Conforming
{
	void receiveForStep(double);
};

struct ReceivesButIsNoGame
{
	void receiveForStep();
};

struct SamplesButDoesNotReceive : tickDriverSampleGameSelfCheck::Conforming
{
};

static_assert(TickDriverReceiveGame<Conforming>,
	"TickDriverReceiveGame self-check: the reference shape must satisfy the concept");
static_assert(TickDriverGame<MissingReceiveForStep> && !TickDriverReceiveGame<MissingReceiveForStep>,
	"TickDriverReceiveGame self-check: receiveForStep is required, and a game without it is still a TickDriverGame");
static_assert(!TickDriverReceiveGame<ReceiveReturnsBool>,
	"TickDriverReceiveGame self-check: receiveForStep returns void, so it cannot veto or skip the step");
static_assert(!TickDriverReceiveGame<ReceiveTakesAnArgument>,
	"TickDriverReceiveGame self-check: receiveForStep takes nothing; the step's tick is decided after it");
static_assert(!TickDriverReceiveGame<ReceivesButIsNoGame>,
	"TickDriverReceiveGame self-check: it refines TickDriverGame, so a receiveForStep alone is not a game");
static_assert(TickDriverSampleGame<SamplesButDoesNotReceive> && !TickDriverReceiveGame<SamplesButDoesNotReceive>,
	"TickDriverReceiveGame self-check: the per-frame sampleInput is not the per-step receiveForStep; each is opted into alone");
} // namespace tickDriverReceiveGameSelfCheck

template <typename G>
concept TickDriverDisplayGame = TickDriverGame<G> && requires(G& g)
{
	{ g.publishForDisplay() } -> std::same_as<void>;
};

namespace tickDriverDisplayGameSelfCheck
{
struct Conforming : tickDriverGameSelfCheck::Conforming
{
	void publishForDisplay();
};

struct MissingPublishForDisplay : tickDriverGameSelfCheck::Conforming
{
};

struct PublishReturnsBool : tickDriverGameSelfCheck::Conforming
{
	bool publishForDisplay();
};

struct PublishTakesAnArgument : tickDriverGameSelfCheck::Conforming
{
	void publishForDisplay(int32_t);
};

struct PublishesButIsNoGame
{
	void publishForDisplay();
};

struct ReceivesButDoesNotPublish : tickDriverReceiveGameSelfCheck::Conforming
{
};

static_assert(TickDriverDisplayGame<Conforming>,
	"TickDriverDisplayGame self-check: the reference shape must satisfy the concept");
static_assert(TickDriverGame<MissingPublishForDisplay> && !TickDriverDisplayGame<MissingPublishForDisplay>,
	"TickDriverDisplayGame self-check: publishForDisplay is required, and a game without it is still a TickDriverGame");
static_assert(!TickDriverDisplayGame<PublishReturnsBool>,
	"TickDriverDisplayGame self-check: publishForDisplay returns void, so it cannot veto or repeat the batch");
static_assert(!TickDriverDisplayGame<PublishTakesAnArgument>,
	"TickDriverDisplayGame self-check: publishForDisplay takes nothing; the game publishes its own state as the batch left it");
static_assert(!TickDriverDisplayGame<PublishesButIsNoGame>,
	"TickDriverDisplayGame self-check: it refines TickDriverGame, so a publishForDisplay alone is not a game");
static_assert(TickDriverReceiveGame<ReceivesButDoesNotPublish> && !TickDriverDisplayGame<ReceivesButDoesNotPublish>,
	"TickDriverDisplayGame self-check: the per-step receiveForStep is not the per-batch publishForDisplay; each is opted into alone");
} // namespace tickDriverDisplayGameSelfCheck

template <typename G>
concept TickDriverIdleGame = TickDriverGame<G> && requires(G& g, double s)
{
	{ g.idleUntil(s) } -> std::same_as<IdleWake>;
};

namespace tickDriverIdleGameSelfCheck
{
struct Conforming : tickDriverGameSelfCheck::Conforming
{
	IdleWake idleUntil(double);
};

struct MissingIdleUntil : tickDriverGameSelfCheck::Conforming
{
};

struct IdleReturnsVoid : tickDriverGameSelfCheck::Conforming
{
	void idleUntil(double);
};

static_assert(TickDriverIdleGame<Conforming>,
	"TickDriverIdleGame self-check: the reference shape must satisfy the concept");
static_assert(TickDriverGame<MissingIdleUntil> && !TickDriverIdleGame<MissingIdleUntil>,
	"TickDriverIdleGame self-check: idleUntil is required, and a game without it is still a TickDriverGame");
static_assert(!TickDriverIdleGame<IdleReturnsVoid>,
	"TickDriverIdleGame self-check: idleUntil returns the IdleWake that says why the runner woke");
} // namespace tickDriverIdleGameSelfCheck

template <typename R>
concept TickDriverRunnerHost = requires(R& r, double s)
{
	{ r.sleepUntil(s) } -> std::same_as<void>;
};

namespace tickDriverRunnerHostSelfCheck
{
struct Conforming
{
	void sleepUntil(double);
};

struct MissingSleepUntil
{
};

struct SleepReturnsBool
{
	bool sleepUntil(double);
};

static_assert(TickDriverRunnerHost<Conforming>,
	"TickDriverRunnerHost self-check: the reference shape must satisfy the concept");
static_assert(!TickDriverRunnerHost<MissingSleepUntil>,
	"TickDriverRunnerHost self-check: sleepUntil (the runner's bounded sleep when the idle hook did not wait) is required");
static_assert(!TickDriverRunnerHost<SleepReturnsBool>,
	"TickDriverRunnerHost self-check: sleepUntil returns void; it sleeps to the deadline and reports nothing");
} // namespace tickDriverRunnerHostSelfCheck

template <typename P>
concept TickDriverPresenterSteps = requires(P& p, uint64_t step, const TickOutcome& outcome, double deadline)
{
	{ p.captureCorrection_Step(step) }    -> std::same_as<void>;
	{ p.publish_Step(outcome, deadline) } -> std::same_as<void>;
};

namespace tickDriverPresenterStepsSelfCheck
{
struct Conforming
{
	void captureCorrection_Step(uint64_t);
	void publish_Step(const TickOutcome&, double);
};

struct MissingCaptureCorrection
{
	void publish_Step(const TickOutcome&, double);
};

struct PublishTakesMutableOutcome
{
	void captureCorrection_Step(uint64_t);
	void publish_Step(TickOutcome&, double);
};

static_assert(TickDriverPresenterSteps<Conforming>,
	"TickDriverPresenterSteps self-check: the reference shape must satisfy the concept");
static_assert(!TickDriverPresenterSteps<MissingCaptureCorrection>,
	"TickDriverPresenterSteps self-check: captureCorrection_Step (the correction capture after a resim) is required");
static_assert(!TickDriverPresenterSteps<PublishTakesMutableOutcome>,
	"TickDriverPresenterSteps self-check: publish_Step reads the outcome and cannot rewrite it");
} // namespace tickDriverPresenterStepsSelfCheck

template <typename H>
concept TickDriverWorldHost = requires(H& h, uint64_t step, const TickOutcome& outcome)
{
	typename H::World;
	requires PhysicsWorldAdapter<typename H::World>;
	{ h.stepWorld() }                   -> std::same_as<typename H::World&>;
	{ h.lockForStep() };
	requires !std::is_void_v<decltype(h.lockForStep())>;
	{ h.afterTick_Step(outcome) }        -> std::same_as<void>;
	{ h.afterRunTick_Step(step, outcome) } -> std::same_as<void>;
	{ h.threadContext() }               -> std::same_as<const TickDriverThreadContext&>;
};

namespace tickDriverWorldHostSelfCheck
{
using World = physicsWorldAdapterSelfCheck::Conforming;

struct StepLock
{
	StepLock() = default;
	StepLock(const StepLock&) = delete;
	StepLock& operator=(const StepLock&) = delete;
};

struct Conforming
{
	using World = tickDriverWorldHostSelfCheck::World;

	World& stepWorld();
	StepLock lockForStep();
	void afterTick_Step(const TickOutcome&);
	void afterRunTick_Step(uint64_t, const TickOutcome&);
	const TickDriverThreadContext& threadContext() const;
};

struct MissingThreadContext
{
	using World = tickDriverWorldHostSelfCheck::World;

	World& stepWorld();
	StepLock lockForStep();
	void afterTick_Step(const TickOutcome&);
	void afterRunTick_Step(uint64_t, const TickOutcome&);
};

struct MutableThreadContext
{
	using World = tickDriverWorldHostSelfCheck::World;

	World& stepWorld();
	StepLock lockForStep();
	void afterTick_Step(const TickOutcome&);
	void afterRunTick_Step(uint64_t, const TickOutcome&);
	TickDriverThreadContext& threadContext();
};

struct LockForStepReturnsVoid
{
	using World = tickDriverWorldHostSelfCheck::World;

	World& stepWorld();
	void lockForStep();
	void afterTick_Step(const TickOutcome&);
	void afterRunTick_Step(uint64_t, const TickOutcome&);
	const TickDriverThreadContext& threadContext() const;
};

struct WorldIsNotAnAdapter
{
	struct World {};

	World& stepWorld();
	StepLock lockForStep();
	void afterTick_Step(const TickOutcome&);
	void afterRunTick_Step(uint64_t, const TickOutcome&);
	const TickDriverThreadContext& threadContext() const;
};

static_assert(TickDriverWorldHost<Conforming>,
	"TickDriverWorldHost self-check: the reference shape must satisfy the concept");
static_assert(!TickDriverWorldHost<MissingThreadContext>,
	"TickDriverWorldHost self-check: threadContext (the role bit, the mode and the access predicates) is required");
static_assert(!TickDriverWorldHost<MutableThreadContext>,
	"TickDriverWorldHost self-check: the driver reads the thread context and cannot reconfigure it");
static_assert(!TickDriverWorldHost<LockForStepReturnsVoid>,
	"TickDriverWorldHost self-check: lockForStep returns the scope that holds the world lock for the whole step");
static_assert(!TickDriverWorldHost<WorldIsNotAnAdapter>,
	"TickDriverWorldHost self-check: the step world is a PhysicsWorldAdapter, so the step driver can run over it");
} // namespace tickDriverWorldHostSelfCheck

template <typename T>
concept OptionalVec3Result = !std::is_void_v<T> && std::convertible_to<T, std::optional<glm::vec3>>;

template <typename S>
concept TickDriverRenderSink = requires(S& s, const S& cs, uint32_t id, const glm::vec3& position,
	const std::optional<glm::quat>& rotation)
{
	{ s.applyRootPose(id, position, rotation) } -> OptionalVec3Result;
	{ cs.readAnchor(id) }                       -> OptionalVec3Result;
	{ cs.name(id) }                             -> std::convertible_to<const char*>;
};

namespace tickDriverRenderSinkSelfCheck
{
struct Conforming
{
	std::optional<glm::vec3> applyRootPose(uint32_t, const glm::vec3&, const std::optional<glm::quat>&);
	std::optional<glm::vec3> readAnchor(uint32_t) const;
	const char* name(uint32_t) const;
};

struct AppliedPositionAlways
{
	glm::vec3 applyRootPose(uint32_t, const glm::vec3&, const std::optional<glm::quat>&);
	glm::vec3 readAnchor(uint32_t) const;
	const char* name(uint32_t) const;
};

struct MissingName
{
	std::optional<glm::vec3> applyRootPose(uint32_t, const glm::vec3&, const std::optional<glm::quat>&);
	std::optional<glm::vec3> readAnchor(uint32_t) const;
};

struct ApplyReturnsVoid
{
	void applyRootPose(uint32_t, const glm::vec3&, const std::optional<glm::quat>&);
	std::optional<glm::vec3> readAnchor(uint32_t) const;
	const char* name(uint32_t) const;
};

struct MutatingAnchorRead
{
	std::optional<glm::vec3> applyRootPose(uint32_t, const glm::vec3&, const std::optional<glm::quat>&);
	std::optional<glm::vec3> readAnchor(uint32_t);
	const char* name(uint32_t) const;
};

static_assert(TickDriverRenderSink<Conforming>,
	"TickDriverRenderSink self-check: the reference shape must satisfy the concept");
static_assert(TickDriverRenderSink<AppliedPositionAlways>,
	"TickDriverRenderSink self-check: a sink whose targets never go away may return the applied position directly");
static_assert(!TickDriverRenderSink<MissingName>,
	"TickDriverRenderSink self-check: name (the id's display name for the render lines) is required");
static_assert(!TickDriverRenderSink<ApplyReturnsVoid>,
	"TickDriverRenderSink self-check: applyRootPose returns the position the engine holds after the write");
static_assert(!TickDriverRenderSink<MutatingAnchorRead>,
	"TickDriverRenderSink self-check: readAnchor must be callable on a const sink");
} // namespace tickDriverRenderSinkSelfCheck
