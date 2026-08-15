#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

#include "OGSimulation/PCTimeManagement/TimeConfig.h"

// ---------------------------------------------------------------------------
// resimGate — THE EDGE-TRIGGERED RESIM GATE'S POLICY ARITHMETIC.
// (og-netcode-v2-input-relay item 45; design_task43_resim_gate_fix.md §1, §3
//  candidate D, §3.1, §4. The defect it repairs is finding_task31_resim_rate.md.)
//
// WHAT THE GATE IS NOW, in four lines, because every predicate below is named
// against it:
//   * A landed correction is an EVENT. When the trigger policy says so it sets a
//     per-character PENDING ANCHOR TICK (`StateCorrectionCache`, one atomic word,
//     coalescing to the newest tick).
//   * `needsResimulation()` is `anchor != 0 && anchor != frontier`. Nothing is
//     derived from per-slot bits any more; there is no scan.
//   * The resim-completion edge CONSUMES the anchor with a CAS. A correction that
//     lands mid-replay makes that CAS fail and survives as the next trigger.
//   * ⇒ TERMINATION IS STRUCTURAL. A consumed correction cannot re-trigger,
//     because nothing recomputes the gate from state the replay just wrote.
//
// WHAT IT USED TO BE, and why that is worth one sentence here: the gate was
// LEVEL-TRIGGERED on two bitsets, `needsResimulation` re-derived it every physics
// frame from a newest-first scan starting AT the frontier, and `pushPredictionTick`
// copied the frontier's `m_isResimulated` bit forward to hold the gate closed
// after a completed resim. That inheritance was a LOAD-BEARING GUARD, not a bug
// (design §1): without it the just-replayed slot one below an unflagged frontier
// re-triggered a 1-tick resim every tick, forever. The defect was the RE-OPEN
// condition — only a correction landing EXACTLY on the frontier slot could clear
// the shadowing bit, so the trigger rate measured clock alignment (~2 events
// against ~8,759 behind-frontier corrections) instead of divergence. The
// inheritance line is deleted BECAUSE the reader is gone, never on its own.
//
// -------------------------------------------------------------------------
// WHY THIS KERNEL IS A SEPARATE, STL-ONLY HEADER
// -------------------------------------------------------------------------
// Exactly the `correctionRotation` argument (Network/CorrectionRotation.h), for
// exactly the same reason. The two policy decisions below are consulted from two
// different places on two different threads —
//
//   shouldSetPendingAnchor      GAME thread, inside StateCorrectionCache::
//                               tryInsertingCorrectState (W1)
//   isAnchorWithinDepthPolicy   PHYSICS thread, inside SimulationReconciliation::
//                               checkDivergenceAll, per character (R1)
//
// — and neither site can be reached from a test without a cache or a storage tuple
// respectively. Pure predicates here can be swept exhaustively; the wiring is then
// proven by one case per site. A green kernel with an unwired caller ships the
// legacy gate, so both halves are required.
//
// -------------------------------------------------------------------------
// ⛔ THERE IS DELIBERATELY NO TRIGGER COOLDOWN. DO NOT ADD ONE.
// (User ruling, 2026-08-11, during item 45's implementation. Design §4 and backlog
//  items 45/46 both name a `resimCooldownTicks` rate ceiling; it was built, then
//  removed on this ruling. Recorded here because a reader who finds it named in
//  those documents and absent from the code must find the reason, not a hole.)
// -------------------------------------------------------------------------
// THE ARGUMENT, which is about correctness and not about cost:
//
//   1. A cooldown does the thing this whole item exists to stop. Item 45 repairs a
//      gate that absorbed corrections KNOWN to disagree with prediction and acted on
//      none of them. A rate ceiling takes a correction known to disagree and defers
//      acting on it for up to R frames, during which the client free-runs on state
//      the authority has already contradicted. That is the same defect with a
//      smaller constant.
//   2. THE THROTTLE IS ALREADY STRUCTURAL, and it is demand-driven rather than
//      arbitrary: the gate is consulted only on non-resim physics frames (Chaos
//      calls `TriggerRewindIfNeeded_Internal` on solver advances, never inside its
//      own rewind loop), and the anchor is consumed only on the completion edge. So
//      **at most one resim can be in flight, and at most one more can be pending**.
//      Corrections arriving during a replay COALESCE into the pending anchor via the
//      CAS-max and fire once, as a single deeper replay, instead of N shallow ones.
//      The bound is "one resim per completed resim", which scales with what the
//      machine can actually finish.
//      ⭐ This makes `ACompletedResimClosesTheGateAndItStaysClosed` (the termination
//      tripwire, og-simulation-tests) the COST bound as well as the correctness
//      bound. If that case ever goes red, the rate ceiling is gone too.
//   3. The storm a cooldown would bound is already forbidden by sequencing. One
//      resim per landing only happens under `OnDisagreement` WITH the degenerate
//      always-false verdict, and item 46 is hard-blocked on item 30 precisely so
//      that combination cannot ship.
//   4. If the measured rate is still unaffordable once the verdict works (design
//      §4's honest unknown: remote proxies may carry a STRUCTURAL disagreement
//      rate), the correct knob is PER-CLASS triggering — proxies weighted
//      differently from locally-predicted characters — not a blanket delay. Per-class
//      triggering costs accuracy where accuracy is cheap; a cooldown costs it
//      everywhere, including on the local character where any disagreement at all is
//      evidence of a real defect (design §4's floor).
//
// A correction arriving mid-replay must therefore RE-ANCHOR, never restart and never
// wait: the running resim is left alone (nothing can interrupt it), the anchor moves
// forward to the newer tick, and the next non-resim frame resimulates from there.
// That is exactly what the consume-CAS produces.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// ---------------------------------------------------------------------------
namespace resimGate
{
    // -----------------------------------------------------------------------
    // W1 — DOES THIS LANDED CORRECTION SET THE PENDING ANCHOR?
    //
    // `landedAtFrontier` is `tick == cache.getPredictionTick()` evaluated at
    // INSERT TIME, on the game thread, inside the hit branch — the same
    // comparison `classifyCorrectionLanding` makes for the landing probe, and the
    // same one the legacy gate made implicitly by clearing the frontier's
    // inherited bit only when the landing slot WAS the frontier.
    //
    // ⭐ `FrontierExact` REPRODUCES TODAY'S OBSERVABLE BEHAVIOUR AND IS THE
    // SHIPPED DEFAULT. In particular it DOES NOT CONSULT THE VERDICT, and that is
    // not an oversight: the legacy gate never read `predictionWasCorrect` either
    // (it is stored and consulted by nothing — item 30), so a verdict test here
    // would make the legacy policy trigger STRICTLY LESS often than the mechanism
    // it is supposed to reproduce, on the very knob setting the behaviour-neutral
    // landing rests on.
    //
    // ⚠ ONE KNOWN, DELIBERATE DIVERGENCE FROM THE LEGACY GATE, recorded here
    // because it is the only one: on a cache in which NO resim has yet completed
    // and no correction has yet landed on the frontier, every `m_isResimulated`
    // bit was false, so the legacy scan could see a BEHIND-frontier corrected slot
    // and would open the gate once. `FrontierExact` does not. It costs at most one
    // trigger per character per virgin cache — session start, and each hard resync
    // (`wipeCache` cleared those bits too) — against a measured population of
    // thousands of corrections, which is why item 45's acceptance criterion asks
    // for statistical indistinguishability rather than equality. Reproducing it
    // exactly would mean keeping the shadow bitset alive purely to emulate its
    // pre-steady-state corner, which is the stale-truth trap the retirement of
    // `m_isResimulated` exists to close.
    //
    // `OnDisagreement` is the DESIGNED trigger: any landed correction whose
    // authority state disagrees with what we predicted. It is DORMANT until item
    // 46 flips the default, because with today's degenerate always-false verdict
    // (item 30) "disagrees" is every landing — the modelled 3-6x physics-cost
    // storm of design §4. That is a verdict defect, not a gate defect, and the
    // sequencing (28 -> 30 -> 46) exists to keep the two apart.
    // -----------------------------------------------------------------------
    constexpr bool shouldSetPendingAnchor(TimeConfig::ResimTriggerPolicy policy,
                                         bool landedAtFrontier,
                                         bool predictionWasCorrect)
    {
        switch (policy)
        {
        case TimeConfig::ResimTriggerPolicy::OnDisagreement:
            return !predictionWasCorrect;
        case TimeConfig::ResimTriggerPolicy::FrontierExact:
        default:
            return landedAtFrontier;
        }
    }

    // -----------------------------------------------------------------------
    // THE DEPTH CEILING IS CONSULTED ONLY UNDER `OnDisagreement`.
    //
    // ⛔ READ THIS BEFORE "SIMPLIFYING" IT INTO AN UNCONDITIONAL CHECK. The ceiling
    // exists to bound the DISAGREEMENT trigger's worst-case depth (design §4: the
    // verdict gate supplies intent, the depth skip supplies boundedness). Under the
    // legacy policy the trigger rate is already bounded — by frontier-exact
    // coincidence, to ~2 events per archived run — so it has nothing to bound, and
    // applying it anyway would COST behaviour-neutrality: the depth skip DROPS a
    // trigger the legacy gate retries. An anchor stranded more than
    // `rollbackWindowTicks` below the frontier is reachable under the legacy gate via
    // a long stranded-resim episode (item 42's I5 class — ~20 % of prepares never
    // reach the apply edge, and the frontier keeps advancing throughout). Legacy
    // retries it at growing depth; the skip abandons it. That is a real behaviour
    // change in a regime whose duration nobody has measured yet.
    //
    // So the trigger policy selects the whole POLICY REGIME, not merely the
    // anchor-set condition, and `FrontierExact` means "the legacy gate, as measured".
    // Item 46's flip turns the trigger and its depth ceiling on together, which is
    // the configuration §4's cost envelope is computed for.
    // -----------------------------------------------------------------------
    constexpr bool policyEnforcesDepthCeiling(TimeConfig::ResimTriggerPolicy policy)
    {
        return policy == TimeConfig::ResimTriggerPolicy::OnDisagreement;
    }

    // -----------------------------------------------------------------------
    // R1 — IS THIS ANCHOR SHALLOW ENOUGH TO RESIMULATE FROM?
    //
    // SKIPPED AND COUNTED, NOT CLAMPED, and the distinction is the whole point
    // (design §3 candidate D (b)): clamping a too-deep anchor up to
    // `frontier - maxDepthTicks` would restore at a mid-window slot that NO
    // correction ever landed in, so the replay would re-integrate the same inputs
    // from the same state and produce the same prediction — a guaranteed no-op
    // costing a full Chaos rewind. Recovery for that regime is a NEWER correction
    // (which raises the anchor) or the HardResync failsafe.
    //
    // ⭐ THIS IS `rollbackWindowTicks`' FIRST REAL CLIENT-SIDE CONSUMER. TimeConfig's
    // ADR status note and its re-scoped deferral trigger (condition 3, "acquires a
    // real client-side consumer for any other reason") are rewritten in the same
    // change; if you are reading this because you are about to add a second one,
    // read that note first.
    //
    // `maxDepthTicks == 0` means "no depth policy" and admits every anchor. An
    // anchor at or AHEAD of the frontier is admitted too: it cannot be produced by
    // a landing (a correction above the frontier has no slot and is discarded
    // before it reaches W1) and the gate's own `anchor != frontier` clause already
    // handles equality, so treating it as infinitely deep would turn a
    // structurally impossible value into a silent drop.
    // -----------------------------------------------------------------------
    constexpr bool isAnchorWithinDepthPolicy(std::uint32_t anchorTick,
                                             std::uint32_t frontierTick,
                                             std::uint32_t maxDepthTicks)
    {
        if (maxDepthTicks == 0u)
            return true;
        if (frontierTick <= anchorTick)
            return true;
        return (frontierTick - anchorTick) <= maxDepthTicks;
    }

    // =======================================================================
    // [og-netcode-v2-input-relay item 47] THE REPLAY WRITE RULE — "AUTHORITY
    // BEATS A RE-DERIVATION OF IT", and the two-clause freshness classifier
    // that measures how often it bites.
    //
    // ⛔ THE DEFECT THIS REPAIRS (the HOLLOW ANCHOR).
    // `tryInsertingCorrectState` writes authority state into slot T on the GAME
    // thread. An in-flight replay on the PHYSICS thread then reaches tick T and
    // `tryInsertingResimulatedState` overwrote slot T with the REPLAYED state,
    // unconditionally — while `m_containsCorrectTick` stayed SET, so the slot
    // claimed authority provenance over a re-derivation. Item 45's surviving
    // anchor then correctly triggered a follow-up resim at T, `prepareResimAll`
    // restored the CLOBBERED state, and the replay faithfully reproduced the same
    // prediction. **The trigger was real; the data it pointed at was gone.**
    // PRE-EXISTING IN THE CLOBBER, NEW IN THAT IT MATTERS: before item 45 a
    // mid-replay landing triggered nothing, so nothing was waiting on that data.
    //
    // -----------------------------------------------------------------------
    // THE RULE IS PROTECT-ALL-CORRECTED, AND IT IS ONE BIT WIDE.
    // -----------------------------------------------------------------------
    // A replay never overwrites a slot with `m_containsCorrectTick` set. Not
    // "never overwrites a FRESH one" — never overwrites ANY of them. The reason
    // is the invariant followed to its conclusion (backlog item 47 amendment 2,
    // after the user review): even a "stale" corrected slot holds SERVER TRUTH
    // that the replay does not supersede, because the replay derives from the old
    // PREDICTION at the shared restore tick, not from newer authority. There is
    // no input to a replay that makes its output better than the authority state
    // it would overwrite.
    //
    // Costs, checked before adopting it rather than after:
    //   * the replay computes its live trajectory regardless — the cache write is
    //     a RECORD of what it computed, not a step in computing it;
    //   * no production consumer needs a corrected slot to hold replay output
    //     (`m_isResimulated`, the only provenance reader, retired with item 45);
    //   * the FRONTIER slot is ruled the same way ON PURPOSE — see the frontier
    //     note below.
    //
    // ⇒ TWO CONSEQUENCES WORTH STATING AS INVARIANTS, because both were broken
    //   before this item:
    //   1. a replay never overwrites a slot carrying a fresh, unconsumed
    //      correction (authority beats a re-derivation of it); and
    //   2. **THE PROVENANCE BIT CANNOT LIE.** Nothing authority-marked is ever
    //      overwritten, so NO bit is ever cleared, and "bit set ⇒ the state in
    //      this slot is authority-grade" holds BY CONSTRUCTION rather than by
    //      discipline across sites. The committed pre-47 state — bit set, state
    //      replayed — was the worst of both.
    //
    // ⚠ "AUTHORITY-GRADE" MEANS ADOPTED-AUTHORITY **OR** WITHIN-TOLERANCE-OF-
    // AUTHORITY. On an AGREEING landing `tryInsertingCorrectState` sets the bit
    // and SKIPS the state copy (`if (!predictionWasCorrect)`), so the slot keeps
    // the PREDICTED value — which the authority has explicitly certified as
    // matching. Protect-all preserves both readings; a rule that protected only
    // adopted state would need a second bit to tell them apart.
    //
    // ⭐ THE FRONTIER SLOT IS NOT AN EXCEPTION (item 47 amendment 3). It is what
    // `applyResimAll` reads to publish the resim's result into live state, so
    // protecting it means a resim that ends on a corrected frontier PUBLISHES THE
    // AUTHORITY STATE rather than the replay's. That is the invariant's own logic
    // applied at the one slot where it is externally visible, and it is pinned by
    // a named case rather than implied.
    //
    // -----------------------------------------------------------------------
    // AND NOW THE PART THAT IS **CLASSIFICATION ONLY**: FRESH vs STALE.
    // -----------------------------------------------------------------------
    // ⛔ READ THIS BEFORE WIRING `classifyResimSlotWrite` INTO ANY DECISION. The
    // WRITE decision above depends on ONE input — `slotContainsCorrectTick`.
    // Everything else this function takes exists to split the protections into
    // two POPULATIONS for the probe: `freshClobbersAvoided` is the live rate of
    // the defect at the top of this block, `staleClobbersAvoided` is hygiene.
    // Under protect-all the two produce the same action; only the counter differs.
    // That is what keeps item 47's new cross-thread reads OBSERVATIONAL (see the
    // threading note on `StateCorrectionCache::getSlotLandingSeq`).
    //
    // A slot is FRESH iff **EITHER** clause holds, and NEITHER CLAUSE ALONE
    // SUFFICES, because the defect has THREE exposure populations:
    //
    //   (a) mid-replay landings — a correction lands on GT while PT is replaying;
    //   (b) THE MULTI-CHARACTER MIN-FOLD — a character's corrected slot ABOVE the
    //       shared restore tick, landed BEFORE prepare;
    //   (c) post-item-46 agreeing landings above the anchor.
    //
    //   CLAUSE 1 — LANDING SEQUENCE (`slotLandingSeq > preparedLandingSeq`).
    //     A per-cache monotonic counter stamped on each landing's slot, captured
    //     at prepare. Catches (a) exactly — "this slot was written after this
    //     resim was prepared" is the definition of mid-replay. It calls (b) and
    //     (c) STALE, because those landed BEFORE the prepare.
    //
    //   CLAUSE 2 — TICK vs THE PER-CACHE CAPTURED ANCHOR
    //     (`slotTick >= capturedAnchorTick`).
    //     Catches (b) and (c): a corrected slot at or above the tick THIS CACHE
    //     anchored is information the resim was supposed to act on. It MISSES (a)
    //     when the mid-replay landing sits BELOW the captured anchor, which is
    //     reachable because the restore tick is a MIN across characters and
    //     another character's deeper anchor can drag the span below this one's.
    //
    // ⚠⚠ DO NOT REPLACE THIS WITH THE NAIVE `containsCorrectTick && tick >
    // runningAnchor`. It is the shape a reasonable reader reaches for first, and
    // with a SINGLE GLOBAL (min-folded) anchor it misclassifies population (b)
    // wholesale — the newer-anchored character's slots sit above the global min,
    // so everything reads "fresh" and the counter stops discriminating. The
    // anchor compared against here is the PER-CACHE capture taken at
    // `prepareResimAll`, never the folded min.
    //
    // `capturedAnchorTick == 0` means this resim was prepared with NO anchor
    // pending (an engine-side rewind we did not ask for). Clause 2 is then
    // vacuously true and every corrected slot classifies FRESH. That is
    // deliberate over-protection in the safe direction, and it is also what keeps
    // the single-character structural zero below honest.
    //
    // ⭐ STALE IS A 2+-CHARACTER-ONLY SIGNAL — A FREE CLASSIFIER-WIRING CHECK.
    // (Item 47's reachability analysis, inherited rather than re-derived.) For a
    // single character the replay span is `anchor+1..frontier` and "stale" needs
    // `tick < capturedAnchor`: DISJOINT RANGES. The only reachable stale
    // population is a NON-min character B restored at the shared min, whose span
    // `min+1..frontier` dips below B's OWN captured anchor `A_B`, passing B's
    // older corrections in `(min, A_B)`. So `staleClobbersAvoided` MUST read 0 in
    // any single-character session; a nonzero there means the classifier is wired
    // to the wrong anchor, not that a new population appeared.
    // =======================================================================

    // What a replay tick's write into one cache slot ACTUALLY did. Reported by
    // `StateCorrectionCache::tryInsertingResimulatedState` through a defaulted
    // out-pointer, so every pre-item-47 call site is byte-identical.
    //
    // ⭐ THE ACTION IS **DERIVED FROM** THIS VALUE — `outcome != Written` IS the
    // protection — RATHER THAN DECIDED BESIDE IT, and that is a correctness
    // property, not a style choice. It was found by a mutation run: an earlier
    // draft had a separate `replayMayOverwriteSlot(bit)` predicate at the write
    // site and used this classifier only for the counter. Breaking that predicate
    // alone produced a build in which the slot WAS overwritten while the outcome
    // still reported `ProtectedFresh` — i.e. `freshClobbersAvoided` counting
    // clobbers it had not avoided, which is precisely the "instrument measuring
    // the wrong quantity under the right name" failure this initiative has
    // shipped five times. One function, one truth: the counter cannot disagree
    // with the action because it IS the action.
    //
    // ⚠ THAT DOES NOT MAKE THE FRESH/STALE SPLIT LOAD-BEARING. Whether this
    // returns `Written` depends on `slotContainsCorrectTick` and NOTHING ELSE;
    // the four other parameters only choose BETWEEN the two protected classes. So
    // the observational argument on the landing stamps still holds exactly: a torn
    // or stale stamp can mislabel a counter, and can never mis-decide a write.
    //
    // `Discarded` is produced by the CACHE (the tick had no slot in the 60-slot
    // window — item 42's `replayOverruns`), never by the classifier below: with no
    // slot there is nothing to classify.
    enum class ResimSlotWriteOutcome : std::uint8_t
    {
        Discarded = 0,
        Written,
        ProtectedFresh,
        ProtectedStale,
    };

    constexpr ResimSlotWriteOutcome classifyResimSlotWrite(bool          slotContainsCorrectTick,
                                                           std::uint32_t slotLandingSeq,
                                                           std::uint32_t preparedLandingSeq,
                                                           std::uint32_t slotTick,
                                                           std::uint32_t capturedAnchorTick)
    {
        if (!slotContainsCorrectTick)
            return ResimSlotWriteOutcome::Written;

        const bool freshBySequence = slotLandingSeq > preparedLandingSeq;
        const bool freshByTick     = slotTick >= capturedAnchorTick;

        return (freshBySequence || freshByTick)
            ? ResimSlotWriteOutcome::ProtectedFresh
            : ResimSlotWriteOutcome::ProtectedStale;
    }

    // ⛔ THERE IS DELIBERATELY NO SECOND `replayMayOverwriteSlot(bit)` PREDICATE.
    // One draft had it; a mutation run proved it could disagree with the counter
    // (see the ACTION note on `ResimSlotWriteOutcome`). The write site asks this
    // function once and acts on `outcome != Written`. If a future edit needs the
    // write rule to stop being "the bit alone", the provenance invariant (2)
    // stops holding by construction and item 47 amendment 2's fallback shape —
    // clear `m_containsCorrectTick` on any overwrite, and rule explicitly on the
    // T4 capture-tick ref, which a replayed state was NOT produced by — becomes
    // mandatory rather than optional.

} // namespace resimGate
