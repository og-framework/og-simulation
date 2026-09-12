#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/PhysicsDeclaration-rationale.md · docs/PhysicsDeclaration-guards.md

#include <concepts>
#include <type_traits>
#include <vector>

#include "glm/vec3.hpp"
#include "OGSimulation/BodyId.h"
#include "OGSimulation/PhysicsBodyState.h"
#include "OGSimulation/QueryGeometry.h"

struct PhysicsRuntimeBindings
{
	BodyId                     ownBodyId;
	BodyId                     parentBodyId;
	glm::vec3                  attachmentOffset;
	std::vector<ShapeId>       shapeIds;
	std::vector<QueryVolumeId> queryVolumeIds;
};

// ⛔G-01  docs/PhysicsDeclaration-guards.md
template <typename D, typename GameStaticDataType>
concept PhysicsDeclaration = requires(
	const GameStaticDataType& gsd,
	typename D::StateType& st,
	const typename D::StateType& cst,
	D d)
{
	{ D::descriptor() } -> std::convertible_to<const PhysicalObjectDescriptor&>;
	{ D::name }         -> std::convertible_to<const char*>;

	{ D::staticDataOf(gsd) };
	{ D::queryVolumes(D::staticDataOf(gsd)) }     -> std::convertible_to<std::vector<QueryVolumeDescriptor>>;
	{ D::attachmentOffset(D::staticDataOf(gsd)) } -> std::convertible_to<glm::vec3>;

	requires BodyStateLike<std::remove_cvref_t<decltype(D::bodyStateOf(st))>>;
	{ D::bodyStateOf(cst) } -> std::convertible_to<PhysicsBodyState>;

	{ d.bindings } -> std::same_as<PhysicsRuntimeBindings&>;
};

namespace physicsDeclarationSelfCheck
{
struct StaticDataProbe {};
struct StateProbe { PhysicsBodyState bodyState; int notABodyState = 0; };

struct Conforming
{
	static const PhysicalObjectDescriptor& descriptor();
	static constexpr const char* name = "selfCheck";
	static const StaticDataProbe& staticDataOf(const StaticDataProbe& gsd) { return gsd; }
	static std::vector<QueryVolumeDescriptor> queryVolumes(const StaticDataProbe&);
	static glm::vec3 attachmentOffset(const StaticDataProbe&);

	using StateType = StateProbe;
	static       PhysicsBodyState& bodyStateOf(      StateType& s) { return s.bodyState; }
	static const PhysicsBodyState& bodyStateOf(const StateType& s) { return s.bodyState; }

	PhysicsRuntimeBindings bindings;
};

struct DerivedBindings : PhysicsRuntimeBindings {};
struct LookAlikeBindings
{
	BodyId                     ownBodyId;
	BodyId                     parentBodyId;
	glm::vec3                  attachmentOffset;
	std::vector<ShapeId>       shapeIds;
	std::vector<QueryVolumeId> queryVolumeIds;
};

struct ProbeDerivedBindings  : Conforming { DerivedBindings              bindings; };
struct ProbeConstBindings    : Conforming { const PhysicsRuntimeBindings bindings{}; };
struct ProbeLookAlikeBindings: Conforming { LookAlikeBindings            bindings; };

struct ProbeNonWideningConst : Conforming
{
	static       PhysicsBodyState& bodyStateOf(      StateType& s) { return s.bodyState; }
	static const int&              bodyStateOf(const StateType& s) { return s.notABodyState; }
};
struct ProbeMutableNotBodyStateLike : Conforming
{
	static       int&              bodyStateOf(      StateType& s) { return s.notABodyState; }
	static const PhysicsBodyState& bodyStateOf(const StateType& s) { return s.bodyState; }
};

StateProbe&       mutableProbeState();
const StateProbe& constProbeState();

static_assert(PhysicsDeclaration<Conforming, StaticDataProbe>,
	"PhysicsDeclaration self-check: VACUITY CONTROL. Without it every negative assertion below "
	"goes vacuously true the moment the concept is renamed or a probe stops conforming for an "
	"unrelated reason. Note what it asserts about: a probe type THIS header defines itself. A "
	"consumer's declaration still cannot be asserted here - see guard G-01.");

static_assert(!PhysicsDeclaration<ProbeDerivedBindings, StaticDataProbe>,
	"PhysicsDeclaration: `bindings` must BE the shared PhysicsRuntimeBindings as a mutable lvalue "
	"- `same_as`, never `convertible_to`. A DERIVED lvalue converts to both `PhysicsRuntimeBindings&` "
	"and `const PhysicsRuntimeBindings&`, so relaxing the requirement in either spelling, or "
	"deleting it, fires here. The creation fold writes all five members after it makes the body; a "
	"merely-convertible binding loses every handle it produced. "
	"Was fence T2c-2, guard G-04, now retired.");

static_assert(!PhysicsDeclaration<ProbeConstBindings, StaticDataProbe>,
	"PhysicsDeclaration: a READ-ONLY `bindings` member is false confidence - the creation fold "
	"writes it. It satisfied the historical `convertible_to<const PhysicsRuntimeBindings&>` "
	"spelling, which `ProbeDerivedBindings` above does not reach. "
	"Was fence T2c-2, guard G-04, now retired.");

static_assert(!PhysicsDeclaration<ProbeLookAlikeBindings, StaticDataProbe>,
	"PhysicsDeclaration: NON-DISCRIMINATING CONTROL - do not mistake it for the check. A "
	"field-identical per-sim copy fails under `same_as` AND under every relaxation of it, so it "
	"proves nothing about which spelling the requirement carries. The two assertions above are "
	"the check; this one only shows the shape a sub-simulation must NOT ship.");

static_assert(!PhysicsDeclaration<ProbeNonWideningConst, StaticDataProbe>,
	"PhysicsDeclaration: the CONST bodyStateOf overload must WIDEN to a full PhysicsBodyState. The "
	"rewind push reads through it at a generic call site taking `const PhysicsBodyState&`, so "
	"merely existing is not enough. Was fence T2c-3, guard G-03, now retired.");

static_assert(!PhysicsDeclaration<ProbeMutableNotBodyStateLike, StaticDataProbe>,
	"PhysicsDeclaration: the MUTABLE bodyStateOf overload's referent must model BodyStateLike - it "
	"is the post-solve capture target. The two overloads are checked for DIFFERENT reasons and "
	"neither line is redundant. Was fence T2c-3, guard G-03, now retired.");

static_assert(BodyStateLike<std::remove_cvref_t<
		decltype(ProbeNonWideningConst::bodyStateOf(mutableProbeState()))>>,
	"PhysicsDeclaration self-check: DISCRIMINATION. ProbeNonWideningConst must be rejected for its "
	"CONST overload alone - its mutable referent is a sound BodyStateLike. Guards G-03.");

static_assert(std::is_convertible_v<
		decltype(ProbeMutableNotBodyStateLike::bodyStateOf(constProbeState())), PhysicsBodyState>,
	"PhysicsDeclaration self-check: DISCRIMINATION. ProbeMutableNotBodyStateLike must be rejected "
	"for its MUTABLE overload alone - its const overload widens correctly. Guards G-03.");
} // namespace physicsDeclarationSelfCheck
