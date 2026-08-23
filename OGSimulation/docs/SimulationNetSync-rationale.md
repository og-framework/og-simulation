<!-- SPDX-License-Identifier: MPL-2.0 -->
# `SimulationNetSync.h` — rationale archive

**Relocation history, retired rationale and archived measurement records for
`OGSimulation/SimulationNetSync.h`.** The header keeps every guard that fires at
a site; this file keeps the derivation behind each one, verbatim, as it stood
immediately before the wave-5 compression of 2026-08-22.

## ⛔ How to read this file, and how not to

1. **This is an ARCHIVE, not a specification.** Where this file and the header
   disagree, **the header is authoritative.** Nothing here is maintained against
   the tree; the header's guards are.
2. **Every block below is quoted VERBATIM from the pre-compression header** and
   is emitted by `impl/task11/gen_doc_11.py`, not retyped. Re-running that script
   reproduces this document byte-for-byte. That is what makes the compression
   reversible and non-lossy.
3. ⚠ **Some quoted text was ALREADY FALSE OR MISLEADING when it was quoted.**
   The known cases are named in §0 below. **Do not repair the quotations** — an
   archive that edits itself to stay true stops being a record of what was there.
   The corrections live in the header, at the site, where a reader will meet them.
4. **No reader should be sent here for anything except provenance.** The header
   names this file exactly once.
5. ⛔ **The line numbers in the block headings below are PRE-COMPRESSION
   coordinates into a 1,281-line file that no longer exists.** They are archive
   labels, not anchors; they resolve against nothing in the tree. The header is
   936 lines today. Nothing in this document cites a live line number, and
   nothing should cite these (R12).

## §0.0 — ⚠ Adapter bindings the quoted text names — one adapter's, never the binding

`og-simulation` is engine-free: it names no game-engine type and is reached from a host engine only
through `concept`s. The archived comments below were written inside a project that binds it to one
particular engine, and several name that engine's symbols. **Those names are one adapter's binding,
not part of the core's contract**, and rule 3 above forbids editing a quotation to remove one — so
this table is where the ROLE each plays is given, and a reader with no engine reads the blocks
through it.

| name in the quotes | the ROLE it plays for the core | note |
|---|---|---|
| `FRelayedInputRing` | one adapter's **wire type for the relay ring** — the replicated carrier that moves a connection's relayed inputs to observing peers | the core sees only the accessor/callback pair; §-numbered blocks below say so explicitly |
| `FInputRedundancyBundle` | one adapter's **wire type for the local-input RPC**, carrying the most-recent `redundancyDepth` ticks in one unreliable send | the core handles only its decoded contents |
| `USimmableUpdateComponent` | one adapter's **per-character component**: it registers the character with the core and owns the replicated buffers the core's callbacks are bound to | |
| `Iris` | that engine's **replication system** — the party that batches, drops and rolls back property writes | role words: *the replication system* / *the replication poll* |
| `Config/DefaultEngine.ini` | one adapter's **host-application configuration surface**, where shipped policy and log values are set | another adapter supplies the same values under its own keys |

⛔ **None of these is a dependency of this core**, and none of them appears in `SimulationNetSync.h`
as code. Where a block below uses one bare, it is this table that scopes it.

## §0 — Claims that were false or misleading at the moment they were archived

| where, in the pre-compression header | the claim | the tree |
|---|---|---|
| the `decideCorrectionArrival` landing-site block (§12) | *"after item 46's flip to `OnDisagreement`"*, in the future tense | `OnDisagreement` has been the **shipped configuration** since item 43, via `Config/DefaultEngine.ini`'s `[OGNetcode] ResimTriggerPolicy`. `FrontierExact` is the **compiled default** only. The inversion the block describes as future **is already live in every real session.** Recorded as `Verdict.md` §4.3 / finding F-B. The header now states the prohibition and **points at `TimeConfig::resimTriggerPolicy`**, which is the one place that keeps the two halves apart — this task deliberately did **not** author a second wording of that fact. |
| the `emitRelayArrival` note (§4) | *"a reference to this class's own `RemoteInputCache` map"* | `SimulationNetSync` has owned no `RemoteInputCache` map since item 86; the container families moved to `SimulationInputResolution`. True when written, false when archived. The header's surviving guard drops the possessive. |
| the `m_telemetry` declaration (§4) | *"their **six** `emit*` helpers"* and *"`RelayReadProbe` + **ten** `emit*` helpers"* | **`NetSyncTelemetry` declares FOUR; `InputResolutionTelemetry` declares TWELVE.** Both halves were wrong. ⚠ `6 + 10 = 16` and `4 + 12 = 16`, so the split agreed with the file's *other*, correct count of sixteen — which is why it never looked wrong, and why no existence check could see it. Corrected at the site, not compressed. |

⚠ **This list is what I verified, not a guarantee of completeness.** It is a
floor. Two of the three were found by reading the file; the third by counting
declarations in the sibling headers. **No anchor lint and no coverage figure can
find this class at all**, because every symbol in all three claims resolves —
which is exactly why the count was wrong for two items and nothing reported it.

<!--
  ESCAPE DECLARATIONS for tools/lint/doc_anchor_lint.ps1.

  Every name below is quoted VERBATIM inside an archived block and MUST NOT be
  de-backticked to satisfy the lint: the quotations ARE the archive, and editing
  one to make a checker happy is the exact failure this document exists to avoid.
  Each identifier was verified to have ZERO hits in comment-stripped code across
  the whole repository on 2026-08-22 (impl/task11/retired_names_11.txt), so
  "retired" below is a measured statement, not an assumption.
-->
<!-- lint-external-ref: Verdict.md -- og-source-doc-extraction initiative workspace; not distributed with this submodule -->
<!-- lint-external-ref: injectCorrectionInput -- RETIRED at T8 with the server->client correction-INPUT channel; quoted verbatim in an archived block -->
<!-- lint-external-ref: getSyncedCorrectionInputBuffer -- RETIRED at T8; the authority owner's outbound input buffer is gone -->
<!-- lint-external-ref: clearOnCorrectionInputReceivedCallback -- RETIRED at T8; the channel it unbound no longer exists -->
<!-- lint-external-ref: m_lastUsedInputs -- RETIRED at T8; the staged value behind the retired input echo -->
<!-- lint-external-ref: m_isResimulated -- RETIRED at item 45; the resim gate is edge-triggered on an explicit anchor now -->
<!-- lint-external-ref: getLastResimulationTick -- RETIRED at item 45 together with the frontier scan it drove -->
<!-- lint-external-ref: allocateFrontierSlotForCharacter -- RETIRED at item 94; frontier allocation is storage-driven on reconciliation's own cache population -->

---

## §1 — Traits, owner structs, and what item 86 moved out

### archived block — PRE-COMPRESSION lines 40–43 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F2)

*primary template INTENTIONALLY undefined; defining it costs the clean missing-specialization compile error*

```text
Primary template — intentionally undefined. Each simulatable type must
specialize this in the TestYo layer (or wherever UE types are available),
declaring PredictionOwnerType and AuthorityOwnerType. Undefined primary
gives a clean compile error if a specialization is missing.
```

### archived block — PRE-COMPRESSION lines 55–58 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F2 + F5a)

*no std::function / no per-id heap alloc; sizeof == sizeof(void*) enforced by static_assert in the test files*

```text
Owner-bound pointer structs — no std::function, no per-id heap allocation.
Single pointer; all surrounding per-tick state (storage slot, last-used
input, pending queue) is looked up in the call body by id, not captured here.
sizeof must equal sizeof(void*) — enforced via static_assert in test files.
```

### archived block — PRE-COMPRESSION lines 74–81 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1c + F3)

*D4 ABSENCE FENCE + the map of where the five container families went; 'what stays here is exactly the two owner-BINDING maps'*

```text
[item 86] The per-type map aliases for the FIVE input container families,
the provider map, the join-key map and the ladder free functions all moved
to `SimulationInputResolution.h` (verbatim — design §F). What stays here is
exactly the two owner-BINDING maps below: `SimulationNetSync` keeps the
owner concepts, `SimulatableOwnerTraits`, and the registration bindings;
the container LIFECYCLE these two maps used to be populated alongside now
lives on the resolution peer (see `registerPredictionOwner` /
`registerAuthorityOwner` / `unregisterSimulatable` below).
```

### archived block — PRE-COMPRESSION lines 1098–1099 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a)

*callers NEVER name the owner template parameters directly; they are resolved through the traits*

```text
Owner types are resolved via SimulatableOwnerTraits<SimulatableT>;
callers never name the owner template parameters directly.
```


## §2 — The buffer and owner concepts — the compile-time wire contract

### archived block — PRE-COMPRESSION lines 91–91 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*titles the concept helpers below*

```text
Concept helpers — declared here so concepts below can reference them.
```

### archived block — PRE-COMPRESSION lines 93–105 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F2 + F5c)

*the write/read MIRROR PAIR enumerated; unconstrained accessors -> runtime wire-format corruption, not a compile error*

```text
CompositeSyncedBufferConcept encodes the wire-format contract of every
tick-stamped replicated buffer in the system: it must support writing a
composite with a tick, and reading one back, returning the tick. The two
are the exact mirror pair used by SimulationNetSync::sendCorrectionAll /
sendLocalInputToAuthorityAll (write side) and
SimulationReconciliation::injectCorrectionState +
SimulationNetSync::registerAuthorityOwner RPC callback (read side).
([T8] `injectCorrectionInput` used to be named here as a third read-side
consumer; the correction-input channel is retired.)
Constraining buffer accessors through this concept is what guarantees
the two sides stay in lockstep — breaking either the write or the readInto
signature becomes a compile error at the registerSimulatable call site,
not a runtime "tick=1056398093" wire-format corruption.
```

### archived block — PRE-COMPRESSION lines 120–126 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F2)

*T4's ref is a REFINEMENT not an extension; requiring it on every buffer puts a meaningless field on the input buffer*

```text
[og-netcode-v2-input-relay T4] The CORRECTION-STATE buffer carries one field
the input buffer does not: the per-tick applied-capture-tick reference (the
join key between the state channel and the relayed-input channel). It is
therefore a REFINEMENT of the shared composite contract rather than an
extension of it — requiring the ref on every tick-stamped buffer would put a
meaningless field on the input buffer, whose payload IS an input and has no
"which input produced this" question to answer.
```

### archived block — PRE-COMPRESSION lines 128–132 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5c + F2)

*writer sendCorrectionAll / reader injectCorrectionState must stay in lockstep; both halves required in one place*

```text
Both halves are required in one place for the same reason
CompositeSyncedBufferConcept exists: the writer (sendCorrectionAll) and the
reader (injectCorrectionState) must stay in lockstep, so breaking either side
is a compile error at the registerSimulatable call site rather than a silent
wire-format divergence.
```

### archived block — PRE-COMPRESSION lines 142–143 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F5d)

*ONE call, so a publish can NEVER pair this tick's state with the previous tick's ref*

```text
Write the state AND the ref together. One call, so a publish can never
pair this tick's state with the previous tick's ref.
```

### archived block — PRE-COMPRESSION lines 148–156 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5c + F2)

*callback bind AND bind-time ring read are both required, else a proxy silently gets a store nothing fills*

```text
[T5 / input relay] The prediction owner additionally exposes the RELAY RING.
It was the THIRD replicated channel when T5 wrote this, alongside the
correction-state and correction-input buffers; **[T8] it is now the second of
two** — the correction-input channel is retired and the relay ring is what
replaced it. Both halves are required here for the same reason the
buffer concepts are: registerPredictionOwner binds the arrival callback AND
reads the ring once at bind, so an owner that grew only one of the two would
fail at the registerSimulatable call site rather than silently leaving a proxy
with a store nothing ever fills.
```

### archived block — PRE-COMPRESSION lines 158–160 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F3)

*the UE ring type is NEVER named here; it arrives as an associated type and is consumed through the codec concept*

```text
The ring type is UE-side (`FRelayedInputRing`) and never named here — it arrives
as `OwnerT::RelayedInputRingType` and is consumed only through the
engine-agnostic codec's BUFFER CONCEPT, exactly as the sync buffers are.
```

### archived block — PRE-COMPRESSION lines 186–190 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F3)

*the FInputRedundancyBundle wire type NEVER appears here -- only core PendingInputQueue + scalars*

```text
The local-input RPC is the unreliable + redundancy FInputRedundancyBundle
path. The owner builds the bundle (a UE wire type opaque to this UE-free
layer) from the most-recent `redundancyDepth` ticks still held in
`pendingQueue` and fires the unreliable RPC. The bundle type never appears
here — only the core PendingInputQueue + scalar params do.
```

### archived block — PRE-COMPRESSION lines 209–211 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5a + F1c)

*the remote-move callback is PER-SLOT, invoked once per (capture_tick, input); the bundle type stays UE-side*

```text
The remote-move callback is per-slot. The owner unpacks the inbound
FInputRedundancyBundle and invokes this callback once per
(capture_tick, input) slot — the bundle type stays UE-side.
```

### archived block — PRE-COMPRESSION lines 675–677 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F5c)

*write(composite, tick, appliedCaptureTick) MUST stay in lockstep with the client-side readInto()*

```text
Wire format is fully encapsulated by the buffer's write(composite, tick,
appliedCaptureTick). Must stay in lockstep with the client-side readInto()
in SimulationReconciliation::injectCorrectionState.
```


## §3 — The retired server→client correction-INPUT channel (T8)

### archived block — PRE-COMPRESSION lines 175–178 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1c + F3)

*SyncedRemoteInputBufferType SURVIVES T8 in its client->server role; the server->client requirement pair is gone*

```text
SyncedRemoteInputBufferType survives T8 in its OTHER role: it is the
CLIENT->SERVER buffer handed back by getClientToServerInputSyncedBuffer
below. What is gone is its server->client role — the correction-input
arrival callback pair used to be required right here.
```

### archived block — PRE-COMPRESSION lines 194–202 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3 + F2)

*D4 ABSENCE FENCE: the outbound input buffer is gone from this concept; leaving it dangling costs every future owner a member*

```text
[og-netcode-v2-input-relay T8] THE AUTHORITY OWNER NO LONGER OWNS AN OUTBOUND
INPUT BUFFER. `getSyncedCorrectionInputBuffer` and, with it, the
`SyncedRemoteInputBufferType` typedef + composite-buffer constraint are gone
from this concept: sendCorrectionAll's input write was the sole consumer of
both, and the authority's outbound surface is now exactly the correction STATE
buffer (carrying T4's applied-capture-tick ref) plus the relay ring the T3 sink
writes through the component. The constraints were removed rather than left
dangling because a concept that still demanded a buffer for a role nothing
serves would make every future authority owner carry a member for nothing.
```

### archived block — PRE-COMPRESSION lines 432–437 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3 + F5c)

*D4 ABSENCE FENCE + the SET: a prediction owner binds EXACTLY TWO inbound channels (Q3)*

```text
[og-netcode-v2-input-relay T8] The correction-INPUT arrival binding that
sat here is gone. A prediction owner now binds exactly two inbound
channels: the correction STATE (above, carrying T4's applied-capture-tick
ref) and — for remote characters only — the relay ring (in the
provider-absent branch above). The input a proxy runs on comes from the
ring, resolved by capture tick, not from a per-tick server echo.
```

### archived block — PRE-COMPRESSION lines 539–540 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3)

*D4 ABSENCE FENCE: clearOnCorrectionInputReceivedCallback was here; nothing to clear*

```text
[T8] `clearOnCorrectionInputReceivedCallback` was here; the channel it
unbound is retired, so there is nothing to clear.
```

### archived block — PRE-COMPRESSION lines 576–580 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **HISTORY** (STEP3)

*'used to publish TWO things ... the second write is gone' -- strike the past-tense sentences and nothing survives*

```text
[og-netcode-v2-input-relay T8] THE CONTRACT HALF OF T3's EXPAND/CONTRACT
PAIR. This method used to publish TWO things per character per tick: the
corrected state, and the input the authority applied ("here is what I ran
you on"). The second write is gone, and with it the whole SERVER->CLIENT
correction-INPUT channel:
```

### archived block — PRE-COMPRESSION lines 582–591 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3)

*TIEBREAK-A / D4: the ten-symbol retirement inventory. Zero hits for every name, so F3 fires mechanically*

```text
core   : m_lastUsedInputs + LastUsedInputMapFor          (the staged value)
StateCorrectionCache::insertCorrectionInput      (the sink)
StateCorrectionCache::getLastCorrectionInput     (the reader)
StateCorrectionCache::m_containsCorrectionInput  (the flag)
receiveCorrectionInput                           (the decoder)
SimulationReconciliation::injectCorrectionInput  (the router)
the two owner-concept requirements + the OnRep binding
UE     : USimmableUpdateComponent::m_replicatedInputSyncedBuffer, its
OnRep_CorrectionInput, its DOREPLIFETIME registration, its
callback plumbing and getSyncedCorrectionInputBuffer
```

### archived block — PRE-COMPRESSION lines 733–735 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3)

*D4 ABSENCE FENCE at the exact line the second write occupied*

```text
[T8] The second write — the input buffer, plus its
`[SendRemoteInputToClients]` trace — used to sit here. See the
retirement block above.
```


## §4 — The diagnostics view, and the probe split across items 79 / 85 / 87

### archived block — PRE-COMPRESSION lines 249–257 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1c + F5c)

*m_logger STAYS on this class; the non-emit* SIMLOG call sites are enumerated and never moved*

```text
[task 79, split at item 85; item 87] `m_telemetry` is this class's
OWN telemetry sibling — the resolution peer's `InputResolutionTelemetry`
is seeded by the composition root calling
`SimulationInputResolution::setLogger` directly now that the peer is
no longer a scaffold sub-object (see that method's own comment).
`m_logger` stays on this class too — the SIMLOG call sites that are
NOT part of the sixteen `emit*` helpers (e.g. [ReceiveLocalInput],
[SendCorrectionStateToClients]) never moved and still use it
directly.
```

### archived block — PRE-COMPRESSION lines 275–280 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*heading + the Grade-4 pointer to docs/DiagnosticsConventions.md ('not re-derived here')*

```text
[og-netcode-v2-input-relay task 59, retargeted task 79] THE THREE PROBE
ACCESSORS' DIAGNOSTIC VIEW — RN-9 + amendment, grouped per
`docs/DiagnosticsConventions.md` (the classification rule these views
follow is centralised there, not re-derived here). A sibling ruling
groups `SimulationManager`'s own resim-gate probe accessor the same
way, in the same task.
```

### archived block — PRE-COMPRESSION lines 282–296 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F3)

*ONLY THESE THREE READ-ONLY ACCESSORS MOVE; relayReadProbe() is absent here and reached on the peer's own view*

```text
⛔ ONLY THESE THREE READ-ONLY ACCESSORS MOVE. The probe MEMBERS, the
`note*` call sites that feed them every frame, and the `emit*` helpers
are production instrumentation and are UNTOUCHED by this view — task 79
relocated all four onto sibling telemetry objects; item 85 split the PT
group onto `InputResolutionTelemetry`; item 87 dropped this class's own
two-hop `relayReadProbe()` delegation entirely (design §C.6: "the
resolution peer's `getDiagnostics()` exposes `relayReadProbe()`;
NetSync's keeps the other three") — callers reach it directly at
`inputResolution.getDiagnostics().relayReadProbe()` now that the peer
is no longer a scaffold sub-object. Each remaining accessor's NAME,
COUNT and const-only shape is unchanged. See `NetSyncTelemetry.h` and
`SimulationInputResolution.h` / `InputResolutionTelemetry.h` for the
probe declarations and the fence comment on each, and
`docs/DiagnosticsConventions.md` §2/§3 for the current roster and
count.
```

### archived block — PRE-COMPRESSION lines 298–304 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F3)

*D4 ABSENCE FENCE: CONST-ONLY DELIBERATELY -- there is no editDiagnostics() on this class, unlike StateCorrectionCache's pair*

```text
CONST-ONLY AND DELIBERATELY SO: the only writers are on the telemetry
siblings, each reachable from exactly one thread (see each class's own
two-thread banner), and a mutable handle handed out here would be an
invitation to increment a physics-thread counter from the game thread —
the exact hazard the two-object split exists to prevent. There is
therefore no `MutableDiagnostics` / `editDiagnostics()` on this class,
unlike `StateCorrectionCache`'s pair.
```

### archived block — PRE-COMPRESSION lines 306–308 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F2)

*nested class not a free function; the alternative needs a friend declaration*

```text
Nested class, not a free function: it has the same access to
SimulationNetSync's private members as any other member function — no
friend declaration needed, and the view holds nothing but a reference.
```

### archived block — PRE-COMPRESSION lines 317–321 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1d + F5c)

*defends its own PUBLIC placement against narrowing; names the og-brawler-tests wiring block as the consumer*

```text
[T24] Same contract, same reason. Its consumer is the og-brawler-tests
wiring block: the probe's arithmetic is unit-tested in og-simulation-tests,
but only a suite with SimulatableOwnerTraits bound to concrete owners can
show that the shipped correction callback actually feeds it AND classifies
by the same provider-presence test the rest of this class uses.
```

### archived block — PRE-COMPRESSION lines 325–331 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1d + F5c)

*same, for the landing probe; names the DISCARD-bucket behaviour only a real-owner suite can show*

```text
[og-netcode-v2-input-relay item 42 / I2] Same contract, same consumer, same
reason as the verdict probe's accessor directly above: the three-way
classification is swept as a unit in og-simulation-tests, but only a suite
that can register a real local and a real remote character against the real
callback can show that the shipped site feeds it, files each landing under
the right class, and puts a DISCARD in the discarded bucket rather than
dropping it on the verdict probe's early return.
```

### archived block — PRE-COMPRESSION lines 835–843 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3)

**** FALSE TODAY *** D4 absence ('no store argument crosses this call') but 'this class's OWN RemoteInputCache map' ended at item 86*

```text
[RN-10 part B, relocated task 79] CALL SITE 1's probing + logging now
lives on the telemetry sibling — `NetSyncTelemetry::emitRelayArrival` —
reusing the existing `emit*` verb rather than coining a new one. See
`docs/DiagnosticsConventions.md` §3 for the current `emit*` roster.
No `store` argument crosses this call: the helper's only use of it was
the one-shot version-mismatch latch, which moved INTO the telemetry
object, id-keyed, rather than being handed a reference to this class's
own `RemoteInputCache` map — see `NetSyncTelemetry.h`'s file banner and
`emitRelayArrival`'s own comment for the full ruling.
```

### archived block — PRE-COMPRESSION lines 988–993 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5c + F3)

*the projection is not here; its new home is also the SOLE CALLER of the two class-line helpers*

```text
[RN-10 part C, relocated task 79] THE PROJECTION now lives on the
telemetry sibling — `NetSyncTelemetry::emitCorrectionArrival` — which
is also the sole caller of `emitCorrectionVerdictClassLine` and
`emitCorrectionLandingClassLine` (both relocated with it; neither had
any other caller). See `NetSyncTelemetry.h` for the full body and its
comments, unchanged from the pre-task-79 shape.
```

### archived block — PRE-COMPRESSION lines 995–1001 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3)

*D4 ABSENCE FENCE: probes 1+3's per-window summary is not here; names where it is and who calls it (Q1)*

```text
[T19, relocated task 79, split at item 85, resolution peer owns the PT
sibling at item 86] PROBES 1 + 3 — the per-window summary now lives on
`SimulationInputResolution`'s `InputResolutionTelemetry` sibling as
`emitRelayReadWindowIfDue`, called once per prediction tick from
`SimulationInputResolution::collectInputAll`. See
`InputResolutionTelemetry.h` for the full bodies and comments,
unchanged otherwise from the pre-task-79 shape.
```

### archived block — PRE-COMPRESSION lines 1008–1018 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5c + F3)

*the probe ROSTER, enumerated per sibling, and the shrunk Diagnostics surface (Q1)*

```text
[T19/T24/item 42, relocated task 79, SPLIT AT ITEM 85] THE GAME-THREAD
telemetry sibling — `RelayArrivalProbe`, `CorrectionVerdictProbe`,
`CorrectionLandingProbe` and their six `emit*` helpers. The
PHYSICS-THREAD sibling (`RelayReadProbe` + ten `emit*` helpers) is
owned by the resolution peer (`m_inputResolution` below) — see
`NetSyncTelemetry.h` and `InputResolutionTelemetry.h` for the probe
declarations, the fence on each, and each class's own two-thread rule.
[item 87] Diagnostics access shrinks with the split: `Diagnostics`
above now exposes exactly the three GAME-THREAD probes this sibling
owns; `relayReadProbe()` is reached directly at
`inputResolution.getDiagnostics().relayReadProbe()` (design §C.6).
```


## §5 — Registration — the cross-peer set invariant and the two branches

### archived block — PRE-COMPRESSION lines 345–345 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*names the delegating free function*

```text
Client overload registration helpers — called from registerSimulatable free function.
```

### archived block — PRE-COMPRESSION lines 347–357 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F5c)

*THE CROSS-PEER SET INVARIANT: sender-map keys subset of provider-set, maintained SOLELY by two named methods in one fixed sequence*

```text
[item 86 / design §C.4's closing paragraph] THE CROSS-PEER SET
INVARIANT. Before this item, "m_inputProviders / m_pendingInputQueues /
m_localInputSenders populated as a set" was an INTRA-class invariant —
all three lived here. Post-cut, providers+queues live on
`m_inputResolution`, senders stay here: the invariant is now
*sender-map keys ⊆ provider-set*, maintained SOLELY by this method (and
`registerAuthorityOwner` below) calling the resolution peer's
container-lifecycle methods and this class's own map insert in the SAME
fixed sequence, every time. `sendLocalInputToAuthorityAll` is the one
consumption point, and it enforces the invariant loudly
(`findPendingInputQueue` + `OG_CHECK`) rather than assuming it.
```

### archived block — PRE-COMPRESSION lines 371–372 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F6)

*TIEBREAK-A. `if (inputProvider)` reads as 'a provider was supplied'; it MEANS 'locally controlled'. The identifier misleads*

```text
Input provider is present iff this owner drives a locally-controlled
simulatable.
```

### archived block — PRE-COMPRESSION lines 382–385 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5c + F1a)

*remote branch: peer creates the neutral-seeded store, this class binds two callbacks BY ID ONLY -- no captured container reference*

```text
[T5] REMOTE character (no provider) — the resolution peer's
registerRemoteCharacter creates the neutral-seeded relay store;
this class binds the two callbacks that feed it, by id only
(design §C.3 — no captured container reference).
```

### archived block — PRE-COMPRESSION lines 451–456 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F5c)

*the cross-peer invariant's other half; this method's own insert is the matching step of the same fixed sequence*

```text
[item 86 / design §C.4's closing paragraph] THE CROSS-PEER SET
INVARIANT'S OTHER HALF — see the full statement at
registerPredictionOwner above. This call seeds the resolution
peer's remote-move queue + join-key entry; this method's own
`m_authorityWriters` insert below is this class's matching half of
the same fixed sequence.
```

### archived block — PRE-COMPRESSION lines 493–503 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5a + F1c)

*by-id routing makes a late fire a benign lookup miss; the guard STAYS NetSync's and crosses BY VALUE; rejection semantics (Q3)*

```text
RPC inbound — [item 86] now routes through the resolution peer's
by-id `queueRemoteMove` door instead of a captured `&remoteQueue`
reference (design §C.3): a late-firing callback after
unregisterCharacter's erase becomes a benign lookup miss instead of
a dangling reference. Guard context crosses in BY VALUE from this
class's own members — the guard stays NetSync's (design §C.4). The
queue dedups by capture_tick (first-writer-wins) and rejects
too-far-future capture ticks against the guard context published by
SimulationManager via setAuthorityGuardContext (current authority tick +
TimeConfig::rollbackWindowTicks). A too-far-future drop is warned here so the
queue stays logger-free.
```

### archived block — PRE-COMPRESSION lines 758–765 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5a + F5c)

*findPendingInputQueue + OG_CHECK is LOUD not silent: the missing queue IS the cross-peer invariant violation*

```text
[item 86 / design §C.4] `findPendingInputQueue` + `OG_CHECK` replaces
the pre-cut `.at(id)` into `m_pendingInputQueues` (now owned by the
resolution peer): the cross-peer set invariant (sender-map keys ⊆
provider-set, see registerPredictionOwner's comment) means a queue
MUST exist for every id enumerated here, so the check is loud
rather than silent — an id present in m_localInputSenders without a
matching pending queue is exactly the invariant violation the two
registration sites exist to prevent.
```

### archived block — PRE-COMPRESSION lines 903–908 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F6 + F1a)

*THE CLASS TEST IS PROVIDER-PRESENCE -- the SAME lookup, not a second notion of 'remote'; read live so it cannot drift*

```text
THE CLASS TEST IS PROVIDER-PRESENCE — [item 86] routed through the
resolution peer's `isLocallyControlled` query, THE identity test
(design §C.1/§A.2) — the SAME lookup registerPredictionOwner forks
on above and collectInputAll forks on every tick, not a second
notion of "remote". Read live rather than captured at bind time so
it cannot drift from the map that actually decides behaviour.
```


## §6 — The bind-time catch-up read, and why latch-and-replay is refused here

### archived block — PRE-COMPRESSION lines 388–390 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5b + F5c)

*CALL SITE 1: fires on the GAME thread, store read on the PHYSICS thread (Q2)*

```text
CALL SITE 1 — arrival. Fires on the GAME thread (see the THREADING
section in Network/RemoteInputCache.h); the store is read on the
physics thread.
```

### archived block — PRE-COMPRESSION lines 396–400 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5c + F2)

*CALL SITE 2 is a bind-time catch-up read; without it the store stays empty until the next relay write (Q3)*

```text
CALL SITE 2 — POPULATE ONCE AT BIND, through the same by-id door
the OnRep callback uses. Closes the hole where the ring
replicated (and its OnRep fired into a null callback) before this
registration ran, and the sender then went quiet: without this the
store would stay empty until the next relay write.
```

### archived block — PRE-COMPRESSION lines 402–409 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F2)

*DO NOT COPY T10/T11's LATCH-AND-REPLAY PATTERN HERE -- the ring is its own latch. Unmarked prohibition; Q3's hazard*

```text
DO NOT COPY T10/T11's LATCH-AND-REPLAY PATTERN HERE — this is the
likely mistake precisely because the two immediately-preceding tasks
established the opposite shape. Tier and floor are change-notification
-only SCALARS: a notification that fires before the listener binds is
never repeated, so the value has to be latched. The relay ring is a
PERSISTENT PROPERTY — re-reading it always yields the full current
state, so THE PROPERTY IS ITS OWN LATCH. Latch machinery here would
earn nothing and would add a second copy of the ring to keep in sync.
```

### archived block — PRE-COMPRESSION lines 411–413 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F4 + F5a)

*UNREACHABLE-BY-DESIGN: on the authority the bound callback can never fire, because OnRep is a client-only notification (Q3)*

```text
On the AUTHORITY this reads the server's own (as-yet-unwritten) ring
and no-ops on version 0; the callback it binds can never fire there,
because OnRep is a client-only notification.
```

### archived block — PRE-COMPRESSION lines 415–422 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F2)

*DELIBERATELY NOT FED TO THE CADENCE PROBE; counting it puts a phantom sample on the role with no relay traffic*

```text
[T19] DELIBERATELY NOT FED TO THE CADENCE PROBE. This is a bind-time
catch-up read, not an arrival: no replication event occurred, and on
the authority it runs at all — so counting it would put a phantom
sample in the histogram on the one role that has no relay traffic.
The cost is that the first real OnRep seeds the watermark instead of
producing a gap, which is exactly one lost sample per component per
session. The report is discarded here, unread — `ingestRelayRing`'s
own comment states the same rule from the peer side.
```


## §7 — The neutral-injection role guard

### archived block — PRE-COMPRESSION lines 459–459 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*heading for the neutral-injection guard below*

```text
[T17] THE NEUTRAL-INJECTION ROLE GUARD.
```

### archived block — PRE-COMPRESSION lines 461–469 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F2)

*DELIBERATELY NOT A HARD FAILURE: the degraded value is a legal input; aborting a live session would be worse than the defect*

```text
Registration is the moment this character's authority path goes live, and
the very next authority tick takes collectInputAll's remote branch with an
empty queue — i.e. underruns, and substitutes the injected neutral. A
composition root that injected on the client role only would therefore
integrate `InputType{}` ((0,0,0) forward vectors) for the whole join
window, every join, with nothing to say so. This warns at the site where
that first becomes reachable. Deliberately not a hard failure: the
degraded value is still a legal input, and aborting a running session over
it would be worse than the defect.
```

### archived block — PRE-COMPRESSION lines 471–484 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1c + F3)

*the warning STAYS and IS KEPT after T8 removed the seed it was written for; names the ordering assumption that dissolved*

```text
[T8] T17 ALSO SEEDED `m_lastUsedInputs` HERE, and that seed is gone with
the correction-input channel it existed to replicate (AM-1's "the seed
reaches peers before the first applied input" reasoning was entirely about
that channel). Two consequences worth stating rather than leaving a reader
to infer:
* The warning STAYS and its site is still right — it now guards the
underrun substitute alone, which is the one authority consumer left.
* The ORDERING ASSUMPTION T17 documented here DISSOLVES. The substitute
re-reads m_neutralInputs on every underrun tick, so a setNeutralInput
arriving after registration now does fix subsequent ticks; nothing is
read once and frozen any more. The warning is consequently a
"not injected yet, and this character is already live" signal rather
than a permanent-damage one. It is kept because that window is still
real and still silent otherwise.
```


## §8 — Unregistration — four ordered steps

### archived block — PRE-COMPRESSION lines 521–527 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F1c)

*ORDERING IS LOAD-BEARING: step 1 MUST precede step 3; KEPT UNCHANGED rather than loosened now its failure mode is cheaper (Q3)*

```text
Centralized unregister — fixed order; ordering is load-bearing.
Step 1 MUST precede step 3: the pre-item-86 onRemoteMoveReceived captured
&remoteQueue from m_remoteMoveQueues; [item 86] the callbacks now route by
id through the resolution peer, so a late fire is a benign lookup miss
rather than UB — but clearing callbacks first, before any lifecycle erase,
remains the contract every registered owner is bound under, and is kept
unchanged rather than loosened now that its failure mode is cheaper.
```

### archived block — PRE-COMPRESSION lines 535–535 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d)

*step 1 runs BEFORE any data-map erase*

```text
Step 1: clear RPC-inbound callbacks on the owner(s) — before any data-map erase.
```

### archived block — PRE-COMPRESSION lines 548–548 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*step 2 label, no consequence stated*

```text
Step 2: erase writer structs.
```

### archived block — PRE-COMPRESSION lines 552–554 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*step 3 label + the pointer to where the container-lifecycle erase lives*

```text
Step 3: [item 86] the container-lifecycle erase — five container
families' entries plus the join key — now lives on the resolution
peer; see SimulationInputResolution::unregisterCharacter.
```

### archived block — PRE-COMPRESSION lines 557–567 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F5c)

*step 4 drops BOTH telemetry siblings' per-id state, one call each, as a fixed step of the same ordering*

```text
[T19, relocated task 79, split at item 85, resolution peer owns the
PT sibling at item 86] Step 4: drop BOTH telemetry siblings' per-id
state (the two relay probes' id-keyed maps plus the
version-mismatch log-once latch) — one call each, same fixed step
of the same ordering that lived here before task 79 moved the
probes off this class. `inputResolution.forgetOwner` forwards to
its own `InputResolutionTelemetry` sibling. See
`NetSyncTelemetry::forgetOwner` and
`InputResolutionTelemetry::forgetOwner` for the full rationale
(including why the two correction probes are deliberately NOT
forgotten by either).
```


## §9 — sendCorrectionAll — the own-character echo drop and T39's rotation

### archived block — PRE-COMPRESSION lines 593–598 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (STEP3)

*what replaced the channel; survives the strike as a present-tense mechanism statement*

```text
WHAT REPLACED IT, and why this is a net removal rather than a trade: a
remote character's input now reaches peers on the RELAY RING, keyed by
CAPTURE tick and stamped with the schedule (T1/T3/T5), and the correction
state carries a 4-byte applied-capture-tick REF (T4) naming which capture
the authority ran. Identity plus one scalar replaced a whole replicated
input composite, so the per-tick wire cost of this send goes DOWN.
```

### archived block — PRE-COMPRESSION lines 600–606 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F3)

*D4 ABSENCE FENCE: the relay ring DELIBERATELY does not replace the own-character echo; there is NO such truth channel at all*

```text
---- OWN-CHARACTER DROP (InputRelayDesign.md §7, coverage gap 3) ----------
The retired channel also fired on the OWNING client — OnRep_CorrectionInput
ran on every peer, including the one whose input it echoed. The relay ring
deliberately does not replace that half: relay stores are created only for
provider-ABSENT ids (registerPredictionOwner), so a client gets no relay
store for its own character and there is now NO server-applied-input truth
channel for the local character at all.
```

### archived block — PRE-COMPRESSION lines 608–625 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F2)

*'A sparse-state increment MUST RE-EXAMINE this drop, NOT ASSUME it' -- an explicit re-open condition*

```text
WHY THAT IS SAFE THIS INCREMENT, stated so a future reader does not have to
re-derive it:
* Nothing consumed it. The local resim resolves its own input from
LocalInputCache by the T4 ref (T6), the local viz reads the live
sampler (T13's hasLiveLocalInput branch), and the motion matcher reads
raw captures (T15). The last reader of the echoed value was re-pointed
before this task landed.
* Divergence is masked by the STATE, not by the input. Corrections ship
every frame; if the authority applied a different input than the client
predicted, the resulting STATE difference lands within ~1 RTT and drives
a resim regardless of whether the input value was echoed.
* The T4 ref still carries the DIAGNOSTIC content that mattered: the
client can always ask "which of my captures did the server actually run
at tick T", and answer it against its own delay line.
THE ONE THING THAT WOULD RE-OPEN THIS: sparse (non-every-frame) state. The
masking argument above is exactly the every-frame-correction argument, and
it is the same premise the deferred stale-hold rule rests on (design §8.7).
A sparse-state increment must re-examine this drop, not assume it.
```

### archived block — PRE-COMPRESSION lines 627–631 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F6)

*the demotion notice: without it the block above reads as a live fence that has in fact already been crossed*

```text
---- ⭐ [T39] THAT INCREMENT HAS LANDED. STATE IS NO LONGER EVERY-FRAME. ---
The paragraph above was written as a fence; this is the task that crossed it,
deliberately and with the trade priced, so the fence is now a RECORD of what
was traded rather than a warning about what must not be
(design_task38_input_first_replication.md §2.3, §13.1).
```

### archived block — PRE-COMPRESSION lines 633–636 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (STEP3)

*what the rotation does and at what rate; mechanism, no prohibition*

```text
WHAT CHANGED. `correctionRotationK` characters' states are written per tick,
round-robin (correctionRotation::isInRound below), so each character's state
replicates at `tickFrequency * K / N` Hz instead of `tickFrequency`. At the
shipped K = 2 that is unchanged at two characters and 40/30/20 Hz at 3/4/6.
```

### archived block — PRE-COMPRESSION lines 638–662 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F2)

*the re-examination, performed; closes with the ONE thing that would re-open it (driving K low enough)*

```text
WHY THE OWN-CHARACTER INPUT-ECHO DROP SURVIVES IT — the re-examination this
block demanded, performed rather than assumed:
* The masking argument WEAKENS but does not invert. A correction snapshot
is a COMPLETE anchor with no delta chain (see §2.1 / the codec), so a
skipped tick costs REPAIR LATENCY, never repair ability: the next
snapshot still detects a divergence introduced during the gap and still
drives the resim. The rotation wait is bounded — every writer is written
within ceil(N/K) sends by construction — so the added latency is at most
ceil(N/K) - 1 ticks, i.e. ~1-2 ticks at the shipped numbers.
* The thing that made the drop safe in the first place is unchanged: the
T4 applied-capture-tick ref still travels with every snapshot, so the
client can still answer "which of my captures did the server run at tick
T" from its own delay line, at whatever cadence the snapshots arrive.
* The trade is strongly favourable and is the POINT of the change: the
payload that lengthens its repair window is the SELF-HEALING one, and it
lengthens so that the IRREPLACEABLE one (the relay ring, which has no
recovery path anywhere) can no longer be displaced by it under packet
pressure. Before T39 both shared one atomic Iris batch and died together
(finding_task37_depth_regression.md).
* It is MEASURED, not asserted: `[DivergenceProbe.Window]` reports
corrections/s per character per class, so the delivered cadence is a
number a run reads back rather than a claim this comment makes.
The one thing that would re-open THIS: driving K low enough that ceil(N/K)
approaches the rollback window. Nothing does today (K >= 1 and N <= 6 give
at most 6), and the clamp's floor of 1 is what bounds it.
```

### archived block — PRE-COMPRESSION lines 665–672 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F3)

*D4 ABSENCE FENCE: correctionRotationK is NOT DEFAULTED ON PURPOSE; a default lets a call site acquire a cadence by accident*

```text
`correctionRotationK` is NOT defaulted, on purpose. The whole deliverable of
T39 is that the state cadence is a DECIDED, configured number rather than an
emergent consequence of Iris dropping things; a default here would let a
future call site acquire a cadence by accident, which is the exact failure
mode the task exists to remove. The production caller
(SimulationManager::onPostSimulationGameThread) passes
TimeConfig::correctionRotationK; tests that are not about cadence pass an
explicit every-frame value.
```

### archived block — PRE-COMPRESSION lines 680–682 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F2)

*read once / advance once; advancing per type re-phases the other type's schedule*

```text
Read ONCE for the whole send so every type map in the tuple is scheduled
against the same round, and advance ONCE at the end. Advancing per type
would make one type's registration count re-phase the other's schedule.
```

### archived block — PRE-COMPRESSION lines 686–687 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F5a)

*TIEBREAK-A. the round base is monotonic and unwrapped PRECISELY SO it can be wrapped per type here*

```text
N for THIS type. The round base is monotonic and unwrapped precisely
so it can be wrapped per type here — see CorrectionRotation.h.
```

### archived block — PRE-COMPRESSION lines 695–697 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F2)

*TIEBREAK-A. the rotation gate's `continue` costs ZERO wire bytes rather than deferring a write (Iris rolls back a clean object)*

```text
[T39] THE ROTATION GATE. A character not written is not dirty,
and Iris rolls back the batch header of a clean object, so this
`continue` costs ZERO wire bytes rather than deferring a write.
```

### archived block — PRE-COMPRESSION lines 699–708 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F2)

*POSITION, NOT ID -- a deliberate simplification; an id-order container buys determinism nothing reads at the cost of a second structure*

```text
POSITION, NOT ID. The cursor walks the enumeration order of this
map rather than a separately maintained registration-order list.
That is a deliberate simplification of the scoped design: the
coverage guarantee is a property of POSITIONS (every position is
covered within ceil(N/K) rounds regardless of which id occupies
it), so a parallel id-order container would buy identity
determinism that nothing reads, at the cost of a second structure
to keep in sync with registration/unregistration. A registration
change re-phases who is in a given frame's window and nothing
more; coverage is unaffected and self-heals within one round.
```

### archived block — PRE-COMPRESSION lines 710–713 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3 + F2)

*D4 ABSENCE FENCE: NO PER-SKIP LOG; 240 formatted lines/s for what the window probe already reports as a rate*

```text
NO PER-SKIP LOG. At 6 characters and K=2 that would be 240
suppressed-but-formatted lines per second for information the
`[DivergenceProbe.Window]` corrections/s figure already reports
as a delivered rate.
```

### archived block — PRE-COMPRESSION lines 721–726 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5a + F2)

*the join key travels WITH the state; read through the accessor so a not-yet-tracked id answers the sentinel instead of throwing on the send path*

```text
[T4] The join key travels WITH the state it belongs to: the
capture tick behind the input this character's authority applied
(T2's tracked value), or kNoInputCaptureTick when the authority
substituted one. Read through the accessor rather than .at() so
an id present in m_authorityWriters but not yet in the track
answers the sentinel instead of throwing on the send path.
```


## §10 — sendLocalInputToAuthorityAll — the redundancy bundle

### archived block — PRE-COMPRESSION lines 749–756 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F2 + F5a)

*ONE unreliable bundled RPC, not one reliable RPC per entry; the retained window is what makes a dropped datagram self-heal*

```text
Unreliable + redundancy local-input RPC. Instead of draining the pending
queue one entry per reliable RPC, the owner builds a single
FInputRedundancyBundle out of the most-recent `redundancyDepth` ticks
still retained in the pending queue and fires ONE unreliable RPC. A dropped
datagram self-heals on the next frame's overlapping bundle. The bundle wire
type stays UE-side (opaque to this layer); we only hand the owner the queue
+ scalar params. After the send we retain only the redundancy window for the
next frame's overlap and release older entries so the queue stays bounded.
```


## §11 — The two named callbacks, and the decide-then-project split

### archived block — PRE-COMPRESSION lines 782–783 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*describes the variadic expansion helper*

```text
Variadic helper: expands over each per-type tuple slot using index_sequence.
Calls fn<SimulatableT>(perTypeMap) for each SimulatableT in the pack.
```

### archived block — PRE-COMPRESSION lines 798–798 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (BANNER)

*section banner for the two named callbacks*

```text
[task 60 / RN-10] registerPredictionOwner's two named callbacks.
```

### archived block — PRE-COMPRESSION lines 800–810 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F2)

*part C CANNOT take part B's lift-and-fold: folding moves what the early `return` returns from. The warning against the obvious tidy-up*

```text
RN-10 measured registerPredictionOwner at 358 lines, 59% comments, with
two ~140-line lambdas burying a ~30-line registration skeleton. Part A
lifted each lambda into a named member below, unchanged in every
probe-call semantic. Part B (arrival, immediately below) then collapsed
the tail probing behind ONE `emit*` call, safe because nothing in that
tail alters control flow. Part C (correction, further below) could NOT
take the same lift-and-fold: its probing has an early `return`
interleaved between two probe calls, and folding "the probing" into one
helper would move what that `return` returns from — see
`decideCorrectionArrival` / `CorrectionArrivalDecision` below, which
apply the RN-8/task-58 decide-then-project shape instead.
```

### archived block — PRE-COMPRESSION lines 813–818 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5c)

*TIEBREAK-A. CALL SITE 1's callback; names the by-id door it routes through and the one projection it makes*

```text
[RN-10 part A+B; re-bound at item 86] CALL SITE 1's callback
(registerPredictionOwner), its probing tail folded behind ONE `emit*`
call. [item 86] No longer takes a `store` reference — the ingest itself
now routes through the resolution peer's by-id `ingestRelayRing` door
(design §C.3); this method's own job is unchanged: project the
returned report through `emitRelayArrival`.
```

### archived block — PRE-COMPRESSION lines 820–824 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (STEP3)

*the measured per-arrival lookup cost; a number, no prohibition, no named alternative*

```text
[item 89 / design §C.3, corrected] Cost of the by-id lookup this callback
makes on every arrival: worst case ~180 lookups/s per character
(redundancy depth 3 × the 60 Hz relay rate) — off the hot per-tick path,
which stays at 60 Hz regardless of how many redundant entries one
replicated bundle carries.
```

### archived block — PRE-COMPRESSION lines 845–848 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (STEP3)

*CALL SITE 2's callback, lifted verbatim; the label survives the strike, the lift narrative does not*

```text
[RN-10 part A] CALL SITE 2's callback (registerPredictionOwner), lifted
out verbatim — a pure lift; the production call and the two helpers it
now delegates to below are unchanged in every semantic, only their
location moved.
```

### archived block — PRE-COMPRESSION lines 854–854 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*heading for the divergence-verdict block*

```text
[T24] THE DIVERGENCE VERDICT, SURFACED AND ATTRIBUTED.
```

### archived block — PRE-COMPRESSION lines 856–861 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1d + F6)

*defends its own placement: THIS site adds the two facts the cache could never supply (id + class); the comparison itself never moved*

```text
The comparison itself is unchanged and happens where it always
has — StateCorrectionCache::tryInsertingCorrectState's
`isSimilarTo`, on every correction, for every character. All that
is new is that the verdict now comes back out, and that THIS site
adds the two facts the cache could never supply: the character
`id`, and its CLASS.
```

### archived block — PRE-COMPRESSION lines 870–870 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*heading for the decision struct*

```text
[task 60 / RN-10 part C] THE CORRECTION-ARRIVAL DECISION.
```

### archived block — PRE-COMPRESSION lines 872–884 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F2)

*noteCorrection MUST NOT run on a discard; folding the probing corrupts item 41's aboveNewest population. `landed` is a FIELD, not a mid-block return*

```text
Task 58/RN-8's shape, reused rather than reinvented: this callback's
probing has an early `return` interleaved between two probe calls
(`noteLanding` always runs; `noteCorrection` must NOT run on a
discard), so folding "the probing" into one helper the way
`emitRelayArrival` does would move what that `return` returns from —
the verdict probe would then run on discarded corrections and silently
corrupt item 41's `aboveNewest` population (the discard bucket
`CorrectionLandingProbe` counts). `landed` is therefore a FIELD of the
decision, not a mid-block `return`, exactly as
`ScheduledRelayedReadDecision::isUnderflowMiss` is a field rather than
a re-derivable fact. `characterClass` / `landingSite` are meaningful
even when `!landed` — the landing probe needs them unconditionally,
see the "hoisted above the gate" note below.
```

### archived block — PRE-COMPRESSION lines 886–891 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3 + F2)

*D4 ABSENCE FENCE: CorrectionArrivalDecision is NOT nested here; both peers need the name without one owning the other*

```text
[relocated task 79] `CorrectionArrivalDecision` itself now lives in
`NetSyncTelemetry.h` as a free struct, not nested here — it needs a
name both this peer (which constructs it, below) and the telemetry
sibling (whose `emitCorrectionArrival` is its one consumer) can see
without one class owning the other. See that struct's own comment in
`NetSyncTelemetry.h` for the full rationale.
```

### archived block — PRE-COMPRESSION lines 893–898 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F5d)

*THE PURE HALF touches no probe and calls no note*/emit*/log*; class and site are computed UNCONDITIONALLY, before `landed` is consulted*

```text
[RN-10 part C] THE PURE HALF — computes the decision, touches no probe
and calls no `note*`/`emit*`/`log*`. `characterClass` and `landingSite`
are computed UNCONDITIONALLY, before `landed` is even consulted,
because the landing probe needs them on every correction including
discards (item 42's "hoisted above the landed gate" — the class/site
facts must exist on the path the verdict probe returns early from).
```


## §12 — Where a correction landed — item 31's retired mechanism and item 45

### archived block — PRE-COMPRESSION lines 916–916 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*heading for the landing-site block*

```text
[og-netcode-v2-input-relay item 42 / I2] WHERE DID IT LAND?
```

### archived block — PRE-COMPRESSION lines 918–926 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **HISTORY** (STEP3)

*item 31's inherited-bit mechanism, entirely past tense and declared GONE 8 lines later*

```text
THE FIRST-GATE DISCRIMINATOR. Item 31 established that resim
triggers were gated by the prediction-frontier slot's INHERITED
`m_isResimulated` bit: `getLastResimulationTick` scanned from the
frontier at offset 0, so that bit shadowed every older corrected
slot, and the ONLY event that re-opened the gate in play was a
correction landing EXACTLY ON the frontier. A correction landing
behind it set its own slot's flag, was never seen by the scan, was
never replayed through (resims restore at the NEWEST corrected
slot) and therefore never touched live state at all.
```

### archived block — PRE-COMPRESSION lines 928–932 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (STEP3)

*what the three-way split measures + the pointer to the full statement on ResimGateProbe.h*

```text
So this three-way split is the measurement the whole item turns
on: `landedBehind` large while triggers track only
`landedAtFrontier` IS the demonstrated under-resimulation
statement. Full statement at CorrectionLandingProbe in
OGSimulation/ResimGateProbe.h.
```

### archived block — PRE-COMPRESSION lines 934–947 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F6)

**** MISLEADING TODAY (Verdict.md 4.3) *** 'READ [ResimGate] session policy BEFORE reading a ratio' fires; the 'after item 46's flip' framing does not*

```text
⚠ [item 45] THAT MECHANISM IS GONE — the gate is now edge-triggered
on an explicit pending anchor, and `m_isResimulated`, the scan and
the inheritance are retired. THIS PROBE IS UNCHANGED AND STILL
CORRECT, for a reason worth stating rather than re-deriving: it
classifies a landing's POSITION relative to the frontier, which is
a fact about the correction stream, not about the gate. What
changed is only what that position IMPLIES: on a default build the
`FrontierExact` policy makes `AtFrontier` exactly the anchor-set
condition (same predicate, same value), so `requested` still tracks
`atFrontier`; after item 46's flip to `OnDisagreement` the trigger
population becomes the DISAGREEING landings — mostly this
`Behind` bucket — and the tracking claim above inverts by design.
Read `[ResimGate] session policy` in the log before reading a
ratio.
```

### archived block — PRE-COMPRESSION lines 949–957 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F2)

*the frontier is read through the EXISTING nullable accessor, NOT by giving the cache an identity -- the placement T24 ruled against*

```text
⚠ THE FRONTIER IS READ THROUGH THE EXISTING RECONCILIATION
ACCESSOR, not by giving the cache an identity. `findInputCache`
is the nullable route every other accessor on that class already
takes, and it answers nullptr on the authority (no caches are
allocated there), which is exactly the right answer: this
callback is OnRep-dispatched and can never fire on an authority
world anyway. Pushing id-awareness down into StateCorrectionCache
to carry the frontier out with the verdict is the placement T24
ruled against, and this needs no such thing.
```


## §13 — Members, threading, and the concept surface

### archived block — PRE-COMPRESSION lines 959–971 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5b + F5d)

*READ AFTER THE INSERT, SAFE AND +/-1 RACY: the file's one admitted race, with both halves of why it is tolerable (Q2)*

```text
⚠ READ AFTER THE INSERT, WHICH IS SAFE AND ±1 RACY, and both
halves of that matter. Safe: `tryInsertingCorrectState` never
touches `m_tickBuffer`, so the frontier it saw and the frontier
read here are the same value on this thread. Racy: the frontier
is ADVANCED by `pushPredictionTick` on the PHYSICS thread, and
the whole cache is already a formally unsynchronized GT/PT
structure (finding §1). A physics frame landing between the
insert and this read misfiles one sample from AtFrontier to
Behind. That is a per-sample ±1 on a 120-sample window, it does
not accumulate, and it is a property of the mechanism being
measured rather than of this instrument: whether a correction
hits the frontier slot at all is decided by that same
interleaving.
```

### archived block — PRE-COMPRESSION lines 1003–1004 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*names the two surviving map members + pointer to the alias comment*

```text
[item 86] Owner-BINDING maps only — see the type aliases' own comment
above for why the container-lifecycle maps moved off this class.
```

### archived block — PRE-COMPRESSION lines 1024–1039 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5c + F5a)

*WHICH methods reach the peer through this MEMBER and which take it as an EXPLICIT PARAMETER, and why each set does*

```text
[item 86 / step 2, PROMOTED item 87 / step 3 of the input-resolution
migration, design §A.3] A real reference to the composition-root-
constructed resolution peer — no longer an owned scaffold sub-object.
NetSync knows both this peer and Reconciliation (design §A.3's
dependency spine); this peer and Reconciliation do not know NetSync
exists. Used by the per-tick/per-event methods whose signatures are
fixed by the manager-facing concepts (`sendCorrectionAll`,
`sendLocalInputToAuthorityAll`) and by the two async-callback helpers
(`onRelayedInputReceived`, `decideCorrectionArrival`) whose bound
lambdas capture only `(this, id)` and so must reach this peer through
a member rather than a call-time parameter. The three
registration/unregistration methods (`registerPredictionOwner`,
`registerAuthorityOwner`, `unregisterSimulatable`) instead take the
resolution peer as an EXPLICIT parameter (design §C.5's facade
signature change) — they run synchronously from the registration
facade, which already has the same reference in hand.
```

### archived block — PRE-COMPRESSION lines 1044–1049 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5b + F5a)

*PLAIN (non-atomic) and why that is safe; -1 disables the future guard until SimulationManager injects the window (Q2)*

```text
Receive-side dedup guard context, pushed by SimulationManager
(setAuthorityGuardContext) every authority tick. Plain (non-atomic) members match
RemoteMoveQueue's existing single-consumer threading assumption — the authority tick
is refreshed once per tick and read at RPC arrival, where an at-most-one-tick-stale
value is fine for a multi-tick rollback window. m_rollbackWindowTicks = -1 disables
the future guard until SimulationManager injects TimeConfig::rollbackWindowTicks.
```

### archived block — PRE-COMPRESSION lines 1053–1057 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5b)

*the rotation cursor is PLAIN because sendCorrectionAll is GAME-thread only (Q2)*

```text
[T39] THE STATE-ROTATION CURSOR. Monotonic, advanced by K once per
sendCorrectionAll, wrapped per type map at the point of use — see the
per-type wrapping note in Network/CorrectionRotation.h. Plain (non-atomic):
sendCorrectionAll runs on the GAME thread only, from
SimulationManager::onPostSimulationGameThread.
```

### archived block — PRE-COMPRESSION lines 1059–1062 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1b + F3)

*D4 ABSENCE FENCE: NOT WIPED BY wipeAllForResync and that is INTENTIONAL -- a round-robin position is not a tick-keyed quantity*

```text
NOT WIPED BY wipeAllForResync, and that is intentional: a hard resync jumps
the CLOCK, and this is a position in a round-robin over characters, not a
tick-keyed quantity. Re-phasing it would skip or double-write a character
for one round and buy nothing.
```

### archived block — PRE-COMPRESSION lines 1070–1078 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3 + F1c)

*TIEBREAK-A / D4: three methods LEFT this concept; SimulatableTs STAYS for signature symmetry though nothing returns one (Q1)*

```text
[item 87 / design §C.7] `collectInputAll` / `collectResimInputAll` /
`wipeAllForResync` LEFT this concept for `SimulationInputResolutionConcept`
(SimulationInputResolution.h, item 83's recorded placement preference) —
the three methods moved off this class onto the resolution peer at the
same item. What remains is exactly NetSync's own surface: the two publish
methods (state send, local-input RPC send) and the receive-side guard.
`SimulatableTs` stays a template parameter for signature symmetry with the
peer concepts even though no member below returns a SimulatableTs-typed
value any more.
```

### archived block — PRE-COMPRESSION lines 1084–1085 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (STEP3)

*why the rotation width is a required argument + pointer to the definition's own note*

```text
[T39] sendCorrectionAll gained the state-rotation width. It is a required
argument rather than a defaulted one — see the note at the definition.
```


## §14 — The registration and unregistration facades — items 92, 93, 94

### archived block — PRE-COMPRESSION lines 1094–1096 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*names the two overloads*

```text
Client overload: prediction owner + optional input provider.
Server overload: prediction owner + authority owner (no input provider needed
— authority reads from the inbound remote-move queue).
```

### archived block — PRE-COMPRESSION lines 1120–1131 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F1a)

*CACHE BEFORE STORAGE. Inverted-order invariant stated, and stated at BOTH overloads so an edit to either sees the sibling it mirrors*

```text
Order matters: cache must exist before storage, because the physics thread
iterates m_storage.forEachSimulatable and looks up each id in the cache map
(postPredictionAll etc.). If storage gets the id first, a concurrent physics
tick sees storage-has-id and calls getCacheFor(id), which throws.
Inverted-order invariant: if storage has id, cache has id.
[item 92] THE SAME SHAPE OF INVARIANT IS ENFORCED BELOW, IN THE SERVER
OVERLOAD — that overload has no cache to create, so it orders
registerAuthorityOwner (the call that lets sweep 2 skip an authority id)
before storage.add instead. State both so a future edit to either overload
cannot break its ordering without seeing the sibling invariant it mirrors.
[item 93] THE SAME INVARIANT, READ BACKWARDS, GOVERNS unregisterSimulatable
below (publish-last on the way in ⇒ unpublish-first on the way out).
```

### archived block — PRE-COMPRESSION lines 1134–1138 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d)

*the peer's containers must exist before registerPredictionOwner binds callbacks against them -- same order, one more call*

```text
[item 87 / design §C.5] `inputResolution` gained by this facade's own
signature — the resolution peer's containers must exist before
`registerPredictionOwner` can bind callbacks against them, same order,
same reason as the cache-before-storage invariant above, now spanning
this extra call.
```

### archived block — PRE-COMPRESSION lines 1143–1146 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F3 + F4)

*D4 ABSENCE FENCE: no correction cache is allocated on the authority, because it does not predict, resim or reconcile*

```text
Server overload — no correction cache is allocated: the authority does not
predict, resim, or reconcile, so it has no need for per-simulatable state
history. NetSync alone handles the outbound correction send and inbound
remote-move queue.
```

### archived block — PRE-COMPRESSION lines 1166–1191 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F2)

*PUBLISH LAST on the server overload; item 94 re-prices WHAT the ordering protects (branch dispatch, not the retired crash)*

```text
[item 92, RE-CHECKED AT ITEM 94] Order matters, mirroring the client
overload's cache-before-storage invariant above: this overload has no
cache to create, but registerAuthorityOwner still SHOULD run before
storage.add. [item 94] ⚠ WHAT THIS ORDERING NOW PROTECTS HAS CHANGED —
priced explicitly rather than left stale. `queueMap` is no longer read
by the frontier-allocation sweep at all: `SimulationReconciliation::
allocateFrontierSlotsAll` filters on ITS OWN cache population
(`findInputCache != nullptr`), not on this class's `queueMap`, so a
storage-exposed id with no cache is now SILENTLY SKIPPED there rather
than falling through to a throwing `.at(id)` — item 92's loud
`OG_CHECK` guard was deleted along with the resolution-side sweep that
carried it (`allocateFrontierSlotForCharacter`, item 94). What THIS
ordering still protects is sweep 1's `queueMap`-based branch dispatch
in `SimulationInputResolution::collectInputForCharacter`: an id exposed
to storage before `queueMap` is populated would misclassify as a
simulated-proxy id instead of an authority id for the width of the
window, reading a relay store that will never exist for it rather than
its remote-move queue — a real, lower-severity defect than the retired
crash, and the reason this reorder is BELT-AND-BRACES now, not
unnecessary. Neither registerPredictionOwner (provider is null here, so
it takes the provider-absent/remote branch) nor registerAuthorityOwner
touches `storage` or the stored `simulatable`, so both are safe to run
first regardless.
Inverted-order invariant: if storage has id, queueMap has id.
[item 93] THE SAME INVARIANT, READ BACKWARDS, GOVERNS unregisterSimulatable
below (publish-last on the way in ⇒ unpublish-first on the way out).
```

### archived block — PRE-COMPRESSION lines 1198–1198 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **NARRATIVE** (LABEL)

*names the facade*

```text
Unregister facade — mirrors registration.
```

### archived block — PRE-COMPRESSION lines 1200–1220 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F1a)

*UNPUBLISH FIRST -- the invariant read backwards. Names the item-92 crash shape this ordering prevents, reachable on player-leave*

```text
[item 93] REGISTRATION PUBLISHES LAST, SO UNREGISTRATION MUST UNPUBLISH
FIRST — the two register overloads above establish "if storage has id,
queueMap/cache has id" by making storage.add the LAST call (queueMap/cache
is already populated the instant the id becomes visible to
storage.forEachSimulatable). Unregistration is the same invariant read
backwards: the id must stop being visible to forEachSimulatable — i.e.
leave storage — BEFORE anything erases the queueMap/cache entries that
gate what a concurrent physics-thread sweep does with it. So
storage.remove<SimulatableT> runs FIRST here, ahead of
netSync.unregisterSimulatable (whose step 3 erases queueMap via
SimulationInputResolution::unregisterCharacter) and reconciliation's
removeCacheFor. This was previously reversed — storage.remove ran AFTER
netSync.unregisterSimulatable — which left the id visible in storage
with an already-erased queueMap entry for the width of that call: exactly
the item-92 crash shape (sweep 2's queueMap guard passes, then
allocateFrontierSlotForCharacter's OG_CHECK aborts / getCacheFor's
bare .at(id) throws), reachable on the authority path on player-leave.
⚠ The comment previously on this facade already CLAIMED "remove from
storage before cache" — but the code below it did the opposite (netSync's
queueMap-erasing call ran first). The claim was aspirational, never
implemented; corrected here rather than just centered on the code.
```

### archived block — PRE-COMPRESSION lines 1222–1232 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d)

*client-path symmetry: the cache is removed AFTER storage.remove, confirmed by reading both callees*

```text
[item 93] CLIENT-PATH SYMMETRY, CONFIRMED NOT ASSUMED: the client register
overload creates the correction cache BEFORE storage.add, so by the same
rule the cache must be removed AFTER storage.remove on the way out.
Placing reconciliation.removeCacheFor LAST (after both storage.remove and
netSync.unregisterSimulatable) satisfies that — read
SimulationReconciliation::removeCacheFor and NetSync::unregisterSimulatable's
own body: neither touches `storage`, so nothing here depends on the cache
or the queueMap/telemetry maps outliving storage.remove. This ordering was
already correct before this fix (removeCacheFor was already the last of
the three calls) and remains correct now; only the storage/netSync
relative order changes.
```

### archived block — PRE-COMPRESSION lines 1234–1248 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F1a + F2)

*THIS REORDER IS LANDED AND MUST NOT BE 'SIMPLIFIED' BACK -- a DIFFERENT hazard depends on it now that the original crash is retired*

```text
[item 94, Part F] ⚠ THIS REORDER IS LANDED AND MUST NOT BE "SIMPLIFIED"
BACK — stated explicitly because task 94 removes the reorder's ORIGINAL
motivation. Frontier allocation is now storage-driven and filtered on
reconciliation's own cache population (`SimulationReconciliation::
allocateFrontierSlotsAll`), so it no longer falls through to the item-92
`OG_CHECK` / bare `.at(id)` throw this reorder was built to avoid — that
specific crash shape is retired by task 94's own existence, independent of
this ordering. That makes this reorder BELT-AND-BRACES, not unnecessary:
it still keeps sweep 1's `queueMap`-based branch dispatch in
`SimulationInputResolution::collectInputForCharacter` correct across the
teardown window (see the server `registerSimulatable` overload's own
item-94 paragraph, above, for the identical argument on the registration
side). A future author must not remove this reorder on the grounds that
"the crash it was fixing can't happen any more" — a DIFFERENT, real hazard
depends on it now.
```

### archived block — PRE-COMPRESSION lines 1250–1263 (coordinates into the 1,281-line original, which no longer exists; NOT live anchors) — **FENCE** (F5d + F4)

*the mirrored invariant re-checked post-94: BOTH sweeps are now tolerant, so the guarantee is stated as a no-window property*

```text
[item 94, Part F] THE MIRRORED INVARIANT, RE-CHECKED POST-94. "If storage
has id, queueMap/cache has id" (registration) / the reverse (teardown)
STILL HOLDS, and the per-tick TOLERANCE for a momentary violation is
STRICTLY WIDER than it was pre-94: previously only sweep 1 (queueMap-keyed
branch dispatch) tolerated the window silently; sweep 2 (frontier
allocation) did not, and that intolerance is exactly what item 92 patched
with a loud guard. Post-94, BOTH sweeps are nullable/storage-filtered — the
allocation sweep now degrades the same way sweep 1 always has (a quiet
skip or a plausible-but-wrong branch choice, never a crash). The guarantee
this ordering states is therefore: *no path from a fully-executed
register/unregister sequence, in the stated order, ever produces a window
visible to a concurrent physics tick* — unchanged in its literal text, but
now backed by two tolerant sweeps instead of one tolerant and one
abort-on-violation.
```


---

## §15 — Blocks NOT archived here, enumerated with a reason

A bare "everything is covered" is not an acceptable report in this initiative. Every classified block that does not appear above is listed here.

| pre-compression block | class | why it is not archived |
|---|---|---|
| `:2` | KEEP-VERBATIM | SPDX / pragma marker — not classifiable comment prose |
| `:17` | NARRATIVE | section banner; carries no rationale to archive |
| `:34` | KEEP-VERBATIM | SPDX / pragma marker — not classifiable comment prose |
| `:219` | NARRATIVE | section banner; carries no rationale to archive |
| `:231` | NARRATIVE | section banner; carries no rationale to archive |
| `:262` | FENCE | section banner; carries no rationale to archive |
| `:342` | NARRATIVE | section banner; carries no rationale to archive |
| `:440` | NARRATIVE | section banner; carries no rationale to archive |
| `:573` | NARRATIVE | section banner; carries no rationale to archive |
| `:744` | NARRATIVE | section banner; carries no rationale to archive |
| `:1092` | NARRATIVE | section banner; carries no rationale to archive |
| `:1281` | KEEP-VERBATIM | SPDX / pragma marker — not classifiable comment prose |

**Archived: 102 of 114 classified blocks. Not archived: 12, all listed above.**

## §16 — The archive labels the header no longer carries (added 2026-08-23)

Wave 6 of `og-source-doc-extraction` applied one rule to this header and to
`SimulationStepSequencing.h`: *a header may contain only what is TRUE and
MEANINGFUL to a reader who has no initiative workspace, no game engine, and no
other file open.* It removed 39 backlog citations and the 3 engine names ruled
VIOLATION-A, and **every sentence those labels sat on stayed at its site** — the
citation was a prefix or an actor's name, not the fact.

Eight of them were more than labels. This section is where they land.

⚠ **This section is an INDEX, not a specification and not a new claim.** Rules 1
and 5 at the top of this file apply to it unchanged: the header is authoritative,
and nothing here is maintained against the tree.

### 16.1 — the ORIENTATION migration table: which decomposition moved which row

The header's orientation block lists WHERE EVERYTHING ELSE WENT. Each row used to
carry an archive item number beside its destination. **The destination is what a
standalone reader needs; the item number resolves only here.** The pairing is
preserved below, so the header can drop one column without losing the join.

| the row, as the header names it | destination, as the header names it | archive item |
|---|---|---|
| the five input container families | `SimulationInputResolution` | item 86 |
| the provider map + the join-key map | `SimulationInputResolution` | item 86 |
| `collectInputAll` | `SimulationInputResolution` | item 87 |
| `collectResimInputAll` | `SimulationInputResolution` | item 87 |
| `wipeAllForResync` | `SimulationInputResolution` | item 87 |
| the `RelayReadProbe` member | `InputResolutionTelemetry` | item 85 |
| the physics-thread emit helpers | `InputResolutionTelemetry` | item 85 |
| frontier-slot allocation | `SimulationReconciliation` | item 94 |

The roster line above that table read *"after items 85, 86, 87 and 94"* and now
reads *"after four decompositions"*. **The count is the checkable half** — it is
the number of distinct entries in this table's third column — and it is the half
a reader with no workspace can use.

Two further re-anchorings in the same class, recorded so the wording can be traced:

| the header used to say | it now says | why |
|---|---|---|
| *"both landed to fix live crashes (items 92, 93) and both re-checked at item 94"* | *"both landed to fix live crashes, and both re-checked when frontier-slot allocation moved out"* | the re-check is tied to the change that forced it, which the table above dates |
| *"WHAT THIS ORDERING PROTECTS CHANGED AT ITEM 94"* / *"RE-CHECKED POST-94"* | *"CHANGED WHEN FRONTIER-SLOT ALLOCATION MOVED OUT"* / *"RE-CHECKED AFTER THAT MOVE"* | same move, named as a mechanism instead of a number |

### 16.2 — ⛔ `sweep 1` was a label the header could not resolve for its own reader

**Measured 2026-08-23, over the whole `og-simulation` submodule.** The phrase
`sweep 1` occurred seven times: **twice in `SimulationNetSync.h` itself**, four
times in the archived blocks of §14 below, and once in `ThreadingCrossings.md`
row 11 — which is the **only** place in the submodule that says what it is (the
input-resolution collect sweep, driven from the prediction physics callback).

⇒ **Neither header use carried a pointer to that definition**, so a reader with
only `SimulationNetSync.h` open met a numbered sweep that named nothing. Both
uses already stated the mechanism in the same sentence, so the fix was to keep
the mechanism and drop the number:

| before | after |
|---|---|
| *"it keeps sweep 1's `queueMap`-based branch dispatch in `SimulationInputResolution::collectInputForCharacter` correct"* | *"it keeps the `queueMap`-based branch dispatch in `SimulationInputResolution::collectInputForCharacter` correct"* |
| *"a DIFFERENT, real hazard depends on it now: sweep 1's branch dispatch in `SimulationInputResolution::collectInputForCharacter`"* | *"… : the `queueMap`-based branch dispatch in `SimulationInputResolution::collectInputForCharacter`"* |

⛔ **The four occurrences in §14's archived blocks are NOT repaired** — rule 3 of
this file forbids editing a quotation, and the label is part of what was there.
This subsection is the scoping a reader of those quotes needs.

### 16.3 — `Iris`: the header's one bare engine name, and where the name now lives

The rotation gate at `sendCorrectionAll` read *"a character not written is not
dirty and Iris rolls back a clean object's batch header, so this continue costs
ZERO wire bytes."* Wave 6's engine-name ruling classified that site a violation:
a bare engine identifier, in a layer whose whole property is that it names none.
The header now says **the replication system**, which is the role word that
ruling prescribes, and the fact — a skipped write costs zero wire bytes, because a clean
object's batch header is rolled back — is unchanged.

**The name itself is not lost.** §0.0 above already carries its row: *`Iris` —
that engine's replication system, the party that batches, drops and rolls back
property writes.* That table is the only place in this tier a reader needs it,
and it is the place §0.0 was written for.

⚠ **This is the one place where wave 6 and wave 5 disagree, and it is recorded
rather than smoothed.** Task 11's sealed subject list names `Iris` as a token
that must survive inside its fence group — the seal was taken before the
standalone-truth rule existed, and it sealed an engine identifier as the reader's
grep handle. Wave 6 is the ruling that such a handle does not belong in an
engine-free header. **The seal is not wrong; it is superseded at exactly one
token, and the token landed here.**
