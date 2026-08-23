#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGSimulation/SimulationTimeContext.h"

#include "OGSimulation/CompilerControl.h"
OGSIM_OPTIMIZE_OFF
// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.

// =============================================================================
// SimulationStepSequencing.h — CROSS-PEER SEQUENCING FACADES.
// (Relocated here from `SimulationInputResolution.h` on the user's ruling,
//  2026-08-21. The relocation record, and the argument for this home, are in
//  `docs/SimulationInputResolution-rationale.md` §8 and §15.2.)
// =============================================================================
// WHAT BELONGS IN THIS FILE, stated as a test and not as a vibe:
//
//     A free function that SEQUENCES TWO OR MORE PEERS, and that NAMES NO PEER
//     TYPE — it is duck-typed on its arguments and depends only on the tick
//     vocabulary (`SimulationTimeStep`).
//
// If a candidate names a concrete peer, it belongs with that peer. If it takes
// one peer, it is a method. This file is for the residue: the orchestration-
// order knowledge that belongs to NO single peer and therefore has no honest
// home inside one.
//
// WHY THIS FILE EXISTS AT ALL — the misplacement it corrects.
// `preparePredictionSimulationStep` first landed in `SimulationInputResolution.h`
// — and that cost nothing mechanically, because that header already included
// `SimulationReconciliation.h`, so no new coupling appeared — which is exactly
// why it slipped through. But the function NAMES NEITHER PEER: it is templated
// on both and deduces its return through `decltype`. Its only named type is
// `SimulationTimeStep`. Hosting it in one participant's header was therefore
// arbitrary — by identical logic it could have gone in
// `SimulationReconciliation.h` — and it made a peer that should be
// independently comprehensible the home of a TICK-SEQUENCING concern owned by
// the orchestration layer.
//
// ⚠ THE SAME MISPLACEMENT ALREADY EXISTS, AND IS OLDER.
// `registerSimulatable` / `unregisterSimulatable` (`SimulationNetSync.h`) are
// multi-peer sequencing facades sitting in one participant's header for the
// same reason. ⚠ THAT home is merely ARBITRARY, not wrong: NetSync IS a
// participant in those two. It is the COLLECT/ALLOCATE pair THIS FILE
// sequences that NetSync stopped touching, and that is what ruled its header
// out for THIS function. They are candidates to move here.
// ⛔ DELIBERATELY NOT MOVED WHEN THIS FILE WAS CREATED. Relocating them
// touches the registration ordering that the publish-last / unpublish-first
// fixes hardened (and whose invariant a live crash was fixed to protect);
// that move deserves its own task with its own tests, not a ride-along in a
// placement fix. Recorded here so the next author finds the reasoning rather
// than the omission.
// =============================================================================


// =============================================================================
// preparePredictionSimulationStep — THE SEQUENCING FACADE FOR THE COLLECT/
// ALLOCATE PAIR.
// =============================================================================
// The two calls were once FUSED into one, and the fusion's stated value was
// explicitly can't-misuse-it. They were then deliberately un-fused — allocation
// now lives in `SimulationReconciliation`, the class that owns the slots —
// and the review confirmed the resulting cost: `collectInputAll` became
// callable without `allocateFrontierSlotsAll`, and a caller who collects and
// lets capture (`postPredictionAll`) run anyway pushes state into the OLD
// frontier slot — the frontier-pair detector's uncovered direction (blind
// spot #2, `CorrectionCache.h`'s `m_frontierSlotAwaitingState`; see
// `collectInputAll`'s own banner in `SimulationInputResolution.h` for the
// successor obligation this trades on). Every EXISTING caller gets the order
// right today; nothing
// enforces that a future one will.
//
// THE PRECEDENT IS `registerSimulatable`/`unregisterSimulatable`
// (`SimulationNetSync.h`) — free functions whose sole purpose is sequencing a
// multi-peer operation that must not be got wrong, hardened for exactly that
// reason after two live crashes. Same shape, same class of hazard, applied here.
// ONE difference, and it is deliberate, not a departure: those two take the
// CONCRETE `SimulationObjectStorage<Ts...>`/`SimulationReconciliation<Ts...>`/
// etc. class templates, because their callers (the composition root) already
// hold the concrete types. This function is called from
// `SimulationManager::onGameSimulationPrediction`, whose own peer template
// parameters are duck-typed by design (see that class's own header comment) —
// so this function is duck-typed too (see its signature below), the same
// contract shape applied at a different genericity level, not a weaker one.
//
// ⛔ BE HONEST ABOUT WHAT THIS IS NOT: enforcement. `collectInputAll` and
// `allocateFrontierSlotsAll` both stay PUBLIC and INDEPENDENTLY CALLABLE —
// the authority role needs them separate (it calls only the former; see
// below). This function buys default-correctness and discoverability, the
// same as the register/unregister facades do for their own pair — NOT a
// compiler-checked guarantee. Blind spot #2 is UNCHANGED and stays open; DO
// NOT describe this as closing it. Overstating a guard is this initiative's
// signature defect, corrected three times already before this task — say
// what this is: the documented door, the way `registerSimulatable` is one.
//
// ⚠ PREDICTION-ONLY, BY NAME, DELIBERATELY. The authority path calls
// `collectInputAll` WITHOUT allocation ON PURPOSE
// (`SimulationManager::onGameSimulationAuthority` — every fully-registered
// authority id has no correction cache at all, so the allocating sweep would
// iterate an empty filter for no reason) and MUST NOT be routed through this
// facade. The name encodes that restriction; the matching statement lives at
// that call site.
//
// HOME, DECIDED NOT DEFAULTED. ⚠ The argument below predates this file: it was
// written against the function's first landing and weighs the same candidates;
// only the home it names changed. The relocation record is in this file's own
// top banner.
//
// This lives HERE, in `SimulationStepSequencing.h`, rather than in
// `SimulationInputResolution.h`, `SimulationNetSync.h` (where the
// register/unregister precedent lives) or `SimulationReconciliation.h`.
// NetSync is no longer party to either call since frontier allocation moved
// to `SimulationReconciliation`; it would be sequencing two peers it doesn't
// touch. `SimulationReconciliation.h` was considered and rejected: this
// function is deliberately DUCK-TYPED (see
// below — it takes the sequence's two calls generically, not the concrete
// `SimulationInputResolution<Ts...>`/`SimulationReconciliation<Ts...>` class
// templates), specifically so `SimulationManager`'s duck-typed
// `InputResolutionT`/`ReconciliationT` template parameters can call it
// without forcing those parameters to be the concrete classes — several
// existing LLTs instantiate the manager against hand-written mocks that
// satisfy the same two methods but are not those classes
// (`ResimGatePolicyTest.cpp`, `StateCorrectionCache4MethodApiTest.cpp`), and
// the manager's own header comment states this duck-typing as a design rule
// to preserve, not incidental. That removes the concrete-type argument either
// candidate header would have offered; what is left is the file test at the top
// of this header — a free function that sequences two peers and NAMES NEITHER
// belongs to no participant, and hosting it in one of them would be arbitrary.
// It fulfills the successor obligation stated at `collectInputAll`
// (`SimulationInputResolution.h`), which is the banner to read next.
//
// THE SWEEP-BOUNDARY FENCE, RELOCATED HERE FROM
// `SimulationManager::onGameSimulationPrediction` (its previous home; it
// belongs here because this function IS the two-statement sequence the fence
// governs): NO CONTROL FLOW may run between the two calls below. Every
// id's input has been consumed by collect (dequeued, delay-pushed, enqueued
// for send) and NO frontier slot has been allocated for this tick yet — that
// window is legal only because nothing intervenes.
//
// ⚠ CARRIED FORWARD VERBATIM FROM ITS ORIGINAL SITE — only the
// MECHANICS relocated.
// "NOTHING RUNS BETWEEN THEM" IS CONTROL-FLOW-ONLY, NOT EXCEPTION-SAFE.
// `collectInputAll`'s per-character body (`collectInputForCharacter`) makes
// `.at(id)` lookups that throw a real, unwinding C++ exception under this
// module's `/EHsc` build if the invariant they assume (provider-present
// implies line/queue-entry-present) is already broken elsewhere. An uncaught
// throw out of `collectInputAll` propagates straight past the
// `allocateFrontierSlotsAll` call below — ordinary C++ control flow, not a
// mechanism this code maintains — leaving every character already resolved
// before the throw point DESTRUCTIVELY CONSUMED WITH NO FRONTIER SLOT
// ALLOCATED for that tick.
//
// DECISION, RECORDED (unchanged since it was first written; this function is now
// its custodian): accepted as documented debt. Reasons: (1) this path is
// reachable only via an ALREADY-BROKEN registration invariant (a second bug
// required), so it is not a standalone defect surface; (2) making collect
// exception-safe would mean either a transactional rollback of partial
// consumption (queue/delay-line pops are not easily undoable) or reverting
// the two-call split's whole architectural point; neither is a five-minute
// fix, and does not justify that cost against a second-bug-required path. If
// a future author finds this path reachable WITHOUT a prior invariant break,
// that changes the calculus and this decision should be revisited.
// =============================================================================
// Duck-typed deliberately (see the banner's precedent paragraph): the return
// type is exactly whatever `inputResolution.collectInputAll(step)` returns
// (`ResolvedInputs<SimulatableTs...>` for the real
// `SimulationInputResolution<SimulatableTs...>`; a mock's own return type in
// a duck-typed `SimulationManager` test rig), and `reconciliation` is
// required only to provide `allocateFrontierSlotsAll(step)` — no concrete
// class name appears in this signature.
template <typename InputResolutionT, typename ReconciliationT>
auto preparePredictionSimulationStep(
    InputResolutionT&          inputResolution,
    ReconciliationT&           reconciliation,
    const SimulationTimeStep&  step)
    -> decltype(inputResolution.collectInputAll(step))
{
    auto inputs = inputResolution.collectInputAll(step);
    reconciliation.allocateFrontierSlotsAll(step);
    return inputs;
}

OGSIM_OPTIMIZE_ON
// pragma optimize on.
