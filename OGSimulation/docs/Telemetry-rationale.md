<!-- SPDX-License-Identifier: MPL-2.0 -->
# `NetSyncTelemetry.h` + `InputResolutionTelemetry.h` — rationale

The two headers keep a one-line guard at every site that has one, plus an orientation block
naming the owner, the threads, the call sites, the per-helper log volume and the severity
routing. **This file holds the full text those guards were compressed from** — the derivation,
the rejected alternatives, the measurement records and the migration history.

**One document for two headers**, because the pair carries one narrative: the probe cluster, the
single-`emit*`-call rule, the straight-fold versus decide/project split, and the
name-the-physical-quantity rule. The `§N` marks in either header resolve to the sections here.

**If this file and a header disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften a header to match it.

⛔ **Do not move a guard into this file.** Every section below is the *expansion* of a fence that
still fires at its own line. If you find yourself deleting a one-liner because "it is in the
rationale doc now", the guard has stopped working — the person about to make the change is
reading code, not documentation.

⚠ **This file quotes the headers as they stood BEFORE task 10's compression, and some of what it
quotes was FALSE when it was quoted.** Four passages asserted that `SimulationNetSync` owned both
telemetry siblings and that `relayReadProbe()` hung off `SimulationNetSync::Diagnostics`; item 87
had already moved both. Those passages are quoted verbatim here because that is what the archive
is for, and each is flagged at its section. **The corrected statement is in the headers'
orientation blocks, and nowhere else.** Do not repair a quotation in this file — it would stop
being a record.

> ⚠ **Every section body below is the VERBATIM pre-compression comment text**, emitted by
> `impl/task10/gen_doc_10.py` rather than retyped, so this file cannot have quietly edited what
> it copied. Bracketed tags (`[T19]`, `[task 79]`, `[item 85]`, `RN-10`) are provenance labels
> that resolve only in the `og-netcode-v2-input-relay` archive — they are not references, and
> nothing in this repository resolves them.

⚠ **One adapter binding the quoted text names.** `og-simulation` is engine-free — it names no
game-engine type and is reached from a host engine only through `concept`s. One quoted block names
**`Config/DefaultEngine.ini`**, which is one adapter's **host-application configuration surface**:
the place its shipped log-category levels are set, and the reason a `Log`-severity line does not
exist on a dedicated server while a `Warning` one does. The severity argument is the fact; the file
is one adapter's binding, and another adapter sets the same level under its own key. The quotation
is byte-verbatim and is not edited to say so.

<!-- lint-external-ref: RemoteInputCache::shouldLogVersionMismatchOnce -- RETIRED NAME (task 79): the per-store version-mismatch latch; relocated onto NetSyncTelemetry id-keyed, no successor on the store, must not resolve -->
<!-- lint-external-ref: SimulationNetSync::collectInputAll -- DEAD OWNER: collectInputAll moved to SimulationInputResolution at item 87; the qualified form must not resolve -->
<!-- lint-external-ref: NetSyncTelemetryTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->
<!-- lint-external-ref: RemoteInputCacheTest.cpp -- test translation unit in the og-simulation-tests submodule, not distributed with this submodule -->

---

## 1. The two siblings — ownership, the split, and what moved when

**`NetSyncTelemetry.h` pre-compression `:30-42` — HISTORY**

```
⚠ ITEM 85 SPLIT THIS CLASS ALONG ITS OWN TWO-THREAD BANNER (design C.6):
the PHYSICS-THREAD-ONLY half — `emitLocalInputRead`, `emitRemoteQueueRead`,
`emitPredictionInputRead`, the six `emitResim*`, `emitRelayReadWindowIfDue`
(+ its two now-private helpers), the `RelayReadProbe` member, and that
probe's `forgetOwner` half — MOVED OUT to `InputResolutionTelemetry.h`,
verbatim. THIS class keeps the GAME-THREAD-ONLY half: `emitRelayArrival`
(+ the version-mismatch latch), `emitCorrectionArrival` (+ the two
class-line helpers), the three GT probe members
(`RelayArrivalProbe`, `CorrectionVerdictProbe`, `CorrectionLandingProbe`),
and this class's own `forgetOwner` half. See `InputResolutionTelemetry.h`
for its half of this same banner, and design C.6 / this item's impl notes
for the full cut-line rationale. `docs/DiagnosticsConventions.md`'s roster
is updated to match.
```

**`NetSyncTelemetry.h` pre-compression `:44-48` — FENCE**

```
⚠ NETSYNC TEMPORARILY OWNS BOTH SIBLINGS. `InputResolutionTelemetry` does
not yet belong to an input-resolution peer — that peer does not exist
until item 86 — so `SimulationNetSync` holds one of each
(`m_telemetry` / `m_inputResolutionTelemetry`) for now. That is correct for
this step, not a leftover; see `InputResolutionTelemetry.h`'s own note.
```

**`InputResolutionTelemetry.h` pre-compression `:19-24` — HISTORY**

```
[og-netcode-v2-input-relay item 85 / step 1 of the input-resolution
migration] InputResolutionTelemetry — the PHYSICS-THREAD half of the sibling
task 79 created (`NetSyncTelemetry`), split out along that class's own
two-thread banner exactly as design C.6 specifies. This is the cut, not a
redesign: every member below moved here VERBATIM from `NetSyncTelemetry.h`;
nothing was renamed, reshaped or re-derived.
```

**`InputResolutionTelemetry.h` pre-compression `:26-32` — HISTORY**

```
WHY THE SPLIT HAPPENS BEFORE THE STATE MOVES (item 79's own gate logic,
restated at C.6): if the resolution peer's state and logic moved first,
each of the sixteen `emit*` helpers would have to be assigned to the
transport peer or the resolution peer WHILE the state was also moving —
two hard decisions entangled in one diff. This step finishes the
assignment while nothing else changes; step 2 (item 86) moves the state
behind an unchanged public surface with this split already settled.
```

**`InputResolutionTelemetry.h` pre-compression `:34-42` — HISTORY**

```
WHAT MOVED HERE (verbatim): the PHYSICS-THREAD-ONLY group of `emit*`
helpers — `emitLocalInputRead`, `emitRemoteQueueRead`,
`emitPredictionInputRead`, the six `emitResim*`, `emitRelayReadWindowIfDue`
(and, with it, its two now-private helpers `emitMissClassLine` /
`emitDeltaLine` — see the B-5 visibility note below) — the `RelayReadProbe`
member, and a `forgetOwner(id)` erasing the probe's id-keyed state.
`NetSyncTelemetry` keeps the GAME-THREAD-ONLY group unmodified in shape;
see that header for its own half of this same banner and design C.6 for
the full cut-line rationale.
```

**`InputResolutionTelemetry.h` pre-compression `:44-49` — FENCE**

```
⚠ NETSYNC TEMPORARILY OWNS BOTH SIBLINGS. This class is not yet owned by an
input-resolution peer — that peer does not exist until item 86 — so
`SimulationNetSync` holds one of each sibling for now
(`m_telemetry` / `m_inputResolutionTelemetry`) and calls straight into
both, unconditionally, on the same call sites it always used. That is
correct for this step, not a leftover.
```

---

## 2. Owner, not a diagnostics view — and the container reference that is never handed in

**`NetSyncTelemetry.h` pre-compression `:50-62` — FENCE**

```
⛔ THIS IS AN OWNER, NOT A DIAGNOSTICS VIEW — DiagnosticsConventions.md §2 STILL
APPLIES, UNCHANGED IN OUTCOME. SimulationNetSync's production methods call
straight into this class on every arrival / every correction — the
OnRep-bound relay-arrival and correction callbacks, and
`unregisterSimulatable`'s lifecycle cleanup, all reach a public method here,
unconditionally, on the game thread. Nothing on this class is reachable
from `SimulationNetSync::getDiagnostics()` — that view still exposes the
four probes, CONST, as it did before this split; three delegate into this
object's own const accessors below, one (`relayReadProbe()`) now delegates
into `InputResolutionTelemetry` instead — see
`Diagnostics::relayReadProbe()` etc. on `SimulationNetSync`. "instrument =
the sibling object" is still structure, not prose — this class (and its new
PT sibling) IS the fence.
```

**`NetSyncTelemetry.h` pre-compression `:64-72` — FENCE**

```
THE ONE THING DELIBERATELY NOT HANDED IN: a reference into a production
container. `emitRelayArrival` used to take the arriving `RemoteInputCache<InputT>&`
store purely to call its one-shot `shouldLogVersionMismatchOnce()` latch —
see that method below for why the latch moved IN here (id-keyed) instead of a
pointer moving in. Handing this class a live reference into
`SimulationNetSync`'s per-id maps would undercut the split this task exists to
make: a telemetry object that can read (or, worse, outlive) a production
container is no longer just an instrument. Unaffected by item 85 — the ruling
was never PT/GT-specific.
```

**`NetSyncTelemetry.h` pre-compression `:205-209` — NARRATIVE**

```
[RN-10 part B, task 79] ALL of the relay-ring arrival callback's probing +
logging, reusing the existing `emit*` verb rather than coining a new one.
See `docs/DiagnosticsConventions.md` §3 for the current `emit*` roster;
this and `emitCorrectionArrival` extend that convention rather than
inventing a second one.
```

**`InputResolutionTelemetry.h` pre-compression `:51-58` — FENCE**

```
⛔ THIS IS AN OWNER, NOT A DIAGNOSTICS VIEW — DiagnosticsConventions.md §2
STILL APPLIES, UNCHANGED IN OUTCOME. `SimulationNetSync`'s production
methods call straight into this class on every prediction / resim tick,
unconditionally, on the physics thread. Nothing on this class is reachable
from `SimulationNetSync::getDiagnostics()` except the one CONST probe
accessor below, exactly as before the split — see
`Diagnostics::relayReadProbe()` on `SimulationNetSync`, which now delegates
into THIS object's const accessor instead of `NetSyncTelemetry`'s.
```

**`InputResolutionTelemetry.h` pre-compression `:60-63` — FENCE**

```
THE ONE THING DELIBERATELY NOT HANDED IN: a reference into a production
container — unchanged from task 79's own ruling (see `NetSyncTelemetry.h`'s
file banner; nothing about that ruling was PT-specific, so it carries here
unmodified).
```

**`InputResolutionTelemetry.h` pre-compression `:145-152` — FENCE**

```
[item 61, task 79] Simulated-proxy branch's WHOLE probing tail: the
T19/T20 probe write (only when a store exists), its two rare-event
Verbose lines, and the unconditional per-tick [CollectInput]
classification line. `hasStore` replaces the pointer
`SimulationNetSync` used to pass — this method never dereferenced the
store, only null-checked it, so the caller now passes that one bit
rather than a reference into its own `RemoteInputCache` map (see the
file banner's note on not handing this class a production reference).
```

---

## 3. The two-thread rule, as a class property

**`NetSyncTelemetry.h` pre-compression `:75-77` — NARRATIVE**

```
THE TWO-THREAD RULE, AS A CLASS PROPERTY (task 79's stated deliverable; now
trivially true rather than merely stated, per item 85's split — every
method left on this class is GAME THREAD ONLY):
```

**`NetSyncTelemetry.h` pre-compression `:79-83` — FENCE**

```
GAME THREAD ONLY, relay-ring arrival (OnRep-bound, via
SimulationNetSync::onRelayedInputReceived):
emitRelayArrival, and with it `shouldLogVersionMismatchOnce` (public, but
production only ever reaches it from here — see that method's own note
on why it is exposed at all).
```

**`NetSyncTelemetry.h` pre-compression `:85-88` — FENCE**

```
GAME THREAD ONLY, correction-state arrival (OnRep-bound, via
SimulationNetSync::onCorrectionReceived):
emitCorrectionArrival (which is also the sole caller of
emitCorrectionVerdictClassLine / emitCorrectionLandingClassLine).
```

**`NetSyncTelemetry.h` pre-compression `:90-93` — FENCE**

```
EITHER THREAD, LIFECYCLE ONLY (never concurrent with the above by
construction — see `unregisterSimulatable`'s own ordering comment):
forgetOwner (this class's GT half — see `InputResolutionTelemetry.h`
for the PT half, called from the same unregistration site).
```

**`NetSyncTelemetry.h` pre-compression `:95-97` — FENCE**

```
CONST, either thread: the three probe accessors — read-only, and the
reason `SimulationNetSync::Diagnostics` may call them from wherever a
test likes.
```

**`NetSyncTelemetry.h` pre-compression `:99-106` — FENCE**

```
No method here is ever called from both threads, so nothing on this class is
atomic — the same property the probe split existed to buy, now stated as a
method-level fence instead of an object-level one (and, since item 85, also
an OBJECT-level one: this class is single-threaded in its entirety, bar the
never-concurrent `forgetOwner`). This complements, and does not restate,
`Network/RelayReadProbe.h`'s own "two objects because there are two
threads" banner: that one is about the PROBE TYPES; this one is about which
of THIS class's METHODS may be called from which thread.
```

**`InputResolutionTelemetry.h` pre-compression `:77-79` — NARRATIVE**

```
THE TWO-THREAD RULE, AS A CLASS PROPERTY — now trivially true rather than
merely stated, because every method left on this class is PHYSICS THREAD
ONLY:
```

**`InputResolutionTelemetry.h` pre-compression `:81-87` — FENCE**

```
PHYSICS THREAD ONLY — reached only from SimulationNetSync::collectInputAll
/ collectResimInputAll and their per-character helpers:
emitLocalInputRead, emitRemoteQueueRead, emitPredictionInputRead,
emitResimNoSlot, emitResimSentinel, emitResimLocalRead, emitResimNoStore,
emitResimRefRead, emitResimScheduledRead, emitRelayReadWindowIfDue
(which is also the sole caller of the private emitMissClassLine /
emitDeltaLine).
```

**`InputResolutionTelemetry.h` pre-compression `:89-91` — FENCE**

```
EITHER THREAD, LIFECYCLE ONLY (never concurrent with the above by
construction — see `unregisterSimulatable`'s own ordering comment on
`SimulationNetSync`): forgetOwner.
```

**`InputResolutionTelemetry.h` pre-compression `:93-94` — FENCE**

```
CONST, either thread: the one probe accessor — read-only, and the reason
`SimulationNetSync::Diagnostics` may call it from wherever a test likes.
```

**`InputResolutionTelemetry.h` pre-compression `:96-97` — NARRATIVE**

```
This is the SAME method-level fence `NetSyncTelemetry.h`'s banner states for
its own (now GAME-THREAD-ONLY) half — split along the banner, not rewritten.
```

---

## 4. `CorrectionArrivalDecision` — the fields, and the probing order they encode

**`NetSyncTelemetry.h` pre-compression `:110-110` — NARRATIVE**

```
[task 60 / RN-10 part C, relocated task 79] THE CORRECTION-ARRIVAL DECISION.
```

**`NetSyncTelemetry.h` pre-compression `:112-119` — FENCE**

```
Free struct rather than nested in SimulationNetSync (unlike before this task):
`SimulationNetSync::decideCorrectionArrival` (the PURE half — computes this,
touches no probe, calls no `note*`/`emit*`/`log*`) still lives on the peer and
still returns this by value; `NetSyncTelemetry::emitCorrectionArrival` below
(the PROJECTION half) is this struct's one consumer. It needs a name both
classes can see without one owning the other, hence file scope here rather
than nested in either — the same reason `ScheduledRelayedReadDecision` at the
top of SimulationNetSync.h is a free template struct rather than nested.
```

**`NetSyncTelemetry.h` pre-compression `:121-132` — FENCE**

```
This callback's probing has an early `return` interleaved between two probe
calls (`noteLanding` always runs; `noteCorrection` must NOT run on a discard),
so folding "the probing" into one helper the way `emitRelayArrival` does would
move what that `return` returns from — the verdict probe would then run on
discarded corrections and silently corrupt item 41's `aboveNewest` population
(the discard bucket `CorrectionLandingProbe` counts). `landed` is therefore a
FIELD of the decision, not a mid-block `return`, exactly as
`ScheduledRelayedReadDecision::isUnderflowMiss` is a field rather than a
re-derivable fact (see the banner above that struct in SimulationNetSync.h).
`characterClass` / `landingSite` are meaningful even when `!landed` — the
landing probe needs them unconditionally, see the "hoisted above the gate"
note in `decideCorrectionArrival` in SimulationNetSync.h.
```

**`NetSyncTelemetry.h` pre-compression `:139-141` — FENCE**

```
Mirrors CorrectionInsertVerdict::landed. Kept as its own field —
rather than re-derived from `landingSite == Discarded` — so the two
notions can never silently diverge in the projection below.
```

**`NetSyncTelemetry.h` pre-compression `:144-149` — FENCE**

```
The correction's tick, echoed back so the caller's log line needs no
second decode of the wire buffer. Set on BOTH paths — mirrors
CorrectionInsertVerdict::tick's own rule. `emitCorrectionArrival`'s
`[Verbose][ResimProbe.Landing]` SIMLOG relies on this: it prints
`decision.tick` unconditionally, BEFORE the `!landed` gate below, so a
discarded correction's landing-probe line still carries the wire tick.
```

**`NetSyncTelemetry.h` pre-compression `:152-154` — FENCE**

```
Meaningless when `!landed` (CorrectionInsertVerdict's own rule) — unlike
`tick` above, this one really is discard-invalid: no comparison happened,
so it must never be read as either an agreement or a disagreement.
```

---

## 5. The relay-arrival probe and its per-window line

**`NetSyncTelemetry.h` pre-compression `:211-224` — FENCE**

```
⚠ [task 79 / THE ONE REAL DESIGN DECISION] NO `RemoteInputCache&` PARAMETER
HERE, UNLIKE THE PRE-TASK-79 SHAPE. The only thing this method ever needed
from the store was its one-shot `shouldLogVersionMismatchOnce()` latch —
"have I already warned about THIS character's incompatible peer, this
session" — and handing this class a live reference into
`SimulationNetSync`'s per-id map would be exactly the production-container
coupling this split exists to avoid. The latch itself MOVED here instead,
id-keyed (`m_versionMismatchLogged`), because log-suppression state
belongs with the logger, not with the data container being logged about.
`RemoteInputCache::shouldLogVersionMismatchOnce()` and its backing bool are
retired with zero remaining callers (see `Network/RemoteInputCache.h`);
`RemoteInputCacheTest.cpp`'s four assertions against it move to
`NetSyncTelemetryTest.cpp` against this method instead — see this task's
impl notes for the full before/after.
```

**`NetSyncTelemetry.h` pre-compression `:227-231` — FENCE**

```
[T19] PROBE 2 — replication cadence, measured in CAPTURE
TICKS. GAME-THREAD window, a different object from the
physics-side read probe; see the two-thread rule at the top of
this file (and Network/RelayReadProbe.h's own probe-level
statement, which this complements).
```

**`NetSyncTelemetry.h` pre-compression `:233-235` — FENCE**

```
`report.newestCaptureTick` is the newest tick THIS RING
carried, not the newest the store holds — the distinction is
load-bearing and the report field's own comment explains it.
```

**`NetSyncTelemetry.h` pre-compression `:240-248` — FENCE**

```
⛔ [T34 loss-counter fix] `newCaptureTicksIngested`, NOT
`entriesIngested` and NOT a hard-coded 1. It is the count
of capture ticks this arrival made newly resident, and it
is what turns `lostCaptureTicksX1000` from a measure of
the BURST RATE into a measure of loss. `entriesIngested`
would count re-delivered ticks as new coverage and hide
real loss; a hard-coded 1 is the retired replace-latest
premise and is exactly what reported ~120 per mille on a
working flush.
```

**`NetSyncTelemetry.h` pre-compression `:256-259` — FENCE**

```
Per-event Verbose, and only when the cadence actually
hiccuped. A gap of exactly 1 is the healthy depth-1
steady state and would be a per-tick line; the interesting
events are the stalls, which are what set the depth rule.
```

**`NetSyncTelemetry.h` pre-compression `:270-274` — FENCE**

```
PER-WINDOW SUMMARY AT WARNING — the cadence
[InputStats] already uses. p99 is what the
`depth >= gap_p99 + margin` rule reads; the mean is
deliberately absent because it hides the tail that
sets depth.
```

**`NetSyncTelemetry.h` pre-compression `:276-286` — FENCE**

```
⭐ [T34] `lostCaptureTicksX1000` IS THE R = 0 LOSS
INSTRUMENT, and it is on this line rather than its
own because it is derived from these same samples.
WARNING, not Log, is load-bearing:
`Config/DefaultEngine.ini` sets `LogOGNet=Warning`, so
a Log line does not exist on a dedicated server —
items 35 and 36 each cost this initiative a proof line
for exactly that. Steady-state expectation ~ 11 per
mille (the measured 1.122 % wire loss); the raw
numerator and denominator ride along so a window can
be re-derived rather than trusted.
```

**`NetSyncTelemetry.h` pre-compression `:288-294` — FENCE**

```
⭐ [T34 rework] `discont=` IS PART OF THE GATE, not
garnish. A window reporting `discont=` > 0 was
interrupted (see kRelayArrivalDiscontinuityTicks) and
must be DISCARDED rather than averaged in — the same
rule `[RelayProbe.Write] discont=` already carries.
`discontMax=` is the largest excluded gap, exact, so
discarding a window never hides how bad it was.
```

**`NetSyncTelemetry.h` pre-compression `:296-302` — FENCE**

```
⭐ [T34 loss-counter fix] `delivered=` IS THE FIELD
THAT MAKES THIS LINE SELF-CHECKING. `lost + delivered
== expected` must hold on every window; a reader who
sees it fail knows the delivered count is not being
plumbed and that `lostCaptureTicksX1000` is measuring
the burst rate again. Before this field existed, that
failure mode was indistinguishable from a lossy wire.
```

---

## 6. The correction projection, and the two per-class window lines

**`NetSyncTelemetry.h` pre-compression `:334-340` — FENCE**

```
[task 60 / RN-10 part C, relocated task 79] THE PROJECTION — the landing
probe fires UNCONDITIONALLY (a discard IS an observation, item 41's
`aboveNewest` population); the verdict probe only if `decision.landed`.
THIS `return` IS THE SAME CONTROL-FLOW FACT the pre-split inline lambda
encoded with its mid-block `if (!verdict.landed) return;` — moved here,
not removed, and reading `decision.landed` rather than re-deriving it
from `landingSite`, per that field's own comment above.
```

**`NetSyncTelemetry.h` pre-compression `:361-364` — FENCE**

```
A correction whose tick had no slot was DISCARDED — no comparison
happened. Counting it would put a denominator under a verdict that
was never reached, and the discard path already logs itself
(isAnomalousMiss-gated, in the cache).
```

**`NetSyncTelemetry.h` pre-compression `:366-372` — FENCE**

```
[item 42] THE LANDING PROBE ABOVE DELIBERATELY SITS ON THE OTHER
SIDE OF THIS RETURN. Its `discarded` bucket is the one place the
two probes' sample sets are required to differ, and moving this
gate up would silently empty it. [RN-10 part C] `decision.landed`
carries the exact same fact `verdict.landed` did before the split
— see `SimulationNetSync::decideCorrectionArrival`'s comment for
why it is a field rather than re-derived here.
```

**`NetSyncTelemetry.h` pre-compression `:376-384` — FENCE**

```
PER-EVENT DETAIL AT VERBOSE — off under the shipped
LogOGDivergenceProbe=Warning. Emitted on EVERY landed correction
rather than on disagreements only, because "per-correction verdict
observable with id and class" is the acceptance criterion and a
disagreement-only line cannot distinguish "predicted correctly"
from "no correction arrived". The cost is one snprintf at a site
that already performs two per correction ([InjectCorrectionState]
and the cache's own line), so this adds no new volume CLASS — the
thing T19 was filed to stop.
```

**`NetSyncTelemetry.h` pre-compression `:396-399` — FENCE**

```
PER-WINDOW SUMMARY AT WARNING, ONE LINE PER CLASS. Never one
pooled line: only the remote half can move with the relay delay
floor, and summing it with a locally-predicted population that
cannot move would dilute exactly the signal T23 scenario 4 reads.
```

**`NetSyncTelemetry.h` pre-compression `:401-404` — FENCE**

```
A class with no corrections in the window is SKIPPED rather than
printed as `rate=0`, which would read as a perfect record instead
of as no observation. That is also the steady state on a client
with no remote proxies.
```

**`NetSyncTelemetry.h` pre-compression `:411-413` — FENCE**

```
[T24, relocated task 79] ONE per-window class block of the
correction-verdict summary. Called twice — once per class — from
`emitCorrectionArrival` above, and ONLY when a window closed.
```

**`NetSyncTelemetry.h` pre-compression `:415-420` — FENCE**

```
SILENT ON AN EMPTY CLASS. `corrections == 0` means this window observed
nothing about that class; printing `disagreed=0 ratePerMille=0` would assert
a perfect record where there is no record at all, and that misreading is
precisely the one a benefit claim would be built on. It is also the steady
state for the remote block on a client that has no proxies and for the local
block on a spectator, so gating it keeps those sessions quiet as well.
```

**`NetSyncTelemetry.h` pre-compression `:422-423` — FENCE**

```
ONE LINE PER CLASS, NEVER ONE POOLED LINE — the reason is at the top of
Network/CorrectionVerdictProbe.h and is the whole point of the split.
```

**`NetSyncTelemetry.h` pre-compression `:441-443` — FENCE**

```
[og-netcode-v2-input-relay item 42 / I2, relocated task 79] ONE per-window
class block of the frontier-landing split. Called twice — once per class —
from `emitCorrectionArrival` above, and ONLY when a window closed.
```

**`NetSyncTelemetry.h` pre-compression `:445-449` — FENCE**

```
SILENT ON AN EMPTY CLASS, same rule and same reason as the verdict line
above: printing `behind=0 atFrontier=0 discarded=0` would assert a perfect
record where there is no record at all, and that is the misreading this whole
instrument exists to prevent. It is also the steady state for the remote
block on a client with no proxies.
```

**`NetSyncTelemetry.h` pre-compression `:451-461` — NARRATIVE**

```
⭐ HOW TO READ THE PAIR THIS LINE FORMS WITH `[ResimProbe.Gate]`. Under the
mechanism, resim triggers track `atFrontier` and are blind to `behind`. So:
* `atFrontierPerMille` here ~= `requestedPerMille` on the Gate line  ⇒ the
finding's central claim reproducing live;
* `behind` large with the Gate line's `requested` small  ⇒ the suppressed-
correction population, i.e. the under-resimulation statement itself;
* `discarded` large  ⇒ item 41's `aboveNewest` anomaly, whose fix will MOVE
this mix (it is referenced here, not solved here).
The two lines cannot be merged into one: they are fed by different threads
and item 42 requires one probe per thread with no sharing. See the cost note
at the top of ResimGateProbe.h.
```

---

## 7. The version-mismatch latch

**`NetSyncTelemetry.h` pre-compression `:324-325` — FENCE**

```
ONCE per component per session — an incompatible peer
re-replicates its ring forever.
```

**`NetSyncTelemetry.h` pre-compression `:479-487` — FENCE**

```
[task 79] ONE-SHOT gate for the wire-version-mismatch log, id-keyed —
relocated from `RemoteInputCache::shouldLogVersionMismatchOnce` (see the
file banner and `emitRelayArrival` above for why it moved rather than
being handed in by reference). Returns true exactly once per id, i.e.
exactly once PER CHARACTER PER SESSION — the same cadence the fence
required when the latch lived on the store, because an incompatible peer
re-replicates its ring forever and an ungated log would fire on every
single replication. Forgotten in `forgetOwner` for the same
unbounded-memo reason `m_relayArrivalProbe` is.
```

**`NetSyncTelemetry.h` pre-compression `:489-493` — FENCE**

```
PUBLIC, same as the store's version was: this is the one piece of
task 79 that is genuinely NEW behaviour rather than a relocation (a
per-store latch becoming an id-keyed one on a shared object), and
`NetSyncTelemetryTest.cpp` exercises it directly rather than only
indirectly through `emitRelayArrival`'s no-logger path.
```

**`NetSyncTelemetry.h` pre-compression `:535-535` — NARRATIVE**

```
[task 79] See `shouldLogVersionMismatchOnce` above.
```

---

## 8. The three game-thread probe members, and the forget asymmetry

**`NetSyncTelemetry.h` pre-compression `:167-176` — FENCE**

```
[task 59, retargeted task 79, split at item 85] THE THREE GT PROBE
ACCESSORS — CONST-ONLY, same contract as before this task.
`SimulationNetSync::Diagnostics` delegates into these rather than
reading its own members directly; every test call site on that view is
unchanged (see the diagnostics banner on SimulationNetSync for the full
rationale, and `docs/DiagnosticsConventions.md` §2/§3 for the
classification these views follow — not re-derived here).
`relayReadProbe()` MOVED to `InputResolutionTelemetry` at item 85 (it
was the PT-only probe); `SimulationNetSync::Diagnostics::relayReadProbe()`
now delegates there instead of here.
```

**`NetSyncTelemetry.h` pre-compression `:182-190` — FENCE**

```
[T19, relocated task 79, split at item 85] LIFECYCLE CLEANUP, NOT
DIAGNOSTICS — called once from `SimulationNetSync::unregisterSimulatable`,
itself unchanged: step 4 of that fixed, load-bearing ordering, ALONGSIDE
`InputResolutionTelemetry::forgetOwner` (the PT half — see that class).
`m_relayArrivalProbe` holds an id-keyed map (the stale run), and without
this it would grow with every character that has ever existed in the
session — the unbounded-memo shape this codebase has already had to fix
once in the log throttles. The version-mismatch latch (below) is the
same shape and is forgotten here for the same reason.
```

**`NetSyncTelemetry.h` pre-compression `:192-198` — FENCE**

```
`m_correctionVerdictProbe` / `m_correctionLandingProbe` are deliberately
NOT forgotten here, and the asymmetry is by design rather than an
omission: neither keeps any id-keyed map at all. Their counters are
per-CLASS aggregates across characters, so an unregistered character
leaves nothing to erase and the class totals for the window it was part
of stay correct. Forgetting something here would need per-id state to
exist first, which would be a different metric.
```

**`NetSyncTelemetry.h` pre-compression `:502-510` — FENCE**

```
[T19; SPLIT AT ITEM 85] THE CLIENT-SIDE RELAY ARRIVAL PROBE. Pure
telemetry: nothing in the resolution path reads it, and every consumer
is a SIMLOG. GAME thread only — its PHYSICS-thread sibling
(`RelayReadProbe`, formerly declared adjacent here) moved to
`InputResolutionTelemetry` at item 85; the two were always owned by a
DIFFERENT thread each (see that class's own note and
Network/RelayReadProbe.h's header banner for why they were ever two
objects rather than one — unchanged by the split, only their storage
location moved).
```

**`NetSyncTelemetry.h` pre-compression `:513-519` — FENCE**

```
[T24] THE CORRECTION-VERDICT PROBE. Also pure telemetry, also client-side,
and — unlike `m_relayArrivalProbe` above — a SINGLE object, because it has
a single feeder: the OnRep-dispatched correction-state callback, on the
GAME thread. There is no physics-thread correction arrival, so there is
no second window to keep apart. It also carries no per-id state, which is
why `forgetOwner` above forgets `m_relayArrivalProbe` and not this one.
Full statement at the top of Network/CorrectionVerdictProbe.h.
```

**`NetSyncTelemetry.h` pre-compression `:522-528` — FENCE**

```
[og-netcode-v2-input-relay item 42 / I2] THE FRONTIER-LANDING SPLIT. Same
feeder, same thread and same no-per-id-state rule as the verdict probe above
— it is deliberately a second object on the same site rather than three more
fields on the first, because the two answer different questions on different
denominators (a DISCARDED correction is a non-event for the verdict and a
first-class observation for the landing site) and merging them would have
forced one of the two to adopt the other's sample set.
```

**`NetSyncTelemetry.h` pre-compression `:530-532` — FENCE**

```
ITS PHYSICS-THREAD SIBLING IS ON SimulationManager (ResimGateProbe). The two
are never shared and never atomic — see the two-object rule at the top of
OGSimulation/ResimGateProbe.h.
```

---

## 9. The resolution path's classification lines and its rare-event detail

**`InputResolutionTelemetry.h` pre-compression `:128-129` — NARRATIVE**

```
[item 61] Local-provider branch's classification line — a plain,
unconditional single-statement fold (Pattern 1); nothing gates it.
```

**`InputResolutionTelemetry.h` pre-compression `:137-138` — NARRATIVE**

```
[item 61] Remote-queue branch's classification line — same shape as
`emitLocalInputRead` above.
```

**`InputResolutionTelemetry.h` pre-compression `:159-160` — FENCE**

```
[T20] The WHOLE report, not just the outcome: the miss class
and the signed probe-to-newest delta are tallied here too.
```

**`InputResolutionTelemetry.h` pre-compression `:163-170` — FENCE**

```
PER-EVENT DETAIL AT VERBOSE, AND ONLY FOR THE OUTCOME THAT IS
SILENT IN THE STEADY STATE — the [RelaySkip] precedent exactly.
A verify-fail means the delay regime moved under this reader,
which does not happen while the schedule is stable, so this
costs nothing per tick in the ordinary case. Hits and misses
are RATES and are reported by the per-window summary; emitting
them per event would be another per-tick line, which is the
thing this task exists to stop adding.
```

**`InputResolutionTelemetry.h` pre-compression `:181-187` — FENCE**

```
[T20] THE OTHER OUTCOME THAT IS SILENT IN THE STEADY STATE.
missInSpan and missAboveNewest are the two expected classes and
are RATES — the per-window summary reports them. A read landing
BELOW the oldest resident entry is different in kind: it means
the receiver's clock has drifted out of the store's 64-tick
reach, which should not happen at all, so it gets the same
rare-event treatment the verify-fail line gets.
```

**`InputResolutionTelemetry.h` pre-compression `:203-203` — NARRATIVE**

```
[item 61] NoSlot rung — single unconditional line, straight fold.
```

**`InputResolutionTelemetry.h` pre-compression `:209-209` — NARRATIVE**

```
[item 61] Sentinel rung — same shape as `emitResimNoSlot` above.
```

**`InputResolutionTelemetry.h` pre-compression `:215-215` — NARRATIVE**

```
[item 61] Local/delay-line rung.
```

**`InputResolutionTelemetry.h` pre-compression `:222-222` — NARRATIVE**

```
[item 61] Remote/no-store rung.
```

**`InputResolutionTelemetry.h` pre-compression `:228-228` — NARRATIVE**

```
[item 61] Remote/Ref rung.
```

**`InputResolutionTelemetry.h` pre-compression `:235-240` — FENCE**

```
[item 61] Remote/NoRef rung — the ONE rung with a probe write
(`noteResimRead`) plus two SIMLOGs (one gated on VerifyFail, one
unconditional). Folds cleanly: nothing after this call in the caller's
`collectResimInputForCharacter` branches on whether the probe fired —
the `map.emplace`/`return` there run unconditionally either way, same as
they did with the diagnostics inline.
```

**`InputResolutionTelemetry.h` pre-compression `:243-243` — FENCE**

```
[T20] The whole report — same reason as the prediction site.
```

---

## 10. The relay-read window, the miss partition and the delta line

**`InputResolutionTelemetry.h` pre-compression `:65-74` — FENCE**

```
[B-5 CORRECTION, folded in at item 85] `emitMissClassLine` / `emitDeltaLine`
were PUBLIC on `NetSyncTelemetry` (design C.6 called them private, which was
wrong on the current-tree fact at design time — review finding B-5(i)).
On THIS sibling they are made PRIVATE: their sole caller
(`emitRelayReadWindowIfDue`) is a member of this same class, no test or
production call site reaches them directly (grep-verified against
`NetSyncTelemetryTest.cpp` and the og-brawler wiring tests before this
change), and private is now reachable where it was not worth fighting for
on the pre-split, single-sibling class. Decision recorded here and in this
item's impl notes; `DiagnosticsConventions.md` updated to match.
```

**`InputResolutionTelemetry.h` pre-compression `:259-264` — FENCE**

```
[T19, relocated task 79, split here at item 85] PROBES 1 + 3 — the
per-window summary. Called once per prediction tick from
`SimulationNetSync::collectInputAll`; silent unless a window both
CLOSED and carried at least one scheduled read, so the authority (no
relay stores, so neither call site is ever reached) and an idle client
never heartbeat a Warning line.
```

**`InputResolutionTelemetry.h` pre-compression `:266-269` — FENCE**

```
TWO LINES, ONE PER CALL SITE, plus a third only when a stale run occurred.
Split rather than concatenated because a single line carrying both blocks runs
close to SIMLOG's 256-byte buffer once the counts reach five digits, and a
silently truncated telemetry line is worse than no line.
```

**`InputResolutionTelemetry.h` pre-compression `:294-299` — FENCE**

```
[T20] PROBE B — the miss PARTITION and the signed-delta distribution, per
call site. SEPARATE LINES rather than more fields on the two above: the
existing lines already run to ~130 characters and SIMLOG's buffer is 256,
so folding ten more five-digit counters in would silently truncate exactly
when the counts get interesting. Each is gated so a call site that carried
nothing (the resim block on a client that never resimmed) stays silent.
```

**`InputResolutionTelemetry.h` pre-compression `:305-308` — FENCE**

```
PROBE 3 — the D4 stale window, which is what sets `K` for the deferred
stale-hold rule. Silent when nothing went stale, which is the healthy
state; rung-0 serves are excluded from the run (review F5), so a join
window does not produce one.
```

**`InputResolutionTelemetry.h` pre-compression `:319-326` — FENCE**

```
[T20, relocated task 79, MADE PRIVATE at item 85 — B-5 correction] PROBE
B — the miss partition for ONE call site. Silent when that call site
missed nothing, so a healthy window costs no line. Sole caller is
`emitRelayReadWindowIfDue` above, in this same class; no test or
production call site reached it directly on `NetSyncTelemetry` either
(grep-verified before this change), so private is reachable here where
it was carried public on the pre-split class only because the sibling
it lived on had no reason to narrow it.
```

**`InputResolutionTelemetry.h` pre-compression `:328-332` — FENCE**

```
THE THREE COUNTERS ARE THE WHOLE POINT OF T20 and they answer three different
questions: `inSpan` is the coverage hole raising the relay depth would close;
`aboveNewest` is the delay deficit, which depth cannot touch; `belowOldest` is
a clock or capacity fault. `noProbeTick` is the early-session underflow guard,
reported alongside so the four always visibly sum to `miss`.
```

**`InputResolutionTelemetry.h` pre-compression `:350-358` — FENCE**

```
[T20, relocated task 79, MADE PRIVATE at item 85 — B-5 correction] PROBE
B — the signed `probeTick - newestResident` distribution for ONE call
site. At depth 1 this is the richer signal: it says WHERE the receiver
is asking relative to what it holds, continuously, rather than in three
buckets. A window whose p50 sits above 0 is a receiver reading ahead of
its data (no depth helps); one whose p50 sits below 0 while missing is
reading inside a span full of holes (depth does). Sole caller is
`emitRelayReadWindowIfDue` above, same reasoning as
`emitMissClassLine`'s visibility note.
```

---

## 11. The physics-thread probe member and its read seam

**`InputResolutionTelemetry.h` pre-compression `:107-112` — FENCE**

```
[task 59, retargeted task 79, split here at item 85] THE PROBE
ACCESSOR — CONST-ONLY, same contract as before this split.
`SimulationNetSync::Diagnostics::relayReadProbe()` delegates into this
rather than reading its own member directly; the og-brawler wiring
tests that call it are unmodified in assertion content (task 59's
standard, re-verified at item 85 — see this item's impl notes).
```

**`InputResolutionTelemetry.h` pre-compression `:115-122` — FENCE**

```
[T19, relocated task 79, split here at item 85] LIFECYCLE CLEANUP, NOT
DIAGNOSTICS — called once from `SimulationNetSync::unregisterSimulatable`
(itself unchanged: step 4 of that fixed, load-bearing ordering), ALONGSIDE
`NetSyncTelemetry::forgetOwner` — both halves are called from that one
site; see `SimulationNetSync.h` for the paired call. Without this the
probe's id-keyed stale-run state would grow with every character that
has ever existed in the session — the same unbounded-memo shape the log
throttles already had to fix once.
```

**`InputResolutionTelemetry.h` pre-compression `:380-385` — FENCE**

```
[T19] THE CLIENT-SIDE RELAY READ PROBE. Pure telemetry: nothing in the
resolution path reads it, and every consumer is a SIMLOG. PHYSICS
thread only — see the two-thread rule at the top of this file. Its
GAME-thread sibling (`RelayArrivalProbe`) stays on `NetSyncTelemetry`;
see that header and `Network/RelayReadProbe.h`'s own banner for why
they were ever two objects rather than one.
```

---

