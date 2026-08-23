<!-- SPDX-License-Identifier: MPL-2.0 -->
# History: how the resim gate got its shape

The resim gate decides one thing: **after an authoritative correction lands, does the client roll
back and replay?** Between 2026-08-06 and 2026-08-12 that decision was measured, found to be made by
an accident of clock alignment, instrumented, pinned in tests, rewritten, shipped, and then defended
against a second defect the rewrite exposed. This document is the record of that sequence — what was
decided, when, on what evidence, and what was ruled out.

It is a **development-history** document. It is not a reference: it does not tell you what the code
does today. For that, read `ResimGatePolicy.h` and the doc named in §13.

---

## 0. The two rules this document is written under

Both exist because this codebase has measured its documentation rotting, repeatedly, and both are
falsifiable rather than aspirational.

**Rule 1 — every statement is closed-tense and dated.** A sentence like *"resims fire on every
landing"* decays the moment the tree moves. A sentence like *"on 2026-08-12, item 43 measured resims
firing on 86–87% of physics frames"* is true forever, because it describes a measurement rather than
a tree. Where this document must describe present code state, the sentence carries a
`file:symbol` anchor a reader can grep, and the anchor — not the sentence — is the thing that is
checkable.

**Rule 2 — ⛔ EVERY SYMBOL NAMED HERE EITHER RESOLVES OR IS DECLARED DEAD.** Rule 1 alone is not
enough, and this is the specific lesson worth carrying: a past-tense history line in
`SimulationReconciliation.h` once named a method that no longer existed under that name *or* on that
class, and it was filed as a defect anyway — closed tense had protected the *claim* and done nothing
for the *symbols the claim named*. A history document is mostly dead symbols, so §11 is a table of
every retired name this file mentions, what it became, and which change retired it. **A name in the
prose below that does not appear in §11 is asserted live, and §12 is the list of those.** Both
tables are machine-checked in the same direction: a live name must have a **non-comment** line in the
file named; a retired name must have **no non-comment line anywhere in the tree**.

The in-tree precedent for Rule 2 is worth reading, because it is the same idea applied to test-case
names rather than symbols: `ResimGateSemanticsTest.cpp` opens with a ledger of which of the original
eight cases were kept, renamed, rewritten or replaced, and why.

> **Provenance, recorded rather than linked.** The narrative below is drawn from the
> `og-netcode-v2-input-relay` initiative archive — a backlog of 96 items with their rulings, the
> design and finding documents beneath them, and the archived PIE run logs. ⚠ **That archive is
> private working material and is NOT distributed with this submodule.** Every `item N` label in
> this document is therefore a **label, not a reference**: it identifies a decision in that archive
> and resolves only there. Nothing in this file asks a reader to open a document that does not ship.
> Everything this document *asserts about code* is anchored to a file in this repository, and only
> to those. Where the archive and the tree disagreed while this was written, the tree won — §11 row 6
> is an instance.

> ⚠ **The machine half of Rule 2.** §11 and §12 are the human halves — a reader can see which
> names are dead and which are live. The declarations below are the same statement addressed to
> `tools/lint/doc_anchor_lint.ps1`: they tell it that these fourteen tokens are **deliberately**
> unresolvable, so it enumerates each one with its reason instead of reporting it as drift. Each
> declaration is document-scoped and covers every occurrence of that token, which is why fourteen
> of them account for all nineteen sites. The lint treats a declaration that suppresses nothing as
> a violation in its own right, so this block cannot rot into a silent blanket exemption: delete
> the last mention of a retired name and the lint fails on the orphaned declaration.

<!-- lint-external-ref: FRewindData::FindValidResimFrame -- ENGINE SYMBOL, not a name in this repository: the rewind-frame search of ONE adapter's physics engine, declared and defined in that engine's own source build; another adapter substitutes its own search and this token means nothing there. §5.1 gives both paths and is its prose declaration -->
<!-- lint-external-ref: Engine/Source/Runtime/Experimental/Chaos/Public/RewindData.h -- ENGINE PATH (§5.1): a source file of ONE adapter's physics engine, outside every scan root of this repository, quoted so a reader who has that engine can re-check the directional argument at source -->
<!-- lint-external-ref: Engine/Source/Runtime/Experimental/Chaos/Private/RewindData.cpp -- ENGINE PATH (§5.1): the definition side of the same engine function; same reason as the header above -->
<!-- lint-external-ref: ClampedGrantsSeeTheSilentFindValidResimFrameMove -- RETIRED NAME (§11 row 7): a test case renamed to ClampedGrantsCountAnyRequestedVsGrantedMismatch because the old name asserted engine behaviour that had been refuted; it must not resolve -->
<!-- lint-external-ref: resimCooldownTicks -- NEVER SHIPPED (§11 row 3): a rate ceiling built during item 45 and removed on the 2026-08-11 ruling §7.1 records; it has no successor by design and must not resolve -->
<!-- lint-external-ref: runningAnchor -- THE NAIVE FORM the fence at ResimGatePolicy.h:230 forbids, quoted in §7.1 so the forbidden shape is legible; it is deliberately not a name in this tree and must not resolve -->
<!-- lint-external-ref: m_isResimulated -- RETIRED NAME (§11 row 1): the per-slot provenance bitset; its gate role became m_pendingResimAnchorTick and its diagnostic role returned as SlotStateProvenance, so the bitset itself must not resolve -->
<!-- lint-external-ref: getLastResimulationTick -- RETIRED NAME (§11 row 2): deleted at item 45 and replaced by needsResimulation reading the pending anchor; it must not resolve -->
<!-- lint-external-ref: pushPredictionInput -- RETIRED NAME (§11 row 4): deleted with the correction cache's input column; a slot now holds state plus an applied-capture-tick reference and no input value, so it must not resolve -->
<!-- lint-external-ref: getDiagnosticStateProvenance -- RETIRED NAME (§11 row 5): landed as Diagnostics::stateProvenance reached through getDiagnostics(), the view type carrying the marker instead of the name; it must not resolve -->
<!-- lint-external-ref: prepareSimulationStep -- RETIRED NAME (§11 row 6): a rename that was reverted — item 90 gave collectInputAll this name, item 94 gave it back; the intermediate name must not resolve -->
<!-- lint-external-ref: InheritanceCarriesTheResimFlagForward -- RETIRED NAME (§11 row 10): a test case replaced rather than renamed, because the guard it pinned was itself retired; it must not resolve -->
<!-- lint-external-ref: freshClobbers -- NEVER SHIPPED under that bare name (§11 row 11): only freshClobbersAvoided and staleClobbersAvoided were built, so the bare half of the archived pair must not resolve -->
<!-- lint-external-ref: dumpSlotProvenanceAll -- RETIRED NAME (§11 row 12): landed as the const logSlotProvenanceAll on the diagnostics view; it must not resolve -->

### ⚠ Adapter bindings named in this document — one adapter's, never the binding

`og-simulation` is engine-free: it never names a game-engine type, and it is reached from a host
engine only through `concept`s. Three names below therefore belong to **one adapter's** host stack
rather than to this core, and are named because a claim in this history was settled by reading them:

| name as it appears here | the ROLE it plays for the core | whose it is |
|---|---|---|
| `FRewindData::FindValidResimFrame` | the physics rewind search — given a requested frame, returns the frame the engine will actually rewind to | one adapter's physics engine (Unreal's Chaos); another adapter supplies its own search |
| the two engine paths declared immediately above | that search's declaration and definition, quoted in full in §5.1 so the directional argument there can be re-checked at source | the same engine's source build, outside every scan root here |
| `Config/DefaultEngine.ini` | the host application's configuration surface, where the shipped value of `ResimTriggerPolicy` is set | one adapter's host project; another supplies the same value from its own configuration under its own key |

⛔ **None of the three is a dependency of this core.** The core reads its policy through `TimeConfig`
and asks for a rewind through its own adapter callback; an engine that supplies neither of the first
two can still host it. Where the sections below use one of these names bare, it is this declaration
that scopes it.

---

## 1. The root: a field-locator was used as a detector

The design specified **two layers** of desync detection:

| layer | mechanism | question it answers |
|---|---|---|
| **Layer 1** | a per-tick CRC-32 over serialized state | *did we diverge, and when?* |
| **Layer 2** | `isSimilarTo`, a per-field comparison | *which field diverged?* |

⛔ **The resim gate was built on Layer 2.** A tool for locating a difference was used as the tool for
detecting one. Recorded on 2026-08-16 in the archive's Layer-1 section as the root of items 28 and
30, and it explains why both of those items exist at all: once a field-locator is the detector, the
detector inherits every tolerance question the locator has, and there is no independent signal to
check it against.

Layer 1's *local* half was already present when that was written — `compute_checksum` on
`StateCorrectionCache` computes a CRC-32 over serialized bytes through `ChecksumByteBuffer`, built
for the determinism harness — and `DesyncDiagnosticSink.h` shipped the reporting boundary with, in
its own words, no production consumer. Wiring either layer into the gate was deferred; see §10.

---

## 2. Timeline

Every row is a dated, closed observation. Dates are ruling or completion dates from the archive.

| date | item | what happened |
|---|---|---|
| 2026-08-06 | 24 | The per-correction divergence verdict was found to already exist and to be invisible by **routing**, not absent. Exposed on its own log tag. |
| 2026-08-06 | 28 | Divergence **magnitude** re-prioritised, on the finding that the boolean verdict read `correct=0` on 24,459 of 24,459 samples. |
| 2026-08-06 | 30 | Filed: one similarity epsilon serves every scale in the system. |
| 2026-08-07 | 31 | Filed on a user challenge: 4,552 always-wrong verdicts produced 59 resims. The architect's own explanation was retracted in writing. |
| 2026-08-11 | 42 | Six counters shipped across two probe objects. Item 31's mechanism named. A refuted engine claim was caught **inside item 42's own new code**. |
| 2026-08-11 | 44 | The shipped gate pinned in 8 LLT cases, two of which deliberately pinned the **defect**. |
| 2026-08-11 | 45 (ruling) | ⛔ The cooldown knob was **built and then removed** by user ruling. |
| 2026-08-12 | 43 | The diagnosis pack ran. Arm A confirmed item 31 live; arm B priced the fix. |
| 2026-08-12 | 45 | The edge-triggered anchor shipped, dormant behind the legacy policy. Two gate members retired. |
| 2026-08-12 | 46 (partial) | ⭐ The user flipped the policy via ini **on observed behaviour, not on the counters**. |
| 2026-08-12 | 47 | The hollow-anchor defect the flip exposed: protect-all-corrected shipped. |
| 2026-08-12 | 48 | The retired provenance diagnostic returned as a fenced enum column. |
| 2026-08-12 | 30 | **DEFERRED** by user ruling; its role changed from blocker to cost reduction. |
| 2026-08-16 | 65 | The comment-extraction pilot ran on `ResimGatePolicy.h`. |
| 2026-08-16 | 73, 74, rewire | **DEFERRED** by user ruling — all three, separately. |
| 2026-08-16 | 76 | The gate's own documentation was corrected after it had gone false. |

---

## 3. Act I — the verdict was already degenerate before anyone examined the gate

On 2026-08-06 item 24 set out to build a correction-magnitude instrument and discovered most of it
already computed. The verdict `isSimilarTo` produced on every correction, for every character, was
being stored and then dropped on the floor by log routing: its line was untagged, so it fell to a
category suppressed at the shipped verbosity, and it appeared **zero** times in a clean run. Two of
the three divergence signals were invisible by routing rather than absent. Exposing them cost a
log-line change.

What that exposure showed, the same day, closed the question of whether the verdict could gate
anything: `correct=0` on **24,459 of 24,459** samples, cross-checked against a second run's
**136,819** occurrences with **zero** `correct=1` anywhere on disk. Split by class:

```
25696  class=LocallyPredicted  correct=0
22158  class=RemoteProxy       correct=0
```

⭐ **The locally-predicted character being always wrong is the part that made item 28 urgent rather
than tidy.** For that class the design says the client applies the same input on the same tick number
as the authority, the simulation is deterministic at a fixed timestep, and serialization is a byte
copy — so the two states should be bit-identical, and bit-identical passes any epsilon. The
mis-scaled tolerance alone could not explain it. Three readings were possible and the discriminator
between them was a magnitude the boolean fold discarded.

Item 30 named the tolerance half on the same day: `kDefaultSimilarityEpsilon` at
`SimulationTypes.h:25` is a single `inline constexpr float` serving every float quantity in the
system, reached through `isSimilarToField`. For a normalised direction or a quaternion dot product
`1e-4` is sensible; for a world position in an adapter whose length unit is the centimetre — which
is what this project's shipping adapter uses — it is one micrometre. Being
`inline constexpr` it was neither in `TimeConfig` nor runtime-tunable, which is how a mis-scaled
value sat undetected.

⚠ **Item 28 never landed.** As of 2026-08-21 no magnitude instrument exists, and the gap is stated at
the site — `ResimGateProbe.h` records that whether the suppressed corrections were micrometres or
metres is *"deliberately NOT answerable from here"*, and names item 28 as the owner.

---

## 4. Act II — the numbers that did not reconcile

Item 31 was filed on 2026-08-07 from a user challenge — *have you ever seen evidence that
`isSimilarTo` is always false, and that this never triggered actual resimulations?* — and its
opening question was arithmetic:

> **4,552 corrections, every one judged "prediction was wrong", produced 59 resims. Which condition
> is actually gating resimulation, and is it the one we think it is?**

The proposed reconciliation was that one resim trigger coalesces a whole rollback window, so the two
numbers answer different questions. That distinction was correct and load-bearing, **and the
arithmetic did not land.** With a rollback window of 12 ticks and a hard cap of 20, the measured
verdicts-per-trigger ratios were 212, 274, 77 and 129 across two independent sessions and four
clients — four to twenty times larger than the window can account for.

Two things about how that item was written are worth preserving, because both are process rather than
mechanism:

- **The architect's own earlier explanation was retracted in the item text**, in writing, rather than
  quietly dropped. The explanation ("triggering is driven by correction arrival, not by the verdict")
  was true and did not explain the rate: on that reading the gate should have been open on almost
  every tick, predicting thousands of resims rather than 59. The item recorded the mechanism as
  **UNKNOWN**.
- **The logs could not settle it, and the item said which half was missing.** The declining branch
  routed to a category suppressed at shipped verbosity, so the numerator was visible and the
  denominator was not. Exposing it was made step 1.

---

## 5. Act III — instrument first, and the claim that was refuted from engine source

Item 42 shipped on 2026-08-11: six counters across two probe objects, one per thread —
`ResimGateProbe` fed on the physics thread and `CorrectionLandingProbe` fed on the game thread, each
flushed on its own feeding thread, no sharing and no atomics. They went out on a new log category at
shipped verbosity, five summary lines per window against a budget of six.

The item's own framing of the two traps it existed not to repeat is the reusable part:

1. **Verbosity had already cost three instruments plus item 31's denominator.** Every per-window line
   was placed at Warning on its own category, silenceable independently, with the full four-step
   routing path wired and a one-shot startup proof line.
2. **A metric that cannot fail is not a metric.** Each counter named its physical quantity and the
   constructed success/failure pair that must move it, and both constructions were run before the
   item closed.

Item 42 also stated item 31's answer in one line: the prediction-frontier slot inherited a
per-slot resimulated bit forward on every tick, and the scan that decided whether a resim was pending
started *at* the frontier — so the inherited bit shadowed every older corrected slot, and the gate
re-opened **only when a correction landed exactly on the frontier**. Corrections landing behind the
frontier triggered nothing.

### ⭐ 5.1 The finding that item 42 refuted — including inside its own new code

The finding item 42 was built from asserted that the physics adapter's rewind search could return a
frame *later* than requested — a silent shallow-clamp. The reviewer read that adapter's engine source
and established that **the search walks downward from the requested frame**, so it can return an
earlier frame or none at all but never a later one: the shallow clamp is directionally impossible on
that wiring. The concrete search is one adapter's binding — `FRewindData::FindValidResimFrame`,
named in full below so a reader who has that engine can re-check the claim at source.

⚠ **`FRewindData::FindValidResimFrame` is an ENGINE symbol and is NOT in this repository** — it is
one adapter's binding for the rewind search, and it lives in that adapter's engine source build:
declared at `Engine/Source/Runtime/Experimental/Chaos/Public/RewindData.h`
and defined at `Engine/Source/Runtime/Experimental/Chaos/Private/RewindData.cpp`, whose paths are
given in full precisely because no anchor check run inside this repository can resolve them. An
adapter built on any other engine supplies its own rewind search, and the directional claim above
has to be re-established against that search rather than inherited from this one. The
function is named rather than elided because the whole point of §5.1 is that the claim was settled by
reading it, and a reader who cannot find it cannot re-check the claim. Re-verified against that
source on 2026-08-21: the walk reads
`for (int32 CheckFrame = RequestedFrame; CheckFrame > EarliestFrame; CheckFrame--)`, so the
directional argument still holds. This is the only symbol in this document that resolves outside this
repository, and this paragraph is its declaration as such — the same treatment §11 gives a retired
name.

⛔ **By the time that was established, the refuted claim had already shipped as fact in four homes of
item 42's own new code**, including a test whose *name* asserted the engine behaviour
(`ClampedGrantsSeeTheSilentFindValidResimFrameMove`). All four were recast to state what a mismatch
would actually mean — that the grant was **deepened** — and the test was renamed to describe the
detector rather than the engine (§11 row 7). The counter itself was kept: its zero is a
source-proven structural constant and the only tripwire that would notice an engine-side requester
deepening our rewinds, which is a different thing from an unexercised counter.

**The lesson recorded at the time:** this is the propagated-claim shape, and it appeared in the
artifact whose entire deliverable was interpretability. A claim inherited from a finding document is
not evidence; it is a citation, and citations rot.

---

## 6. Act IV — pin the defect before fixing it

Item 44 landed on 2026-08-11 and is the reason the rewrite was safe to attempt. The hole it closed
was proven from the tree rather than argued: a behaviour-changing experimental block sat in the
working tree inside the correction-insert path, and **both suites passed byte-exact with it in.**
No test drove the gate's semantics at all.

Eight cases were written against the committed semantics, adding 228 assertions. Two of them
deliberately pinned the **defect** rather than a specification, and were named so no one could
mistake which was which — the suffix `_DocumentsADefect_Item45WillChangeThis` (§11 row 8). One case
pinned the frontier-bit inheritance as **correct at the time, and not as a defect**, because it was a
load-bearing guard: seeding it false, or starting the scan one slot later, produced an unterminating
one-tick resim storm. Two of the fix's candidate designs were dead for that reason and the item said
so, to stop them being resurrected.

⭐ **The suite's ability to fail was demonstrated, not claimed.** With the experimental block
temporarily restored the suite went 6 passed / 2 failed — and **the failing set and both failing
assertions were predicted in writing before the probe ran, and matched exactly.** The file was then
restored byte-exact against a recorded checksum, rebuilt, and re-verified green.

---

## 7. Act V — the edge-triggered rewrite, and the cooldown that was built and then removed

Item 45 shipped on 2026-08-12. The gate stopped being level-triggered on derived slot state and
became **edge-triggered on one word**:

- `StateCorrectionCache` gained a single `std::atomic<uint32> m_pendingResimAnchorTick`, zero meaning
  none. ⚠ **The atomic is load-bearing, not defensive**: the word is two-writer — the game thread
  sets it on the landing path, the physics thread consumes and wipes it — so a plain word carries a
  real lost-update window in which the consume stomps a newer anchor. That is the original defect in
  miniature.
- The landing path raises it through a **CAS-max**, so coalescing to the newest corrected tick is
  deliberate: authority state at the newest corrected tick subsumes older corrections on the same
  trajectory, which keeps replay depth equal to transit latency while consuming the whole backlog.
- Consumption is a **literal compare-and-swap** against the anchor captured at prepare time. A newer
  anchor written mid-replay makes the CAS *fail* and survive — the intended re-trigger, made
  impossible to violate rather than improbable by timing, and unit-testable single-threaded.
- Two members were **retired** — the per-slot resimulated bitset and the scan that read it (§11 rows
  1 and 2). The frontier-inheritance line was deleted *because its only reader was gone*, and the
  item required the commit message to say so in those words.
- Policy became configurable through one `TimeConfig` field on the established four-step config path:
  `FrontierExact` reproduces the legacy gate, `OnDisagreement` anchors on any landed correction the
  verdict calls wrong.
- The depth policy **skips and counts** rather than clamping: restoring at an uncorrected mid-window
  slot would replay the same prediction, a no-op costing a full engine rewind. This gave
  `rollbackWindowTicks` its first real client-side consumer, and the config comment that had recorded
  the clamp as intended-but-not-implemented was rewritten in the same change.

### ⛔ 7.1 The no-cooldown ruling, 2026-08-11

`resimCooldownTicks` was **designed, implemented, and then removed on a user ruling** during item 45.
It is not a knob that was considered and skipped; it existed and was taken back out. The reasoning
was recorded at six code sites at the time, and the fence that survives at `ResimGatePolicy.h:52`
states it verbatim:

> ⛔ THERE IS DELIBERATELY NO TRIGGER COOLDOWN. DO NOT ADD ONE.
> (User ruling, 2026-08-11. Design §4 and backlog items 45/46 both name a `resimCooldownTicks` rate
> ceiling; it was built, then removed on this ruling.) A rate ceiling defers a correction KNOWN to
> disagree with prediction — the exact defect this item repairs, at a smaller constant.

The throttle is **structural** rather than configured: a landing never restarts a running resim, a
correction arriving mid-replay only moves the *next* resim's anchor through the CAS-max, and
serialization caps triggers at one per non-resim physics frame. Landings in between coalesce into one
deeper replay. The storm regime was excluded by process — a pre-flip demand preview, a probe alarm,
and a one-line rollback to the legacy policy — rather than by a timer.

⚠ **The re-add condition was named rather than forbidden:** only a measurement showing legitimate
demand that is still unaffordable puts a rate breaker back on the table. As of 2026-08-21 no document
in this repository names the knob as live; §11 row 3 records the name so a future reader who finds it
in an archived design can tell what happened to it.

---

## 8. Act VI — the measurement, and a ruling made on what the user could see

Item 43 ran the diagnosis pack on 2026-08-12 as two arms in one session, one ini line apart on the
same binary.

**Arm A — the legacy policy — confirmed item 31 live.** Roughly **9,100–9,400** corrections landed
behind the frontier and were ignored, against **1** and **0** resims across the two clients, matching
the frontier-exact landing count exactly. The gate was firing on a clock-alignment artifact.

**Arm B — the disagreement policy — priced the fix**, and was explicitly labelled a cost preview
rather than the flip itself:

| quantity | measured, 2026-08-12 |
|---|---|
| frames on which a resim ran | **86–87%** |
| mean replay depth | **6.43** / **3.74** ticks (cross-checked two independent ways) |
| integration work vs baseline | **6.35×** / **4.18×** — inside the design's modelled 3–6× band |
| sustained sim rate | **60.50** / **59.12** Hz |
| absorbed by the depth ceiling | **4.5%** / **2.8%** — so that cost was unbraked |
| verdict disagreement rate | **1000‰** across **388** windows, **23,280** corrections, zero agreements |

### ⭐ 8.1 The flip was ruled on observed behaviour, not on the counters

On the same day the user set the policy to `OnDisagreement` in the ini and left it there. The reason
recorded in the archive is not in any of the numbers above:

> Under the legacy policy a wrong prediction was never corrected at all — remote characters spawning
> a special through the hit sequencer sometimes ran a plain strike instead, and it persisted. Under
> the disagreement policy the strike showed for a few frames and then converged.

⛔ **No counter in that run could have shown it.** With the verdict pinned at 1000‰, every
prediction-quality statistic was constant by construction; the acceptance criterion asking for the
disagreement rate to fall was **unmeasurable, not failed.** This is the clearest instance in the
whole sequence of a decision that the instrumentation was structurally unable to make.

Three consequences were recorded rather than waved through, and none of them were discharged by the
ruling: the storm criteria genuinely failed; the six-character target was unmeasured, because cost is
frames × depth × characters, and three characters passing is not evidence for six; and the
compiled default stayed on the legacy policy, so the revert remained deletion of one ini line with no
rebuild.

⚠ **The ini line's own provenance is easy to misread.** It was set for item 43's arm B, left in the
tree, and *then* blessed by the item 46 ruling the same day — item 43's own status text asks for it
to be removed before any run meant to measure shipped behaviour. `TimeConfig.h` states the resulting
split explicitly and tells the reader to check the ini rather than the comment for the value.

---

## 9. Act VII — the hollow anchor, and a retired diagnostic brought back behind fences

### 9.1 Item 47 — a replay must not overwrite a fresh authority correction

The rewrite exposed a defect that had been harmless before it. A correction wrote authority state
into a slot; an in-flight replay then reached that tick and `tryInsertingResimulatedState` overwrote
it **unconditionally**, while `m_containsCorrectTick` stayed set — so the slot claimed authority
provenance over a re-derivation. The surviving anchor correctly triggered a follow-up resim, which
restored the clobbered state and faithfully reproduced the same prediction. **The trigger was real
and the data it pointed at was gone.** The clobber pre-dated item 45; what item 45 changed is that
something was finally waiting on that data.

What shipped is the simplest rule that satisfies *authority beats a re-derivation of it* everywhere:
**protect-all-corrected, one bit wide.** A replay never overwrites a slot with `m_containsCorrectTick`
set — not "never overwrites a fresh one", never overwrites any of them. Under that shape no
provenance bit is ever cleared, so *bit set implies authority-grade state* holds by construction
rather than by discipline. The fence stating this sits at `ResimGatePolicy.h:186`.

The two-clause freshness discriminator that the design had made the *decision* rule was demoted to
**classification only**, feeding two counters. ⛔ The naive single-clause form
(`containsCorrectTick && tick > runningAnchor`) is used nowhere, and the fence at
`ResimGatePolicy.h:230` says why in place: under a single min-folded anchor it misclassifies the
multi-character population wholesale, and the anchor compared against is the **per-cache** capture
taken at `prepareResimAll`, never the folded min.

⭐ **The reachability analysis produced a free wiring check.** Stale-in-span is structurally
impossible for a single character — the replay span and the stale condition are disjoint ranges — and
is reachable only through the multi-character min-fold. Therefore `staleClobbersAvoided` **must** read
zero in any single-character session, which turns a correctness argument into an assertion anyone can
run. That statement is at the counter, at `ResimGatePolicy.h:243`.

Evidence: ten cases each with a failing twin, and **three mutation runs — the protection removed, then
each clause removed in turn — whose failure sets were predicted before running and matched exactly**,
which is what proves neither clause alone suffices. A fidelity defect in the earlier items' test
helpers surfaced in the same pass: they replayed the anchor slot, while production replays
`anchor+1..frontier`. Harmless under item 45; material here.

### 9.2 Item 48 — the provenance column, and the fence that had to be poisoned exhaustively

Item 45 had retired the per-slot resimulated bit citing the rule that a stored value nothing reads
becomes a stale-truth trap. Item 48 deliberately reopened that, on a user request to be able to ask
each cache slot where its state came from, with the hazards designed out:

1. **It is not the old bit and does not take the old name.** A `bool` answers *did a replay write
   this?*; the diagnostic question is *what is the lineage of this slot's state?* So it shipped as a
   six-value enum, `SlotStateProvenance`, about 60 bytes per character. Two of the six values are the
   payload precisely because the old bit could not represent them:
   `AuthorityAgreedKeptPrediction` (the skip-the-copy branch — authority-grade state that holds the
   prediction) and `ReplayedOverCorrection` (item 47's provenance lie, previously unexpressible,
   which is exactly why it stayed invisible).
2. **No production reader, machine-checked.** The independence case garbage-fills the column at three
   lifecycle points and asserts that the entire production surface — gate, anchor, both prepare
   captures, every per-slot bit and stamp, the verdict, every replay outcome, and the determinism
   checksum — is byte-identical.
3. **Readers on day one**, so the column is not a stored value nothing consumes: the scenario tests
   assert whole provenance maps directly, and a Verbose-only slot-map line dumps one character per
   slot.

⭐ **A mutation run caught the fence under-poisoning itself.** The first draft used one scribble seed,
which gave each slot exactly one of six values; a planted real leak survived it green. The case was
rewritten to sweep all **6³ = 216** seed combinations — exhaustive over the generator — and the same
mutant then failed.
**No other case in the 392-case suite caught that leak** — which is the measured statement of what
the fence is for, rather than an assertion that it works.

⛔ **One of the item's own three fences was contradictory and was correctly not implemented.** Fence
3(c) asked item 47's instrument to read the column instead of re-deriving lineage; fence 2 forbids any
production reader. Both cannot hold — item 47's counters ride a shipped Warning line, so making the
classifier read provenance would make a shipped counter a function of the column and turn the
independence case red. The fence was struck.

---

## 10. What was deferred, and when — DEFERRED, not planned

⛔ **None of the following is scheduled work.** Each is recorded with the date it was ruled, and each
needs a fresh ruling before it is picked up. Presenting a deferred item as planned is how a document
promises something the tree never delivers.

| what | status | ruled | why it was set down |
|---|---|---|---|
| **Item 30** — per-scale similarity tolerances | **DEFERRED** | 2026-08-12, user | Its role inverted rather than disappearing: once the disagreement policy shipped, a degenerate verdict cost *waste* rather than *capability*, so item 30 became a cost reduction and blocks nothing. |
| **Item 28** — divergence magnitude | **not landed** | — | Never implemented; item 30's tolerances were to be set from its measured deltas. The gap is stated at `ResimGateProbe.h`. |
| **Item 73** — Layer-1 exact comparison at correction ticks | **DEFERRED** | 2026-08-16, user ruling | No dependency blocked it; a scheduling choice. Both states are already in hand at the comparison site, so it needs no wire — it would discriminate *comparison bug* from *genuine upstream divergence*, which is the question items 28/30 are stuck on. |
| **Item 74** — replicated per-tick state hash | **DEFERRED** | 2026-08-16, user ruling | This is the half that costs packet budget, and it was deferred explicitly on that basis. Its carrier choice was left unresolved, and wire-format choices have historically been the user's personally. |
| **The gate rewire** — wiring either layer into the anchor decision | **DEFERRED**, separately from 73 and 74 | 2026-08-16 | It flips resim cadence from every-landing to hash-driven — the largest behaviour change available in this codebase — and it **re-opens the no-cooldown ruling**, part of which rested on the premise that a particular combination could not ship. A rewire changes which combination ships. It must be priced against re-measured cost tables and field data, in a dedicated pass that also owns renaming `onCheckIsSimilar` — a name that stops being true the moment a hash verdict exists. |
| **Item 46's remaining scope** | **partially shipped**, not discharged | 2026-08-12 | The ini flip landed; the compiled-default flip, the pre-flip demand preview, the six-character cost measurement and the additional latency arm did not. |
| **Item 31** | **open** | — | Its mechanism was answered by items 42 and 43; the item's own status was never closed. |
| **Item 78** — re-price the cost tables on an optimized build | **open** | promoted 2026-08-16 | The 2026-08-12 numbers were taken with optimization disabled by the per-file pragmas that were later put behind a build switch. ⚠ Its own instruction is that the archived tables get a *factor*, not a replacement. |
| **Item 51** — re-triage the resim log lines | **open** | filed 2026-08-12 | Once the gate started firing, per-replayed-tick-per-character lines classified as *"rare simulation lifecycle"* became **99.5%** of a client log — 343,514 of about 344,000 lines, 26,172,981 bytes in 3.3 minutes. Nothing in the code broke; **the classification stopped matching the mechanism.** It was sequenced *before* the six-character run because formatting that volume on the game thread risks measuring logging cost on the one instrument built to tell logging and resim cost apart. |
| **Item 50** — a physics-engine crash after a hard resync (observed on one adapter's engine, Chaos) | **open, and explicitly a suspicion rather than a defect** | filed 2026-08-12 | One occurrence, no repro. Recorded because under the legacy gate the deep-restore path was almost never exercised, so there is very little history for a path that began running constantly. Its own instruction is not to write it up as a known crash. |

---

## 11. ⛔ RETIRED NAMES — every dead symbol this document mentions

Each row names something the history above refers to that **does not exist as code**. Verified
2026-08-21 by a whole-tree sweep: every name in this table has **zero non-comment lines** anywhere
under the plugin, source, test and config trees. Several still appear in *comments* — deliberately,
as history — which is exactly why "present by grep" is not a usable check and why this table exists.

| # | retired name | what it became | retired by | comment-only lines, 2026-08-21 |
|---|---|---|---|---:|
| 1 | `m_isResimulated` | Deleted. Its gate role became `m_pendingResimAnchorTick`; its *diagnostic* role returned as `SlotStateProvenance`. | item 45, 2026-08-12 | 41 |
| 2 | `getLastResimulationTick` | Deleted. Replaced by `needsResimulation` reading the pending anchor. | item 45, 2026-08-12 | 13 |
| 3 | `resimCooldownTicks` | **Never shipped.** Built during item 45 and removed before landing; the throttle is structural. | user ruling, 2026-08-11 | 5 |
| 4 | `pushPredictionInput` | Deleted with the correction cache's input column. A slot holds state plus an applied-capture-tick reference and no input value. | item 16 (earlier initiative scope) | 19 |
| 5 | `getDiagnosticStateProvenance` | Landed as `Diagnostics::stateProvenance`, reached through `getDiagnostics()` — the view type carries the marker so the name does not. ⚠ The retirement is recorded in `docs/CorrectionCache-rationale.md` §4 — it was in-file at `CorrectionCache.h` until the task-13 compression relocated the record. **Anchored on the section, not a line number:** the line anchor this row used to carry was severed by that compression, which is the join hazard R12 exists to stop. | RN-1 + RN-2 grouping, item 52 | 1 |
| 6 | `prepareSimulationStep` | A rename that was **reverted**: item 90 renamed `collectInputAll` to it; item 94 reverted once frontier allocation left the method. `SimulationReconciliation.h`'s history line records both hops. | item 90 (2026-08-20), reverted by item 94 (2026-08-21) | 31 |
| 7 | `ClampedGrantsSeeTheSilentFindValidResimFrameMove` | Renamed `ClampedGrantsCountAnyRequestedVsGrantedMismatch` — the old name asserted a live engine behaviour that had been refuted; the new one describes the detector. | item 42 rework, 2026-08-11 | 0 |
| 8 | `ABehindFrontierLandingIsShadowedByTheInheritedFlag_DocumentsADefect_Item45WillChangeThis` | Rewritten as `ABehindFrontierDisagreeingLandingSetsThePendingAnchor`. The defect it documented was fixed, so the case pins the fix. | item 45, 2026-08-12 | 1 |
| 9 | `RepeatedBehindFrontierLandingsNeverReopenTheGate_DocumentsADefect_Item45WillChangeThis` | Rewritten as `RepeatedBehindFrontierLandingsCoalesceIntoOneNewestAnchor`. | item 45, 2026-08-12 | 1 |
| 10 | `InheritanceCarriesTheResimFlagForward` | **Replaced**, not renamed: it pinned the inheritance as correct at the time, and the guard was retired, so the replacement asserts what took its place — a consumed resim leaves no anchor and frontier advance touches no gate state. | item 45, 2026-08-12 | 2 |
| 11 | `freshClobbers` | **Never shipped** under that bare name. The pre-fix twin was not landed; only `freshClobbersAvoided` and `staleClobbersAvoided` exist. | item 47 as landed, 2026-08-12 | 0 |
| 12 | `dumpSlotProvenanceAll` | Landed as the const `logSlotProvenanceAll` on the diagnostics view. | RN-7 / item 56 | 0 |

⚠ **Rows 5, 6, 7, 11 and 12 are the ones a reader would otherwise get wrong**, because in each case
the archive names one identifier and the tree shipped another. Row 11 in particular: an archived
design describes a *pair* of counters, and only one half of the pair was ever built.

---

## 12. Live anchors — every symbol above that still resolves

Verified 2026-08-21: each `symbol` below appears on a **non-comment** line in the file named. The
check and its negative controls are recorded in this initiative's implementation notes; the point of
requiring a non-comment line is that a symbol surviving only in prose fails, which is the defect
class §11 exists for.

⚠ **One row below is not part of `og-simulation` at all.** `Config/DefaultEngine.ini` is the project
configuration of **one adapter's** host application — the shipping game built on this core — and not
of the engine-free core itself. It appears because the value that row names is what the shipped
build runs with; an adapter on another engine supplies that value from its own configuration surface
under whatever key it chooses, and the core reads it through `TimeConfig` either way.

| file | symbols |
|---|---|
| `ResimGatePolicy.h` | `shouldSetPendingAnchor`, `policyEnforcesDepthCeiling`, `isAnchorWithinDepthPolicy`, `classifyResimSlotWrite`, `ResimSlotWriteOutcome` |
| `CorrectionCache.h` | `StateCorrectionCache`, `m_pendingResimAnchorTick`, `needsResimulation`, `consumeResimAnchor`, `tryInsertingCorrectState`, `tryInsertingResimulatedState`, `pushPredictionTick`, `wipeCache`, `m_containsCorrectTick`, `m_slotLandingSeqNr`, `m_preparedLandingSeqNr`, `m_stateProvenance`, `compute_checksum`, `ChecksumByteBuffer` |
| `SlotStateProvenance.h` | `SlotStateProvenance`, `AuthorityAgreedKeptPrediction`, `ReplayedOverCorrection`, `slotStateProvenanceChar` |
| `SimulationReconciliation.h` | `checkDivergenceAll`, `prepareResimAll`, `applyResimAll`, `consumeResimAnchorsAll`, `postResimulationAll`, `logSlotProvenanceAll` |
| `SimulationManager.h` | `onCheckIsSimilar`, `prepareResimulation`, `onGameSimulationResimulation`, `onPostGameSimulation` |
| `PCTimeManagement/TimeConfig.h` | `resimTriggerPolicy`, `ResimTriggerPolicy`, `FrontierExact`, `OnDisagreement`, `rollbackWindowTicks` |
| `ResimGateProbe.h` | `ResimGateProbe`, `CorrectionLandingProbe`, `landedBehind`, `landedAtFrontier`, `freshClobbersAvoided`, `staleClobbersAvoided`, `clampedGrants`, `deepAnchorExclusions`, `survivingAnchors`, `stuckResimFrames`, `replayOverruns` |
| `Network/CorrectionVerdictProbe.h` | `CorrectionVerdictProbe` |
| `SimulationTypes.h` | `kDefaultSimilarityEpsilon`, `isSimilarToField` |
| `DesyncDiagnosticSink.h` | `DesyncDiagnosticEvent`, `shouldEscalateToLayer2` |
| `PCTimeManagement/ClientPredictionClock.h` | `isResimulating` |
| `Config/DefaultEngine.ini` | `ResimTriggerPolicy` |
| `og-simulation-tests/…/CorrectionCache/ResimGateSemanticsTest.cpp` | `ACompletedResimClosesTheGateAndItStaysClosed`, `ABehindFrontierDisagreeingLandingSetsThePendingAnchor`, `RepeatedFrontierExactLandingsCoalesceIntoOneNewestAnchor`, `TheProvenanceColumnCannotReachAnyProductionOutput`, `ReplayedOverCorrectionIsUnreachableButTheAlarmIsWired` |
| `og-simulation-tests/…/Reconciliation/ResimGateProbeTest.cpp` | `ClampedGrantsCountAnyRequestedVsGrantedMismatch` |

⚠ **One symbol above the table is deliberately outside it:** `FRewindData::FindValidResimFrame`
belongs to one adapter's physics engine, not to this repository, and §5.1 declares it as such rather
than leaving it to fail a check silently. That is the third disposition a symbol can have
here — live in-repo (§12),
retired (§11), or external and declared (§5.1) — and every symbol this document names has exactly
one of them.

---

## 13. What this document deliberately does not contain

Three siblings ship in this directory and each owns a slice this file stays out of:

- **`ResimGatePolicy-rationale.md`** — the *derivations*: why the naive freshness form is wrong, why
  the policy arithmetic sits in an STL-only header, the full no-cooldown argument, the fresh/stale
  classification, and the measurement records. **If this file and that one disagree about a
  mechanism, that one is right** — this file describes how a decision was reached, not what the code
  does.
- **`ThreadingCrossings.md`** — which of the calls above cross a thread boundary.
- **`Perspective-AuthorityVsPrediction.md`** — what runs on the authority versus a predicting client,
  and where the resim cycle sits in the tick.

⛔ **The fences stay in the headers.** Every guard this history mentions — the no-cooldown fence, the
naive-form fence, the protect-all rule, the single-character structural zero, the
classify-does-not-decide fence — is quoted here only to explain how it came to exist. The line that
stops the next editor is the one at the edit site, and moving it into a document would disarm it.

---

## 14. Honest gaps — what this history cannot tell you

1. **Whether the suppressed corrections mattered.** The legacy gate ignored roughly nine thousand
   behind-frontier corrections per run. Whether those were micrometres or metres is item 28's
   magnitude measurement, which was never taken. The under-resimulation statement is about *design
   intent*, not about observable damage.
2. **What the disagreement policy costs at six characters.** Cost scales as frames × depth ×
   characters and every measurement on record was taken at three.
3. **Whether the 2026-08-12 cost figures survive an optimized build.** They were taken with the
   per-file optimization pragmas disabled; item 78 exists to supply the factor and had not run as of
   2026-08-21.
4. **Why the locally-predicted character disagreed at all.** The design says it should be
   bit-identical. That anomaly was named on 2026-08-06 and, as of 2026-08-21, has no answer in the
   record — only three candidate readings and the measurement that would separate them.
5. **What the two protection counters read in the field.** `freshClobbersAvoided` and
   `staleClobbersAvoided` shipped with no live reading; both are near zero by construction under the
   compiled default, so their first measurement belongs to a run under the shipped configuration.
6. **Whether the anchor's threading argument has ever been exercised under contention.** The
   consume-CAS is proven both ways single-threaded, which is what makes it testable at all; the
   two-writer race it exists to close is argued from the memory model, not observed.
