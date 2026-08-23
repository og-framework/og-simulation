<!-- SPDX-License-Identifier: MPL-2.0 -->

# `SimulationManager.h` — relocation history, retired rationale, archived records

> **This document is CUSTODY, not product.** It is where the prose removed from
> `SimulationManager.h` during the og-source-doc-extraction sweep (task 12, 2026-08-22)
> is archived, so that the compression is reversible and non-lossy. The header carries
> every guard that must fire at its own site; nothing here is load-bearing, and no
> reader should be sent here for anything except provenance.
>
> **Generated, never retyped** (`impl/task12/gen_doc_12.py`). Every quoted block below is
> sliced out of the pre-compression file by line number and emitted unchanged.
>
> ⛔ **It quotes claims that were FALSE when quoted.** Four of them, listed in §9. They
> are preserved verbatim and flagged rather than repaired — an archive that corrects its
> own quotations stops being a record of what the file said.

Line numbers are into `SimulationManager.h` **as it stood before this sweep**
(1,331 lines, nested-submodule working tree at 2026-08-22). They do not resolve against
the shipped file and are not intended to.

<!-- lint-external-ref: prepareSimulationStep -- retired identifier: collectInputAll was renamed to this at item 90 and back at item 94; quoted verbatim in the archived blocks -->
<!-- lint-external-ref: editTimeConfig -- an alternative that deliberately does not exist -- the narrow-setter fence at :357 names the mutable accessor it refuses to provide -->
<!-- lint-external-ref: setResimCooldownTicks -- D4 absence fence at :450: the name is unresolvable BECAUSE the setter was never built -->
<!-- lint-external-ref: resimCooldownTicks -- D4 absence fence at :648: the removed suppression branch's field, quoted verbatim -->
<!-- lint-external-ref: SimulationInputResolution::preparePredictionSimulationStep -- F-T12-2, quoted verbatim and flagged FALSE in section 9 -- the function is a free function in SimulationStepSequencing.h and this qualified form never existed after the relocation -->
<!-- lint-external-ref: Reconciliation::allocateFrontierSlotsAll -- the abbreviated form used in the pre-compression text; the real qualified name is SimulationReconciliation::allocateFrontierSlotsAll -->

## 0. ⚠ Adapter bindings the quoted text names — one adapter's, never the binding

`og-simulation` is engine-free: it names no game-engine type and is reached from a host engine only
through `concept`s. The archived comments below were written inside a project that binds it to one
particular engine, so several of them name that engine's symbols. **Those names are illustrations of
one adapter's binding, not part of the core's contract**, and the quotations are left byte-verbatim
(see the note above) rather than edited, so this table is where the ROLE each one plays is given.
A reader with no engine can read every quoted block through it.

| name in the quotes | the ROLE it plays for the core | note |
|---|---|---|
| `FSimulationManagerAsyncCallback` | one adapter's **physics callback object** — the object the host physics engine calls into, on the physics thread, to drive a step | another adapter supplies its own; the core only requires the calls arrive on one thread |
| `…::TriggerRewindIfNeeded_Internal` | its **resim-request hook**: consulted on non-resim advances, where the engine asks whether to rewind and how deep | |
| `…::FirstPreResimStep_Internal` | its **first-replayed-step hook**: runs once at the start of a granted replay | |
| `…::OnPostSolve_Internal` | its **post-solve hook**: after this sub-step's physics is integrated and solved | |
| `Chaos` | that adapter's **physics engine** — the party that accepts or refuses a rewind request and owns the resim loop | role words: *the physics engine* / *a physics rewind* |
| `ASimulationManagerUImpl` | one adapter's **composition root**: constructs the per-role manager and exposes the two narrow passthroughs the callback reaches it through | |
| `UObject` | that engine's **base object type**; "a game-thread `UObject` callback" means a replication notification delivered on the game thread | |
| `FMath::Min` | that engine's **math utility**, here the engine-side merge that can only DEEPEN a requested rewind | the directional argument is the fact; the utility is not |
| `Config/DefaultEngine.ini` | one adapter's **host-application configuration surface**, where the shipped log level and policy values are set | another adapter supplies the same values under its own keys |

⛔ **None of these is a dependency of this core**, and none appears in `SimulationManager.h` as code.
Where a section below uses one bare, it is this table that scopes it.

---

## 1. Relocation and rename history

### `:26-37` — FENCE / F2+F5a

*compressed to 5 line(s) at the site.* *** FALSE TODAY (F-T12-1) *** the block claims the facade is templated on the CONCRETE peer classes; SimulationStepSequencing.h:151-157 says no concrete class name appears in its signature

    // [item 96] THE ONE NAMED EXCEPTION TO THE PARAGRAPH ABOVE.
    // `onGameSimulationPrediction` below calls the free-function facade
    // `preparePredictionSimulationStep`, which is templated on the CONCRETE
    // `SimulationInputResolution<Ts...>`/`SimulationReconciliation<Ts...>` class
    // templates (deduction needs the real types, not a duck-typed concept) — see
    // that function's own "home" paragraph for why it lives there rather than
    // here or on `SimulationNetSync.h`. `InputResolutionT`/`ReconciliationT`
    // themselves remain duck-typed template parameters, unconstrained by this
    // include; the member function template only instantiates successfully when
    // those parameters happen to BE the concrete classes, which is true in every
    // production instantiation and in no mock-based test today (see the
    // function's own banner for the consequence if that ever changes).

### `:39-42` — NARRATIVE / LABEL

*compressed to 2 line(s) at the site.* names the sequencing facade's own header and points at its banner; no consequence stated here

    // [item 96] `preparePredictionSimulationStep` — the collect/allocate sequencing
    // facade. It lives in its own header, not in either peer's: it names neither
    // peer type (duck-typed on both), so hosting it inside one participant would be
    // arbitrary. See that file's banner for the belongs-here test.

### `:151-160` — FENCE / F5c+F5d

*compressed to 2 line(s) at the site.* what remains on NetSyncT's tick surface, and that it publishes immediately BEFORE the authority collect

    // [item 87 / design §C.7] `collectInputAll` (RENAMED `prepareSimulationStep`
    // at item 90, RENAMED BACK to `collectInputAll` at item 94 once allocation
    // left the method — see that method's own banner) / `collectResimInputAll` /
    // `wipeAllForResync` LEFT this concept — they moved off `SimulationNetSync`
    // onto the resolution peer at item 87, and with them the manager no longer
    // calls any of the three ON `m_netSync`. This concept shrinks to the one
    // member the manager still reaches on NetSyncT from a tick-group method: the
    // receive-side guard, published once per authority tick immediately before
    // `onGameSimulationAuthority`'s own `collectInputAll` call (now on
    // `InputResolutionT` — see `SimulationInputResolutionTickConcept` below).

### `:226-231` — NARRATIVE / STEP3

*compressed to 71 line(s) at the site.* item 87: how InputResolutionT arrived; the surviving present-tense half is a peer roster

    // [item 87 / design §A.3] InputResolutionT is the seventh template parameter,
    // added when the resolution peer was promoted off `SimulationNetSync`'s
    // scaffold to a real, composition-root-constructed peer of its own. The
    // manager reads prediction/authority/resim input straight from it
    // (`collectInputAll` / `collectResimInputAll`) and drives its resync wipe —
    // NetSyncT's own tick-group surface shrank to the receive-side guard alone.

### `:322-324` — HISTORY / STEP3

*deleted, archived here.* item 87 re-target of the wipe call off m_netSync

    // [item 87] Re-targeted off `m_netSync` — the resolution
    // peer now owns this call, matching its own container
    // lifecycle (design §C.4/§C.7).

### `:1225-1227` — NARRATIVE / LABEL

*compressed to 1 line(s) at the site.* names allocateFrontierSlotsAll as reconciliation-distinguishing

    // [item 94] The frontier pair's OPENING half, relocated here from the
    // resolution peer's tick surface — see SimulationReconciliation.h's
    // own concept for why this is reconciliation-distinguishing now.

### `:1239-1245` — NARRATIVE / STEP3

*compressed to 2 line(s) at the site.* item 83 f3: why IntegrationExecShaped was added; the surviving present-tense half is a label

    // [item 83 / 80 f3] Shaped like an IntegrationExec peer. Satisfies every
    // SimulationIntegrationExecutorConcept member. Added to LEVEL the proof
    // coverage: unlike the NetSync/Reconciliation pair above,
    // SimulationIntegrationExecutorConcept previously got no positive shaped
    // control and no transposition negative in this block — only the Empty-type
    // negative below — even though NetSyncShaped/ReconciliationShaped would both
    // (correctly) fail it, a claim nothing here proved before this addition.

### `:1301-1305` — NARRATIVE / STEP3

*compressed to 2 line(s) at the site.* item 83 f3 coverage gap closed; decision report

    // [item 83 / 80 f3, extended item 87] THE COVERAGE GAP THE REVIEW NAMED,
    // CLOSED: none of the three peer shapes above has
    // `firstResimStepAll`/`captureBodyStatesAll`, so all three would fail
    // SimulationIntegrationExecutorConcept — asserted here instead of left
    // as an unproven claim.

## 2. The manager-facing peer concepts: why presence-only, and the placement ruling

### `:97-98` — NARRATIVE / BANNER

*compressed to 2 line(s) at the site.* section heading for the two NetSync splits

    // [og-netcode-v2-input-relay item 80 / B6] SimulationManager-facing splits of
    // SimulationNetSyncConcept (SimulationNetSync.h).

### `:100-114` — FENCE / F2+F6

*compressed to 1 line(s) at the site.* why presence-only: the manager has no SimulatableTs pack, so the peer concept's return checks are inexpressible here

    // SimulationManager's own six template parameters never carry SimulatableTs —
    // that pack lives inside NetSyncT's own instantiation
    // (SimulationNetSync<SimulatableTs...>), never as one of the manager's type
    // parameters — so SimulationNetSyncConcept's two SimulatableTs-typed return
    // checks (collectInputAll / collectResimInputAll ->
    // convertible_to<ResolvedInputs<SimulatableTs...>>) cannot be expressed at
    // the manager's generality: there is no pack for the manager to supply.
    // These two sibling concepts check the SAME method names and argument
    // shapes, presence-only (no return-type pin), split along the two groups
    // SimulationManager's own member functions actually call together: the
    // per-tick read side (onGameSimulationPrediction/Authority/Resimulation) and
    // the once-per-tick publish side (onPostSimulationGameThread).
    // SimulationNetSyncConcept itself is untouched by this task and stays the
    // precise, SimulatableTs-exact check used ad hoc adapter-side where the
    // concrete SimulatableTs are known (task 6, SimulationManagerUImplConceptTest.cpp).

### `:116-127` — NARRATIVE / STEP3

*deleted, archived here.* item 83 f2 residual: a tighter check WAS reachable and was not attempted; reassurance + decision report, no prohibition

    // ⚠ [item 83 / 80 f2] PRESENCE-ONLY IS NOT THE STRONGEST CHECK REACHABLE HERE,
    // AND THAT IS WORTH SAYING PLAINLY RATHER THAN LEAVING IMPLIED. A concept could
    // still constrain the SHAPE of `collectInputAll`/`collectResimInputAll`'s return
    // (e.g. "convertible to some `std::tuple` of `std::unordered_map<unsigned int, X>`s")
    // without knowing `SimulatableTs` — strictly tighter than presence-only, and
    // expressible at this generality. That was not attempted here: the manager's own
    // proof namespace below demonstrates just how weak presence-only is
    // (`InputResolutionShaped::collectInputAll` returns a bare `int` and still satisfies this
    // concept), and a wrong return type at the manager still fails loudly downstream —
    // at the real `ResolvedInputs` consumer (`SimulationIntegrationExecutor::integrateAll`) —
    // rather than silently. Left as a residual for a future revisit of this concept,
    // not attempted in this task.

### `:129-148` — FENCE / F1c+F1d

*compressed to 2 line(s) at the site.* item 83 f4 RULING: these two concepts stay HERE rather than beside their peer; names the move it refuses and why

    // [item 83 / 80 f4] PLACEMENT, RECONSIDERED ON THE MERITS NOW THAT THE
    // SCHEDULING CONTENTION IS OVER. These two concepts were originally defined here
    // (rather than beside `SimulationNetSyncConcept` in `SimulationNetSync.h`, where
    // `SimulationReconciliationConcept` and `SimulationIntegrationExecutorConcept`
    // each live beside THEIR peer class) because item 79 owned `SimulationNetSync.h`
    // concurrently. That is a scheduling reason, not a design one, and the
    // "definition-at-call-site is better anyway" argument that accompanied it does
    // not hold as a general principle — this file's other two peer concepts are both
    // defined at their PEER's declaration site and pulled in by `#include`, not
    // defined at their one call site (here). Item 79 has since landed, so the
    // contention that forced this placement no longer exists.
    // RULING: left here rather than moved, because this task (item 83) owns
    // `SimulationManager.h`, `docs/ThreadingCrossings.md`, `CorrectionCache.h` and
    // `NetSyncTelemetry.h` — not `SimulationNetSync.h` — and moving these two
    // concepts is a `SimulationNetSync.h` edit with its own blast radius (a new
    // include cycle to check, since `SimulationManager.h` deliberately does not
    // include `SimulationNetSync.h` today). Recorded here as a decision, not an
    // oversight: a follow-up task that already owns `SimulationNetSync.h` should
    // relocate `SimulationNetSyncTickConcept` / `SimulationNetSyncPublishConcept`
    // beside `SimulationNetSyncConcept` for consistency with the other two peers.

### `:177-191` — FENCE / F5c

*compressed to 1 line(s) at the site.* enumerates the resolution tick group and why a resync-reachable member belongs to it

    // [item 87 / design §C.7] SimulationManager-facing split of
    // SimulationInputResolutionConcept (SimulationInputResolution.h) — same
    // SimulatableTs-invisibility reasoning as `SimulationNetSyncTickConcept`
    // above (item 80/83's residual, inherited here): the manager's own template
    // parameters never carry `SimulatableTs`, so the peer concept's two
    // SimulatableTs-typed return checks cannot be expressed at the manager's
    // generality. Presence-only, checking the group all three prediction/
    // authority/resim step methods call: `collectInputAll` (prediction,
    // authority; named `prepareSimulationStep` between item 90 and item 94,
    // which reverted the rename once allocation left the method),
    // `collectResimInputAll` (resim) and `wipeAllForResync` (the
    // resync-callback path, reachable from the same physics-thread call chain
    // `onGameSimulationPrediction` drives — see that concept's own history on
    // `SimulationNetSyncTickConcept` above for why a resync-reachable member
    // belongs on a tick-group concept rather than a publish one).

### `:193-204` — FENCE / F6+F1b

*compressed to 4 line(s) at the site.* wipeAllForResync is shared by THREE peers -- a wipe-only clause distinguishes NOTHING; the ctor's unconstrained-ness rests on this

    // ⚠ `wipeAllForResync` IS NOW SHARED VERBATIM BY THREE PEER CONCEPTS —
    // `SimulationReconciliationConcept`, this one, and (until item 87) formerly
    // `SimulationNetSyncConcept` too, before it left NetSync entirely — so a
    // wipe-only clause distinguishes NOTHING between a resolution-shaped peer and
    // a reconciliation-shaped one. The DISTINGUISHING members are `collect*`
    // (resolution, checked below), `postPredictionAll` /
    // `consumeResimAnchorsAll` / [item 94] `allocateFrontierSlotsAll`
    // (reconciliation) and `sendCorrectionAll` (netsync). The manager's ctor
    // stays deliberately unconstrained by any peer concept for exactly this
    // reason (see the ctor's own comment) — a proof that leaned on
    // `wipeAllForResync` alone would look like transposition protection while
    // providing none, the same trap item 80's unconstrained ctor documented.

### `:295-304` — FENCE / F1b+F6

*compressed to 3 line(s) at the site.* the ctor is DELIBERATELY unconstrained; a wipe-only requires-clause would fake transposition protection

    // [item 80 / B6, updated item 87] The ctor itself stays UNCONSTRAINED
    // by any peer concept, deliberately: `wipeAllForResync(uint32)` is now
    // shared verbatim by THREE peer concepts —
    // SimulationInputResolutionTickConcept, SimulationReconciliationConcept,
    // and (until item 87) formerly SimulationNetSyncConcept too — so a
    // requires-clause built only from it could not distinguish a
    // resolution-shaped peer from a reconciliation-shaped one — it would
    // give the false impression of transposition protection while
    // providing none. The methods that actually differ between the peers
    // are constrained below, at the member functions that call them.

### `:512-519` — FENCE / F5c

*compressed to 3 line(s) at the site.* what onPostGameSimulation does plus the item-80 concept-member enumeration its requires-clause rests on

    // Called from FSimulationManagerAsyncCallback::OnPostSolve_Internal.
    // Runs after Chaos has integrated and solved physics for this sub-step.
    // Captures post-solve state into the cache and detects the resim-batch
    // catch-up edge to apply resim results.
    // [item 80 / B6] captureBodyStatesAll (SimulationIntegrationExecutorConcept)
    // plus postPredictionAll / postResimulationAll / applyResimAll /
    // consumeResimAnchorsAll (SimulationReconciliationConcept) — every one of
    // this method's peer calls below.

### `:623-623` — FENCE / F5c

*compressed to 1 line(s) at the site.* item 80 concept-member enumeration for this method's requires-clause

    // [item 80 / B6] checkDivergenceAll (SimulationReconciliationConcept).

### `:690-692` — FENCE / F5c

*compressed to 2 line(s) at the site.* parameter meanings plus the item-80 concept enumeration

    // chaosStep is the raw Chaos physics step; simTick is the simulation tick to resim from.
    // [item 80 / B6] prepareResimAll (SimulationReconciliationConcept) +
    // firstResimStepAll (SimulationIntegrationExecutorConcept).

### `:788-790` — FENCE / F5c

*compressed to 2 line(s) at the site.* item 80 publish-surface enumeration

    // [item 80 / B6] sendCorrectionAll + sendLocalInputToAuthorityAll — the
    // NetSync "publish" surface (SimulationNetSyncPublishConcept), the other
    // half of the NetSync method inventory the tick methods above don't call.

### `:916-923` — FENCE / F5c

*compressed to 3 line(s) at the site.* why prediction is the ONE step function constrained on both peer concepts

    // [item 80 / B6, re-targeted item 87] Constrained on the resolution
    // peer's tick surface — the group this method, onGameSimulationAuthority
    // and onGameSimulationResimulation all draw from (collectInputAll here,
    // named prepareSimulationStep between item 90 and item 94; NetSyncT no
    // longer has one). [item 94] AND, NEW HERE, on the reconciliation peer's
    // concept too — this is the ONE step function that needs both, because
    // it is now the sole caller of `reconciliation.allocateFrontierSlotsAll`
    // (see the sweep-boundary fence below).

### `:1015-1023` — FENCE / F5c

*compressed to 3 line(s) at the site.* why authority needs both NetSync-tick and resolution-tick, and NOT the reconciliation concept

    // [item 80 / B6, re-targeted item 87] setAuthorityGuardContext is now the
    // sole member of NetSyncT's shrunk tick surface
    // (SimulationNetSyncTickConcept); collectInputAll (named
    // prepareSimulationStep between item 90 and item 94) moved to the
    // resolution peer's tick surface (SimulationInputResolutionTickConcept)
    // — this method is the one step function that needs BOTH. [item 94] It
    // does NOT need `SimulationReconciliationConcept` — unlike
    // `onGameSimulationPrediction`, this method never calls
    // `reconciliation.allocateFrontierSlotsAll` (see below).

### `:1082-1085` — FENCE / F5c

*compressed to 1 line(s) at the site.* resim's single concept member; NetSyncT contributes nothing to this step

    // [item 80 / B6, re-targeted item 87] collectResimInputAll — the third
    // member of the resolution peer's tick surface
    // (SimulationInputResolutionTickConcept); NetSyncT contributes nothing to
    // this step.

## 3. The compiler-control pragma and the measurement rule

### `:50-52` — FENCE / F1d+F5c

*compressed to 2 line(s) at the site.* CANONICAL pragma statement: every other OGSim-core pragma site points HERE

    // pragma optimize off — debugger-friendliness across all build configs (breakpoints hit,
    // locals visible, call-stack intact). OGSim-core convention; canonical statement — the
    // other OGSim-core pragma sites point here.

### `:54-65` — FENCE / F5a

*compressed to 2 line(s) at the site.* item 77 macro contract: what OGSIM_OPTIMIZE_OFF/ON expand to in each configuration

    // [og-netcode-v2-input-relay item 77] As of this item the pair below is
    // OGSIM_OPTIMIZE_OFF/ON, not a raw pragma — every OGSim-core, OGBrawler-core,
    // and UE-adapter site (43 files) uses the same two macros, defined once in
    // OGSimulation/CompilerControl.h. That header is the mechanism; this comment
    // is the contract:
    //   - Default (Debug/Development, no switch defined): the macros expand to
    //     exactly this pragma pair — daily debugging is untouched by item 77.
    //   - Define OGSIM_FORCE_OPTIMIZED=1 (or build Test/Shipping) and both macros
    //     expand to nothing everywhere — the whole core compiles at full
    //     command-line optimization.
    //   - Non-MSVC compilers get a no-op pair unconditionally — CompilerControl.h
    //     is where the MSVC-only pragma is made portable, not just renamed.

### `:67-74` — FENCE / F1a

*compressed to 2 line(s) at the site.* THE MEASUREMENT RULE: no gate-family cost number is quotable unless its record names the optimize setting

    // THE MEASUREMENT RULE: no gate-family cost number (items 43, 46, 78, ...) is
    // quotable unless the log or record that produced it NAMES the optimize
    // setting it was taken under. A number measured under the default (this
    // pragma active, as every archived resim number to date was) and one measured
    // with OGSIM_FORCE_OPTIMIZED=1 are not the same measurement — treating them as
    // interchangeable without saying so is this initiative's signature defect (an
    // instrument measuring the wrong quantity under the right name), applied to
    // build configuration. Full rationale in OGSimulation/CompilerControl.h.

## 4. The composition-root knobs, and the TimeConfig re-read rule

### `:313-320` — FENCE / F3+F1d

*compressed to 3 line(s) at the site.* D4 ABSENCE FENCE: the slot-provenance log's second call site is NOT here, and why

    // [item 48] THE SLOT-PROVENANCE LOG's SECOND CALL SITE IS NOT
    // HERE — it is inside `wipeAllForResync`, one line down, and
    // that placement is deliberate rather than incidental. Putting
    // it here would have added a method to the four mock
    // `Reconciliation` types the LLTs instantiate this ctor with,
    // for a line that belongs to the wipe sweep itself and reads
    // the same caches in the same pass. See
    // `SimulationReconciliation::logSlotProvenanceFor`.

### `:344-351` — FENCE / F2+F5a

*compressed to 2 line(s) at the site.* one config, one tier policy: the alternative (adapter keeps a second TimeConfig) and the reference-stability contract

    // [C.2 / T10] Exists so an engine-side composition root can bind
    // per-connection structures (ConnectionTierTable, ServerInputDelayQueue)
    // to the SAME config instance the clocks and the authority guard already
    // read. Those structures hold `const TimeConfig&`, so handing out a
    // reference to a manager-owned member — rather than letting the adapter
    // keep a second TimeConfig — is what makes "one config, one tier policy"
    // true by construction instead of by convention. The reference is stable
    // for the manager's lifetime (m_timeConfig is a by-value member).

### `:354-355` — FENCE / F5c

*compressed to 1 line(s) at the site.* THE one writable location for the relay delay floor

    // [T11 / og-netcode-v2-input-relay] THE one writable location for the session
    // relay delay floor. (RelayDelaySpectrumDesign.md §6, §11 Q1/Q6.)

### `:357-363` — FENCE / F1b+F2

*compressed to 1 line(s) at the site.* narrow setter rather than editTimeConfig(); the alternative and what it would make re-settable

    // Deliberately a single narrow setter rather than a mutable `editTimeConfig()`:
    // the rest of TimeConfig is a start-up constant that the clocks, the tier
    // table and the delay queue all hold by reference, and handing out a mutable
    // handle would make every one of those fields silently re-settable mid-session.
    // The floor is the one field that legitimately changes after construction —
    // it is server-owned and REPLICATED, so a client learns it from an OnRep, and
    // the deferred dynamic-floor policy (§11 Q6) publishes through this same door.

### `:365-367` — FENCE / F5a

*compressed to 1 line(s) at the site.* clamped at the write site too; idempotent

    // CLAMPED HERE TOO. `clampRelayDelayFloorTicks` also runs at both intake
    // points; repeating it at the single write site means no future caller can
    // store an out-of-range floor by forgetting, and the clamp is idempotent.

### `:369-374` — FENCE / F5b

*compressed to 2 line(s) at the site.* game thread only; the one atomic the value crosses on

    // THREADING. Game thread only, and the value it feeds crosses to the physics
    // thread exactly where it always did — through the one
    // `setClientEffectiveInputDelayTicks` atomic. On the server the floor is
    // written once at composition, before any connection exists; on a client it
    // is written from the floor OnRep, a game-thread UObject callback, and read
    // back on the same thread by the recompute.

### `:381-388` — FENCE / F3+F1a

*compressed to 3 line(s) at the site.* D4 ABSENCE FENCE: the retired relay-ring-depth setter; do not re-add

    // ⛔ RETIRED (og-netcode-v2-input-relay item 63 / RN-13, 2026-08-16): there is
    // deliberately no relay-ring-depth setter here any more (its old identifier
    // is on record in RN-13, ReviewNotes.md). It wrote a session-configurable
    // retention depth into the outbound relay ring's replace-latest write path;
    // item 34 replaced that write path with bare-C1 flush-on-poll, whose stage
    // capacity is `relayedInputRing::kMaxDepth` — a compile-time constant with no
    // setter to receive a configured value — and item 63 removed the now-inert
    // setter along with the field and its ini intake chain.

### `:390-392` — FENCE / F5c

*compressed to 2 line(s) at the site.* THE one writable location for correctionRotationK

    // [T39 / og-netcode-v2-input-relay] THE one writable location for the session
    // correction-state rotation width — the door the composition root's
    // `[OGNetcode] CorrectionRotationK` ini override writes through.

### `:394-399` — FENCE / F1b+F5a

*compressed to 1 line(s) at the site.* server-only and NOT replicated; no OnRep counterpart exists by design

    // SERVER-ONLY and NOT replicated, for the same reason as the delay floor's
    // ONE-SHOT siblings: only the authority runs
    // `SimulationNetSync::sendCorrectionAll`, so a client's copy of this value
    // would have no reader, and a receiver reconciles against whatever
    // corrections arrive without needing to know the sender's cadence. So this
    // setter has no OnRep counterpart and no client-side intake point.

### `:401-406` — FENCE / F1a

*compressed to 1 line(s) at the site.* one-shot at composition; it must not acquire a cvar

    // ONE-SHOT AT COMPOSITION. Not a hard requirement the way the depth's is (K
    // holds no allocated state, so changing it mid-session would merely re-phase
    // the schedule), but it is deliberately kept to the same discipline: the
    // cadence is a DESIGNED number that a session's probe output is read against,
    // and a value that can move mid-run makes those readings unattributable.
    // It must not acquire a cvar.

### `:408-411` — FENCE / F5a

*compressed to 2 line(s) at the site.* clamped; 0 clamps UP to 1 because K=0 is a permanent desync, not 'off'

    // CLAMPED HERE TOO, with the same shared idempotent guard the intake and the
    // selection predicate call (`correctionRotation::clampK`). 0 and negatives
    // clamp UP to 1: a K of 0 is a correction channel that never publishes, which
    // is a permanent desync, not "off".

### `:417-419` — FENCE / F5c

*compressed to 2 line(s) at the site.* THE one writable location for the resim trigger policy

    // [item 45 / og-netcode-v2-input-relay] THE one writable location for the
    // session RESIM-GATE TRIGGER POLICY — the door the composition root's
    // `[OGNetcode] ResimTriggerPolicy` ini override writes through.

### `:421-427` — NARRATIVE / STEP3

*deleted, archived here.* role-agnosticism explained as the simple answer; the role fact itself is owned by Perspective-AuthorityVsPrediction.md

    // ROLE-AGNOSTIC, and unlike the two knobs above that is the simplest correct
    // answer rather than a considered exception: the gate only exists on a
    // predicting client (an authority allocates no correction caches and never
    // rewinds), so applying the value on a server is inert. Writing it on both keeps
    // one TimeConfig shape for both roles and keeps the intake in the composition
    // root's shared section, where a reader looking for "what is this session's
    // policy" finds one answer.

### `:429-435` — FENCE / F1a+F2

*compressed to 3 line(s) at the site.* one-shot IS load-bearing here: a cvar would race the policy word against in-flight landings. There must not be one

    // ONE-SHOT AT COMPOSITION, and here that IS load-bearing. The trigger policy is
    // pushed down into every `StateCorrectionCache` and consulted on the GAME thread
    // at the correction-landing site, with no synchronization — safe precisely
    // because it is written once, before any correction can land. A cvar or console
    // command would make it writable while landings are in flight, which is a data
    // race on the policy word AND a run whose trigger regime changed mid-measurement.
    // There must not be one.

### `:437-442` — FENCE / F5c+F2

*compressed to 3 line(s) at the site.* the setter has a SECOND effect (fan-out to every cache); a direct TimeConfig write leaves the caches on the compiled default

    // ⚠ THE POLICY SETTER HAS A SECOND EFFECT and must therefore stay the only door:
    // it fans the value out through `SimulationReconciliation::setResimTriggerPolicy`
    // to every allocated cache. A caller that wrote `TimeConfig` directly would leave
    // the caches on the compiled default and the proof line reporting a policy
    // nothing implements.
    // [item 80 / B6] setResimTriggerPolicy (SimulationReconciliationConcept).

### `:592-597` — FENCE / F5c

*compressed to 3 line(s) at the site.* *** FALSE TODAY *** the return is wired to noteSurvivingAnchors, but 'expired the day item 46 shipped' is false and the cited comment no longer says it

    // [item 57 / RN-6] THE RETURN VALUE IS NO LONGER DISCARDED. It is
    // fed to `m_resimGateProbe.noteSurvivingAnchors`, surfaced as a
    // field on the `[ResimProbe.Gate]` line — see
    // `SimulationReconciliation::consumeResimAnchorsAll`'s comment for
    // why that promotion's old blocker ("reads 0 until item 46")
    // expired the day item 46 shipped.

### `:627-638` — FENCE / F2+F1a

*compressed to 4 line(s) at the site.* *** FALSE TODAY *** the no-cache rule (Q3) is sound; 'on today's default this fold is byte-for-byte pre-item-45' is false under the shipped ini

    // Caller (FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal)
    // short-circuits on !runsPrediction() so this is only reached on the
    // predicting client — the authority is the truth, never rewinds. That
    // short-circuit is ALSO why item 42's "zero new lines on the authority"
    // criterion holds without a role test here: the window is driven by this
    // method, so on a server it never advances and never flushes.
    // [item 45] THE DEPTH POLICY, read LIVE from TimeConfig and handed down —
    // the `sendCorrectionAll(step, correctionRotationK)` shape, for the same
    // reason: caching it in reconciliation would make an ini-driven setting
    // silently ineffective. 0 means "no depth policy", which is what the legacy
    // trigger regime gets (`policyEnforcesDepthCeiling`), so on today's default
    // this fold is byte-for-byte the pre-item-45 one.

### `:670-676` — FENCE / F2

*compressed to 4 line(s) at the site.* *** FALSE TODAY *** deepSkips rides the existing call (sound); 'structural 0 under the shipped legacy policy' is false under the shipped ini

    // [item 45] `diagnosticDeepAnchorSkips` rides the same call rather than a new
    // log line: T19 volume discipline, and item 45 forbids new `[Resim.` /
    // `[ResimCheck.` lines outright. It appears as one extra field on the
    // existing `[ResimProbe.Gate]` line, and it reads a structural 0 under the
    // shipped legacy policy (which enforces no depth policy for the anchor to
    // fail). The count is diagnostic; `checkDivergenceAll`'s underlying skip is
    // production (see its declaration, `docs/DiagnosticsConventions.md` §4).

### `:795-798` — FENCE / F2

*compressed to 3 line(s) at the site.* the rotation width is read LIVE; caching it makes an ini-driven setting silently ineffective

    // [T39] The state-rotation width — how many characters' correction buffers
    // are written this tick. Read live from TimeConfig for the same reason the
    // redundancy depth below is: it is the configured session value, and
    // caching it here would make an ini-driven setting silently ineffective.

### `:800-801` — NARRATIVE / LABEL

*deleted, archived here.* states redundancyDepthTicks' value and NOT the rule -- Q3's measured scope gap

    // redundancy depth tracks the runtime tick rate via
    // TimeConfig::redundancyDepthTicks (5 @ 100 Hz interim / 3 @ 60 Hz target).

### `:908-912` — FENCE / F4

*compressed to 3 line(s) at the site.* *** FALSE TODAY *** staleProtects MUST read 0 in a single-character session (sound); 'near-0 under the shipped FrontierExact' is false

    // [item 47] The hollow-anchor ledger, on the EXISTING line rather than
    // a new one. `freshProtects` is the live defect rate (near-0 under the
    // shipped `FrontierExact`, a rate after item 46's flip);
    // `staleProtects` MUST read 0 in any single-character session — see
    // ResimGateWindowSummary for both readings.

### `:1031-1033` — FENCE / F1a

*compressed to 2 line(s) at the site.* the window comes from TimeConfig::rollbackWindowTicks -- no hardcoded literal

    // publish the current authority tick + rollback window so the
    // RPC-arrival queueMove path can reject too-far-future capture ticks. The window
    // is TimeConfig::rollbackWindowTicks (no hardcoded literal).

## 5. The resim-gate probe: ownership, the adapter feeders, and the accessor with no callers

### `:44-46` — FENCE / F5b+F6

*compressed to 2 line(s) at the site.* the probe pair is split by THREAD; the include name does not say which half. *** OWNERSHIP IMPRECISE (F-T12-5) *** CorrectionLandingProbe is a member of NetSyncTelemetry, not of SimulationNetSync

    // [og-netcode-v2-input-relay item 42] The resim-gate telemetry. Physics-thread
    // half only — the game-thread half (CorrectionLandingProbe) is owned by
    // SimulationNetSync, at the site that knows the character id and its class.

### `:450-455` — FENCE / F3+F1a

*compressed to 3 line(s) at the site.* D4 ABSENCE FENCE: there is no setResimCooldownTicks and that is a ruling

    // ⛔ THERE IS NO `setResimCooldownTicks`, AND THAT IS A RULING. A trigger-rate
    // ceiling defers acting on a correction already known to disagree, which is the
    // defect item 45 repairs; the throttle is structural instead (one resim in
    // flight, one pending, mid-replay landings coalesce). See the ruling block on
    // `TimeConfig::resimTriggerPolicy` and the argument at
    // `resimGate::policyEnforcesDepthCeiling`.

### `:577-584` — FENCE / F5a+F5d

*compressed to 3 line(s) at the site.* the consume edge is a CAS; a mid-replay correction makes it fail and the anchor SURVIVES by design

    // [item 45] W2 — THE CONSUME EDGE. The resim that just completed
    // consumes the anchor it was PREPARED with, per character, as a
    // CAS: a correction that landed on the game thread mid-replay has
    // raised that character's anchor past the prepared value, so its
    // CAS fails and the anchor SURVIVES to re-trigger next frame.
    // That is the designed behaviour and the reason termination is
    // structural rather than timing-dependent — see
    // `StateCorrectionCache::consumeResimAnchor`.

### `:586-590` — FENCE / F5d

*compressed to 1 line(s) at the site.* ORDER: consume AFTER applyResimAll, and on the completion edge, not at prepare

    // ⚠ ORDER: after `applyResimAll`, so the gate is closed only once
    // the replayed state has actually been published into live state.
    // And on THIS edge rather than at prepare, because ~20 % of
    // prepares never reach here (item 42's stranded class) and an
    // anchor consumed at prepare would take its correction with it.

### `:648-659` — FENCE / F3+F1a

*compressed to 3 line(s) at the site.* D4 ABSENCE FENCE: no rate ceiling, and the structural throttle that replaces it

    // ⛔ [item 45] THERE IS NO RATE CEILING HERE, AND THAT IS A RULING RATHER THAN
    // AN OMISSION — a `resimCooldownTicks` suppression branch stood on this line
    // and was removed. A ceiling defers acting on a correction already KNOWN to
    // disagree with prediction, which is the defect this gate was rebuilt to
    // repair. What throttles the rate instead is STRUCTURAL: Chaos consults this
    // method only on non-resim advances (never inside its own rewind loop) and the
    // anchor is consumed only on the completion edge, so AT MOST ONE RESIM IS IN
    // FLIGHT AND AT MOST ONE MORE IS PENDING, and corrections landing mid-replay
    // coalesce into the pending anchor via its CAS-max and fire once as a single
    // deeper replay. A correction ahead of a running resim RE-ANCHORS; it never
    // restarts the replay and never waits. Full argument at
    // `resimGate::policyEnforcesDepthCeiling`.

### `:715-720` — FENCE / F5b+F5c

*compressed to 3 line(s) at the site.* two of six instruments are fed from the ADAPTER, on the SAME physics thread -- that is why one object needs no atomics

    // Public because two of its six instruments are fed from the ADAPTER side:
    // `FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal` (I3's
    // `requests`, I4's requested depth) and `::FirstPreResimStep_Internal` (I3's
    // `grants`, I4's `clampedGrants`). Those two hooks straddle the core/adapter
    // boundary but run on the SAME physics thread as every other feeder here, which
    // is the whole reason one object can serve them all with no atomics.

### `:722-727` — FENCE / F1a

*compressed to 3 line(s) at the site.* NOT a general mutable handle; game-thread use corrupts a window

    // ⛔ THIS IS NOT A GENERAL MUTABLE HANDLE. It is a counter sink; nothing it
    // holds is read by any gate, clock, cache or integrator, and calling any of its
    // methods from the game thread would corrupt a window (see the two-object rule
    // in ResimGateProbe.h). The adapter reaches it through two NARROW named
    // passthroughs on ASimulationManagerUImpl, not through this accessor, for the
    // same reason `requestInputDelayIncreaseStall` is narrow.

### `:729-734` — FENCE / F1d

*compressed to 2 line(s) at the site.* editResimGateProbe STAYS here: a production write handle is not a diagnostic read seam

    // [T53 / task 59] `editResimGateProbe()` STAYS exactly here — it is a
    // production write handle (two adapter-side feeders, above), not a
    // diagnostic read seam, and `getDiagnostics()` / `editDiagnostics()` group
    // read seams only (see `docs/DiagnosticsConventions.md` §2). Only the
    // read-only probe accessor that used to sit beside it moves — see the
    // `Diagnostics` view directly below.

### `:743-747` — FENCE / F1d+F5c

*compressed to 3 line(s) at the site.* ONLY the read-only accessor moved; the probe, every note* feeder and editResimGateProbe are production and untouched

    // ⛔ ONLY THIS READ-ONLY ACCESSOR MOVES. `m_resimGateProbe` itself, every
    // `note*` call site that feeds it (this class's own `noteDivergenceCheck`
    // included — private, one production caller every frame, feeds this probe
    // directly) and `editResimGateProbe()` above are production and are
    // UNTOUCHED. See `docs/DiagnosticsConventions.md` §2.

### `:749-760` — FENCE / F4+F5c

*compressed to 5 line(s) at the site.* the accessor has ZERO production callers and is kept; the LLT case name is the reader's only grep handle to that ruling

    // ⚠ [task 59 RULING] Unlike the other four probe accessors this task
    // groups, this class's pre-existing read-only accessor had ZERO callers
    // anywhere — no production, no test — before this task. That made the
    // resim gate probe the one
    // instrument in the family whose SHIPPED wiring was unproven, on the
    // family that carried every measurement the `OnDisagreement` ship decision
    // rested on. RULING: wire a test rather than delete it — see
    // `ResimGate.Policy.TheResimGateProbeAccessorObservesTheShippedFeed` in
    // `ResimGatePolicyTest.cpp`, which drives the real `onCheckIsSimilar()`
    // through a duck-typed manager rig and asserts this view's probe observed
    // it. Deleting the accessor because "production never calls it" would have
    // removed the only mechanism that could ever prove this family is wired.

### `:762-764` — FENCE / F2

*compressed to 1 line(s) at the site.* nested class, not a free function -- no friend declaration needed

    // Nested class, not a free function: it has the same access to
    // SimulationManager's private members as any other member function — no
    // friend declaration needed, and the view holds nothing but a reference.

### `:928-936` — FENCE / F5d

*compressed to 4 line(s) at the site.* the stranded observation is taken BEFORE advancePrediction so it reports what this frame INHERITED

    // [item 42 / I5] THE STRANDED-CURSOR OBSERVATION, taken BEFORE
    // advancePrediction so it reports the state this frame INHERITED rather
    // than one this frame created. A normal prediction frame entered while the
    // clock still believes it is resimulating means the previous resim's apply
    // edge never ran — finding §4b, ~20 % of granted resims in every archived
    // run. Its side effect is real and measurable: `currentStep()` keeps
    // returning the stale resim step on the game thread, so
    // `sendLocalInputToAuthorityAll` stamps a stale tick until the next
    // `startResimulation` resets the cursor.

### `:1165-1172` — FENCE / F1b+F2

*compressed to 3 line(s) at the site.* the probe is constructed unconditionally, on the authority too; guarding it on the role adds a second place to get the role wrong

    // [og-netcode-v2-input-relay item 42] THE RESIM-GATE PROBE. PHYSICS THREAD
    // ONLY, and unconditional: it is constructed on the authority too, where
    // `onCheckIsSimilar` is never reached (the adapter short-circuits on
    // !runsPrediction) so it never advances and never flushes. That is why the
    // "zero new lines on the authority" criterion needs no role test anywhere —
    // the counters simply have no feeder there. Sixteen `uint32`s and four flags;
    // guarding its construction on the role would buy nothing and would add a
    // second place for the role to be got wrong.

## 6. The per-window flush and the counter identities

### `:533-535` — NARRATIVE / STEP3

*deleted, archived here.* item 42 I6 wiring note; the surviving half is a field-name mapping the probe already carries

    // [item 42 / I6] The sweep's discard count feeds `replayOverruns`.
    // The call, the sweep and the cache's own per-discard Warning line
    // are all exactly as they were.

### `:537-544` — FENCE / F2+F1a

*compressed to 3 line(s) at the site.* all three counts ride ONE sweep -- separating them would let them describe different replay ticks; no new log line

    // [item 47] AND the same sweep now reports how many corrected
    // slots it was refused permission to overwrite, split fresh/stale.
    // All three counts ride this ONE call rather than a second sweep —
    // they are derived from the same per-character pass, and
    // separating them would let them describe different replay ticks.
    // No new log line: they appear as fields on the existing
    // `[ResimProbe.Apply]` line (T19 volume discipline; item 47
    // forbids new `[Resim.` / `[ResimCheck.` lines outright).

### `:546-551` — FENCE / F2

*compressed to 1 line(s) at the site.* one struct by value, not a return plus two out-pointers; auto keeps the site duck-typed

    // [item 55] The sweep returns one `ResimSweepDiagnostics` by value
    // (RN-3, option B) instead of a return value plus two out-pointers
    // — `auto` here keeps this call site duck-typed on the
    // reconciliation peer, exactly as
    // `m_inputResolution.collectInputAll`'s return is
    // captured a few lines up.

### `:567-573` — FENCE / F5c

*compressed to 1 line(s) at the site.* finishes == the [Resim.Finish] occurrence count BY CONSTRUCTION; a disagreement means the instrument moved

    // [item 42 / I5] THE APPLY EDGE, counted at the edge itself —
    // the same condition the existing `[Resim.Finish]` line reports,
    // so `finishes` and that line's occurrence count are the same
    // number by construction. That identity is item 42's I5
    // self-validation: over any archived protocol run the counter
    // and the grep must agree, and a disagreement means the
    // instrument is measuring something other than the emitter.

### `:599-605` — FENCE / F2+F5d

*compressed to 1 line(s) at the site.* the provenance log fires on THIS edge; at prepare it would print the map the resim was about to change

    // [item 48] THE SLOT-PROVENANCE LOG — one Verbose line per
    // character, ONCE PER COMPLETED RESIM. This edge is chosen
    // because it is the only point at which a replay's whole
    // effect on the cache is finished and visible: the span has
    // been written, the protections have been taken, and the
    // frontier slot has been published. Emitting at prepare would
    // show the map the resim was ABOUT to change.

### `:607-612` — FENCE / F1a+F4

*compressed to 3 line(s) at the site.* VERBOSE-ONLY and read by no decision; the DiagnosticsConventions read-seam convention is what makes that checkable

    // ⛔ It is VERBOSE-ONLY and reads nothing any decision uses —
    // see `SimulationReconciliation::getDiagnostics().
    // logSlotProvenanceAll` and SlotStateProvenance.h. At the
    // shipped `LogOGResimProbe=Warning` these lines do not exist,
    // which is why a per-resim 60-character-per-character line is
    // affordable at all.

### `:662-668` — FENCE / F4+F6

*compressed to 3 line(s) at the site.* the denominator exists BECAUSE its log line is invisible at shipped verbosity -- delete the counter and read the log and you read nothing

    // [item 42 / I1] THE DENOMINATOR, and the first thing that task exists to
    // fix. Both branches below already existed; what did not exist was any way
    // to see the declining one. Its log line — `[ResimCheck.IsSimilar]`, one
    // line down — emits at `Log` under `LogOGSimTick`, which ships at `Warning`,
    // so it has ZERO occurrences in every archived log and trigger counts have
    // always been read against nothing. This counts instead of logging, and the
    // per-window flush below is at Warning on its own category.

### `:703-705` — FENCE / F5c

*compressed to 1 line(s) at the site.* prepares == the [Resim.Prepare] occurrence count by construction

    // [item 42 / I5] Counted at the same edge the existing `[Resim.Prepare]`
    // line reports, so `prepares` equals that line's occurrence count by
    // construction — the other half of I5's self-validation equality.

### `:830-836` — FENCE / F1a+F2

*compressed to 3 line(s) at the site.* Warning, not Log, and on its own category -- four instruments have already been lost to verbosity

    // ⚠ WARNING, NOT Log, AND ON ITS OWN CATEGORY. This initiative has lost FOUR
    // instruments to verbosity — item 36's `Log`-level line invisible on a
    // dedicated server, T33's `[RelayDepth]`, T35's proof line, and item 31's own
    // missing denominator `[ResimCheck.IsSimilar]`, which has zero occurrences in
    // every log on disk because it emits at `Log` under a category that ships at
    // `Warning`. A per-window line here that could not be seen at shipped verbosity
    // would repeat that defect on the instrument built to end it.

### `:838-843` — FENCE / F1a+F6

*compressed to 3 line(s) at the site.* *** STALE VALUE *** the tag must not begin [Resim. or [ResimCheck.; the LogOGSim=Verbose it cites is no longer the shipped value

    // ⛔ AND THE TAG DELIBERATELY DOES NOT BEGIN `[Resim.` OR `[ResimCheck.`.
    // `[Resim.` inherits `LogOGSim=Verbose` (T19's 10 MB defect) and `[ResimCheck.`
    // splits across two categories. `[ResimProbe.` is its own family with its own
    // knob: `LogOGResimProbe=Warning` keeps these three, `=Verbose` adds the
    // per-event detail, `=NoLogging` drops both, none of which disturbs any
    // neighbour.

### `:845-848` — FENCE / F2+F5c

*compressed to 3 line(s) at the site.* three lines, not one, split by pipeline stage; 5 Warning lines per window against a budget of 6

    // THREE LINES, NOT ONE, and the split is by PIPELINE STAGE so each is readable
    // alone: Gate = did we ask, Chaos = did the engine accept, Apply = did it land.
    // Together with the game thread's two `[ResimProbe.Landing]` lines that is 5
    // Warning lines per window per category, inside item 42's budget of 6.

### `:855-865` — FENCE / F1a+F2

*compressed to 4 line(s) at the site.* *** FALSE TODAY *** fields are APPENDED not inserted so archived greps still match; the 'structurally 0 under the shipped legacy policy' half is false

    // I1 — the denominator. `checks` always equals the window length (the
    // window is driven by it) and is printed as the denominator of the rate,
    // exactly as `[DivergenceProbe.Window]` prints `samples`.
    // [item 45] `deepSkips` APPENDED, not inserted, so every archived grep that
    // keys on `checks=` / `declined=` / `requested=` still matches and the item
    // 43 baseline stays comparable field-for-field. It counts CHARACTER-FRAMES on
    // which a pending anchor sat deeper than `rollbackWindowTicks` below its
    // character's frontier and was skipped rather than clamped — structurally 0
    // under the shipped legacy policy, which enforces no depth policy at all.
    // A nonzero reading after item 46's flip means anchors are being stranded and
    // is read against `requested`, not against `checks`.

### `:867-873` — FENCE / F1a

*compressed to 2 line(s) at the site.* survivingAnchors appended for the same grep-stability reason, and fed from a different edge than this sweep

    // [item 57 / RN-6] `survivingAnchors` APPENDED, not inserted, for the same
    // grep-stability reason `deepSkips` was appended rather than inserted: every
    // archived line keying on `checks=`/`declined=`/`requested=`/`deepSkips=`
    // still matches. It is fed from the `[Resim.Finish]` apply edge, not from
    // this call's own `checkDivergenceAll` sweep — see
    // `ResimGateWindowSummary::survivingAnchors` for why it still lands in this
    // window, and why it is on THIS line rather than `[ResimProbe.Apply]`.

### `:880-892` — FENCE / F4+F6

*compressed to 4 line(s) at the site.* clamped reads a constant 0 live and a nonzero is an engine-behaviour-change alarm, not a bug in the counter

    // I3 + I4 — the second gate. `refused` is trustworthy as a refusal count
    // because a refusal leaves needsResimulation() true and the request repeats
    // (finding §4a — and [item 45] the property SURVIVES the redesign by a
    // stronger argument: a refusal consumes nothing, so the pending anchor is
    // still pending on the next frame. It is now impossible for a refusal to
    // silently clear the gate, where previously it relied on no bit having
    // moved); `repeat` is that run's own signature; `clamped` counts
    // requested-vs-granted mismatches, which on this wiring can only mean the
    // grant was DEEPENED (engine-side FMath::Min requester, or validation
    // walking down) — a shallow clamp is structurally impossible (downward
    // walk + Min merge + PhysicsStep == ResimStep; item 42 review §2), so it
    // reads a constant 0 live and a nonzero is an engine-behaviour-change
    // alarm. See ResimGateWindowSummary::clampedGrants.

### `:900-902` — FENCE / F6

*compressed to 1 line(s) at the site.* read stuck in preference to abandoned; abandoned carries a +-1 boundary term

    // I5 + I6 — the apply edge and the replay span. Read `stuck` in preference
    // to `abandoned` on any single window: `abandoned` carries a ±1 boundary
    // term (see ResimGateWindowSummary::abandoned) and `stuck` does not.

### `:938-941` — FENCE / F2

*compressed to 1 line(s) at the site.* one-shot per episode, not per stuck frame -- the stuck state persists by construction

    // The Verbose line is ONE-SHOT PER EPISODE (the probe gates it), not per
    // stuck frame: the stuck state can persist for many consecutive frames by
    // construction, and a per-frame line here is exactly the volume class T19
    // was filed to stop.

### `:1092-1096` — FENCE / F5c

*compressed to 1 line(s) at the site.* replayTicks counted at the [Resim.Pre] edge; replayTicks/finishes is the mean span

    // [item 42 / I6] One replay tick. Counted at the same edge the existing
    // `[Resim.Pre]` line reports — the pairing item 31 step 0 derived by hand
    // (69 replay ticks over 41 passes, mean span ~1.7) and which this makes
    // permanent. `replayTicks / finishes` is the mean span; a value in the
    // 12-20 range would put rollback-window coalescing back on the table.

## 7. The three step functions: collect, allocate, capture

### `:557-560` — FENCE / F5d

*compressed to 2 line(s) at the site.* the apply-resim edge condition -- unique to the last resim sub-step's PostSolve

    // Apply-resim edge: Chaos still in resim mode but our clock has
    // caught up (advanceResimulation brought m_resimulationTick up
    // to m_predictionTick during this sub-step's PreSim). Unique to
    // the last resim sub-step's PostSolve.

### `:779-782` — FENCE / F5a

*compressed to 2 line(s) at the site.* sentinel semantics: 0 before the first integrate, and 0 is the reserved pre-sim tick

    // Tick of the most recently integrated step — authority tick / prediction tick /
    // resim tick, i.e. whichever step integrateAll just ran. Consumed by the manager's
    // post-integrate inbound-hit routing pass to one-shot projectile slots that ended
    // THIS tick. Returns 0 before the first integrate (0 is the reserved pre-sim tick).

### `:807-812` — NARRATIVE / LABEL

*compressed to 3 line(s) at the site.* what the lifecycle forwarders are for

    // Character-lifecycle notifications — forwarded to the systems executor so
    // each system can maintain its own per-character bookkeeping (e.g. the hit-
    // routing system's rootBodyId map). Called by the engine adapter on
    // register/unregister, out of band from the integrate loop. The storage +
    // static data are supplied from the integration layer so a system's hook
    // can read the just-(un)registered character out of storage (§3.11 timing).

### `:957-958` — FENCE / F5a

*compressed to 2 line(s) at the site.* dt must be carried onto the synthesized Stall/Skip wrappers or per-tick timers stop

    // baseStep already carries dt from ClientPredictionClock; preserve it on
    // the synthesized Stall/Skip wrappers so per-tick timers advance correctly.

### `:970-972` — FENCE / F6

*compressed to 1 line(s) at the site.* getStepKind() collapses HardResync to Normal -- the enum hides the case, so both are logged

    // result captures HardResync distinctly; step.getStepKind() collapses it
    // to Normal (treated like Normal for the integrate step). Log both so a
    // HardResync advance is visible in the trace.

### `:981-988` — FENCE / F5d

*compressed to 2 line(s) at the site.* capture completes the frontier pair opened by collect; the contract itself is owned by SimulationReconciliation.h

    // [og-netcode-v2-input-relay item 84] Capture completes the frontier
    // pair opened by collect — see the frontier-pair contract
    // (Reconciliation::allocateFrontierSlotsAll's header;
    // CorrectionCache.h's m_frontierSlotAwaitingState). Capture itself
    // runs later, from onPostGameSimulation's non-resim branch.
    // [item 87] Re-targeted off `m_netSync` onto the resolution peer,
    // which owns collectInputAll (named prepareSimulationStep between
    // item 90 and item 94).

### `:990-1001` — FENCE / F1b

*compressed to 3 line(s) at the site.* the facade is PREDICTION-only by name and by design; the authority path deliberately does not use it

    // [item 96] THE SINGLE CALL. This used to be two statements here —
    // `m_inputResolution.collectInputAll(step)` followed by
    // `m_reconciliation.allocateFrontierSlotsAll(step)` — with a
    // sweep-boundary fence and the item-91-I exception-safety debt note
    // both stated at this exact spot. Both texts, and the ordering
    // invariant they document, now live on
    // `SimulationInputResolution::preparePredictionSimulationStep`
    // (`SimulationInputResolution.h`, beside `collectInputAll`), which
    // performs the identical two calls in the identical order — this is
    // a re-route, not a behaviour change. This is the PREDICTION-only
    // facade, by name and by design (see that function's own banner);
    // `onGameSimulationAuthority`, below, deliberately does not use it.

### `:1004-1006` — FENCE / F5d

*compressed to 1 line(s) at the site.* preIntegrate BEFORE integrateAll; m_lastStep BEFORE postIntegrate

    // Sequencing: fire preIntegrate BEFORE integrateAll; save m_lastStep
    // (so currentIntegratedTick() == step.getTick() for postIntegrate hooks)
    // BEFORE firing postIntegrate. See §7 "Sequencing consideration".

### `:1035-1043` — FENCE / F5d+F4

*compressed to 3 line(s) at the site.* on the authority this collect opens no frontier pair: the server overload allocates no correction cache at all

    // [og-netcode-v2-input-relay item 84, re-scoped item 90, re-scoped
    // AGAIN item 94] Unlike onGameSimulationPrediction, this collect call
    // does NOT open the frontier-pair contract (see
    // `Reconciliation::allocateFrontierSlotsAll`'s header): the server
    // `registerSimulatable` overload allocates no correction cache at all
    // (registerPredictionOwner is called with a null provider, so
    // collectInputForCharacter's provider-present branch never matches
    // here; every FULLY-REGISTERED authority id lands in the remote-queue
    // branch instead).

### `:1045-1064` — FENCE / F3+F1a

*compressed to 4 line(s) at the site.* D4-shaped: the authority NEVER calls allocateFrontierSlotsAll, and collect/allocate/capture agree by all three being absent

    // [item 94] THIS METHOD DOES NOT CALL `reconciliation.
    // allocateFrontierSlotsAll(step)` AT ALL — the answer to the question
    // that started this investigation. Pre-94, sweep 2 ran on every role
    // (guarded per-id by a `queueMap` lookup, then item 92's loud-failure
    // `OG_CHECK` as an independent backstop); post-94, the sweep is
    // storage-driven and cache-population-filtered on the RECONCILIATION
    // peer, and on the authority role EVERY fully-registered id has no
    // cache — the sweep would iterate an empty filter over and over for
    // no reason, so this role simply never calls it.
    // `allocateFrontierSlotsAll` is now, literally, single-caller (through
    // `preparePredictionSimulationStep`, called from
    // `onGameSimulationPrediction`, above). The role's
    // collect-and-capture-both-absent comment extends naturally to
    // allocation: collect (this call), allocate (never called here), and
    // capture (`onPostGameSimulation`'s `postPredictionAll`, never
    // reached on this role — `m_runsPrediction == false`) all agree on
    // authority by all three being absent, not by one completing another.
    // [item 87] Re-targeted off `m_netSync` onto the resolution peer.
    // [item 90 / item 94] `collectInputAll` -> `prepareSimulationStep` ->
    // `collectInputAll`.

### `:1066-1071` — FENCE / F1a

*compressed to 3 line(s) at the site.* MUST NOT route the authority collect through the prediction-only facade

    // [item 96] MUST NOT ROUTE THROUGH `preparePredictionSimulationStep`.
    // That facade's name encodes PREDICTION-only, deliberately (see its
    // banner, `SimulationInputResolution.h`): it always pairs this call
    // with `allocateFrontierSlotsAll`, which — per the paragraph above —
    // this role must never call. Calling `collectInputAll` directly, as
    // below, is the correct and only correct choice on this path.

### `:1098-1111` — FENCE / F5d

*compressed to 4 line(s) at the site.* the resim collect resolves against slots that ALREADY exist and never pushes a prediction tick

    // [og-netcode-v2-input-relay T6, re-targeted item 87] The resolution
    // peer, not reconciliation. The resim input is resolved from the
    // delay lines / relay stores / neutrals the resolution peer owns;
    // reconciliation now contributes only the per-tick applied-capture
    // reference, which the resolution peer asks it for internally.
    // [og-netcode-v2-input-relay item 84] This collect call does NOT open
    // the frontier-pair contract either — collectResimInputAll resolves
    // input against slots that already exist (getAppliedCaptureTickRef)
    // and never calls pushPredictionTick. A resim tick's state write
    // (postResimulationAll -> tryInsertingResimulatedState, called from
    // onPostGameSimulation's resim branch) is a REPLAY over an already-
    // allocated slot, not the pair's completing half — see the frontier-
    // pair contract at Reconciliation::pushPredictionTick's header for
    // what that half actually is.

### `:1113-1114` — FENCE / F5d

*compressed to 1 line(s) at the site.* systems fire on every resim replay tick too, or routing stops being deterministic across rollback

    // Sequencing (see onGameSimulationPrediction). Systems fire on every
    // resim replay tick too — routing stays deterministic across rollback (D4).

### `:1146-1149` — FENCE / F5a

*compressed to 2 line(s) at the site.* storage is held by reference so the hook fan-out and the integrator iterate the SAME storage -- no bridge accessor

    // Externally-owned object storage + static data (owned at the engine adapter's
    // composition root, shared by all peers). Held by reference here so the
    // manager's system-hook fan-out and lifecycle forwarders read/mutate the same
    // storage the integration executor iterates — no bridge accessor.

### `:1159-1162` — FENCE / F5b+F5d

*compressed to 3 line(s) at the site.* m_lastStep crosses PreSim -> PostSolve so the capture targets the tick that produced the state

    // Step from the most recent onGameSimulation call, consumed by the
    // matching onPostGameSimulation. Crosses the PreSim → PostSolve boundary
    // so the post-solve cache writes target the same tick / StepKind as the
    // integrate step that produced the state being captured.

## 8. The concept-proof namespace

### `:259-266` — FENCE / F2+F6

*compressed to 2 line(s) at the site.* Params exists because a same-category duck-typed transposition COMPILES and misbehaves only at runtime

    // Grouped dependency refs for the ctor. Bundles the five
    // peer refs + storage + staticData + logger into one named aggregate so the
    // two composition-root emplace() sites cannot silently transpose the adjacent
    // duck-typed peer refs (a transposition of two same-category refs COMPILES —
    // they are duck-typed — and misbehaves only at runtime). Nested so it inherits
    // the enclosing template params: call sites write ManagerType::Params{ ... }
    // without re-naming the seven type arguments. Only the scalar loop config
    // (shouldRunPrediction / tickFrequency) stays positional.

### `:1177-1182` — FENCE / F6

*compressed to 1 line(s) at the site.* the proof exists because the transposition it rejects COMPILES on duck-typed refs

    // [og-netcode-v2-input-relay item 80 / B6, extended item 87] COMPILE-CHECKED
    // PROOF that the concept vocabulary applied above actually rejects a
    // same-category transposition — the exact scenario `Params`' own comment
    // (top of this class) says compiles today for a duck-typed, unconstrained
    // peer reference: "a transposition of two same-category refs COMPILES —
    // they are duck-typed — and misbehaves only at runtime."

### `:1184-1195` — FENCE / F6+F5c

*compressed to 3 line(s) at the site.* three shapes share wipeAllForResync; the DISTINGUISHING members are enumerated by peer

    // Minimal local types, each shaped like exactly one peer. Three of them —
    // NetSync, InputResolution, Reconciliation — now share `wipeAllForResync`
    // verbatim (item 87 added the resolution peer as a third sharer; NetSync's
    // own copy of the method left with the other two collect* members it moved
    // off — see `SimulationNetSyncTickConcept`'s own history comment). A
    // wipe-only clause could not distinguish any of the three from another; the
    // DISTINGUISHING members are `collect*` (resolution), `postPredictionAll` /
    // `consumeResimAnchorsAll` (reconciliation) and `sendCorrectionAll`
    // (netsync) — exactly what the positive/negative controls below check.
    // Swapping a shaped type into another concept's slot is the transposition
    // itself, and it is now a hard compile-time rejection rather than a silent
    // duck-typed pass-through.

### `:1199-1203` — NARRATIVE / LABEL

*compressed to 1 line(s) at the site.* names NetSyncShaped and what it satisfies

    // Shaped like a NetSync peer. Satisfies every SimulationNetSyncConcept
    // member — [item 87] `collectInputAll` (RENAMED `prepareSimulationStep`
    // at item 90) / `collectResimInputAll` / `wipeAllForResync` left this
    // shape along with the concept; what remains is transport + the
    // receive-side guard.

### `:1211-1213` — NARRATIVE / LABEL

*compressed to 1 line(s) at the site.* names InputResolutionShaped

    // [item 87, renamed item 94] Shaped like the resolution peer. Satisfies
    // every SimulationInputResolutionTickConcept member — exactly the three
    // methods that LEFT NetSyncShaped above for this shape.

### `:1221-1222` — NARRATIVE / LABEL

*compressed to 1 line(s) at the site.* names ReconciliationShaped

    // Shaped like a Reconciliation peer. Satisfies every
    // SimulationReconciliationConcept member.

### `:1264-1267` — FENCE / F6

*compressed to 1 line(s) at the site.* negative-controls rule: these ARE the transposition Params guards against, caught at compile time

    // --- NEGATIVE CONTROLS — THE TRANSPOSITION. A shaped ref used where a
    // different peer's typed slot is expected. This is the failure the
    // Params aggregate exists to guard against structurally; these lines are
    // the concept vocabulary catching it at compile time instead. ---

### `:1278-1281` — NARRATIVE / LABEL

*compressed to 2 line(s) at the site.* names the resolution peer's two-direction negatives

    // [item 87] THE RESOLUTION PEER'S TRANSPOSITION NEGATIVES, BOTH
    // DIRECTIONS — against BOTH the NetSync- and Reconciliation-shaped
    // types, exactly the two the shared `wipeAllForResync` name could be
    // mistaken for distinguishing.

### `:1316-1318` — NARRATIVE / LABEL

*compressed to 1 line(s) at the site.* names the Empty control

    // A plain nonconforming type (no members at all) satisfies none of the
    // four constrained concepts, including the ones with no SimulatableTs
    // wrinkle to work around.

## 10. The clocks

### `:16-22` — FENCE / F1b+F1d

*compressed to 2 line(s) at the site.* the two concept-only includes: why they exist and why SimulationNetSync.h is NOT one of them

    // [item 80 / B6] The two peer concepts this file names directly, so the
    // `requires` clauses below can use them. Nothing else in this file needed the
    // concrete peer classes (NetSyncT / ReconciliationT / IntegrationExecT stay
    // duck-typed template parameters) — these two includes exist ONLY to bring
    // the concept declarations into scope. NetSyncT is constrained by two
    // sibling concepts DEFINED IN THIS FILE, not by including SimulationNetSync.h
    // — see the rationale beside SimulationNetSyncTickConcept below.

### `:77-77` — NARRATIVE / LABEL

*kept at the site unchanged.* names SimulationUpdateInfo

    // SimulationUpdateInfo — passed from the Chaos async callback into onGameSimulation.

### `:216-217` — NARRATIVE / LABEL

*kept at the site unchanged.* the class's template signature

    // SimulationManager<IntegrationExecT, NetSyncT, InputResolutionT,
    //                   ReconciliationT, SystemsExecT, StorageT, StaticDataT>

### `:219-224` — NARRATIVE / LABEL

*compressed to 3 line(s) at the site.* what the class is and what it holds

    // Orchestrates the simulation loop. Holds references to the five peers
    // (integration executor, net sync, input resolution, reconciliation, systems
    // executor), plus references to the externally-owned object storage and
    // static data, and owns the clocks. Storage + static data live at the engine
    // adapter's composition root (shared by all peers); the manager reaches them
    // directly rather than bridging through the integration executor.

### `:233-239` — NARRATIVE / LABEL

*deleted, archived here.* what SystemsExecT is; the ordering it mentions is fenced at its own call site

    // The SystemsExecT peer (the SimulationSystemsExecutor — the ogsim-system-api
    // "fourth peer") fires cross-simulatable system hooks around integrateAll:
    // firePreIntegrate BEFORE integration, firePostIntegrate AFTER (see the
    // sequencing note in onGameSimulation* below). Character-lifecycle
    // notifications are forwarded through notifyCharacterRegistered/Unregistered.
    // The manager depends only on the peer's duck-typed method surface — it never
    // names a concrete system type.

### `:241-246` — NARRATIVE / LABEL

*compressed to 3 line(s) at the site.* the R7 Grade-4 pointer to Perspective-AuthorityVsPrediction.md -- task 12 pins this INLINE

    // What this loop runs on the authority and what it runs under prediction,
    // and which of the differences are role gates rather than separate code
    // paths: `docs/Perspective-AuthorityVsPrediction.md` — §4 ("The three step
    // functions, and the one line where they differ") covers the three
    // onGameSimulation* members below, §3 ("The role gates come in four kinds,
    // and they are not interchangeable") the gates they consult.

### `:248-248` — NARRATIVE / LABEL

*compressed to 4 line(s) at the site.* layer line, one line, no pointer is shorter

    // Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.

### `:330-333` — FENCE / F6

*compressed to 1 line(s) at the site.* ANTI-SYMMETRY: the ctor param named tickFrequency is a dt in seconds

    // SimulationManager's `tickFrequency` ctor param is actually the fixed
    // physics dt in seconds (it's passed in from solver->GetAsyncDeltaTime()).
    // Feed that directly to ServerTickClock so the steps it produces carry
    // the right dt through to integrate().

### `:342-342` — NARRATIVE / LABEL

*kept at the site unchanged.* names getTimeConfig

    // Read-only view of the manager's owned TimeConfig.

### `:494-494` — NARRATIVE / BANNER

*kept at the site unchanged.* section rule for the dispatch group

    // Simulation dispatch — called from FSimulationManagerAsyncCallback

### `:713-713` — NARRATIVE / BANNER

*compressed to 1 line(s) at the site.* section rule for the probe

    // [og-netcode-v2-input-relay item 42] THE RESIM-GATE PROBE — PHYSICS THREAD.

### `:738-741` — NARRATIVE / BANNER

*compressed to 2 line(s) at the site.* section rule for the Diagnostics view

    // [og-netcode-v2-input-relay task 59] THE RESIM-GATE PROBE'S DIAGNOSTIC
    // VIEW — RN-9 + amendment, grouped per `docs/DiagnosticsConventions.md`. A
    // sibling ruling groups `SimulationNetSync`'s four probe accessors the
    // same way, in the same task.

### `:827-828` — NARRATIVE / BANNER

*compressed to 2 line(s) at the site.* section rule for the flush

    // [og-netcode-v2-input-relay item 42] THE PER-WINDOW FLUSH — three Warning
    // lines on LogOGResimProbe, emitted only when a window closed.

### `:1073-1073` — NARRATIVE / LABEL

*kept at the site unchanged.* STUB (3 words): the resim sequencing back-pointer

    // Sequencing (see onGameSimulationPrediction).

### `:1138-1141` — NARRATIVE / LABEL

*compressed to 1 line(s) at the site.* names the resolution peer member

    // [item 87 / design §A.3] The resolution peer — a real
    // composition-root-constructed sibling, not owned by NetSyncT. See
    // SimulationInputResolutionTickConcept above for what the manager reads
    // from it and why.

## 9. Claims that were FALSE when quoted (D3 record, 2026-08-22)

Every one of these was verified against the tree on 2026-08-22 by reading the cited
file, not by reading another comment. They are quoted above verbatim; this section says
what is actually true and where the header now says it.

### F-T12-1 — `:26-37`

**The header said:** "templated on the CONCRETE `SimulationInputResolution<Ts...>`/`SimulationReconciliation<Ts...>` class templates (deduction needs the real types, not a duck-typed concept)"

**The tree says:** `preparePredictionSimulationStep` is declared `template <typename InputResolutionT, typename ReconciliationT>` at `SimulationStepSequencing.h:159`, and that file states at `:151-157`: "Duck-typed deliberately … no concrete class name appears in this signature." The claim was true of item 96's FIRST landing, when the function lived in `SimulationInputResolution.h`; the relocation to `SimulationStepSequencing.h` falsified it.

**Disposition:** ⚠ This paragraph was the whole stated justification for the `#include` at `:38`. If the justification is false the include may be unnecessary — but removing it is a CODE change and R6 requires zero code delta, so the header now says the stated reason is stale AND that whether the include is required is unverified, explicitly declining to invite its removal.

### F-T12-2 — `:996-997`

**The header said:** "now live on `SimulationInputResolution::preparePredictionSimulationStep` (`SimulationInputResolution.h`, beside `collectInputAll`)"

**The tree says:** Wrong three ways: the function is at `SimulationStepSequencing.h:159`, it is a FREE FUNCTION and not a member of `SimulationInputResolution`, and it is not beside `collectInputAll`. The call site in `onGameSimulationPrediction` invokes it unqualified, with three arguments. (Anchored on the enclosing member rather than on a line number: the pre-sweep `:1002` anchor is already out of range for the shipped file, which is the same fragile-join failure this section exists to record.)

**Disposition:** Raised by the lead from task 14's concurrent audit; verified here independently from the call sites before acting. The header now names `SimulationStepSequencing.h` as the home.

### F-T12-3 — `:1068`

**The header said:** "see its banner, `SimulationInputResolution.h`"

**The tree says:** The banner is at `SimulationStepSequencing.h:50-53`.

**Disposition:** Same relocation, third site.

### F-T12-4 — `:838-843`

**The header said:** "`[Resim.` inherits `LogOGSim=Verbose` (T19's 10 MB defect)"

**The tree says:** the shipped configuration sets `LogOGSim=Warning` — one adapter's binding for that is the `LogOGSim` entry in `Config/DefaultEngine.ini`. `Verbose` was the value T19 MEASURED against; it is not the shipped value. The fence's payload — do not name the tag `[Resim.` — survives on knob-independence alone, which is verified: `[Resim.` routes to `LogOGSim` and `[ResimCheck.` splits across `LogOGSim` and `LogOGSimTick` (`SimulationManagerUImpl.cpp:155-164`).

**Disposition:** The header now states the routing without the stale level.

### The one that was TRUE, and why it matters

`:39-42` — *"It lives in its own header, not in either peer's"* — was correct all along,
about 950 lines from F-T12-2. Item 96's relocation updated **one of four** references in
this file, so the header carried the true and the false statement simultaneously. That
is the measurable shape of the defect, and it is why a sweep that only checks whether
cited symbols EXIST cannot catch it: every symbol in the false sentence resolves.

## 11. Coverage of this archive

Blocks in the pre-compression file: **119**. Quoted above: **116**.

Not quoted (3), each with the reason it needs no archive:

- `:2` — KEEP-VERBATIM (carried to the shipped file UNCHANGED — there is nothing removed to archive)
- `:1252` — NARRATIVE (carried to the shipped file UNCHANGED — there is nothing removed to archive)
- `:1331` — KEEP-VERBATIM (carried to the shipped file UNCHANGED — there is nothing removed to archive)

