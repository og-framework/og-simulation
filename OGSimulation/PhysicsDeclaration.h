#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <concepts>
#include <type_traits>
#include <vector>

#include "glm/vec3.hpp"
#include "OGSimulation/BodyId.h"
#include "OGSimulation/PhysicsBodyState.h"
#include "OGSimulation/QueryGeometry.h"

// ---------------------------------------------------------------------------
// PhysicsDeclaration — the contract a body-owning sub-simulation offers to the
// generic machinery, made CHECKABLE.
//
// Every sub-simulation that owns a physics body publishes a small declaration
// type. Four generic sites fold over the declarations of a simulatable — body
// creation, query-volume registration, post-solve capture and the rewind push —
// and each one reaches into the declaration by name. Until this header those
// names were DUCK-TYPED: a declaration missing one of them failed deep inside a
// template fold, hundreds of lines from the mistake. The concept below turns
// that into one line at the assertion site.
//
// The assertions themselves DO NOT live here, and they do not live on
// SimulationPhysicsComposite either: the concept is parameterised on the GAME's
// StaticData type, and only the game's own simulatable header knows it. Assert
// there, once per declaration.
//
// ⚠ NAME NOTE: sub-simulation namespaces conventionally call their declaration
// type `PhysicsDeclaration` too. Inside such a namespace the unqualified name
// resolves to the STRUCT; write `::PhysicsDeclaration<Decl, GameStaticData>` to
// reach this concept.
// ---------------------------------------------------------------------------

// The runtime handles the engine adapter fills in when it creates a
// declaration's body — local-only, never serialized, never corrected. This is
// the ONE shared definition; before it, every body-owning sub-simulation
// declared its own byte-identical `RuntimeBindings` copy.
//
// A ROOT body carries `parentBodyId == ownBodyId`: it is attached to nothing
// above itself, so it is its own parent rather than carrying a null id that
// every consumer would have to special-case.
struct PhysicsRuntimeBindings
{
	BodyId                     ownBodyId;
	BodyId                     parentBodyId;
	glm::vec3                  attachmentOffset;
	std::vector<ShapeId>       shapeIds;
	std::vector<QueryVolumeId> queryVolumeIds;
};

// D is a physics declaration of a sub-simulation belonging to a game whose
// aggregate static data is GameStaticDataType.
//
// `staticDataOf` is the member that makes body creation fully generic: it maps
// the GAME's aggregate static data to the SUB-simulation's own slice, which is
// what `queryVolumes` and `attachmentOffset` take. Without it the creation fold
// needs a hand-written per-declaration `if constexpr` arm in engine code — the
// single engine edit adding a sub-simulation still costs today.
template <typename D, typename GameStaticDataType>
concept PhysicsDeclaration = requires(
	const GameStaticDataType& gsd,
	typename D::StateType& st,
	const typename D::StateType& cst,
	D d)
{
	{ D::descriptor() } -> std::convertible_to<const PhysicalObjectDescriptor&>;
	{ D::name }         -> std::convertible_to<const char*>;

	// The sub-StaticData this declaration's queryVolumes/attachmentOffset take.
	{ D::staticDataOf(gsd) };
	{ D::queryVolumes(D::staticDataOf(gsd)) }     -> std::convertible_to<std::vector<QueryVolumeDescriptor>>;
	{ D::attachmentOffset(D::staticDataOf(gsd)) } -> std::convertible_to<glm::vec3>;

	// BOTH overloads are checked, for DIFFERENT reasons. The mutable one is the
	// capture target, so its referent must model the body-state pair (see
	// PhysicsBodyState.h). The const one is what the rewind push READS:
	// `pushBodyState(id, D::bodyStateOf(state))` takes a `const PhysicsBodyState&`
	// at a generic call site, so it must WIDEN to a full body state — existing is
	// not enough. Nothing relates the two overloads' return types otherwise.
	requires BodyStateLike<std::remove_cvref_t<decltype(D::bodyStateOf(st))>>;
	{ D::bodyStateOf(cst) } -> std::convertible_to<PhysicsBodyState>;

	// The creation fold WRITES all five members of this after it makes the body,
	// so a read-only — or merely convertible — binding is false confidence. It
	// must BE the one shared PhysicsRuntimeBindings, as a mutable lvalue. A
	// field-identical per-sim copy is a distinct type and correctly fails here;
	// that is what makes adopting the shared type mandatory, not cosmetic.
	{ d.bindings } -> std::same_as<PhysicsRuntimeBindings&>;
};
