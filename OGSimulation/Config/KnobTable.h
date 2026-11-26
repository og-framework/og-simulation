#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/KnobTable-rationale.md

#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>

#include "OGSimulation/OGAssert.h"

namespace knobTable
{
enum class KnobRole : uint8_t { Both, Authority, Prediction };
enum class KnobWorld : uint8_t { Authority, Prediction };
enum class KnobLevel : uint8_t { Log, Warning };

template <typename ValueT>
concept KnobValue = std::same_as<ValueT, int32_t> || std::same_as<ValueT, double>;

template <typename C>
concept KnobConfig = requires(const C& c, const char* key)
{
	{ c.readInt(key) }    -> std::same_as<std::optional<int32_t>>;
	{ c.readDouble(key) } -> std::same_as<std::optional<double>>;
};

template <typename F>
concept KnobLog = std::invocable<F&, KnobLevel, const char*>;

inline void knobRowTextIsEmpty() {}
inline void knobRowFunctionIsNull() {}

class KnobText
{
public:
	consteval KnobText(const char* text)
		: m_text(text)
	{
		if (text == nullptr || text[0] == '\0')
			knobRowTextIsEmpty();
	}

	const char* c_str() const { return m_text; }

private:
	const char* m_text;
};

template <typename SignatureT>
class KnobFn;

template <typename R, typename... ArgsT>
class KnobFn<R(ArgsT...)>
{
public:
	using Pointer = R (*)(ArgsT...);

	template <typename F>
		requires std::convertible_to<F, Pointer> && (!std::same_as<std::remove_cvref_t<F>, std::nullptr_t>)
	consteval KnobFn(F function)
		: m_function(static_cast<Pointer>(function))
	{
		if (m_function == nullptr)
			knobRowFunctionIsNull();
	}

	KnobFn(std::nullptr_t) = delete;

	R operator()(ArgsT... args) const { return m_function(args...); }

private:
	Pointer m_function;
};

template <KnobValue ValueT>
struct KnobIntake
{
	KnobText section;
	KnobText key;
	KnobRole role = KnobRole::Both;
};

template <typename OwnerT, KnobValue ValueT>
struct KnobRow
{
	KnobIntake<ValueT>             intake;
	KnobText                       tag;
	KnobText                       name;
	KnobLevel                      proofLevel = KnobLevel::Warning;
	KnobFn<ValueT(ValueT)>         clamp;
	KnobFn<void(OwnerT&, ValueT)>  apply;
	KnobFn<ValueT(const OwnerT&)>  stored;
};

static_assert(!std::is_default_constructible_v<KnobText> && !std::is_default_constructible_v<KnobFn<int32_t(int32_t)>>,
	"A knob row names its key, tag and proof name and has a clamp, an apply and a read-back: none of them has a default, so "
	"a row that leaves one out does not compile");
static_assert(!std::is_constructible_v<KnobFn<int32_t(int32_t)>, std::nullptr_t>,
	"A knob row's clamp, apply and read-back cannot be nullptr");

constexpr bool knobRuns(KnobRole role, KnobWorld world)
{
	return role == KnobRole::Both || (role == KnobRole::Authority) == (world == KnobWorld::Authority);
}

static_assert(knobRuns(KnobRole::Both, KnobWorld::Authority) && knobRuns(KnobRole::Both, KnobWorld::Prediction)
	&& knobRuns(KnobRole::Authority, KnobWorld::Authority) && !knobRuns(KnobRole::Authority, KnobWorld::Prediction)
	&& knobRuns(KnobRole::Prediction, KnobWorld::Prediction) && !knobRuns(KnobRole::Prediction, KnobWorld::Authority),
	"knobRuns: a Both row runs on every world, an Authority or Prediction row on that world only");

namespace knobLines
{
inline constexpr char kProof[]      = "[%s] %s=%s (%s)";
inline constexpr char kOutOfRange[] = "[%s] ini [%s] %s=%s out of range, clamped to %s";
inline constexpr char kNotFinite[]  = "[%s] ini [%s] %s is not a finite number, keeping the compiled default";
inline constexpr char kInt32[]      = "%d";
inline constexpr char kDouble[]     = "%.4f";

template <typename... ArgsT>
std::string formatLine(const char* pattern, ArgsT... args)
{
	const int length = std::snprintf(nullptr, 0, pattern, args...);
	OG_CHECK(length >= 0, "knobLines::formatLine: snprintf refused a knob line's format and arguments");
	std::string line(static_cast<std::size_t>(length), '\0');
	std::snprintf(line.data(), line.size() + 1u, pattern, args...);
	return line;
}

template <KnobValue ValueT>
std::string valueText(ValueT value)
{
	if constexpr (std::same_as<ValueT, int32_t>)
		return formatLine(kInt32, value);
	else
		return formatLine(kDouble, value);
}
} // namespace knobLines

template <KnobValue ValueT, KnobConfig ConfigT>
std::optional<ValueT> readKnob(const KnobIntake<ValueT>& intake, const ConfigT& config, KnobWorld world)
{
	if (!knobRuns(intake.role, world))
		return std::nullopt;
	if constexpr (std::same_as<ValueT, int32_t>)
		return config.readInt(intake.key.c_str());
	else
		return config.readDouble(intake.key.c_str());
}

template <typename OwnerT, KnobValue ValueT, KnobLog LogT>
void applyKnob(const KnobRow<OwnerT, ValueT>& row, std::optional<ValueT> requested, OwnerT& owner, KnobWorld world, LogT&& log)
{
	if (!knobRuns(row.intake.role, world))
		return;
	bool fromConfig = false;
	if (requested.has_value())
	{
		const ValueT value = *requested;
		bool finite = true;
		if constexpr (std::same_as<ValueT, double>)
			finite = std::isfinite(value);
		if (!finite)
		{
			const std::string line = knobLines::formatLine(knobLines::kNotFinite, row.tag.c_str(), row.intake.section.c_str(),
				row.intake.key.c_str());
			log(KnobLevel::Warning, line.c_str());
		}
		else
		{
			const ValueT clamped = row.clamp(value);
			if (clamped != value)
			{
				const std::string line = knobLines::formatLine(knobLines::kOutOfRange, row.tag.c_str(),
					row.intake.section.c_str(), row.intake.key.c_str(), knobLines::valueText(value).c_str(),
					knobLines::valueText(clamped).c_str());
				log(KnobLevel::Warning, line.c_str());
			}
			row.apply(owner, clamped);
			fromConfig = true;
		}
	}
	const std::string proof = knobLines::formatLine(knobLines::kProof, row.tag.c_str(), row.name.c_str(),
		knobLines::valueText(row.stored(owner)).c_str(), fromConfig ? "ini" : "compiled");
	log(row.proofLevel, proof.c_str());
}

template <typename OwnerT, KnobConfig ConfigT, KnobLog LogT, KnobValue... ValuesT>
void applyKnobs(const std::tuple<KnobRow<OwnerT, ValuesT>...>& rows, const ConfigT& config, OwnerT& owner, KnobWorld world,
	LogT&& log)
{
	std::apply([&](const auto&... row) { (applyKnob(row, readKnob(row.intake, config, world), owner, world, log), ...); }, rows);
}
} // namespace knobTable
