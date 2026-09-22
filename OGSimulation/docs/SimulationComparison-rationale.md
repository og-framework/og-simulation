<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationComparison.h` — rationale

The header keeps a one-line-or-two guard at every site that has one, plus an orientation
block naming the question it answers, the one production caller and the cost gate. **This
file holds the derivation, the rejected alternatives and the measurement records those
guards were compressed from.**

**If this file and `SimulationComparison.h` disagree, the header is authoritative and this
file is stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a guard into this file.** Every section below is the *expansion* of a fence
that still fires at its own line.

⚠ **Adapter bindings named below — one adapter's, never the binding.** `og-simulation` is
engine-free. `UE_LOG_ACTIVE`, `LogOGDivergenceProbe` and `Config/DefaultEngine.ini` belong to
one consumer's host stack and are named here only where a section records what the cost gate
is actually wired against in that consumer. None of them is a dependency of this core.

<!-- lint-external-ref: BrawlerMovementSimulation.h -- ONE CONSUMER'S GAME CODE, in the og-brawler submodule; named as the measured blast radius of the rejected NTTP design, must not resolve here -->
<!-- lint-external-ref: BrawlerRingoutSimulation.h -- ONE CONSUMER'S GAME CODE, in the og-brawler submodule, same reason as BrawlerMovementSimulation.h; must not resolve here -->
<!-- lint-external-ref: CorrectionFieldDivergenceTest.cpp -- TEST TRANSLATION UNIT in og-simulation-tests, a separate submodule not distributed with this one; it must not resolve here -->
<!-- lint-external-ref: BrawlerCorrectionFieldDivergenceTest.cpp -- TEST TRANSLATION UNIT in og-brawler-tests, outside this submodule; it must not resolve here -->
<!-- lint-external-ref: Config/DefaultEngine.ini -- ONE ADAPTER'S HOST-APPLICATION CONFIGURATION SURFACE, outside this submodule; another adapter sets the same value under its own key -->

---

## 1. Why a second entry point rather than a wider `isSimilarTo`

`isSimilarTo` is the production resim trigger. `StateCorrectionCache::tryInsertingCorrectState`
computes it on every landed correction, stores it in `m_predictionWasCorrect`, feeds it to
`resimGate::shouldSetPendingAnchor`, and under the shipped trigger policy that boolean decides
whether a resim runs at all.

It is a **fold**, and a fold discards everything except its own answer. A 2026-09-21 analysis of
a knockback-revert defect reached a wall because of exactly that: a client disagreed with the
authority on **36 and then 70 consecutive corrections**, every one granted a rewind, and the
question *which field kept disagreeing, and by how much* was not answerable from any line in any
log. The field identity in that report is marked INFERRED. This header exists to make it
MEASURED.

**Three shapes were considered and two rejected.**

1. **Widen `isSimilarTo` to return a reason.** Rejected: it is on the per-correction decision
   path on every client, for every character, on every tick a correction lands. Its cost is the
   one cost in this area nobody has to think about, and it stops being that the moment it
   carries a payload. It is also the CONTROL — the thing every case in
   `CorrectionFieldDivergenceTest.cpp` and `BrawlerCorrectionFieldDivergenceTest.cpp` asserts is
   unchanged. A control you edited is not a control.
2. **Compute the difference at the emit site**, in `NetSyncTelemetry::emitCorrectionArrival`.
   Rejected as impossible, not merely undesirable: by the time that runs, a disagreeing
   correction has already overwritten the cache slot (`m_stateBuffer[cacheIndex] =
   std::move(state)`), so the predicted value the diff needs no longer exists. This is why the
   walk sits in the cache and only its RESULT travels, through `CorrectionInsertVerdict` ->
   `CorrectionArrivalDecision` -> the log line.
3. **A second entry point beside the fold, gated.** Adopted.

⛔ Nothing in this header may be read by the simulation. A branch on `FieldDivergence` outside a
log line would make the shipped build's behaviour depend on the log verbosity.

---

## 2. The cost gate — what "no diff at Warning" means, and how it is a number

The shipped configuration in one consumer sets the per-correction line's category to Warning,
which drops the `[Verbose][DivergenceProbe.Correction]` line entirely. The walk must cost
nothing there. That is the design's central constraint and the acceptance criterion it was
written against.

**"No diff is computed" is enforced by three conditions at ONE call site**, all of them in
`tryInsertingCorrectState`, all evaluated before `describeFirstDivergingField`'s arguments are
formed:

| condition | what it excludes | why it is not one of the others |
|---|---|---|
| `outDiagnosticVerdict != nullptr` | callers that cannot read the answer | the cache is deliberately id-agnostic; most call sites pass nothing |
| `!predictionWasCorrect` | agreeing corrections | an agreeing correction has no first differing field to name, and agreements are the common case |
| `correctionFieldDiff::enabled()` | the shipped verbosity | the only one that reads host state, and the only one that can change mid-session |

`&&` short-circuits left to right, so the two free tests run first and the host predicate is
only invoked on a correction that already disagreed.

**Why a predicate and not a bool.** `enabled()` calls a `std::function<bool()>` every time
rather than reading a latched flag, so a console `LogOGDivergenceProbe Verbose` typed mid-session
starts naming fields on the next correction and Warning stops it again. A value read once at
composition would pin a whole session to whatever the configuration said at startup — and a
diagnostic you cannot switch on during the event you are diagnosing is not much of a diagnostic.

**Why it ships CLOSED.** `g_enabledPredicate` is an empty `std::function` in this submodule. A
host that never installs one walks no field ever. That is a stronger statement than "the gate
answers correctly" and it is asserted separately, because it is what makes the claim true for
every consumer of og-simulation rather than for the one adapter that happens to wire it.

**Why the counter counts WALKS.** `correctionFieldDiff::g_walkCount` is incremented at the
single entry point, `describeFirstDivergingField`. A probe that counted log lines, or formatted
strings, would pass in the exact scenario this has to catch: the walk running and its result
being discarded downstream. The test file drives five disagreeing corrections through the real
cache in each of three arms:

| arm | predicate | walks | verdict |
|---|---|---|---|
| unconfigured host | none installed | 0 | unchanged |
| shipped verbosity | installed, returns false (and counts its own calls: 5) | 0 | unchanged |
| Verbose | installed, returns true | 5 | unchanged |

The third arm is the anti-vacuity control. Without it the two zeros are equally consistent with
an instrument that does nothing at all.

**The globals.** `g_enabledPredicate` and `g_walkCount` are process-global for the reason
`simlog::g_sink` is: the cache is constructed per character, deep inside a template, by call
sites this feature has no business editing. They are game-thread only — the one production
reader is the replication-dispatched correction callback — and deliberately not atomic, matching
every other member of this telemetry family.

---

## 3. Field names — why a member and not a template parameter, and what was measured

The path `dAttackRadialSimulation::State.bodyState.angularVelocity` needs the member SPELLINGS,
and the descriptors did not carry them.

**`nameof` for a member does not work on this toolchain. MEASURED**, on MSVC 14.38.33130 — the
toolset UBT selects for every target in one consumer's tree — with `/std:c++20 /permissive-`:

```
template <auto P>    const char* sigOf()  { return __FUNCSIG__; }
template <typename T> const char* tsigOf() { return __FUNCSIG__; }

sigOf<&LinearBodyState::x>()                        -> "const char *__cdecl sigOf<pointer-to-member(0x0)>(void)"
sigOf<&dAttackRadialSimulation::State::bodyState>() -> "const char *__cdecl sigOf<pointer-to-member(0x0)>(void)"
tsigOf<dAttackRadialSimulation::State>()            -> "const char *__cdecl tsigOf<struct dAttackRadialSimulation::State>(void)"
```

The member spelling is **gone**; the TYPE spelling is not. That asymmetry is why type names come
from the compiler (§6) and field names come from the macro.

**So the spelling had to come from `SIM_MEMBER(Class, member)`'s `#member`.** Two ways to carry
it, and the choice was forced:

* **As a second non-type template parameter** (`MemberFieldDesc<&S::f, "f">`). This changes the
  descriptor's TYPE.
* **As a non-static data member**, aggregate-initialised by the macro. This does not.

⭐ **The type identity is load-bearing, and the cost of getting it wrong was MEASURED rather
than reasoned about.** Several games pin their wire layout with an APPEND-ONLY fence of the form

```cpp
static_assert(std::is_same_v<
        decltype(SerializableFields<S>::get()),
        std::tuple<MemberFieldDesc<&S::a>, MemberFieldDesc<&S::b>, ...>>,
    "S - APPEND ONLY. The wire layout is POSITIONAL ...");
```

Building one consumer's test target with the descriptor's name poisoned into a template
parameter fired **five distinct APPEND-ONLY fences in two files** — three in
`BrawlerMovementSimulation.h` and two in `BrawlerRingoutSimulation.h` — with
`Result: Failed (OtherCompilationError)`. `BrawlerMovementSimulation.h` was under another
initiative's uncommitted work at the time and could not be touched at all.

As a member, the descriptor's type is byte-for-byte the one those fences name, and every one of
them stayed green. The two assertions that keep it that way live in
`CorrectionFieldDivergenceTest.cpp`: an `is_same_v` between the macro's expansion and the bare
type, PAIRED with `name != nullptr` — because identity alone is equally satisfied by dropping
the name altogether.

**The honest edge.** `forEachField` default-constructs each descriptor
(`std::tuple_element_t<Is, Fields>{}`), and four sub-simulations spell their descriptors as bare
types rather than through the macro. Both produce `name == nullptr`, and the walk falls back to
`#<index>`. That is not a defect to patch by editing those call sites — which are in files this
work was forbidden to touch anyway — it is a documented property, and every reader of `name` has
the fallback.

---

## 4. Agreement with the fold — the definition-point rule

The walk must give the same answer as the fold, per field, or the instrument describes a
divergence the simulation did not act on. It achieves that by **calling the fold's own
functions**, not by re-deriving the comparison:

* a `Serializable<V>` field is tested with `fieldwiseIsSimilarTo<V>` — literally the function
  `fieldwiseIsSimilarTo` calls on itself;
* a composite ELEMENT is tested with `compositeDetail::compareElement` — what
  `SimulationComposite::isSimilarTo` folds over, which matters because an element may define a
  member `isSimilarTo` that overrides the fieldwise walk;
* a leaf is tested with `leafIsSimilarToAsFieldwiseDoes<V>`, which exists **only** to make the
  leaf question resolve at the fold's definition point.

**Why that last one needs a shim at all.** `fieldwiseIsSimilarTo` calls `isSimilarToField`
DEPENDENTLY. Under `/permissive-`, ordinary lookup for a dependent call is frozen at the
TEMPLATE'S DEFINITION POINT and ADL contributes only the arguments' own associated namespaces. So
the overload set the fold sees is decided by where the fold is written — and it excludes both the
`Serializable<T>` and the `std::vector<T>` overloads declared below it, and the glm overloads in
`SimulationComparisonGlm.h` (global namespace, not `glm`, so ADL never finds them).

A walk written in a different header would see a DIFFERENT overload set and would, for instance,
forgive with an epsilon a `std::vector` field the fold compared with `operator==`. The shim is
declared on the line after the fold, so its overload set is identical by construction rather than
by inspection. ⛔ Moving it below the two overloads is a behaviour change.

**The tripwire, and why the obvious one was VACUOUS.** The claim is only observable on a
SUB-EPSILON plant, where an exact `==` and the 1e-4 epsilon disagree. The first attempt planted a
single sub-epsilon difference and asserted `named() == !isSimilarTo()` — and with the leaf
predicate poisoned to a bare `==`, **it still passed**. The reason is structural: with one
difference planted, the COMPOSITE-LEVEL fold forgives the whole element and the walk never
descends into it, so the poisoned leaf is unreachable. The case that bites plants TWO — a
sub-epsilon difference in an EARLIER descriptor and a real one in a LATER one — so the element
already differs, the walk descends, and the walk must name the SECOND field. A re-derived exact
leaf names the first.

⛔ **The sub-epsilon cases assert a RELATION, never a direction.** Under the lookup rule above a
bare float compares with the epsilon while a `glm::vec3` compares exactly — a known property of
this tree that is deliberately NOT fixed here and deliberately NOT pinned. A case asserting "a
1e-5 vector difference IS a divergence" would turn the eventual fix into a false regression. What
must hold either way is that one verdict comes out of this codebase.

---

## 5. The path grammar, the magnitude, and `Opaque`

**The path** is the composite element's type name, then one dotted segment per descriptor
descended through, down to the scalar leaf:
`dAttackRadialSimulation::State.bodyState.angularVelocity`. Capacity is 96 characters — the
longest path this tree can produce is 56 — and the buffer TRUNCATES rather than dropping the
segment it cannot fit, because a shortened path is still a name an operator can grep.

⛔ **96 is also a log-line budget, not just a string budget.** `SIMLOG` formats into `char[256]`.
The correction line's fixed part is ~101 characters at maximum field widths, so a path much
longer than 120 would silently truncate the `delta=` the line exists to carry — turning the
instrument back into a field name with no magnitude, which is the half-answer this task exists to
replace. A case asserts a maximal line lands strictly inside 255.

**The magnitude is the MAX COMPONENT |Δ|, not a norm.** A norm hides which axis moved and
compares badly across differently-shaped fields; the max component is the number an operator can
put next to a tolerance. It is detected STRUCTURALLY — `if constexpr (requires { a.x; })` and so
on for `y`, `z`, `w` — rather than through overloads for the glm types, and that is not a style
choice: an overload declared in `SimulationComparisonGlm.h` would be invisible to a dependent
call from this header for exactly the reason §4 gives, so it would COMPILE, be SILENTLY SKIPPED,
and every weapon-spin delta would report 0. The structural form covers vec2, vec3 and quat
without naming any of them.

**First, in declaration order.** `walkFieldsImpl` is a short-circuiting `||` fold, which is the
same order and the same first-false semantics `fieldwiseIsSimilarTo` stops on. An accumulating
fold would report the LAST differing field.

**`Opaque` is the honest middle.** The walk asks the fold's question FIRST and only then descends
to refine the name — so the answer never depends on the descent succeeding. When a field the fold
called different cannot be descended into (a member `isSimilarTo`, a `std::vector`, an element
with no `SerializableFields`), the field is still named and the kind is `Opaque`. `None` means
something else entirely: the walk named nothing at all, which on a disagreeing pair means the
walk and the fold disagreed and is a defect. The two are kept apart so a future failure reads as
a failure.

---

## 6. Type names from the compiler signature

`typeName<T>()` parses `__FUNCSIG__` (MSVC) or `__PRETTY_FUNCTION__` (gcc/clang) once, on first
use, into a function-local static, and strips MSVC's elaborated `struct ` / `class ` / `enum `
prefix. An unparsed signature yields `?`: a diagnostic that can abort is worse than one that says
it does not know.

⚠ **MEASURED, and it shows up in tests rather than in production.** MSVC spells a type declared in
an ANONYMOUS namespace as `` `anonymous-namespace'::MockMovementState ``, and the walk faithfully
reports that as the path root — correct, and unreadable. There is nothing to fix in the walk:
every type it names in production is in a named namespace. The test mocks were moved into a named
namespace for that reason, which also keeps them an honest sample of the production shape. The
first run of that file asserted the unqualified spelling and failed with exactly that expansion.

---

## 7. Keeping this document true

Every section above is a record of a decision and its evidence at the time it was taken. ⚠ What
that does NOT protect is the symbols the sections name: a rename lands here as a stale identifier.
The `lint-external-ref` declarations at the top exist for names that live outside this submodule
and must not resolve here; a new one needs a new declaration in the same change, and an unused
declaration is itself a failure.

⛔ **A clean anchor-lint run is not evidence that anything here is TRUE.** That checker resolves
names. Read the header.
