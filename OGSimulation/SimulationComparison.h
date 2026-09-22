#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/SimulationComposite.h"
#include "OGSimulation/SimulationSerialization.h"
#include "OGSimulation/SimulationTypes.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// SimulationComparison — THE SECOND ENTRY POINT BESIDE `isSimilarTo`.
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free, GLM-free.
//
// Rationale, the rejected alternatives and the archived measurements:
// `docs/SimulationComparison-rationale.md` — the `§N` marks below are its sections.
//
// ---------------------------------------------------------------------------
// ORIENTATION — WHAT THIS FILE IS FOR, WHAT IT MUST NOT BECOME, AND WHO PAYS.
//
// Read this first. Every fence below states one invariant at the line it guards;
// none of them restates this map, and this map states no invariant.
//
//   * THE QUESTION IT ANSWERS. `isSimilarTo` is a boolean fold: it decides
//     whether a resim runs, and it DISCARDS THE DISTANCE. When a client
//     disagrees with the authority for 36 and then 70 consecutive corrections,
//     "which field kept disagreeing, and by how much" is a question NO LINE IN
//     ANY LOG can answer. `describeFirstDivergingField` is the instrument that
//     answers it; everything else here exists to serve it, or to render its
//     result (`formatFieldDivergence`). §1
//
//   * IT IS A SECOND ENTRY POINT, NOT A REPLACEMENT. `isSimilarTo`,
//     `fieldwiseIsSimilarTo` and `compositeDetail::compareElement` are
//     UNTOUCHED — same code, same cost, same verdict. Nothing here is on the
//     resim decision path, and nothing here may ever be put on it. §1
//
//   * THE COST GATE IS THE POINT. The walk is ~10x the fold (it re-asks the
//     fold's question per field, then descends). It runs ONLY when the
//     per-correction `[DivergenceProbe.Correction]` Verbose line is actually
//     going to be emitted. At the SHIPPED `LogOGDivergenceProbe=Warning` NOT ONE
//     FIELD IS READ. Three things make that true, and all three are measured by
//     `Network/CorrectionFieldDivergenceTest.cpp`, never argued: §2
//       1. the predicate is UNSET by default, so a host that never opts in pays
//          nothing at all — including every other game that links this submodule;
//       2. the one production call site tests `correctionFieldDiff::enabled()`
//          BEFORE forming the walk's arguments;
//       3. `correctionFieldDiff::walkCount()` counts entries to the walk itself,
//          not log lines, so "no diff was computed" is a NUMBER rather than a
//          reading of the source.
//
//   * WHO CALLS IT. Exactly one production site:
//     `StateCorrectionCache::tryInsertingCorrectState`, on the landed-and-
//     DISAGREEING path only, on the GAME thread (the correction callback). The
//     result rides out through `CorrectionInsertVerdict` ->
//     `CorrectionArrivalDecision` -> `NetSyncTelemetry::emitCorrectionArrival`.
//     Nothing in this file logs; the vocabulary rule is
//     `docs/DiagnosticsConventions.md` §2/§3 and is not re-derived here.
//
//   * THE TWO GLOBALS. `correctionFieldDiff::g_enabledPredicate` and
//     `g_walkCount` are process-global for the same reason `simlog::g_sink` is:
//     the cache is constructed per character, deep inside a template, by call
//     sites this feature has no business editing. They are GAME-THREAD ONLY,
//     like every other member of this telemetry family, and deliberately not
//     atomic — see the fence at each one. §2
// ---------------------------------------------------------------------------
//
// ⛔ NOTHING IN THIS FILE MAY BE READ BY THE SIMULATION. It is a reporter. A
//    branch on `FieldDivergence` outside a log line makes the shipped build's
//    behaviour depend on the log verbosity. §1
//
// ⛔ NO GLM HERE, and adding it would be a behaviour change, not a convenience —
//    see `fieldDeltaMagnitude`'s fence. §5
//
// ⛔ THIS HEADER IS NOT INCLUDED BY `SimulationSerialization.h` AND MUST NOT BE.
//    The dependency runs one way: the walk needs the fold, the fold must never
//    need the walk, or `isSimilarTo`'s cost stops being auditable in one file. §1

// ---------------------------------------------------------------------------
// FieldDivergence — WHAT THE WALK FOUND. A value, carried by
// `CorrectionInsertVerdict`; never a reference into a cache slot.
// ---------------------------------------------------------------------------

// ⛔ `None` IS NOT "the states agree" — it is "this walk named no field", which on
//    a disagreeing pair means the walk and the fold DISAGREED and is a defect.
//    `Opaque` is the honest middle: the fold says this field differs and the walk
//    could not descend into it (a member `isSimilarTo`, or a leaf with no
//    component structure). §5
enum class FieldDivergenceKind : std::uint8_t
{
    None = 0,
    Numeric,   // float / double / glm-shaped — `delta` is the max component |Δ|.
    Discrete,  // enum / bool / integer — `oldValue` / `newValue` are the two values.
    Opaque,    // differs, but no scalar leaf could be named.
};

// Path capacity. `dAttackRadialSimulation::State.bodyState.angularVelocity` is 52
// characters; the deepest composite element name in this tree plus two levels of
// nesting fits inside 96 with room to spare.
//
// ⛔ AND IT MUST STAY WELL UNDER THE SIMLOG BUFFER. `SIMLOG` formats into
//    `char[256]`; the correction line's fixed part is ~101 characters at maximum
//    field widths, so a path longer than ~120 would silently truncate the `delta=`
//    the line exists to carry. §5
inline constexpr std::size_t kFieldPathCapacity = 96;

struct FieldDivergence
{
    // ⛔ THE GATE'S OWN WITNESS, and the reason it is a separate flag from `kind`:
    //    "the walk did not run" and "the walk ran and found nothing" are different
    //    facts, and collapsing them would make a disabled probe read as a clean
    //    bill of health. §2
    bool evaluated = false;

    FieldDivergenceKind kind = FieldDivergenceKind::None;

    // Dotted path from the composite element type down to the scalar leaf, e.g.
    // `dAttackRadialSimulation::State.bodyState.angularVelocity`. Always NUL
    // terminated. `#<n>` appears for a descriptor written as a bare type, which
    // carries no spelling — see `SIM_MEMBER`'s fence. §3
    char path[kFieldPathCapacity] = {};

    // MAX COMPONENT |Δ|, not a norm: a norm hides which axis moved and compares
    // badly across differently-shaped fields. `Numeric` only. §5
    float delta = 0.f;

    // `Discrete` only. Enums arrive here as their underlying value.
    long long oldValue = 0;
    long long newValue = 0;

    bool named() const { return kind != FieldDivergenceKind::None; }
};

// ---------------------------------------------------------------------------
// correctionFieldDiff — THE COST GATE AND ITS COUNTER. §2
// ---------------------------------------------------------------------------

namespace correctionFieldDiff
{

// ⛔ UNSET IS OFF, AND THAT IS THE DEFAULT SHIPPED STATE OF THIS SUBMODULE. A host
//    that never installs a predicate never walks a field, which is what makes
//    "no diff at Warning" true for every consumer of og-simulation and not just
//    for the one adapter that happens to wire it. §2
//
// ⛔ NOT A `bool`. The answer must be re-read per correction from the host's LIVE
//    log configuration — a console `LogOGDivergenceProbe Verbose` mid-session has
//    to start naming fields without a restart, and a latched bool cannot. §2
//
// ⛔ GAME THREAD ONLY, non-atomic, like every other member of this telemetry
//    family. The one production reader is the correction callback. §2
inline std::function<bool()> g_enabledPredicate;

inline void setEnabledPredicate(std::function<bool()> predicate)
{
    g_enabledPredicate = std::move(predicate);
}

inline void clearEnabledPredicate()
{
    g_enabledPredicate = nullptr;
}

inline bool enabled()
{
    return static_cast<bool>(g_enabledPredicate) && g_enabledPredicate();
}

// ⛔ THE COUNTER COUNTS WALKS, NOT LOG LINES, and that is the difference between
//    an instrument and a restatement. A line-counting probe would pass while the
//    walk ran and its result was thrown away — exactly the regression this exists
//    to catch. Incremented at the single entry point below. §2
inline std::uint64_t g_walkCount = 0u;

inline std::uint64_t walkCount() { return g_walkCount; }
inline void resetWalkCount() { g_walkCount = 0u; }

} // namespace correctionFieldDiff

// ---------------------------------------------------------------------------
// fieldDivergenceDetail — the walk. §5
// ---------------------------------------------------------------------------

namespace fieldDivergenceDetail
{

// Appends `segment`, prefixed by '.' unless the path is empty.
//
// ⛔ TRUNCATES, NEVER OVERFLOWS, AND NEVER DROPS THE SEGMENT IT CANNOT FIT — a
//    shortened path is still a name an operator can grep; a missing one is not.
inline void appendSegment(FieldDivergence& out, const char* segment)
{
    std::size_t len = std::strlen(out.path);
    if (segment == nullptr)
        segment = "?";
    if (len != 0u && len + 1u < kFieldPathCapacity)
        out.path[len++] = '.';
    for (const char* s = segment; *s != '\0' && len + 1u < kFieldPathCapacity; ++s)
        out.path[len++] = *s;
    out.path[len] = '\0';
}

// `#<index>` — the fallback name for a descriptor written as a bare type. §3
inline void appendIndex(FieldDivergence& out, std::size_t index)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%u", static_cast<unsigned int>(index));
    appendSegment(out, buf);
}

// THE COMPILER'S OWN SPELLING OF A TYPE. §6
//
// ⚠ MEASURED, NOT ASSUMED, on MSVC 14.38.33130 (the toolset UBT selects for every
//   target here): `__FUNCSIG__` prints
//   `const char *__cdecl typeSignature<struct dAttackRadialSimulation::State>(void)`.
//   The same trick for a POINTER-TO-MEMBER prints `pointer-to-member(0x0)` — the
//   spelling is gone — which is why field names come from the macro instead. §3
template <typename T>
const char* typeSignature()
{
#if defined(_MSC_VER) && !defined(__clang__)
    return __FUNCSIG__;
#else
    return __PRETTY_FUNCTION__;
#endif
}

// Parses `typeSignature<T>()` down to a bare type name once, on first use, into
// its own storage. ⛔ THE FALLBACK IS THE POINT: an unparsed signature yields `?`,
// never a crash and never a truncated lie — a diagnostic that can abort is worse
// than one that says it does not know. §6
struct TypeNameStorage
{
    char value[128] = {};

    explicit TypeNameStorage(const char* signature)
    {
        const char* begin = nullptr;
        const char* end = nullptr;

        // gcc / clang: `... [with T = X::Y]` or `... [T = X::Y]`.
        if (const char* eq = std::strstr(signature, "T = "))
        {
            begin = eq + 4;
            end = std::strrchr(begin, ']');
        }
        // MSVC: `... typeSignature<struct X::Y>(void)`.
        else if (const char* lt = std::strchr(signature, '<'))
        {
            begin = lt + 1;
            end = std::strrchr(begin, '>');
        }

        if (begin == nullptr || end == nullptr || end <= begin)
        {
            std::snprintf(value, sizeof(value), "?");
            return;
        }

        // MSVC spells the elaborated form; the games never write it, so neither do we.
        static const char* const kElaborations[] = { "struct ", "class ", "enum " };
        for (const char* prefix : kElaborations)
        {
            const std::size_t n = std::strlen(prefix);
            if (static_cast<std::size_t>(end - begin) > n && std::strncmp(begin, prefix, n) == 0)
            {
                begin += n;
                break;
            }
        }

        std::size_t n = static_cast<std::size_t>(end - begin);
        if (n >= sizeof(value))
            n = sizeof(value) - 1u;
        std::memcpy(value, begin, n);
        value[n] = '\0';
    }
};

template <typename T>
const char* typeName()
{
    static const TypeNameStorage storage(typeSignature<T>());
    return storage.value;
}

// THE MAGNITUDE. §5
//
// ⛔ SHAPE-DETECTED, NOT GLM-OVERLOADED, AND THE REASON IS THE SAME LOOKUP TRAP
//    THAT MAKES `isSimilarToField` EXACT FOR VECTORS: an overload declared in
//    `SimulationComparisonGlm.h`, in the GLOBAL namespace, is invisible to a
//    dependent call from here under /permissive- (ordinary lookup is frozen at
//    this definition point; ADL searches `glm`, where the overload is not). A
//    `fieldDeltaMagnitude(glm::vec3)` overload would therefore COMPILE, be
//    SILENTLY SKIPPED, and every weapon-spin delta would report 0. Detecting the
//    components structurally cannot be defeated that way. §5
//
// ⚠ It covers vec2 (x,y), vec3 (x,y,z) and quat (x,y,z,w) without naming any of
//   them, and a future vec4 for free.
template <typename V>
float fieldDeltaMagnitude(const V& a, const V& b)
{
    const auto axis = [](auto x, auto y) {
        return static_cast<float>(std::abs(static_cast<double>(x) - static_cast<double>(y)));
    };
    const auto keepMax = [](float& best, float candidate) {
        if (candidate > best)
            best = candidate;
    };

    float worst = 0.f;
    if constexpr (std::is_arithmetic_v<V>)
    {
        keepMax(worst, axis(a, b));
    }
    else
    {
        if constexpr (requires { { a.x } -> std::convertible_to<double>; })
            keepMax(worst, axis(a.x, b.x));
        if constexpr (requires { { a.y } -> std::convertible_to<double>; })
            keepMax(worst, axis(a.y, b.y));
        if constexpr (requires { { a.z } -> std::convertible_to<double>; })
            keepMax(worst, axis(a.z, b.z));
        if constexpr (requires { { a.w } -> std::convertible_to<double>; })
            keepMax(worst, axis(a.w, b.w));
    }
    return worst;
}

// Fills in `kind` plus the payload for a leaf the fold has already called DIFFERENT.
template <typename V>
void describeLeaf(const V& a, const V& b, FieldDivergence& out)
{
    if constexpr (std::is_enum_v<V>)
    {
        out.kind = FieldDivergenceKind::Discrete;
        out.oldValue = static_cast<long long>(static_cast<std::underlying_type_t<V>>(a));
        out.newValue = static_cast<long long>(static_cast<std::underlying_type_t<V>>(b));
    }
    else if constexpr (std::is_integral_v<V>)
    {
        out.kind = FieldDivergenceKind::Discrete;
        out.oldValue = static_cast<long long>(a);
        out.newValue = static_cast<long long>(b);
    }
    else if constexpr (std::is_floating_point_v<V>
                       || requires { { a.x } -> std::convertible_to<double>; })
    {
        out.kind = FieldDivergenceKind::Numeric;
        out.delta = fieldDeltaMagnitude(a, b);
    }
    else
    {
        // std::array, std::vector, a bare aggregate with no SerializableFields —
        // the fold said they differ and there is no component to point at.
        out.kind = FieldDivergenceKind::Opaque;
    }
}

template <typename T>
bool walkFields(const T& a, const T& b, FieldDivergence& out);

// ONE FIELD. Returns true when this field is the first differing one, having
// written the path and the payload.
//
// ⛔ THE SIMILARITY TEST HERE IS THE FOLD'S OWN, BRANCH FOR BRANCH. The
//    `Serializable<V>` arm calls `fieldwiseIsSimilarTo` — literally the same
//    function `fieldwiseIsSimilarTo` calls on itself — and the leaf arm calls
//    `leafIsSimilarToAsFieldwiseDoes`, which exists precisely so the leaf question
//    resolves at the FOLD'S definition point rather than this one. Re-deriving
//    either would give the walk a second notion of similarity, and the first
//    epsilon change would make the line name a field the verdict never disagreed
//    about. §4
template <typename T, std::size_t I>
bool walkOneField(const T& a, const T& b, FieldDivergence& out)
{
    using Fields = decltype(SerializableFields<T>::get());
    using FD = std::tuple_element_t<I, Fields>;
    using V = typename FD::Value;

    const auto& av = FD::read(a);
    const auto& bv = FD::read(b);

    if constexpr (Serializable<V>)
    {
        if (fieldwiseIsSimilarTo<V>(av, bv))
            return false;
    }
    else
    {
        if (leafIsSimilarToAsFieldwiseDoes<V>(av, bv))
            return false;
    }

    // The spelling lives on the descriptor INSTANCE, so the tuple must be built,
    // not merely named. It is a tuple of pointers; this runs under the gate only.
    const auto fields = SerializableFields<T>::get();
    const char* name = std::get<I>(fields).name;
    if (name != nullptr)
        appendSegment(out, name);
    else
        appendIndex(out, I);

    if constexpr (Serializable<V>)
    {
        // ⛔ REFINE, DO NOT RE-DECIDE. The field above IS the answer; descending
        //    only makes the name more specific. If the descent names nothing the
        //    answer stays, marked Opaque — never discarded. §5
        if (!walkFields<V>(av, bv, out))
            out.kind = FieldDivergenceKind::Opaque;
    }
    else
    {
        describeLeaf(av, bv, out);
    }
    return true;
}

template <typename T, std::size_t... Is>
bool walkFieldsImpl(const T& a, const T& b, FieldDivergence& out, std::index_sequence<Is...>)
{
    // ⛔ A SHORT-CIRCUITING `||` FOLD, IN DECLARATION ORDER — the same order and the
    //    same first-false semantics `fieldwiseIsSimilarTo` stops on. A `&&`-style
    //    accumulation would report the LAST differing field. §5
    return (walkOneField<T, Is>(a, b, out) || ...);
}

template <typename T>
bool walkFields(const T& a, const T& b, FieldDivergence& out)
{
    using Fields = decltype(SerializableFields<T>::get());
    return walkFieldsImpl<T>(a, b, out, std::make_index_sequence<std::tuple_size_v<Fields>>{});
}

// ONE COMPOSITE ELEMENT. Mirrors `SimulationComposite::isSimilarTo`'s fold, which
// goes through `compositeDetail::compareElement` — NOT through
// `fieldwiseIsSimilarTo` directly, because an element may define a member
// `isSimilarTo` that overrides it. §4
template <typename E>
bool walkOneElement(const E& a, const E& b, FieldDivergence& out)
{
    if (compositeDetail::compareElement(a, b))
        return false;

    appendSegment(out, typeName<E>());

    if constexpr (Serializable<E>)
    {
        if (!walkFields<E>(a, b, out))
            out.kind = FieldDivergenceKind::Opaque;
    }
    else
    {
        out.kind = FieldDivergenceKind::Opaque;
    }
    return true;
}

template <typename... Ts>
bool walkComposite(const SimulationComposite<Ts...>& a,
                   const SimulationComposite<Ts...>& b,
                   FieldDivergence& out)
{
    return (walkOneElement<Ts>(a.template get<Ts>(), b.template get<Ts>(), out) || ...);
}

} // namespace fieldDivergenceDetail

// `IsSimulationComposite<T>` — the dispatch discriminator for the entry point
// below. A trait rather than a `requires`, so the three arms stay mutually
// exclusive by construction.
template <typename T>
struct IsSimulationComposite : std::false_type {};

template <typename... Ts>
struct IsSimulationComposite<SimulationComposite<Ts...>> : std::true_type {};

// ---------------------------------------------------------------------------
// describeFirstDivergingField — THE ENTRY POINT. §1
//
// `predicted` is the client's slot, `authority` the arriving correction, in that
// order: `oldValue` / `newValue` read as "the prediction held X, the authority
// says Y", which is the direction an operator reads the log line in.
//
// ⛔ IT DOES NOT CHECK THE GATE. The gate belongs at the CALL SITE, before the
//    arguments are formed, or "no diff at Warning" would depend on a function
//    this file cannot see being entered at all. `tryInsertingCorrectState` holds
//    it; `Network/CorrectionFieldDivergenceTest.cpp` counts it. §2
//
// ⛔ THE COUNTER IS BUMPED HERE AND ONLY HERE — one entry, one count, whatever the
//    walk then finds. §2
// ---------------------------------------------------------------------------

template <typename S>
void describeFirstDivergingField(const S& predicted, const S& authority, FieldDivergence& out)
{
    out = FieldDivergence{};
    out.evaluated = true;
    ++correctionFieldDiff::g_walkCount;

    if constexpr (Serializable<S>)
    {
        fieldDivergenceDetail::appendSegment(out, fieldDivergenceDetail::typeName<S>());
        if (!fieldDivergenceDetail::walkFields<S>(predicted, authority, out))
        {
            // The caller only asks on a DISAGREEING correction, so an empty result
            // is the walk contradicting the fold. Say so in the path rather than
            // printing a bare type name that reads like an answer.
            out.path[0] = '\0';
            fieldDivergenceDetail::appendSegment(out, "<none>");
        }
    }
    else if constexpr (IsSimulationComposite<S>::value)
    {
        if (!fieldDivergenceDetail::walkComposite(predicted, authority, out))
        {
            out.path[0] = '\0';
            fieldDivergenceDetail::appendSegment(out, "<none>");
        }
    }
    else
    {
        // A POD test state, or a game whose state is neither. Naming the type is
        // the most this can honestly say.
        fieldDivergenceDetail::appendSegment(out, fieldDivergenceDetail::typeName<S>());
        out.kind = FieldDivergenceKind::Opaque;
    }
}

// ---------------------------------------------------------------------------
// formatFieldDivergence — the `field=… delta=…` TAIL of the
// `[DivergenceProbe.Correction]` line, as a fragment.
//
// ⛔ A FRAGMENT, NOT A LINE, and it carries no tag: the tag is the string
//    operators grep for and it belongs at the emit site with the id and the tick.
//    `docs/DiagnosticsConventions.md` §4. §5
//
// ⛔ EMPTY WHEN THE WALK DID NOT RUN. A `field=` token on a Warning-level line
//    would assert an observation nothing made. §2
// ---------------------------------------------------------------------------

inline void formatFieldDivergence(const FieldDivergence& divergence, char* buffer, std::size_t capacity)
{
    if (capacity == 0u)
        return;
    buffer[0] = '\0';
    if (!divergence.evaluated)
        return;

    switch (divergence.kind)
    {
    case FieldDivergenceKind::Numeric:
        std::snprintf(buffer, capacity, " field=%s delta=%g",
            divergence.path, static_cast<double>(divergence.delta));
        break;
    case FieldDivergenceKind::Discrete:
        std::snprintf(buffer, capacity, " field=%s old=%lld new=%lld",
            divergence.path, divergence.oldValue, divergence.newValue);
        break;
    case FieldDivergenceKind::Opaque:
        std::snprintf(buffer, capacity, " field=%s delta=?", divergence.path);
        break;
    case FieldDivergenceKind::None:
    default:
        std::snprintf(buffer, capacity, " field=%s", divergence.path);
        break;
    }
}

OGSIM_OPTIMIZE_ON
// pragma optimize on.
