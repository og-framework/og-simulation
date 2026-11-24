<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationComparisonGlm.h` — rationale

The header holds the correction comparator's epsilon overloads of `isSimilarToField` for `glm::vec3`, `glm::vec2` and
`glm::quat`, and since og-input-latency task 2h their `similarityRatioField` twins (§6). It keeps its licence, its docs pointer and its code; everything that explains it lives here.

**If this file and `SimulationComparisonGlm.h` disagree, the header is authoritative and this file is stale.**

<!-- lint-external-ref: GlmFieldSimilarityTest.cpp -- TEST TRANSLATION UNIT in og-simulation-tests, a separate submodule not distributed with this one; it must not resolve here -->
<!-- lint-external-ref: VerdictMarginTest.cpp -- TEST TRANSLATION UNIT in og-simulation-tests, a separate submodule not distributed with this one; it must not resolve here -->

This file has no guards document: the one prohibition it would hold ("do not move the overloads out of `namespace
glm`") is enforced by the compiler instead, by the `static_assert` in `SimulationTypes.h` (§3).

---

## 1. What it is for

`isSimilarTo` decides whether a prediction matches the authority's correction, and so whether a resim runs. Its leaves
are `isSimilarToField` calls: `SimulationTypes.h` holds the generic template (an epsilon for floating-point types, `==`
for everything else), `SimulationSerialization.h` the overloads for `Serializable` aggregates and `std::vector` (the
fold never reaches the `std::vector` one, which is declared after it: `SimulationComparison-rationale.md` §8), and this
header the glm ones. They live in a header of their own so that `SimulationTypes.h`, `SimulationSerialization.h` and
`SimulationComposite.h` include no glm header.

## 2. The tolerance per type

Every tolerance is `kDefaultSimilarityEpsilon` (`SimulationTypes.h`, `0.0001f`), in the simulation's own units. The
comparisons are strict (`<`):

| Type | Similar when |
|---|---|
| `glm::vec3` | every component differs by less than the epsilon (`std::abs(a.x - b.x) < kDefaultSimilarityEpsilon`, and so on) |
| `glm::vec2` | the same, for two components |
| `glm::quat` | after sign alignment (`b` is replaced by `-b` when `glm::dot(a, b) < 0.f`), every one of the four components differs by less than the epsilon: the vectors' rule on four components |

- **Sign alignment.** `q` and `-q` are the same rotation. Of the two representations of `b`, the one with a
  non-negative dot product with `a` is the nearer one, so the components are compared against it.
- **The angle the quaternion rule implies.** Write `b = a·r`, with `r = (cos(θ/2), sin(θ/2)·n)` the rotation by θ about
  a unit axis `n`. Then `b − a = a·(r − 1)`, and multiplying by a unit quaternion keeps the length of a four-component
  vector, so `‖b − a‖ = ‖r − 1‖ = 2·sin(θ/4) ≈ θ/2`. The largest of four components lies between a half of that
  length and all of it, i.e. between θ/4 and θ/2. With the epsilon at 1e-4:
  - **always similar** below θ = 2e-4 rad (≈ 0.0115°), where even θ/2 is under the epsilon;
  - **never similar** from θ = 4e-4 rad (≈ 0.0229°), where even θ/4 reaches it;
  - in between, the verdict depends on the orientation and the axis. From the identity, a rotation about one axis
    changes that axis's component by `sin(θ/2) ≈ θ/2` (and `w` by `1 − cos(θ/2) ≈ θ²/8`), so the bound there is
    `2·asin(1e-4)` ≈ 2.0e-4 rad. The 4e-4 rad end is reached where the change spreads evenly over all four components.
  - og-simulation-tests' `GlmFieldSimilarityTest.cpp` checks these numbers and both bounds (from the identity about
    each axis, over a grid of 6 bases and 5 axes, and at a base that reaches the 4e-4 rad end).
- **For contrast, the rule before og-input-latency task 2g:** `std::abs(std::abs(glm::dot(a, b)) - 1.f)` less than the
  epsilon. For unit quaternions `|dot| = cos(θ/2)`, so it forgave θ < 2·acos(1 − 1e-4) ≈ 0.0283 rad ≈ 1.62°, about
  140 times the per-axis bound above. It could not be tightened in its own terms: `1 − cos(θ/2) ≈ θ²/8`, so a bound of
  2e-4 rad needs `1 − |dot|` below 5e-9, and the float just below 1 is already 6e-8 away from it. The component form
  needs no new constant and no `double`.
- The vector tests are absolute. One float ULP grows with the magnitude: it is 9.5e-7 at 13 and 1.2e-4 at 1024, so from
  a magnitude of 1024 one ULP is no longer inside the epsilon. The quaternion components of a unit quaternion are at
  most 1 in magnitude, so one ULP of each is at most 6e-8, well inside the epsilon.
- The vec3 and vec2 definitions are the ones this header has always had: og-input-latency task 2e made them reachable
  (§3) and did not change them. Task 2g replaced the quaternion's (§5).
- A glm type with no overload here (`glm::vec4`, `glm::dvec3`, a matrix such as `glm::mat3`, any other glm type with a
  floating-point `value_type`) is rejected at compile time by `SimulationTypes.h`'s `static_assert` rather than
  compared exactly. Add its overload here first.

## 3. Why the overloads are in `namespace glm`

`fieldwiseIsSimilarTo` (`SimulationSerialization.h`) calls `isSimilarToField` with dependent arguments. Every target in
this tree compiles with `/permissive-`, which turns on MSVC's two-phase lookup:
- ordinary unqualified lookup runs at the template's **definition**, where only `SimulationTypes.h`'s generic template
  is declared;
- argument-dependent lookup (ADL) runs at the point of instantiation, but searches only the argument's associated
  namespaces. For `glm::vec3` (`glm::vec<3, float, glm::defaultp>`) that is `glm` alone: `float` adds none, and the
  qualifier is a non-type template argument, which adds none either.

Until task 2e the overloads were declared in the **global** namespace, so neither half found them. A `glm` field in a
`Serializable` aggregate fell to the generic template and compared with `operator==`: exactly. Measured on the
pre-task-2e headers with the UBT flags (`/std:c++20 /permissive- /fp:fast /O2`): a 1-ULP difference in
`LinearBodyState::linearVelocity`, a 1e-5 difference in its `position` and in a `glm::vec2` field, and a 2e-4 rad
rotation of a `glm::quat` field all made `fieldwiseIsSimilarTo` return false, while a direct call of
`isSimilarToField` on the same pair returned true. With `/Zc:twoPhase-` (lookup deferred to instantiation) the same
probe returned true for all four. So the cause was the two-phase lookup, not the epsilon.

In `namespace glm` the overloads are found by ADL wherever the call is made, whatever the include order, as long as the
translation unit includes this header. After the move the same probe returns true for all four cases, under both lookup
modes.

**The pin.** The generic template in `SimulationTypes.h` `static_assert`s that it is never instantiated for a glm float
vector or quaternion. If the overloads leave `namespace glm`, or a translation unit compares a glm field without this
header, or a new glm type has no overload, the build fails with a message that names this header. A runtime case
(og-simulation-tests' `GlmFieldSimilarityTest.cpp`) also pins the verdict through the correction cache.

**Calls that were never affected.** Non-dependent calls, such as og-brawler's projectile slot `operator==` and the former
Chaos backend's body-state comparison in the UE module (removed with the Chaos configuration by og-session-runtime tasks
3–6), always found the global overloads by ordinary lookup. They now find
the same functions by ADL, so task 2e did not change their results. Task 2g's quaternion rule (§5) does reach a
non-dependent call that compares a quaternion, such as the former Chaos backend's comparison of
`PhysicsBodyState::rotation`: it called the same overload. The projectile slot's `operator==` compares two `glm::vec3` fields only.

## 4. What task 2e changed for corrections
- Before, a 1-ULP difference in any glm field of a correction was a disagreement and a resim. Two builds that round one
  operation differently (a Shipping server against Development clients, task 2c; Android against Win64 or Linux) then
  disagreed on every tick an affected field was not exactly equal.
- After, glm fields get the same tolerance a float field always had (§2). A disagreement is still reported wherever a
  difference reaches the epsilon.
- **Measured (task 2e, one consumer's packaged build: a Shipping dedicated server and Development clients, Jolt, two
  idle characters on 30° slopes):** disagreeing `linearVelocity` corrections fell from 9,656 and 5,596 per run (task 2c)
  to 0 and 0, and resim preparations to 3 per client per run, all at spawn or join.
- **A known difference that stays:** the two build configurations still compile the simulation differently, so their
  results can differ by a few ULPs. `CompilerControl.h`'s `OGSIM_OPTIMIZE_OFF` / `OGSIM_OPTIMIZE_ON` expand to nothing
  in Shipping and Test builds and to an MSVC `optimize` pragma pair otherwise; UE's Shipping default also adds
  `/experimental:deterministic`. Task 2e does not change either. The tolerance absorbs differences of that size; a
  difference that grows to the epsilon is corrected as usual.

## 5. What task 2g changed for corrections
- Before, two quaternions were similar for any rotation under about 1.62° (§2, the rule before task 2g). The one
  corrected quaternion field, the weapon-axis body's `PhysicsBodyState::rotation`, could be that far off without a
  resim. During a swing its torque axis comes from its own orientation, so a pose error steers the swing.
- After, a rotation is always similar below about 0.011° and never from about 0.023° (§2). Between the two, the
  verdict depends on the orientation and the axis.
- The rule is stricter than the old one: a pair it calls similar has θ below 4e-4 rad, so the old rule calls it similar
  too. So when no verdict in a run is decided by a quaternion, both rules give the same verdicts for the same states.
- **Measured (og-input-latency task 2g):**
  - The setup: the 2-minute split smoke, Jolt, Development Editor binaries; a server, client1, and client2a, which
    leaves at 60 s while client2b joins at 75 s. Two runs used this rule, and one used the old rule in the same tree.
  - Not one disagreeing correction in any run was decided by a rotation. Each was decided by a body position or by an
    attack state's time-in-state timer, which was one tick apart.
  - Resim preparations per client varied from run to run under both rules: client1 5 to 9, client2a 4 to 6, client2b
    4 to 6. They are timing, not this rule.

## 6. The normalised delta beside each overload (og-input-latency task 2h)
- **What.** Each `isSimilarToField` overload here has a twin, `similarityRatioField`, that returns how much of the
  tolerance a pair uses: the largest component `|Δ|` divided by `kDefaultSimilarityEpsilon`, as a `double`.
  `measureVerdictMargin` (`SimulationComparison.h`, its rationale §8) takes the largest over a whole verdict.
- **The same quantity as the verdict.** Each twin subtracts the same components in `float`, as its verdict does. So for
  every type here, a ratio below 1 means the overload says similar. A quaternion uses the same sign alignment as its
  verdict: one inline function, `similaritySignAligned`, serves both, so the two cannot align differently. `q` against
  `−q` gives 0.
- **In `namespace glm`, for §3's reason.** The walk's call is dependent, and a glm argument brings ADL to `glm` only.
  The scalar arm, the generic template in `SimulationComparison.h`, has a `static_assert` that rejects any glm float
  type, the twin of `SimulationTypes.h`'s pin. So a glm leaf with no ratio overload here fails to compile instead of
  reaching the scalar arm. `VerdictMarginTest.cpp` pins that all three overloads are found by ADL alone.
- **Matrices.** There is no matrix overload, as there is no matrix `isSimilarToField`: a `glm::mat` field fails the
  pin first (§2, task 2g).
- **The verdict did not change.** The quat `isSimilarToField` now gets its aligned operand from
  `similaritySignAligned`, which returns `dot(a, b) < 0 ? −b : b`, the expression task 2g wrote inline. The four
  component tests are unchanged.
