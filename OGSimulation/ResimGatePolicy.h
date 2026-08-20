#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

#include "OGSimulation/PCTimeManagement/TimeConfig.h"

// ---------------------------------------------------------------------------
// resimGate — THE EDGE-TRIGGERED RESIM GATE'S POLICY ARITHMETIC.
// (og-netcode-v2-input-relay item 45; the defect it repairs is
//  finding_task31_resim_rate.md.)
//
// ORIENTATION:
//   * A landed correction is an EVENT. When the trigger policy says so it
//     sets a per-character PENDING ANCHOR TICK (`StateCorrectionCache`, one
//     atomic word, CAS-max, coalescing to the newest tick).
//   * `needsResimulation()` is `anchor != 0 && anchor != frontier`. Nothing
//     is derived from per-slot bits any more; there is no scan.
//   * The resim-completion edge CONSUMES the anchor with a CAS. A correction
//     landing mid-replay makes that CAS fail and survives as the next
//     trigger — TERMINATION IS STRUCTURAL, not policed by a cooldown.
//   * Four predicates below, and where each is consulted:
//       shouldSetPendingAnchor      GAME thread    — StateCorrectionCache::
//                                                     tryInsertingCorrectState,
//                                                     hit branch (W1)
//       policyEnforcesDepthCeiling  PHYSICS thread — SimulationReconciliation::
//       isAnchorWithinDepthPolicy   PHYSICS thread   checkDivergenceAll,
//                                                     per character (R1)
//       classifyResimSlotWrite      PHYSICS thread — StateCorrectionCache::
//                                                     tryInsertingResimulatedState,
//                                                     reading landing stamps the
//                                                     GAME thread wrote (W1)
//   * Pure predicates here are swept exhaustively; wiring is then proven by
//     one case per call site above. A green kernel with an unwired caller
//     ships the legacy gate, so both halves are required.
//
// Full derivation, defect history, and measurement records:
// `docs/ResimGatePolicy-rationale.md`.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// ---------------------------------------------------------------------------
namespace resimGate
{
    // ⛔ THERE IS DELIBERATELY NO TRIGGER COOLDOWN. DO NOT ADD ONE.
    // (User ruling, 2026-08-11. Design §4 and backlog items 45/46 both name a
    // `resimCooldownTicks` rate ceiling; it was built, then removed on this
    // ruling.) A rate ceiling defers a correction KNOWN to disagree with
    // prediction — the exact defect this item repairs, at a smaller constant.
    // The throttle is already structural: the gate is consulted only on
    // non-resim physics frames and the anchor is consumed only on the
    // completion edge, so at most one resim is ever in flight and at most one
    // more is pending — corrections landing mid-replay COALESCE into it via
    // the CAS-max instead of queuing.

    // -----------------------------------------------------------------------
    // W1 — DOES THIS LANDED CORRECTION SET THE PENDING ANCHOR? `landedAtFrontier`
    // is `tick == cache.getPredictionTick()` evaluated at INSERT TIME, on the
    // game thread, inside the hit branch.
    //
    // ⭐ `FrontierExact` REPRODUCES THE LEGACY GATE'S OBSERVABLE BEHAVIOUR AND
    // IS THE COMPILED DEFAULT (`TimeConfig.h`) — but NOT the shipped
    // configuration: `Config/DefaultEngine.ini:371` has overridden it to
    // `OnDisagreement` since item 43 (check that file for the current value,
    // not this comment). It does NOT consult `predictionWasCorrect` — the
    // legacy gate never read it either (item 30) — so ANDing a verdict check
    // onto this predicate would make it trigger no more often than it already
    // does, and STRICTLY less often only once a real verdict (item 30)
    // sometimes returns true. Under today's degenerate always-false verdict —
    // no longer just assumed: item 43 measured `OnDisagreement` firing on
    // 86-87% of physics frames — `!predictionWasCorrect` is true on nearly
    // every landing, so ANDing it in here would collapse to a no-op rather
    // than a reduction.
    //
    // ⚠ ONE KNOWN, DELIBERATE DIVERGENCE FROM THE LEGACY GATE, recorded here
    // because it is the only one: on a virgin cache (no resim yet completed,
    // no correction yet landed on the frontier) the legacy scan could open the
    // gate once for a behind-frontier corrected slot; `FrontierExact` does
    // not. Bounded to at most one trigger per character per virgin cache
    // (session start, each hard resync) against a measured population of
    // thousands of corrections.
    //
    // `OnDisagreement` is the DESIGNED trigger: any landed correction whose
    // authority state disagrees with prediction. It is the LIVE SHIPPED
    // CONFIGURATION (`Config/DefaultEngine.ini:371`, since item 43) — not
    // dormant. Item 46 (the *compiled default's* own flip to `OnDisagreement`)
    // remains blocked on item 30; that only governs what a build with no ini
    // override runs, not whether this trigger runs today. Full argument:
    // `docs/ResimGatePolicy-rationale.md` §3 point 3, §5.
    //
    // [og-netcode-v2-input-relay item 84 / design §F.2] WHY `landedAtFrontier`
    // CAN BE COMPUTED AT ALL: the frontier slot is allocated BEFORE the
    // physics update runs (`StateCorrectionCache::pushPredictionTick`, called
    // from collect), not after it at capture time, precisely so this word is
    // fresh for the whole update while a landing can arrive on the game
    // thread. `m_tickBuffer` is both the slot directory and the frontier
    // (`getPredictionTick()` is `max_element` over it), so allocation timing
    // IS trigger timing — it cannot slide to capture time without re-timing
    // this exact predicate for the full width of a physics step. Full
    // argument, including the four-read enumeration and the correction-
    // arrival race walked both ways: `CorrectionCache.h`'s write-site-1 block
    // (`m_frontierSlotAwaitingState`) and `DesignInputResolutionPeer.md` §F.
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
    // ⛔ READ THIS BEFORE "SIMPLIFYING" IT INTO AN UNCONDITIONAL CHECK. Under
    // `FrontierExact` the trigger rate is already bounded by frontier-exact
    // coincidence (~2 events/run, archived) — it has nothing to bound, and
    // applying the ceiling anyway would DROP a trigger the legacy gate
    // retries (a stranded-resim episode can sit deeper than
    // `rollbackWindowTicks` for a duration nobody has measured). The trigger
    // policy therefore selects the whole POLICY REGIME, not merely the
    // anchor-set condition — `FrontierExact` means "the legacy gate, as
    // measured".
    // -----------------------------------------------------------------------
    constexpr bool policyEnforcesDepthCeiling(TimeConfig::ResimTriggerPolicy policy)
    {
        return policy == TimeConfig::ResimTriggerPolicy::OnDisagreement;
    }

    // -----------------------------------------------------------------------
    // R1 — IS THIS ANCHOR SHALLOW ENOUGH TO RESIMULATE FROM?
    //
    // SKIPPED AND COUNTED, NOT CLAMPED: clamping a too-deep anchor up to
    // `frontier - maxDepthTicks` would restore at a slot NO correction ever
    // landed in, so the replay would reproduce a guaranteed no-op. Recovery
    // for that regime is a NEWER correction (raises the anchor) or the
    // HardResync failsafe.
    //
    // ⭐ THIS IS `rollbackWindowTicks`' FIRST REAL CLIENT-SIDE CONSUMER —
    // TimeConfig's ADR status note and its re-scoped deferral trigger
    // (condition 3) are rewritten in the same change as this one; read that
    // note first if you are about to add a second consumer.
    //
    // `maxDepthTicks == 0` means "no depth policy" and admits every anchor.
    // An anchor at or AHEAD of the frontier is admitted too: it cannot be
    // produced by a landing (a correction above the frontier has no slot and
    // is discarded before it reaches W1), and the gate's own
    // `anchor != frontier` clause already handles equality.
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
    // [item 47] THE REPLAY WRITE RULE — "AUTHORITY BEATS A RE-DERIVATION OF
    // IT" — and the fresh/stale classifier that measures how often it bites.
    //
    // ⛔ THE DEFECT THIS REPAIRS (the HOLLOW ANCHOR). A replay could overwrite
    // a slot a correction had just landed in (the game thread writes, an
    // in-flight physics-thread replay clobbers) while leaving
    // `m_containsCorrectTick` SET, so the slot claimed authority provenance
    // over a re-derivation it did not have.
    //
    // THE RULE IS PROTECT-ALL-CORRECTED, ONE BIT WIDE: a replay never
    // overwrites a slot with `m_containsCorrectTick` set — not "never
    // overwrites a FRESH one", never overwrites ANY of them. Even a "stale"
    // corrected slot holds SERVER TRUTH the replay does not supersede, because
    // the replay derives from the old PREDICTION at the shared restore tick,
    // not from newer authority. Consequence worth stating as an invariant:
    // THE PROVENANCE BIT CANNOT LIE — nothing authority-marked is ever
    // overwritten, so no bit is ever cleared, and "bit set ⇒ authority-grade"
    // holds BY CONSTRUCTION.
    //
    // ⚠ "AUTHORITY-GRADE" MEANS ADOPTED-AUTHORITY **OR** WITHIN-TOLERANCE-OF-
    // AUTHORITY. On an AGREEING landing `tryInsertingCorrectState` sets the
    // bit and SKIPS the state copy (`if (!predictionWasCorrect)`), so the slot
    // keeps the PREDICTED value — which the authority has certified as
    // matching. Protect-all preserves both readings; a rule that protected
    // only adopted state would need a second bit to tell them apart. (The
    // frontier slot is not an exception to any of this.)
    //
    // -----------------------------------------------------------------------
    // AND NOW THE PART THAT IS **CLASSIFICATION ONLY**: FRESH vs STALE.
    // -----------------------------------------------------------------------
    // ⛔ READ THIS BEFORE WIRING `classifyResimSlotWrite` INTO ANY DECISION.
    // The WRITE decision above depends on ONE input — `slotContainsCorrectTick`.
    // Everything else this function takes exists to split the protections into
    // two POPULATIONS for the probe (`freshClobbersAvoided` / `staleClobbersAvoided`).
    // Under protect-all the two produce the SAME action; only the counter
    // differs.
    //
    // A slot is FRESH iff **EITHER** clause holds, and NEITHER CLAUSE ALONE
    // SUFFICES:
    //
    //   CLAUSE 1 — LANDING SEQUENCE (`slotLandingSeqNr > preparedLandingSeqNr`).
    //     A per-cache monotonic counter stamped on each landing's slot,
    //     captured at prepare. Catches a MID-REPLAY landing exactly — "this
    //     slot was written after this resim was prepared".
    //
    //   CLAUSE 2 — TICK vs THE PER-CACHE CAPTURED ANCHOR
    //     (`slotTick >= capturedAnchorTick`). Catches a corrected slot at or
    //     above the tick THIS CACHE anchored — information the resim was
    //     supposed to act on. MISSES a mid-replay landing that sits BELOW the
    //     captured anchor, reachable because the restore tick is a MIN across
    //     characters and another character's deeper anchor can drag the span
    //     below this one's.
    //
    // ⚠⚠ DO NOT REPLACE THIS WITH THE NAIVE `containsCorrectTick && tick >
    // runningAnchor`. It is the shape a reasonable reader reaches for first,
    // and with a SINGLE GLOBAL (min-folded) anchor it misclassifies the
    // multi-character population wholesale — a newer-anchored character's
    // slots all sit above the global min, so everything reads "fresh" and the
    // counter stops discriminating. The anchor compared against here is the
    // PER-CACHE capture taken at `prepareResimAll`, NEVER the folded min.
    //
    // `capturedAnchorTick == 0` means this resim was prepared with NO anchor
    // pending (an engine-side rewind we did not ask for). Clause 2 is then
    // vacuously true and every corrected slot classifies FRESH — deliberate
    // over-protection in the safe direction.
    //
    // ⭐ `staleClobbersAvoided` MUST read 0 in any single-character session — a
    // free classifier-wiring check. A nonzero there means the classifier is
    // wired to the wrong anchor, not that a new population appeared.
    // =======================================================================

    // What a replay tick's write into one cache slot ACTUALLY did. Reported by
    // `StateCorrectionCache::tryInsertingResimulatedState` through a defaulted
    // out-pointer, so every pre-item-47 call site is byte-identical.
    //
    // ⭐ THE ACTION IS **DERIVED FROM** THIS VALUE — `outcome != Written` IS
    // the protection — RATHER THAN DECIDED BESIDE IT, and that is a
    // correctness property, not a style choice (a mutation run proved it).
    // One function, one truth: the counter cannot disagree with the action
    // because it IS the action.
    //
    // ⚠ THAT DOES NOT MAKE THE FRESH/STALE SPLIT LOAD-BEARING. Whether this
    // returns `Written` depends on `slotContainsCorrectTick` and NOTHING ELSE;
    // the four other parameters only choose BETWEEN the two protected classes.
    // So the observational argument on the landing stamps still holds exactly:
    // a torn or stale stamp can mislabel a counter, and can never mis-decide a
    // write.
    //
    // `Discarded` is produced by the CACHE (the tick had no slot in the
    // 60-slot window — item 42's `replayOverruns`), never by the classifier
    // below: with no slot there is nothing to classify.
    enum class ResimSlotWriteOutcome : std::uint8_t
    {
        Discarded = 0,
        Written,
        ProtectedFresh,
        ProtectedStale,
    };

    constexpr ResimSlotWriteOutcome classifyResimSlotWrite(bool          slotContainsCorrectTick,
                                                           std::uint32_t slotLandingSeqNr,
                                                           std::uint32_t preparedLandingSeqNr,
                                                           std::uint32_t slotTick,
                                                           std::uint32_t capturedAnchorTick)
    {
        if (!slotContainsCorrectTick)
            return ResimSlotWriteOutcome::Written;

        const bool freshBySequence = slotLandingSeqNr > preparedLandingSeqNr;
        const bool freshByTick     = slotTick >= capturedAnchorTick;

        return (freshBySequence || freshByTick)
            ? ResimSlotWriteOutcome::ProtectedFresh
            : ResimSlotWriteOutcome::ProtectedStale;
    }

    // ⛔ THERE IS DELIBERATELY NO SECOND `replayMayOverwriteSlot(bit)` PREDICATE.
    // One draft had it; a mutation run proved it could disagree with the
    // counter (see the ACTION note on `ResimSlotWriteOutcome` above). The
    // write site asks this function once and acts on `outcome != Written`. If
    // a future edit needs the write rule to stop being "the bit alone", the
    // provenance invariant above stops holding by construction, and item 47
    // amendment 2's fallback shape — clear `m_containsCorrectTick` on any
    // overwrite, and rule explicitly on the T4 capture-tick ref, which a
    // replayed state was NOT produced by — becomes mandatory rather than
    // optional.

} // namespace resimGate
