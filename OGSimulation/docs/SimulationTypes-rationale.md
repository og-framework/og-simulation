<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationTypes.h` — rationale

The header holds `PlainType`, `kDefaultSimilarityEpsilon` and the generic `isSimilarToField`. It includes only standard
library headers (`<cmath>`, `<concepts>`), so any simulation header can include it. It keeps its licence, its docs
pointer and its code; everything that explains it lives here.

**If this file and `SimulationTypes.h` disagree, the header is authoritative and this file is stale.**

This file has no guards document: the header's one prohibition is a `static_assert` (§3).

---

## 1. `PlainType`

`PlainType<T>` strips `const`, `volatile` and references, so `sizeof` and type comparisons see the underlying type.
Most simulation headers take it from here. One class still defines its own member copy: `ServerTickClock`
(`PCTimeManagement/ServerTickClock.h`), which does not include this header.

## 2. The generic `isSimilarToField` and `kDefaultSimilarityEpsilon`

`isSimilarToField(a, b)` is the leaf of the correction comparator (`isSimilarTo`, `fieldwiseIsSimilarTo`). The
generic template compares a floating-point type (`std::floating_point`: `float`, `double`, `long double`) with
`std::abs(a - b) < kDefaultSimilarityEpsilon`, and every other type with `operator==`.

`kDefaultSimilarityEpsilon` is `0.0001f`. It is the one tolerance every float leaf and every glm leaf uses.

The other overloads live elsewhere:
- `SimulationSerialization.h`: one constrained to `Serializable` types (it calls `fieldwiseIsSimilarTo`), and one for
  `std::vector`, element by element;
- `SimulationComparisonGlm.h`: `glm::vec3`, `glm::vec2` and `glm::quat`, in `namespace glm`, so that this header stays
  free of glm.

## 3. The `static_assert`: a glm field never reaches the exact compare

The generic template asserts that the concept `simulationTypesDetail::GlmFloatType` is false for its `T`. The concept
matches a type with a `value_type` that is floating point and a static `length()`: the shape of every glm float vector
(`glm::vec<L, T, Q>`), quaternion (`glm::qua<T, Q>`) and matrix (`glm::mat<C, R, T, Q>`). It is detected by shape so
that this header needs no glm include. A static `length()` is what sets glm apart from the standard containers:
`std::array` and `std::vector` have a `size()` and no `length()`, and `std::basic_string`'s `length()` is not static
(and its `value_type` is a character type).

Why it exists: until og-input-latency task 2e, every glm field of a `Serializable` aggregate reached this template and
compared exactly, because the glm overloads sat in the global namespace, where the comparator's dependent call could not
find them under `/permissive-` (`SimulationComparisonGlm-rationale.md` §3). The assertion turns each way of getting
back there into a compile error:
- the overloads leave `namespace glm`;
- a translation unit compares a glm field without including `SimulationComparisonGlm.h`;
- a glm float type with no overload there (`glm::vec4`, `glm::dvec3`, `glm::mat3`) is compared.

Until task 2g the concept also required a member `x`. No glm matrix has one, so a `glm::mat3` or `glm::mat4` field
passed the assertion and compared exactly, with no compile error (task 2e's review, N1). Task 2g dropped that
requirement; a probe with a `glm::mat3` field in a `Serializable` aggregate now fails to compile with this assertion's
message, and compiles with the field removed.

Integer glm vectors (`glm::ivec3`) have an integer `value_type`, so they pass and compare with `operator==`, which is
exact by design.

**Out of scope: `std::array<float, N>`.** It has a floating `value_type` but no `length()`, so it passes the assertion
and compares with `operator==`, exactly. The pin is about glm types and does not cover it. No `Serializable` state
holds one today. A state that needs one would need an overload that compares it element by element, as the
`std::vector` overload in `SimulationSerialization.h` does.
