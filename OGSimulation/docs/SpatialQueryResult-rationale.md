<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SpatialQueryResult.h` — rationale

This is the narrative and the field-by-field orientation for the three engine-independent spatial
query result types — `SpatialQueryHit`, `SpatialQueryReport` and `SweepHit`. Under
CommentExtractionRule v2 the header keeps its licence, its code and one `⛔G-nn` tag per guard, and
nothing else; everything that used to orient a reader lives here.

**If this file and `SpatialQueryResult.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a fence into this file.** Every prohibition lives in
`SpatialQueryResult-guards.md`, reachable from the `⛔G-nn` tag at the declaration it governs.

⛔ **This file is not the source of truth for any VALUE.** The field defaults live in the header.
Where a value appears below it is there to make an argument readable.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-agnostic by
construction. Everything asserted below is true to a reader with no game engine, no workspace and
no other repository. Where something outside this submodule is named, it is named as **provenance
only**, marked as not distributed here, and nothing below depends on the reader being able to open
it.

---

## §1 `SpatialQueryHit` — one hit per overlapping object

Engine-independent query result — one per overlapping object.

### §1.1 Two-level identity — why there are two ids and not one

⚠ **R0 correction, task 80.** The header's own sentence opened *"Two-level identity (see
current_state.md §D10)"*. **There is no `current_state.md` anywhere in this repository**, so the
pointer resolved to nothing for every reader of this tree, and to a *different* initiative's
private working document for anyone who had the workspace open. From a header that ships without
that workspace it was the standalone-truth rule's textbook violation. The provenance is kept,
closed-tense, and the path is gone.

Two-level identity — decided 2026-07-02 (og-brawler-hit-resolution T11), and the reason both
fields exist rather than one:

```
  • bodyId     — the SHAPE body reported by the overlap. Use for shape-level physics
                 access (e.g. physics.getBodyTransform(hit.bodyId) to read a guard
                 sphere's aim-facing rotation).
  • rootBodyId — the root of the body hierarchy the shape body belongs to. Equal to
                 bodyId when the shape body is standalone (projectile bodies,
                 environment). For character shapes (hurtbox, guard), this is the
                 character capsule's body id — the id that identifies the whole
                 character regardless of which shape was actually hit. This is what
                 actor-hit mergers, cross-character hit routing, and projectile
                 hitRootBodyId use. Populated by the query adapter from a shape→root
                 map built at registerShape time; falls back to bodyId when no parent
                 was registered for the shape body.
```

### §1.2 The members, and the one-line labels that used to sit beside them

Under v2 a trailing label is prose and leaves the source like everything else (v2 §1.4 item 5,
user ruling 2026-09-11 — the loss is accepted, not overlooked). What they said:

| member | what it is |
|---|---|
| `objectPosition` | world position of colliding object |
| `bodyId` | SHAPE body reported by the overlap |
| `rootBodyId` | root of the body hierarchy (== bodyId for standalone) |
| `objectCategories` | which collision categories this object belongs to |

---

## §2 `SpatialQueryReport` — the collection

Engine-independent query report — collection of hits.

It is a thin wrapper over `std::vector<SpatialQueryHit>` with `empty()`, `size()`, indexed access
and begin/end, so that a caller can range-for over a report without naming the vector.

---

## §3 `SweepHit` — the nearest blocking hit, or a miss

Engine-independent SWEEP result — the NEAREST BLOCKING hit only (v1), or a miss.
Produced by SpatialQueryAdapter::sweep.

### §3.1 Field validity — the summary, NOT the rule

⛔ **The rule itself is a guard and lives in `SpatialQueryResult-guards.md`:** **G-01** (read the
two flags first), **G-02** (a miss is `!blocked`) and **G-03** (`penetrationDepth` is a push-out).
⚠ **Read G-03 before acting on anything about `fraction` and penetration** — it carries a claim
this repository does not enforce, and backlog task 69 owns the correction. This section exists to
orient, and it defers to those three entries wherever they disagree with it.

In outline: `blocked` and `startPenetrating` are the two flags, and six of the remaining seven
fields are conditional on them. `bodyId`, `rootBodyId` and `objectCategories` are the exception —
they are meaningful whenever `blocked`.

### §3.2 Two-level identity, exactly as `SpatialQueryHit` (§1.1)

⚠ **Positional word re-resolved, task 80.** The header's sentence read *"exactly as
`SpatialQueryHit` **above**"*. Once both blocks leave the file, *"above"* points at nothing, so the
reference names its target: **§1.1 of this document**.

```
  • bodyId     — the SHAPE body the sweep hit.
  • rootBodyId — the root of that shape body's hierarchy; equal to bodyId when the
                 shape body is standalone. This is the id that identifies a whole
                 character regardless of which of its shapes the sweep struck.
```

### §3.3 The members, and the one-line labels that used to sit beside them

| member | what it is |
|---|---|
| `blocked` | false => moved the full delta unobstructed |
| `fraction` | [0,1] of delta travelled before contact |
| `normal` | surface normal at contact (world; unit as reported by the query backend) &nbsp;⚠ |
| `impactPoint` | world |
| `startPenetrating` | already overlapping at fraction 0 |
| `penetrationDepth` | valid iff startPenetrating; push-out along `normal` |
| `bodyId` | SHAPE body hit |
| `rootBodyId` | root of the body hierarchy (== bodyId for standalone) |
| `objectCategories` | which collision categories the hit object belongs to |

⚠ `penetrationDepth`'s label is the short form of **G-03**; read the guard, not the label.

⚠ **R0 correction, task 80 — `normal`.** The header's label read *"(unit, world)"*, stating
unit-ness as a property of this type. Nothing in this repository normalises the vector or asserts
its length; it is passed through from the query backend, so unit-ness is **inherited**, not
guaranteed here. The label above says so. ⚠ This is the same class of defect as the `fraction`
claim in **G-03** and is deliberately **not** bundled with it: this one is a wording nuance, that
one is a contract defect with an owner, and merging them would bury the one that matters.

---

## §4 What is NOT in this file

* **The prohibitions.** `SpatialQueryResult-guards.md`, three entries, one tag each in the header.
* **How the fields are populated.** That is the query adapter's business, and the adapter is
  engine-specific and lives outside this submodule by design. This header is the contract; the
  adapter is one implementation of it.

Origin: the `og-brawler-hit-resolution` initiative (T11, 2026-07-02 — the two-level identity), and
the `brawler-movement-simulation` initiative's Phase C comment extraction (tasks 65, 69 and 80,
2026-09-11).

> ⚠ **Both initiatives named in that Origin line are private working material and are NOT
> distributed with this submodule.** They are named as provenance, deliberately unlinked. Every
> claim this file *asserts* is anchored to `SpatialQueryResult.h` in this repository and to nothing
> else.

<!-- lint-external-ref: current_state.md -- named ONLY to record the dead pointer this task removed; private initiative working material, not distributed with this submodule -->
<!-- lint-external-ref: og-brawler-hit-resolution -- initiative workspace; private working material, not distributed with this submodule -->
<!-- lint-external-ref: brawler-movement-simulation -- initiative workspace; private working material, not distributed with this submodule -->
