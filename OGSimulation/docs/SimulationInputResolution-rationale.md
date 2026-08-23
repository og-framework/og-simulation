<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationInputResolution.h` — relocation history, retired rationale and
# archived measurement records

**This document is CUSTODY, not delivery.** It is where the prose task 14 removed
from `SimulationInputResolution.h` is archived so the removal is reversible and
non-lossy. The header is where the guards live and is the thing to read; nothing
here is a live claim and nothing here should be cited as one.

⛔ **GENERATED, NOT WRITTEN** by a script in the initiative workspace. Every block
below is extracted from the pre-compression header by LINE RANGE, so item 86's
defect class — *two comments silently edited during a move that promised
verbatim* — is structurally impossible here rather than promised. Only the
section headings and §15 are authored.

⚠ **The quoted text is a CLOSED-TENSE ARTIFACT: it is what the header said on
2026-08-22, before task 14.** Where it contradicts the shipped header, the header
wins. §15 lists the three places task 14 knew that to be the case.

<!-- lint-external-ref: m_lastUsedInputs -- retired at T8 with the correction-INPUT channel; the archive quotes the text that retired it -->
<!-- lint-external-ref: LastUsedInputMapFor -- retired at T8, same removal -->
<!-- lint-external-ref: prepareSimulationStep -- item 90's name for collectInputAll; item 94 reverted it, and the revert is what the quoted text records -->
<!-- lint-external-ref: getLastCorrectionInput -- retired accessor, quoted as the known trap it left behind -->
<!-- lint-external-ref: reportDelta -- T20's local, gone in RN-8's decide/project split -->
<!-- lint-external-ref: Reconciliation::pushPredictionTick -- the frontier banner's parked home before item 94 moved it -->
<!-- lint-external-ref: remoteQueue -- a captured reference the by-id doors replaced at item 86 -->
<!-- lint-external-ref: pushPredictionInput -- retired at T16 with the correction cache's input column -->
<!-- lint-external-ref: currentEffectiveDelay -- prose shorthand inside a formula, never a symbol -->

⚠ **One adapter binding the quoted text names.** `og-simulation` is engine-free — it names no
game-engine type and is reached from a host engine only through `concept`s. One quoted block names a
host-engine type, `USimmableUpdateComponent`, together with a **tier-arrival replication callback**.
The role is what matters and it is engine-neutral: *the game-thread writer of the
effective-input-delay atomic, whose physics-thread reader is `collectInputAll` — one adapter reaches
`setClientEffectiveInputDelayTicks` from a replication notification, and another substitutes its own
arrival signal.* The crossing is the fact; the type is one adapter's binding for the writing side.

⛔ **F-28-1 — that quotation's QUALIFICATION is false, and it is preserved false.** In the tree as
read on 2026-08-22 the tier callback `OnRep_ConnectionTier` is declared on
`Source/OGSimulationUnreal/SimulationConnectionRelay.h`, on the **per-connection relay actor** — not
on the per-character component, which only *sends* the tier server-side through
`sendConnectionTierToOwningClient`. The write reaches the core through the composition root's
`publishClientEffectiveInputDelayTicks` passthrough. The quotation is not repaired, per this file's
archive rule; this note is the correction, and the same wrong attribution survives in
`Network/ReplicatedTierConsumer.h`, which no task in this wave owns.

Source: `SimulationInputResolution.h` at `og-simulation` `a8c03a8` + task 15's
inbound pointer. The byte-for-byte snapshot those line numbers address is kept
in the initiative workspace as SimulationInputResolution.BEFORE.h.

⚠ **Line numbers here address that snapshot, NOT the shipped header.** They are
deliberately not written as `File.h:N` anchors: the first run of this generator
did write them that way and the doc-anchor lint immediately reported 24 of them
as out of range -- a broken join manufactured by the extraction itself, which is
exactly the defect class the lint exists for.

The nine escapes declared at the top of this file are RETIRED NAMES. A closed-tense
archive quoting a symbol that no longer exists is correct precisely because the
name does not resolve; each is declared with its reason so it cannot silence a
real hit later.

---

## Contents

- [§1 — The migration record -- items 86, 87, 89, 90, 94, 96](#1--the-migration-record--items-86-87-89-90-94-96)
- [§2 — The provider contract, the delay line, and the local/remote test](#2--the-provider-contract-the-delay-line-and-the-localremote-test)
- [§3 — The retired maps, the wire sentinel, and T2's join key](#3--the-retired-maps-the-wire-sentinel-and-t2s-join-key)
- [§4 — The scheduled-read ladder (T6/T7)](#4--the-scheduled-read-ladder-t6t7)
- [§5 — RN-8's decide/project split and the diagnostic projection](#5--rn-8s-decideproject-split-and-the-diagnostic-projection)
- [§6 — Neutral injection (T17) and the (0,0,0) poison](#6--neutral-injection-t17-and-the-000-poison)
- [§7 — Registration, unregistration and the by-id doors](#7--registration-unregistration-and-the-by-id-doors)
- [§8 — The frontier pair: item 90's fusion, item 94's split, item 96's facade](#8--the-frontier-pair:-item-90s-fusion-item-94s-split-item-96s-facade)
- [§9 — The resim resolution table (T6)](#9--the-resim-resolution-table-t6)
- [§10 — Resync, and the wipe asymmetry](#10--resync-and-the-wipe-asymmetry)
- [§11 — Per-character collect: the capture/applied split and the gates](#11--per-character-collect:-the-captureapplied-split-and-the-gates)
- [§12 — The simulated-proxy branch and the emergent regime](#12--the-simulated-proxy-branch-and-the-emergent-regime)
- [§13 — What left for the telemetry sibling, and the retired sweep](#13--what-left-for-the-telemetry-sibling-and-the-retired-sweep)
- [§14 — Members, threading, and the two-thread roster](#14--members-threading-and-the-two-thread-roster)
- [§15 — Corrections landed by task 14](#15--corrections-landed-by-task-14)

---

## §1 — The migration record -- items 86, 87, 89, 90, 94, 96

How this class came to exist, what moved verbatim, what was deleted, and the prose debt item 89 still owes.

**Archived from the pre-task-14 header, lines 28-85.**

```
---------------------------------------------------------------------------
[og-netcode-v2-input-relay item 86 / step 2, PROMOTED item 87 / step 3 of
the input-resolution migration] SimulationInputResolution — the resolution
peer DesignInputResolutionPeer.md names throughout as "the resolution
peer".

STEP 2 MOVED THIS VERBATIM, NOT A TRIM (design §F). Every member below —
the type aliases, the ladder free functions, the five container families,
the provider map, the join-key map, the delay atomic, the collect paths
and their per-character helpers, `wipeAllForResync` — moved out of
`SimulationNetSync.h` UNCHANGED, comments included. Where a comment still
says "SimulationNetSync" it is because the comment was not reworded on the
way over; prose correction is item 89's job (design §C.8's own filter),
not this one's. The two deliberate exceptions, both load-bearing seam
edits rather than tidying, are called out where they occur: the
frontier-pair contract banner above `collectInputAll` (relocated from its
parked home at `Reconciliation::pushPredictionTick`'s header, design
§B.4.3(1); item 90 re-relocated it again, to `prepareSimulationStep`, its
then-name — and item 94 moved the banner a THIRD time, off this class
entirely, to Reconciliation's own frontier-allocating sweep, once
frontier allocation itself moved there; the method's name reverts to
`collectInputAll`, item 90's rename reversed on the record — see the
method's own banner for why), and the registration/lifecycle methods
below, which are NEW — they did not exist as such on `SimulationNetSync`
(see design §C.5); the container-lifecycle HALF of what those methods
used to do moved here verbatim as the body of each.

⭐ [item 87] PROMOTED — NO LONGER SCAFFOLDING. This peer is now constructed
directly at the composition root (design §A.3's intended final wiring:
storage → reconciliation → inputResolution(storage, reconciliation) →
netSync(storage, reconciliation, inputResolution) → manager), handed to
`SimulationManager` as its own peer (the manager's seventh template
parameter), and referenced — not owned — by `SimulationNetSync` for the
by-id doors its own transport methods still call. Every forwarder that
used to reproduce this class's surface on `SimulationNetSync` is deleted;
callers reach this peer directly.

DEPENDENCY SPINE (design §A.3): this class knows `SimulationObjectStorage`
and `SimulationReconciliation` ONLY — never `SimulationNetSync`. NetSync
knows both this peer and Reconciliation; this peer and Reconciliation do
not know NetSync exists. "No peer owns another" (design §A.3) is now a
true statement rather than the one-step exception step 2 accepted.

[item 94] THE RECONCILIATION EDGE JUST SHRANK TO ONE READ. Frontier
allocation (`pushPredictionTick` / `backfillSkippedTick`) moved to
Reconciliation's own frontier-allocating sweep — this class calls
neither any more (grep-checked: zero `pushPredictionTick` /
`backfillSkippedTick` call sites remain below). The sole surviving reason
this class still holds `m_reconciliation` is `collectResimInputAll`'s
`getAppliedCaptureTickRef` — the resim join-key read (T6/D3) — the one
query this peer cannot answer from its own containers.

Where this peer's two resolution paths sit in the whole path a captured
input travels, end to end: `docs/Perspective-RemoteInputFlow.md` — §6
("B's half — the scheduled read, and what happens when nothing arrived")
is this class's own ladder, §9 ("The resim mirror — why the same ladder
is called twice") its resim twin.
---------------------------------------------------------------------------
```

**Archived from the pre-task-14 header, lines 87-89.**

```
---------------------------------------------------------------------------
Per-type map aliases for SimulationNetSync members
---------------------------------------------------------------------------
```

**Archived from the pre-task-14 header, lines 513-524.**

```
---------------------------------------------------------------------------
SimulationInputResolution<SimulatableTs...>

Owns the five per-id input container families (local caches, remote caches,
remote-move queues, pending-input queues, neutrals), the provider map and
its identity test, the join-key map, the client delay atomic, both collect
paths and the ladder they share with resim. Holds refs to
SimulationObjectStorage and SimulationReconciliation — never to
SimulationNetSync (design §A.3: this peer knows Reconciliation only).

Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
---------------------------------------------------------------------------
```

**Archived from the pre-task-14 header, lines 539-547.**

```
[item 86, re-targeted item 87] This peer's own copy of the
split-telemetry logger. Through item 86 `SimulationNetSync::
setLogger` called this for us (it owned the scaffold member); as of
item 87 this peer is a composition-root-constructed sibling, not a
sub-object, so the composition root calls this directly — the same
pattern it already uses for `m_reconciliation.setLogger` and
`m_netSync.setLogger` (task 79/85's "each sibling gets its own
copy" rule, now applied at the composition root rather than via a
forwarding hop).
```

**Archived from the pre-task-14 header, lines 731-744.**

```
=======================================================================
[item 86] DIAGNOSTIC VIEW — the one probe this peer owns.

`RelayReadProbe` moved here with its owner (`InputResolutionTelemetry`,
item 85) and is exposed the same way `SimulationNetSync::Diagnostics`
exposed it before this peer existed: one const accessor, on a nested
view class, delegating into the telemetry sibling's own const accessor.
`SimulationNetSync::Diagnostics::relayReadProbe()` is GONE, not
delegating — item 87 removed the two-hop forwarder; every caller reaches
this view directly at `inputResolution.getDiagnostics().relayReadProbe()`
(design §C.6). Const-only for the same reason NetSync's view is: the
only writer is the telemetry sibling, reachable from exactly one thread
(see InputResolutionTelemetry.h's own two-thread banner).
=======================================================================
```

**Archived from the pre-task-14 header, lines 759-772.**

```
-----------------------------------------------------------------------
[item 86 / design §C.5] Registration and unregistration — the
CONTAINER-LIFECYCLE half of what `SimulationNetSync::registerPredictionOwner`
/ `registerAuthorityOwner` / `unregisterSimulatable` used to do in one
method each. NetSync keeps the BINDING half (owner pointer structs,
callback registration) and calls these in a fixed sequence — see
SimulationNetSync.h for the call sites and the cross-peer set-invariant
comment written at both of them (design §C.4's closing paragraph).

These four methods are NEW — they did not exist as such before this
item — but every line inside each is a verbatim-moved fragment of the
pre-cut registerPredictionOwner / registerAuthorityOwner /
unregisterSimulatable bodies; nothing here is a re-derivation.
-----------------------------------------------------------------------
```

**Archived from the pre-task-14 header, lines 1848-1866.**

```
---------------------------------------------------------------------------
[item 87 / design §C.7] SimulationInputResolutionConcept

The SimulatableTs-exact peer concept (mirrors SimulationNetSyncConcept's
own shape and its home-beside-the-class placement, item 83's recorded
preference). Checks exactly the three members design §C.7 names as having
LEFT `SimulationNetSyncConcept` for this new concept: `collectInputAll`
(RENAMED `prepareSimulationStep` at item 90 — same slot, same signature,
same return-type pin — and RENAMED BACK to `collectInputAll` at item 94,
once allocation left the method entirely; see that method's own banner for
why item 90's naming argument now argues for the reversal rather than
against it), `collectResimInputAll` (the SimulatableTs-typed return pins,
unchanged from their pre-move form) and `wipeAllForResync`. This is the
ad-hoc, concrete-SimulatableTs check used adapter-side (task 6's
precedent, SimulationManagerUImplConceptTest.cpp) — the manager-facing,
pack-invisible split lives in SimulationManager.h as
`SimulationInputResolutionTickConcept` (item 80/83's SimulatableTs-invisibility
reasoning, inherited here).
---------------------------------------------------------------------------
```

---

## §2 — The provider contract, the delay line, and the local/remote test

Why the provider takes the line as a parameter, why provider-presence is the identity test, and why couch co-op is what rules out a role check.

**Archived from the pre-task-14 header, lines 91-109.**

```
[T15 / input relay] THE PROVIDER TAKES THE DELAY LINE AS A PARAMETER.

The second argument is the character's own LocalInputCache — its raw
capture history, keyed by capture tick. It is here because the motion-sequence
matcher (the game's Hadouken detector, which runs INSIDE the provider) needs
contiguous raw captures up to `tick - 1`, and passing them in is what makes
the read ordering statable in one place rather than as a cross-file contract.

THE ORDERING CONTRACT, AND WHY IT IS NOW VISIBLE. collectInputAll
looks the line up and calls the provider BEFORE it pushes this
tick's capture. So the
line a provider receives holds ticks <= `step.getTick() - 1`, never the
current tick, and the current tick's sample is the value the provider is in
the middle of producing. Both halves of that statement are now in ONE
function; before, the provider reached back for the history itself and nothing
at either site said it must not observe the current tick — it held only
because one line happened to precede another.

The reference is const: a provider reads history, it never writes it.
```

**Archived from the pre-task-14 header, lines 169-174.**

```
[T9 parts 3+4] Per-locally-controlled-simulatable ring of the client's own raw
input captures. Populated ONLY for ids that have an input provider, i.e. the
exact set for which m_pendingInputQueues is populated. See
Network/LocalInputCache.h for why this is a separate structure from the
correction cache — the short version is that the cache's slot T already means
"input APPLIED at tick T" and resim reads it with no offset.
```

**Archived from the pre-task-14 header, lines 180-193.**

```
[T5 / input relay] Per-REMOTE-simulatable store of the inputs the server
relayed for that character, keyed by the SENDER's capture tick.

THE EXACT COMPLEMENT of LocalInputCacheMapFor above: that map is populated
for the ids that HAVE an input provider (locally controlled), this one for the
ids that do NOT (remote proxies). Provider-presence is the local-vs-remote test
throughout this file, and it is the one that stays correct under COUCH CO-OP,
where a single client legitimately owns several locally-controlled characters —
any test based on "the client's character" would pick exactly one of them and
hand the others a relay store they must not have.

NOT wiped by wipeAllForResync — see the deliberate non-wipe note there, and the
naming ruling in Network/RemoteInputCache.h for why this is its own type rather
than a second LocalInputCache.
```

**Archived from the pre-task-14 header, lines 774-778.**

```
The provider-present half of the old registerPredictionOwner: creates
the provider entry, the pending-input queue, and the neutral-seeded
local delay line. Keep m_inputProviders / m_pendingInputQueues in sync
with NetSync's m_localInputSenders — see the set-invariant comment at
the call site.
```

**Archived from the pre-task-14 header, lines 794-798.**

```
[T9 part 3] The delay line is populated for exactly the ids that
have a provider — the locally-controlled ones. A remote proxy has
no capture of its own to delay, so it must not get a line —
collectInputAll's proxy branch reads no delay line
at all (post-T7 it reads that character's relay store instead).
```

**Archived from the pre-task-14 header, lines 804-810.**

```
The provider-absent half of the old registerPredictionOwner: creates
the neutral-seeded relay store. [T5] The exact complement of
registerLocalCharacter above — a locally-controlled character must NOT
get a relay store; its inputs come from its own provider, and the
server does not relay a character's input back to the client that
produced it. NetSync performs the callback bind and the bind-time
catch-up read (via `ingestRelayRing`, below) after calling this.
```

**Archived from the pre-task-14 header, lines 936-941.**

```
[item 86] THE identity test (design §C.1/§A.2), replacing the direct
`m_inputProviders...count(id)` read `decideCorrectionArrival` used to
make on NetSync. "is this id locally controlled" is precisely "who
provides this id's input" — an input-resolution fact; the provider-map
mechanism stays private so a second, cheaper notion of "remote" cannot
grow anywhere else.
```

**Archived from the pre-task-14 header, lines 1318-1336.**

```
[T15] HOISTED ABOVE THE PROVIDER CALL — this used to be looked
up further down, right before the delayed read.

The line is passed INTO the provider because the provider runs
the game's motion-sequence matcher, which needs raw capture
history. Reading it here and handing it over puts two facts
side by side that used to live in different files:

  * the provider sees history up to `tick - 1` ONLY — the push
    below is the next statement it does not get to observe;
  * the current tick's sample is the provider's own return
    value, so it never needs to look for it.

`.at(id)` rather than a nullable lookup on purpose: the line is
created iff a provider is registered (see
registerPredictionOwner), so provider-present and line-present
are the same condition by construction. A throw here would mean
that invariant had been broken, which is exactly when a silent
fallback would be wrong.
```

**Archived from the pre-task-14 header, lines 1677-1680.**

```
`.at(id)` for the same reason `collectInputAll`'s
local-provider branch uses it: the line is created iff a
provider is registered, so provider-present and line-present
are the same condition by construction.
```

---

## §3 — The retired maps, the wire sentinel, and T2's join key

What T8 removed, what stayed, and why the two are easy to confuse.

**Archived from the pre-task-14 header, lines 126-137.**

```
[og-netcode-v2-input-relay T8] `LastUsedInputMapFor` / `m_lastUsedInputs` — the
authority's per-id record of "the input I applied on my most recent tick" — ARE
GONE. That record existed for exactly one purpose: sendCorrectionAll replicated
it on the correction-INPUT channel. With that write removed it had zero readers
and was pure write-only bookkeeping on the server's hot path (a hash lookup and
a composite copy per character per authority tick, feeding nothing).

DO NOT CONFUSE IT WITH `LastUsedCaptureTickMapFor` / `m_lastUsedCaptureTicks`
BELOW, WHICH STAYS. That one shares the retired map's key set and its
population/erase sites, but it serves T2's applied-capture-tick REF — the join
key the relay is built on, published in the correction STATE payload and
consumed by T6's resim resolution. It is load-bearing.
```

**Archived from the pre-task-14 header, lines 139-146.**

```
---------------------------------------------------------------------------
[T2 / input relay] kNoInputCaptureTick — the "no real input was applied"
sentinel for the applied-capture-tick track below — now lives in
OGSimulation/CorrectionStateBufferCodec.h (included above), because T4 made it
a WIRE value shared by three layers that must not depend on this header: the
correction-state payload, the correction cache's per-slot ref, and the UE sync
buffer. Same name, same value, same meaning; the rationale moved with it.
---------------------------------------------------------------------------
```

**Archived from the pre-task-14 header, lines 148-162.**

```
[T2 / input relay] Per-authority-id record of the capture tick behind the
input the authority APPLIED this tick — the join key the relay is built on.
It used to mirror LastUsedInputMapFor (same key set, same population/erase
sites, same threading); **[T8] that twin is retired and this is now the only
per-id authority track**, populated at registerAuthorityOwner and written in
collectInputAll's remote branch. See the member declaration
for threading.

WHY A STRUCT AND NOT A BARE ALIAS. The mapped type (uint32) does not depend on
T, so a plain `using ... = std::unordered_map<unsigned int, uint32>` would give
EVERY member of the pack the identical tuple slot type and make the
`std::get<LastUsedCaptureTickMapFor<T>>` lookups used below ill-formed the
moment a second simulatable type is registered. Wrapping keyed on T gives
distinct slots — the same reason NeutralInputFor is a struct, except that here
the collision is guaranteed rather than merely possible.
```

**Archived from the pre-task-14 header, lines 819-823.**

```
The container-lifecycle half of the old registerAuthorityOwner: the
remote-move queue and the join-key sentinel entry. [T2] The initial
join-key value is the SENTINEL, not 0: before the first authority tick
this id has applied no input at all, which is exactly what the
sentinel means. Seeding 0 would claim capture tick 0 was applied.
```

**Archived from the pre-task-14 header, lines 848-848.**

```
[T2] Erased here, populated in registerAuthorityCharacter.
```

**Archived from the pre-task-14 header, lines 948-959.**

```
-----------------------------------------------------------------------
[T2 / input relay] Applied-capture-tick accessor
-----------------------------------------------------------------------

The capture tick behind the input the authority applied for `id` on its most
recent tick, or kNoInputCaptureTick if that input was a substitute (queue
underrun) or the authority has not ticked this id yet. T4 attaches this to
the correction state; the tests observe it here.

Returns the sentinel rather than throwing for an unknown id: a caller asking
about an id the authority does not own is in exactly the "no real input
applied" situation the sentinel describes.
```

**Archived from the pre-task-14 header, lines 1441-1452.**

```
[T2] The relay's join key: the ORIGINAL capture tick of the input
just applied. move.tick is that original capture tick: the drain
(ServerReceptionCoordinator::releaseDelayedInputs) delivers the
entry's STORED captureTick rather than a reconstructed
`simTick - delay`, so it survives an overdue release intact.
The sentinel replaces it when the input was a substitute and no
client capture stands behind it.

[T17] UNCHANGED by the substitution below — the ref stays the
sentinel on an underrun, which remains exactly true: no client
capture stands behind the game's zero input either. T17 fixes
WHICH input is substituted, not how it is classified.
```

---

## §4 — The scheduled-read ladder (T6/T7)

The three rungs, the verify step, and the emergent-regime property.

**Archived from the pre-task-14 header, lines 199-239.**

```
---------------------------------------------------------------------------
[T6] resolveScheduledRelayedInput — THE UNIFIED SCHEDULED READ, in one place.

The remote-proxy answer to "which relayed input does tick N run on, when no
authoritative ref is available for N". Spelled out as a three-step ladder in
Backlog T7 and RelayDelaySpectrumDesign.md §4/§5.2:

  0. nothing has ever arrived (`!findLatest().valid`) -> TERMINAL FALLBACK, and
     specifically NO PROBE AT ALL. `find(N)` can genuinely hit for a LAN peer,
     so probing with a default dA of 0 would be accidentally-safe-because-a-
     later-check-catches-it rather than correct (RemoteInputCache.h's
     initial-state contract states and requires this skip).
  1. probe `find(N - dLatest)`;
  2. VERIFY the candidate's own stamp equals `dLatest`. This means "the delay
     REGIME has not shifted under me", NOT "this entry is individually
     scheduled at N": the authority applies ONE current delay to every parked
     entry (its drain reads `effectiveDelay` at drain time), so the freshest
     stamp is the best estimate of that delay, and a per-entry-stamp scan would
     faithfully reproduce a schedule the server does not use. On mismatch the
     schedule is in transition — fall back, never guess.
  3. hit -> the candidate's input; miss or verify-fail -> `fallback()`, the
     newest arrived input or the INJECTED game zero if nothing ever arrived.

EMERGENT REGIME, no flag: at floor 0 the probe nearly always misses and this
degenerates to last-known (the pre-relay behaviour); at a high floor it nearly
always hits and the proxy consumes the server's actual schedule. Same code.

WHY IT LIVES HERE, as a free function, rather than inline in its callers: T6's
resim frontier row and T7's prediction proxy branch must run the IDENTICAL
ladder — a resim that resolved a frontier tick differently from the prediction
that produced it would manufacture divergence out of nothing. T7 calls this
from collectInputAll's proxy branch; it is deliberately not a
private member so the ladder can also be unit-tested on its own.

CLASSIFICATION NOTE — the `tick < dA` guard reports Miss, not NoProbe. Rung 0 is
specifically `!findLatest().valid`, "nothing has EVER arrived": the join window.
The underflow guard is a different situation — data HAS arrived, we simply cannot
form a probe tick for a session younger than the delay. Since the D4 stale-run
rule (review F5) is stated exactly as "count fallback serves where
`findLatest().valid` is true", this case belongs in the run and NoProbe does not.
---------------------------------------------------------------------------
```

---

## §5 — RN-8's decide/project split and the diagnostic projection

The three properties the split must preserve, the decision struct's per-field contracts, and T20's two probe halves.

**Archived from the pre-task-14 header, lines 240-269.**

```
[RN-8 / task 58] DECIDE, THEN PROJECT — the ladder above is 14 load-bearing
lines; T19/T20 grew ~50 lines of diagnostic scaffolding (three lambdas) in front
of the first one of them. This is option B from ReviewNotes.md RN-8: the ladder
is factored into a PURE decision (`decideScheduledRelayedRead`, below) and a
thin projection (`resolveScheduledRelayedInput`, further below) that turns the
decision into candidate-or-fallback and, only if asked, into the diagnostic
report. THIS IS THE SHAPE TASKS 60 (part C) and 61 COPY — keep it clean.

THREE PROPERTIES THIS SPLIT MUST PRESERVE, none obvious from either half alone:

  1. THE NO-REPORT PATH STAYS BYTE-FOR-BYTE AS CHEAP. `decideScheduledRelayedRead`
     never touches the reporting out-param at all — it cannot, it is not passed
     one — and returns a small POD-ish struct by value: the same integer/enum
     scalars the old lambdas captured, plus the one `InputT` copy `find` already
     produced on the ladder's own stack. No new scan, no new allocation.
  2. THE `residentSpan()` SECOND SCAN IS PAID ONLY ON A MISS AND ONLY WHEN A
     REPORT WAS ASKED FOR. `decideScheduledRelayedRead` NEVER calls
     `residentSpan()` — it cannot classify InSpan/AboveNewest/BelowOldest at all,
     only whether a probe tick was formed. That classification is the
     PROJECTION's job, inside `resolveScheduledRelayedInput`'s
     `outDiagnosticReport != nullptr` guard, exactly where T20 already paid it.
  3. `NoProbeTick` STAYS DISTINCT FROM `BelowOldest`. The decision struct carries
     `isUnderflowMiss` as its OWN field rather than folding the tick<dA guard into
     the ordinary Miss arm — this is precisely where the two would get merged by
     someone reaching for the smallest struct. One IS an early-session artefact,
     the other a clock/capacity fault, and telling them apart is the whole point
     (see the classification note above `decideScheduledRelayedRead`'s underflow
     arm, and `RelayMissClass: the tick < dA underflow guard is its OWN class,
     not belowOldest` in RelayReadProbeTest.cpp).
---------------------------------------------------------------------------
```

**Archived from the pre-task-14 header, lines 271-276.**

```
What the ladder decided, before any of it is turned into a report. Returned BY
VALUE for the same reason `resolveScheduledRelayedInput` always has: `find`
copies into an out-param, so the Hit arm has a local to hand back, and a decision
struct is just that local plus the classification scalars the old lambdas used to
capture. Every field is meaningless on the outcomes the comment beside it excludes
— same convention `ScheduledRelayedReadReport` (RelayReadProbe.h) already uses.
```

**Archived from the pre-task-14 header, lines 282-284.**

```
The probed capture tick. 0u — and meaningless — on NoProbe and on the
tick<dA underflow guard, matching what the pre-split ladder reported on
those two rungs (neither ever formed a real probe tick).
```

**Archived from the pre-task-14 header, lines 289-292.**

```
The candidate PROJECT should serve when `useCandidate` is true (Hit only);
every other outcome projects to `store.fallback()`. Filled unconditionally by
`find`'s out-param, same as the pre-split ladder's `candidate` local — no
extra cost on the no-report path.
```

**Archived from the pre-task-14 header, lines 296-298.**

```
Whether a probe tick was actually FORMED — false on rung 0 (NoProbe) and on
the tick<dA underflow guard, true on Hit/VerifyFail/every other Miss. Gates
both the delta projection and the `isUnderflowMiss` short-circuit below.
```

**Archived from the pre-task-14 header, lines 301-303.**

```
FREE when set — `latest.captureTick`, which `decide` already has in hand from
its first line (T20's comment on the old `reportDelta`). Meaningless unless
`probeTickFormed`.
```

**Archived from the pre-task-14 header, lines 306-308.**

```
Set ONLY on the tick<dA underflow guard. `decide` already knows the miss
class there without a scan; PROJECT must not re-derive it and must not pay
`residentSpan()` for this rung (property 3 above).
```

**Archived from the pre-task-14 header, lines 312-315.**

```
THE PURE LADDER — property 1's whole reason for existing. Side-effect-free,
`const` over the store, callable with no reporting out-param because it has none
to take. Byte-for-byte the same arms, conditions, order and returned values T6/T7
shipped and T19/T20 left alone; only the SHAPE of what leaves the function changed.
```

**Archived from the pre-task-14 header, lines 328-331.**

```
Guard the subtraction rather than wrapping into a ~4-billion capture tick:
a session fewer than dA ticks old has no scheduled entry yet, which is the
same "nothing to read" situation as rung 0 — but see the classification note
above for why it is reported as Miss, not NoProbe.
```

**Archived from the pre-task-14 header, lines 359-361.**

```
A candidate WAS resident, but stamped against a delay that is no longer
the current one. The delay REGIME shifted under us — a transition, not
starvation. Same fallback, completely different diagnosis.
```

**Archived from the pre-task-14 header, lines 370-403.**

```
---------------------------------------------------------------------------
[T19, updated RN-8] `outDiagnosticReport` (renamed from the old, undifferentiated
out-param name, matching DiagnosticsConventions.md's `outDiagnostic…` prefix) —
THE OUTCOME, REPORTED TO THE CALLER RATHER THAN COUNTED HERE.

The relay hit-rate probe needs to know WHICH rung answered. The counters for it
deliberately do NOT live in this function, and that is a design constraint rather
than a style choice:

  * `decideScheduledRelayedRead` is side-effect-free so that it can be reasoned
    about (and unit-tested) as a pure classification;
  * it is SHARED by T6's resim frontier row and T7's prediction branch precisely
    so the two can never disagree — a resim that resolved a frontier tick
    differently from the prediction that produced it would manufacture divergence
    out of nothing. State living here would be state shared across two threads'
    worth of call sites through a function whose whole value is that it has none.

Reporting to the caller also means THE TWO CALL SITES ARE COUNTED SEPARATELY,
which is worth having on its own: prediction and resim hitting at different rates
is a real signal about the frontier, and summing them would erase it.

The parameter is a defaulted OUT-POINTER rather than a widened return type so
that every existing call site — and every existing test — compiles and behaves
unchanged. Writing through a pointer the CALLER owns is not a side effect on the
store, and mirrors `RemoteInputCache::find`, which already answers through
out-params for the same reason.

[RN-8] THIS FUNCTION IS NOW A PROJECTION: it calls `decideScheduledRelayedRead`
once, turns the decision into candidate-or-fallback, and — only when
`outDiagnosticReport != nullptr` — projects the SAME decision into the report.
The `residentSpan()` second scan (T20 PROBE B, half two — WHY the miss happened)
lives here, inside that guard, and ONLY on the Miss/non-underflow arm: this is
property 2 from the banner above `decideScheduledRelayedRead`.
---------------------------------------------------------------------------
```

**Archived from the pre-task-14 header, lines 417-422.**

```
[T20] PROBE B, half one — the SIGNED DISTANCE from the probe tick to the
newest thing the store holds. Set on every rung that formed a probe tick,
Hit included: the hit deltas are the calibration the miss deltas are read
against. FREE — `decision.newestResident` is `latest.captureTick`, which
`decide` computed on its first line, so this is an integer subtraction and
no scan.
```

**Archived from the pre-task-14 header, lines 432-443.**

```
[T20] PROBE B, half two — WHY the miss happened, from the store's
RESIDENT SPAN. This is the one addition that costs anything: a second
scan of the store's slots. Paid ONLY on a miss and ONLY when a report was
asked for (this whole block sits behind `outDiagnosticReport != nullptr`),
so a caller that passes no report is byte-for-byte as cheap as before T20.

THE SPAN IS THE STORE'S, NOT THE RING'S. The ring carries `depth` entries
per replication; the store accumulates up to 64 arrivals. That is exactly
why an in-span hole is meaningful at depth 1 — it IS the hole the
replace-latest ring punched between two replications. Reading the ring's
span here instead would make every miss trivially "out of span" and the
classification worthless.
```

**Archived from the pre-task-14 header, lines 448-452.**

```
NO PROBE TICK EXISTS, so there is no delta and no span comparison
to make. Kept as its own miss class rather than folded into
BelowOldest, which it superficially resembles: this is an
early-session artefact and that one is a clock/capacity fault, and
the whole point of the split is to tell causes apart.
```

**Archived from the pre-task-14 header, lines 465-468.**

```
Unreachable: this arm only runs on a Miss below the rung-0
gate, so at least one slot is occupied. Classified rather than
asserted because a probe must never be the thing that brings a
session down.
```

---

## §6 — Neutral injection (T17) and the (0,0,0) poison

Why the default InputType{} is not the game's zero, and the three consumers of the injected value.

**Archived from the pre-task-14 header, lines 485-505.**

```
Per-SIMULATABLE-TYPE (not per-id) neutral input. Wrapped in a struct keyed on
the simulatable rather than stored as a bare `InputType` so that a pack whose
members happen to share one InputType still gets distinct tuple slots —
`std::get<InputType>` would be ill-formed there, and silently so at the
template level until such a pack first appeared.

[og-netcode-v2-input-relay T17] `injected` records whether the composition root
ever called setNeutralInput for this type. It exists because the AUTHORITY is
now a first-class consumer of this value — collectInputAll's remote
branch substitutes it on a queue underrun, i.e. on every tick of
every join window.
([T8] T17 also seeded `m_lastUsedInputs` with it at registerAuthorityOwner;
that map is retired, so the underrun substitute is the surviving authority
consumer. The warning at the registration site is unchanged and is still the
right place for it — registration is where an un-injected neutral first
becomes reachable by the branch below.) A composition root that injects on the
client role only
would silently reintroduce the value-initialised `InputType{}` — whose (0,0,0)
forward vectors are exactly the value LocalInputCache.h documents as
normalisation-breaking. Before T17 the miss was invisible; the flag turns it
into a warning at the registration site where it bites.
```

**Archived from the pre-task-14 header, lines 590-611.**

```
[T9 part 4] Inject the game's zero input, used to fill the
[0, effectiveDelay) window at session start and after a resync wipe.

MUST be called by the composition root ON BOTH ROLES: the default
`InputType{}` is NOT the brawler's zero input (`getZeroPlayerInput` builds
(0,0,1) forward vectors, a value-initialised one would carry (0,0,0) into
normalisation). Applies to delay lines created later AND to any already
created, so it is order-independent with respect to registration.

[T5] The SAME injected zero also seeds every relay store — it is that
store's `fallback()` value before anything has ever arrived for a remote
character (T7's rung 0, the pre-registration / idle window). One injection
point, two consumers, so the two can never disagree about what "zero" is.

[T17] ...and a THIRD consumer, on the AUTHORITY: the substitute integrated
on a remote-move queue underrun. "Client" is no longer part of this value's
name for that reason. ([T8] T17 named a second authority consumer here, the
`m_lastUsedInputs` seed at registerAuthorityOwner; that map is retired with
the correction-input channel, so the underrun substitute is the whole of
the authority's use.) The authority path is still the one that is NOT
order-independent (see registerAuthorityOwner), which is why the injection
being missed there is warned about rather than merely commented.
```

**Archived from the pre-task-14 header, lines 630-635.**

```
[T17] Has the composition root injected the game's zero input for this
simulatable type? Public so the ROLE that consumes it can be pinned by a
test rather than by a comment: the authority's seed and its underrun
substitute are both silently degraded — never wrong-looking — when the
injection is missed, which is exactly the class of regression a comment
does not catch. Also the read side of registerAuthorityOwner's warning.
```

**Archived from the pre-task-14 header, lines 642-644.**

```
[T17] The injected neutral itself, for the same reason: a test that asserts
"the authority applied the GAME's zero" must be able to name the value the
composition root injected without re-deriving it.
```

**Archived from the pre-task-14 header, lines 1458-1481.**

```
[T17] THE SUBSTITUTE IS THE GAME'S ZERO INPUT.

dequeueMove() on an empty queue hands back a value-initialised
Move (SimulationQueues.h), so `move.input` here is an
`InputType{}` — for the brawler that is (0,0,0) forward
vectors, the value LocalInputCache.h names as the one
that "would be carried into normalisation and break". The
injected neutral is the game's real zero ((0,0,1) forwards),
it is already present on both roles, and it was simply unused
on this path.

This is NOT a loss-only path: the remote branch runs from
registerAuthorityOwner onward, so every tick between
registration and the client's first input arriving underruns
— i.e. every join window, in ordinary play.

[T8] T17 also wrote this value into `m_lastUsedInputs` here,
because that map was replicated as "the input applied" on the
correction-input channel. Both the map and the channel are
retired; what the substitution feeds now is the integration
(and, through it, the correction STATE peers actually consume).
The T17 fix is undiminished — this is still the only value the
authority simulates on an underrun tick — it simply has one
consumer instead of two.
```

**Archived from the pre-task-14 header, lines 1487-1488.**

```
The dequeued input passes through UNTOUCHED — this arm is
byte-for-byte the pre-T17 behaviour.
```

**Archived from the pre-task-14 header, lines 1804-1812.**

```
[T9 part 4, RENAMED by T17] The game's zero input, per simulatable type.
NOT "client" neutral inputs any more: the AUTHORITY reads this too (the
underrun substitute in collectInputAll's remote branch; [T8] T17 also named
the `m_lastUsedInputs` seed in registerAuthorityOwner, since retired), so the
old name described one of several consumers and invited exactly the reasoning
that left the authority path integrating `InputType{}`. Written once by the
composition root
(setNeutralInput, game thread, before/around registration), read on the
physics thread thereafter.
```

---

## §7 — Registration, unregistration and the by-id doors

The container-lifecycle half, the fixed cross-peer ordering, and why the accessors are nullable rather than throwing.

**Archived from the pre-task-14 header, lines 651-663.**

```
-----------------------------------------------------------------------
[T5 / input relay] Relay-store access — NULLABLE BY DESIGN
-----------------------------------------------------------------------

Returns nullptr for an id that has no store, which is every LOCAL character
(provider-present) and every id the authority owns. Nullable rather than
throwing because both eventual callers are on paths that legitimately see
both classes of id: T7's proxy branch iterates all simulatables, and the
game-thread visualization readers run on the listen-server host too, where
no store exists at all. A throwing accessor on those paths is a known trap
in this codebase — the retired `getLastCorrectionInput` routed through the
THROWING `getCacheFor`, so calling it on the authority (which keeps no
caches) threw rather than answering "nothing here".
```

**Archived from the pre-task-14 header, lines 680-716.**

```
-----------------------------------------------------------------------
[T7] getLastRelayedInput — LAST-KNOWN, for the GAME-THREAD viz readers
-----------------------------------------------------------------------

`lastKnown` as the design doc's §4 vocabulary defines it: the newest relayed
input for a REMOTE character, or nothing if none has ever arrived. It is the
replacement source for the two remote-proxy visualization consumers that
used to read the correction cache's input column
(SimulationReconciliation::getLatestInput -> the SimmableUpdateComponent viz
site, and through it BrawlerVisualizationInputSource's remote branch).
[T8] That re-point is now IRREVERSIBLE, not merely preferred: with the
correction-input channel retired, a remote character's cache input column
held only this client's own prediction. [T16] The column does not exist at
all any more. This accessor is THE source of a remote character's actual
input on the client — there is no second one to fall back to.

WHY `findLatest().input` RATHER THAN `fallback()`, which is what the
per-tick ladder terminates on. The viz caller must be able to tell "nothing
has ever arrived" from "an input arrived and it happened to be neutral":
the pre-existing behaviour is that a remote proxy with no data yet has its
input-carrying viz SKIPPED for that frame, and `fallback()` — which invents
the injected game zero on an empty store — would silently start drawing an
aim indicator at the neutral pose instead. std::optional keeps the two
distinguishable, exactly as the (since-retired, T16) getLatestInput it
replaced did with its own nullopt. (T5's notes name both spellings and this
condition for choosing.)

NULLABLE FOR TWO SEPARATE REASONS, both of which the callers really see:
no store exists for a LOCAL character or on the AUTHORITY at all (a listen
server's own pawn), and a store that exists may still be cold.

THREADING. This is the ONE reader of the store on the GAME thread, which is
the store's WRITER thread (OnRep_RelayedInputRing -> populate). It therefore
does not participate in the accepted GT-write/PT-read tear documented in
RemoteInputCache.h — it is same-thread with the writer, and strictly safer
than the physics-thread readers. It returns a COPY, so no reference into the
slots outlives the call.
```

**Archived from the pre-task-14 header, lines 789-790.**

```
try_emplace default-constructs in place — PendingInputQueue
holds std::atomic members and is neither copyable nor movable.
```

**Archived from the pre-task-14 header, lines 834-838.**

```
The container-lifecycle half of the old unregisterSimulatable (its
step 3): erases the five container families' entries for `id` plus the
join key. NetSync calls this AFTER clearing callbacks and erasing its
own writer/sender maps (steps 1-2), preserving the same fixed ordering
the pre-cut single method enforced.
```

**Archived from the pre-task-14 header, lines 852-857.**

```
[item 86] Drops this peer's telemetry sibling's per-id state. Called
from NetSync::unregisterSimulatable's step 4 ALONGSIDE
`m_telemetry.forgetOwner(id)` — the same fixed step of the same
ordering that called `m_inputResolutionTelemetry.forgetOwner(id)`
directly before this peer existed. See InputResolutionTelemetry.h for
the full rationale.
```

**Archived from the pre-task-14 header, lines 863-874.**

```
-----------------------------------------------------------------------
[item 86 / design §C.3] The by-id ingest/drain/query doors.

NetSync's registration callbacks now capture `(this, id)` only — never a
reference into a container this peer owns — and route through these
four methods. A late-firing callback that survives past
`unregisterCharacter`'s erase can no longer dangle (the pre-cut shape
captured `&store` / `&remoteQueue` directly, which the ordering
invariant in NetSync's unregisterSimulatable protected against UB by
clearing callbacks first); post-cut, the SAME late fire becomes a
benign lookup miss, answered as a no-op/drop.
-----------------------------------------------------------------------
```

**Archived from the pre-task-14 header, lines 876-881.**

```
Replaces the OnRep-bound relay-arrival callback's body (the ingest
itself) AND the registration-time bind-time catch-up read — both now
by-id lookups into m_remoteInputCaches rather than a captured
reference. NetSync's bind-time catch-up call discards the returned
report (T19's not-fed-to-the-cadence-probe rule); its OnRep thin-call
forwards the report to `m_telemetry.emitRelayArrival`.
```

**Archived from the pre-task-14 header, lines 889-893.**

```
Benign lookup miss (design §C.3) — a late-firing callback after
this id's store was erased. A default-constructed report reads
as NeverWritten, which is the accurate "nothing was ingested"
answer; NetSync's OnRep thin-call still logs it through the
normal emitRelayArrival path rather than special-casing it.
```

**Archived from the pre-task-14 header, lines 899-905.**

```
Replaces the RPC-bound remote-move callback's queueing call
(`remoteQueue.queueMove(...)`), by id instead of by captured reference.
Guard context (`currentAuthorityTick` / `rollbackWindowTicks`) crosses
in BY VALUE from NetSync's own members — the guard stays NetSync's
(design §C.4); this door only ever sees copies. The too-far-future
warning and the `[ReceiveLocalInput]` SIMLOG stay at NetSync's lambda,
which is where the logger already lives.
```

**Archived from the pre-task-14 header, lines 915-916.**

```
Benign lookup miss (design §C.3) — see ingestRelayRing's
comment; same rule, same by-id door, same reason.
```

**Archived from the pre-task-14 header, lines 923-927.**

```
Replaces `sendLocalInputToAuthorityAll`'s `.at(id)` lookup into
m_pendingInputQueues with a nullable by-id door; the send `OG_CHECK`s
non-null (today's `.at` throw becomes an explicit loud check — design
§C.4). The queue TYPE remains the thread seam (SimulationQueues.h);
this accessor only hands the consumer end to the consumer thread.
```

**Archived from the pre-task-14 header, lines 1701-1704.**

```
REMOTE. Nullable by design (T5): the authority allocates no stores
for ids it owns, and a character can be iterated before its
registration completes. Neutral rather than a throw, matching every
other reader of this accessor.
```

**Provenance relocated out of the header by task 21 — the two crashes behind the
fixed sequence.**

The header states that `SimulationNetSync` keeps the *binding* half of registration
— `registerPredictionOwner` / `registerAuthorityOwner` / `unregisterSimulatable` —
and calls these four container-lifecycle methods in a fixed sequence, and that two
live crashes were fixed to protect that sequence. It no longer carries the item
numbers, which are here: **items 92 and 93** of `og-netcode-v2-input-relay`. Both
were pre-existing lifecycle-ordering defects on the authority path that the
migration's own requirements exposed; the fixes made **registration publish to
storage LAST** and **unregistration unpublish FIRST**, so no peer can be reached for
an id that is only half-registered. Item 92 also added the loud-failure `OG_CHECK`
whose retirement §13 prices.

---

## §8 — The frontier pair: item 90's fusion, item 94's split, item 96's facade

The successor obligation, why allocation left this class, and the name reversal that item 94 put on the record.

**Archived from the pre-task-14 header, lines 973-1039.**

```
-----------------------------------------------------------------------
collectInputAll — resolves every registered id's input for this tick.
ALLOCATES NOTHING. See `SimulationReconciliation`'s own frontier-
allocating sweep for the frontier-pair contract's full text (item 94
moved both the allocating sweep and its contract banner there).
-----------------------------------------------------------------------
⚠ THE SUCCESSOR OBLIGATION (part of the frontier-pair contract, stated
here because this is the OPENING call a future author sees first): this
call MUST be followed by reconciliation's frontier-allocating sweep
in the SAME phase, before anything else runs. Collecting without
allocating leaves capture (`postPredictionAll`) free to push state into
the PREVIOUS frontier slot — the frontier-pair detector's uncovered
direction (blind spot #2, `CorrectionCache.h`'s
`m_frontierSlotAwaitingState`). This class can no longer prevent that
by construction (item 90 through item 93's shape fused
resolve-then-allocate inside one call; item 94 un-fuses them so both
live where the slots do) — the obligation is now a caller discipline,
not a compiler-enforced one, and this sentence is the record of that
trade.

[item 96] THE DOCUMENTED DOOR: `preparePredictionSimulationStep`
(`SimulationStepSequencing.h` — NOT this header; it names neither peer
type, so hosting it here would have been arbitrary) performs this call
followed by
`reconciliation.allocateFrontierSlotsAll(step)` under one name, on the
`registerSimulatable`/`unregisterSimulatable` precedent
(`SimulationNetSync.h`). It is default-correctness and discoverability,
NOT enforcement — this method stays public and independently callable,
and blind spot #2 above is unchanged by its existence. The
PREDICTION-only manager path (`SimulationManager::
onGameSimulationPrediction`) routes through it; the authority path
(`onGameSimulationAuthority`) calls this method directly, alone, on
purpose — see both call sites.

[item 94] THE NAME REVERSAL, ON THE RECORD. This method is named
`collectInputAll` again, reversing item 90's `prepareSimulationStep`
ruling deliberately (RN-3 precedent: an overturned ruling is recorded,
not silently undone). Item 90's own words for the rename: *"the twins'
asymmetry is now meaningful and named, not merely historical: PREPARE
allocates, RESIM COLLECT reads."* That argument no longer holds: this
method does not allocate any more — allocation is exactly what item 94
removes from it — so keeping "prepare" would describe a responsibility
the method no longer has, the RN-14 defect (a name that outlives the
fact that justified it), reintroduced by the very item that once fixed
it. With allocation gone, `collectInputAll` and `collectResimInputAll`
are symmetric pure readers again, and item 90's own naming argument now
argues FOR the revert rather than against it.

[item 61 / RN-11] SKELETON — the per-character dispatch lives in
`collectInputForCharacter`, extracted below (straight fold, Pattern 1:
no branch in the old lambda body contained a `return`, so nothing here
needed the RN-8/task-58 decide/project split — see the banner on
`collectInputForCharacter` for the full argument).

[item 94] ONLY ONE PASS NOW, NOT TWO. Item 90's internal sweep-1/sweep-2
split, its sweep-boundary "nothing may run between them" fence, and its
sweep-boundary exception-safety debt paragraph (item 91 part I) are all
GONE from this method — there is only one `forEachSimulatable` pass
left below, and the debt paragraph's DECISION (accepted as documented
debt; a mid-sweep throw skips the whole tick's allocation, reachable
only via an already-broken registration invariant) carries forward
UNCHANGED, but its MECHANICS are now about two separate statements in
`SimulationManager::onGameSimulationPrediction` rather than an internal
boundary in this function — see that method for the relocated text.
`collectInputForCharacter`'s own three `.at(id)` lookups (the ones that
can throw) are unchanged by this move; see that method's own comment
for their corrected attribution (91-I's fix, folded in by this task).
```

**Archived from the pre-task-14 header, lines 1069-1075.**

```
[item 94] THIS METHOD ALLOCATES NOTHING AND RETURNS HERE. The
former sweep-boundary fence and the sweep-2 call (Reconciliation's
frontier-allocating sweep) that used to sit at this exact point
are GONE from this class — see the method's own banner above for
where the fence and the debt-acceptance decision it protected
relocated to (`SimulationManager::onGameSimulationPrediction`,
between this call and reconciliation's own sweep).
```

**Archived from the pre-task-14 header, lines 1256-1307.**

```
[item 61 / RN-11] `collectInputAll`'s per-character body (named
`collectInputAll` before item 90's rename to `prepareSimulationStep`;
item 94 reverted that rename once allocation left the method — see
`collectInputAll`'s own banner), lifted out verbatim (RN-10 part A
precedent) — three mutually exclusive branches (local provider / remote
queue / simulated proxy), each ending in a `map.emplace`, none
containing a `return`. PATTERN 1 — STRAIGHT FOLD applies to every
branch's diagnostics: the dispatch's fence is "no branch is a return
affecting the caller", and there is exactly one `return` anywhere in
this function (the implicit one at the end of each branch, same as
before the split) — never one interleaved BETWEEN two probe/log calls
the way `decideCorrectionArrival` had to guard against. Each branch's
SIMLOG/probe tail folds into its own `emit*` call below.

[item 90, unchanged by item 94] FRONTIER ALLOCATION DOES NOT HAPPEN
HERE. Every branch used to end with its own `if (Skip)
backfillSkippedTick(...)` and its own `if
(stepAllocatesFrontierSlot(...)) pushPredictionTick(...)` — both are
gone, relocated at item 90 to a dedicated sweep-2 method on THIS class
and relocated AGAIN at item 94 to Reconciliation's own
frontier-allocating sweep — this class no
longer has an allocating sweep of its own at all. What stays here, per
the local-provider branch's own comment at its site, is the delay-line
capture gate (unchanged) and the send enqueue, now re-gated on the SAME
captured predicate result as the capture gate rather than a second call
to `stepAllocatesFrontierSlot` — see that site for why one call, not
two.
[item 90] `T& simulatable` DROPPED FROM THE SIGNATURE — it was read
exactly twice, both times as `simulatable.getAllState().getState()` for
a `backfillSkippedTick` call, and both calls moved to what was then
sweep 2 (now Reconciliation's own frontier-allocating sweep,
which takes its own `simulatable` from its own `forEachSimulatable`
pass). Nothing else in this function ever touched the character object;
keeping an unused reference parameter here would be dead weight that
restructuring created.

⚠ [og-netcode-v2-input-relay item 91 part I, MISATTRIBUTION FIXED AT
ITEM 94 — 91-I / review 92, folded in per task 94's part F] This
function's local-provider branch does TWO `.at(id)` lookups that can
throw a real, unwinding C++ exception under this module's `/EHsc`
build if the invariant they assume — provider-present implies
line/queue-entry-present — has already been broken elsewhere: the
delay-line fetch (below) and the pending-input-queue enqueue (below).
The THIRD such lookup this function makes — the last-used-capture-tick
write — is NOT in the local-provider branch; it is in the
AUTHORITY/QUEUE branch (`queueMap.find` succeeds), guarding
`registerAuthorityCharacter`'s pairing. A prior version of this note
misattributed all three to the local-provider branch (review 91 finding
1, confirmed still unfixed by review 92, corrected here). See
`collectInputAll`'s own banner for what happens at the CALLER when any
of the three throws (the debt-acceptance decision, now stated at
`SimulationManager::onGameSimulationPrediction`).
```

**Archived from the pre-task-14 header, lines 1409-1415.**

```
[item 90, relocated again item 94] `backfillSkippedTick` and
`pushPredictionTick` do not run in this class at all any more —
this branch never allocated (item 90), and the dedicated
sweep-2 method that used to is deleted; its
replacement lives on `SimulationReconciliation`. The `[T16]
pushPredictionInput` removal note that used to sit here moved
with the tick push, all the way to `CorrectionCache.h`.
```

**Archived from the pre-task-14 header, lines 1583-1589.**

```
[item 90, relocated item 94] `backfillSkippedTick` and
`pushPredictionTick` do not run in this class at all — this
branch never allocated (item 90); it never wrote an input
column either ([T16], the `pushPredictionInput<T>(id, input)`
removal noted below the old gate). The relay store holds what
the proxy actually sent; nothing here stores this client's
guess.
```

---

## §9 — The resim resolution table (T6)

The two-level dispatch, the precedence rule, NoSlot, the D2 frontier edge and the self-heal.

**Archived from the pre-task-14 header, lines 1079-1168.**

```
-----------------------------------------------------------------------
[T6] Resim replay input (physics thread) — THE RESOLUTION TABLE
-----------------------------------------------------------------------

[item 90 / design review §F.6, re-scoped item 94] THIS METHOD ALLOCATES
NOTHING, EVER — true since T6 and unchanged by item 94. What item 94
DOES change: `collectInputAll` above ALSO allocates nothing any more
(item 90's "PREPARE allocates, RESIM COLLECT reads" asymmetry named a
real difference at the time; item 94 removes the allocating half from
`collectInputAll` entirely, so that specific asymmetry is gone — both
twins are pure readers now, matching their names again). The asymmetry
that survives is about the CALLER, not either method: only the
prediction path's caller (`SimulationManager::onGameSimulationPrediction`)
follows its collect with reconciliation's own frontier-allocating sweep;
nothing follows a resim collect with one, because resim never opens a
frontier pair (verified at every one of this method's call sites in
§F.6: `prepareResimAll` restores existing slots, this method's join key
is a read of a slot allocated by a PAST prediction pass,
`postResimulationAll` finds-or-discards and never allocates, and
`applyResimAll` reads a frontier slot the original prediction pass
allocated — replay itself pushes no ticks).

RELOCATED HERE from SimulationReconciliation (T6 placement ruling, Option
C), and home finished by item 87. It used to read one thing — the
correction cache's input column — and its old home was justified on
exactly that. It now reads the client's own delay lines, the relayed-input
stores and the injected neutrals, all owned by this class; only the JOIN
KEY still comes from reconciliation, through the single narrow
getAppliedCaptureTickRef query. Nothing was added to either class's
dependencies: this class already held m_reconciliation, and reconciliation
still knows nothing about resolution.

TWO-LEVEL DISPATCH: TICK CLASS x CHARACTER CLASS.

                   | LOCAL (provider present)   | REMOTE (proxy)
  -----------------+----------------------------+---------------------------
  Ref (corrected)  | delayLine.at(ref)          | store.find(ref) -> input
  Sentinel         | injected game zero         | injected game zero
  ...store miss    | line miss -> its neutral   | store.fallback() (SELF-HEAL)
  NoRef (frontier) | delayLine.at(t - d)        | the scheduled read
  NoSlot           | no entry at all — the character is not in this resim

CHARACTER CLASS IS PROVIDER PRESENCE, NEVER A ROLE CHECK (T5's mechanism).
It is the only test that stays correct under COUCH CO-OP, where one client
legitimately owns several locally-controlled characters and any
"the client's character" test would pick exactly one of them.

---------------------------------------------------------------------
THE PRECEDENCE RULE (RelayDelaySpectrumDesign.md §5.3): WHEREVER A REF
EXISTS, THE REF WINS — and the relay entry's `dA` stamp is IGNORED on a
corrected tick. The stamp is the INTENDED schedule; the ref is what the
authority ACTUALLY did, and resim exists to reproduce the authority. The
case that makes this concrete: T26 can release a capture LATE (up to
rollbackWindowHardCap), so capture 6 stamped for tick 7 may be applied at
tick 7+d — the ref on 7+d then says 6, and resim must replay 6 there.
An implementation that consulted the stamp would replay whatever capture
the schedule pointed at instead, and be confidently wrong.

---------------------------------------------------------------------
NoSlot EMITS NOTHING, AND THAT IS LOAD-BEARING. The pre-T6 body only
emplaced on a cache hit, and SimulationIntegrationExecutor::integrateAll
skips any id absent from the map. prepareResimAll likewise only restores
state for a character whose slot exists — so emitting an input for a
slotless character would integrate it from an UN-RESTORED state. Freshly
registered proxies sit in exactly that window for a few ticks.

---------------------------------------------------------------------
THE D2 FRONTIER EDGE — DOCUMENTED, DELIBERATELY NOT SOLVED (Backlog T6).
The NoRef/local row re-derives with the CURRENT effective delay. If the
delay changed between the original prediction of tick `t` and this resim,
`t - currentEffectiveDelay` can name a DIFFERENT capture than the one
originally integrated there, so a resim that crosses a delay change replays
a mixture of offsets. That is the accepted offset-mixture edge: exposure is
about one tick at every-frame correction cadence, the every-frame state
anchor supersedes it immediately, and T14's stall paydown narrows the window
further. Do not "fix" this by stashing the per-tick delay without re-opening
the design decision — the cache slot would have to carry a third column.

---------------------------------------------------------------------
RESYNC INTERPLAY, stated so it is not rediscovered as a regression. A hard
resync wipes the correction cache AND the local delay lines, but NOT the
relay stores (T5's ruling — a relayed entry is keyed by the SENDER's capture
tick, which our clock jumping does not invalidate). So through the wipe
window local ticks resolve to the neutral, exactly as they did pre-T6, while
remote proxies keep resolving from surviving entries. Strictly better than
the pre-T6 behaviour, in which the wiped cache blinded both.
[item 61 / RN-11] SKELETON — see `collectResimInputForCharacter`'s banner
for why this is also a straight fold (Pattern 1), same reasoning as
`collectInputAll`, applied per rung of the resolution
table below rather than per branch.
```

**Archived from the pre-task-14 header, lines 1661-1666.**

```
BOTH character classes, one answer: the authority told us it
applied no client capture at this tick, and T17 made the value
it substituted the INJECTED GAME ZERO. Reproducing an authority
decision means applying what the authority applied — never a
value-initialised InputType{}, which is the exact poison T17
removed from that path.
```

**Archived from the pre-task-14 header, lines 1688-1695.**

```
A line miss answers with the line's own INJECTED neutral (never
InputT{}) — the store-miss/local cell. Post-resync that is the
only reachable way to get here, and it is today's behaviour.

NOT resolveDelayedInput: that helper exists to protect the LIVE
capture at delay 0, and a resim has no live capture. Reading
at(simTick) at delay 0 is right precisely because the original
prediction pass pushed that tick's capture into the line.
```

**Archived from the pre-task-14 header, lines 1715-1718.**

```
THE REF WINS — see the precedence block above. `outDA` is bound
only because `find` requires an out-param; its value is
deliberately never read. The stamp is the intended schedule and
has no authority over a tick the server has already ruled on.
```

**Archived from the pre-task-14 header, lines 1723-1725.**

```
THE SELF-HEAL: a miss degrades THIS TICK's replay input to
last-known, never the injected state. The state is a complete
anchor, so the next every-frame correction supersedes the error.
```

---

## §10 — Resync, and the wipe asymmetry

Which containers a hard resync wipes, which it deliberately does not, and the second consequence T15 added.

**Archived from the pre-task-14 header, lines 846-846.**

```
[T5] Erased on UNREGISTRATION only — never on resync (see wipeAllForResync).
```

**Archived from the pre-task-14 header, lines 1194-1196.**

```
Drops any queued local inputs that were produced against the pre-resync
prediction clock. Invoked from the ClientPredictionClock resync callback
alongside the reconciliation cache wipe.
```

**Archived from the pre-task-14 header, lines 1209-1226.**

```
[T9 part 3] The delay line is keyed by CAPTURE TICK against the
pre-resync prediction clock. After a hard resync that clock has jumped,
so those keys describe ticks that no longer mean what they meant, and a
surviving capture would be read at the wrong tick for `effectiveDelay`
ticks. Dropping them re-enters the neutral-filled window — the same
well-defined state as session start (part 4), which is exactly why part
4 is not special-cased to tick 0.

[T15] SECOND CONSUMER, SECOND CONSEQUENCE — stated so it is not
rediscovered as a bug. The line is now also the motion matcher's
history source, so this wipe blanks the matcher for up to
`inputSequence::kHistoryWindowFrames` (30) ticks after a hard resync,
where the correction cache it used to read would still have served
data. That is CORRECT, not a regression: the cache's data was keyed to
the pre-resync clock, so a motion "matched" out of it would have been
assembled from ticks that no longer mean what they meant. Going cold is
the honest answer, and it costs at most half a second of matcher
availability on an event that already visibly jumps the simulation.
```

**Archived from the pre-task-14 header, lines 1237-1252.**

```
[T5 / input relay] m_remoteInputCaches is DELIBERATELY NOT WIPED HERE,
and this comment exists because the loop above is the obvious thing to
mirror.

The delay line's keys are the LOCAL prediction clock's tick numbers, and
a hard resync jumps that clock — so those keys stop describing the ticks
they were written for, and keeping them would read a capture at the wrong
tick for `effectiveDelay` ticks. A relay entry's key is the SENDER's
capture tick: a server-domain identity produced by another machine's
clock, which a resync of OUR clock does not touch. Wiping it would blind
every remote proxy for a window after every resync, for no reason.

(This wipe divergence is also the decisive reason the store is its own
type rather than a reused LocalInputCache — see the naming ruling in
Network/RemoteInputCache.h. A reused type would have been swept by
whoever next mirrored these per-id map loops.)
```

---

## §11 — Per-character collect: the capture/applied split and the gates

What goes on the wire versus what the integrator sees, the third != Stall gate, and the underrun detection order.

**Archived from the pre-task-14 header, lines 1343-1397.**

```
[T9 parts 3+4] Layer-1 client input delay.

Two different values leave this branch and the distinction is
the whole point of the task:

  `capture` — the ORIGINAL, undelayed input, stamped at the
    CURRENT tick. This is what goes on the wire. The server
    parks it in its own ServerInputDelayQueue and applies the
    same delay itself; sending an already-delayed input would
    have the server delay it a SECOND time and the two ends
    would diverge by exactly `effectiveDelay` ticks.

  `applied`  — the capture from `tick - effectiveDelay`. This
    is what the integrator predicts with.

    [T16] It used to ALSO be pushed into the correction cache's
    input column, because that column's slot T meant "the input
    APPLIED at tick T" — matching both the server's replicated
    correction-input write and the no-offset read the pre-T6
    collectResimInputAll did. All three of those are retired:
    the channel at T8, the resim read at T6 (it resolves by the
    T4 capture-tick REF now), and the column itself at T16. The
    `applied` value is consumed by the integrator and by nothing
    else on this line; the RAW capture is what persists, in the
    delay line, which is where the resim and the motion matcher
    both read it from.

With `effectiveDelay == 0` this is exactly the pre-T9 path:
resolveDelayedInput returns the live capture untouched.

[T15] `delayLine` is bound ABOVE, before the provider call.

[og-netcode-v2-input-relay item 84 / review B-3, re-scoped item 90]
THE THIRD `!= Stall` GATE, ruled rather than left literal:
capture-history admission and frontier allocation are today the
SAME decision — "does this step advance the tick" — so this
shares `stepAllocatesFrontierSlot` rather than re-deriving a
literal. A future StepKind that wants the two to differ
introduces a second, NAMED predicate deliberately; it does not
silently fork this one. Do not "simplify" this back to a
literal `!= StepKind::Stall`.

[item 90] CAPTURED ONCE, USED TWICE — this is this class's
ONLY remaining call to `stepAllocatesFrontierSlot` (the
grep-counted "sweep-1 capture gate"; see `collectInputAll`'s
banner and SimulationTimeContext.h's own banner — item 94's two
allocation-side call sites both moved to
`SimulationReconciliation.h`). It gates the delay-line push
immediately below AND
this branch's send enqueue, further down, which used to be
gated by a SEPARATE call sharing the tick-push gate that sweep
2 now owns. The enqueue and the tick push were never the same
decision by coincidence — item 84 already named them as one —
so reusing this result for the enqueue is not a new merge, it
is the same one, minus the literal doubled call.
```

**Archived from the pre-task-14 header, lines 1418-1420.**

```
ORIGINAL capture, current tick — the send channel carries
the undelayed value; see the `capture` vs `applied` split
above. Do not pass `applied` here.
```

**Archived from the pre-task-14 header, lines 1429-1437.**

```
[T2] UNDERRUN MUST BE DETECTED HERE, BEFORE THE DEQUEUE.

dequeueMove() on an empty queue returns a value-initialised
Move{} whose `tick` is 0 (SimulationQueues.h) — and 0 is a
perfectly ordinary real capture tick at session start. So the
RETURNED tick cannot distinguish "the authority substituted an
input" from "the authority applied the client's tick-0 capture".
Only the pre-dequeue empty() gate can, which is why the flag is
taken first and the returned tick is never used to infer it.
```

**Archived from the pre-task-14 header, lines 1628-1641.**

```
[item 61 / RN-11] `collectResimInputAll`'s per-character body, lifted out
verbatim. This is the TWO-LEVEL RESOLUTION TABLE described in the banner
above `collectResimInputAll` — a ladder of mutually exclusive rungs, each
a guard clause ending in its own `return`. PATTERN 1 — STRAIGHT FOLD
applies at the PER-RUNG granularity: within any single rung, the
diagnostics (one SIMLOG, or — the last rung — one probe write plus two
SIMLOGs) are never split by a `return`; the `return` always comes AFTER
the diagnostics and the `map.emplace`, exactly like the guard clauses in
`decideCorrectionArrival`'s CALLER (`onCorrectionReceived`), never like
`decideCorrectionArrival` itself. There is also only ONE probe write in
this whole function (`noteResimRead`, last rung) — none of it shares a
counter with a sibling probe the way the correction callback's two probes
did, so there is no discard-population hazard to guard against here.
Each rung's tail folds into its own `emit*` call below.
```

---

## §12 — The simulated-proxy branch and the emergent regime

One code path, no regime flag; what the probes count and what they refuse to count.

**Archived from the pre-task-14 header, lines 1179-1184.**

```
LOG VOLUME, deliberately bounded. The branches below are mutually
exclusive, so this emits EXACTLY ONE line per character per resim tick
— and it is [Verbose]-prefixed while its `[Resim.*]` neighbours are not,
so an operator can silence this table trace alone (LogOGSim=Log) and
keep [Resim.Pre]/[Resim.Post]. The pre-T6 body logged nothing at all;
a resolution table nobody can trace in PIE is not worth that saving.
```

**Archived from the pre-task-14 header, lines 1494-1562.**

```
-----------------------------------------------------------
[T7] SIMULATED-PROXY BRANCH — THE UNIFIED SCHEDULED READ.
-----------------------------------------------------------

The client is predicting a character it does not control. Until
T7 this read the CORRECTION CACHE's last server-reported input
— "hold whatever the server last told us this player did",
about one RTT stale and with no notion of WHEN the authority
would apply it. It now asks the relay store the same question
the resim asks: which relayed input does tick N run on.

ONE CODE PATH, NO REGIME FLAG — the emergent-regime property
(RelayDelaySpectrumDesign.md §4). resolveScheduledRelayedInput
probes `N - dLatest` and verifies the stamp:

  * at relay delay floor 0 the probe nearly always MISSES (the
    sender's capture for tick N has not reached us yet, because
    nothing was scheduled to make it) and the ladder degenerates
    to last-known — byte-for-byte the pre-relay behaviour;
  * at a high floor it nearly always HITS and the proxy consumes
    the server's actual schedule, tick for tick.

Nothing here selects between those. The regime is decided per
input, per receiver, by whether the data is there — which is why
raising the floor needs no second implementation and no switch.

And "nearly always" is deliberate on BOTH ends: a hit at floor 0
is legitimate, not a bug. A mixed pair — a WAN sender stamped
dA=4 against a LAN receiver whose lead wobbles down inside the
dead band — can satisfy the schedule, and the answer is then the
server's REAL scheduled input rather than a stale hold. That is
a non-regression, and it is why the degenerate-equivalence test
pins byte-equivalence to empty/behind-store conditions only
rather than asserting it absolutely at floor 0 (review A4).

THE SAME FUNCTION T6'S RESIM CALLS, on purpose: a resim that
resolved a frontier tick differently from the prediction that
produced it would manufacture divergence out of nothing. Do not
inline a second copy of the ladder here.

THE TERMINAL VALUE IS THE INJECTED GAME ZERO. This branch used
to read `cached.value_or(typename T::InputType{})` — the same
(0,0,0)-forward poison T17 removed from the authority path, and
reached on EVERY tick of the D4 pre-registration / idle window
rather than only on loss. Rung 0 of the ladder answers
`store.fallback()`, which is the injected neutral until anything
arrives; a missing store answers the same neutral directly.
Sentinel-aware for free: `kNoInputCaptureTick` is never a store
key (push rejects it), so a sentinel can only ever resolve
through the same neutral, never through an InputType{}.

Nullable store lookup for the reason T5 documents on the
accessor: this branch iterates every simulatable, and a
character can be seen here before its registration completes.

The prediction tick is still advanced in lockstep with the
provider branch below — otherwise postPredictionAll keeps
overwriting a stale tick slot and every correction lands outside
the cache window. That part is unchanged by T7, and by T16:
the tick push is the slot allocation and it stays; only the
paired input write went with the column.

[T19] The read is CLASSIFIED here (probe 1 + probe 3). The ladder
itself stays stateless — it reports which rung answered through
`readReport` and this call site does the counting, which is also
what keeps prediction and resim counted separately. A missing
store is not a read at all and is deliberately not counted: there
was no probe to hit or miss, and folding it in would make a
not-yet-registered proxy look like starvation.
```

**Archived from the pre-task-14 header, lines 1730-1744.**

```
NoRef / REMOTE — no authoritative answer for this tick, so run the
same scheduled read the prediction pass runs (T7). Sharing the ladder
is what stops a resim from disagreeing with the prediction it replays.

[T19] SECOND CALL SITE OF PROBE 1 — counted into its OWN counter block,
never summed with the prediction one. Resim resolves ticks the
prediction already ran, and entries missing then may have landed since,
so a resim hit rate materially above the prediction's is real
information about the frontier rather than noise.

It does NOT feed the stale run (probe 3): a run is "consecutive ticks
this character was served a fallback", and resim revisits ticks out of
order and repeatedly. It also does not advance the window — the window
is keyed to the monotonic prediction tick, and a resim's `simTick`
walks backwards.
```

---

## §13 — What left for the telemetry sibling, and the retired sweep

Item 85's split, and the record of the frontier-allocating sweep that item 94 retired from this class.

**Archived from the pre-task-14 header, lines 1570-1580.**

```
[item 61] The `if (store != nullptr)` probe/log block plus the
unconditional `[CollectInput]` line below are ALL folded into
ONE emit* call — safe because nothing after this point branches
on whether the fold happened: the Skip/Stall handling and the
final `map.emplace` below run unconditionally either way,
exactly as they did with the diagnostics inline.

[task 79] `store != nullptr` rather than `store` itself: the
helper never dereferenced the pointer, only null-checked it, so
the telemetry sibling gets the one bit it needs instead of a
reference into this class's own RemoteInputCache map.
```

**Archived from the pre-task-14 header, lines 1595-1626.**

```
-----------------------------------------------------------------------
[og-netcode-v2-input-relay item 90] SWEEP 2 — FRONTIER ALLOCATION — IS
GONE FROM THIS CLASS. RETIRED AT ITEM 94.
-----------------------------------------------------------------------
The whole-tick sweep method and its per-character helper used to
live here, gated on a `queueMap` lookup (this class's own
`m_remoteMoveQueues`) plus item 92's loud-failure `OG_CHECK` against
`m_reconciliation.findInputCache`. Both are DELETED, not moved
verbatim: the replacement, Reconciliation's own frontier-allocating
sweep, filters on reconciliation's OWN cache population
(`findInputCache` alone, no `queueMap` reference — see that method's
banner for why this class's `m_remoteMoveQueues` was never
load-bearing for the exclusion, only a second, redundant way of
saying "has a cache") and silently skips rather than aborting loudly
(item 92's guard is traded away — priced, not hidden, at the new
site's banner). Grep proof this file no longer defines or calls the
relocated sweep by name — its acceptance criterion is a case-
insensitive zero-hit grep for the retired names across this whole
TU, which is why this note is deliberately written without spelling
them literally.

[item 61, relocated task 79, split at item 85] The local-provider
branch's classification line, the remote-queue branch's, and the
simulated-proxy branch's whole probing tail now live on the PT telemetry
sibling as `InputResolutionTelemetry::emitLocalInputRead` /
`emitRemoteQueueRead` / `emitPredictionInputRead`. The last of the three
takes `bool hasStore` rather than the `RemoteInputCache<InputT>*` this
class used to pass — the helper only ever null-checked the pointer, so
the one bit crosses instead of a reference into this class's own map
(see the call site above and `InputResolutionTelemetry.h` for the
ruling). See that header for the full bodies and comments, unchanged
otherwise from the pre-task-79 shape.
```

**Archived from the pre-task-14 header, lines 1752-1758.**

```
[item 61, relocated task 79, split at item 85] The five resim rungs'
classification lines (NoSlot, Sentinel, Local, NoStore, Ref) plus the
NoRef/scheduled rung's probe write + two SIMLOGs now live on the PT
telemetry sibling as `InputResolutionTelemetry::emitResimNoSlot` /
`emitResimSentinel` / `emitResimLocalRead` / `emitResimNoStore` /
`emitResimRefRead` / `emitResimScheduledRead`. See that header for the
full bodies and comments, unchanged from the pre-task-79 shape.
```

---

## §14 — Members, threading, and the two-thread roster

Every crossing this class participates in, and the argument for each.

**Archived from the pre-task-14 header, lines 552-578.**

```
-----------------------------------------------------------------------
[T9 part 3] Client-side Layer-1 input delay
-----------------------------------------------------------------------

The number of ticks the LOCAL client's own captured input is held before
the integrator sees it. One scalar, not a per-id map, because a delay is a
property of the local CONNECTION and a client has exactly one — the same
reason the server keys its queue by Address rather than by simulatable.

THE GAME->PHYSICS THREAD CROSSING, and why an atomic is sufficient here.
The writer is USimmableUpdateComponent::OnRep_ConnectionTier on the GAME
thread; the reader is collectInputAll on the PHYSICS thread, which
loads it ONCE per tick and uses that one value for the whole tick. This is safe
precisely because it is a lone independent scalar with no internal
structure: the worst a relaxed, one-tick-stale read can do is apply a tier
change one tick later than it landed. It is emphatically NOT the same
situation as T10's ServerInputDelayQueue, where the shared thing was an
unordered_map whose concurrent rehash is undefined behaviour rather than a
stale value — which is why that one needed the R2 restructuring and this
one does not.

Relaxed ordering is deliberate: there is no other datum whose visibility
this value has to order, so acquire/release would buy nothing.

Defaults to 0 = "no delay", i.e. exact pre-T9 behaviour, so an instance
nobody configures (isolated unit tests, the authority-role manager) is
unaffected.
```

**Archived from the pre-task-14 header, lines 1044-1047.**

```
[T9 part 3] ONE load per tick, used for every locally-controlled
simulatable below. Reading it once (rather than per id) is what makes
a concurrent OnRep_ConnectionTier write unable to split a single tick
across two different delays.
```

**Archived from the pre-task-14 header, lines 1055-1066.**

```
[T19] PROBES 1 + 3, per-window summary. Driven from here — once per
prediction tick, after every character has been resolved — for the same
reason ServerReceptionCoordinator drives [InputStats] from its
once-per-tick reapConnections hook: it is the only place with a
monotonic tick AND a guarantee of running exactly once per tick.

PHYSICS-THREAD WINDOW. The game-thread arrival probe has its own, separate
window object; see the two-thread statement at the top of
Network/RelayReadProbe.h for why they must not be merged.
[split at item 85] This call now reaches `InputResolutionTelemetry`,
not `NetSyncTelemetry` — see that header's own two-thread banner for
which of ITS methods this call site may reach.
```

**Archived from the pre-task-14 header, lines 1173-1176.**

```
ONE load per resim tick, mirroring collectInputAll's
one-load-per-tick rule so a concurrent OnRep cannot split a single
tick across two different delays. See the D2 note above for what it
does NOT promise.
```

**Archived from the pre-task-14 header, lines 1775-1777.**

```
m_inputProviders uses std::function intentionally — converting to a pointer struct
would require extending PredictionSyncedBufferOwnerConcept with a typed
getLocalInputFor<T>(step) method; tracked as a post-cutover follow-up.
```

**Archived from the pre-task-14 header, lines 1782-1796.**

```
[T2 / input relay] The capture tick behind each applied remote input, or
kNoInputCaptureTick on an underrun substitution. Written in
collectInputAll (PHYSICS thread), read in sendCorrectionAll (GAME
thread, on SimulationNetSync, via its m_inputResolution peer reference —
no forwarder, since item 87 promoted this class to a real peer and
deleted NetSync's scaffold-era forwarders) where T4 attaches it to the
correction state.

[T8] It used to be the strict parallel of `m_lastUsedInputs` and INHERITED
that member's pre-existing physics-write / game-read pattern rather than
introducing one. `m_lastUsedInputs` is now retired, so this member carries
that pattern alone — it is the same pattern, not a new one, and the argument
for why it is tolerable is unchanged: it is a plain scalar per id, so the
worst a torn schedule can do is carry the previous tick's value for one
tick, which the every-frame correction that transports it then supersedes.
```

**Archived from the pre-task-14 header, lines 1799-1801.**

```
[T9 parts 3+4] Client Layer-1 input delay. Populated for provider-owning
ids only; touched exclusively from collectInputAll (physics thread) and
wipeAllForResync.
```

**Archived from the pre-task-14 header, lines 1815-1824.**

```
[T5 / input relay] The client's relayed-input stores, one per REMOTE
character (provider-ABSENT ids — the complement of m_localInputCaches).

WRITTEN ON THE GAME THREAD (the OnRep_RelayedInputRing callback bound in
registerPredictionOwner), READ ON THE PHYSICS THREAD (T7's proxy branch of
collectInputAll, T6's collectResimInputAll). That crossing is INHERITED from
the correction path, which already does exactly this, and its full rationale
— the torn-slot-not-container-UB argument, the correction heal, and the
named deferred SPSC-seam cleanup — is in the THREADING section of
Network/RemoteInputCache.h. Do not restate it here; do not weaken it there.
```

**Archived from the pre-task-14 header, lines 1827-1832.**

```
[T19/T24/item 42, relocated task 79, split at item 85, OWNED HERE AT ITEM 86]
The PHYSICS-THREAD-ONLY telemetry sibling: `RelayReadProbe` and its ten
`emit*` helpers. See `InputResolutionTelemetry.h` for the probe
declaration, the fence on each method, and the class's own two-thread
rule. `NetSyncTelemetry` (the other, GAME-THREAD-ONLY sibling) stays on
`SimulationNetSync` — see that class's own member comment.
```

**Archived from the pre-task-14 header, lines 1835-1840.**

```
[T9 part 3] Written on the GAME thread (OnRep_ConnectionTier, via the
adapter's publishClientEffectiveInputDelayTicks calling this class's own
setClientEffectiveInputDelayTicks directly — no SimulationNetSync
forwarder since item 87), read once per tick on the PHYSICS thread.
Atomic — and ONLY atomic — for the reasons spelled out on
setClientEffectiveInputDelayTicks. 0 = pre-T9 behaviour.
```

---

## §15 — Corrections landed by task 14

Task 14 was a compression task. These three are **corrections, not
compressions**: the text they replace was false, and a text-preserving
compression would have preserved the falsehood in a shorter form. Each is
recorded here because the archive above still quotes the ORIGINAL, false wording
and a reader comparing the two must know which way the correction ran.

### 15.1 The self-contradiction in the simulated-proxy branch

`:1549-1554` said the prediction tick *"is still advanced in lockstep with the
provider branch below"* and that *"the tick push is the slot allocation and it
stays"*. `:1583-1589` — the same branch, thirty lines later — said
`pushPredictionTick` *"do not run in this class at all"*. The second is true:
`stepAllocatesFrontierSlot` has three production call sites and none is in that
branch. The paragraph survived items 90 and 94 and three reviews because it is
**written in the open present tense and names no symbol**, so no grep, lint or
glyph sweep could reach it. Its positional word was wrong too — the provider
branch is ABOVE, not below. **Removed.** The surviving guard at that site is the
`⛔ [item 90/94] backfillSkippedTick and pushPredictionTick do not run in this
class at all` fence, which was already correct.

### 15.2 The four-file stale-citation loop from item 96's relocation

Item 96 moved `preparePredictionSimulationStep` out of this header into
`SimulationStepSequencing.h` and updated the new file's top banner and nothing
else. Three sites in THIS file (`:1034`, `:1071`, `:1305`) still sent a reader to
`SimulationManager::onGameSimulationPrediction` for a fence and a debt paragraph
that had moved on. **All three now name `SimulationStepSequencing.h`.** Two
further sites in `SimulationStepSequencing.h` were corrected under the same task
on the lead's ruling — including a paragraph that said *"This lives here, in
`SimulationInputResolution.h`"* from inside the file it was denying. One site in
`SimulationManager.h` belongs to task 12 and was **not** touched.

### 15.3 The stale ownership labels item 89 still owes

Two of this file's 38 `SimulationNetSync` / `NetSync` mentions were wrong rather
than merely un-reworded; the other 36 were verified correct and left alone.
`:88` said *"Per-type map aliases for SimulationNetSync members"* over aliases
that have been this class's own since item 86. `:1331-1333` sent the reader to
`registerPredictionOwner` for a delay line that `registerLocalCharacter` — this
class — creates. **Both corrected, both annotated at the site.** The banner's
declaration that the remaining un-reworded prose is item 89's open debt is
preserved, because it is still true.

---

## Provenance

| | |
|---|---|
| Comment lines before / after | 1,244 / 645 |
| Inventory of prohibitions | 134 items, in the initiative workspace |
| Subject seal (taken before compressing) | sha256, in the initiative workspace |
| Checks | coverage 1:1, sealed subjects, code-only diff, both suites -- in the initiative workspace |
