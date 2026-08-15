#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include <concepts>
#include <limits>
#include <tuple>
#include <unordered_map>

#include "OGSimulation/CorrectionCache.h"
#include "OGSimulation/SimulationLog.h"
#include "OGSimulation/SimulationObjectStorage.h"
#include "OGSimulation/SimulationTimeContext.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
#pragma optimize("", off)

// ---------------------------------------------------------------------------
// [og-netcode-v2-input-relay T6 / design D3] AppliedCaptureRef — the answer to
// "which capture tick did the authority apply at tick T for this character".
//
// This is the ONE piece of reconciliation-owned data the relocated
// SimulationNetSync::collectResimInputAll consumes (T6 placement ruling, Option
// C). It deliberately exposes NO cache and NO store: reconciliation answers the
// D3 question and knows nothing about how netsync then resolves an input from it.
//
// FOUR KINDS, THREE OUTCOMES. The ruling names three — ref / sentinel / absent —
// and `Absent` is split in two here because the two halves must produce different
// BEHAVIOUR at the call site while meaning the same thing to this class:
//
//   NoSlot   this tick is outside the character's cache window, so the character
//            is not part of this resim at all. Resim's state restore
//            (prepareResimAll) skips such a character for the same reason, so
//            handing the integrator an input for it would integrate an
//            un-restored state. The caller must emit NO input — which is exactly
//            what the pre-T6 body did by only emplacing on a cache hit.
//   NoRef    the slot exists but no correction has landed in it. Covers the
//            prediction FRONTIER (ticks newer than the last correction) and
//            correction HOLES (an intermediate tick whose correction was never
//            replicated — routine, since the net update rate is below the sim
//            rate). Both re-derive; neither is a sentinel.
//   Sentinel a correction landed and named NO capture: the authority substituted
//            an input (RemoteMoveQueue underrun, D1). Resolves to the injected
//            game zero — the value T17 made the authority actually integrate.
//   Ref      a correction landed carrying a real capture tick. `captureTick` is
//            that tick, and it WINS over any relay-entry schedule stamp
//            (RelayDelaySpectrumDesign.md §5.3 — see the precedence note at the
//            resolution site).
// ---------------------------------------------------------------------------

enum class AppliedCaptureRefKind : uint8
{
    NoSlot,
    NoRef,
    Sentinel,
    Ref,
};

struct AppliedCaptureRef
{
    AppliedCaptureRefKind kind        = AppliedCaptureRefKind::NoSlot;
    // Meaningful only when `kind == Ref`; the sentinel otherwise.
    uint32                captureTick = kNoInputCaptureTick;
};

// ---------------------------------------------------------------------------
// SimulationReconciliation<SimulatableTs...>
//
// Owns per-simulatable StateCorrectionCache instances and every operation that
// reads or writes those caches. This is the full client-side prediction /
// server reconciliation loop: predict forward, accept authoritative corrections,
// detect divergence, serve resim replay, wipe on resync.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
// ---------------------------------------------------------------------------

template <typename... SimulatableTs>
class SimulationReconciliation
{
public:
    explicit SimulationReconciliation(
        SimulationObjectStorage<SimulatableTs...>& storage,
        std::function<void(const char*)> logger = nullptr)
        : m_storage(storage)
        , m_logger(std::move(logger))
    {}

    void setLogger(std::function<void(const char*)> logger)
    {
        m_logger = std::move(logger);
    }

    // -----------------------------------------------------------------------
    // Lifecycle — called by registration facade
    // -----------------------------------------------------------------------

    // [item 45] `try_emplace`, NOT `emplace`, AND THAT IS A REQUIREMENT NOW.
    // `emplace(id, Cache(m_logger))` built a temporary and MOVED it into the node;
    // `StateCorrectionCache` is non-movable since it acquired the atomic resim
    // anchor (a duplicated cache would give two objects one gate). `try_emplace`
    // forwards the ctor arguments and constructs in place, so no copy or move
    // exists to delete. `std::unordered_map` is node-based, so rehashing relinks
    // nodes and never moves the value either.
    //
    // THE POLICY STAMP IS WHY THIS CLASS REMEMBERS THE POLICY AT ALL: characters
    // register mid-session, long after the composition root pushed the configured
    // value, so a cache created here must be born with it. See
    // `setResimTriggerPolicy`.
    template <typename T>
    void createCacheFor(unsigned int id)
    {
        auto result = std::get<CacheMapFor<T>>(m_caches).try_emplace(id, m_logger);
        result.first->second.setResimTriggerPolicy(m_resimTriggerPolicy);
    }

    template <typename T>
    void removeCacheFor(unsigned int id)
    {
        std::get<CacheMapFor<T>>(m_caches).erase(id);
    }

    // -----------------------------------------------------------------------
    // Prediction push — called by SimulationNetSync::collectInputAll per tick
    // -----------------------------------------------------------------------

    template <typename T>
    void pushPredictionTick(unsigned int id, uint32 tick)
    {
        getCacheFor<T>(id).pushPredictionTick(tick);
    }

    // [og-netcode-v2-input-relay T16] `pushPredictionInput` — the wrapper around
    // StateCorrectionCache's input-column writer — IS GONE with the column. Its
    // two production callers were both arms of SimulationNetSync::collectInputAll;
    // both now push only the prediction TICK, which is what allocates the slot.
    // A cache slot is state + the applied-capture-tick ref; it holds no input
    // value, and nothing in this class can put one there.

    // Called on StepKind::Skip — back-fills the skipped tick with the prior state.
    // skippedTick is step.getTick() - 1 (the tick that was jumped over).
    // Must push the tick before the state: pushPredictionState writes into the slot
    // for the current prediction tick, so the tick advance must happen first.
    //
    // [T16] The `pushPredictionInput(InputType{})` that used to sit between them is
    // gone with the column. Note it was a VALUE-INITIALISED input, not the injected
    // game zero — the (0,0,0)-forward poison T17 hunted elsewhere. Removing the
    // column removes that write entirely rather than having to fix it, since the
    // backfilled slot exists to be re-derivable, not to carry a remembered input.
    template <typename T>
    void backfillSkippedTick(unsigned int id, uint32 skippedTick, const typename T::StateType& priorState)
    {
        SIMLOG(m_logger, "[TimeResync.BackfillSkipped] id=%u skippedTick=%u", id, skippedTick);
        auto& cache = getCacheFor<T>(id);
        cache.pushPredictionTick(skippedTick);
        cache.pushPredictionState(priorState);
    }

    // -----------------------------------------------------------------------
    // Post-prediction state push — called by SimulationManager after integrate
    // (replaces postSimulationAll from the retired SimulationNetworking class).
    // -----------------------------------------------------------------------

    void postPredictionAll(const SimulationTimeStep& step)
    {
        if (step.getStepKind() == StepKind::Stall)
            return;

        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            SIMLOG(m_logger, "[PostPrediction] id=%u tick=%u", id, step.getTick());
            getCacheFor<T>(id).pushPredictionState(
                simulatable.getAllState().getState());
        });
    }

    // Resim equivalent of postPredictionAll. Writes into the cache slot for the
    // resim tick (step.getTick()), not into the prediction-frontier slot that
    // pushPredictionState targets.
    //
    // ⛔ [item 45] IT NO LONGER TOUCHES GATE STATE, AND MUST NOT. This comment used
    // to end "also flips m_isResimulated on the slot so getLastResimulationTick /
    // needsResimulation can see the resim has progressed" — that bit, that scan and
    // that gate are all retired. A replay tick now writes STATE ONLY; the gate is
    // closed once, explicitly, by the CAS on the completion edge
    // (`consumeResimAnchorsAll`). This is the site that makes candidates A/B's
    // 1-tick-resim storm structurally impossible instead of merely improbable: if a
    // replay tick wrote gate state, the gate would again be derivable from what the
    // replay just did, and a completed resim could re-trigger itself.
    //
    // [og-netcode-v2-input-relay item 42] RETURNS THE NUMBER OF DISCARDS — replayed
    // ticks whose slot had already left the 60-slot cache window. Observational
    // only; the caller (SimulationManager) hands it to the resim-gate probe as I6's
    // `replayOverruns`. Behaviour is unchanged: every character is still swept, the
    // cache's own Warning line still fires per discard, and the previous `void`
    // return had no reader. The baseline is 1-2 per RUN, so a nonzero window
    // reading is the over-replay / domain-skew evidence finding §3 predicts and
    // could not previously count.
    //
    // ⛔ [item 47] AND A REPLAY TICK NO LONGER OVERWRITES A CORRECTED SLOT. The
    // per-slot rule, its two invariants and the fresh/stale classifier live in
    // `resimGate::classifyResimSlotWrite`; what is visible from HERE is the
    // sweep's report of it. `outFreshProtections` / `outStaleProtections` are
    // DEFAULTED out-pointers on the item 42 pattern, so the call site that does
    // not want them is byte-identical.
    //
    // ⚠ A PROTECTED SLOT IS NOT A DISCARD. The return value keeps its item 42
    // meaning exactly — replayed ticks whose slot had left the 60-slot window —
    // because `replayOverruns` has an archived baseline (1-2 per RUN) that a
    // redefinition would silently invalidate. Protections are a different
    // population and get their own two counters.
    unsigned int postResimulationAll(const SimulationTimeStep& step,
                                     unsigned int* outFreshProtections = nullptr,
                                     unsigned int* outStaleProtections = nullptr)
    {
        unsigned int discards          = 0u;
        unsigned int freshProtections  = 0u;
        unsigned int staleProtections  = 0u;
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            SIMLOG(m_logger, "[Resim.Post] id=%u tick=%u", id, step.getTick());
            resimGate::ResimSlotWriteOutcome outcome =
                resimGate::ResimSlotWriteOutcome::Discarded;
            if (!getCacheFor<T>(id).tryInsertingResimulatedState(
                    typename T::StateType(simulatable.getAllState().getState()),
                    step.getTick(), &outcome))
            {
                ++discards;
                return;
            }
            if (outcome == resimGate::ResimSlotWriteOutcome::ProtectedFresh)
                ++freshProtections;
            else if (outcome == resimGate::ResimSlotWriteOutcome::ProtectedStale)
                ++staleProtections;
        });
        if (outFreshProtections != nullptr)
            *outFreshProtections = freshProtections;
        if (outStaleProtections != nullptr)
            *outStaleProtections = staleProtections;
        return discards;
    }

    // -----------------------------------------------------------------------
    // Correction injection — called from OnRep_-dispatched lambdas (game thread)
    // -----------------------------------------------------------------------

    // [og-netcode-v2-input-relay T4 / design D3] The correction now carries a
    // SECOND scalar beside the tick: the capture tick of the input the authority
    // applied when it produced this state (kNoInputCaptureTick when it substituted
    // one). It is stashed in the SAME cache slot as the state it corrects, so a
    // later resim can ask "which input produced tick T" for every tick it replays
    // rather than only for the newest correction — the reason a single scalar
    // stash would not do (T6 consumes it).
    //
    // Read through the buffer's getAppliedCaptureTick rather than a second
    // readInto: the ref is a fixed-offset header field, and the state read has
    // already paid for the composite.
    //
    // [og-netcode-v2-input-relay T24] `outVerdict` forwards the cache's existing
    // prediction-vs-authority verdict to the caller. It is DEFAULTED and purely
    // observational — see CorrectionInsertVerdict in CorrectionCache.h for why the
    // verdict has to leave the cache at all (the cache is id-agnostic; the
    // attribution this initiative needs is by id AND by character class, and
    // neither fact exists down there).
    //
    // THIS CLASS IS NOT THE PLACE THE VERDICT IS INTERPRETED, and deliberately so.
    // It knows the `id` but not whether that id is locally controlled or a remote
    // proxy: provider-presence lives in SimulationNetSync's `m_inputProviders`, and
    // reconciliation owning a second answer to "is this character remote" is
    // exactly the duplicate-truth shape the T6/T16 retirements exist to avoid. So
    // this method only relays.
    template <typename T, typename BufferT>
    void injectCorrectionState(unsigned int id, const BufferT& buffer,
                               CorrectionInsertVerdict* outVerdict = nullptr)
    {
        typename T::StateType state;
        const uint32 tick = buffer.readInto(state);
        const uint32 appliedCaptureTick = buffer.getAppliedCaptureTick();
        SIMLOG(m_logger, "[InjectCorrectionState] id=%u tick=%u appliedCaptureTick=%u",
            id, tick, appliedCaptureTick);
        getCacheFor<T>(id).tryInsertingCorrectState(
            std::move(state), tick, appliedCaptureTick, outVerdict);
    }

    // [og-netcode-v2-input-relay T8] `injectCorrectionInput` IS GONE. It was the
    // client-side half of the SERVER->CLIENT correction-INPUT channel: decode the
    // replicated (tick, input) payload, stash it in the cache slot. The whole
    // channel is retired — the server no longer writes it (sendCorrectionAll), the
    // component no longer replicates it, and StateCorrectionCache no longer has an
    // insertion point for it. A remote character's input now travels on the relay
    // ring and is resolved by CAPTURE TICK, not by application tick.
    //
    // `injectCorrectionState` above is untouched and carries the T4
    // applied-capture-tick ref, which is what replaced this channel's usefulness.

    // -----------------------------------------------------------------------
    // Divergence check — called by SimulationManager (replaces checkIsSimilarAll)
    // -----------------------------------------------------------------------

    // [og-netcode-v2-input-relay item 45] R1 — THE GATE READ, once per physics
    // frame, folded across characters with `min` because a Chaos rewind is global:
    // one restore tick has to serve every character, so it must be the OLDEST tick
    // anybody still needs replayed.
    //
    // `maxAnchorDepthTicks` IS THE DEPTH POLICY, and 0 MEANS NO POLICY — the same
    // convention `resimGate::isAnchorWithinDepthPolicy` documents, so the caller can
    // hand it `rollbackWindowTicks` or 0 without a second boolean. The manager
    // decides which (`resimGate::policyEnforcesDepthCeiling`) and reads the value
    // live from its TimeConfig, exactly as `sendCorrectionAll` reads
    // `correctionRotationK` — caching it here would make an ini-driven setting
    // silently ineffective.
    //
    // AN OVER-DEEP ANCHOR IS SKIPPED AND COUNTED, NEVER CLAMPED. Clamping would
    // restore at a mid-window slot no correction ever landed in, replay identical
    // inputs from identical state, and reproduce the same prediction: a guaranteed
    // no-op costing a full Chaos rewind. Skipping leaves the anchor PENDING — it is
    // not consumed here — so recovery is a newer correction raising it back inside
    // the window, or the HardResync failsafe. That is also why `outDeepAnchorSkips`
    // counts CHARACTER-FRAMES rather than distinct anchors: a stranded deep anchor is
    // re-examined and re-counted every frame it stays stranded, which is the shape
    // `refusedFrames` already uses for the second gate and the reading that makes a
    // stuck one visible.
    unsigned int checkDivergenceAll(uint32 maxAnchorDepthTicks,
                                    unsigned int* outDeepAnchorSkips = nullptr)
    {
        unsigned int correctionTick = std::numeric_limits<unsigned int>::max();
        unsigned int deepAnchorSkips = 0u;
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& cache = getCacheFor<T>(id);
            const bool needsResim = cache.needsResimulation();
            const uint32 anchorTick = cache.getPendingResimAnchorTick();
            const uint32 predictionTick = cache.getPredictionTick();
            const bool withinDepth = resimGate::isAnchorWithinDepthPolicy(
                anchorTick, predictionTick, maxAnchorDepthTicks);
            SIMLOG(m_logger,
                "[ResimCheck.Check] id=%u needsResim=%d anchorTick=%u predictionTick=%u withinDepth=%d",
                id, needsResim ? 1 : 0, anchorTick, predictionTick, withinDepth ? 1 : 0);
            if (!needsResim)
                return;
            if (!withinDepth)
            {
                ++deepAnchorSkips;
                return;
            }
            correctionTick = std::min(correctionTick,
                static_cast<unsigned int>(anchorTick));
        });
        if (outDeepAnchorSkips != nullptr)
            *outDeepAnchorSkips = deepAnchorSkips;
        return correctionTick == std::numeric_limits<unsigned int>::max() ? 0u : correctionTick;
    }

    // -----------------------------------------------------------------------
    // Resim restore — called by SimulationManager before resim replay
    // -----------------------------------------------------------------------

    // [item 45] R2 lives here: every cache CAPTURES the anchor it currently has
    // pending, so the completion edge can CAS against it. Captured for EVERY
    // character, including one whose slot for `simTick` is missing and which is
    // therefore not restored — deliberately, because that character is still
    // replayed forward by `integrateAll` and its UNCORRECTED slots are still
    // written by `postResimulationAll`, so its pending correction is consumed by
    // this resim whether or not the restore reached it. ([item 47] "its slots are
    // still overwritten" was true without qualification when item 45 wrote it;
    // corrected slots are now protected, which is the next paragraph's subject.)
    //
    // ⚠ [item 47] THE PARENTHETICAL THAT STOOD HERE IS NOW HALF FALSE, AND THE
    // HALF THAT CHANGED IS ITEM 47's WHOLE SUBJECT. It read: "that its corrected
    // state is OVERWRITTEN rather than applied is a PRE-EXISTING property of
    // anchoring the whole replay on a single min tick ... and is not something
    // this change alters". Item 45 did not alter it; ITEM 47 DOES. A replay no
    // longer overwrites any corrected slot, so the newer-anchored character's
    // authority state SURVIVES its shared replay — which is what makes the
    // follow-up trigger point at real data instead of at a re-derivation of the
    // prediction (the HOLLOW ANCHOR). What is STILL true, and is item 47's named
    // P1 policy rather than an oversight: that character's ANCHOR is still
    // consumed by this resim's per-cache CAS even though the resim restored at
    // the shared min and never applied its correction — consume-all, as shipped.
    // Recovery is the next rotation landing (<= ceil(N/K) ticks). The alternative
    // (P2, capture-the-restore-tick, a convergent cascade of <= N resims) is
    // priced by item 46's per-resim cost data and is deliberately NOT decided
    // here — see item 47's acceptance criteria for the flip condition.
    //
    // [item 47] `captureResimAnchorForConsume` now captures TWO values: the
    // anchor for the consume CAS, and the landing-sequence baseline the
    // fresh/stale classifier compares against. One call, one instant — see the
    // note on that method.
    void prepareResimAll(uint32 simTick)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& cache = getCacheFor<T>(id);
            cache.captureResimAnchorForConsume();
            const uint32 idx = cache.getCacheIndex(simTick);
            const bool found = idx != StateCorrectionCache<typename T::StateType, typename T::InputType>::InvalidCacheIndex;
            SIMLOG(m_logger, "[ResimCheck.PrepareRestore] id=%u simTick=%u found=%d",
                id, simTick, found ? 1 : 0);
            if (found)
                simulatable.editAllState().editState() = cache.getState(idx);
        });
    }

    // [item 45] W2 — THE CONSUME EDGE, swept across characters. Called from the
    // `[Resim.Finish]` block in `SimulationManager::onPostGameSimulation`, beside
    // `applyResimAll`, which is the one place where "a resim actually completed" is
    // known.
    //
    // Returns the number of caches whose anchor SURVIVED the CAS, i.e. characters
    // for which a newer correction landed on the game thread mid-replay and which
    // will therefore re-trigger. That is the intended behaviour, not an error, and
    // the count exists so a future reader can tell "mid-replay landings are
    // frequent" from "the consume edge is broken" — the failure mode this design is
    // chosen for is fail-LOUD (the anchor stays pending and the gate retries,
    // visible in item 42's `repeatRequests` within one window) rather than the
    // legacy fail-silent (a broken flag discipline pins the gate shut and
    // under-resimulates for months).
    //
    // ⚠ IT MUST STAY ON THE COMPLETION EDGE. An anchor consumed at PREPARE time
    // would be lost if the resim never reaches its apply edge — item 42 measures
    // ~20 % of prepares doing exactly that (the stranded-cursor class) — and the
    // correction it stood for would never be replayed by anybody.
    //
    // ⛔ AND IT EMITS NO LOG LINE, deliberately: item 45 forbids new `[Resim.` /
    // `[ResimCheck.`-prefixed lines outright (`[Resim.` inherits `LogOGSim=Verbose`,
    // T19's 10 MB defect) and a per-character-per-resim line is exactly that volume
    // class. It needs none, which is the design's best maintainability property: a
    // surviving anchor leaves the gate OPEN, so the next frame requests again with a
    // DIFFERENT anchor — visible in item 42's existing `requests` without a matching
    // `repeatRequests`, at Warning, in one probe window.
    //
    // The RETURN VALUE is purely observational — the count of caches whose anchor
    // survived the CAS, i.e. characters that took a mid-replay landing and will
    // re-trigger. Its only reader today is the LLT that pins this sweep
    // (og-brawler-tests `[SimulationReconciliation]`), which is the same bargain item
    // 42 struck for `tryInsertingResimulatedState`'s bool: a `void` here would make
    // the sweep unobservable from a test at all, and the alternative — asserting it
    // through a probe field — would add a shipped counter that reads 0 until item 46.
    unsigned int consumeResimAnchorsAll()
    {
        unsigned int survivingAnchors = 0u;
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& cache = getCacheFor<T>(id);
            const bool consumed = cache.consumeCapturedResimAnchor();
            if (!consumed && cache.getPendingResimAnchorTick() != 0u)
                ++survivingAnchors;
        });
        return survivingAnchors;
    }

    // [item 45] The trigger-policy door. ONE-WAY PUBLICATION: `TimeConfig::
    // resimTriggerPolicy` is the source of truth, `SimulationManager::
    // setResimTriggerPolicy` is the only writable entry point, and this method fans
    // it out to every existing cache AND remembers it for caches created later
    // (`createCacheFor`). The remembered copy exists because character registration
    // happens long after composition; it is never written from anywhere else, which
    // is what keeps it from becoming a second source of truth.
    void setResimTriggerPolicy(TimeConfig::ResimTriggerPolicy policy)
    {
        m_resimTriggerPolicy = policy;
        forEachCache([policy](auto& cache) { cache.setResimTriggerPolicy(policy); });
    }

    // ⭐ [item 47 amendment 3] THE FRONTIER-SLOT RULING LIVES HERE, AND IT NEEDS NO
    // CODE. This method publishes whatever the frontier SLOT holds. Item 47 stops
    // the replay overwriting a corrected slot, so if the frontier slot is
    // corrected at replay end, what this publishes into live state is the
    // AUTHORITY state rather than the replay's result — authority beats a
    // re-derivation of it at the one slot where the difference is externally
    // visible, exactly as it does everywhere else. That is a deliberate
    // behaviour change in what `[Resim.Finish]` publishes, and it is pinned by a
    // named case (`AFrontierExactLandingAtReplayEndIsWhatResimPublishes`) rather
    // than left implied.
    void applyResimAll()
    {
        // Read from the prediction frontier — resim postPredictionAll writes into
        // the predictionTick slot every step, so the freshly-resimulated state lives
        // there, not at the earliest-resim slot.
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& cache = getCacheFor<T>(id);
            const uint32 tick = cache.getPredictionTick();
            const uint32 idx = cache.getCacheIndex(tick);
            const bool found = idx != StateCorrectionCache<typename T::StateType, typename T::InputType>::InvalidCacheIndex;
            SIMLOG(m_logger, "[Resim.Apply] id=%u tick=%u found=%d",
                id, tick, found ? 1 : 0);
            if (found)
                simulatable.editAllState().editState() = cache.getState(idx);
        });
    }

    // =======================================================================
    // [og-netcode-v2-input-relay item 48] THE SLOT-PROVENANCE DUMP — the one
    // SHIPPED reader of `StateCorrectionCache`'s diagnostic provenance column,
    // and the reason that column satisfies T16 instead of re-breaking it.
    //
    //     [Verbose][ResimProbe.SlotMap] id=%u frontier=%u map=<60 chars>
    //
    // ⚠ THE VOLUME, ROUTING, ORDERING AND DECIDES-NOTHING RULINGS ARE ALL AT THE
    // ONE FORMATTING SITE, `logSlotProvenanceFor` in this class's private
    // section — read that before changing anything here or adding a third caller.
    // The short version: Verbose-only on the existing `LogOGResimProbe`, so the
    // line does not exist at shipped verbosity; raw slot-index order; and it
    // feeds no decision anywhere, which is machine-checked one level down by
    // `…TheProvenanceColumnCannotReachAnyProductionOutput`.
    // =======================================================================
    // CALL SITE 1 of 2 — ONE COMPLETED RESIM. Called from the `[Resim.Finish]`
    // block in `SimulationManager::onPostGameSimulation`, after `applyResimAll`
    // and the anchor consume, because that is the only point at which a replay's
    // whole effect on the cache is finished and visible: the span has been
    // written, the protections have been taken and the frontier slot has been
    // published. Emitting at prepare would show the map the resim was ABOUT to
    // change.
    void dumpSlotProvenanceAll()
    {
        if (!m_logger)
            return;

        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            logSlotProvenanceFor(id, getCacheFor<T>(id));
        });
    }

    // -----------------------------------------------------------------------
    // Resim wipe — called via clock callback on hard resync
    // -----------------------------------------------------------------------

    void wipeAllForResync(uint32 newPredictionTick)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            SIMLOG(m_logger, "[TimeResync.WipeCache] id=%u newPredictionTick=%u",
                id, newPredictionTick);
            // [item 48] CALL SITE 2 of 2 — ONE WIPE, and deliberately BEFORE it,
            // in the SAME sweep so the map cannot describe a different set of
            // characters than the wipe it accompanies.
            //
            // ⭐ AFTER THE WIPE THE MAP IS ALL `.` BY CONSTRUCTION, which is
            // knowable without reading a log and is therefore not a diagnostic.
            // The interesting map is the one a hard resync is about to DESTROY —
            // what lineage the cache carried at the moment the clock gave up on
            // it.
            //
            // ⚠ IT LIVES HERE RATHER THAN BESIDE THE `[TimeResync.Wipe]` LINE IN
            // `SimulationManager`'s resync callback for a mechanical reason worth
            // recording: that callback is instantiated by every LLT that builds a
            // `SimulationManager` on a MOCK reconciliation, so a call there would
            // have required adding this method to four mock types that model
            // nothing about it.
            logSlotProvenanceFor(id, getCacheFor<T>(id));
            getCacheFor<T>(id).wipeCache(newPredictionTick);
        });
    }

    // -----------------------------------------------------------------------
    // Resim replay input — RELOCATED to SimulationNetSync by T6.
    // -----------------------------------------------------------------------
    //
    // `collectResimInputAll` used to live here, and its own comment said why:
    // "resim inputs come purely from the cache". T6 falsified that premise. The
    // resim input is now resolved from the client's own delay lines, the relayed-
    // input stores and the injected neutrals — all NetSync-owned — and only the
    // JOIN KEY comes from the cache. So the method moved to the class that owns
    // the sources, and this class kept exactly one narrow query
    // (getAppliedCaptureTickRef, below) that hands over that key and nothing else.
    //
    // Do not re-add a resim collector here. If a future task needs one, it needs
    // the stores, and the stores are not this class's business.

    // -----------------------------------------------------------------------
    // The cache input column — RETIRED. There are no input readers here.
    // -----------------------------------------------------------------------
    //
    // [og-netcode-v2-input-relay T8] `getLastCorrectionInput` IS GONE, at this
    // level and on StateCorrectionCache. This section's old heading — "remote-
    // client input lookup, called by collectInputAll for simulatables with no
    // local input provider" — described the pre-T7 proxy branch, which now
    // resolves through the relay store. With the correction-input channel retired
    // there is nothing left to flag an input slot as server-sourced, so the
    // accessor could only have answered nullopt for the rest of time: a call that
    // compiles, looks authoritative and quietly always fails.
    //
    // [og-netcode-v2-input-relay T16] `getLatestInput` IS ALSO GONE, and with it
    // the COLUMN both of them read. T8 left it standing under "remove what T8
    // makes FALSE, leave what T8 makes merely UNUSED" — it still returned real,
    // freshly-written data, it just had no caller. T16 removes the writer, so
    // there is nothing left to return.
    //
    // WHAT SURVIVES THIS SECTION, and it is the point of the whole retirement:
    // `findInputCache` below (still the route for every accessor here — it is NOT
    // an input reader despite the name, which is pre-initiative), plus the two
    // applied-capture-tick queries. A slot answers "WHICH capture the authority
    // applied at tick T", never "what that capture WAS". The value comes from
    // ClientInputDelayLine (local) or RelayedInputStore (remote), both owned by
    // SimulationNetSync.

    // [T4 / D3] The join key stored for `tick` — "which capture tick did the
    // authority apply when it produced the state it corrected us to at `tick`".
    //
    // Returns nullopt when the tick is outside this character's cache window (no
    // slot to have stored anything), and kNoInputCaptureTick when a slot exists
    // but names no capture — the two are deliberately distinguishable: the first
    // means "cannot answer", the second means "the authority itself had no client
    // capture behind that tick" (D1), which T6 resolves to game-zero.
    //
    // Routed through findInputCache (nullable) so it is safe to call on the
    // authority, where no correction caches are allocated at all.
    template <typename T>
    std::optional<uint32> getAppliedCaptureTick(unsigned int id, uint32 tick) const
    {
        const auto* cache = findInputCache<T>(id);
        if (cache == nullptr)
            return std::nullopt;

        const uint32 idx = cache->getCacheIndex(tick);
        if (idx == StateCorrectionCache<typename T::StateType, typename T::InputType>::InvalidCacheIndex)
            return std::nullopt;

        return cache->getAppliedCaptureTick(idx);
    }

    // [T6] THE ONE QUERY SimulationNetSync::collectResimInputAll consumes — the
    // classified form of the raw accessor above. See the AppliedCaptureRef block
    // at the top of this header for what each kind means and why `Absent` is two
    // kinds rather than one.
    //
    // WHY BOTH EXIST. getAppliedCaptureTick (T4) reports the SLOT VALUE and is the
    // accessor T4's own wire/stash tests assert against; it cannot tell a
    // correction that named no capture from a slot no correction ever reached,
    // because both hold the sentinel. This one adds that single discriminator (the
    // slot's correction flag) and is the only form safe to dispatch a resolution
    // table on. Neither exposes the cache.
    //
    // Routed through the nullable findInputCache, so it is safe on the authority
    // (no caches allocated there) — where it answers NoSlot for every id, which is
    // correct: the authority never resimulates.
    template <typename T>
    AppliedCaptureRef getAppliedCaptureTickRef(unsigned int id, uint32 simTick) const
    {
        const auto* cache = findInputCache<T>(id);
        if (cache == nullptr)
            return AppliedCaptureRef{};

        const uint32 idx = cache->getCacheIndex(simTick);
        if (idx == StateCorrectionCache<typename T::StateType, typename T::InputType>::InvalidCacheIndex)
            return AppliedCaptureRef{};

        // An uncorrected slot is NOT a sentinel — it is "no answer yet". Checking
        // the flag before the value is what keeps the prediction frontier (and any
        // never-replicated intermediate tick) out of the sentinel row.
        if (!cache->containsCorrectTick(idx))
            return AppliedCaptureRef{ AppliedCaptureRefKind::NoRef, kNoInputCaptureTick };

        const uint32 ref = cache->getAppliedCaptureTick(idx);
        return ref == kNoInputCaptureTick
            ? AppliedCaptureRef{ AppliedCaptureRefKind::Sentinel, kNoInputCaptureTick }
            : AppliedCaptureRef{ AppliedCaptureRefKind::Ref, ref };
    }

    // Returns a pointer to the correction cache for the given simulatable type and id,
    // or nullptr if no cache exists (e.g. on the authority, where caches are not allocated).
    //
    // [T16] THE NAME IS HISTORICAL — it predates this initiative and no longer
    // describes anything about inputs. This is the nullable route every accessor
    // above takes, and in particular the route for getAppliedCaptureTickRef, which
    // is T6's join-key query and the single most load-bearing read in the resim
    // path. It was NOT retired with the input column. Renaming it is a separate,
    // purely cosmetic change and was deliberately not bundled into a deletion task.
    template <typename T>
    const StateCorrectionCache<typename T::StateType, typename T::InputType>* findInputCache(unsigned int id) const
    {
        const auto& map = std::get<CacheMapFor<T>>(m_caches);
        auto it = map.find(id);
        if (it == map.end())
            return nullptr;
        return &it->second;
    }

private:
    template <typename T>
    using CacheMapFor = std::unordered_map<
        unsigned int,
        StateCorrectionCache<typename T::StateType, typename T::InputType>>;

    template <typename T>
    auto& getCacheFor(unsigned int id)
    {
        return std::get<CacheMapFor<T>>(m_caches).at(id);
    }

    // [og-netcode-v2-input-relay item 48] THE ONE FORMATTING SITE for the
    // slot-provenance map, shared by both call sites above so the two cannot
    // print different shapes for the same column.
    //
    //     [Verbose][ResimProbe.SlotMap] id=%u frontier=%u map=<60 chars>
    //
    // ⛔ VERBOSE, AND THAT IS NOT NEGOTIABLE DOWNWARD. This is per-slot,
    // per-window data — 60 characters per character per emission. At Warning it
    // would be T19's 10 MB defect wearing a different tag. `LogOGResimProbe`
    // ships at `Warning`, so on a default run THIS LINE DOES NOT EXIST;
    // `-LogCmds="LogOGResimProbe Verbose"` turns it on beside the existing
    // `[ResimProbe.Request]` / `[ResimProbe.Stranded]` per-event lines — same
    // knob, same category, same family. NO new category and NO new ini key:
    // `[ResimProbe` already routes here by prefix
    // (`SimulationManagerUImpl::EmitOGLine`).
    //
    // ⚠ AT MOST ONCE PER COMPLETED RESIM AND ONCE PER WIPE, and that bound is
    // STRUCTURAL rather than a throttle — those are the only two callers. A
    // per-frame or per-replay-tick call would be exactly the volume class T19
    // was filed to stop. Do not add one.
    //
    // ⭐ WHY THE MAP IS IN RAW SLOT-INDEX ORDER (0..59) AND NOT TICK ORDER: the
    // column IS the ring, so ring order is the literal contents with no derived
    // ordering to go stale, and `frontier=` is printed beside it so a reader can
    // locate the newest tick. A tick-ordered map would be a second, computed
    // view that could disagree with the thing it claims to show.
    //
    // ⛔ IT DECIDES NOTHING AND MUST NEVER BE MADE TO. Deleting this method would
    // change no simulated value; the independence of the column it reads is
    // machine-checked one level down by
    // `…TheProvenanceColumnCannotReachAnyProductionOutput`. See
    // SlotStateProvenance.h for what fence 2's "production output" excludes and
    // why this line is not one.
    template <typename CacheT>
    void logSlotProvenanceFor(unsigned int id, const CacheT& cache)
    {
        if (!m_logger)
            return;

        // +1 for the terminator. Sized from the cache's own constant rather than
        // a literal 60, so a future ring resize cannot silently truncate the map
        // into a half-truth.
        char map[CacheT::StateBufferSize + 1u];
        for (uint32 slot = 0u; slot < static_cast<uint32>(CacheT::StateBufferSize); ++slot)
            map[slot] = slotStateProvenanceChar(cache.getDiagnosticStateProvenance(slot));
        map[CacheT::StateBufferSize] = '\0';

        SIMLOG(m_logger, "[Verbose][ResimProbe.SlotMap] id=%u frontier=%u map=%s",
            id, cache.getPredictionTick(), map);
    }

    // [item 45] Visits every allocated cache of every simulatable type. Note it
    // walks the CACHE MAPS, not storage: the policy fan-out runs at composition,
    // before any character is registered, so a `forEachSimulatable` sweep would
    // reach nothing. It is a config-publication helper only — every per-tick sweep
    // in this class stays storage-driven so it visits characters in the storage's
    // order.
    template <typename Fn>
    void forEachCache(Fn&& fn)
    {
        std::apply(
            [&fn](auto&... cacheMaps) {
                auto visitOne = [&fn](auto& cacheMap) {
                    for (auto& entry : cacheMap)
                        fn(entry.second);
                };
                (visitOne(cacheMaps), ...);
            },
            m_caches);
    }

    std::tuple<CacheMapFor<SimulatableTs>...>  m_caches;
    SimulationObjectStorage<SimulatableTs...>& m_storage;
    std::function<void(const char*)>           m_logger;

    // [item 45] The session trigger policy, remembered ONLY so that
    // `createCacheFor` can stamp a cache created after composition. Source of truth
    // is TimeConfig; see setResimTriggerPolicy. Default read from the TimeConfig
    // default, not a literal (R-P1).
    TimeConfig::ResimTriggerPolicy m_resimTriggerPolicy = TimeConfig{}.resimTriggerPolicy;
};

// ---------------------------------------------------------------------------
// SimulationReconciliationConcept
// ---------------------------------------------------------------------------

template <typename T, typename... SimulatableTs>
concept SimulationReconciliationConcept = requires(
    T& t, const SimulationTimeStep& step, uint32 tick)
{
    { t.postPredictionAll(step) };
    { t.postResimulationAll(step) };
    // [item 45] The gate read takes the DEPTH POLICY (0 == no policy) and reports
    // depth-skipped character-frames through a defaulted out-pointer. The manager
    // supplies both from its live TimeConfig.
    { t.checkDivergenceAll(tick) } -> std::convertible_to<unsigned int>;
    { t.wipeAllForResync(tick) };
    { t.prepareResimAll(tick) };
    { t.applyResimAll() };
    // [item 45] The resim-completion consume edge, and the trigger-policy door. Both
    // are named here because SimulationManager calls them, so a substitute
    // reconciliation without them would fail at the call site rather than at the
    // concept — which is the whole reason this concept exists.
    { t.consumeResimAnchorsAll() } -> std::convertible_to<unsigned int>;
    { t.setResimTriggerPolicy(TimeConfig{}.resimTriggerPolicy) };
    // [T6] collectResimInputAll MOVED to SimulationNetSyncConcept — resim input
    // resolution is no longer a cache-only operation. See the relocation note in
    // the class body.
};

#pragma optimize("", on)
// pragma optimize on.
