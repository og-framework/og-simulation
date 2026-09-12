<!-- SPDX-License-Identifier: MPL-2.0 -->
# `PhysicsDeclaration.h` — guards

Every fence that stood in the header. Each entry has an **opaque, stable id**; in the header a
single line `// ⛔G-nn` sits exactly where the fence's text used to sit, on the same
declaration.

**If this file and `PhysicsDeclaration.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is
moved to §R and its number is spent forever. Reusing one silently re-points every reference that
ever named it. A retired id may be **named** in prose or in a `static_assert` message; it may
never again appear as a `⛔G-nn` **tag**.

⭐ **The join is machine-checked, in both directions** by the guard-tag lint that ships with the
consuming repository (`guard_tag_lint.ps1`): every tag resolves to an entry here, every live entry
is referenced by exactly one tag, no id is duplicated, and no retired id reappears as a tag. It is
a hard gate. ⚠ It checks that an entry EXISTS. It never checks that this text is TRUE.
⚠ That tool is **not distributed with this submodule**; the discipline it enforces is stated here
so it survives without it.

⛔ **Nothing in this file is a rationale.** The narrative, the provenance and the orientation for
a sub-simulation author live in `PhysicsDeclaration-rationale.md`. A guard is a prohibition plus
the consequence of ignoring it, and nothing else.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below
is stated so that it is true and actionable to a reader with no game, no engine integration and
no other repository open. Where a consumer outside this submodule is named it is named as
**provenance** and marked as not distributed here.

⭐ **Three of this header's four fences left it, and two of those became compile errors.** That is
recorded in §R, with the text they replaced, so the trade is visible rather than silent.

<!-- lint-external-ref: SimulatableBrawler.h -- header of the host game that consumes this submodule; provenance only, not distributed with og-simulation -->
<!-- lint-external-ref: SimulationManagerUImpl.cpp -- engine-integration translation unit of the host game; provenance only, not distributed with og-simulation -->

---

## G-01 — The assertions do not live in this header, and not on the composite either

**Tag site:** `PhysicsDeclaration.h`, immediately above
`template <typename D, typename GameStaticDataType>` — the concept's own declaration.
**Taxonomy clause:** F1d — a block defending its own placement against a named move
(must-never-move **T2c-4**).

**The fence, verbatim — these are the bytes it occupied in the header (lines 25-28 of the
pre-conversion file):**

```
// The assertions themselves DO NOT live here, and they do not live on
// SimulationPhysicsComposite either: the concept is parameterised on the GAME's
// StaticData type, and only the game's own simulatable header knows it. Assert
// there, once per declaration.
```

**What breaks if it moves.** This block exists to say the assertion was deliberately *not* put in
this header and *not* on the composite, and why: the concept's second parameter is the
**consuming game's aggregate static-data type**, and neither this header nor
`SimulationPhysicsComposite` — an engine-side alias — has any way to name it. A reader who thinks
the concept is unasserted adds a "helpful" `static_assert` here; the assertion cannot be written
at all, and the effort is spent before the note that would have redirected it is ever found. The
correct home is the consumer's own simulatable header, one assertion per declaration, where that
type is in scope.

⚠ **This guard survived step zero and is NOT a compile-time check, for a reason worth stating.**
The forbidden edit is *adding* an assertion. There is no assertion whose failure corresponds to
someone having added one, so the compiler cannot express this prohibition — the verdict is
DOES NOT CONVERT, and it is a verdict about what an assertion can say, not about this tree.

⚠ **Read this guard together with the `physicsDeclarationSelfCheck` block at the bottom of the
header.** That block *does* contain `static_assert`s naming this concept, and they are not a
counter-example: they assert about **probe types the block defines itself**, never about a
consumer's declaration. The distinction is the whole of G-01 — a declaration this header can name
is assertable here; a declaration only the consumer can name is not.

---

## §R Retired ids

⛔ **These ids are spent.** They may be named — G-03 and G-04 are named in the `static_assert`
messages that replaced them — but they must never again appear as a `⛔G-nn` **tag** in source.

### G-02 — RETIRED, deleted: its forbidden edit is typed in a consumer's file

Was must-never-move **T2c-1** (F6, described by the audit as a textbook case):
*"Two different entities share one name: this concept and each sub-sim's declaration struct.
Inside a sub-sim namespace the unqualified spelling silently picks the struct, so
`static_assert(PhysicsDeclaration<D, SD>)` becomes a nonsense instantiation of the struct — and
the error, if any, names a template the author never wrote. The correction only works where the
name is typed."*

**The text it replaced, verbatim (lines 30-33 of the pre-conversion file):**

```
// ⚠ NAME NOTE: sub-simulation namespaces conventionally call their declaration
// type `PhysicsDeclaration` too. Inside such a namespace the unqualified name
// resolves to the STRUCT; write `::PhysicsDeclaration<Decl, GameStaticData>` to
// reach this concept.
```

**Why it is retired rather than tagged.** ⭐ **The edit it forbids is never typed in this file.**
The ambiguity only exists *inside a sub-simulation's own namespace*, in the consumer's simulatable
header, at the line where the assertion is written. A tag here sits where nobody is standing when
they make the mistake, and CommentExtractionRule v2's own rule for that case is explicit: a fence
whose forbidden edit is typed in another file gets no tag — it is routed, or deleted where a
working copy already stands at the real site.

A working copy **does** already stand at the real site in the reference consumer
(`SimulatableBrawler.h`, immediately above its six assertions:
*"⚠ QUALIFIED ::PhysicsDeclaration — the sub-simulation namespaces each define a STRUCT of that
name, so the unqualified spelling is ambiguous here."*). ⚠ **That file is provenance and is not
distributed with this submodule**, so the claim is *also* carried, as orientation rather than as a
fence, in `PhysicsDeclaration-rationale.md` — which is the copy a standalone consumer gets.

⛔ **The naming hazard itself is unchanged and is not a defect of this decision.** It is a
property of C++ name lookup, it is legal, and no compiler diagnostic exists for it: a consumer who
writes the unqualified spelling gets a diagnostic about a template they never wrote. The only
correction that works is one the author reads **while typing their own assertion**.

### G-03 — RETIRED, converted to two `static_assert`s

Was must-never-move **T2c-3** (F5a + F6): *"Two adjacent requirements on what looks like one
function read as duplication — exactly the shape someone deletes as redundant. They are checked at
two different generic call sites (capture vs rewind push) with two different requirements, and
nothing else relates them."*

**The text it replaced, verbatim (lines 82-87 of the pre-conversion file):**

```
	// BOTH overloads are checked, for DIFFERENT reasons. The mutable one is the
	// capture target, so its referent must model the body-state pair (see
	// PhysicsBodyState.h). The const one is what the rewind push READS:
	// `pushBodyState(id, D::bodyStateOf(state))` takes a `const PhysicsBodyState&`
	// at a generic call site, so it must WIDEN to a full body state — existing is
	// not enough. Nothing relates the two overloads' return types otherwise.
```

**Now enforced by**, in the header's `physicsDeclarationSelfCheck` block, two negative controls
that the concept must reject:

* `ProbeNonWideningConst` — a declaration whose **const** `bodyStateOf` returns something that is
  not convertible to `PhysicsBodyState`, and whose mutable overload is otherwise perfect. Only the
  `{ D::bodyStateOf(cst) } -> std::convertible_to<PhysicsBodyState>` requirement can reject it.
* `ProbeMutableNotBodyStateLike` — the mirror: the **mutable** overload's referent is not
  `BodyStateLike`, and the const overload widens correctly. Only the
  `requires BodyStateLike<...>` line can reject it.

**Deleting either requirement is now a translation failure in every translation unit that includes
this header**, which is exactly the edit the sentence forbade. ⭐ Each assertion is false for
exactly **one** reason, and the header carries the two discrimination assertions that prove it —
so neither control can silently start passing for the wrong reason.

### G-04 — RETIRED, converted to two `static_assert`s

Was must-never-move **T2c-2** (F2 + F5a): *"Explains why the requirement is
`std::same_as<PhysicsRuntimeBindings&>` and not `convertible_to`. `convertible_to` is the reflex
relaxation when a new declaration fails the concept — it compiles, and the fold then writes
through a temporary or fails to write at all. The consequence is invisible at the requirement line
without this sentence."*

**The text it replaced, verbatim (lines 91-95 of the pre-conversion file):**

```
	// The creation fold WRITES all five members of this after it makes the body,
	// so a read-only — or merely convertible — binding is false confidence. It
	// must BE the one shared PhysicsRuntimeBindings, as a mutable lvalue. A
	// field-identical per-sim copy is a distinct type and correctly fails here;
	// that is what makes adopting the shared type mandatory, not cosmetic.
```

**Now enforced by**, in the header's `physicsDeclarationSelfCheck` block, two negative controls
that the concept must reject:

* `ProbeDerivedBindings` — a `bindings` member whose type **derives from**
  `PhysicsRuntimeBindings`. A derived lvalue converts to both `PhysicsRuntimeBindings&` and
  `const PhysicsRuntimeBindings&`, so this control fires on the reflex relaxation in **either**
  spelling, and on deleting the requirement altogether.
* `ProbeConstBindings` — a **read-only** member of the shared type. It fires on the historical
  `convertible_to<const PhysicsRuntimeBindings&>` spelling and on `same_as<const …&>`, which
  `ProbeDerivedBindings` does not reach.

⚠ **The control the sentence itself named is deliberately NOT the enforcing one.** *"A
field-identical per-sim copy is a distinct type and correctly fails here"* is true under **every**
candidate spelling — `same_as`, `convertible_to`, `convertible_to<const …&>` alike — so a
look-alike control proves nothing about the choice the fence is defending. It is kept in the
header as `ProbeLookAlikeBindings`, labelled as the non-discriminating control, precisely so that
nobody later mistakes it for the check.

⭐ **Why this is not merely a shorter sentence.** The creation fold in the consuming engine
integration writes **all five** members of `bindings` after it creates the body
(`SimulationManagerUImpl.cpp`; provenance, not distributed here). A binding that is read-only, or
merely convertible, cannot receive those writes — and under the loose spelling the declaration
compiles, satisfies the concept, and loses every handle the fold produced.
