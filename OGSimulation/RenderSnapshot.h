#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/RenderSnapshot-rationale.md · docs/RenderSnapshot-guards.md

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "glm/gtc/quaternion.hpp"
#include "glm/vec3.hpp"
#include "OGSimulation/OGAssert.h"
#include "OGSimulation/PhysicsBodyState.h"
#include "OGSimulation/PhysicsWorldAdapter.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationTimeContext.h"
#include "OGSimulation/StepHooks.h"

struct RenderBody
{
	uint32_t simulatableId = 0;
	uint8_t declarationIndex = 0;
	uint8_t hasRotation = 0;
	glm::vec3 positionCm{ 0.f };
	glm::quat rotation{ 1.f, 0.f, 0.f, 0.f };
};

template <size_t MaxBodies>
struct RenderSnapshotT
{
	static_assert(MaxBodies >= 1, "RenderSnapshotT: MaxBodies must be at least 1");
	static_assert(MaxBodies <= std::numeric_limits<uint16_t>::max(), "RenderSnapshotT: bodyCount is a uint16_t, so MaxBodies must fit in one");

	static constexpr size_t kMaxBodies = MaxBodies;

	uint64_t physicsStep = 0;
	double stepDeadlineSeconds = 0.0;
	SimTick tick = 0;
	StepKind kind = StepKind::Normal;
	bool hardResync = false;
	uint32_t replayedTicks = 0;
	uint16_t bodyCount = 0;
	std::array<RenderBody, MaxBodies> bodies{};
};

namespace renderSnapshotDetail
{
	constexpr bool keyLess(const RenderBody& first, const RenderBody& second)
	{
		return first.simulatableId != second.simulatableId ? first.simulatableId < second.simulatableId
			: first.declarationIndex < second.declarationIndex;
	}

	constexpr bool sameKey(const RenderBody& first, const RenderBody& second)
	{
		return first.simulatableId == second.simulatableId && first.declarationIndex == second.declarationIndex;
	}
} // namespace renderSnapshotDetail

template <size_t MaxBodies, typename... SimulatableTs>
void fillRenderSnapshot(RenderSnapshotT<MaxBodies>& snapshot,
	const SimulationObjectStorage<SimulatableTs...>& storage,
	const TickOutcome& outcome,
	double stepDeadlineSeconds)
{
	snapshot.physicsStep = outcome.physicsStep;
	snapshot.stepDeadlineSeconds = stepDeadlineSeconds;
	snapshot.tick = outcome.tick;
	snapshot.kind = outcome.kind;
	snapshot.hardResync = outcome.hardResync;
	snapshot.replayedTicks = outcome.replayedTicks;

	size_t count = 0;
	storage.forEachSimulatable([&](unsigned int simulatableId, const auto& simulatable) {
		const auto& state = simulatable.getAllState().getState();
		uint32_t declarationIndex = 0;
		simulatable.getPhysicsComposite().forEach([&](const auto& declaration) {
			using D = std::decay_t<decltype(declaration)>;
			using S = typename D::StateType;
			using BodyStateT = std::remove_cvref_t<decltype(D::bodyStateOf(state.template get<S>()))>;
			// ⛔G-01  docs/RenderSnapshot-guards.md
			constexpr bool hasRotation = std::is_same_v<BodyStateT, PhysicsBodyState>;

			const uint32_t index = declarationIndex++;
			OG_CHECK(index <= std::numeric_limits<uint8_t>::max(), "fillRenderSnapshot: a physics composite has more than 256 declarations; RenderBody::declarationIndex is a uint8_t");
			OG_CHECK(count < MaxBodies, "fillRenderSnapshot: the storage holds more bodies than the snapshot's MaxBodies; the excess is dropped");
			if (count >= MaxBodies)
			{
				return;
			}

			const PhysicsBodyState body = static_cast<PhysicsBodyState>(D::bodyStateOf(state.template get<S>()));
			RenderBody& out = snapshot.bodies[count++];
			out.simulatableId = static_cast<uint32_t>(simulatableId);
			out.declarationIndex = static_cast<uint8_t>(index);
			out.hasRotation = hasRotation ? uint8_t{ 1 } : uint8_t{ 0 };
			out.positionCm = body.position;
			out.rotation = hasRotation ? body.rotation : glm::quat(1.f, 0.f, 0.f, 0.f);
		});
	});

	// ⛔G-02  docs/RenderSnapshot-guards.md
	std::sort(snapshot.bodies.begin(), snapshot.bodies.begin() + static_cast<std::ptrdiff_t>(count), &renderSnapshotDetail::keyLess);
	for (size_t index = 1; index < count; ++index)
	{
		OG_CHECK(!renderSnapshotDetail::sameKey(snapshot.bodies[index - 1], snapshot.bodies[index]),
			"fillRenderSnapshot: two bodies share a (simulatableId, declarationIndex) key; storage ids must be unique across simulatable types");
	}
	snapshot.bodyCount = static_cast<uint16_t>(count);
}
