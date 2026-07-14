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
template <typename... Ts> using SimulationPhysicsComposite = SimulationComposite<Ts...>;

// ---------------------------------------------------------------------------
// High-level simulation-role concepts.
// All three roles require Serializable<T>.
// ---------------------------------------------------------------------------

template <typename T>
concept SimulationState = Serializable<T>;

template <typename T>
concept SimulationInput = Serializable<T>;

template <typename T>
concept SimulationInitialConditions = Serializable<T>;

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
