#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>

// `PredictedCharacterClass` is REUSED, NOT REDECLARED, and that is what this include buys.
// ⛔ NEVER DERIVE A SECOND NOTION OF "REMOTE" — two would let the halves of one summary describe different populations. §1
// ⚠ THE ONLY NON-STL INCLUDE, and transitively still `<cstdint>`-only: testability holds, "no other OGSimulation header" does not. §1
#include "OGSimulation/Network/CorrectionVerdictProbe.h"

// ===========================================================================
// ORIENTATION — TWO PROBES, TWO THREADS, AND HOW TO READ A NUMBER OFF A LOG LINE
//
// Read this first. Every fence below states one rule at the declaration it guards; none of
// them restates this map, and this map states no rule.
//
// WHAT THESE ARE. Two independent counter objects that make the resimulation gate countable
// at shipped verbosity. INSTRUMENTATION ONLY: every counter is written by a call whose only
// other effect is a log line, and no gate, clock, cache or integrator reads one. Deleting
// this header would change no simulated value.
//
//   object                  thread   window driven by   fed from
//   ----------------------  -------  -----------------  --------------------------------
//   ResimGateProbe          PHYSICS  CHECK COUNT        `SimulationManager` — the divergence
//     check, the resim prepare, the post-sim apply edge, a stuck prediction frame and a
//     replay tick; plus, across the adapter boundary but STILL on the physics thread, the
//     rewind-request and first-resim-step callbacks.
//   CorrectionLandingProbe  GAME     SAMPLE COUNT       `NetSyncTelemetry::
//     emitCorrectionArrival`, given the class and site `SimulationNetSync::
//     decideCorrectionArrival` computes on the correction callback that
//     `registerPredictionOwner` binds.
//
// TWO OBJECTS BECAUSE THERE ARE TWO THREADS, and that is a correctness property rather than a
// layout choice: one shared window would have a thread reset counters the other is
// mid-increment on, costing a whole window's totals instead of one sample. Its price is that
// the under-resimulation reading spans two adjacent Warning lines and cannot be one. §2
//
// NEITHER OBJECT LOGS. Each accumulates and hands back a summary struct; the caller owns the
// logger and does the SIMLOG. Same convention as `RelayReadProbe`, `CorrectionVerdictProbe`
// and `RemoteMoveQueue`, and it is what lets the low-level tests assert on numbers rather
// than on strings.
//
// WHAT A CLOSED WINDOW EMITS at the shipped `Warning` verbosity:
//   ResimGateProbe          `[ResimProbe.Gate]` `[ResimProbe.Chaos]` `[ResimProbe.Apply]`
//   CorrectionLandingProbe  `[ResimProbe.Landing]`, one per character class
// Five lines per window per category against a commissioned budget of six. Verbose adds
// `[ResimProbe.Request]`, `[ResimProbe.Stranded]` and a per-correction landing line.
//
// ⛔ BEFORE READING ANY NUMBER BELOW, ESTABLISH WHICH TRIGGER POLICY THE RUN USED. The
// compiled default and the shipped configuration DIFFER; every archived baseline quoted on
// this page was measured under the compiled default, and three counters here read differently
// under the other. Which one ships is stated once, with both halves anchored, at
// `TimeConfig::resimTriggerPolicy` — deliberately not re-derived here. For a specific run,
// grep `[ResimGate] session policy` in its log.
//
// HOW THE COUNTERS ARE GROUPED, and the labels are used as section keys throughout: `I1` the
// denominator, `I3` + `I4` the physics-rewind request / grant ledger, `I5` the apply edge,
// `I6` the replay span, `I2` where a correction landed relative to the frontier.
//
// WHERE THE REST LIVES, and it is two documents, not one:
//   `docs/History-ResimGate.md`         the lineage — what the gate was, why it was rebuilt,
//                                       and every retired name. Closed tense.
//   `docs/ResimGateProbe-rationale.md`  the derivations, the archived measurements and the
//                                       reachability arguments behind each counter.
//
// Global namespace, matching the rest of the OGSim core.
// ===========================================================================

// ONE WINDOW LENGTH FOR THE WHOLE PROBE FAMILY, ALIASED FROM THE EXISTING CONSTANT.
// ⛔ DO NOT RE-LITERAL IT — a second `= 120u` is a second source of truth that drifts, and it kills window-for-window comparison. §3
// ⚠ THE UNIT DIFFERS PER THREAD: 120 divergence checks (~2 s at 60 Hz) against 120 correction events. Same SIZE, never aligned. §3
inline constexpr std::uint32_t kResimGateProbeWindowSamples =
    kCorrectionVerdictProbeWindowSamples;

// ---------------------------------------------------------------------------
// I1 + I3 + I4 + I5 + I6 — the per-window summary handed to the caller.
// ⛔ PLAIN `std::uint32_t`, NO FLOATS AND NO ACCUMULATING AVERAGES: every derived figure is integer per-mille, so a test reproduces it exactly. §3
// ⛔ DO NOT ADD A PROVENANCE FIELD HERE — `StateCorrectionCache::m_stateProvenance` is a fenced diagnostic that feeds no counter
//   on this page and no decision anywhere; its independence is machine-checked. See `SlotStateProvenance.h`. §3
// ---------------------------------------------------------------------------
struct ResimGateWindowSummary
{
// --- I1: THE DENOMINATOR. -------------------------------------------------
// Physics frames on which `checkDivergenceAll` ran, split by outcome. `checks` always equals
// the window length that drives it, and is reported anyway for the same reason
// `CorrectionVerdictWindowSummary::samples` is: it makes the line self-describing.
// ⚠ ITS FAILURE MODE IS "NO `[ResimProbe.Gate]` LINE AT ALL", never `checks=0` — and a missing line is CORRECT on the authority. §4
    std::uint32_t checks = 0u;

// `checkDivergenceAll` returned 0 — no character reported `needsResimulation()`.
    std::uint32_t declined = 0u;

// `checkDivergenceAll` returned a nonzero anchor tick: the gate opened and a rewind was asked
// for. Always `checks - declined`.
    std::uint32_t requested = 0u;

// `requested / checks` in parts per thousand, rounded to nearest. §5
    std::uint32_t requestRatePerMille = 0u;

// --- THE SURVIVING-ANCHOR COUNT. ------------------------------------------
// Characters whose pending anchor SURVIVED the completion-edge CAS: a correction landed on the
// game thread mid-replay and raised the anchor past the value `prepareResimAll` captured, so
// the CAS failed, the gate stays OPEN and it re-requests next frame with a DIFFERENT anchor.
// That is the mechanism working: `SimulationReconciliation::consumeResimAnchorsAll` argues it.
// ⛔ FED FROM THE APPLY EDGE beside `noteFinish`, NOT from `noteCheck`'s call site like `checks`/`declined`/`requested`;
//   it still lands in this window because `noteCheck` alone drives the window. §6
// ⛔ ON THE GATE LINE BY EXPLICIT RULING, not on `[ResimProbe.Apply]`: a surviving anchor is a property of the GATE. §6
// ⚠ EXPECT NONZERO, POSSIBLY LARGE, UNDER THE SHIPPED CONFIGURATION — near-0 only under the compiled default. Check the policy first. §6
// ⚠ A NONZERO SHOULD SHOW IN `requests` WITHOUT a matching `repeatRequests`, because the next anchor differs from this one. §6
    std::uint32_t survivingAnchors = 0u;

// --- THE DEPTH-POLICY EXCLUSION COUNT. ------------------------------------
// Character-frames on which a character reported `needsResimulation()` but its pending anchor
// sat more than `TimeConfig::rollbackWindowTicks` below that character's own prediction
// frontier, so `checkDivergenceAll` excluded it from the min fold.
// ⛔ SKIP, NOT CLAMP, AND DELIBERATELY — clamping restores at an uncorrected slot and replays the identical prediction. §7
// ⚠ CHARACTER-FRAMES, NOT DISTINCT ANCHORS: a stranded anchor re-counts every frame, so a sustained nonzero reads "stuck out of reach". §7
// ⛔ LIVE ON EVERY RUN OF THIS PROJECT, NOT STRUCTURALLY 0 — the depth policy is consulted only under the disagreement
//   policy, which the compiled default does not select and the SHIPPED CONFIGURATION DOES. §7
// ⚠ THREE SPELLINGS OF ONE THING: `deepAnchorExclusions` here, `noteDeepAnchorSkips` at the setter, `deepAnchorSkips` at the call site. §7
    std::uint32_t deepAnchorExclusions = 0u;

// --- I3: THE PHYSICS-REWIND REQUEST / REFUSAL LEDGER. ---------------------
// ⭐ ONE ADAPTER'S BINDING, stated once here and inherited by I3, I4 and I5. Another adapter
// substitutes its own, and the engine-free Catch2 suite drives the same door with no physics
// engine at all (`ResimGatePolicyTest.cpp`). For one adapter — Unreal/Chaos — the request hook
// is `FSimulationManagerAsyncCallback::TriggerRewindIfNeeded_Internal`, the grant hook
// `::FirstPreResimStep_Internal`, the rewind-frame search `FRewindData::FindValidResimFrame`,
// the depth merge `FMath::Min`, and the refusal floor `BlockResimFrame`.
// ⇒ Those are ONE adapter's names. EVERYTHING BELOW NAMES THE ROLE.
// ⚠ The `…ChaosFrame` parameters and fields are named for that one adapter's fixed-step frame
// index: read "chaos frame" as PHYSICS FRAME throughout.
//
// ⛔ WHY WE COUNT THIS OURSELVES: one adapter compiles its refusal diagnostics out (`DEBUG_REWIND_DATA` / `DEBUG_NETWORK_PHYSICS`),
//   so a refused rewind is COMPLETELY SILENT and we just retry next frame. These four fields end that silence. §8
//
// Frames on which our rewind-request callback returned a physics frame, i.e. the request
// actually crossed into the physics engine.
// ⚠ `requests != requested` IS A WIRING ALARM: they differ only by the adapter's short-circuits, so on a predicting client they must agree. §8
    std::uint32_t requests = 0u;

// Requests for which the grant hook subsequently fired: the physics engine accepted and began
// a rewind.
    std::uint32_t grants = 0u;

// `requests - grants`. A DROP toward 0 after a future fix is that fix's proof; a RISE is an alarm. §8
// ⚠ VALID AS A REFUSAL COUNT because a refusal leaves `needsResimulation()` true, so the request repeats rather than being lost. §8
    std::uint32_t refusedFrames = 0u;

// `refusedFrames / requests` in parts per thousand.
// ⚠ ZERO WHEN `requests` IS ZERO — no observation, not a perfect record.
    std::uint32_t refusedRatePerMille = 0u;

// Requests whose anchor tick equals the PREVIOUS request's anchor tick — the refusal-RUN
// signature: the retry keeps hammering a frame at or below the physics engine's refusal floor
// until a newer correction moves the anchor past the block. §8
    std::uint32_t repeatRequests = 0u;

// --- I4: THE DOMAIN-CONVERSION PIN. ---------------------------------------
// Grants whose granted physics step differs from the frame we last requested. A mismatch means
// the grant was DEEPENED — an engine-side requester merged a deeper request in under the depth
// merge, or the rewind-frame search's own validation walked DOWN.
// ⛔ A SHALLOW CLAMP — A GRANT LATER THAN REQUESTED — IS STRUCTURALLY IMPOSSIBLE, by a property of the ROLES and not of any one
//   engine: the frame search walks DOWNWARD, the depth merge is a MIN, and the replay starts at the granted frame. §9
// ⚠ ONLY RUNG THREE IS CHECKABLE INSIDE AN ENGINE, and it was checked in ONE adapter's source. ANOTHER ADAPTER MUST RE-CHECK ALL THREE. §9
// ⚠ A CONSTANT 0 IS A VERIFIED ASSERTION, NOT AN UNEXERCISED COUNTER — no engine-side requester exists today; a nonzero is an upgrade alarm. §9
    std::uint32_t clampedGrants = 0u;

// Requested rewind DEPTH in physics frames (`lastCompletedStep - requestedChaosFrame`), min and
// max over the window's requests. Both read 0 when `requests` is 0. §9
// ⚠ A NEGATIVE DEPTH IS CLAMPED TO 0, NOT STORED — the ±1 tick↔frame skew produces it; it shows as `depthMin=0` beside a nonzero `refusedFrames`. §9
    std::uint32_t minRequestDepth = 0u;
    std::uint32_t maxRequestDepth = 0u;

// --- I5: THE APPLY-EDGE LEDGER. -------------------------------------------
// `SimulationManager::prepareResimulation` calls — one per granted rewind.
// ⚠ `prepares` AND `grants` MUST AGREE — counted on opposite sides of the adapter boundary on purpose, like `requests`/`requested`. §10
    std::uint32_t prepares = 0u;

// The `[Resim.Finish]` edge in `onPostGameSimulation`: the sub-step on which the clock's resim
// cursor caught up to the frontier and `applyResimAll` ran.
    std::uint32_t finishes = 0u;

// `prepares - finishes`: granted resims whose apply edge NEVER RAN. Healthy would be 0; the
// archived readings are not, and pinning that defect is the whole point of the counter. §10
// ⛔ DO NOT READ A SINGLE WINDOW'S `abandoned` — a prepare whose finish falls in the next window is charged here and credits that
//   one, noise of at most 1 against a signal of the same order. Read several, or `stuckResimFrames`, which has no boundary term. §10
    std::uint32_t abandoned = 0u;

// Normal prediction frames entered while `ClientPredictionClock::isResimulating()` is STILL
// TRUE — the stranded-cursor state, and the boundary-noise-free reading of `abandoned`.
// Healthy: 0.
// ⚠ ITS SIDE EFFECT IS REAL, not bookkeeping: `SimulationManager::currentStep()` then returns the stale resim step, so
//   `sendLocalInputToAuthorityAll` stamps a stale tick until the next `startResimulation`. §10
    std::uint32_t stuckResimFrames = 0u;

// --- I6: REPLAY-SPAN ACCOUNTING. ------------------------------------------
// Resim replay ticks executed (`onGameSimulationResimulation` calls). Replays are SHORT, and
// this counter locks that answer in permanently. §11
// ⚠ IF THIS EVER READS `finishes * 12-20` the rollback window is genuinely being spanned and the coalescing explanation is back. §11
    std::uint32_t replayTicks = 0u;

// `tryInsertingResimulatedState` discards — a replayed tick whose slot is no longer in the
// cache window. Already visible at Warning in the cache's own line; counted here so it has a
// denominator. A nonzero window reading is the over-replay / window-skew evidence. §11
// ⚠ ITS POPULATION IS UNCHANGED by the correction-protection fix — a PROTECTED slot is not a discard, so archived readings still bind. §11
    std::uint32_t replayOverruns = 0u;

// --- THE HOLLOW-ANCHOR LEDGER. --------------------------------------------
// Replay ticks whose slot carried a correction and was therefore NOT overwritten
// (`resimGate::classifyResimSlotWrite`), split into the two populations that discriminator
// exists to separate. Character-slot events, like `replayOverruns`: one replay tick can protect
// one slot per character.
// ⭐ `freshClobbersAvoided` IS THE LIVE DEFECT RATE — each one is a replay tick that would
// otherwise have overwritten authority state the resim was supposed to act on: the hollow
// trigger, counted. §12
    // supposed to act on — the hollow trigger, counted. Its PRE-FIX twin was
    // never shipped: the fix and its instrument land together here because the
    // pre-fix counter would have had to be added, measured and removed inside one
    // regression window, and this initiative has already lost four instruments
    // that way. What makes the number honest instead is the LLT pair recorded in
    // the impl note: for each case, the input where the mechanism protects AND
    // the input where it does not.
// ⛔ EXPECT A RATE, NOT AN EVENT, ON EVERY RUN OF THIS PROJECT — near-0 only under the compiled default's frontier-exact anchor
//   and ~1-2 tick span; the SHIPPED CONFIGURATION selects the disagreement policy, which is what makes it a rate. §12
    std::uint32_t freshClobbersAvoided = 0u;

// ⭐ A 2+-CHARACTER-ONLY SIGNAL, AND THEREFORE A FREE CLASSIFIER-WIRING CHECK: it MUST read 0 in
// any single-character session, because for one character the replay span and the stale
// condition are DISJOINT. A nonzero reading there means the classifier is comparing against the
// folded min instead of the per-cache capture — not that a new population appeared. Full
// reachability argument at `resimGate::classifyResimSlotWrite`. §12
    std::uint32_t staleClobbersAvoided = 0u;
};

// One stranded-resim episode, handed back so the caller can emit ONE Verbose line per episode.
// ⭐ THE THREE NUMBERS TOGETHER DISCRIMINATE THE TWO SURVIVING CANDIDATE MECHANISMS: a
// `replayedTicks` one short of `catchUpDeficit` is the ±1 tick↔physics-frame skew; a larger
// shortfall points at a game-thread correction landing mid-replay. §13
// ⚠ A THIRD CANDIDATE WAS RULED OUT FROM ONE ADAPTER'S ENGINE SOURCE ONLY — another must re-check it; no instrument was invalidated. §13
struct StrandedResimEpisode
{
    // The simulation tick `prepareResimulation` restored to.
    std::uint32_t anchorTick = 0u;
// The prediction frontier at the moment the stranded frame was observed.
    std::uint32_t predictionTick = 0u;
    // Replay ticks actually executed since that prepare.
    std::uint32_t replayedTicks = 0u;
// `predictionTick - anchorTick`, i.e. how many the clock needed to catch up. Saturates at 0
// rather than wrapping if the frontier is somehow behind.
    std::uint32_t catchUpDeficit = 0u;
};

// ---------------------------------------------------------------------------
// ResimGateProbe — PHYSICS THREAD ONLY.
// ⚠ ORDERING NOTE, THE ONLY SURPRISING THING HERE: the window closes inside `noteCheck`, at the TOP of the frame, so that frame's
//   own request / grant / prepare / finish land in the NEXT window. A SHIFT, not a drop — every event counted exactly once. §14
// ---------------------------------------------------------------------------
class ResimGateProbe
{
public:
    explicit ResimGateProbe(std::uint32_t windowSamples = kResimGateProbeWindowSamples)
        : m_windowSamples(windowSamples == 0u ? kResimGateProbeWindowSamples : windowSamples)
    {
    }

// --- I1 -----------------------------------------------------------------
// Record one divergence check and its outcome. Returns true — filling `outSummary` and
// resetting the window — when this check completed a window.
    bool noteCheck(bool requestedResim, ResimGateWindowSummary& outSummary)
    {
        ++m_checks;
        if (requestedResim)
            ++m_requested;
        else
            ++m_declined;

        if (m_checks < m_windowSamples)
            return false;

        fillSummary(outSummary);
        resetWindow();
        return true;
    }

// One completed resim's surviving-anchor count.
// ⚠ TAKES A COUNT, not a per-character call — `consumeResimAnchorsAll` sweeps every character and hands back one total. §6
    void noteSurvivingAnchors(std::uint32_t count) { m_survivingAnchors += count; }

// The depth-policy exclusion count for THIS frame. Takes a count for the same reason
// `noteReplayOverruns` does, and is called unconditionally — with 0 on a normal frame — from
// the same site that reports the check outcome, so the two can never describe different frames.
// ⛔ CALL IT BEFORE `noteCheck`, NOT AFTER: this count comes from the SAME `checkDivergenceAll` call that produces
//   `declined`/`requested`, and `noteCheck` flushes the window it records those into. §7
    void noteDeepAnchorSkips(std::uint32_t count) { m_deepAnchorExclusions += count; }

// --- I3 + I4 ------------------------------------------------------------
// One rewind request that crossed into the engine.
// ⛔ TWO DOMAINS, TWO PARAMETERS: `lastCompletedStep`/`requestedChaosFrame` are PHYSICS frames, `anchorTick` is a SIMULATION tick. §9
    void noteRequest(std::uint32_t anchorTick,
                     std::int32_t  lastCompletedStep,
                     std::int32_t  requestedChaosFrame)
    {
        ++m_requests;

        if (m_hasPreviousRequest && m_previousRequestAnchorTick == anchorTick)
            ++m_repeatRequests;
        m_hasPreviousRequest       = true;
        m_previousRequestAnchorTick = anchorTick;

        m_lastRequestedChaosFrame    = requestedChaosFrame;
        m_hasLastRequestedChaosFrame = true;

        const std::int32_t rawDepth = lastCompletedStep - requestedChaosFrame;
        const std::uint32_t depth =
            rawDepth <= 0 ? 0u : static_cast<std::uint32_t>(rawDepth);
        if (m_requestDepthSamples == 0u)
        {
            m_minRequestDepth = depth;
            m_maxRequestDepth = depth;
        }
        else
        {
            if (depth < m_minRequestDepth) m_minRequestDepth = depth;
            if (depth > m_maxRequestDepth) m_maxRequestDepth = depth;
        }
        ++m_requestDepthSamples;
    }

// One granted rewind, at the physics frame the engine actually started it from.
// ⚠ A GRANT WITH NO REQUEST ON RECORD IS NOT A CLAMP — it is an engine-side requester rewinding without us, so it counts as a
//   grant and is left OUT of `clampedGrants` rather than charged against a request that never happened. §9
    void noteGrant(std::int32_t grantedChaosFrame)
    {
        ++m_grants;
        if (m_hasLastRequestedChaosFrame && grantedChaosFrame != m_lastRequestedChaosFrame)
            ++m_clampedGrants;
        m_hasLastRequestedChaosFrame = false;
    }

// --- I5 -----------------------------------------------------------------
// `prepareResimulation` ran: the clock started a resim at `anchorTick` and the cache restored
// into live state. Opens an EPISODE, closed by a finish or reported by a stranded frame.
    void notePrepare(std::uint32_t anchorTick)
    {
        ++m_prepares;
        m_episodeOpen           = true;
        m_episodeReported       = false;
        m_episodeAnchorTick     = anchorTick;
        m_episodeReplayedTicks  = 0u;
    }

// The `[Resim.Finish]` apply edge. Closes the episode.
    void noteFinish()
    {
        ++m_finishes;
        m_episodeOpen     = false;
        m_episodeReported = false;
    }

// A normal prediction frame entered while the clock still believes it is resimulating. Always
// counted.
// ⛔ RETURNS TRUE ONLY ON AN EPISODE'S FIRST SUCH FRAME, so the caller's line is one-shot: the state persists for many frames
//   by construction, and a per-frame line here is precisely the log-volume defect. §14
    bool noteStuckResimFrame(std::uint32_t predictionTick, StrandedResimEpisode& outEpisode)
    {
        ++m_stuckResimFrames;

        if (!m_episodeOpen || m_episodeReported)
            return false;

        m_episodeReported = true;
        outEpisode.anchorTick     = m_episodeAnchorTick;
        outEpisode.predictionTick = predictionTick;
        outEpisode.replayedTicks  = m_episodeReplayedTicks;
        outEpisode.catchUpDeficit =
            predictionTick > m_episodeAnchorTick ? predictionTick - m_episodeAnchorTick : 0u;
        return true;
    }

// --- I6 -----------------------------------------------------------------
// One resim replay tick executed.
    void noteReplayTick()
    {
        ++m_replayTicks;
        if (m_episodeOpen)
            ++m_episodeReplayedTicks;
    }

// `count` slots that a replay tick could not be written into. Takes a count because
// `postResimulationAll` sweeps every character and can discard more than one per replay tick.
    void noteReplayOverruns(std::uint32_t count) { m_replayOverruns += count; }

// `fresh` + `stale` slots that the replay was refused permission to overwrite on THIS replay
// tick. Same sweep and same call site as `noteReplayOverruns`, for the same reason.
// ⛔ BOTH POPULATIONS IN ONE CALL, so a caller cannot report one and forget the other: fresh is the defect rate, stale is the
//   hygiene rate, and the SPLIT is the instrument. §12
    void noteCorrectionProtections(std::uint32_t fresh, std::uint32_t stale)
    {
        m_freshClobbersAvoided += fresh;
        m_staleClobbersAvoided += stale;
    }

// --- introspection; tests and diagnostics only -------------------------
    std::uint32_t checkCount()   const { return m_checks; }
    std::uint32_t windowSamples() const { return m_windowSamples; }

// Snapshot of the window IN PROGRESS, without closing or resetting it. Same contract as
// `CorrectionVerdictProbe::fillSummary`.
// ⚠ NO SHIPPED CALLER — it exists so a test can read a PARTIAL window, the only way to observe a rate in a session whose
//   windows never complete. The suite reaches it through `getDiagnostics().resimGateProbe()`. §15
    void fillSummary(ResimGateWindowSummary& out) const
    {
        out.checks              = m_checks;
        out.declined            = m_declined;
        out.requested           = m_requested;
        out.requestRatePerMille = perMille(m_requested, m_checks);
        out.survivingAnchors    = m_survivingAnchors;
        out.deepAnchorExclusions = m_deepAnchorExclusions;

        out.requests            = m_requests;
        out.grants              = m_grants;
        out.refusedFrames       = m_requests > m_grants ? m_requests - m_grants : 0u;
        out.refusedRatePerMille = perMille(out.refusedFrames, m_requests);
        out.repeatRequests      = m_repeatRequests;

        out.clampedGrants       = m_clampedGrants;
        out.minRequestDepth     = m_requestDepthSamples == 0u ? 0u : m_minRequestDepth;
        out.maxRequestDepth     = m_requestDepthSamples == 0u ? 0u : m_maxRequestDepth;

        out.prepares            = m_prepares;
        out.finishes            = m_finishes;
        out.abandoned           = m_prepares > m_finishes ? m_prepares - m_finishes : 0u;
        out.stuckResimFrames    = m_stuckResimFrames;

        out.replayTicks         = m_replayTicks;
        out.replayOverruns      = m_replayOverruns;

        out.freshClobbersAvoided = m_freshClobbersAvoided;
        out.staleClobbersAvoided = m_staleClobbersAvoided;
    }

private:
    static std::uint32_t perMille(std::uint32_t numerator, std::uint32_t denominator)
    {
        if (denominator == 0u)
            return 0u;
        return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(numerator) * 1000u + denominator / 2u) / denominator);
    }

    void resetWindow()
    {
        m_checks = m_declined = m_requested = 0u;
        m_survivingAnchors = 0u;
        m_deepAnchorExclusions = 0u;
        m_requests = m_grants = m_repeatRequests = 0u;
        m_clampedGrants = 0u;
        m_minRequestDepth = m_maxRequestDepth = 0u;
        m_requestDepthSamples = 0u;
        m_prepares = m_finishes = m_stuckResimFrames = 0u;
        m_replayTicks = m_replayOverruns = 0u;
// The protection counters ARE per-window, like every other event count here: each is a
// completed observation of one replay tick, with no straddling sequence to preserve.
        m_freshClobbersAvoided = m_staleClobbersAvoided = 0u;
// ⛔ DELIBERATELY NOT RESET: the previous-request pair, the last-requested-frame pair and the whole episode block — all four
//   straddle the boundary, so clearing them would silently under-report `repeatRequests` and `clampedGrants` once per window. §14
    }

    // I1
    std::uint32_t m_checks    = 0u;
    std::uint32_t m_declined  = 0u;
    std::uint32_t m_requested = 0u;

// Surviving-anchor count this window; see `ResimGateWindowSummary::survivingAnchors`. Fed from
// the apply edge, not from `noteCheck`.
    std::uint32_t m_survivingAnchors = 0u;

// Depth-policy exclusions this window; see `deepAnchorExclusions`.
    std::uint32_t m_deepAnchorExclusions = 0u;

    // I3
    std::uint32_t m_requests       = 0u;
    std::uint32_t m_grants         = 0u;
    std::uint32_t m_repeatRequests = 0u;
    bool          m_hasPreviousRequest        = false;
    std::uint32_t m_previousRequestAnchorTick = 0u;

    // I4
    std::uint32_t m_clampedGrants       = 0u;
    std::uint32_t m_minRequestDepth     = 0u;
    std::uint32_t m_maxRequestDepth     = 0u;
    std::uint32_t m_requestDepthSamples = 0u;
    bool          m_hasLastRequestedChaosFrame = false;
    std::int32_t  m_lastRequestedChaosFrame    = 0;

    // I5
    std::uint32_t m_prepares         = 0u;
    std::uint32_t m_finishes         = 0u;
    std::uint32_t m_stuckResimFrames = 0u;
    bool          m_episodeOpen          = false;
    bool          m_episodeReported      = false;
    std::uint32_t m_episodeAnchorTick    = 0u;
    std::uint32_t m_episodeReplayedTicks = 0u;

    // I6
    std::uint32_t m_replayTicks    = 0u;
    std::uint32_t m_replayOverruns = 0u;

// The hollow-anchor ledger; the summary fields carry what each population means and why
// `stale` is a wiring check rather than a tuning signal.
    std::uint32_t m_freshClobbersAvoided = 0u;
    std::uint32_t m_staleClobbersAvoided = 0u;

    std::uint32_t m_windowSamples;
};

// ---------------------------------------------------------------------------
// I2 — WHERE A CORRECTION LANDED RELATIVE TO THE PREDICTION FRONTIER.
//
// THE FIRST-GATE DISCRIMINATOR, and the reason it is three buckets and not two:
//
//   Behind      `tick < predictionTick`. The correction set `m_containsCorrectTick` on its own
//               slot and adopted authority state INTO THAT SLOT. Whether that also opens the
//               gate is the TRIGGER POLICY's decision, never this bucket's.
//   AtFrontier  `tick == predictionTick`. LITERALLY the same predicate as
//               `resimGate::shouldSetPendingAnchor`'s frontier-exact arm — deliberately, so
//               that these counts are the baseline that policy has to reproduce.
//   Discarded   the tick had no slot at all: above the newest, or older than the cache window.
//               No comparison happened, no flag moved and no anchor is set, which is also why
//               an anchor can never be AHEAD of the frontier.
//
// ⛔ THIS CLASSIFIES A POSITION, NOT A TRIGGER — which is why the edge-triggered rewrite left it unchanged. §16
// ⛔ SO DO NOT READ A `requested`/`atFrontier` RATIO WITHOUT ESTABLISHING THE SESSION POLICY: under the compiled default a
//   behind-frontier landing triggers nothing; under the SHIPPED configuration a disagreeing one makes this the trigger population. §16
//
// ⭐ WHAT THE PAIR PROVES, so nobody has to re-derive it: `landedBehind` large while triggers
// track only `landedAtFrontier` IS the demonstrated under-resimulation statement — the system
// resimulates on a clock-alignment artifact instead of on disagreement. §16
// ⚠ WHETHER IT MATTERS IS DELIBERATELY NOT ANSWERABLE FROM HERE — `isSimilarTo` is a boolean fold that discards the distance. §16
//
// PER CLASS, NEVER POOLED. The class test is PROVIDER-PRESENCE, read LIVE through the
// input-resolution peer's `isLocallyControlled` rather than captured at bind.
// ⛔ NEVER A SECOND NOTION OF "REMOTE" — the same lookup `registerPredictionOwner` and `collectInputAll` fork on; a second one
//   would let the two halves of this summary describe different populations while looking authoritative. §17
// ⚠ NO PER-ID STATE, HENCE NO `forgetOwner` — a per-CLASS aggregate leaves nothing to erase. Do not copy the relay probes'
//   per-id watermark teardown looking for symmetry; `CorrectionVerdictProbe` states the same rule. §17
// ---------------------------------------------------------------------------
enum class CorrectionLandingSite : std::uint8_t
{
    Behind = 0,
    AtFrontier,
    Discarded,
};

// The classification, as a free function so the ONE definition of "at the frontier" is shared by
// the shipped call site and every test, and is testable with no cache, owner or simulatable.
// ⛔ `landed == false` WINS OUTRIGHT — a discarded correction has no slot, so comparing its tick would classify a non-event. §18
inline CorrectionLandingSite classifyCorrectionLanding(bool          landed,
                                                       std::uint32_t correctionTick,
                                                       std::uint32_t predictionTick)
{
    if (!landed)
        return CorrectionLandingSite::Discarded;
    return correctionTick == predictionTick
        ? CorrectionLandingSite::AtFrontier
        : CorrectionLandingSite::Behind;
}

struct CorrectionLandingClassSummary
{
// Landed strictly behind the frontier — the suppressed-trigger count under the compiled
// default. See the `Behind` bucket in the I2 block.
    std::uint32_t landedBehind = 0u;
// Landed exactly ON the frontier — the only in-play gate opener under the compiled default.
    std::uint32_t landedAtFrontier = 0u;
// No slot; no comparison; no flag moved.
    std::uint32_t discarded = 0u;

// `landedAtFrontier / total`, in parts per thousand — THE FRONTIER-TOUCH RATE. Under the
// compiled default this and `[ResimProbe.Gate]`'s `requestRatePerMille` are the two halves of
// the finding's central claim and should move together. §16
// ⚠ ZERO WHEN THE CLASS SAW NOTHING — no observation, not a perfect record; the caller SKIPS an empty class rather than print zeros. §16
    std::uint32_t atFrontierRatePerMille = 0u;

    std::uint32_t total() const { return landedBehind + landedAtFrontier + discarded; }
};

struct CorrectionLandingWindowSummary
{
// Events across BOTH classes — the window's own size. Always `local.total() + remote.total()`.
    std::uint32_t samples = 0u;

    CorrectionLandingClassSummary local;
    CorrectionLandingClassSummary remote;
};

// ---------------------------------------------------------------------------
// CorrectionLandingProbe — GAME THREAD ONLY.
// ---------------------------------------------------------------------------
class CorrectionLandingProbe
{
public:
    explicit CorrectionLandingProbe(
        std::uint32_t windowSamples = kResimGateProbeWindowSamples)
        : m_windowSamples(windowSamples == 0u ? kResimGateProbeWindowSamples : windowSamples)
    {
    }

// Record one correction's landing site. Returns true — filling `outSummary` and resetting the
// window — when this sample completed a window.
// ⛔ DISCARDS ARE SAMPLES HERE, the one deliberate difference from `CorrectionVerdictProbe`: the discard IS an observation of
//   the stream's alignment, so excluding them would hide the very class this instrument exists to see. §19
    bool noteLanding(PredictedCharacterClass characterClass,
                     CorrectionLandingSite   site,
                     CorrectionLandingWindowSummary& outSummary)
    {
        ClassCounters& counters = countersFor(characterClass);
        switch (site)
        {
        case CorrectionLandingSite::Behind:     ++counters.landedBehind;     break;
        case CorrectionLandingSite::AtFrontier: ++counters.landedAtFrontier; break;
        case CorrectionLandingSite::Discarded:  ++counters.discarded;        break;
        }

        ++m_samples;
        if (m_samples < m_windowSamples)
            return false;

        fillSummary(outSummary);
        resetWindow();
        return true;
    }

// --- introspection; tests and diagnostics only -------------------------
    std::uint32_t sampleCount()   const { return m_samples; }
    std::uint32_t windowSamples() const { return m_windowSamples; }

    std::uint32_t countFor(PredictedCharacterClass characterClass,
                           CorrectionLandingSite   site) const
    {
        const ClassCounters& counters = countersFor(characterClass);
        switch (site)
        {
        case CorrectionLandingSite::Behind:     return counters.landedBehind;
        case CorrectionLandingSite::AtFrontier: return counters.landedAtFrontier;
        case CorrectionLandingSite::Discarded:  return counters.discarded;
        }
        return 0u;
    }

// Snapshot of the window IN PROGRESS; does not close or reset it.
    void fillSummary(CorrectionLandingWindowSummary& out) const
    {
        out.samples = m_samples;
        fillClassSummary(m_local,  out.local);
        fillClassSummary(m_remote, out.remote);
    }

private:
    struct ClassCounters
    {
        std::uint32_t landedBehind     = 0u;
        std::uint32_t landedAtFrontier = 0u;
        std::uint32_t discarded        = 0u;
    };

    static void fillClassSummary(const ClassCounters& counters,
                                 CorrectionLandingClassSummary& out)
    {
        out.landedBehind     = counters.landedBehind;
        out.landedAtFrontier = counters.landedAtFrontier;
        out.discarded        = counters.discarded;

        const std::uint32_t total = out.total();
        out.atFrontierRatePerMille =
            (total == 0u)
                ? 0u
                : static_cast<std::uint32_t>(
                      (static_cast<std::uint64_t>(out.landedAtFrontier) * 1000u + total / 2u)
                      / total);
    }

    ClassCounters& countersFor(PredictedCharacterClass characterClass)
    {
        return (characterClass == PredictedCharacterClass::RemoteProxy) ? m_remote : m_local;
    }

    const ClassCounters& countersFor(PredictedCharacterClass characterClass) const
    {
        return (characterClass == PredictedCharacterClass::RemoteProxy) ? m_remote : m_local;
    }

    void resetWindow()
    {
        m_local   = ClassCounters{};
        m_remote  = ClassCounters{};
        m_samples = 0u;
    }

    ClassCounters m_local;
    ClassCounters m_remote;

    std::uint32_t m_samples = 0u;
    std::uint32_t m_windowSamples;
};

// Human-readable landing-site name for a log line.
// ⛔ DEFINED ONCE, HERE, so the spellings an operator greps for cannot drift between the shipped emitter and a test. §19
inline const char* correctionLandingSiteName(CorrectionLandingSite site)
{
    switch (site)
    {
    case CorrectionLandingSite::Behind:     return "Behind";
    case CorrectionLandingSite::AtFrontier: return "AtFrontier";
    case CorrectionLandingSite::Discarded:  return "Discarded";
    }
    return "Unknown";
}
