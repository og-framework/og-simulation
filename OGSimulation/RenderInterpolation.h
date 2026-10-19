#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/RenderInterpolation-rationale.md · docs/RenderInterpolation-guards.md

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "glm/geometric.hpp"
#include "glm/gtc/quaternion.hpp"
#include "glm/vec3.hpp"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/RenderSnapshot.h"
#include "OGSimulation/SimulationTimeContext.h"

struct RenderInterpolationParams
{
	double renderTimeSeconds = 0.0;
	float snapDistanceCm = std::numeric_limits<float>::infinity();
	bool newestOnly = false;
};

struct RenderInterpolationFrame
{
	double renderTimeSeconds = 0.0;
	double alpha = 1.0;
	uint64_t prevStep = 0;
	uint64_t nextStep = 0;
	uint64_t newestStep = 0;
	StepKind prevKind = StepKind::Normal;
	StepKind nextKind = StepKind::Normal;
	uint32_t nextBack = 0;
	uint32_t snaps = 0;
	bool hasPrev = false;
	bool newestOnly = false;
	bool atNewest = false;
	bool beyondOldest = false;
};

static_assert(std::is_trivially_copyable_v<RenderInterpolationParams> && std::is_trivially_copyable_v<RenderInterpolationFrame>,
	"RenderInterpolationParams and RenderInterpolationFrame must stay flat values with no heap-owning member: the "
	"interpolation runs once per rendered frame and allocates nothing. Was task 19's allocation-free render path.");

template <size_t MaxBodies>
const RenderBody* findRenderBody(const RenderSnapshotT<MaxBodies>& snapshot, uint32_t simulatableId, uint8_t declarationIndex)
{
	RenderBody key;
	key.simulatableId = simulatableId;
	key.declarationIndex = declarationIndex;
	const RenderBody* const end = snapshot.bodies.data() + snapshot.bodyCount;
	const RenderBody* const found = std::lower_bound(snapshot.bodies.data(), end, key, &renderSnapshotDetail::keyLess);
	return found != end && renderSnapshotDetail::sameKey(*found, key) ? found : nullptr;
}

inline double renderInterpolationAlpha(double prevDeadlineSeconds, double nextDeadlineSeconds, double renderTimeSeconds)
{
	const double span = nextDeadlineSeconds - prevDeadlineSeconds;
	if (!(span > 0.0))
	{
		return 1.0;
	}
	const double alpha = (renderTimeSeconds - prevDeadlineSeconds) / span;
	const double belowOne = alpha < 1.0 ? alpha : 1.0;
	return 0.0 < belowOne ? belowOne : 0.0;
}

namespace renderInterpolationDetail
{
	inline glm::vec3 blendPosition(const glm::vec3& prev, const glm::vec3& next, float alpha)
	{
		return alpha >= 1.f ? next : (alpha <= 0.f ? prev : prev + (next - prev) * alpha);
	}

	inline glm::quat blendRotation(const glm::quat& prev, const glm::quat& next, float alpha)
	{
		return alpha >= 1.f ? next : (alpha <= 0.f ? prev : glm::slerp(prev, next, alpha));
	}
} // namespace renderInterpolationDetail

template <size_t MaxBodies>
uint32_t blendRenderPoses(const RenderSnapshotT<MaxBodies>& prev, const RenderSnapshotT<MaxBodies>& next, double alpha,
	float snapDistanceCm, RenderSnapshotT<MaxBodies>& poses)
{
	OG_CHECK(&poses != &prev, "blendRenderPoses: the pose set is the prev snapshot; the copy of next would overwrite the poses it blends from");
	poses = next;

	const float bodyAlpha = static_cast<float>(alpha);
	uint32_t snaps = 0;
	for (uint16_t index = 0; index < poses.bodyCount; ++index)
	{
		RenderBody& body = poses.bodies[index];
		const RenderBody* const from = findRenderBody(prev, body.simulatableId, body.declarationIndex);
		if (from == nullptr)
		{
			continue;
		}
		if (glm::distance(from->positionCm, body.positionCm) > snapDistanceCm)
		{
			++snaps;
			continue;
		}
		body.positionCm = renderInterpolationDetail::blendPosition(from->positionCm, body.positionCm, bodyAlpha);
		if (body.hasRotation != 0u)
		{
			body.rotation = renderInterpolationDetail::blendRotation(from->rotation, body.rotation, bodyAlpha);
		}
	}
	return snaps;
}

template <typename Channel, size_t MaxBodies>
bool interpolateRenderPoses(Channel& channel, const RenderInterpolationParams& params, RenderSnapshotT<MaxBodies>& poses,
	RenderInterpolationFrame& frame, RenderSnapshotT<MaxBodies>* nextCopy = nullptr)
{
	using Snapshot = RenderSnapshotT<MaxBodies>;
	static_assert(std::is_same_v<std::remove_cvref_t<decltype(*channel.peekNewest(0))>, Snapshot>,
		"interpolateRenderPoses: the channel must carry the pose set's own RenderSnapshotT, so the pose set holds every body a snapshot can");
	static_assert(Channel::kMaxHeldKeepingNewest >= 2,
		"interpolateRenderPoses holds two render snapshots at once, the pair that brackets the render time, so the channel must "
		"keep its newest snapshot while the reader holds two: N >= H + 2 = 4 for a SnapshotChannel. Was design D section 8's "
		"'the reader holds at most the two snapshots that bracket the render time'.");
	static_assert(std::is_trivially_copyable_v<Snapshot>,
		"interpolateRenderPoses copies a snapshot into the caller's pose set once per frame; RenderSnapshotT must stay a flat "
		"value with no heap-owning member, or that copy allocates.");

	const Snapshot* const newest = channel.peekNewest(0);
	if (newest == nullptr)
	{
		return false;
	}

	RenderInterpolationFrame result;
	result.renderTimeSeconds = params.renderTimeSeconds;
	result.newestStep = newest->physicsStep;
	result.newestOnly = params.newestOnly;

	const Snapshot* next = newest;
	const Snapshot* prev = nullptr;
	if (!params.newestOnly && newest->stepDeadlineSeconds > params.renderTimeSeconds)
	{
		for (size_t back = 1; back < Channel::kCapacity; ++back)
		{
			const Snapshot* const older = channel.peekNewest(back);
			if (older == nullptr)
			{
				break;
			}
			if (older->stepDeadlineSeconds < params.renderTimeSeconds)
			{
				prev = older;
				break;
			}
			// ⛔G-01  docs/RenderInterpolation-guards.md
			channel.release(next);
			next = older;
			result.nextBack = static_cast<uint32_t>(back);
		}
		result.beyondOldest = prev == nullptr;
	}
	else
	{
		result.atNewest = !params.newestOnly && newest->stepDeadlineSeconds < params.renderTimeSeconds;
	}

	result.nextStep = next->physicsStep;
	result.nextKind = next->kind;
	if (nextCopy != nullptr)
	{
		*nextCopy = *next;
	}
	if (prev != nullptr)
	{
		result.alpha = renderInterpolationAlpha(prev->stepDeadlineSeconds, next->stepDeadlineSeconds, params.renderTimeSeconds);
		result.hasPrev = true;
		result.prevStep = prev->physicsStep;
		result.prevKind = prev->kind;
		result.snaps = blendRenderPoses(*prev, *next, result.alpha, params.snapDistanceCm, poses);
		channel.release(prev);
	}
	else
	{
		poses = *next;
	}
	channel.release(next);

	frame = result;
	return true;
}
