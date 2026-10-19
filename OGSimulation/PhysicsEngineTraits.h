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
	{ EngineTraitsT::kHasWorldHost } -> std::convertible_to<bool>;
} && (!EngineTraitsT::kHasWorldHost || requires
{
	typename PhysicsEngineWorldHostTemplate<EngineTraitsT::template WorldHost>;
	{ &EngineTraitsT::logEngineMessage } -> std::convertible_to<EngineLogSink>;
});

template <typename BindingT, typename EngineTraitsT>
concept PhysicsEngineBinding = PhysicsEngineTraits<EngineTraitsT> && requires
{
	typename BindingT::Simulatable;
	{ BindingT::categoryChannels() } -> std::same_as<typename EngineTraitsT::CategoryTable>;
} && (!EngineTraitsT::kHasWorldHost || requires
{
	typename EngineTraitsT::template WorldHost<typename BindingT::Simulatable>;
	{ BindingT::kParkSnap } -> std::convertible_to<float>;
});

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
	static constexpr bool kHasWorldHost = true;
	template <typename SimulatableT> using WorldHost = WorldHostOf<SimulatableT>;
	static void logEngineMessage(const char*);
};

struct SelfScheduledTraits : TraitsCore
{
	static constexpr bool kHasWorldHost = false;
};

struct DriverScheduledTraitsWithoutWorldHost : TraitsCore
{
	static constexpr bool kHasWorldHost = true;
	static void logEngineMessage(const char*);
};

struct DriverScheduledTraitsWithNullLogSink : TraitsCore
{
	static constexpr bool kHasWorldHost = true;
	template <typename SimulatableT> using WorldHost = WorldHostOf<SimulatableT>;
	static constexpr std::nullptr_t logEngineMessage = nullptr;
};

struct SelfScheduledBinding
{
	using Simulatable = physicsEngineSeamSelfCheck::Simulatable;
	static int categoryChannels();
};

struct DriverScheduledBinding : SelfScheduledBinding
{
	static constexpr float kParkSnap = 0.f;
};

static_assert(PhysicsEngineTraits<DriverScheduledTraits> && PhysicsEngineTraits<SelfScheduledTraits>,
	"PhysicsEngineTraits self-check: the reference shapes (every member; a world-host template exactly when "
	"kHasWorldHost) must model the concept.");
static_assert(!PhysicsEngineTraits<DriverScheduledTraitsWithoutWorldHost>,
	"PhysicsEngineTraits self-check: traits with kHasWorldHost true and no WorldHost template must NOT model the "
	"concept.");
static_assert(!PhysicsEngineTraits<DriverScheduledTraitsWithNullLogSink>,
	"PhysicsEngineTraits self-check: a driver-scheduled engine's log sink must be a function, never a typed none: a "
	"std::nullptr_t member converts to a function pointer, so a host that stored it would compile and make a null "
	"call. Was a self-scheduled engine's typed-none log sink, before the sink was required only when "
	"kHasWorldHost.");
static_assert(PhysicsEngineBinding<DriverScheduledBinding, DriverScheduledTraits>
		&& PhysicsEngineBinding<SelfScheduledBinding, SelfScheduledTraits>,
	"PhysicsEngineBinding self-check: a binding with a simulatable, a category table and a park snap must model the "
	"concept over a driver-scheduled engine, and one without the park snap over a self-scheduled engine.");
static_assert(!PhysicsEngineBinding<SelfScheduledBinding, DriverScheduledTraits>,
	"PhysicsEngineBinding self-check: a binding of a driver-scheduled engine without kParkSnap must NOT model the "
	"concept.");
} // namespace physicsEngineSeamSelfCheck
