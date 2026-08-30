#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

// ---------------------------------------------------------------------------
// [og-netcode-v2-input-relay item 48] SlotStateProvenance — "WHERE DID THE STATE
// IN THIS CACHE SLOT COME FROM?", as a DIAGNOSTIC and nothing else.
//
// ⛔⛔ READ THIS WHOLE BLOCK BEFORE TOUCHING ANYTHING BELOW. This column
// DELIBERATELY OVERRIDES a written warning. `StateCorrectionCache`'s item 45
// retirement block (CorrectionCache.h, at the `m_isResimulated` gravestone) says
// that re-adding a per-slot provenance column would re-create the five-site write
// discipline whose SILENT failure was the original resim-gate defect — the one
// items 31/42/43/44/45 were spent finding and repairing. That warning is CORRECT
// and it still stands for what it describes. This column is allowed to exist
// anyway because three fences make the hazard it warns about un-shippable rather
// than merely discouraged. If you weaken any of the three, you have re-opened the
// defect and the warning applies to you again.
//
// -------------------------------------------------------------------------
// FENCE 1 — THIS IS NOT THE OLD `bool`, AND IT MUST NEVER TAKE THE OLD NAME.
// -------------------------------------------------------------------------
// `m_isResimulated` was a bool answering "did a replay write this slot?". That
// question has ONE bit of answer and, crucially, it is the question a TRIGGER
// asks. Every archived document in this initiative binds that name to trigger
// semantics, so a reader who finds it back will reasonably assume the gate reads
// it again. ⛔ DO NOT NAME THIS `m_isResimulated`, `isResimulated`,
// `needsResim*`, or anything else that reads like a gate. The name is half the
// fence.
//
// The question THIS column answers is different in kind: **what is the LINEAGE
// of the state currently sitting in this slot?** `Replayed` reproduces
// everything the old bit could say. The two values below that the old bit could
// NOT represent are the reason the column is worth having at all:
//
//   * `AuthorityAgreedKeptPrediction` — the SKIP-THE-COPY branch.
//     `tryInsertingCorrectState` sets `m_containsCorrectTick` and then only
//     copies the authority state `if (!predictionWasCorrect)`. On an AGREEING
//     landing the slot is therefore authority-GRADE while physically holding the
//     PREDICTED value. Item 47's header calls this out in prose ("authority-grade
//     means adopted-authority OR within-tolerance-of-authority"); this is the
//     first time the cache can say which of the two a given slot is.
//
//   * `ReplayedOverCorrection` — THE PROVENANCE LIE ITSELF, made expressible.
//     Pre-item-47 a replay overwrote a corrected slot unconditionally while
//     leaving the bit set, so the slot CLAIMED authority provenance over a
//     re-derivation. That state was real, it was shipped, and it stayed invisible
//     for months for exactly one reason: **nothing in the system could represent
//     it.** See the ⭐ block on the enumerator itself.
//
// -------------------------------------------------------------------------
// FENCE 2 — NO PRODUCTION READER, AND IT IS MACHINE-CHECKED.
// -------------------------------------------------------------------------
// The resim gate (`needsResimulation`, the pending-anchor lifecycle), the
// divergence fold (`checkDivergenceAll`), the T6 input-resolution ladder, state
// adoption and the correction verdict are all PROVABLY independent of this
// column. Not "by inspection" and not "by convention":
//
//   ⭐ `CorrectionCache.ResimGate.TheProvenanceColumnCannotReachAnyProductionOutput`
//      (og-simulation-tests, `[CorrectionCache][ResimGate][Provenance]`) runs a
//      full gate lifecycle twice, scribbling arbitrary garbage into the column at
//      three points in the second run, and asserts EVERY production output is
//      byte-identical between the two runs — gate, anchor, captures, verdicts,
//      write outcomes, adopted state, and the determinism checksum.
//
// THAT CASE **IS** THE FENCE. The original defect was production logic derived
// from per-slot bits; this makes that regression fail a test instead of merely
// contradicting a comment. If you are here to make the gate read provenance:
// you must argue against that case in your own design and delete it on purpose.
// Do not weaken it in passing.
//
// ⚠ WHAT "PRODUCTION OUTPUT" EXCLUDES, stated so the scope is not mistaken for a
// loophole. Fence 3's sanctioned readers do change when the column changes.
//
// They are (a) the LLTs, (b) the `[ResimProbe.SlotMap]` Verbose line and (c)
// og-brawler's display fold — THREE, not the two this note counted before (c)
// existed. That is what a diagnostic is, and none of the three feeds a decision;
// the no-decision claim is the one carrying the weight and it holds for all three
// unchanged.
//
// Their default-off arguments, however, are NOT the same argument. (a) and (b) do
// not exist at all at shipped verbosity. (c) does exist: it is compiled into a
// shipped build and gated by a default-`false` cvar rather than by a log verbosity,
// so "absent" is simply the wrong word for it. What bounds (c) is direction instead
// — it reads this column into a client-local display ring that is never replicated,
// never enters a correction payload and never reaches `compute_checksum`. Setting
// that cvar therefore changes pixels and nothing else.
//
// ⛔ (c) ALSO SITS OUTSIDE THE CASE ABOVE, which scribbles this column from inside
// og-simulation. What keeps (c) a read is stated at the fold; do not relax it there.
//
// ⛔ AND THIS COLUMN NEVER ENTERS `compute_checksum` OR THE DETERMINISM
// COMPARISON — same prohibition, same reasoning and same shape as the resim
// anchor's (design_task43 §3.1's non-site list): two peers replaying identical
// inputs from identical state must agree on the STATE, and they may legitimately
// disagree on how each of them arrived at it (one resimulated, one did not).
// Hashing lineage would make identical simulations hash differently.
//
// -------------------------------------------------------------------------
// FENCE 3 — T16 IS SATISFIED BY REAL READERS ON DAY ONE.
// -------------------------------------------------------------------------
// A stored value nothing reads is how the last stale-truth trap grew (T16's rule,
// and the reason `m_inputBuffer` and `m_isResimulated` were both retired rather
// than left standing). So this column ships WITH its readers:
//   (a) the scenario LLTs assert whole provenance MAPS directly, one case per
//       lifecycle scenario — every one of the five write sites is pinned, so a
//       wrong write fails a test instead of silently lying. That is precisely the
//       property the old bit never had;
//   (b) `[ResimProbe.SlotMap] id=… frontier=… map=…` — one line per character,
//       at most once per completed resim and once per wipe, at **Verbose** on the
//       existing `LogOGResimProbe` category (see `SimulationReconciliation::
//       getDiagnostics().logSlotProvenanceAll`, RN-7/task 56).
//   (c) og-brawler's `captureSummaryOf` display fold — the one EXTERNAL, downstream reader.
//
// ⚠ VERBOSE IS DELIBERATE AND IS NOT NEGOTIABLE DOWNWARD. This is per-slot,
// per-window data: 60 characters per character per resim. At Warning it would be
// T19's 10 MB defect with a different tag. `LogOGResimProbe` ships at Warning, so
// the line does not exist on a default run; `-LogCmds="LogOGResimProbe Verbose"`
// turns it on beside the existing `[ResimProbe.Request]` / `[ResimProbe.Stranded]`
// per-event lines, which is the same knob and the same family.
//
// -------------------------------------------------------------------------
// THREADING — the same pre-existing race, deliberately not a new one.
// -------------------------------------------------------------------------
// Written on the GAME thread (corrections) and on the PHYSICS thread (prediction,
// replay, wipe); read from either. The bytes are PLAIN, exactly like the item 47
// landing stamps beside them, and for the same argument: they RIDE the cache's
// long-standing unsynchronized `m_stateBuffer` GT/PT access (the finding's §1
// thread note, design §7.4's own future item) rather than adding a class of race.
// A diagnostic read tolerating a torn or ±1-stale lineage byte is the intended
// contract — one slot of one map line may be wrong. Nothing decides anything on
// it, which is fence 2, so there is no correctness consequence to be had.
//
// NAMESPACE NOTE: global namespace, matching the rest of the OGSim core.
// STL-ONLY, so the enum and its alphabet can be swept with no cache, no
// simulatable and no logger — the same property `ResimGatePolicy.h` and
// `ResimGateProbe.h` state for themselves.
// ---------------------------------------------------------------------------
enum class SlotStateProvenance : std::uint8_t
{
    // No state has been written for this slot's tick. The initial value in both
    // `StateCorrectionCache` constructors and the value `wipeCache` restores.
    // ⚠ It is NOT the same as "tick 0": both constructors fill the tick buffer
    // with 0, so an unwritten slot claims tick 0 (the tick-0 phantom, see
    // `getCacheIndex`). `Empty` is what actually distinguishes the two, and it is
    // the only place in this class where that distinction is stated in data.
    Empty = 0,

    // Client prediction wrote this. `pushPredictionTick` allocating a fresh
    // frontier slot, `save_snapshot` allocating one, and `advance_frame` via
    // both.
    Predicted,

    // A correction landed here and DISAGREED, so the authority's state was
    // adopted into the slot (`tryInsertingCorrectState`'s
    // `if (!predictionWasCorrect)` copy actually ran).
    AuthorityAdopted,

    // A correction landed here and AGREED, so the state copy was SKIPPED and the
    // slot still physically holds the PREDICTED value — which the authority has
    // explicitly certified as matching.
    //
    // ⭐ THIS IS ONE OF THE TWO VALUES THE RETIRED BOOL COULD NOT REPRESENT.
    // `m_containsCorrectTick` is set for both this and `AuthorityAdopted`, and
    // item 47's protect-all rule treats them identically ON PURPOSE ("a rule that
    // protected only adopted state would need a second bit to tell them apart").
    // The rule does not need the distinction; a human reading a slot map does.
    AuthorityAgreedKeptPrediction,

    // A resim replay wrote this slot (`tryInsertingResimulatedState` took the
    // write branch). Equivalent in meaning to the retired `m_isResimulated` bit
    // being set — everything the old bit could answer, this value answers.
    Replayed,

    // ⭐⭐ THE PROVENANCE LIE, MADE EXPRESSIBLE — AND UNREACHABLE ON THE SHIPPED
    // DEFAULT. Stamped when a replay write lands on a slot whose provenance is
    // already AUTHORITY-GRADE, i.e. the state claims a lineage it no longer has.
    //
    // ⛔ IT MUST NEVER APPEAR POST-ITEM-47, AND THAT IS THE POINT OF HAVING IT.
    // Item 47 ships PROTECT-ALL-CORRECTED: `tryInsertingResimulatedState` never
    // writes into a slot with `m_containsCorrectTick` set, and that bit is set at
    // exactly the two sites that stamp the two authority-grade values above. So
    // the write branch is reachable only with a clear bit, i.e. only over
    // non-authority provenance, i.e. this value cannot be produced. Item 47's
    // review §6 audited every writer of the bit and confirmed no replay path can
    // falsify it.
    //
    // ⇒ ZERO OCCURRENCES IS THE ASSERTION, NOT THE OMISSION. "The value is
    // unreachable, so leave it out" is precisely the reasoning that kept the
    // pre-47 defect invisible: the lie was real and shipped for months, and the
    // single reason nobody saw it is that no representation of it existed. A
    // column that cannot express the failure cannot report the regression. The
    // LLTs therefore assert this reads zero across every scenario AND separately
    // prove the alarm is wired by forging its precondition — see
    // `…ReplayedOverCorrectionIsUnreachableButTheAlarmIsWired`.
    //
    // ⚠ TWO RESIDUAL, PRE-EXISTING PATHS can still put non-authority state under
    // a set bit, and NEITHER is a replay, so neither produces this value (item 47
    // review §6, recorded here so a future reader does not re-derive them):
    // `save_snapshot` overwriting an EXISTING corrected slot (the 4-method
    // determinism-harness API only), and the GT/PT `m_stateBuffer` race at the
    // frontier. Both pre-date item 47 and both belong to design §7.4.
    ReplayedOverCorrection,
};

// THE MAP ALPHABET — one character per value, for the `[ResimProbe.SlotMap]`
// line. Defined ONCE, here, beside the enum, so the spellings an operator greps
// for cannot drift between the shipped emitter and a test. Same rule and same
// reason as `correctionLandingSiteName` in ResimGateProbe.h.
//
// The alphabet is `.PACRX` in enumerator order and the characters are chosen to
// be distinguishable at a glance in a 60-character run:
//   `.`  Empty                          — nothing here
//   `P`  Predicted
//   `A`  AuthorityAdopted               — authority state was copied in
//   `C`  AuthorityAgreedKeptPrediction  — authority Certified the prediction
//   `R`  Replayed
//   `X`  ReplayedOverCorrection         — ⛔ must never appear; see the enumerator
//
// `?` is returned for a value outside the enumeration, which is unreachable
// through the accessor but is what a torn cross-thread byte would look like.
inline char slotStateProvenanceChar(SlotStateProvenance provenance)
{
    switch (provenance)
    {
    case SlotStateProvenance::Empty:                         return '.';
    case SlotStateProvenance::Predicted:                     return 'P';
    case SlotStateProvenance::AuthorityAdopted:              return 'A';
    case SlotStateProvenance::AuthorityAgreedKeptPrediction: return 'C';
    case SlotStateProvenance::Replayed:                      return 'R';
    case SlotStateProvenance::ReplayedOverCorrection:        return 'X';
    }
    return '?';
}

// Long-form name, for an assertion message or a one-off diagnostic. Kept beside
// the alphabet so the two cannot describe different enumerations.
inline const char* slotStateProvenanceName(SlotStateProvenance provenance)
{
    switch (provenance)
    {
    case SlotStateProvenance::Empty:                         return "Empty";
    case SlotStateProvenance::Predicted:                     return "Predicted";
    case SlotStateProvenance::AuthorityAdopted:              return "AuthorityAdopted";
    case SlotStateProvenance::AuthorityAgreedKeptPrediction: return "AuthorityAgreedKeptPrediction";
    case SlotStateProvenance::Replayed:                      return "Replayed";
    case SlotStateProvenance::ReplayedOverCorrection:        return "ReplayedOverCorrection";
    }
    return "Unknown";
}

// "Does this slot claim authority lineage?" — TRUE for both authority-grade
// values, which is the same population `m_containsCorrectTick` marks.
//
// ⛔ IT IS NOT A SUBSTITUTE FOR THAT BIT AND MUST NEVER BE USED AS ONE. The
// replay write rule reads `m_containsCorrectTick` and nothing else
// (`resimGate::classifyResimSlotWrite`), and that one-bit property is the whole
// safety argument for item 47's plain cross-thread stamps. This predicate exists
// so the write site can ask a SECOND, INDEPENDENT question — "was the bit
// telling the truth?" — whose only consumer is the `ReplayedOverCorrection`
// diagnostic value. Two independent sources are what make the disagreement
// detectable; collapsing them would delete the alarm.
constexpr bool isAuthorityGradeProvenance(SlotStateProvenance provenance)
{
    return provenance == SlotStateProvenance::AuthorityAdopted
        || provenance == SlotStateProvenance::AuthorityAgreedKeptPrediction;
}

// The number of enumerators, for the alphabet sweep and for the fence test's
// garbage generator. Kept adjacent to the enum so adding a value without
// extending the alphabet fails a test rather than silently printing '?'.
inline constexpr std::uint8_t kSlotStateProvenanceCount = 6u;
