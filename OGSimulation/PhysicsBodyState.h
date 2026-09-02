#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"
#include "OGSimulation/SimulationSerialization.h"
#include "OGSimulation/SimulationFieldDescriptors.h"
#include "OGSimulation/SimulationComparisonGlm.h"

// ---------------------------------------------------------------------------
// CHOICE RULE — which body-state type a sub-simulation's State should carry.
// The adapters always produce and consume the FULL PhysicsBodyState; the wire
// shape is the SIM's choice, not the engine's, so pick per body:
//
//   * free-rotating body                  -> PhysicsBodyState (52 B). Its
//                                            orientation and spin are part of
//                                            the simulated result.
//   * rotation-locked body                -> LinearBodyState  (24 B), below.
//                                            The descriptor MUST set
//                                            lockRotation; that flag is what
//                                            makes LinearBodyState's fabricated
//                                            identity rotation true rather than
//                                            a lie.
//   * body snapped from closed-form state -> keep it OFF the wire entirely
//     every tick                             (the projectile pattern: the pose
//                                            is re-derived, never corrected).
//
// Rationale, the exhaustive touch-point table and the new-sub-sim recipe:
// docs/PhysicsBodyState-rationale.md
// ---------------------------------------------------------------------------

// Pure wire-shape — every field is serialized and compared. The body identifier
// is NOT stored here; it lives in DerivedState's RuntimeBindings, which is
// local-only and immune to correction-state overwrites.
struct PhysicsBodyState
{
	glm::vec3 position        {0.f};                // 12 bytes
	glm::quat rotation        {1.f, 0.f, 0.f, 0.f}; // 16 bytes
	glm::vec3 linearVelocity  {0.f};                // 12 bytes
	glm::vec3 angularVelocity {0.f};                // 12 bytes
	// Wire size: 52 bytes
};

template <>
struct SerializableFields<PhysicsBodyState>
{
	static constexpr auto get()
	{
		return std::make_tuple(
			SIM_MEMBER(PhysicsBodyState, position),
			SIM_MEMBER(PhysicsBodyState, rotation),
			SIM_MEMBER(PhysicsBodyState, linearVelocity),
			SIM_MEMBER(PhysicsBodyState, angularVelocity));
	}
};

// ---------------------------------------------------------------------------
// LinearBodyState — the slim wire shape for a rotation-locked body
// ---------------------------------------------------------------------------

// Slim wire shape for a body whose rotation is locked at the descriptor
// (BodyDescriptor's lockRotation flag): position + linear velocity only, 24 B.
// Bridges to PhysicsBodyState by conversion so the executor's capture loop
// (`bodyStateOf(state) = adapter.captureBodyState(id)`) and the engine's rewind
// push (`pushBodyState(id, const PhysicsBodyState&)`) compile UNCHANGED — every
// adapter, Chaos and Jolt alike, keeps its `captureBodyState(id) ->
// PhysicsBodyState` contract and needs no per-body-state-type code.
struct LinearBodyState
{
	glm::vec3 position       {0.f}; // 12 bytes
	glm::vec3 linearVelocity {0.f}; // 12 bytes
	// Wire size: 24 bytes

	LinearBodyState& operator=(const LinearBodyState&) = default;

	// CAPTURE BRIDGE (narrowing). rotation and angularVelocity are DROPPED, by
	// design — this is the whole point of the slim shape.
	//
	// Deliberately an ASSIGNMENT and deliberately NOT a converting constructor:
	// capture only ever assigns into an existing member
	// (`D::bodyStateOf(state) = adapter.captureBodyState(id)`), so a constructor
	// would buy nothing while enabling silent lossy construction
	// (`LinearBodyState x = someFullState;`) and silent lossy pass-by-value.
	// Pinned by a `!std::is_constructible_v<...>` static_assert in
	// LinearBodyStateTest.cpp.
	LinearBodyState& operator=(const PhysicsBodyState& full)
	{
		position       = full.position;
		linearVelocity = full.linearVelocity;
		return *this;
	}

	// PUSH BRIDGE (widening). Fabricates rotation = identity, angularVelocity = 0.
	//
	// ⚠ SOUNDNESS CONDITION — read this before widening anything: the fabricated
	// identity rotation and zero angular velocity are SOUND ONLY FOR A BODY WHOSE
	// DESCRIPTOR SETS lockRotation = true. That flag is what makes the fabricated
	// values true rather than a lie. Widen a free-rotating body through here and
	// its orientation and spin die silently — carry PhysicsBodyState for such a
	// body instead (see the CHOICE RULE at the top of this file).
	//
	// DELIBERATELY IMPLICIT, and it must stay that way: the engine's rewind push
	// `pushBodyState(ownBodyId, D::bodyStateOf(state))` takes a
	// `const PhysicsBodyState&` at a GENERIC call site. The implicit conversion
	// there is the zero-edit property this bridge exists for; marking this
	// `explicit` forces a hand edit at that call site and defeats the design.
	operator PhysicsBodyState() const
	{
		return PhysicsBodyState{
			position,
			glm::quat{1.f, 0.f, 0.f, 0.f},
			linearVelocity,
			glm::vec3{0.f}};
	}
};

template <>
struct SerializableFields<LinearBodyState>
{
	static constexpr auto get()
	{
		return std::make_tuple(
			SIM_MEMBER(LinearBodyState, position),
			SIM_MEMBER(LinearBodyState, linearVelocity));
	}
};

// Satisfied by PhysicsBodyState and LinearBodyState. The two generic sites that
// touch a body state — the executor's capture loop and the engine's rewind push
// — need exactly these two operations and nothing else: assignable FROM a
// captured PhysicsBodyState, and convertible back TO one for the push. Anything
// that models this pair can be a sim's chosen body-state wire shape.
template <typename T>
concept BodyStateLike = requires(T t, const PhysicsBodyState& f)
{
	t = f;
	{ static_cast<PhysicsBodyState>(t) };
};
