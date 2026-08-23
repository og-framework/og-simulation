<!-- SPDX-License-Identifier: MPL-2.0 -->
# `CorrectionCache.h` — rationale

The header keeps a one-line guard at every site that has one, plus an orientation block
naming the six per-slot columns, the two threads and the five provenance write sites.
**This file holds the full text those guards were compressed from** — the derivation, the
rejected alternatives, the measurement records and the migration history.

**If this file and `CorrectionCache.h` disagree, the header is authoritative and this file
is stale.** Fix this file; do not soften the header to match it.

⛔ **Do not move a guard into this file.** Every section below is the *expansion* of a fence
that still fires at its own line. If you find yourself deleting a one-liner because "it is
in the rationale doc now", the guard has stopped working — the person about to make the
change is reading code, not documentation.

⛔ **The two D4 exemplars are NOT expanded away.** `m_isResimulated.set(cacheIndex)` IS GONE
FROM HERE (in `tryInsertingResimulatedState`) guards a line that does not exist, and the
deliberately-redundant `isAuthorityGradeProvenance` re-check (in the same method) guards
code whose justification is that it never fires. Both keep their full text **at their own
sites** in the header; what is quoted below is the record, not the fence.

⚠ **Adapter bindings the quoted text names — one adapter's, never the binding.** `og-simulation`
is engine-free: it names no game-engine type and is reached from a host engine only through
`concept`s. Three names in the quoted blocks belong to one adapter's host stack rather than to this
core, and the quotations are byte-verbatim, so their ROLES are given here instead of edited in:
**`Chaos`** is that adapter's **physics engine** — the party handed a tick to restore at, and the
owner of the rewind loop; **`FSimulationManagerAsyncCallback::OnPreSimulate_Internal`** is one
adapter's **pre-simulate physics callback**, where the core's step is entered before the solve; and
**`FSimulationStateSyncBuffer`** is one adapter's **replicated state buffer** — the wire-side type
whose `writeToBuffer<T>` / `readFromBuffer<T>` surface `ChecksumByteBuffer` deliberately mirrors so
the same serializers can target both. A fourth, `Config/DefaultEngine.ini`, is one adapter's
**host-application configuration surface**, named where a section records which compiled default the
shipped session actually runs; another adapter sets the same value under its own key.
None of the four is a dependency of this core.

> ⚠ **Every section body below is the VERBATIM pre-compression comment text**, emitted by
> `impl/task13/gen_doc_13.py` rather than retyped, so this file cannot have quietly edited
> what it copied. Bracketed tags (`[item 45]`, `[T16]`, `[item 84]`) are provenance labels
> that resolve only in the `og-netcode-v2-input-relay` archive — they are not references,
> and nothing in this repository resolves them.

> ⛔ **THREE OF THE QUOTED BLOCKS WERE FALSE WHEN QUOTED, and they are preserved false on
> purpose** — an archive that repairs itself stops being a record. All three named
> `SimulationNetSync` as the owner of things that moved to `SimulationInputResolution` at
> items 84-90: the `LocalInputCache` / `RemoteInputCache` ownership and the
> `getLastRelayedInput` / `resolveScheduledRelayedInput` readers (§1, twice), and the
> delay-line clear in `wipeAllForResync` (§10). **The shipped header states all three
> correctly.** Filed as F-T13-2.

<!-- lint-external-ref: m_isResimulated -- RETIRED (item 45): the per-slot resim bitset. Its gate role became m_pendingResimAnchorTick and its diagnostic role returned as SlotStateProvenance; the bitset must not resolve -->
<!-- lint-external-ref: getLastResimulationTick -- RETIRED (item 45): the newest-first scan that read the bitset; replaced by needsResimulation reading the pending anchor; must not resolve -->
<!-- lint-external-ref: pushPredictionInput -- RETIRED (T16): the input column's only writer; retired WITH the column, no successor, must not resolve -->
<!-- lint-external-ref: getInput -- RETIRED (T16): the per-slot input reader; retired with the column, must not resolve -->
<!-- lint-external-ref: getLatestInput -- RETIRED (T16): the frontier-slot input reader; must not resolve -->
<!-- lint-external-ref: getLastCorrectionInput -- RETIRED (T8): the correction-input reader; no successor, must not resolve -->
<!-- lint-external-ref: insertCorrectionInput -- RETIRED (T8): the SERVER->CLIENT correction-input channel terminus; must not resolve -->
<!-- lint-external-ref: m_containsCorrectionInput -- RETIRED (T8): the per-slot authority-input flag; retired with the channel that set it, must not resolve -->
<!-- lint-external-ref: receiveCorrectionInput -- RETIRED (T8): the free-function decoder for the retired input channel; must not resolve -->
<!-- lint-external-ref: getDiagnosticStateProvenance -- RETIRED NAME (T52): landed as Diagnostics::stateProvenance reached through getDiagnostics(); the view type carries the marker, so this name must not resolve -->
<!-- lint-external-ref: getDiagnosticSlotLandingSeqNr -- NEVER SHIPPED: the tripled name T52's own comment quotes to show what the view AVOIDS; it must not resolve -->
<!-- lint-external-ref: SimulationReconciliation::injectCorrectionInput -- RETIRED (T8): the reconciliation-side entry to the correction-input channel; must not resolve -->
<!-- lint-external-ref: SimulationNetSync::collectInputAll -- DEAD OWNER: collectInputAll moved to SimulationInputResolution at item 87; the qualified form must not resolve -->
<!-- lint-external-ref: SimulationNetSync::getLastRelayedInput -- DEAD OWNER, and QUOTED FALSE ON PURPOSE (F-T13-2): the reader lives on SimulationInputResolution since items 84-90. The archived block named SimulationNetSync and is preserved unrepaired; the shipped header states the correct owner -->
<!-- lint-external-ref: otherCache -- A LOCAL IN A QUOTED SNIPPET (`cache = otherCache`), not a name in this tree; it must not resolve -->
<!-- lint-external-ref: Diagnostic -- A WORD FRAGMENT, not a type: quoted from the rule that a view member carries no `Diagnostic` infix of its own; it must not resolve -->
<!-- lint-external-ref: ResimGatePolicyTest -- TEST TRANSLATION UNIT in og-simulation-tests, outside every scan root of this repository; it must not resolve here -->
<!-- lint-external-ref: impl/research_correction_discards.md -- ARCHIVE PATH in the og-netcode-v2-input-relay workspace, outside this repository; it must not resolve -->

---

## 1. Relocation and retirement — the input column, the retired accessors, the gravestone

**`CorrectionCache.h` pre-compression `:154-155` — NARRATIVE**

```
[og-netcode-v2-input-relay T16] WHAT A CACHE SLOT IS, AFTER THE INPUT COLUMN
WAS RETIRED.
```

**`CorrectionCache.h` pre-compression `:157-160` — NARRATIVE**

```
slot i  ==  { tick, STATE at that tick, the APPLIED-CAPTURE-TICK REF for
that tick, the LANDING STAMP for that tick, the DIAGNOSTIC
STATE-PROVENANCE byte for that tick, plus the two
bookkeeping bits }
```

**`CorrectionCache.h` pre-compression `:183-190` — FENCE**

```
There is NO input value in a slot any more. `m_inputBuffer`, `getInput`,
`getLatestInput` and `pushPredictionInput` are all gone: T6 re-pointed the
resim read to identity resolution, T7 re-pointed the remote-proxy viz to the
relay store, T8 removed the SERVER->CLIENT correction-input channel outright,
and T15 re-sourced the motion matcher to the client's raw capture history. The
column then had no consumer for any character, local or remote, and storing a
value nothing reads is how a stale second copy of the truth gets re-discovered
and trusted years later.
```

**`CorrectionCache.h` pre-compression `:192-204` — FENCE**

```
ANY FUTURE CODE LOOKING FOR AN INPUT VALUE IN THE CORRECTION CACHE IS LOOKING
FOR SOMETHING THAT NO LONGER EXISTS BY DESIGN. The three live sources, by
question asked:
* "what did the local player capture at tick t?"   -> LocalInputCache
(capture-tick keyed; SimulationNetSync owns one per provider-present id).
* "what did a REMOTE player send for capture t?"   -> RemoteInputCache, via
SimulationNetSync::getLastRelayedInput / resolveScheduledRelayedInput.
* "which capture did the AUTHORITY apply at tick t?" -> the ref that lives in
THIS slot, read through getAppliedCaptureTick / the classified
SimulationReconciliation::getAppliedCaptureTickRef.
The ref is the join key that replaced the value: 4 bytes naming an input,
instead of a copy of one. Re-adding the column would re-create exactly the
application-tick-vs-capture-tick ambiguity T15 was filed to fix.
```

**`CorrectionCache.h` pre-compression `:206-209` — FENCE**

```
The InputType template parameter DELIBERATELY SURVIVES the column: it is still
named by IntegrateFn and by advance_frame, i.e. by the externally-driven
4-method API the determinism harness runs on. The cache no longer STORES an
input; it still INTEGRATES with one.
```

**`CorrectionCache.h` pre-compression `:277-284` — FENCE**

```
[og-netcode-v2-input-relay T16] `getInput(cacheIndex)` IS GONE with the input
column. It also carried the tick-0 phantom: `m_tickBuffer.fill(0)` in both
constructors means every UNWRITTEN slot claims tick 0, so
`getInput(getCacheIndex(0))` resolved to slot 0 and then OG_CHECK-failed on
that slot's empty optional (calling value() on it in an unchecked build).
Nothing can trip that any more, because there is no optional to be empty.
The replacement for "which input produced tick T" is the ref immediately
below — read it, then ask the delay line or the relay store for the value.
```

**`CorrectionCache.h` pre-compression `:302-303` — FENCE**

```
[T16] This ref is now the ONLY input-related thing a slot carries; the input
COLUMN it used to sit beside is retired. See the class header block.
```

**`CorrectionCache.h` pre-compression `:500-509` — FENCE**

```
[og-netcode-v2-input-relay T8] `getLastCorrectionInput` — the walk-backwards
"what did the server last tell us this player did" reader — IS GONE, together
with `insertCorrectionInput` and the `m_containsCorrectionInput` bitset it
tested. It read the SERVER->CLIENT correction-input channel, and that channel
is retired: nothing sets a correction flag on an input slot any more, so the
method could only ever have returned `std::nullopt`. It was removed rather
than left in place precisely BECAUSE it would still have compiled, still have
looked authoritative at a call site, and still have answered "no input has
ever been corrected" forever, silently. See the retirement block at
SimulationNetSync::sendCorrectionAll for the full channel inventory.
```

**`CorrectionCache.h` pre-compression `:511-512` — FENCE**

```
[og-netcode-v2-input-relay T16] `getLatestInput` — "the input at the current
prediction-tick slot" — IS GONE, together with the column it read.
```

**`CorrectionCache.h` pre-compression `:514-517` — FENCE**

```
T8 kept it deliberately, under the rule "remove what T8 makes FALSE, leave
what T8 makes merely UNUSED": it still returned real, freshly-written data,
it just had no caller left. T16 removes the writer, so there is nothing to
return and the distinction collapses.
```

**`CorrectionCache.h` pre-compression `:519-526` — FENCE**

```
It was ALSO the most dangerous accessor on this class for a remote
character, and that is worth keeping on the record: the slot was written by
`pushPredictionInput` only, so for a proxy it answered the CLIENT'S OWN
PREDICTION of that player's input (post-T7, the relay store's scheduled
read) while looking exactly like a server-sourced value — refreshed every
tick, so with no staleness to notice. The correct remote source is
`SimulationNetSync::getLastRelayedInput` / `resolveScheduledRelayedInput`;
the correct local source is the client's own `LocalInputCache`.
```

**`CorrectionCache.h` pre-compression `:743-756` — FENCE**

```
[og-netcode-v2-input-relay T16] `pushPredictionInput` — the client-local
writer of the input column, and by T8 its ONLY writer — IS GONE. Its two
production call sites were both arms of what was then
`SimulationNetSync::collectInputAll` (the provider branch, writing the
already-delayed applied capture; and the simulated-proxy branch, writing
this client's guess at the remote input) — since item 87 that class is
`SimulationInputResolution`, since item 90 the tick push moved to a
dedicated sweep, and since item 94 that sweep itself moved to
`SimulationReconciliation::allocateFrontierSlotsAll` — this class's own
`pushPredictionTick`, called from there, not inline in either branch —
plus `SimulationReconciliation::backfillSkippedTick` and `advance_frame`
below. The prediction TICK is still pushed at every one of those sites —
only the input write went, so the ring's slot allocation is bit-for-bit
unchanged.
```

**`CorrectionCache.h` pre-compression `:1064-1071` — FENCE**

```
[og-netcode-v2-input-relay T8] `insertCorrectionInput` — the SERVER->CLIENT
correction-input channel's terminus — IS GONE. Its only production caller was
`SimulationReconciliation::injectCorrectionInput`, itself reached only from
the OnRep-bound callback that `SimulationNetSync::registerPredictionOwner`
used to install; all three are retired together with the replicated property
on the UE side. The isAnomalousMiss severity gate it shared with
`tryInsertingCorrectState` is unaffected — that gate lives on the method
below and is still exercised through the state path.
```

**`CorrectionCache.h` pre-compression `:1073-1076` — FENCE**

```
[T16] T8's note here said the input COLUMN itself (`m_inputBuffer`) and its
client-local writer `pushPredictionInput` were deliberately left standing,
as T16's scope. THEY ARE NOW GONE TOO — T8 removed the channel, T16 removed
the column. Nothing on this class stores an input value any more.
```

**`CorrectionCache.h` pre-compression `:1171-1173` — FENCE**

```
[item 45] `m_isResimulated[slot] = false;` is gone with the bitset.
[T4] Freshly allocated slot — no correction has named a join key for
this tick yet (mirrors pushPredictionTick).
```

**`CorrectionCache.h` pre-compression `:1267-1270` — FENCE**

```
[T16] `input` is CONSUMED, not stored: it feeds m_integrateFn and nothing
else, because the cache no longer has an input column to commit it to. This
is why the InputType template parameter outlives that column — see the class
header block. The 4-method external API's shape is unchanged for callers.
```

**`CorrectionCache.h` pre-compression `:1595-1599` — FENCE**

```
[T16] `m_inputBuffer` — `std::array<std::optional<InputType>, 60>`, the
input COLUMN — is gone. One optional input composite per slot, times 60
slots, times every predicted character on every client, storing a value that
no consumer had left after T6/T7/T8/T15. See the class header block for what
replaced each read.
```

**`CorrectionCache.h` pre-compression `:1663-1670` — FENCE**

```
[og-netcode-v2-input-relay item 45] `m_isResimulated` — the per-slot "this
slot's state came from a resim replay" bit — IS GONE, and with it the gate's
entire derived-state machinery: the newest-first scan
(`getLastResimulationTick`), the frontier inheritance in `pushPredictionTick`,
and the five-site bit discipline that had to stay correct across
`pushPredictionTick` / `tryInsertingCorrectState` /
`tryInsertingResimulatedState` / `wipeCache` / `save_snapshot` or the gate
broke SILENTLY (which is exactly what happened, for months).
```

**`CorrectionCache.h` pre-compression `:1672-1679` — FENCE**

```
It had exactly ONE production reader — the gate — verified by exhausting
readers before removal (design §1 blast radius): no serialization, no input
resolution, no state adoption, no probe (the probes count events, not bits).
So retiring the reader retired the value, and the T16 rule says to remove it
rather than leave a stored second copy of the truth for a future reader to
find and trust. `tryInsertingResimulatedState` keeps its item-42 `bool` return
— that is a REPORT of whether the slot existed, which the I6 probe counts, and
it never had anything to do with this bit.
```

**`CorrectionCache.h` pre-compression `:1681-1685` — FENCE**

```
⚠ IF YOU ARE HERE BECAUSE YOU WANT PER-SLOT RESIM PROVENANCE FOR A
DIAGNOSTIC: it is genuinely gone, and re-adding it as a bitset would re-create
the discipline above. The event stream is already counted at Warning
(`ResimGateProbe`: prepares / finishes / replayTicks / replayOverruns), which
is what every diagnostic asking "was this tick replayed" has actually wanted.
```

**`CorrectionCache.h` pre-compression `:1718-1722` — FENCE**

```
[T8] `m_containsCorrectionInput` — the per-slot "this input came from the
authority" flag — is gone with the channel that set it. Its sole setter was
insertCorrectionInput and its sole reader getLastCorrectionInput; with the
setter retired the flag could only ever have read false, so keeping it would
have kept a discriminator that no longer discriminates.
```

**`CorrectionCache.h` pre-compression `:1884-1888` — FENCE**

```
[og-netcode-v2-input-relay T8] `receiveCorrectionInput` — the free-function
mirror of receiveCorrectionState for the input channel — IS GONE. It decoded a
(tick, input) payload that no sender writes any more. `receiveCorrectionState`
above is unchanged and remains the live correction path; it is the state, plus
the T4 applied-capture-tick ref it carries, that the client now needs.
```

---

## 2. The resim gate — item 45, one word, edge-triggered

**`CorrectionCache.h` pre-compression `:178-181` — FENCE**

```
[item 45] TWO BITS, NOT THREE: `m_isResimulated` retired with the
level-triggered resim gate. The gate's state is no longer per-slot at all — it
is ONE per-character atomic word, `m_pendingResimAnchorTick`. See the gate block
above `needsResimulation`.
```

**`CorrectionCache.h` pre-compression `:331-331` — NARRATIVE**

```
find the highest tick in the buffer
```

**`CorrectionCache.h` pre-compression `:435-438` — NARRATIVE**

```
[og-netcode-v2-input-relay item 45] THE RESIM GATE — ONE WORD, EDGE-TRIGGERED.
(design_task43_resim_gate_fix.md §1, §3 candidate D, §3.1; the defect it
repairs is impl/finding_task31_resim_rate.md; the semantics are pinned by
og-simulation-tests `[CorrectionCache][ResimGate]`.)
```

**`CorrectionCache.h` pre-compression `:440-450` — FENCE**

```
⛔ `getLastResimulationTick()` IS GONE, together with the `m_isResimulated`
bitset it scanned and the `pushPredictionTick` inheritance that fed it. What
it did: computed `getDiagnostics().lastCorrectTick()`, then walked newest->oldest FROM THE
FRONTIER SLOT (offset 0) and returned the first slot flagged `m_isResimulated
|| m_containsCorrectTick`. Since `postResimulationAll` flagged every replayed
slot INCLUDING the frontier and `pushPredictionTick` copied that bit into
every new frontier slot, after any completed resim the scan terminated at
offset 0 with `anchor == predictionTick` and the gate was pinned FALSE. A
correction landing BEHIND the frontier set its own slot's bit and was never
reached: ~8,759 behind-frontier corrections against 2 triggers per measured
run. The gate measured CLOCK ALIGNMENT, not divergence.
```

**`CorrectionCache.h` pre-compression `:452-460` — FENCE**

```
⚠ AND THE INHERITANCE WAS A LOAD-BEARING GUARD, NOT THE BUG. A level-triggered
gate needs something to hold it closed once the state that opened it has been
consumed. Seeding the new frontier `false`, or starting the scan at offset 1,
produced an UNTERMINATING 1-tick-resim storm (the just-replayed flagged slot
sits one below an unflagged frontier and re-triggers every tick, for the ~60
ticks until the ring recycles it). Those are design §3's candidates A and B
and they are DEAD — do not resurrect them. The inheritance line was deleted
HERE, in this change, ONLY BECAUSE THE READER IS GONE: an edge-triggered gate
needs no hold, because nothing recomputes it from derived state.
```

**`CorrectionCache.h` pre-compression `:462-475` — FENCE**

```
WHAT REPLACES IT — `m_pendingResimAnchorTick`, 0 == none:
W1  set   `tryInsertingCorrectState`'s hit path, GAME thread, when
`resimGate::shouldSetPendingAnchor` says so. CAS-max, so several
landings between two checks COALESCE to the newest.
W2  clear the resim-completion edge, PHYSICS thread, as a LITERAL CAS
against the anchor captured at prepare time (see
`consumeResimAnchor`).
W3  clear `wipeCache` (hard resync), zeroed with the tick numbering it
belongs to.
R1  read  `needsResimulation()` + this accessor, PHYSICS thread, per frame
via `SimulationReconciliation::checkDivergenceAll`.
R2  read  the prepare-time capture, PHYSICS thread.
TERMINATION IS STRUCTURAL: events set the anchor, completion consumes it, and
no rescan of slot state can re-trigger a consumed correction.
```

**`CorrectionCache.h` pre-compression `:484-489` — FENCE**

```
⛔ IT MUST NEVER ENTER THE DETERMINISM CHECKSUM. It is transient CONTROL
state, not simulation state: two peers replaying the same inputs from the same
state must agree on the STATE, and they legitimately differ on whether a resim
is pending. `compute_checksum` hashes `m_stateBuffer[tick]` only, and
`save_snapshot` / `load_snapshot` / `advance_frame` deliberately do not touch
the anchor — see the non-site list in design §3.1.
```

**`CorrectionCache.h` pre-compression `:492-494` — FENCE**

```
R1 — the pending anchor, or 0 when no resim is pending. This is the value
`checkDivergenceAll` folds to a min across characters and hands to Chaos as
the tick to restore at.
```

**`CorrectionCache.h` pre-compression `:528-533` — FENCE**

```
R1 — THE GATE. Unchanged in shape from the level-triggered version
(`anchor != 0 && anchor != predictionTick`) and deliberately so: the second
clause is what makes a correction landing ON the frontier wait for the
frontier to move before it triggers, which is the legacy timing the
`FrontierExact` policy has to reproduce. Only the SOURCE of `anchor` changed
— a stored trigger instead of a derived scan.
```

**`CorrectionCache.h` pre-compression `:535-537` — NARRATIVE**

```
`const` now, because it no longer computes anything: worth one word of note
because the old signature's non-constness was the only thing stopping
`findInputCache`'s const route from asking the gate directly.
```

**`CorrectionCache.h` pre-compression `:544-547` — FENCE**

```
W2 — CONSUMPTION, AND IT IS A LITERAL COMPARE-AND-SWAP. Called on the resim
completion edge (the `[Resim.Finish]` block in
`SimulationManager::onPostGameSimulation`, beside `applyResimAll`) with the
anchor this resim was PREPARED with. Returns true when it consumed.
```

**`CorrectionCache.h` pre-compression `:549-558` — FENCE**

```
⭐ WHY THE CAS IS THE MECHANISM AND NOT A DEFENSIVE FLOURISH. A correction can
land on the GAME thread while the replay is running on the PHYSICS thread. A
compare-then-store would read `pending == prepared`, then be overtaken by W1
writing a NEWER anchor, then store 0 over it — a silently lost trigger, which
is the original defect in miniature. As a CAS the newer anchor makes the
exchange FAIL and SURVIVE, so the next frame re-triggers on it. The intended
behaviour becomes impossible to violate rather than improbable by timing, and
— because the expected value is a parameter — it is exercisable
SINGLE-THREADED in a unit test: prime the anchor, consume with a stale
expected value, assert survival.
```

**`CorrectionCache.h` pre-compression `:560-563` — FENCE**

```
A `preparedAnchorTick` of 0 means this resim consumed nothing (no anchor was
pending when it was prepared, e.g. an engine-side rewind we did not ask for);
it can never match a live anchor, so it is rejected up front rather than
CAS-ing against the "none" sentinel.
```

**`CorrectionCache.h` pre-compression `:574-574` — NARRATIVE**

```
R2 — the prepare-time capture, and its consume partner.
```

**`CorrectionCache.h` pre-compression `:576-584` — FENCE**

```
⚠ WHY THE CAPTURE IS PER CACHE AND NOT ONE VALUE AT THE MANAGER. A resim is
prepared from the MIN anchor across characters (`checkDivergenceAll` folds
with `std::min`), so character B's anchor is frequently NEWER than the tick
the resim restores at. CAS-ing every character against that single min would
fail for B, leave B's anchor pending, and re-trigger a resim next frame — a
treadmill in any session with more than one character, where the legacy gate
closed for everyone because `postResimulationAll` flagged every character's
replayed slots. Each cache therefore captures ITS OWN anchor and consumes
against that.
```

**`CorrectionCache.h` pre-compression `:586-598` — FENCE**

```
⚠ AND THIS ONE IS A PLAIN `uint32`, WHICH IS NOT AN OVERSIGHT NEXT TO THE
ATOMIC ABOVE. It is PHYSICS-THREAD-PRIVATE: written by `prepareResimAll` and
read by the completion edge, both on the physics thread, never touched by the
game thread. The one-atomic-word fence (design §3.2 invariant 4) governs the
GATE word, which is two-writer; this is a single-threaded scratch value for
the CAS's expected argument and gains nothing from an atomic.
[item 47] IT ALSO CAPTURES THE LANDING-SEQUENCE BASELINE, and that is why
this one method is still the whole of "prepare" for this cache. The two
captured values are the two arms of the freshness classifier
(`resimGate::classifyResimSlotWrite`): the anchor answers "which ticks was
this resim supposed to act on", the sequence answers "which slots were
written after it started". Capturing them anywhere but the same instant
would let a landing fall between them and be counted in neither population.
```

**`CorrectionCache.h` pre-compression `:624-631` — FENCE**

```
The trigger-policy seam. The SOURCE OF TRUTH is `TimeConfig::
resimTriggerPolicy`; this is the pushed copy the game-thread write site
consults, published one way through `SimulationManager::
setResimTriggerPolicy` -> `SimulationReconciliation::setResimTriggerPolicy`
-> here, which is also what stamps caches created later (a character
registering mid-session). The default below is read from the TimeConfig
default rather than named as a literal, so R-P1 holds and a retune of the
shipped policy tracks automatically.
```

**`CorrectionCache.h` pre-compression `:633-636` — FENCE**

```
GAME-THREAD-ONLY in effect: it is written at composition (before any
correction can land) and read at W1. It is not atomic for that reason, and
making it a runtime-tunable cvar would break that argument — see the
one-shot-at-composition ruling on the sibling knobs in SimulationManager.h.
```

**`CorrectionCache.h` pre-compression `:673-687` — FENCE**

```
⛔ [item 45] THE INHERITANCE LINE IS GONE FROM HERE:
m_isResimulated[newPredictionIndex] = m_isResimulated[predictionIndex];
IT IS DELETED BECAUSE THE READER IS GONE, NOT BECAUSE THE GUARD WAS WRONG.
It held the level-triggered gate closed after a completed resim, and
deleting it on its own reintroduced an unterminating 1-tick-resim storm
(design §3 candidate A). With the gate edge-triggered there is nothing left
to hold: `getLastResimulationTick`'s scan — `m_isResimulated`'s only reader
— is retired, and frontier advance no longer touches gate state AT ALL.
That last clause is the property to protect here: a future edit that makes
`pushPredictionTick` write the anchor would put derived-state re-triggering
back and the storm with it. See the gate block above `needsResimulation`.
[T4] The recycled slot now describes a DIFFERENT tick, so the previous
occupant's join key must retire with it — otherwise a resim through the
fresh tick would resolve remote input from a capture ~StateBufferSize
ticks old and be confidently wrong rather than merely uninformed.
```

**`CorrectionCache.h` pre-compression `:734-739` — FENCE**

```
⛔ AND UNLIKE THE LINE THIS SITS BESIDE, IT INHERITS NOTHING. The old
bit's `m_isResimulated[new] = m_isResimulated[prev]` inheritance is
the deleted line six comments up, and it was DELETED BECAUSE ITS
READER WAS GONE. Writing a CONSTANT here — never a value read out of
the previous slot — is what keeps that true: frontier advance still
touches no gate state, and nothing here is derived from anything.
```

**`CorrectionCache.h` pre-compression `:900-908` — FENCE**

```
⛔ [item 45] `m_isResimulated.set(cacheIndex)` IS GONE FROM HERE, and
this is the site that makes the storm structurally impossible rather
than merely unlikely: REPLAY WRITES STATE AND PROVENANCE, NEVER GATE
STATE. Under the level-triggered gate this line was what re-closed the
gate after a resim (and, one tick later via the frontier inheritance,
what shadowed every behind-frontier correction). The gate is now closed
by an explicit CAS on the completion edge — see
`StateCorrectionCache::consumeResimAnchor` — so a replay tick has no
business touching it.
```

**`CorrectionCache.h` pre-compression `:1003-1003` — NARRATIVE**

```
[item 45] W1 — THE ONE EVENT THAT OPENS THE RESIM GATE.
```

**`CorrectionCache.h` pre-compression `:1005-1011` — FENCE**

```
This replaces `m_isResimulated[cacheIndex] = false;` — the old
un-shadow, which re-opened the gate only when the slot it landed in
HAPPENED to be the frontier. The condition is now explicit and
configured instead of emergent (`resimGate::shouldSetPendingAnchor`),
and it is evaluated HERE because this is the single chokepoint every
landed correction already passes through: a future correction source
or cadence feeds the gate with no new wiring.
```

**`CorrectionCache.h` pre-compression `:1013-1019` — FENCE**

```
`landedAtFrontier` is the same comparison `classifyCorrectionLanding`
makes for item 42's landing probe, so the probe's `atFrontier` bucket
and the legacy policy's trigger condition are the same predicate on
the same value — which is what makes the probe's archived
`atFrontier` counts the baseline this policy has to reproduce.
`getPredictionTick()` is read here rather than after the state move
only for locality; the insert never touches `m_tickBuffer`.
```

**`CorrectionCache.h` pre-compression `:1084-1089` — FENCE**

```
[item 45] W3 — THE ANCHOR DIES WITH THE TICK NUMBERING. A resync renumbers
the prediction clock, so a surviving anchor would name a tick that no
longer exists — at best a resim to nowhere, at worst (if the new numbering
is lower) an anchor permanently ABOVE the frontier that the gate can never
close. Same retirement discipline as `m_appliedCaptureTickBuffer` two lines
down, and it also re-arms the CAS-max coalescing from a clean 0.
```

**`CorrectionCache.h` pre-compression `:1152-1158` — FENCE**

```
[item 45] AND IT DELIBERATELY DOES NOT TOUCH THE RESIM ANCHOR — one of the
explicit non-sites in design §3.1. The 4-method API is the externally-driven
determinism surface: it saves and replays SIMULATION state, and whether a
resim is pending is transient CONTROL state that two peers may legitimately
disagree on while remaining perfectly deterministic. Writing the anchor here
would make a harness run's gate state depend on snapshot order, and reading it
into a checksum would make identical simulations hash differently.
```

**`CorrectionCache.h` pre-compression `:1378-1380` — FENCE**

```
[item 45] R2's diagnostic read of the prepare-time anchor capture. Write
side: `captureResimAnchorForConsume` (physics thread, production; stays
on the cache — see the fence above this class).
```

**`CorrectionCache.h` pre-compression `:1383-1385` — NARRATIVE**

```
[og-netcode-v2-input-relay item 45] "Which is the newest tick an
authoritative correction has LANDED in?" — a pure query over
`m_containsCorrectTick`.
```

**`CorrectionCache.h` pre-compression `:1387-1397` — FENCE**

```
ITS FORMER PRODUCTION READER IS GONE: `getLastResimulationTick` used
this as the lower bound of its newest-first scan, and that scan is
retired with the level-triggered gate (see the anchor block above
`getPendingResimAnchorTick`). It is kept rather than retired with it,
and the T16 rule is why that is not a contradiction: T16 retires STORED
values nothing reads, because a second copy of the truth goes stale
silently. This stores nothing — it derives an answer from the live
bitset on every call and cannot be stale. It remains the honest way to
ask the question (the resim ANCHOR is now a decided trigger, not "the
newest correction", and the two must not be conflated), and it is what
the wipe cases assert against.
```

**`CorrectionCache.h` pre-compression `:1415-1415` — NARRATIVE**

```
iterate backwards, from the prediction index, through the ring buffer to find the last correct tick
```

**`CorrectionCache.h` pre-compression `:1430-1431` — FENCE**

```
The trigger-policy read-back. Write side: `setResimTriggerPolicy`,
which stays on the cache — see the fence above this class.
```

**`CorrectionCache.h` pre-compression `:1539-1540` — NARRATIVE**

```
[item 45] W1's CAS-MAX — "the anchor is the NEWEST tick anybody asked to
resimulate from".
```

**`CorrectionCache.h` pre-compression `:1542-1550` — FENCE**

```
NEWEST-CORRECTED COALESCING IS DELIBERATE, not a shortcut around a queue
(design §3, candidate C analysis): corrections arrive in tick order, and the
authority state at the newest corrected tick SUBSUMES every older correction
on the same trajectory. Restoring at the newest and replaying forward consumes
the whole backlog at depth = `frontier - anchor` ~= correction transit latency,
so several landings between two divergence checks cost ONE resim rather than
one each. "Older corrections are dead weight" dissolves the moment resims fire
against recent ones: the older ones are SUBSUMED, which is not the same as
ignored.
```

**`CorrectionCache.h` pre-compression `:1552-1558` — FENCE**

```
A CAS loop rather than `fetch_max` (C++26) or a plain compare-then-store: W1
is on the game thread while W2/W3 are on the physics thread, so a
read-then-write could resurrect an anchor W2 has just consumed. Losing the CAS
means someone else moved the word; re-reading and re-testing is what makes the
max honest. `tick == 0` cannot become an anchor — 0 is the "none" sentinel AND
the reserved pre-sim tick, so the loop's guard rejects it for both reasons at
once.
```

**`CorrectionCache.h` pre-compression `:1730-1733` — FENCE**

```
[item 45] THE GATE. 0 == no resim pending. The ONE piece of cross-thread state
this class has any synchronization on; see the block above
`getPendingResimAnchorTick` for the four write sites and why the atomic is
load-bearing (two writers on two threads: GT set vs PT consume/wipe).
```

**`CorrectionCache.h` pre-compression `:1735-1740` — FENCE**

```
⛔ DO NOT "SIMPLIFY" THIS BACK TO A PLAIN `uint32`, and do not grow the gate to
a second shared word. The plain word carries a real lost-update window, not
merely formal UB — a consume's compare-then-clear can stomp a newer anchor,
which is a silently lost resim trigger, i.e. the very defect this change
exists to fix, reintroduced in miniature. Two shared words would need an
ordering argument between them that one word does not need.
```

**`CorrectionCache.h` pre-compression `:1749-1752` — FENCE**

```
[item 45] R2's capture — PHYSICS-THREAD-PRIVATE, hence plain. Written by
`captureResimAnchorForConsume` at prepare, read by `consumeCapturedResimAnchor`
on the completion edge. See the note at those methods for why the expected
value must be per cache rather than the manager's single min anchor.
```

**`CorrectionCache.h` pre-compression `:1763-1765` — FENCE**

```
[item 45] The pushed copy of `TimeConfig::resimTriggerPolicy` — see
`setResimTriggerPolicy`. Sourced from the TimeConfig default rather than a
literal so R-P1 holds.
```

**`CorrectionCache.h` pre-compression `:1785-1805` — FENCE**

```
DECLARED NON-PROPERTIES — load-bearing, read before touching this bit:
* NOT GATE STATE. `needsResimulation()` must never read it — item
45's one-atomic-word fence governs the GATE
(`m_pendingResimAnchorTick`); this is a different word entirely,
and growing the gate to a second shared word is what that fence
forbids.
* NEVER ENTERS `compute_checksum`. Transient control bookkeeping,
the anchor's prohibition verbatim: two peers replaying identical
inputs from identical state must agree on STATE and may
legitimately disagree on this bit's transient value.
* PT-ONLY. `pushPredictionTick` / `pushPredictionState` /
`wipeCache` / `advance_frame` are all physics-thread (or
harness-thread), so this adds NO GT/PT crossing — no
`docs/ThreadingCrossings.md` row.
* A SEPARATE WORD FROM THE PROVENANCE COLUMN, deliberately. An
`OG_CHECK` against `m_stateProvenance` instead would be a
production read of that column and would fire under fence 2's
`…TheProvenanceColumnCannotReachAnyProductionOutput` scribble
(that test garbage-fills the provenance column mid-lifecycle and
asserts every production output byte-identical). This bit is a
dedicated word precisely so that fence stays green.
```

**`CorrectionCache.h` pre-compression `:1854-1854` — FENCE**

```
[item 45] NON-COPYABLE AND NON-MOVABLE, stated rather than inherited.
```

**`CorrectionCache.h` pre-compression `:1856-1864` — FENCE**

```
`std::atomic` is neither copyable nor movable, so both would be implicitly
deleted anyway; they are spelled out because the resulting compile error at a
future `cache = otherCache` is otherwise a puzzle about a member three
hundred lines up. THE SEMANTIC REASON THEY MUST STAY DELETED: a cache
duplicated while a resim is pending would give two objects one anchor, so the
CAS on either would consume a trigger the other still believes in. Caches are
created in place — `SimulationReconciliation::createCacheFor` uses
`try_emplace` so the map never needs a move — and live for the character's
registration; nothing legitimately copies one.
```

---

## 3. The landing stamps and the protect-all rule — item 47

**`CorrectionCache.h` pre-compression `:171-176` — FENCE**

```
[item 47] THE LANDING STAMP is the one column added since: "the value of this
cache's monotonic landing counter when a correction was last inserted here",
0 for never. It exists to split item 47's replay-write PROTECTIONS into fresh
and stale populations for the probe — it decides nothing. The write rule reads
`m_containsCorrectTick` alone. See `m_slotLandingSeqNr` and
`resimGate::classifyResimSlotWrite`.
```

**`CorrectionCache.h` pre-compression `:310-313` — FENCE**

```
[og-netcode-v2-input-relay T6] Has an authoritative correction landed in this
slot? Set by tryInsertingCorrectState, cleared by pushPredictionTick (ring
recycle), save_snapshot (fresh slot) and wipeCache (resync) — i.e. exactly
alongside m_appliedCaptureTickBuffer's own retirement points.
```

**`CorrectionCache.h` pre-compression `:600-603` — FENCE**

```
The sequence capture is PHYSICS-THREAD-PRIVATE in the same way the anchor
capture is, and it READS a game-thread-written counter — see the threading
note on `m_slotLandingSeqNr` (private section, below). It feeds a COUNTER,
never a decision.
```

**`CorrectionCache.h` pre-compression `:689-696` — FENCE**

```
[item 47] AND SO MUST THE LANDING STAMP, for a sharper version of the
same reason: it is retired alongside `m_containsCorrectTick`, which is
what the protection rule reads. Leaving a stale HIGH stamp on a
recycled slot would make the NEXT resim classify a fresh landing there
as... still fresh (the stamp only rises), so it cannot mis-protect —
but it would report a protection for a landing that never happened.
The stamp's "0 == no correction has ever landed here" contract is worth
more than the two instructions it costs to keep.
```

**`CorrectionCache.h` pre-compression `:772-777` — FENCE**

```
[og-netcode-v2-input-relay item 42] RETURNS TRUE WHEN THE SLOT WAS FOUND —
purely observational, and the same defaulted-out-parameter trick
`tryInsertingCorrectState` uses one method down, in its cheapest form: a
return value nothing was reading. Every existing call site ignores it and is
byte-identical, which is what makes this behaviour-neutral by construction
rather than by inspection.
```

**`CorrectionCache.h` pre-compression `:779-783` — FENCE**

```
WHY IT HAS TO LEAVE THE FUNCTION AT ALL. The discard branch already logs at
Warning, so the EVENT is visible; what does not exist is a DENOMINATOR — item
42's I6 `replayOverruns` needs "how many discards per window of replay ticks",
and a log line cannot be counted by the code that emits it. The cache stays
id-agnostic and probe-agnostic: it reports, `postResimulationAll` tallies.
```

**`CorrectionCache.h` pre-compression `:786-787` — FENCE**

```
[og-netcode-v2-input-relay item 47] ⛔ IT NO LONGER WRITES UNCONDITIONALLY —
A REPLAY NEVER OVERWRITES A CORRECTED SLOT.
```

**`CorrectionCache.h` pre-compression `:789-795` — FENCE**

```
The rule, its rationale, the provenance invariant it makes hold, and the
fresh/stale classifier are all in ONE block: `resimGate::classifyResimSlotWrite`
in ResimGatePolicy.h. Read that before changing anything here. In one line:
authority state (or a within-tolerance prediction the authority certified)
is strictly better than a re-derivation of it, so the replay's own output
is dropped for that slot and `m_containsCorrectTick` is left ALONE — never
cleared, because nothing authority-marked is ever overwritten.
```

**`CorrectionCache.h` pre-compression `:821-825` — FENCE**

```
⚠ THE RETURN VALUE STILL MEANS "THE SLOT EXISTED", NOT "I WROTE". A
PROTECTED slot returns TRUE: item 42's `replayOverruns` counts ticks that
fell out of the 60-slot window, and re-pointing it at protections would
silently redefine an archived baseline (1-2 per RUN) into a different
population. The protections get their OWN counters, through `outOutcome`.
```

**`CorrectionCache.h` pre-compression `:827-829` — FENCE**

```
`outOutcome` is the same defaulted, purely observational out-pointer
`tryInsertingCorrectState` uses one method down — every pre-item-47 call
site is byte-identical by construction, not by inspection.
```

**`CorrectionCache.h` pre-compression `:842-848` — FENCE**

```
[item 47] ONE CALL DECIDES BOTH THE ACTION AND ITS LABEL. The
action is `outcome != Written`; the fresh/stale split only chooses
which counter the protection lands in. A separate write predicate
beside this call was tried and REJECTED — a mutation run produced a
build that overwrote the slot while still reporting
`ProtectedFresh`, i.e. a counter that disagreed with reality. See
the ACTION note on `resimGate::ResimSlotWriteOutcome`.
```

**`CorrectionCache.h` pre-compression `:861-864` — FENCE**

```
PROTECTED. The replay's state for this tick is discarded; the
authority state and its provenance bit both stand. No log line:
this is per-replay-tick and would be exactly the T19 volume
defect. The event is counted on `[ResimProbe.Apply]` instead.
```

**`CorrectionCache.h` pre-compression `:971-977` — FENCE**

```
[item 47] THE LANDING STAMP. One monotonic counter per cache,
bumped here and only here, so "this slot was corrected after that
resim was prepared" is answerable by comparing two integers rather
than by reasoning about wall time. It is stamped on BOTH verdicts
— an agreeing landing is authority information too (it certifies
the prediction), and the protection rule makes no verdict
distinction. See `resimGate::classifyResimSlotWrite`.
```

**`CorrectionCache.h` pre-compression `:1102-1107` — FENCE**

```
[item 47] The landing stamps die with the tick numbering too, and for
the strongest form of the reason: after a resync a surviving stamp
would describe a slot whose TICK has been renumbered, so the classifier
would compare an old landing's sequence against a new resim's capture
and call an ancient slot fresh. The counter is re-armed from 0 with the
slots, which keeps "stamp 0 == never landed" true after a wipe as well.
```

**`CorrectionCache.h` pre-compression `:1356-1362` — FENCE**

```
[item 47] The per-slot landing stamp. THE FULL THREADING CONTRACT — the
GT-write/PT-read argument, why this rides the pre-existing state-buffer
race rather than opening a new one, why it is observational and can
never mis-decide a write, and the wraparound bound — sits with
`m_slotLandingSeqNr`'s declaration in the private section below this
class. Read it there before relying on this accessor for anything but a
diagnostic.
```

**`CorrectionCache.h` pre-compression `:1369-1369` — NARRATIVE**

```
The monotonic counter itself — the value the NEXT landing will exceed.
```

**`CorrectionCache.h` pre-compression `:1372-1375` — FENCE**

```
[item 47] The prepare-time landing-sequence capture. Diagnostics and
tests only — production reads it exactly once, inside the classifier
call in `tryInsertingResimulatedState`. Write side (the capture itself):
`captureResimAnchorForConsume`, which stays on the cache.
```

**`CorrectionCache.h` pre-compression `:1606-1608` — FENCE**

```
[item 47] THE PER-SLOT LANDING STAMP — "the value of this cache's monotonic
landing counter when a correction was last inserted into this slot", 0 for a
slot no correction has ever landed in.
```

**`CorrectionCache.h` pre-compression `:1646-1650` — FENCE**

```
WRAPAROUND: `m_landingSeqNr` is monotonic and never reset except by
`wipeCache`. At 2^32 landings — >2 years of continuous 60 Hz corrections on
one character — the `>` comparison would invert for one window and
mis-CLASSIFY (never mis-protect). Not defended against, recorded so it is a
known bound rather than a surprise.
```

**`CorrectionCache.h` pre-compression `:1652-1653` — NARRATIVE**

```
60 * 4 B = 240 B per character, which is the whole memory cost of the
fresh/stale split.
```

**`CorrectionCache.h` pre-compression `:1755-1759` — FENCE**

```
[item 47] THE MONOTONIC LANDING COUNTER (GAME thread; W1 is its only
writer) and the PREPARE-TIME CAPTURE of it (PHYSICS thread; written by
`captureResimAnchorForConsume`, read by the classifier). Both plain, both
observational — the write rule reads neither. Full argument, including why
this is not a second gate word, at `m_slotLandingSeqNr`'s declaration above.
```

---

## 4. The state-provenance column and its five write sites — item 48

**`CorrectionCache.h` pre-compression `:162-169` — FENCE**

```
[item 48] THE STATE-PROVENANCE BYTE is the newest column and it is the ONLY
one on this class that NOTHING IN PRODUCTION READS — deliberately, and
machine-checked. It answers "where did the state in this slot come from?"
(`SlotStateProvenance`: Empty / Predicted / AuthorityAdopted /
AuthorityAgreedKeptPrediction / Replayed / ReplayedOverCorrection), 60 bytes
per cache. ⚠ It is NOT the retired `m_isResimulated` bit coming back — read
the three fences in SlotStateProvenance.h and the gravestone at the bottom of
this class BEFORE assuming otherwise. See `getDiagnostics().stateProvenance`.
```

**`CorrectionCache.h` pre-compression `:231-233` — FENCE**

```
[item 48] Nothing has written state anywhere yet, and `Empty` is the one
place that fact is recorded in data — the tick buffer cannot carry it,
because filling it with 0 makes every unwritten slot claim tick 0.
```

**`CorrectionCache.h` pre-compression `:698-700` — FENCE**

```
[item 48] WRITE SITE 1 of 5 — RING RECYCLE. The slot now describes a
different tick and holds no state for it yet, so its lineage retires
with the rest of its bookkeeping.
```

**`CorrectionCache.h` pre-compression `:725-732` — FENCE**

```
⚠ `Predicted`, NOT `Empty`, and the reason is the ordering of the two
calls production always makes: `pushPredictionTick` is immediately
followed by `pushPredictionState` (postPredictionAll, backfillSkippedTick,
advance_frame — every one of them). A slot allocated by this method
is a prediction slot; `Empty` would be true for the width of one
statement and would then be a stale lie for the whole ring pass. The
place `Empty` genuinely belongs is a slot that has never been written
at all — the constructors and `wipeCache`.
```

**`CorrectionCache.h` pre-compression `:866-869` — FENCE**

```
[item 48] AND THE PROVENANCE COLUMN IS LEFT ALONE TOO, which is
the whole reason a completed resim's map shows an unbroken
`A`/`C` at every protected slot rather than an `R`: the state
did not change, so its lineage did not either.
```

**`CorrectionCache.h` pre-compression `:873-874` — FENCE**

```
[item 48] WRITE SITE 3 of 5 — THE REPLAY WRITE, and the ONE site
that can stamp the alarm value.
```

**`CorrectionCache.h` pre-compression `:876-883` — FENCE**

```
⭐ THE CHECK IS DELIBERATELY REDUNDANT WITH THE ONE ABOVE, AND THE
REDUNDANCY IS THE INSTRUMENT. `classifyResimSlotWrite` decided this
write on `m_containsCorrectTick`; this asks a SECOND, INDEPENDENT
source — the provenance column — whether that bit was telling the
truth. Under item 47's protect-all the two can never disagree
(`ProtectedFresh`/`ProtectedStale` short-circuits above at exactly
the population `isAuthorityGradeProvenance` marks), so this branch
is UNREACHABLE and `ReplayedOverCorrection` reads zero forever.
```

**`CorrectionCache.h` pre-compression `:885-890` — FENCE**

```
⛔ THAT IS NOT A REASON TO DELETE IT. The pre-item-47 state — bit
set, state replayed — was real, shipped and invisible for months
for exactly one reason: nothing could represent it. A regression
that re-opens the clobber now stamps an `X` in the slot map instead
of hiding. Collapsing the two sources into one would delete the
alarm and leave a comment claiming it exists.
```

**`CorrectionCache.h` pre-compression `:892-893` — FENCE**

```
It reads no state it did not already have and changes NO decision:
the write below happens either way. See `SlotStateProvenance`.
```

**`CorrectionCache.h` pre-compression `:947-953` — FENCE**

```
⛔ THE MARKER NAMES THE CHANNEL, NOT THE FACT (docs/DiagnosticsConventions.md
§4). `predictionWasCorrect` below, computed by `isSimilarTo`, feeds
`resimGate::shouldSetPendingAnchor` directly — under the shipped
ResimTriggerPolicy=OnDisagreement that boolean decides whether a resim runs
at all. `outDiagnosticVerdict` only carries a COPY of that fact out for
reporting; it is not the path the decision takes. Deleting `isSimilarTo`
silently disables the resim gate.
```

**`CorrectionCache.h` pre-compression `:980-981` — FENCE**

```
[item 48] WRITE SITE 2 of 5 — AND THE ONE THAT CARRIES THE COLUMN'S
FIRST PAYLOAD VALUE.
```

**`CorrectionCache.h` pre-compression `:983-991` — FENCE**

```
⭐ THE BRANCH IS THE VERDICT BRANCH TAKEN TWENTY LINES DOWN, and
this is the point of an ENUM rather than a bool. `m_containsCorrectTick`
is set for BOTH verdicts, so the bit alone says "authority-grade" and
stops; it cannot say WHICH KIND. The state copy below runs only
`if (!predictionWasCorrect)` — so on an AGREEING landing the slot is
authority-grade while physically holding the PREDICTED value. Item 47
treats the two identically ON PURPOSE (a rule that protected only
adopted state would need a second bit to tell them apart); its rule
does not need the distinction, and a human reading a slot map does.
```

**`CorrectionCache.h` pre-compression `:993-997` — FENCE**

```
⚠ MIRROR THE `if (!predictionWasCorrect)` BELOW IF IT EVER MOVES.
The two are one decision expressed twice, and the case
`…ADisagreeingLandingAdoptsAuthorityAndAnAgreeingOneCertifiesThePrediction`
asserts state and provenance TOGETHER on both arms precisely so they
cannot drift apart silently.
```

**`CorrectionCache.h` pre-compression `:1111-1117` — FENCE**

```
[item 48] WRITE SITE 4 of 5 — THE WIPE, and the one place `Empty` is
written after construction. A resync renumbers every tick, so no slot
describes state for the tick it now claims: "predicted" and "corrected"
are both false of every slot here, and the ONLY honest lineage is "no
state has been written for this tick". Note the frontier slot is
renumbered two lines down and stays `Empty` too — `wipeCache` sets its
TICK, never its state; the next `pushPredictionState` is what fills it.
```

**`CorrectionCache.h` pre-compression `:1182-1182` — FENCE**

```
[item 48] WRITE SITE 5 of 5 — THE FRESHLY ALLOCATED HARNESS SLOT.
```

**`CorrectionCache.h` pre-compression `:1184-1194` — FENCE**

```
⭐ WHY THE 4-METHOD API WRITES PROVENANCE WHILE IT DELIBERATELY DOES
NOT WRITE THE ANCHOR — the two look like the same kind of exception
and are not. THE ANCHOR IS A LIVE TRIGGER: writing it here would let
a determinism harness's snapshot ORDER decide whether a resim fires,
which is why design §3.1 lists this method as an explicit non-site.
PROVENANCE DESCRIBES STATE LINEAGE, and the harness legitimately
CREATES state — `save_snapshot` is the externally-driven twin of
`pushPredictionTick` + `pushPredictionState`, so the honest answer to
"where did this state come from" is the same one that path gives.
Leaving it `Empty` would claim no state had been written into a slot
this line is about to write state into.
```

**`CorrectionCache.h` pre-compression `:1235-1246` — FENCE**

```
⚠ [item 48] AN **EXISTING** SLOT KEEPS ITS OLD PROVENANCE WHILE ITS
STATE IS REPLACED, so a harness snapshotting over a slot a correction
already landed in leaves an `A`/`C` describing state the harness wrote.
That is a KNOWN, PRE-EXISTING RESIDUAL rather than a new one, and it is
recorded rather than repaired for three reasons: `m_containsCorrectTick`
and the applied-capture ref have behaved exactly this way here since T4,
so provenance is CONSISTENT with its neighbours instead of uniquely
wrong; the path is reachable only from the 4-method determinism-harness
API, never from the live client; and item 47's review §6 already lists
it (with the GT/PT `m_stateBuffer` frontier race) as the residual pair
belonging to design §7.4. ⛔ It does NOT produce `ReplayedOverCorrection`
— that value is stamped by the replay path alone.
```

**`CorrectionCache.h` pre-compression `:1435-1436` — NARRATIVE**

```
[og-netcode-v2-input-relay item 48] THE PER-SLOT STATE PROVENANCE —
"WHERE DID THE STATE IN THIS SLOT COME FROM?", DIAGNOSTIC-ONLY.
```

**`CorrectionCache.h` pre-compression `:1438-1441` — FENCE**

```
⛔ THE FULL CONTRACT, THE THREE FENCES AND THE ⚠ AGAINST MISTAKING THIS
FOR THE RETIRED `m_isResimulated` BIT ARE IN `SlotStateProvenance.h`.
Read that block before changing any of the five write sites, and read
the gravestone at the bottom of this class before adding a sixth.
```

**`CorrectionCache.h` pre-compression `:1443-1452` — FENCE**

```
The one-paragraph version, because a reader who lands HERE needs it:
this column is written at five sites and read by NOTHING IN
PRODUCTION. [T52] It was named `getDiagnosticStateProvenance` for
exactly that reason before RN-1+RN-2 grouped it here; under
`getDiagnostics()` the VIEW carries the marker instead of the name, so
the member below is simply `stateProvenance`. Its readers are the
`[CorrectionCache][ResimGate][Provenance]` LLTs and the Verbose
`[ResimProbe.SlotMap]` line (`SimulationReconciliation::
getDiagnostics().logSlotProvenanceAll`, RN-7/task 56), and that is the
complete list.
```

**`CorrectionCache.h` pre-compression `:1454-1463` — FENCE**

```
⭐ THE INDEPENDENCE IS MACHINE-CHECKED, and the case is named so it can
be found and so it cannot be quietly dropped:
CorrectionCache.ResimGate.TheProvenanceColumnCannotReachAnyProductionOutput
It scribbles arbitrary garbage into this column at three points of a
live gate lifecycle and asserts every production output — gate,
anchor, captures, verdicts, replay write outcomes, adopted state and
the determinism CHECKSUM — is byte-identical to the un-scribbled run.
THAT CASE IS THE FENCE. The original resim-gate defect was production
logic derived from per-slot bits; the case makes that regression fail
a test rather than merely contradict a comment.
```

**`CorrectionCache.h` pre-compression `:1500-1501` — FENCE**

```
⛔ THE FENCE'S OWN INSTRUMENT. IT HAS NO PRODUCTION CALLER AND MUST
NEVER ACQUIRE ONE — the name says so at every call site on purpose.
```

**`CorrectionCache.h` pre-compression `:1503-1505` — FENCE**

```
Fills the whole provenance column with a deterministic garbage cycle
derived from `seed`, i.e. makes every slot's recorded lineage a LIE
while leaving every other column untouched. Two cases use it:
```

**`CorrectionCache.h` pre-compression `:1507-1514` — FENCE**

```
1. `…TheProvenanceColumnCannotReachAnyProductionOutput` — fence 2. If
any production output moved, some production path is reading
this column and the fence is broken.
2. `…ReplayedOverCorrectionIsUnreachableButTheAlarmIsWired` — forges
the guard-failure precondition (authority-grade provenance over a
CLEAR `m_containsCorrectTick`) that protect-all makes
unreachable, so the alarm value can be proven live rather than
merely asserted absent.
```

**`CorrectionCache.h` pre-compression `:1516-1521` — FENCE**

```
The cycle walks all `kSlotStateProvenanceCount` enumerators and
offsets by slot index, so no two adjacent slots agree and every value
appears. It stays INSIDE the enumeration deliberately: a genuinely
out-of-range byte would be undefined behaviour on the switch in
`slotStateProvenanceChar`, and "every slot lies" is already the whole
hypothesis under test.
```

**`CorrectionCache.h` pre-compression `:1655-1659` — FENCE**

```
[item 48] Per-slot STATE LINEAGE — see `Diagnostics::stateProvenance`
(relocated here by RN-1+RN-2/T52) for the contract and
`SlotStateProvenance.h` for the three fences. 60 * 1 B = 60 B per character.
⛔ DIAGNOSTIC ONLY: no production reader, and the independence is
machine-checked by a named LLT rather than asserted here.
```

**`CorrectionCache.h` pre-compression `:1688-1693` — FENCE**

```
⚠⚠ [og-netcode-v2-input-relay item 48, 2026-08-12] ANNOTATION, NOT A
REVERSAL. **THE BITSET ABOVE STAYS RETIRED AND EVERY WORD OF THIS BLOCK
STILL STANDS.** `m_isResimulated` is gone, the gate does not read per-slot
state, `getLastResimulationTick` is not coming back, and the five-site
discipline this block warns about is exactly the hazard that cost items
31/42/43/44/45.
```

**`CorrectionCache.h` pre-compression `:1695-1710` — FENCE**

```
WHAT CHANGED is that the DIAGNOSTIC the paragraph directly above turns
away now exists deliberately, as `m_stateProvenance` — an ENUM column,
not a bitset, whose value `Replayed` answers everything the old bit
could and whose other five values answer things it could not. It was
added ON TOP OF this warning rather than in ignorance of it, and it is
admitted only because three fences make the hazard un-shippable:
1. it is not a bool and DOES NOT TAKE THE OLD NAME (every archived
document binds `m_isResimulated` to trigger semantics);
2. it has NO production reader, and that is MACHINE-CHECKED by
`…TheProvenanceColumnCannotReachAnyProductionOutput`, which
garbage-fills the column mid-lifecycle and asserts every production
output is byte-identical — so "the gate derives from a per-slot
column" is now a RED TEST rather than a discouraged practice;
3. its readers exist on day one (the scenario LLTs and one Verbose
`[ResimProbe.SlotMap]` line), so T16's stored-value-nothing-reads
rule is satisfied rather than re-broken.
```

**`CorrectionCache.h` pre-compression `:1712-1716` — FENCE**

```
⛔ A READER OF THIS BLOCK MUST NOT CONCLUDE THE OLD MECHANISM IS BACK. If
you are about to wire provenance into a trigger, item 48's own non-goal
forbids it by name — not even to reproduce the legacy virgin-cache free
trigger — and you must argue against fence 2's independence case in your
design rather than weaken it here. Full contract: SlotStateProvenance.h.
```

---

## 5. The two-thread surface, and the honest bound on the invariant

**`CorrectionCache.h` pre-compression `:477-482` — FENCE**

```
⚠ IT IS A TRIGGER, NOT A DATA-PUBLICATION CHANNEL. The corrected STATE this
anchor points at travels through `m_stateBuffer`, whose unsynchronized GT/PT
access is the cache's PRE-EXISTING formal race (finding §1 thread note),
unchanged by this change and owned by its own future item (design §7.4). The
anchor makes the GATE race-free; it does not make the cache race-free, and it
must never be read as if it did.
```

**`CorrectionCache.h` pre-compression `:797-809` — FENCE**

```
⚠ [og-netcode-v2-input-relay item 81] THE INVARIANT'S HONEST BOUND — it
holds UP TO AN ACKNOWLEDGED WORD RACE, not "by construction" unqualified.
`m_containsCorrectTick` is a TWO-WRITER WORD: GT `set()` right here (below,
~:898) and the PT clear in the push path (`pushPredictionTick`, ~:660) are
both read-modify-writes of the same `std::bitset` word, so a PT
frontier-clear can lose-update a concurrent GT landing-set (or the
reverse), erasing one slot's authority mark. The deliberately-redundant
provenance check (`ReplayedOverCorrection`, WRITE SITE 3 below) catches
exactly that lost-set case, but only on the `[ResimProbe.SlotMap]` Verbose
surface — never on this decision path. The mechanism fix (moving this
crossing onto the SPSC staging seam) is item 82's, deliberately gated;
this note corrects the description only, and changes neither behaviour
nor severity assessment.
```

**`CorrectionCache.h` pre-compression `:811-819` — FENCE**

```
⚠ [item 83 / 81 f2] CALIBRATION, FOR A READER WHO LANDS ONLY HERE: the
architecture review that derived this correction sizes the PRACTICAL
exposure as a few-instruction window per event at 60 Hz x ~20 Hz —
self-healing at the next landing, hence rare. The ARCHITECTURAL point
stands regardless (an accepted-tear price list priced torn *values*; a
lost *decision bit* is a different line item), which is why the
description above states the honest bound unqualified. This sentence adds
no new severity judgement — it surfaces the one already reached — and does
not reopen item 82's gated mechanism fix.
```

**`CorrectionCache.h` pre-compression `:1091-1099` — FENCE**

```
⚠ THREAD: this runs on whichever thread drives the resync callback, which
is the PHYSICS thread — `ClientPredictionClock::advancePrediction` is
called from `SimulationManager::onGameSimulationPrediction`, i.e. from
`FSimulationManagerAsyncCallback::OnPreSimulate_Internal`, and the clock
invokes the registered resync callback inline from there
(`SimulationManager`'s ctor -> `m_reconciliation.wipeAllForResync`). So W3
shares its thread with W2 and races only against W1 on the game thread —
which the atomic store handles, and which is why the anchor is atomic
rather than this site needing a lock.
```

**`CorrectionCache.h` pre-compression `:1465-1470` — FENCE**

```
⚠ THREADING: GT-written (corrections) and PT-written (prediction,
replay, wipe), plain bytes, riding the pre-existing `m_stateBuffer`
GT/PT race exactly as item 47's landing stamps do. A diagnostic read
TOLERATES a torn or ±1-stale value: one slot of one map line may be
wrong, and because nothing decides on it there is no correctness
consequence to be had.
```

**`CorrectionCache.h` pre-compression `:1616-1644` — FENCE**

```
⚠ THREADING, STATED HERE BECAUSE THIS IS THE ONE NEW CROSS-THREAD SURFACE
ITEM 47 ADDS. The stamps are written on the GAME thread (`W1`'s hit path)
and read on the PHYSICS thread (the classifier). They are PLAIN `uint32`s,
deliberately, and this is NOT a second gate word:
* they RIDE THE PRE-EXISTING STATE-BUFFER RACE they exist to protect. The
authority STATE this stamp describes travels through `m_stateBuffer`,
whose unsynchronized GT/PT access is the cache's long-standing formal
race (design §7.4, its own future item). A stamp torn or stale relative
to the state beside it is the same race, not a new class of one.
* they are OBSERVATIONAL. `resimGate::classifyResimSlotWrite` returns
`Written` iff `!slotContainsCorrectTick` — REGARDLESS of the stamps —
and the write site acts on `outcome != Written`, so the decision rests
on exactly one input: `m_containsCorrectTick`. [og-netcode-v2-input-relay
item 81] That bit is TWO WRITERS ON ONE WORD, not the milder
"GT-written / PT-read" shape this sentence used to claim — GT `set()`
in `tryInsertingCorrectState` (~:898) and the PT clear in the push
path (~:660) are both read-modify-writes of the same `std::bitset`
word, so the failure mode this decision is exposed to is a LOST
UPDATE, not a torn read. See the honest-bound note on item 47's header
block above for the full shape and what catches it. The stamps only
split the protections into fresh/stale for the probe. A torn stamp
mis-labels a COUNTER; it can never mis-decide a write. (That one-bit
property is swept as its own section in `ResimGatePolicyTest`,
because it is what this whole paragraph rests on.)
* ⛔ THEY ARE NOT GATE STATE. `needsResimulation()` does not read them and
must never be made to. Item 45's one-atomic-word fence governs the GATE
(`m_pendingResimAnchorTick`), which is two-writer control state; growing
the GATE to a second shared word is what that fence forbids, and this is
not that.
```

**`CorrectionCache.h` pre-compression `:1742-1746` — FENCE**

```
⚠ THIS DOES NOT CONTRADICT ITEM 42's "NO ATOMICS" RULE. That rule governs the
PROBES — telemetry, where the answer was two objects, one per thread, because
a shared window costs a whole window's totals on a race. This is CONTROL state
with two legitimate writers and exactly one correct value; splitting it per
thread would mean two gates that disagree.
```

---

## 6. The correction-miss log gate — Stage 5 / Task 18

> **Archive provenance for the cadence number.** The header's cost-model fence says
> only that *the last retune falsified the number this comment used to carry* and
> refuses to restate K. The retune was **T34** of `og-netcode-v2-input-relay`: it
> moved the compiled `TimeConfig::correctionRotationK` default from 2 to **1** and
> left `CorrectionRotationK` commented out in `Config/DefaultEngine.ini`, so the
> shipped session runs the compiled default. The ini's own block records the same
> thing in its own words, and names item 40 (the wire diet) as what would restore 2.
> ⛔ Both quoted blocks below still say *"the shipped K = 2 with six characters"* and
> a miss cost of *"3 at K=2/N=6"*. **They are preserved false on purpose** — the
> shipped header is authoritative and states the relation rather than the value.

**`CorrectionCache.h` pre-compression `:341-341` — NARRATIVE**

```
Correction-miss log gating (Stage 5 / Task 18)
```

**`CorrectionCache.h` pre-compression `:343-346` — NARRATIVE**

```
A correction whose tick has no slot in this cache is DISCARDED. That is a
routine, expected, self-healing outcome on three known-benign paths, all
characterised from the 2026-07-20 dedicated-server PIE session
(`impl/research_correction_discards.md`):
```

**`CorrectionCache.h` pre-compression `:348-356` — NARRATIVE**

```
1. Freshly registered remote proxy — the per-simulatable cache exists but
pushPredictionTick has never been called, so the tick buffer is still
all-zero and EVERY correction misses for a few ticks until the proxy
joins the resim loop.
2. Connect transient — the client's prediction frontier has not yet
converged on authority; corrections land ahead of or behind it until
the clock settles (or HardResync fires).
3. Post-Skip holes — a graduated Skip advances the frontier by two,
leaving a gap that backfillSkippedTick does not always cover.
```

**`CorrectionCache.h` pre-compression `:358-360` — NARRATIVE**

```
Reconciliation anchors on the newest LANDED correction rather than on a
specific tick, so a miss costs delayed reconciliation rather than lost
reconciliation.
```

**`CorrectionCache.h` pre-compression `:362-370` — FENCE**

```
⚠ [og-netcode-v2-input-relay T39] THE COST IS NO LONGER "AT MOST ONE TICK",
AND CORRECTIONS ARE NO LONGER AN UNTHROTTLED PER-TICK STREAM. Both clauses
were true when this block was written and both are now false:
`SimulationNetSync::sendCorrectionAll` writes `TimeConfig::correctionRotationK`
characters' state buffers per tick, round-robin, so each character is
corrected at `tickFrequency * K / N` Hz — 60 Hz at N <= K, and 20 Hz at the
shipped K = 2 with six characters. A miss therefore costs up to one ROTATION
SLOT of delayed reconciliation: bounded by ceil(N/K) ticks (3 at K=2/N=6),
not by 1.
```

**`CorrectionCache.h` pre-compression `:372-379` — FENCE**

```
⛔ THE GATE BELOW IS UNAFFECTED, AND THAT IS A DERIVED FACT, NOT AN
OVERSIGHT — do not "update" it for the new cadence. The gate compares the
missed tick's DISTANCE FROM THE PREDICTION FRONTIER against
rollbackWindowHardCap. That distance is a CLOCK-OFFSET quantity (how far the
client is predicting ahead of the authority tick the correction describes);
it does not depend on how OFTEN corrections are sent. Rotation changes the
number of corrections per second, not the distance any one of them lands at.
The 2026-07-20 verification numbers below therefore still bind unchanged.
```

**`CorrectionCache.h` pre-compression `:381-383` — HISTORY**

```
Logging these at Warning cried wolf badly enough to cost real diagnostic time
during that session, because the shape resembled the v1 T23/T24 hard-lock bug
signature.
```

**`CorrectionCache.h` pre-compression `:385-392` — FENCE**

```
GATE: warn only when the missed tick is FURTHER than rollbackWindowHardCap
from the prediction frontier. That bound is the maximum depth the reconciler
will ever resimulate, so a miss beyond it could not have become a useful
resim anchor even had a slot existed — and, because TimeConfig pins the
ordering invariant `hardResyncThresholdTicks > rollbackWindowHardCap`, a
sustained miss out there means the clock should have hard-resynced and has
not. That is genuinely anomalous. Everything closer is routine and logs at
Verbose.
```

**`CorrectionCache.h` pre-compression `:394-397` — HISTORY**

```
Verified against the 2026-07-20 session (both clients, both miss sites,
111 events): 108 events had a live frontier with a max distance of exactly
20 == rollbackWindowHardCap, and 3 were empty-cache registration transients.
This gate suppresses all 111 while still firing at distance 21+.
```

**`CorrectionCache.h` pre-compression `:399-402` — FENCE**

```
NOTE: the frontier check is `getPredictionTick() != 0` because both
constructors fill the tick buffer with 0. The deferred UINT32_MAX empty-slot
sentinel (researcher item 3, deliberately out of scope here) would make this
exact rather than conventional.
```

**`CorrectionCache.h` pre-compression `:407-409` — FENCE**

```
No prediction frontier yet — registration transient. Comparing a real
server tick against a never-initialised frontier is meaningless, not
anomalous.
```

**`CorrectionCache.h` pre-compression `:420-423` — FENCE**

```
Seam for wiring the LIVE TimeConfig once a consumer has one to hand. The
default is sourced from the TimeConfig default (not a literal), so R-P1
holds and the gate tracks any retune of the field's default; it does NOT
track a runtime override. Acceptable for a log gate, one line to fix.
```

**`CorrectionCache.h` pre-compression `:920-920` — NARRATIVE**

```
Tick not in cache window — correction arrived too late or too early; discard.
```

**`CorrectionCache.h` pre-compression `:1048-1051` — FENCE**

```
Tick not in cache window — correction arrived too late or too early; discard.
Severity is gated by isAnomalousMiss: routine self-healing misses log at
Verbose, only a miss beyond the reconciler's reach warns. predictionTick is
now included so a Warning from this site is actionable on its own.
```

**`CorrectionCache.h` pre-compression `:1726-1726` — FENCE**

```
Log gate only — never read by insertion logic. See isAnomalousMiss.
```

---

## 7. The 4-method determinism API and the checksum

> **Archive provenance for the deferral.** The header's orientation block says the
> comparison and wire layers that would give this API a production consumer were
> deferred on 2026-08-16 by user ruling, and points here for *what* was deferred.
> They are **items 73 and 74** of `og-netcode-v2-input-relay`: the state-comparison
> layer and the wire layer. Until one of them lands, the only consumers of
> `save_snapshot` / `load_snapshot` / `advance_frame` / `compute_checksum` are the
> Catch2 determinism harness and the low-level tests, which is why the header says so
> as a fence rather than as a remark.

**`CorrectionCache.h` pre-compression `:28-28` — NARRATIVE**

```
Checksum support for the StateCorrectionCache 4-method external API.
```

**`CorrectionCache.h` pre-compression `:30-34` — FENCE**

```
crc32 is the standard reflected CRC-32 (polynomial 0xEDB88320, init 0xFFFFFFFF,
final XOR 0xFFFFFFFF) implemented via a 256-entry lookup table built once on
first use. It is declared `inline` (not `static`) so a translation unit that
includes this header without ever instantiating compute_checksum does not emit
an -Wunused-function warning on the standalone GCC/Clang/NDK build paths.
```

**`CorrectionCache.h` pre-compression `:36-41` — FENCE**

```
ChecksumByteBuffer is a minimal byte sink exposing the same writeToBuffer<T> /
readFromBuffer<T> template surface as the UE-side FSimulationStateSyncBuffer, so
the existing writeToSyncedBuffer / writeCompositeToSyncedBuffer serializers can
target it without any UE dependency. compute_checksum CRCs the serialized bytes
(padding-free, deterministic) rather than the raw object, which keeps the hash
stable across compilers/architectures for the cross-arch determinism harness.
```

**`CorrectionCache.h` pre-compression `:67-67` — NARRATIVE**

```
Scalar/trivially-copyable field sink used by writeToSyncedBuffer descriptors.
```

**`CorrectionCache.h` pre-compression `:217-219` — NARRATIVE**

```
Integrate functor for the externally-driven advance_frame() path:
(tick, prevState, input) -> newState. The cache itself owns no integration
logic; the harness injects it via the 2-arg constructor below.
```

**`CorrectionCache.h` pre-compression `:237-239` — FENCE**

```
Overload that injects an integrate functor so advance_frame() can
drive one externally-triggered sim step. The single-arg constructor stays;
caches built with it must not call advance_frame() (it OG_CHECK-fails).
```

**`CorrectionCache.h` pre-compression `:1135-1139` — NARRATIVE**

```
StateCorrectionCache 4-method external API (proposal §2.2).
Additive public wrappers around the existing internal mechanisms; the legacy
API surface above is unchanged. These exist so the Catch2 determinism harness
(and, from Stage 3, the rollback driver) can save/load/advance/checksum the
cache without a live SimulationManager.
```

**`CorrectionCache.h` pre-compression `:1141-1141` — NARRATIVE**

```
Writes `state` into the cache slot for `tick`.
```

**`CorrectionCache.h` pre-compression `:1143-1150` — FENCE**

```
Slot-collision semantics: if `tick` is already present in the cache (at any
index, e.g. inserted earlier via the ring-advancing prediction path), its
existing slot is updated in place — we never create a duplicate entry for the
same tick. Otherwise the slot is `tick % StateBufferSize`; writing there
naturally evicts whatever older tick previously mapped to that ring slot
(the cache holds a rolling window of StateBufferSize ticks). A freshly
allocated slot has its per-slot bookkeeping bits reset (mirrors
pushPredictionTick), so stale correction metadata cannot leak.
```

**`CorrectionCache.h` pre-compression `:1175-1180` — FENCE**

```
[item 47] Same reset as pushPredictionTick's, for the same reason:
a freshly allocated slot has had no landing. NOTE the 4-method API
still touches NO gate state and NO landing COUNTER — the stamp is
per-slot bookkeeping that mirrors the bits beside it, whereas
`m_landingSeqNr` / the anchor are session control state a determinism
harness must not be able to perturb (design §3.1's non-site list).
```

**`CorrectionCache.h` pre-compression `:1196-1198` — FENCE**

```
Neither write can perturb a simulated value: the anchor is excluded
because it WOULD, and provenance is admitted because it CANNOT (fence
2, and the checksum prohibition on both).
```

**`CorrectionCache.h` pre-compression `:1250-1250` — NARRATIVE**

```
Returns true and fills `out_state` if `tick` is in the cache window; false otherwise.
```

**`CorrectionCache.h` pre-compression `:1261-1265` — FENCE**

```
Drives one externally-triggered sim step. Reads the previous prediction
state, integrates it via the injected functor, then commits the new
(tick, state) into the cache exactly as the manual
pushPredictionTick + pushPredictionState sequence would.
OG_CHECK-fails if the cache was built without an integrate functor.
```

**`CorrectionCache.h` pre-compression `:1279-1280` — FENCE**

```
Integrate into a fresh value BEFORE mutating the ring (prevState is a
reference into m_stateBuffer and must stay valid through the call).
```

**`CorrectionCache.h` pre-compression `:1287-1291` — FENCE**

```
CRC-32 over the serialized bytes of the state cached at `tick`. Returns 0
(with a logger warning) if `tick` is not in the cache window. The state is
serialized via the project serializer when possible (padding-free,
deterministic across compilers/architectures); trivially-copyable test
types fall back to a raw-byte hash.
```

**`CorrectionCache.h` pre-compression `:1472-1475` — FENCE**

```
⛔ IT NEVER ENTERS `compute_checksum` OR THE DETERMINISM COMPARISON —
the anchor's prohibition, verbatim, for the same reason: two peers
replaying identical inputs from identical state must agree on the
STATE and may legitimately disagree on how each of them got there.
```

**`CorrectionCache.h` pre-compression `:1567-1567` — NARRATIVE**

```
`current` has been reloaded with the value that beat us — re-test.
```

**`CorrectionCache.h` pre-compression `:1571-1573` — NARRATIVE**

```
Serializes a state into the checksum byte sink. Prefers the project's
field-wise serializer (Serializable scalars/aggregates and SimulationComposite)
for determinism; trivially-copyable POD test types use a raw-byte fallback.
```

---

## 8. The diagnostic views — T52 / RN-1 + RN-2

**`CorrectionCache.h` pre-compression `:429-432` — NARRATIVE**

```
[og-netcode-v2-input-relay T52] The newest-landed-correction query MOVED to
`getDiagnostics().lastCorrectTick()` — see the Diagnostics view near the end
of this class's public section for the full "why it is kept" rationale
(item 45) and the walk-backwards implementation.
```

**`CorrectionCache.h` pre-compression `:610-620` — FENCE**

```
[og-netcode-v2-input-relay T52] The six read-only diagnostic accessors that
used to sit here — the anchor prepare-time capture, the landing-sequence
prepare-time capture, the per-slot landing stamp, the landing counter, the
per-slot state provenance and the trigger-policy read-back — MOVED into the
`Diagnostics` / `MutableDiagnostics` views — see `getDiagnostics()` /
`editDiagnostics()` near the end of this class's public section (RN-1 +
RN-2 resolved design, ReviewNotes.md). The write side of this pair —
`captureResimAnchorForConsume` above and `setResimTriggerPolicy` below —
STAYS here: production calls both. The landing-stamp threading contract
moved down to sit with `m_slotLandingSeqNr`'s declaration in the private
section.
```

**`CorrectionCache.h` pre-compression `:638-640` — FENCE**

```
[T52] The READ side (the trigger-policy read-back) moved into
`getDiagnostics()` — this setter is production-called
(`SimulationReconciliation::setResimTriggerPolicy`) and stays on the cache.
```

**`CorrectionCache.h` pre-compression `:1313-1316` — FENCE**

```
[og-netcode-v2-input-relay T52] DIAGNOSTIC VIEWS — RN-1 + RN-2, RESOLVED
DESIGN (ReviewNotes.md, user rulings 2026-08-13). Full triage and rationale
there; the grouping rule these views follow is centralised in
`docs/DiagnosticsConventions.md` (T53) — do not re-derive it here.
```

**`CorrectionCache.h` pre-compression `:1318-1322` — FENCE**

```
WHAT THESE ARE. Eight accessors that share one property — deleting them and
every caller changes no production behaviour and no shipped telemetry
(RN-2's corrected triage: six read seams whose production reads the
underlying member directly, one logging-only read, one test-only mutator) —
grouped behind two nested views instead of a name-based marker on each:
```

**`CorrectionCache.h` pre-compression `:1324-1325` — NARRATIVE**

```
cache.getDiagnostics().slotLandingSeqNr(i)
cache.editDiagnostics().scribbleStateProvenanceForFenceTest(seed)
```

**`CorrectionCache.h` pre-compression `:1327-1333` — FENCE**

```
`get` / `edit` on the OUTER call is existing house style (`editStorage`,
`editState`, `editResimGateProbe`, `editReconciliation`, `editClientClock`,
…); the members INSIDE a view carry no `get` prefix and no `Diagnostic`
infix of their own — the view's TYPE is the marker, so the name is not
tripled the way `getDiagnostics().getDiagnosticSlotLandingSeqNr(i)` would.
The const/non-const split lands exactly on the triage boundary: seven read
seams on the const view, one test-only mutator on the non-const one.
```

**`CorrectionCache.h` pre-compression `:1335-1344` — FENCE**

```
⛔ THE WRITE SIDE STAYS ON THE CACHE — NEVER GROUP IT. Three of these have a
production write partner that a "move everything related" pass would sweep
in by mistake:
resimTriggerPolicy (read)      moved here | setResimTriggerPolicy        STAYS (SimulationReconciliation, config path)
capturedResimAnchorTick (read) moved here | captureResimAnchorForConsume STAYS (physics thread, production)
capturedLandingSeqNr (read)    moved here | (same capture call)          STAYS
The asymmetry is the point: performing the act is production, observing
the result is diagnostic. `isAnomalousMiss` and `getPendingResimAnchorTick`
are NOT in this view for the same reason — both are production-read (the
former picks a log severity, the latter is the live gate R1).
```

**`CorrectionCache.h` pre-compression `:1346-1349` — FENCE**

```
Nested classes rather than free functions: a nested class is a member of
the enclosing class template and, per the standard, has the same access to
StateCorrectionCache's private members as any other member — no friend
declaration needed, and the view can hold nothing but a reference.
```

**`CorrectionCache.h` pre-compression `:1399-1409` — FENCE**

```
[task 59 RULING] Still 0 production callers / 7 test callers, same
shape as the resim-gate probe accessor task 59 wires a proof for —
but the two are not analogous, and this one is deliberately left as
a test-only seam rather than getting a wiring test of its own. The
resim-gate probe needed one because it is a COUNTER FED BY SEPARATE
WRITE SITES elsewhere, and an accessor alone cannot prove those
sites are connected to it. This method has no such split: it is a
pure derivation over `m_containsCorrectTick`, computed fresh on
every call, with no probe object and no `note*` feeder in between.
The 7 tests exercising it already run the exact same code a
production caller would — there is nothing upstream left to wire.
```

**`CorrectionCache.h` pre-compression `:1610-1614` — FENCE**

```
[og-netcode-v2-input-relay T52] RELOCATED FROM ABOVE the old read accessor
(RN-1 + RN-2 resolved design, ReviewNotes.md): this is the MEMBER's
contract, not the now-relocated accessor's, so it sits here instead of
travelling into `Diagnostics::slotLandingSeqNr`'s definition. The view
leaves a one-line pointer back to this block.
```

**`CorrectionCache.h` pre-compression `:1848-1849` — NARRATIVE**

```
Read seam for tests: `getDiagnostics().frontierSlotAwaitingState()` —
a pure read on the existing const view.
```

---

## 9. The frontier-pair detector — items 84 and 91 part A

**`CorrectionCache.h` pre-compression `:655-661` — FENCE**

```
[og-netcode-v2-input-relay item 84] THE FRONTIER-PAIR DETECTOR — see
m_frontierSlotAwaitingState's declaration (private section below)
for the full contract, its declared non-properties and its honest
coverage statement. Fires iff the PREVIOUS allocation never
received its pushPredictionState — exactly the failure mode B.2
names ("tick pushed, state never pushed"), caught one allocation
late.
```

**`CorrectionCache.h` pre-compression `:702-723` — FENCE**

```
[og-netcode-v2-input-relay item 84 / design §F.2] AND THIS IS WHY THE
ALLOCATION HAPPENS HERE, BEFORE THE UPDATE, RATHER THAN AT CAPTURE
TIME WITH THE STATE IT PAIRS WITH — the fact that forbids what would
otherwise be the obvious structural fix (§B.3):
The frontier slot must be allocated before the update because
`m_tickBuffer` is both the slot directory and the frontier —
`getPredictionTick()` is max over it, so one write serves both —
and the game thread lands corrections against that word
concurrently throughout the update (`tryInsertingCorrectState`'s
lookup, W1's `landedAtFrontier`, the landing classifier).
Allocating after the update would make every mid-update arrival
look up, compare, classify and trigger against the PREVIOUS tick's
frontier for the full width of the physics step — re-timing the
anchor-set predicate the `FrontierExact` baselines are defined
against, and turning mid-update frontier-tick landings into
discards. Allocation timing IS trigger timing; it cannot slide to
capture time, and it cannot be split from the frontier without
creating a second frontier (design §F.4). Four concurrent
game-thread reads decide against this exact word while the window
is open: the slot lookup, the verdict compare, `landedAtFrontier`
feeding `resimGate::shouldSetPendingAnchor` (W1), and
`isAnomalousMiss`'s distance-from-frontier gate on the miss branch.
```

**`CorrectionCache.h` pre-compression `:762-768` — FENCE**

```
[og-netcode-v2-input-relay item 84] COMPLETES the frontier pair —
see m_frontierSlotAwaitingState's declaration. NO CHECK on this
side, deliberately: bare state pushes are legal (first-frame
tick-equality re-entry, wipe renumber, stall re-entry), so clearing
unconditionally is correct here even when no allocation opened this
particular push. The one-sided check lives on pushPredictionTick's
allocation path instead.
```

**`CorrectionCache.h` pre-compression `:1126-1131` — FENCE**

```
[og-netcode-v2-input-relay item 84] THE FRONTIER-PAIR DETECTOR DIES
WITH THE TICK NUMBERING TOO — same discipline as the resim anchor's
W3. This is coverage blind spot (iii) at the member declaration: an
allocation abandoned before this wipe is SWALLOWED, not reported —
deliberate, because a surviving flag would describe a tick this
resync just renumbered away.
```

**`CorrectionCache.h` pre-compression `:1203-1205` — FENCE**

```
[og-netcode-v2-input-relay item 91 part A] COMPLETES THE FRONTIER
PAIR ON THIS PATH TOO — see m_frontierSlotAwaitingState's
declaration (private section, below) for the full contract.
```

**`CorrectionCache.h` pre-compression `:1207-1215` — NARRATIVE**

```
FIXES THE FOURTH, OVER-DETECTION BLIND SPOT. Before this fix,
this existing-slot path fell straight to `m_stateBuffer[slot] =
state;` below without ever touching the bit — functionally
identical to `pushPredictionState` on that path, but a
separately-written one that skipped the clear. A `save_snapshot`
call completing a `pushPredictionTick`-opened pairing therefore
left the bit falsely `true`, arming a spurious `OG_CHECK` crash
at the NEXT legitimate `pushPredictionTick` allocation — on a
cache whose state is entirely correct.
```

**`CorrectionCache.h` pre-compression `:1217-1231` — FENCE**

```
GUARDED on `tick == getPredictionTick()`, deliberately NOT an
unconditional clear like `pushPredictionState`'s own.
`pushPredictionState` always targets the frontier slot by
construction (`getCacheIndex(getPredictionTick())`); this
method's `tick` is caller-supplied, and `getCacheIndex` scans
the WHOLE ring for a value match, so an existing-slot hit can
legitimately name an OLDER resident tick that is NOT the
frontier (see the class's own "at any index" slot-collision
note above). Clearing unconditionally there would swallow a
still-genuinely-open frontier pairing for an unrelated tick —
the same swallowing failure mode blind spot (iii) describes for
`wipeCache`, reintroduced here in a new disguise. Restricting
the clear to the exact frontier tick keeps this a completion of
THE open pairing, never an unrelated one; a no-op when the bit
is already false, same as `pushPredictionState`'s own clear.
```

**`CorrectionCache.h` pre-compression `:1483-1488` — FENCE**

```
[og-netcode-v2-input-relay item 84] Read seam for the frontier-pair
detector — see `m_frontierSlotAwaitingState`'s declaration (private
section above) for the full contract, its declared non-properties
and its honest coverage statement. A pure read; the write side
(`pushPredictionTick` / `pushPredictionState` / `wipeCache`) stays
on the cache — see the fence above this class.
```

**`CorrectionCache.h` pre-compression `:1769-1772` — FENCE**

```
[og-netcode-v2-input-relay item 84] THE FRONTIER-PAIR DETECTOR —
PHYSICS-THREAD-PRIVATE. Catches "tick pushed, state never pushed", one
allocation late. Full derivation: DesignInputResolutionPeer.md §B.4.2,
review B-2/B-3, Backlog item 84.
```

**`CorrectionCache.h` pre-compression `:1774-1783` — FENCE**

```
Set `true` by `pushPredictionTick` on its SLOT-ALLOCATION path only
(after the tick==frontier early-return), guarded there by
`OG_CHECK(!m_frontierSlotAwaitingState, ...)` — a second allocation
while the previous one is still awaiting its state push is exactly the
defect this bit exists to catch. Cleared by `pushPredictionState` WITH
NO CHECK — bare state pushes are legal at the first-frame
tick-equality re-entry, the wipe renumber, and stall re-entry, so
clearing unconditionally is correct even when no allocation opened
this particular push. Reset by `wipeCache` (dies with the tick
numbering, same discipline as the resim anchor's W3).
```

**`CorrectionCache.h` pre-compression `:1807-1810` — FENCE**

```
BUILD REACH — development-time detector only, and it cannot be
otherwise: standalone `OG_CHECK` is a bare `assert` (gone under
`NDEBUG`); UE-side it is `checkf` (gone in Shipping). It never fires
in a shipping build.
```

**`CorrectionCache.h` pre-compression `:1812-1846` — FENCE**

```
COVERAGE — honestly bounded, not "cannot fail silently" (review B-2
corrects the design's original honesty line; item 91 part A corrects it
a SECOND time — the line below used to say "THREE blind spots", all
UNDER-detection, and that was itself an overstatement of what the
detector catches). This catches exactly ONE failure mode, one
allocation late, and has FOUR known limits across its history, split by
DIRECTION — three are live UNDER-detection blind spots (a violation
exists and is never reported); the fourth WAS a live OVER-detection
false positive (a crash fires on a cache whose state is entirely
correct) and is now FIXED, kept here as history so nobody "simplifies"
the fix back out:
UNDER-DETECTION, still live:
(i) a violation on the FINAL TICK before teardown — no later
allocation exists to catch it;
(ii) the ENTIRE REVERSE DIRECTION — state pushed without a paired
allocation is legal at the cache (see the no-check note
above) and is covered ONLY by the shared predicate
(`stepAllocatesFrontierSlot`), never by this bit;
(iii) a WIPE interposed between an abandoned allocation and the
next allocation SWALLOWS the violation — `wipeCache` resets
the bit by design (it dies with the tick numbering), so the
abandoned allocation is never reported.
OVER-DETECTION, fixed [item 91 part A]:
(iv) `save_snapshot`'s existing-slot path used to never touch this
bit at all — functionally identical to `pushPredictionState` on
that path, but a separately-written one that skipped the clear.
A `save_snapshot` call completing a `pushPredictionTick`-opened
pairing left the bit falsely `true`, arming a spurious
`OG_CHECK` crash at the NEXT legitimate allocation, on a cache
whose state was entirely correct — the more damaging direction,
since it fires on CORRECT state rather than staying silent on
BROKEN state. Closed by the guarded clear in `save_snapshot`
(see that method); pinned RED-then-GREEN by
`FrontierPairContractTest.cpp`'s
`SaveSnapshotOnAnExistingSlotClearsTheAwaitingBit` case.
```

---

## 10. The correction verdict and the applied-capture ref — T24 / T4 / T6

**`CorrectionCache.h` pre-compression `:108-110` — NARRATIVE**

```
[og-netcode-v2-input-relay T24] CorrectionInsertVerdict — what
`tryInsertingCorrectState` DECIDED, handed back to a caller that knows WHO it
decided it about.
```

**`CorrectionCache.h` pre-compression `:112-115` — FENCE**

```
THIS ADDS NO COMPUTATION. `predictionWasCorrect` is the existing
`m_stateBuffer[cacheIndex].isSimilarTo(state)` verdict, verbatim, already
computed on every correction and already stored in `m_predictionWasCorrect`.
The only thing that changes is that it can now leave the function.
```

**`CorrectionCache.h` pre-compression `:117-131` — FENCE**

```
WHY IT LEAVES AS AN OUT-POINTER RATHER THAN A RETURN VALUE OR A LOG LINE.
* The cache is deliberately ID-AGNOSTIC — it holds one character's ticks and
has never known which character it belongs to. The T24 acceptance criterion
needs the verdict attributed by `id` AND by class (remote proxy vs locally
predicted, decided by provider-presence), and NEITHER of those facts exists
in this class or could be handed to it without giving it an identity it has
no other use for. So the verdict has to travel up to the site that already
knows both, which is the OnRep-bound correction callback in
`SimulationNetSync::registerPredictionOwner`.
* A DEFAULTED out-pointer leaves every existing caller — the four production
paths and every `[CorrectionCache]` / `[AppliedCaptureTickSlot]` case —
byte-identical, which is what makes this task behaviour-neutral by
construction rather than by inspection. Same trick T19 used to widen
`resolveScheduledRelayedInput` without touching a single existing call
site, and the same shape `find()` already uses locally.
```

**`CorrectionCache.h` pre-compression `:133-136` — FENCE**

```
The cache's own existing log line is UNCHANGED and stays where it is. It is
untagged, so it still falls to the LogOG fallback and is still suppressed at
the shipped default — retagging it would have moved a line that cannot carry
an id anyway, and would have perturbed the log-gating cases that count it.
```

**`CorrectionCache.h` pre-compression `:140-143` — FENCE**

```
False when the correction's tick had no slot in the cache window and the
correction was DISCARDED. No comparison happened, so `predictionWasCorrect`
is meaningless — a discarded correction must never be counted as either an
agreement or a disagreement, because it produced neither.
```

**`CorrectionCache.h` pre-compression `:146-146` — FENCE**

```
The existing `isSimilarTo` verdict. Meaningful only when `landed`.
```

**`CorrectionCache.h` pre-compression `:149-150` — FENCE**

```
The correction's tick, echoed back so the caller's log line needs no second
decode of the wire buffer. Set on BOTH paths.
```

**`CorrectionCache.h` pre-compression `:286-286` — NARRATIVE**

```
[og-netcode-v2-input-relay T4 / design D3] THE PER-TICK JOIN KEY.
```

**`CorrectionCache.h` pre-compression `:288-294` — FENCE**

```
The capture tick of the input the AUTHORITY applied at this slot's tick, as
carried by the correction that landed here (correctionStateBufferCodec's
second wire field), or kNoInputCaptureTick when no ref is known — which
covers three distinct situations that all mean the same thing to a reader:
the slot has never received a correction; the correction said the authority
substituted an input (RemoteMoveQueue underrun, the D1 sentinel); or the
slot was recycled by the prediction ring and its old ref retired with it.
```

**`CorrectionCache.h` pre-compression `:296-300` — FENCE**

```
WHY IT LIVES IN THE SLOT and not in one scalar per character: T6 resolves
remote input for EVERY tick it resimulates, so it needs the ref that
belonged to each of those ticks, not the newest one. It is stored parallel
to the state it corrects — slot T's ref describes what the authority applied
AT tick T, with no offset, which is what lets resim read it straight.
```

**`CorrectionCache.h` pre-compression `:315-322` — FENCE**

```
WHY T6 NEEDS IT, given getAppliedCaptureTick already exists. The sentinel is
AMBIGUOUS on its own: `kNoInputCaptureTick` in a slot means either "a
correction landed and the authority named no capture" (the D1 underrun — T6
resolves that to the game zero) or "no correction has landed here at all"
(a prediction-frontier tick, or a tick whose correction was never replicated
because the net update rate is below the sim rate — T6 must RE-DERIVE those,
not zero them). Only this bit separates the two, and getting it wrong would
replay game-zero over roughly every uncorrected resim tick.
```

**`CorrectionCache.h` pre-compression `:931-935` — FENCE**

```
`appliedCaptureTick` is the correction's per-tick join key (T4): the capture
tick of the input the authority applied when it produced THIS state. It
defaults to the sentinel so every pre-T4 call site (and any caller with no
wire ref to hand, e.g. the test harnesses) keeps its exact prior meaning —
"no ref known for this tick".
```

**`CorrectionCache.h` pre-compression `:937-939` — FENCE**

```
It is stored on the HIT path regardless of predictionWasCorrect: the ref
describes what the AUTHORITY did, which is equally true whether or not the
local prediction happened to match, and T6 needs it in both cases.
```

**`CorrectionCache.h` pre-compression `:941-945` — FENCE**

```
[T24] `outDiagnosticVerdict` is a DEFAULTED, PURELY OBSERVATIONAL out-parameter: it
reports the `isSimilarTo` verdict this method has always computed, so a
caller that knows the character id and its class can attribute it. Nothing
on this method's behaviour depends on whether it is supplied. See
CorrectionInsertVerdict at the top of this file.
```

**`CorrectionCache.h` pre-compression `:1030-1032` — NARRATIVE**

```
[T24] Reported BEFORE the state move below, for no reason other than
that the verdict is about the state as it was compared; the flag is a
bool copy and the ordering is not load-bearing.
```

**`CorrectionCache.h` pre-compression `:1119-1121` — FENCE**

```
[T4] Every surviving join key describes a tick numbering that the resync
just invalidated — same reasoning as the delay-line clear in
SimulationNetSync::wipeAllForResync.
```

**`CorrectionCache.h` pre-compression `:1601-1603` — FENCE**

```
[T4 / D3] Per-slot join key — see getAppliedCaptureTick. Parallel to
m_tickBuffer: index i answers "which capture tick did the authority apply at
m_tickBuffer[i]". kNoInputCaptureTick everywhere it is unknown.
```

---

## 11. Keeping this document true

Every section body is a **closed record**: the text a header comment carried before task 13
compressed it, dated 2026-08-22. It is not a claim about what the code does today — the
header is. That is deliberate, and it is what makes the bulk of this file un-rottable: a
record of what was written cannot become false, only less relevant.

⚠ **What it does NOT protect is the symbols those records name.** A rename lands here as a
stale identifier in a quoted block. The retired names declared above are for
`tools/lint/doc_anchor_lint.ps1`; a new retirement needs a new declaration in the same change.

⛔ **A clean lint run is not evidence that anything here is TRUE.** That checker resolves
names, and it has been demonstrated to return an identical clean verdict for a sentence and
for its negation — and F-T13-2 is a live instance: three of the quoted blocks name a wrong
owner, every symbol in them resolves, and no configuration of that lint can see it. Read the
header, not the lint result.

---

## 12. Archive provenance — the labels the header no longer carries

The header used to prefix its guards with archive tags — `[item 45]`, `[T16]`,
`[item 84 / design §F.2]`, `[task 59 RULING]`. **A tag is provenance, not a guard.** It
resolves only inside the `og-netcode-v2-input-relay` workspace, and this subtree is
consumed standalone, so to a reader with no workspace those tags named nothing at all.
They were removed from the header; every fact they labelled stayed at its own line, and
every one of them is recorded here.

⛔ **This is a MOVE, and the table is what makes it checkable.** If a tag below names a
record you cannot find in the section given, the move lost something — say so rather
than re-adding the tag to the header.

| archive tag | what it labelled | where the record is |
|---|---|---|
| `item 45` | the resim gate: the retired `m_isResimulated` bitset, the one-atomic-word rule, the W1/W2/W3 write roster, the gravestone | §1, §2 |
| `item 47` | the landing stamps `m_slotLandingSeqNr` / `m_landingSeqNr` and the protect-all rule a replay must not overwrite a corrected slot | §3 |
| `item 48` | the `m_stateProvenance` column, its five write sites, and its stated non-goal | §4 |
| `item 42` | the observational `bool` return of `tryInsertingResimulatedState`, and the no-atomics rule that governs the probes | §3, §5 |
| `item 81` | the honest bound on the invariant — a lost update of a decision bit, not a torn read | §5 |
| `item 84`, `item 91 part A`, `design §F.2` | the frontier-pair detector, its four known limits, and the over-detection fix in `save_snapshot` | §9 |
| `items 84-90` | the extraction of `SimulationInputResolution` out of `SimulationNetSync`, which moved `LocalInputCache`, `RemoteInputCache` and both readers | §1, §10 |
| `items 73 and 74` | the deferred comparison and wire layers — why the 4-method API has no production consumer | §7 |
| `T4`, `design D3` | the per-tick join key `m_appliedCaptureTickBuffer` and the `kNoInputCaptureTick` sentinel | §10 |
| `D1` | the underrun sentinel: the authority substituted an input because the remote input queue underran | §10 |
| `T6` | resim input resolution, the reader that consumes the join key | §10 |
| `T8` | the retirement of the SERVER→CLIENT correction-input channel | §1 |
| `T15` | the application-tick-vs-capture-tick ambiguity the input column re-creates if re-added | §1 |
| `T16` | the retirement of the input column `m_inputBuffer` and its three accessors | §1 |
| `T19` | the defaulted out-pointer trick, and the log-volume defect a per-protection log line would re-create | §3, §10 |
| `T24` | `CorrectionInsertVerdict` and the `outDiagnosticVerdict` out-pointer | §10 |
| `T34` | the retune that falsified the correction-cadence number | §6 |
| `T39` | the old cost model, false and not to be re-adopted | §6 |
| `T52`, `RN-1`, `RN-2` | the diagnostic views and the grouping rule they follow | §8 |
| `Stage 5 / Task 18` | the correction-miss log gate | §6 |
| `task 59 RULING` | why `lastCorrectTick` keeps a test-only seam with 0 production callers | §8 |

⚠ **The tags survive inside the quoted blocks below, and that is correct.** Those blocks
are a verbatim record of what the header said before compression; repairing them would
stop them being a record. What changed is the **header**, which now names symbols and
roles a standalone reader can resolve.
