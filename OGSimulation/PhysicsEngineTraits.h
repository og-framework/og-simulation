// SPDX-License-Identifier: MPL-2.0
// docs/PhysicsEngineTraits-rationale.md · docs/PhysicsEngineTraits-guards.md

#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>

using EngineLogSink = void (*)(const char*);

template <template <typename> class WorldHostT>
struct PhysicsEngineWorldHostTemplate {};

template <typename EngineTraitsT>
concept PhysicsEngineTraits = requires(EngineLogSink logger)
{
	typename EngineTraitsT::BodyAdapter;
	typename EngineTraitsT::ReaderAdapter;
	typename EngineTraitsT::QueryAdapter;
	typename EngineTraitsT::Factory;
	typename EngineTraitsT::VizQuery;
	typename EngineTraitsT::VizReader;
	typename EngineTraitsT::CategoryTable;
	{ EngineTraitsT::kName } -> std::convertible_to<const char*>;
	{ EngineTraitsT::computeDeterminismFingerprint(logger) } -> std::same_as<std::optional<uint64_t>>;
	typename PhysicsEngineWorldHostTemplate<EngineTraitsT::template WorldHost>;
	{ &EngineTraitsT::logEngineMessage } -> std::convertible_to<EngineLogSink>;
};

template <typename BindingT, typename EngineTraitsT>
concept PhysicsEngineBinding = PhysicsEngineTraits<EngineTraitsT> && requires
{
	typename BindingT::Simulatable;
	{ BindingT::categoryChannels() } -> std::same_as<typename EngineTraitsT::CategoryTable>;
	typename EngineTraitsT::template WorldHost<typename BindingT::Simulatable>;
	{ BindingT::kParkSnap } -> std::convertible_to<float>;
};

namespace physicsEngineSeamSelfCheck
{
struct Adapter {};
struct Simulatable {};
template <typename SimulatableT> struct WorldHostOf {};

struct TraitsCore
{
	using BodyAdapter   = Adapter;
	using ReaderAdapter = Adapter;
	using QueryAdapter  = Adapter;
	using Factory       = Adapter;
	using VizQuery      = Adapter;
	using VizReader     = Adapter;
	using CategoryTable = int;
	static constexpr char kName[] = "self-check";
	static std::optional<uint64_t> computeDeterminismFingerprint(EngineLogSink);
};

struct DriverScheduledTraits : TraitsCore
{
	template <typename SimulatableT> using WorldHost = WorldHostOf<SimulatableT>;
	static void logEngineMessage(const char*);
};

struct DriverScheduledTraitsWithoutWorldHost : TraitsCore
{
	static void logEngineMessage(const char*);
};

struct DriverScheduledTraitsWithNullLogSink : TraitsCore
{
	template <typename SimulatableT> using WorldHost = WorldHostOf<SimulatableT>;
	static constexpr std::nullptr_t logEngineMessage = nullptr;
};

struct BindingCore
{
	using Simulatable = physicsEngineSeamSelfCheck::Simulatable;
	static int categoryChannels();
};

struct DriverScheduledBinding : BindingCore
{
	static constexpr float kParkSnap = 0.f;
};

static_assert(PhysicsEngineTraits<DriverScheduledTraits>,
	"PhysicsEngineTraits self-check: the reference shape (every member, the world-host template and the log sink "
	"among them) must model the concept.");
static_assert(!PhysicsEngineTraits<DriverScheduledTraitsWithoutWorldHost>,
	"PhysicsEngineTraits self-check: traits without a WorldHost template must NOT model the concept: every engine "
	"is scheduled by the tick driver through its world host. Was the self-scheduled engine shape, which modelled "
	"the concept with no world host while a world-host flag was false (the flag removed by og-session-runtime "
	"task 6).");
static_assert(!PhysicsEngineTraits<DriverScheduledTraitsWithNullLogSink>,
	"PhysicsEngineTraits self-check: an engine's log sink must be a function, never a typed none: a "
	"std::nullptr_t member converts to a function pointer, so a host that stored it would compile and make a null "
	"call. Was a self-scheduled engine's typed-none log sink, before the sink was required of every engine.");
static_assert(PhysicsEngineBinding<DriverScheduledBinding, DriverScheduledTraits>,
	"PhysicsEngineBinding self-check: a binding with a simulatable, a category table and a park snap must model the "
	"concept.");
static_assert(!PhysicsEngineBinding<BindingCore, DriverScheduledTraits>,
	"PhysicsEngineBinding self-check: a binding without kParkSnap must NOT model the concept. Was the self-scheduled "
	"binding shape, which modelled it over a self-scheduled engine until og-session-runtime task 6.");
} // namespace physicsEngineSeamSelfCheck
