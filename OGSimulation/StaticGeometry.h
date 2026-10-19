#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/StaticGeometry-rationale.md

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>
#include <vector>

#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"
#include "OGSimulation/QueryGeometry.h"

struct StaticBox
{
	glm::vec3 halfExtents{0.f};
};

struct StaticSphere
{
	float radius = 0.f;
};

struct StaticCapsuleZ
{
	float radius = 0.f;
	float totalHalfHeight = 0.f;
};

struct StaticConvexHull
{
	std::vector<glm::vec3> points;
};

struct StaticTriangleMesh
{
	std::vector<glm::vec3> vertices;
	std::vector<uint32_t> indices;
};

using StaticShape = std::variant<StaticBox, StaticSphere, StaticCapsuleZ, StaticConvexHull, StaticTriangleMesh>;

inline constexpr std::size_t kStaticShapeTypeCount = std::variant_size_v<StaticShape>;

template <typename Shape, typename Variant>
struct StaticShapeIndexOf;

template <typename Shape, typename... Alternatives>
struct StaticShapeIndexOf<Shape, std::variant<Alternatives...>>
{
	static constexpr std::size_t value = []
	{
		constexpr bool matches[] = { std::is_same_v<Shape, Alternatives>... };
		std::size_t found = sizeof...(Alternatives);
		for (std::size_t i = 0; i < sizeof...(Alternatives); ++i)
		{
			if (matches[i])
			{
				found = i;
			}
		}
		return found;
	}();
	static_assert(value < sizeof...(Alternatives), "StaticShapeIndexOf: Shape is not an alternative of StaticShape");
};

template <typename Shape>
inline constexpr std::size_t kStaticShapeIndex = StaticShapeIndexOf<Shape, StaticShape>::value;

struct StaticShapeDescriptor
{
	StaticShape shape;
	glm::mat4 localToWorld{1.f};
	CollisionCategories categories;
	CollisionCategories blockingCategories{};
	float friction = 0.f;
	float restitution = 0.f;
	uint64_t stableKey = 0;
};

struct StaticWorldDescription
{
	std::vector<StaticShapeDescriptor> shapes;
};

struct StaticWorldBuildReport
{
	std::array<uint32_t, kStaticShapeTypeCount> shapeCountByType{};
	double buildSeconds = 0.0;

	template <typename Shape>
	uint32_t countOf() const { return shapeCountByType[kStaticShapeIndex<Shape>]; }
};

template <typename B>
concept StaticWorldBuilder = requires(B b, const StaticWorldDescription& d)
{
	{ b.build(d) } -> std::same_as<StaticWorldBuildReport>;
};

namespace staticWorldBuilderSelfCheck
{
struct Conforming
{
	StaticWorldBuildReport build(const StaticWorldDescription&);
};

struct ConsumesTheDescription
{
	StaticWorldBuildReport build(StaticWorldDescription&);
};

struct ReportsSomethingElse
{
	bool build(const StaticWorldDescription&);
};

static_assert(StaticWorldBuilder<Conforming>,
	"StaticWorldBuilder self-check: the reference shape must satisfy the concept");
static_assert(!StaticWorldBuilder<ConsumesTheDescription>,
	"StaticWorldBuilder self-check: build must accept a const description");
static_assert(!StaticWorldBuilder<ReportsSomethingElse>,
	"StaticWorldBuilder self-check: build must return a StaticWorldBuildReport");
static_assert(kStaticShapeIndex<StaticBox> == 0 && kStaticShapeIndex<StaticTriangleMesh> == kStaticShapeTypeCount - 1,
	"StaticWorldBuilder self-check: kStaticShapeIndex follows the StaticShape alternative order");
} // namespace staticWorldBuilderSelfCheck
