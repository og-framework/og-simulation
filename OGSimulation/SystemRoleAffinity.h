#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

// ---------------------------------------------------------------------------
// SystemRoleAffinity — the one fact every SimulationSystem must state about
// itself: whether it is PART OF the step, or a SIDE EFFECT OF the step.
//
// Engine-free and game-agnostic OGSim core, in the global namespace (D12). It
// lives in its own header rather than in SystemsExecutor.h because a conformer
// has to NAME the enumerator in a `static constexpr` member, and a conformer is
// not obliged to include the executor (BrawlerHitRoutingSystem.h does not);
// pulling SystemsExecutor.h in for a two-line declaration would drag
// SimulationObjectStorage.h behind it.
//
// PART OF THE STEP (AllRoles). The system's outputs are RECOMPUTABLE from the
// tick's inputs and are consumed inside the simulation. They must be recomputed
// on every replayed tick, or a rollback replay diverges from the run it is
// replaying (D4). Cross-character routing of a combat signal is this kind.
//
// SIDE EFFECT OF THE STEP (AuthorityOnly). The system's outputs are MONOTONIC
// and live beside the simulation — no composite, no wire, never corrected,
// never rewound. They must be produced EXACTLY ONCE per tick. Today exactly one
// role has exactly-once ticks: the authority never rewinds, so it is the only
// role such a system may observe. A match score, a statistic or a telemetry
// counter is this kind.
//
// ⛔ EVERY SYSTEM MUST DECLARE IT, AND THERE IS DELIBERATELY NO DEFAULT. The
// concept requires the member, so a system that says nothing does not compile.
// Both silent defaults are wrong, in opposite directions: a silent AllRoles
// makes a forgotten side-effect system double-count on every client, once per
// replayed tick, forever; a silent AuthorityOnly removes a simulation system
// from every client's step. A typo in the member's name would pick whichever
// default existed instead of failing, which is the failure this costs +1 line
// per conformer to prevent.
//
// HOW THE EXECUTOR APPLIES IT — the half SystemsExecutor.h points here for.
// Nothing stores a role. Every one of the executor's four fire methods takes
// the caller's role as its LAST parameter, `isAuthority`, and asks
// `firesOnRole<SystemT>` before it projects the storage down to the system's
// view — so a system the role excludes is not merely not called, it is not
// projected either. The test is `if constexpr` over a compile-time constant, so
// for an AllRoles system the branch folds away and the executor costs exactly
// what it cost before the parameter existed.
//
// The two STEP hooks then pass through `checkAuthorityOnlyStep<SystemT>`, a
// tripwire reached only AFTER the gate admitted the system: a client's replay
// tick is already silent, so this can fire only if a caller claimed authority
// on a resimulated step. Either that caller lied about the role, or the
// authority has grown a replay path and the exactly-once property above no
// longer holds. The two LIFECYCLE hooks have no step to read and do not consult
// it — which is why an AuthorityOnly system's bookkeeping stays default off the
// authority BY CONSTRUCTION rather than by each hook remembering to return.
// ⚠ OG_CHECK compiles out where checks compile out: a dev/test tripwire, never
// a Shipping guarantee.
//
// ⚠ WHAT DECLARING IT DOES NOT BUY. Nothing checks that the value is RIGHT: a
// side-effect system declared AllRoles compiles, satisfies every check here,
// and double-counts. "No observable effect off the authority" is a per-system
// property, so the proof of it belongs in the system's own suite; this header
// only makes the claim one greppable line long.
// ---------------------------------------------------------------------------
enum class SystemRoleAffinity : uint8_t
{
    AllRoles,       // part of the step: fires wherever integrateAll fires, incl. every replay tick
    AuthorityOnly,  // side effect of the step: fires only where a tick is final on simulation
};
