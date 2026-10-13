#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/RenderCorrectionSmoothing-rationale.md

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>

#include "glm/geometric.hpp"
#include "glm/gtc/quaternion.hpp"
#include "glm/vec3.hpp"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/PhysicsBodyState.h"
#include "OGSimulation/PhysicsDeclaration.h"
#include "OGSimulation/RenderInterpolation.h"
#include "OGSimulation/RenderSnapshot.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationTimeContext.h"
#include "OGSimulation/StepHooks.h"

constexpr double kRenderCorrectionMinErrorCm = 0.1;
constexpr double kRenderCorrectionMinRotationError = 0.02;

struct RenderCorrectionBody
{
	uint32_t simulatableId = 0;
	uint8_t declarationIndex = 0;
	uint8_t hasRotation = 0;
	float speedCmPerSecond = 0.f;
	glm::dvec3 errorCm{ 0.0 };
	glm::dquat errorRotation{ 1.0, 0.0, 0.0, 0.0 };
};

template <size_t MaxBodies>
struct RenderCorrectionT
{
	static constexpr size_t kMaxBodies = MaxBodies;

	uint64_t physicsStep = 0;
	SimTick tick = 0;
	uint16_t bodyCount = 0;
	std::array<RenderCorrectionBody, MaxBodies> bodies{};
};

struct RenderCorrectionParams
{
	float correctionSeconds = 0.3f;
	float maxCorrectionCm = 250.f;
	float desyncSeconds = 0.6f;
	float stepSeconds = 0.f;
};

struct RenderCorrectionFrame
{
	uint32_t corrections = 0;
	uint32_t smoothedBodies = 0;
	uint32_t snappedBodies = 0;
	uint32_t late = 0;
	uint32_t pendingDrops = 0;
	uint32_t finished = 0;
	uint64_t finishedSteps = 0;
	uint32_t maxFinishedSteps = 0;
	uint32_t active = 0;
	double maxErrorCm = 0.0;
	double maxAppliedCm = 0.0;
	uint32_t policySnaps = 0;
	uint32_t rootFramed = 0;
	double maxPolicySnapCm = 0.0;
	double maxPolicySnapDeg = 0.0;
	uint32_t straddleSnaps = 0;
};

static_assert(std::is_trivially_copyable_v<RenderCorrectionBody> && std::is_trivially_copyable_v<RenderCorrectionParams>
		&& std::is_trivially_copyable_v<RenderCorrectionFrame>,
	"The render correction types must stay flat values with no heap-owning member: a correction is copied through a "
	"lock-free ring once per resimulation and the smoothing runs once per rendered frame, allocating nothing. Was "
	"task 19's allocation-free render path, extended by task 57.");

namespace renderCorrectionDetail
{
	inline bool isNearlyZero(const glm::dvec3& value, double tolerance)
	{
		return std::abs(value.x) <= tolerance && std::abs(value.y) <= tolerance && std::abs(value.z) <= tolerance;
	}

	inline bool isIdentity(const glm::dquat& value, double tolerance)
	{
		return (std::abs(value.x) <= tolerance && std::abs(value.y) <= tolerance && std::abs(value.z) <= tolerance
				   && std::abs(value.w - 1.0) <= tolerance)
			|| (std::abs(value.x) <= tolerance && std::abs(value.y) <= tolerance && std::abs(value.z) <= tolerance
				&& std::abs(value.w + 1.0) <= tolerance);
	}

	inline glm::dquat normalized(const glm::dquat& value)
	{
		const double squareSum = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
		if (squareSum >= 1e-8)
		{
			const double scale = 1.0 / std::sqrt(squareSum);
			return glm::dquat(value.w * scale, value.x * scale, value.y * scale, value.z * scale);
		}
		return glm::dquat(1.0, 0.0, 0.0, 0.0);
	}

	inline glm::dquat slerp(const glm::dquat& from, const glm::dquat& to, double alpha)
	{
		double rawCosom = from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w;
		const double sign = rawCosom >= 0.0 ? 1.0 : -1.0;
		rawCosom *= sign;

		double scale0 = 1.0 - alpha;
		double scale1 = alpha * sign;
		if (rawCosom < 0.9999)
		{
			const double omega = std::acos(rawCosom);
			const double invSin = 1.0 / std::sin(omega);
			scale0 = std::sin(scale0 * omega) * invSin;
			scale1 = std::sin(scale1 * omega) * invSin;
		}
		return normalized(glm::dquat(scale0 * from.w + scale1 * to.w, scale0 * from.x + scale1 * to.x,
			scale0 * from.y + scale1 * to.y, scale0 * from.z + scale1 * to.z));
	}

	inline glm::dvec3 lerp(const glm::dvec3& from, const glm::dvec3& to, double alpha)
	{
		return from + alpha * (to - from);
	}

	inline bool keyLess(const RenderCorrectionBody& body, uint32_t simulatableId, uint8_t declarationIndex)
	{
		return body.simulatableId != simulatableId ? body.simulatableId < simulatableId : body.declarationIndex < declarationIndex;
	}

	inline double rotationDegrees(const glm::dquat& rotation)
	{
		return 2.0 * std::acos(std::min(1.0, std::abs(rotation.w))) * (180.0 / std::numbers::pi);
	}

	inline bool snapsInRootFrame(const RenderBody& body)
	{
		return body.renderCorrection == static_cast<uint8_t>(RenderCorrectionMode::SnapInRootFrame);
	}
} // namespace renderCorrectionDetail

inline int32_t renderCorrectionDurationSteps(float correctionSeconds, float stepSeconds)
{
	if (!(stepSeconds > 0.f))
	{
		return 0;
	}
	const double steps = std::floor(static_cast<double>(correctionSeconds) / static_cast<double>(stepSeconds));
	if (!(steps > static_cast<double>(std::numeric_limits<int32_t>::min())))
	{
		return std::numeric_limits<int32_t>::min();
	}
	return steps < static_cast<double>(std::numeric_limits<int32_t>::max()) ? static_cast<int32_t>(steps)
	                                                                       : std::numeric_limits<int32_t>::max();
}

template <size_t MaxBodies>
uint16_t captureRenderCorrection(const RenderSnapshotT<MaxBodies>& shown, const RenderSnapshotT<MaxBodies>& corrected,
	uint64_t physicsStep, RenderCorrectionT<MaxBodies>& correction)
{
	correction.physicsStep = physicsStep;
	correction.tick = corrected.tick;

	uint16_t count = 0;
	for (uint16_t index = 0; index < corrected.bodyCount; ++index)
	{
		const RenderBody& after = corrected.bodies[index];
		const RenderBody* const before = findRenderBody(shown, after.simulatableId, after.declarationIndex);
		if (before == nullptr)
		{
			continue;
		}

		const glm::dvec3 errorCm = glm::dvec3(before->positionCm) - glm::dvec3(after.positionCm);
		glm::dquat errorRotation(1.0, 0.0, 0.0, 0.0);
		if (after.hasRotation != 0u)
		{
			errorRotation = glm::conjugate(glm::dquat(after.rotation)) * glm::dquat(before->rotation);
			if (errorRotation.w < 0.0)
			{
				errorRotation = -errorRotation;
			}
			errorRotation = renderCorrectionDetail::normalized(errorRotation);
		}

		if (renderCorrectionDetail::isNearlyZero(errorCm, kRenderCorrectionMinErrorCm)
			&& renderCorrectionDetail::isIdentity(errorRotation, kRenderCorrectionMinRotationError))
		{
			continue;
		}

		RenderCorrectionBody& out = correction.bodies[count++];
		out.simulatableId = after.simulatableId;
		out.declarationIndex = after.declarationIndex;
		out.hasRotation = after.hasRotation;
		out.speedCmPerSecond = 0.f;
		out.errorCm = errorCm;
		out.errorRotation = errorRotation;
	}
	correction.bodyCount = count;
	return count;
}

template <size_t MaxBodies, typename... SimulatableTs>
void recordRenderCorrectionSpeeds(RenderCorrectionT<MaxBodies>& correction, const SimulationObjectStorage<SimulatableTs...>& storage)
{
	RenderCorrectionBody* const first = correction.bodies.data();
	RenderCorrectionBody* const last = first + correction.bodyCount;
	storage.forEachSimulatable([&](unsigned int simulatableId, const auto& simulatable) {
		const auto& state = simulatable.getAllState().getState();
		uint32_t declarationIndex = 0;
		simulatable.getPhysicsComposite().forEach([&](const auto& declaration) {
			using D = std::decay_t<decltype(declaration)>;
			using S = typename D::StateType;
			const uint32_t index = declarationIndex++;
			const uint32_t id = static_cast<uint32_t>(simulatableId);
			const uint8_t declarationKey = static_cast<uint8_t>(index);
			RenderCorrectionBody* const found = std::lower_bound(first, last, id,
				[declarationKey](const RenderCorrectionBody& body, uint32_t key) {
					return renderCorrectionDetail::keyLess(body, key, declarationKey);
				});
			if (found == last || found->simulatableId != id || found->declarationIndex != declarationKey)
			{
				return;
			}
			const PhysicsBodyState body = static_cast<PhysicsBodyState>(D::bodyStateOf(state.template get<S>()));
			found->speedCmPerSecond = glm::length(body.linearVelocity);
		});
	});
}

enum class RenderCorrectionCaptureOutcome : uint8_t
{
	NoNewResim,
	NoShownHead,
	TickMismatch,
	NoError,
	Captured
};

template <size_t MaxBodies>
class RenderCorrectionCapture
{
public:
	using Snapshot = RenderSnapshotT<MaxBodies>;
	using Correction = RenderCorrectionT<MaxBodies>;
	static constexpr size_t kMaxBodies = MaxBodies;

	void noteShownSnapshot(const Snapshot& published)
	{
		m_shownHead = published;
		m_shownHeadValid = true;
	}

	template <typename... SimulatableTs>
	RenderCorrectionCaptureOutcome onStepAfterReplay(uint64_t grantedResims, SimTick integratedTick, uint64_t physicsStep,
		const SimulationObjectStorage<SimulatableTs...>& storage, Correction& out)
	{
		if (grantedResims == m_resimsSeen)
		{
			return RenderCorrectionCaptureOutcome::NoNewResim;
		}
		m_resimsSeen = grantedResims;
		if (!m_shownHeadValid)
		{
			return RenderCorrectionCaptureOutcome::NoShownHead;
		}

		TickOutcome head;
		head.tick = integratedTick;
		head.physicsStep = physicsStep;
		fillRenderSnapshot(m_correctedHead, storage, head, 0.0);
		if (m_correctedHead.tick != m_shownHead.tick)
		{
			return RenderCorrectionCaptureOutcome::TickMismatch;
		}
		if (captureRenderCorrection(m_shownHead, m_correctedHead, physicsStep, out) == 0u)
		{
			return RenderCorrectionCaptureOutcome::NoError;
		}
		recordRenderCorrectionSpeeds(out, storage);
		return RenderCorrectionCaptureOutcome::Captured;
	}

	void reset()
	{
		m_shownHeadValid = false;
		m_resimsSeen = 0u;
	}

	bool hasShownSnapshot() const { return m_shownHeadValid; }
	uint64_t resimsSeen() const { return m_resimsSeen; }

private:
	Snapshot m_shownHead{};
	Snapshot m_correctedHead{};
	uint64_t m_resimsSeen = 0u;
	bool m_shownHeadValid = false;
};

static_assert(std::is_trivially_copyable_v<RenderCorrectionCapture<1>>,
	"RenderCorrectionCapture keeps the shown head, the corrected head and the resimulation count as flat members, so "
	"the step that captures allocates nothing. Was task 57's step-side capture members, moved here by task 68.");

template <size_t MaxBodies, size_t MaxPending = 8>
class RenderCorrectionSmoother
{
public:
	using Correction = RenderCorrectionT<MaxBodies>;
	using Snapshot = RenderSnapshotT<MaxBodies>;
	static constexpr size_t kMaxBodies = MaxBodies;
	static constexpr size_t kMaxPending = MaxPending;

	struct BodyError
	{
		uint32_t simulatableId = 0;
		uint8_t declarationIndex = 0;
		uint8_t hasRotation = 0;
		bool inUse = false;
		bool decaying = false;
		int64_t lastStep = 0;
		int64_t stepsSinceUpdate = 0;
		int64_t endDecayStep = 0;
		glm::dvec3 errorCm{ 0.0 };
		glm::dvec3 errorCmPrev{ 0.0 };
		glm::dquat errorRotation{ 1.0, 0.0, 0.0, 0.0 };
		glm::dquat errorRotationPrev{ 1.0, 0.0, 0.0, 0.0 };
		uint64_t correctionStep = 0;
		glm::dvec3 errorCmBeforeCorrection{ 0.0 };
		glm::dquat errorRotationBeforeCorrection{ 1.0, 0.0, 0.0, 0.0 };
		int64_t smoothingStartStep = 0;

		bool isSmoothing() const { return lastStep < endDecayStep; }
	};

	struct PolicyCarrier
	{
		uint32_t simulatableId = 0;
		uint8_t declarationIndex = 0;
		bool inUse = false;
		uint64_t correctionStep = 0;
	};

	static_assert(std::is_trivially_copyable_v<Correction> && std::is_trivially_copyable_v<BodyError>
			&& std::is_trivially_copyable_v<PolicyCarrier>,
		"RenderCorrectionSmoother keeps its corrections, per-body errors and policy carrier steps in fixed arrays of flat "
		"values, so a frame allocates nothing. Was task 19's allocation-free render path, extended by tasks 57 and 76.");

	template <typename Ring>
	void correctPoses(Ring& corrections, const RenderInterpolationFrame& frame, const RenderCorrectionParams& params,
		Snapshot& poses, RenderCorrectionFrame& summary, const Snapshot* next = nullptr)
	{
		RenderCorrectionFrame result;
		while (auto correction = corrections.tryPop())
		{
			if (m_hasLastNextStep && correction->physicsStep <= m_lastNextStep)
			{
				++result.late;
			}
			if (m_pendingCount == MaxPending)
			{
				++result.pendingDrops;
				continue;
			}
			m_pending[m_pendingCount++] = *correction;
		}

		releaseErrorsOfDepartedBodies(poses);

		const int64_t currentStep = static_cast<int64_t>(frame.nextStep);
		const int32_t durationSteps = renderCorrectionDurationSteps(params.correctionSeconds, params.stepSeconds);
		size_t kept = 0;
		for (size_t index = 0; index < m_pendingCount; ++index)
		{
			if (m_pending[index].physicsStep <= frame.nextStep)
			{
				accumulate(m_pending[index], currentStep, durationSteps, params, poses, result);
			}
			else
			{
				if (kept != index)
				{
					m_pending[kept] = m_pending[index];
				}
				++kept;
			}
		}
		m_pendingCount = kept;

		for (BodyError& error : m_errors)
		{
			if (error.inUse)
			{
				update(error, currentStep, result);
			}
		}
		m_lastNextStep = frame.nextStep;
		m_hasLastNextStep = true;

		takeCarrierRotationsOnTheStraddle(frame, next, poses, result);
		const double alpha = static_cast<double>(static_cast<float>(frame.alpha));
		drawSnappedBodiesInRootFrame(frame, alpha, poses, result);
		for (const BodyError& error : m_errors)
		{
			if (!error.inUse || !error.isSmoothing())
			{
				continue;
			}
			RenderBody* const body = findPoseBody(poses, error.simulatableId, error.declarationIndex);
			if (body == nullptr)
			{
				continue;
			}
			++result.active;
			const glm::dvec3 offset = appliedErrorCm(error, frame, alpha);
			body->positionCm = glm::vec3(glm::dvec3(body->positionCm) + offset);
			if (body->hasRotation != 0u)
			{
				body->rotation = glm::quat(glm::dquat(body->rotation) * appliedErrorRotation(error, frame, alpha));
			}
			result.maxAppliedCm = std::max(result.maxAppliedCm, glm::length(offset));
		}
		summary = result;
	}

	glm::dvec3 appliedErrorCm(const RenderInterpolationFrame& frame, uint32_t simulatableId, uint8_t declarationIndex) const
	{
		const BodyError* const error = findError(simulatableId, declarationIndex);
		if (error == nullptr || !error->isSmoothing())
		{
			return glm::dvec3(0.0);
		}
		return appliedErrorCm(*error, frame, static_cast<double>(static_cast<float>(frame.alpha)));
	}

	const BodyError* findError(uint32_t simulatableId, uint8_t declarationIndex) const
	{
		for (const BodyError& error : m_errors)
		{
			if (error.inUse && error.simulatableId == simulatableId && error.declarationIndex == declarationIndex)
			{
				return &error;
			}
		}
		return nullptr;
	}

	size_t pendingCount() const { return m_pendingCount; }

	void reset()
	{
		m_errors = {};
		m_policyCarriers = {};
		m_pendingCount = 0;
		m_lastNextStep = 0;
		m_hasLastNextStep = false;
	}

private:
	static RenderBody* findPoseBody(Snapshot& poses, uint32_t simulatableId, uint8_t declarationIndex)
	{
		return const_cast<RenderBody*>(findRenderBody(static_cast<const Snapshot&>(poses), simulatableId, declarationIndex));
	}

	static const RenderBody* findRootBody(const Snapshot& poses, uint32_t simulatableId)
	{
		RenderBody key;
		key.simulatableId = simulatableId;
		const RenderBody* const end = poses.bodies.data() + poses.bodyCount;
		const RenderBody* const first = std::lower_bound(poses.bodies.data(), end, key, &renderSnapshotDetail::keyLess);
		for (const RenderBody* body = first; body != end && body->simulatableId == simulatableId; ++body)
		{
			if (body->isRoot != 0u)
			{
				return body;
			}
		}
		return nullptr;
	}

	void drawSnappedBodiesInRootFrame(const RenderInterpolationFrame& frame, double alpha, Snapshot& poses, RenderCorrectionFrame& result) const
	{
		for (uint16_t index = 0; index < poses.bodyCount; ++index)
		{
			RenderBody& body = poses.bodies[index];
			if (!renderCorrectionDetail::snapsInRootFrame(body))
			{
				continue;
			}
			const RenderBody* const root = findRootBody(poses, body.simulatableId);
			OG_CHECK(root != nullptr, "RenderCorrectionSmoother: a body drawn in its root's frame has no root body in the pose set; its simulatable must have exactly one isRoot body");
			if (root == nullptr)
			{
				continue;
			}
			OG_CHECK(root->hasRotation == 0u, "RenderCorrectionSmoother: a body drawn in its root's frame has a root that carries a rotation; only a translating root's frame is composed, a rotating root's error would also have to turn the body's lever arm");
			const BodyError* const rootError = findError(root->simulatableId, root->declarationIndex);
			if (rootError != nullptr && rootError->isSmoothing())
			{
				body.positionCm = glm::vec3(glm::dvec3(body.positionCm) + appliedErrorCm(*rootError, frame, alpha));
				++result.rootFramed;
			}
		}
	}

	void takeCarrierRotationsOnTheStraddle(const RenderInterpolationFrame& frame, const Snapshot* next, Snapshot& poses,
		RenderCorrectionFrame& result)
	{
		for (PolicyCarrier& carrier : m_policyCarriers)
		{
			if (!carrier.inUse)
			{
				continue;
			}
			if (!frame.hasPrev || frame.prevStep >= carrier.correctionStep)
			{
				carrier = PolicyCarrier{};
				continue;
			}
			if (next == nullptr || carrier.correctionStep > frame.nextStep)
			{
				continue;
			}
			RenderBody* const body = findPoseBody(poses, carrier.simulatableId, carrier.declarationIndex);
			const RenderBody* const carried = findRenderBody(*next, carrier.simulatableId, carrier.declarationIndex);
			if (body == nullptr || carried == nullptr || body->hasRotation == 0u)
			{
				continue;
			}
			body->rotation = carried->rotation;
			++result.straddleSnaps;
		}
	}

	static bool straddlesCorrection(const BodyError& error, const RenderInterpolationFrame& frame)
	{
		return frame.hasPrev && frame.prevStep < error.correctionStep;
	}

	static glm::dvec3 appliedErrorCm(const BodyError& error, const RenderInterpolationFrame& frame, double alpha)
	{
		const glm::dvec3& from = straddlesCorrection(error, frame) ? error.errorCmBeforeCorrection : error.errorCmPrev;
		return renderCorrectionDetail::lerp(from, error.errorCm, alpha);
	}

	static glm::dquat appliedErrorRotation(const BodyError& error, const RenderInterpolationFrame& frame, double alpha)
	{
		const glm::dquat& from = straddlesCorrection(error, frame) ? error.errorRotationBeforeCorrection : error.errorRotationPrev;
		return renderCorrectionDetail::slerp(from, error.errorRotation, alpha);
	}

	void releaseErrorsOfDepartedBodies(const Snapshot& poses)
	{
		for (BodyError& error : m_errors)
		{
			if (error.inUse && findRenderBody(poses, error.simulatableId, error.declarationIndex) == nullptr)
			{
				error = BodyError{};
			}
		}
		for (PolicyCarrier& carrier : m_policyCarriers)
		{
			if (carrier.inUse && findRenderBody(poses, carrier.simulatableId, carrier.declarationIndex) == nullptr)
			{
				carrier = PolicyCarrier{};
			}
		}
	}

	void recordPolicyCarrier(uint32_t simulatableId, uint8_t declarationIndex, uint64_t correctionStep)
	{
		PolicyCarrier* free = nullptr;
		for (PolicyCarrier& carrier : m_policyCarriers)
		{
			if (carrier.inUse && carrier.simulatableId == simulatableId && carrier.declarationIndex == declarationIndex)
			{
				carrier.correctionStep = correctionStep;
				return;
			}
			if (!carrier.inUse && free == nullptr)
			{
				free = &carrier;
			}
		}
		OG_CHECK(free != nullptr, "RenderCorrectionSmoother: more bodies carry a policy carrier step than the pose set holds; "
			"every carrier of a body that left the pose set is released before a correction is accumulated");
		if (free != nullptr)
		{
			free->simulatableId = simulatableId;
			free->declarationIndex = declarationIndex;
			free->inUse = true;
			free->correctionStep = correctionStep;
		}
	}

	BodyError* findOrAddError(uint32_t simulatableId, uint8_t declarationIndex)
	{
		BodyError* free = nullptr;
		for (BodyError& error : m_errors)
		{
			if (error.inUse && error.simulatableId == simulatableId && error.declarationIndex == declarationIndex)
			{
				return &error;
			}
			if (!error.inUse && free == nullptr)
			{
				free = &error;
			}
		}
		OG_CHECK(free != nullptr, "RenderCorrectionSmoother: more bodies carry an error than the pose set holds; every error "
			"of a body that left the pose set is released before a correction is accumulated");
		if (free != nullptr)
		{
			*free = BodyError{};
			free->simulatableId = simulatableId;
			free->declarationIndex = declarationIndex;
			free->inUse = true;
		}
		return free;
	}

	void accumulate(const Correction& correction, int64_t currentStep, int32_t durationSteps, const RenderCorrectionParams& params,
		const Snapshot& poses, RenderCorrectionFrame& result)
	{
		++result.corrections;
		const double maxCorrectionSquared = static_cast<double>(params.maxCorrectionCm) * static_cast<double>(params.maxCorrectionCm);
		for (uint16_t index = 0; index < correction.bodyCount; ++index)
		{
			const RenderCorrectionBody& body = correction.bodies[index];
			const RenderBody* const poseBody = findRenderBody(poses, body.simulatableId, body.declarationIndex);
			if (poseBody == nullptr)
			{
				continue;
			}
			if (renderCorrectionDetail::snapsInRootFrame(*poseBody))
			{
				++result.policySnaps;
				result.maxPolicySnapCm = std::max(result.maxPolicySnapCm, glm::length(body.errorCm));
				result.maxPolicySnapDeg = std::max(result.maxPolicySnapDeg, renderCorrectionDetail::rotationDegrees(body.errorRotation));
				recordPolicyCarrier(body.simulatableId, body.declarationIndex, correction.physicsStep);
				continue;
			}
			BodyError* const error = findOrAddError(body.simulatableId, body.declarationIndex);
			if (error == nullptr)
			{
				continue;
			}

			const double errorSquared = glm::dot(body.errorCm, body.errorCm);
			const double desyncCm = static_cast<double>(body.speedCmPerSecond) * static_cast<double>(params.desyncSeconds);
			const bool withinDesync = params.desyncSeconds > 0.f && errorSquared < desyncCm * desyncCm;
			const int32_t steps = errorSquared < maxCorrectionSquared || withinDesync ? durationSteps : 0;
			result.maxErrorCm = std::max(result.maxErrorCm, std::sqrt(errorSquared));

			const bool wasSmoothing = error->isSmoothing();
			error->errorCmBeforeCorrection = wasSmoothing ? error->errorCm : glm::dvec3(0.0);
			error->errorRotationBeforeCorrection = wasSmoothing ? error->errorRotation : glm::dquat(1.0, 0.0, 0.0, 0.0);
			error->correctionStep = correction.physicsStep;
			error->smoothingStartStep = currentStep;
			error->hasRotation = body.hasRotation;

			error->lastStep = currentStep - 1;
			error->endDecayStep = error->lastStep + steps;
			error->stepsSinceUpdate = 0;
			if (error->isSmoothing())
			{
				error->errorCm += body.errorCm;
				error->errorCmPrev = error->errorCm;
				error->errorRotation = error->errorRotation * body.errorRotation;
				error->errorRotationPrev = error->errorRotation;
				error->decaying = true;
				++result.smoothedBodies;
			}
			else
			{
				resetError(*error);
				++result.snappedBodies;
			}
		}
	}

	static void resetError(BodyError& error)
	{
		error.errorCm = glm::dvec3(0.0);
		error.errorCmPrev = glm::dvec3(0.0);
		error.errorRotation = glm::dquat(1.0, 0.0, 0.0, 0.0);
		error.errorRotationPrev = glm::dquat(1.0, 0.0, 0.0, 0.0);
		error.endDecayStep = 0;
		error.lastStep = 0;
		error.stepsSinceUpdate = 0;
		error.decaying = false;
	}

	static void update(BodyError& error, int64_t currentStep, RenderCorrectionFrame& result)
	{
		error.stepsSinceUpdate = currentStep - error.lastStep;
		error.lastStep = currentStep;
		if (error.stepsSinceUpdate <= 0)
		{
			return;
		}
		if (!error.isSmoothing())
		{
			if (error.decaying)
			{
				const int64_t steps = currentStep - error.smoothingStartStep;
				++result.finished;
				result.finishedSteps += static_cast<uint64_t>(std::max<int64_t>(steps, 0));
				result.maxFinishedSteps = std::max(result.maxFinishedSteps, static_cast<uint32_t>(std::max<int64_t>(steps, 0)));
			}
			error = BodyError{};
			return;
		}

		const int64_t smoothingCount = error.endDecayStep - error.lastStep;
		int64_t ticks = std::clamp<int64_t>(error.stepsSinceUpdate, 0, smoothingCount);
		for (; ticks > 0; --ticks)
		{
			float alpha = static_cast<float>(smoothingCount - 1) / static_cast<float>(smoothingCount);
			alpha = std::clamp(alpha, 0.f, 1.f);

			error.errorCmPrev = error.errorCm;
			error.errorCm *= static_cast<double>(alpha);
			error.errorRotationPrev = error.errorRotation;
			error.errorRotation = renderCorrectionDetail::slerp(glm::dquat(1.0, 0.0, 0.0, 0.0), error.errorRotation, static_cast<double>(alpha));
		}
	}

	std::array<BodyError, MaxBodies> m_errors{};
	std::array<PolicyCarrier, MaxBodies> m_policyCarriers{};
	std::array<Correction, MaxPending> m_pending{};
	size_t m_pendingCount = 0;
	uint64_t m_lastNextStep = 0;
	bool m_hasLastNextStep = false;
};
