#pragma once
// SPDX-License-Identifier: MPL-2.0

// SimulationComposite — variadic state/input composite
// aggregate with isSimilarTo fold and role concepts for composite serialization.
//
// Split history (Task 61):
//   SimulationTypes.h          — PlainType, base isSimilarToField
//   SimulationSerialization.h  — SerializableFields, Serializable, syncSize, write/read,
//                                fieldwiseIsSimilarTo, compositeDetail dispatch
//   SimulationComposite.h      — (this file) composite class, role concepts, helpers
//   SimulationDependencies.h   — deps infrastructure, validation, diagnostics

#include "OGSimulation/SimulationSerialization.h"

// ---------------------------------------------------------------------------
// High-level simulation-role concepts.
// All three roles require Serializable<T>.
//
// [movement-sim task 22] Declared ABOVE SimulationComposite because the composite
// constrains its zero() fold on SimulationInput, and a requires-clause cannot name
// a concept declared later in the file. Nothing else about them moved.
// ---------------------------------------------------------------------------

template <typename T>
concept SimulationState = Serializable<T>;

// `zero()` is the type's NEUTRAL input — the value a resolution peer substitutes
// when no input exists for a tick (SimulationInputResolution::setNeutralInput).
// It is deliberately NOT required to equal T{}, and for aim-carrying inputs it
// must not be: a value-initialised aim reaches normalize(), and the difference
// between the two values is also the tag the input-resolution anti-vacuity tests
// discriminate on (see SimulationInputResolution.h §6). Requiring it here is what
// lets SimulationComposite::zero() fold the neutral input instead of every game
// hand-building one argument per element.
template <typename T>
concept SimulationInput = Serializable<T> && requires { { T::zero() } -> std::same_as<T>; };

template <typename T>
concept SimulationInitialConditions = Serializable<T>;

// ---------------------------------------------------------------------------
// SimulationComposite<Ts...>
// ---------------------------------------------------------------------------

template <typename... Ts>
class SimulationComposite
{
public:
	SimulationComposite() = default;

	explicit SimulationComposite(Ts... args)
		requires (sizeof...(Ts) > 0)
		: m_data(std::move(args)...)
	{}

	template <typename T>
	const T& get() const { return std::get<T>(m_data); }

	template <typename T>
	T& edit() { return std::get<T>(m_data); }

	// Fold over all elements using compositeDetail::compareElement.
	bool isSimilarTo(const SimulationComposite& other) const
		requires (Serializable<Ts> && ...)
	{
		return (compositeDetail::compareElement(
			std::get<Ts>(m_data), std::get<Ts>(other.m_data)) && ...);
	}

	// THE NEUTRAL INPUT for the whole composite: each element's OWN zero(), folded.
	// Adding an element to an input composite is therefore the only edit its neutral
	// value costs — no caller ever hand-builds one argument per element again.
	// ⛔ This is NOT SimulationComposite{}: an element's zero() may differ from its
	// value-initialised value, and for aim-carrying inputs it deliberately does.
	static SimulationComposite zero()
		requires (SimulationInput<Ts> && ...)
	{
		return SimulationComposite(Ts::zero()...);
	}

	template <typename Func>
	void forEach(Func&& func) { (func(std::get<Ts>(m_data)), ...); }

	template <typename Func>
	void forEach(Func&& func) const { (func(std::get<Ts>(m_data)), ...); }

private:
	std::tuple<Ts...> m_data;
};

// Domain aliases (same underlying template, separate names for clarity).
template <typename... Ts> using SimulationStateComposite  = SimulationComposite<Ts...>;
template <typename... Ts> using SimulationInputComposite  = SimulationComposite<Ts...>;

// SimulationDerivedComposite<Ts...> — per-simulatable LOCAL scratch: recomputed or reset
// every tick, never serialized, never corrected, never checksummed.
//
// [movement-sim task 23] The requires-clause is the D1 off-wire discipline MADE
// MECHANICAL. Until now "derived state never goes on the wire" was a comment on the
// slice; here it is the compiler's problem. Giving any element a SerializableFields
// specialization — the one edit that would silently start costing wire bytes and
// checksum coverage — stops being a review catch and becomes a build break.
//
// ⛔ Do NOT relax this clause to make an instantiation compile. If an element needs to
// cross the wire it belongs in the game's State composite, not here.
//
// The underlying template is the same SimulationComposite: get/edit/forEach work, and
// isSimilarTo / the write*ToSyncedBuffer helpers are themselves constrained on
// Serializable, so they simply do not participate for a derived element list.
template <typename... Ts>
	requires (!(Serializable<Ts> || ...))
using SimulationDerivedComposite = SimulationComposite<Ts...>;

// Every element of a SimulationPhysicsComposite is a body-owning sub-simulation's
// physics declaration, and each one is expected to satisfy the
// `PhysicsDeclaration<D, GameStaticDataType>` concept in OGSimulation/PhysicsDeclaration.h
// — the contract the creation, capture and rewind-push folds reach into by name.
//
// It is NOT asserted here, and that is deliberate: the concept is parameterised on
// the GAME's aggregate static data type, which this engine-side alias has no way to
// name. The assertions belong in the game's own simulatable header, one per
// declaration, where GameStaticDataType is in scope.
template <typename... Ts> using SimulationPhysicsComposite = SimulationComposite<Ts...>;

// ---------------------------------------------------------------------------
// Composite serialization helpers
// ---------------------------------------------------------------------------

// Serialize all Serializable elements in tuple order; returns total bytes written.
template <typename SyncedBuffer, typename... Ts>
	requires (Serializable<Ts> && ...)
std::uint32_t writeCompositeToSyncedBuffer(
	const SimulationComposite<Ts...>& state, SyncedBuffer& buffer, std::uint32_t offset)
{
	std::uint32_t it = offset;
	((it += compositeDetail::writeElement(state.template get<Ts>(), buffer, it)), ...);
	return it - offset;
}

// Deserialize all Serializable elements in tuple order.
template <typename SyncedBuffer, typename... Ts>
	requires (Serializable<Ts> && ...)
void readCompositeFromSyncedBuffer(
	SimulationComposite<Ts...>& state, const SyncedBuffer& buffer, std::uint32_t offset)
{
	std::uint32_t it = offset;
	((compositeDetail::readElement(state.template edit<Ts>(), buffer, it), it += syncSize<Ts>()), ...);
}

// Serialize all Serializable input elements; returns total bytes written.
template <typename SyncedBuffer, typename... Ts>
	requires (Serializable<Ts> && ...)
std::uint32_t writeCompositeInputToSyncedBuffer(
	const SimulationComposite<Ts...>& input, SyncedBuffer& buffer, std::uint32_t offset)
{
	std::uint32_t it = offset;
	((it += compositeDetail::writeElement(input.template get<Ts>(), buffer, it)), ...);
	return it - offset;
}

// Deserialize all Serializable input elements; returns composite by value.
template <typename... Ts, typename SyncedBuffer>
	requires (Serializable<Ts> && ...)
SimulationComposite<Ts...> readCompositeInputFromSyncedBuffer(
	const SyncedBuffer& buffer, std::uint32_t offset)
{
	std::uint32_t it = offset;
	SimulationComposite<Ts...> result;
	((compositeDetail::readElement(result.template edit<Ts>(), buffer, it),
	  it += syncSize<Ts>()), ...);
	return result;
}

// ---------------------------------------------------------------------------
// compositeSyncSize<Ts...>() — sum of syncSize for all Ts
// ---------------------------------------------------------------------------

template <typename... Ts>
constexpr std::uint32_t compositeSyncSize()
{
	return (syncSize<Ts>() + ... + std::uint32_t(0));
}

// ---------------------------------------------------------------------------
// SimulationAllInput<PlayerInputT, IntegrationUtilsT>
// A generic value-type pairing a player-input reference with an integration-
// utils reference.  Per-simulation AllInput types become using-aliases of this.
// ---------------------------------------------------------------------------

template <typename PlayerInputT, typename IntegrationUtilsT>
class SimulationAllInput
{
public:
	SimulationAllInput(const PlayerInputT& playerInput, const IntegrationUtilsT& integrationUtils)
		: m_playerInput(playerInput)
		, m_integrationUtils(integrationUtils)
	{}

	SimulationAllInput(const SimulationAllInput& other)
		: m_playerInput(other.m_playerInput)
		, m_integrationUtils(other.m_integrationUtils)
	{}

	const PlayerInputT& getPlayerInput() const { return m_playerInput; }
	const IntegrationUtilsT& getIntegrationUtils() const { return m_integrationUtils; }

private:
	SimulationAllInput() = default;

	const PlayerInputT& m_playerInput;
	const IntegrationUtilsT& m_integrationUtils;
};

// ---------------------------------------------------------------------------
// Generic fallback no-ops for correctSimulation /
// contextSwitchSimulation.  Per-simulation namespaces used to provide empty
// stubs for these; the stubs have been removed and these global templates
// serve as fallbacks via ordinary name-lookup at unqualified call sites.
// ---------------------------------------------------------------------------

template <typename InputType, typename StateType>
void correctSimulation(const InputType&, const StateType&, const StateType&) {}

template <typename InputType, typename StateType>
void contextSwitchSimulation(const InputType&, const StateType&, const StateType&) {}
