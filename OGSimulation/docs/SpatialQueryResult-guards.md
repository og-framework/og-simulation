<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SpatialQueryResult.h` — guards

Every fence that stood in the header. Each entry has an **opaque, stable id**; in the header a
single line `// ⛔G-nn` sits exactly where the fence's text used to sit, on the same
declaration.

**If this file and `SpatialQueryResult.h` disagree, the header is authoritative and this file is
stale.** Fix this file; do not soften the header to match it.

⛔ **An id is never reused.** A guard that is deleted, or that becomes a compile-time check, is
moved to §R and its number is spent forever. Reusing one silently re-points every reference that
ever named it. A retired id may be **named** in prose or in a `static_assert` message; it may
never again appear as a `⛔G-nn` **tag**.

⭐ **The join is machine-checked, in both directions**, by `tools/lint/guard_tag_lint.ps1`: every
tag resolves to an entry here, every live entry is referenced by exactly one tag, no id is
duplicated, and no retired id reappears as a tag. It is a hard gate. ⚠ It checks that an entry
EXISTS. It never checks that this text is TRUE.

⛔ **Nothing in this file is a rationale.** The narrative, the provenance and the field-by-field
orientation live in `SpatialQueryResult-rationale.md`. A guard is a prohibition plus the
consequence of ignoring it, and nothing else.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0.** Every guard below
is stated so that it is true and actionable to a reader with no game engine, no other repository
and no other file open. Where a consumer outside this submodule is named it is named as
provenance and marked as not distributed here.

---

## G-01 — Read the two flags FIRST; six of the nine fields are conditional on them

**Tag site:** `SpatialQueryResult.h`, immediately above `struct SweepHit`.
**Taxonomy clause:** F5a + F6 (must-never-move **T2e-1**).

**The fence, verbatim — these are the bytes it occupied in the header (line 49 of the
pre-conversion file):**

```
// Field validity — read the two flags FIRST; the rest are conditional on them:
```

**What breaks if it moves.** `SweepHit` is a **flat aggregate of nine public fields with plausible
defaults** — `fraction = 1.f`, `normal{0.f}`, `penetrationDepth = 0.f`. Every field reads as
unconditionally valid and the identifiers actively argue that they are. This is the only thing at
the declaration saying six of the nine are gated on two `bool`s. Moved away, a consumer reads
`impactPoint` off a miss and gets `(0,0,0)` — a plausible world position, silently wrong, with no
crash and no test failure.

⭐ **This guard has a measured in-tree audience: three sites outside this submodule cite it by
name** as *"`SpatialQueryResult.h`'s field-validity rule"* and reason from it rather than
restating it. It is not inert.

---

## G-02 — A miss is `!blocked`. It is never `fraction < 1`.

**Tag site:** `SpatialQueryResult.h`, immediately above `float fraction = 1.f;`.
**Taxonomy clause:** F5a + F4 (must-never-move **T2e-3**).

**The fence, verbatim — these are the bytes it occupied in the header (lines 50-52 of the
pre-conversion file; the pinned line is the middle one):**

```
//   • fraction / normal / impactPoint are valid IFF `blocked`. When `blocked` is
//     false the volume travelled the full delta unobstructed, `fraction` is 1, and
//     normal/impactPoint carry nothing.
```

**What breaks if it moves.** It states what a **miss** looks like — a state with no code of its
own: the miss path is one defaulted return and names nothing. A reader who checks `fraction < 1.f`
instead of `!blocked` has written a predicate that is right today and wrong the moment the backend
reports a hit time slightly under 1 on a grazing miss. The absent branch is the whole point, and
the field's own default (`1.f`) is what makes the wrong predicate look reasonable.

---

## G-03 — `penetrationDepth` with `normal` is a PUSH-OUT, not a distance along the sweep

**Tag site:** `SpatialQueryResult.h`, immediately above `float penetrationDepth = 0.f;`.
**Taxonomy clause:** F5a + F6 (must-never-move **T2e-2**).

**The fence, verbatim — these are the bytes it occupied in the header (lines 53-56 of the
pre-conversion file; the pinned line is the first):**

```
//   • penetrationDepth together with `normal` is the PUSH-OUT IFF `startPenetrating`
//     — the volume was already overlapping at fraction 0, and displacing it
//     `penetrationDepth` along `normal` separates it. `fraction` is 0 in that case
//     and carries no distance information.
```

**What breaks if it moves.** `penetrationDepth` and `fraction` both look like distances and are
not interchangeable; `normal` changes meaning between the two modes — contact normal in one,
separation axis in the other. The reader deriving push-out arithmetic is looking at the fields, not
at a document.

⛔⛔ **THE SECOND SENTENCE OF THIS FENCE IS A KNOWN-UNENFORCED CLAIM AND IT IS MOVED HERE
VERBATIM, UNCORRECTED, ON PURPOSE.** *"`fraction` is 0 in that case"* is stated as a **type
invariant**, and no code in this repository clamps, asserts or tests it — the field is passed
through from the query backend's own hit time. ⚠ **That is a RUNTIME finding, not a stale
sentence.** Correcting it means first establishing **by measurement** whether the backend really
guarantees a zero hit time on an initial overlap, and then either pinning that fact in the adapter
or restating the sentence as the inherited premise it is. ⛔ **A documentation change cannot do
either**, which is why the claim still stands here unaltered. It is tracked as its own work item
outside this submodule (see the provenance note at the end of this file).
⛔ **Do not reword this entry to make it read as true.** Softening it into vagueness is the exact
failure mode task 69's acceptance criteria name. It is quoted here as it shipped so that task 69
has something to correct and so that the defect is not laundered by the move.
⚠ Until task 69 lands, the safe reading is: **branch on `startPenetrating`, never on
`fraction == 0`.**

---

## §R Retired ids

**None.** No fence in this header became a compile-time check.

⭐ **That verdict is backed by a compile, not by reasoning.** A default-constructed `SweepHit` IS
usable as a `constexpr` object in this tree, and every one of its defaults is readable in a
constant expression — the two
`glm::vec3` members and glm's `operator==` included. A `static_assert` pinning those defaults
compiles green, and the same assertion with one wrong value fails with `C2338`. So the compile-time
check was **available**, and it was declined on **narrowness**: all three fences forbid a *reading*
discipline exercised by a consumer, and an assertion over the struct's default values would have
pinned one true fact while licensing every wrong read it does not cover. ⛔ **An assertion that
looks like enforcement and is not is worse than the sentence it replaced.**

---

## Provenance

These three guards were extracted from the header on 2026-09-11, and the unenforced claim quoted in
**G-03** is tracked as a separate work item. ⚠ **That tracking lives in private initiative working
material and is NOT distributed with this submodule.** It is mentioned so that a reader of G-03
knows the defect is recorded rather than ignored; nothing in this file depends on being able to
open it, and every claim above is anchored to `SpatialQueryResult.h` in this repository and to
nothing else.
