<!-- SPDX-License-Identifier: MPL-2.0 -->
# `PhysicsDeclaration.h` — rationale

What a physics declaration is, why the contract is a concept rather than a convention, where the
assertions belong, and what a sub-simulation author has to provide. The header keeps the code, one
guard tag and a self-check block; this file carries the *why*.

**If this file and `PhysicsDeclaration.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Nothing in this file is a prohibition.** The one surviving fence, and the text of the three
that left the header, are in `PhysicsDeclaration-guards.md`.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Everything below is
written to be readable by someone with only this submodule. Where a consumer outside it is named,
it is named as **provenance** — an example of a real integration — and marked as not distributed
here. Its companion, `PhysicsBodyState-rationale.md`, describes the body-state shapes this
concept locates.

<!-- lint-external-ref: SimulatableBrawler.h -- header of the host game that consumes this submodule; provenance only, not distributed with og-simulation -->

---

## 1. What the header is for

`PhysicsDeclaration` is **the contract a body-owning sub-simulation offers to the generic
machinery, made CHECKABLE.**

Every sub-simulation that owns a physics body publishes a small declaration type. Generic folds
over a simulatable's physics composite reach into those declarations **by name** — they ask for the
descriptor, the debug name, the static-data slice, the query volumes, the attachment offset, the
body state and the runtime bindings.

Until this header those names were **duck-typed**: a declaration missing one of them failed deep
inside a template fold, hundreds of lines from the mistake. The concept turns that into one line
at the assertion site.

### 1.1 How many folds reach into a declaration

⛔ **Do not restate a count here.** The number of generic folds is a property of the *consuming*
integration, not of this submodule, and it moves. In the reference consumer the operations are
body creation, query-volume registration (the same fold as creation), post-solve capture, the
rewind push, a development-build check that the creation fold populated the body ids, and a
body-resolvability gate that keeps a registration pending — **five folds carrying six named
operations**, which is not the "four generic sites" an earlier version of this text asserted.

⭐ **The honest handle is the grep, not the number.** Search the consumer for its physics-composite
accessor (`getPhysicsComposite` / `editPhysicsComposite`) before assuming the set. The two folds
the old enumeration omitted were the two *gates* — and a reader who trusts an enumeration to be
complete will not know that adding a declaration whose body never resolves silently parks its
registration, or that a development build asserts on a body id left at zero.

<!-- Corrected 2026-09-11 (v2 conversion, R0). The pre-conversion header said "Four generic sites
     fold over the declarations of a simulatable ... and each one reaches into the declaration by
     name" and enumerated four operations. It was wrong by measurement on two independent
     readings, and the omissions were the load-bearing ones. -->

## 2. Where the assertions live — and where they cannot

See **G-01** in `PhysicsDeclaration-guards.md`. In short: the concept's second parameter is the
consuming game's **aggregate static-data type**, and only the consumer's own simulatable header
knows it, so that is the only place the assertion can be written — one per declaration.
`SimulationPhysicsComposite` is an engine-side alias and cannot name that type either, which is
why it carries a doc comment and no `static_assert`.

### 2.1 ⚠ The name is ambiguous inside a sub-simulation namespace

Sub-simulation namespaces conventionally call their declaration **type** `PhysicsDeclaration` too.
Inside such a namespace the unqualified name resolves to the **struct**, not to this concept — so
`static_assert(PhysicsDeclaration<Decl, GameStaticData>)` becomes a nonsense instantiation of the
struct, and the error, if any, names a template the author never wrote.

⇒ **Write `::PhysicsDeclaration<Decl, GameStaticData>`**, fully qualified, at every assertion site.

⚠ This was a fence in the header until the v2 conversion. It is orientation here rather than a
guard because the mistake is only *typable* in the consumer's own file — see **G-02** in the
guards doc for the full reasoning and for the text as it shipped. In the reference consumer the
same warning stands at the real site, immediately above the six assertions in
`SimulatableBrawler.h` (provenance; not distributed here).

## 3. `PhysicsRuntimeBindings` — one struct, not one per sub-simulation

The runtime handles the engine adapter fills in when it creates a declaration's body:
`ownBodyId`, `parentBodyId`, `attachmentOffset`, `shapeIds`, `queryVolumeIds`. **Local-only, never
serialized, never corrected** — which is why they must not live in the state a correction
overwrites.

This is the **one shared definition**. Before it, every body-owning sub-simulation declared its own
byte-identical `RuntimeBindings` copy. The concept requires
`{ d.bindings } -> std::same_as<PhysicsRuntimeBindings&>`, so a field-identical per-sim copy is a
**distinct type** and does not conform: adopting the shared type is mandatory, not cosmetic. A
sub-simulation that wants to keep its own spelling should make it an **alias**
(`using RuntimeBindings = PhysicsRuntimeBindings;`), not a second struct. The reasoning behind that
requirement, and the compile-time checks that now hold it in place, are **G-04** in the guards doc.

### 3.1 A root body is its own parent

A **root** body ends up with `parentBodyId == ownBodyId`, and it needs no null-id special case.

⚠ **Nothing assigns one to the other.** The identity is not written; it *falls out* of adoption.
A root declaration's descriptor says so (`BodyDescriptor::isRoot`), and the engine adapter then
**adopts** the existing root body rather than creating a new one beneath it — so the id it reports
back as `ownBodyId` is the same value the integration already had as the parent. A reader who
greps a consumer for `parentBodyId = ownBodyId` finds nothing, and that is correct.

⛔ **The consequence for an integration author:** removing `isRoot`, or "simplifying" the adoption
path into an ordinary create, breaks the identity silently — both ids stay populated, both stay
plausible, and nothing asserts.

<!-- Precision fix 2026-09-11 (v2 conversion, R0). The pre-conversion header said a root body
     "is its own parent", which reads as an assignment. There is none. -->

## 4. `staticDataOf` — what makes body creation fully generic

`staticDataOf` maps the **game's** aggregate static data to the **sub-simulation's** own slice,
which is what `queryVolumes` and `attachmentOffset` take. It is the member that lets one fold
serve every declaration.

Without it, a creation fold needs a hand-written per-declaration `if constexpr` arm in the
integration code — one engine edit for every sub-simulation added.

### 4.1 What generic creation does and does not buy — state both halves

⭐ **Updated 2026-09-08.** In the reference consumer that per-declaration `if constexpr` chain has
been **deleted**: every declaration provides `staticDataOf`, and adding a body-owning
sub-simulation there costs **no engine edit to have its body created, bound, captured, rewound and
checksummed**.

⛔ **That is the whole of the claim.** Making that body **collidable or queryable** is still
hand-written integration work. In the reference consumer a new `collisionCategory` needs one entry
in **each** of the two duplicated engine-channel mapping tables, or the physics factory types the
shape by the unmapped fallback and no object query can ask for it — a well-formed query that
matches nothing. Two tasks in that consumer paid exactly that cost, one category each.

⇒ **Generic creation, hand-written collision.** A standalone integration will have its own
equivalent of the second half; what does not change is that `staticDataOf` removes the *first*
cost and not the second.

<!-- Corrected 2026-09-11 (v2 conversion, R0). The pre-conversion header said adding a
     body-owning sub-simulation "costs no engine edit at all". That is contradicted at the
     consumer's own creation fold, by a comment written to correct this exact overstatement. The
     same unqualified sentence still stands in PhysicsBodyState-rationale.md section 6 and is
     routed for correction by whoever owns that document; it is NOT corrected from here. -->

## 5. The requirements, one at a time

| Requirement | Why the generic machinery needs it |
|---|---|
| `descriptor()` | the body and shapes to create |
| `name` | the created object's debug name |
| `staticDataOf(gameStaticData)` | maps the game's aggregate static data to this sub-simulation's own slice — **the sub-`StaticData` this declaration's `queryVolumes` and `attachmentOffset` take** |
| `queryVolumes(subStaticData)` | volumes to register after creation |
| `attachmentOffset(subStaticData)` | the local offset the attachment math re-snaps to |
| `StateType` + the two `bodyStateOf` overloads | where the body state lives — see §5.1 |
| `bindings` | a **mutable member of the shared `PhysicsRuntimeBindings`**, `same_as` and not merely convertible — see §3 and **G-04** |

### 5.1 Both `bodyStateOf` overloads are checked, for different reasons

They are not duplication. The **mutable** one is the capture target, so its referent must model
the body-state pair (`BodyStateLike`, in `PhysicsBodyState.h`). The **const** one is what the
rewind push reads: a generic call site hands it to something taking a `const PhysicsBodyState&`,
so it must **widen** to a full body state — merely existing is not enough. Nothing relates the two
overloads' return types otherwise.

Both halves are now held by compile-time checks rather than by this paragraph — see **G-03** in
the guards doc.

## 6. The self-check block at the bottom of the header

The header ends with a small `physicsDeclarationSelfCheck` namespace: one conforming probe
declaration, four deliberately broken ones, and the assertions that the concept accepts the first
and rejects the rest.

⭐ **It is there because this submodule ships standalone.** The equivalent negative controls exist
in the test suite, but the test suite is a separate module and is **not distributed with
og-simulation**. A consumer who takes only this submodule gets the checks anyway, in every
translation unit that includes the header.

**What each probe is for**

| probe | rejected by | replaces |
|---|---|---|
| `Conforming` | nothing — it must be **accepted** | the vacuity control: without it every negative assertion goes true the moment the concept is renamed |
| `ProbeDerivedBindings` | `same_as<PhysicsRuntimeBindings&>` | G-04 |
| `ProbeConstBindings` | `same_as<PhysicsRuntimeBindings&>` | G-04 |
| `ProbeNonWideningConst` | `convertible_to<PhysicsBodyState>` on the const overload | G-03 |
| `ProbeMutableNotBodyStateLike` | `BodyStateLike` on the mutable overload's referent | G-03 |
| `ProbeLookAlikeBindings` | every candidate spelling alike | **nothing** — it is kept, labelled, as the NON-discriminating control, so it is not mistaken for the check |

⚠ **The probes are declarations, not definitions**, and no object of any of them is ever created.
They cost the consumer six concept instantiations per translation unit and nothing at run time.

⛔ **Do not delete a probe because "the test suite covers it".** The test suite is not distributed
with this submodule; that is the whole reason the block exists.

## 7. History

* The contract was duck-typed before this header existed. A declaration missing a member failed
  inside a template fold, far from the mistake; the concept converts that into one named line at
  the assertion site.
* `PhysicsRuntimeBindings` was four byte-identical per-sub-simulation copies before it was one
  shared struct. Each of those spellings is now an alias for this one.
* `staticDataOf` did not exist when the concept was first written, and the per-declaration
  `if constexpr` chain in the reference consumer's creation fold was a live cost. Both are history
  — with the qualification in §4.1, which the sentence that replaced them originally omitted.
* **2026-09-11** — converted under CommentExtractionRule v2: all prose left the header, one guard
  tag stayed, and two fences became compile-time checks. See §R of the guards doc for what was
  retired and why.
