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

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// [T6] AppliedCaptureRef — which capture tick the authority applied at tick T.
//
// ⛔ AppliedCaptureRef exposes NO cache and NO store, deliberately — §1.
//
// ⛔ The ruling's `Absent` is split into NoSlot + NoRef — same meaning here, different required behaviour at the call site. §1.
//
// ⛔ NoSlot = emit NO input. NoRef = re-derive. Sentinel = injected game zero.
// Ref = use `captureTick`, which WINS over any relay schedule stamp. §1.

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
    // `captureTick` is meaningful only when `kind == Ref`; the sentinel otherwise.
    uint32                captureTick = kNoInputCaptureTick;
};

// ⛔ ALL THREE `ResimSweepDiagnostics` FIELDS COME FROM ONE `postResimulationAll` PASS — §2.
//
// ⛔ `staleProtections` HAS NEVER BEEN OBSERVED NONZERO — not a reason to delete it. §2.

struct ResimSweepDiagnostics
{
    unsigned int discards         = 0u;
    unsigned int freshProtections = 0u;
    unsigned int staleProtections = 0u;
};

// SimulationReconciliation<SimulatableTs...>
//
// Owns the per-simulatable StateCorrectionCache instances and every operation
// that reads or writes them.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
//
// Relocation history, retired rationale and archived measurement records:
// `docs/SimulationReconciliation-rationale.md`.

// ---------------------------------------------------------------------------
// ORIENTATION — WHO CALLS WHAT, ON WHICH THREAD, IN WHAT ORDER.
//
// Read this first. Every fence in this file states one invariant at the line
// it guards; none of them restates this map, and this map states no invariant.
//
//   * TWO THREADS TOUCH THIS CLASS, AND ONLY ONE OF THEM TICKS.
//       GAME thread    corrections arrive — `injectCorrectionState`, from the
//                      replication-dispatched lambdas — and caches are created
//                      and destroyed — `createCacheFor` / `removeCacheFor`,
//                      from the registration facade.
//       PHYSICS thread every per-tick sweep in this class, without exception,
//                      all driven from the adapter's physics callbacks.
//     That crossing is row 2 of `docs/ThreadingCrossings.md` — accepted debt,
//     healed by the next correction; not re-argued here.
//
//   * ⭐ ONE ADAPTER'S BINDING FOR THE CALLBACK COLUMN OF BOTH TABLES BELOW.
//     That column names the physics-callback ROLE a phase is driven from, never
//     an engine type. One adapter binds those roles on
//     `FSimulationManagerAsyncCallback` (`OGSimulationUnreal`,
//     `SimulationManagerUImpl.cpp`), where that one adapter's names are
//     `OnPreSimulate_Internal` (pre-simulate step), `OnPostSolve_Internal`
//     (post-solve step), `TriggerRewindIfNeeded_Internal` (rewind request) and
//     `FirstPreResimStep_Internal` (first resim step). Another adapter
//     substitutes its own; the ROLES, their ORDER and their THREAD are what this
//     map states, and are what a port must re-establish.
//
//   * NOTHING HERE RUNS ON THE AUTHORITY. The server allocates no correction
//     cache at all, so `findInputCache` answers nullptr for every id there and
//     `SimulationManager` gates the tick calls behind `m_runsPrediction`.
//
//   * THE PREDICTION TICK — one PAIR, both halves on the PHYSICS thread:
//
//       allocateFrontierSlotsAll   opens the frontier slot   pre-simulate step
//       postPredictionAll          completes it with state   post-solve step
//
//     The contract binding the two is stated in full at
//     `allocateFrontierSlotsAll`.
//
//   * THE RESIM CYCLE — six phases, ALL on the PHYSICS thread, in this order:
//
//       phase   this class               SimulationManager.h            adapter callback
//       ask     checkDivergenceAll       onCheckIsSimilar               rewind request
//       arm     prepareResimAll          prepareResimulation            first resim step
//       replay  -- integration only --   onGameSimulationResimulation   pre-simulate step, per tick
//       record  postResimulationAll      onPostGameSimulation           post-solve step
//       land    applyResimAll            onPostGameSimulation           post-solve step, catch-up edge
//       close   consumeResimAnchorsAll   onPostGameSimulation           post-solve step, same edge
//
//     `land` runs before `close`, and `close` runs on the completion edge
//     rather than at `arm`. Both orderings are load-bearing and both are
//     argued at `consumeResimAnchorsAll` below — not here.
//
//   * `wipeAllForResync` belongs to none of those six phases: it is the
//     hard-resync failsafe, reached through the clock callback, not the tick.
//
// The same cycle seen from the manager's side, with the role gates that keep
// it off the authority: `docs/Perspective-AuthorityVsPrediction.md` §7 — not
// re-derived here.
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

    // ⛔ `try_emplace`, NOT `emplace` — the cache is non-movable, and a moved copy
    // would give two objects one gate. §3.
    //
    // ⛔ A cache created after composition is born with `setResimTriggerPolicy`'s stamp. §3.
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

    // The frontier-pair contract lives at `allocateFrontierSlotsAll` — §5.

    template <typename T>
    void pushPredictionTick(unsigned int id, uint32 tick)
    {
        getCacheFor<T>(id).pushPredictionTick(tick);
    }

    // ⛔ `pushPredictionInput` IS GONE FROM HERE, with the input column — §4.

    // ⛔ Push the TICK before the STATE — `pushPredictionState` writes the current tick's slot.
    //
    // ⛔ The `pushPredictionInput(InputType{})` that sat between them IS GONE — §4.
    template <typename T>
    void backfillSkippedTick(unsigned int id, uint32 skippedTick, const typename T::StateType& priorState)
    {
        SIMLOG(m_logger, "[TimeResync.BackfillSkipped] id=%u skippedTick=%u", id, skippedTick);
        auto& cache = getCacheFor<T>(id);
        cache.pushPredictionTick(skippedTick);
        cache.pushPredictionState(priorState);
    }

    // THE FRONTIER-PAIR CONTRACT — this method's ALLOCATING half. §5.
    // ⛔ Every tick pushed here is completed by `postPredictionAll`'s
    //    `pushPredictionState` in the SAME manager tick; both halves gate on ONE
    //    predicate, `stepAllocatesFrontierSlot` — never re-derive it.
    // ⛔ No gate writes here, and do not move this push to capture time.
    //
    // ⛔ THREE PRODUCTION CALL SITES of `stepAllocatesFrontierSlot`, not fewer — §5.
    //
    // ⛔ `findInputCache != nullptr` IS the prediction-ownership test here — §6.
    //
    // ⛔ ITERATION IS STORAGE-DRIVEN, FILTERED ON `findInputCache` — a bare cache-map
    // walk fires `m_frontierSlotAwaitingState` as a false positive by construction. §6.
    //
    // ⛔ ITEM 92's `OG_CHECK` IS GONE FROM HERE with `allocateFrontierSlotForCharacter`;
    // a half-registered id is now silently skipped, not aborted. Priced at §6.
    //
    // ⛔ NO gate state here — `pushPredictionTick` / `backfillSkippedTick` must not write `needsResimulation`.
    //
    void allocateFrontierSlotsAll(const SimulationTimeStep& step)
    {
        const bool allocatesSlot = stepAllocatesFrontierSlot(step.getStepKind());

        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;

            // ⛔ `findInputCache` here proves the entry exists, which makes `getCacheFor`'s `.at(id)` safe. §6.
            if (findInputCache<T>(id) == nullptr)
                return;

            if (step.getStepKind() == StepKind::Skip)
                backfillSkippedTick<T>(id, step.getTick() - 1, simulatable.getAllState().getState());

            if (allocatesSlot)
                pushPredictionTick<T>(id, step.getTick());
        });
    }

    void postPredictionAll(const SimulationTimeStep& step)
    {
        // ⛔ THE COMPLETING half. Shares one predicate with `allocateFrontierSlotsAll`;
        // it was a literal `== StepKind::Stall`, and re-splitting diverges the gates. §5.
        //
        // ⛔ THERE IS DELIBERATELY NO CAPTURE-SIDE `OG_CHECK` HERE — a
        // `getPredictionTick()` check was audited and REJECTED: an ordinary player join
        // is indistinguishable from a violation at this point. §7.
        if (!stepAllocatesFrontierSlot(step.getStepKind()))
            return;

        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            SIMLOG(m_logger, "[PostPrediction] id=%u tick=%u", id, step.getTick());
            getCacheFor<T>(id).pushPredictionState(
                simulatable.getAllState().getState());
        });
    }

    // ⛔ Resim twin of `postPredictionAll` — but writes the RESIM tick's slot, not
    // the frontier slot `pushPredictionState` targets.
    //
    // ⛔ IT NO LONGER TOUCHES GATE STATE, AND MUST NOT. `m_isResimulated` is gone;
    // the gate closes once at `consumeResimAnchorsAll`'s CAS — that is what makes
    // the 1-tick resim storm structurally impossible. §8.
    //
    // `discards` — replayed ticks past the window; reaches the probe as `replayOverruns`. §8.
    //
    // ⛔ A replay no longer overwrites a corrected slot — `resimGate::classifyResimSlotWrite` holds the rule; `freshProtections`/`staleProtections` report it.
    //
    // ⛔ A PROTECTED SLOT IS NOT A DISCARD — `discards` keeps its exact meaning,
    // because `replayOverruns` has an archived baseline. §8.
    //
    // ⛔ ONE SWEEP, ONE TICK, ALL THREE COUNTS in `ResimSweepDiagnostics` — same failure as `noteDeepAnchorSkips` after `noteCheck`. §2.
    ResimSweepDiagnostics postResimulationAll(const SimulationTimeStep& step)
    {
        ResimSweepDiagnostics diagnostics;
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            SIMLOG(m_logger, "[Resim.Post] id=%u tick=%u", id, step.getTick());
            resimGate::ResimSlotWriteOutcome outcome =
                resimGate::ResimSlotWriteOutcome::Discarded;
            if (!getCacheFor<T>(id).tryInsertingResimulatedState(
                    typename T::StateType(simulatable.getAllState().getState()),
                    step.getTick(), &outcome))
            {
                ++diagnostics.discards;
                return;
            }
            if (outcome == resimGate::ResimSlotWriteOutcome::ProtectedFresh)
                ++diagnostics.freshProtections;
            else if (outcome == resimGate::ResimSlotWriteOutcome::ProtectedStale)
                ++diagnostics.staleProtections;
        });
        return diagnostics;
    }

    // Correction injection — called from replication-dispatched lambdas (GAME thread).

    // ⛔ `appliedCaptureTick` rides the SAME slot as the state it corrects
    // (`kNoInputCaptureTick` = substituted); one scalar would answer only for the
    // newest correction. §9.
    //
    // Read via `getAppliedCaptureTick`, not a second `readInto`: fixed-offset field.
    //
    // ⛔ `outDiagnosticVerdict` names the CHANNEL — `tryInsertingCorrectState`'s verdict is production. §9.
    //
    // ⛔ THIS CLASS DOES NOT INTERPRET THE VERDICT — that is `isLocallyControlled`'s job.
    template <typename T, typename BufferT>
    void injectCorrectionState(unsigned int id, const BufferT& buffer,
                               CorrectionInsertVerdict* outDiagnosticVerdict = nullptr)
    {
        typename T::StateType state;
        const uint32 tick = buffer.readInto(state);
        const uint32 appliedCaptureTick = buffer.getAppliedCaptureTick();
        SIMLOG(m_logger, "[InjectCorrectionState] id=%u tick=%u appliedCaptureTick=%u",
            id, tick, appliedCaptureTick);
        getCacheFor<T>(id).tryInsertingCorrectState(
            std::move(state), tick, appliedCaptureTick, outDiagnosticVerdict);
    }

    // ⛔ `injectCorrectionInput` IS GONE — the correction-INPUT channel is retired. §9.
    //
    // `injectCorrectionState` above carries the applied-capture ref that replaced it.

    // ⛔ R1 — the gate read, once per PHYSICS frame, folded with `min`: one physics
    // rewind serves everyone, so it must be the OLDEST tick anybody needs. §10.
    //
    // ⛔ `maxAnchorDepthTicks` IS THE DEPTH POLICY, 0 MEANS NO POLICY; the manager
    // reads `rollbackWindowTicks` live — caching it here kills an ini setting. §10.
    //
    // ⛔ AN OVER-DEEP ANCHOR IS SKIPPED AND COUNTED, NEVER CLAMPED — clamping is a
    // guaranteed no-op costing a full physics rewind. `outDiagnosticDeepAnchorSkips`
    // counts character-FRAMES. §10.
    //
    // ⛔ THE SKIP IS PRODUCTION; ONLY THE `++` IS OBSERVATION. `needsResim`,
    // `anchorTick`, `predictionTick`, `withinDepth` are DUAL-USE — guarding any of
    // the four behind a diagnostic branch breaks the depth policy silently. §10.
    unsigned int checkDivergenceAll(uint32 maxAnchorDepthTicks,
                                    unsigned int* outDiagnosticDeepAnchorSkips = nullptr)
    {
        unsigned int correctionTick = std::numeric_limits<unsigned int>::max();
        unsigned int diagnosticDeepAnchorSkips = 0u;
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
                ++diagnosticDeepAnchorSkips;
                return;
            }
            correctionTick = std::min(correctionTick,
                static_cast<unsigned int>(anchorTick));
        });
        if (outDiagnosticDeepAnchorSkips != nullptr)
            *outDiagnosticDeepAnchorSkips = diagnosticDeepAnchorSkips;
        return correctionTick == std::numeric_limits<unsigned int>::max() ? 0u : correctionTick;
    }

    // ⛔ R2 — captures the anchor for EVERY character, including one whose slot is
    // missing: it is still replayed by `integrateAll` and written by `postResimulationAll`. §11.
    //
    // ⛔ CONSUME-ALL IS A RULING — an anchor is consumed even when the shared-min restore never applied its correction. §11.
    //
    // `captureResimAnchorForConsume` captures TWO values in ONE instant. §11.
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

    // ⛔ W2 — the consume edge, beside `applyResimAll` in `onPostGameSimulation`. §12.
    //
    // ⛔ A SURVIVING ANCHOR IS INTENDED, NOT AN ERROR — it re-triggers, visible in `repeatRequests`. §12.
    //
    // ⛔ IT MUST STAY ON THE COMPLETION EDGE — ~20 % of prepares never reach their
    // apply edge, and an anchor consumed at prepare takes its correction with it. §12.
    //
    // ⛔ THIS FUNCTION EMITS NO LOG LINE, deliberately — cross-check is `requests` without a matching `repeatRequests`. Do not add one. §12.
    //
    // ⛔ THE RETURN IS DIAGNOSTIC; THE CALL IS LOAD-BEARING. The CAS inside this
    // sweep is what CLOSES THE GATE — deleting the call breaks it. Do not group it
    // under `getDiagnostics()`; `logSlotProvenanceAll` went there for the opposite reason. §12.
    //
    // `SimulationReconciliationConcept` constrains the return, so `void` was never an option.
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

    // ⛔ ONE-WAY PUBLICATION — TimeConfig is the source of truth; this is the only writer of the copy `createCacheFor` stamps. §13.
    void setResimTriggerPolicy(TimeConfig::ResimTriggerPolicy policy)
    {
        m_resimTriggerPolicy = policy;
        forEachCache([policy](auto& cache) { cache.setResimTriggerPolicy(policy); });
    }

    // ⛔ THE FRONTIER-SLOT RULING NEEDS NO CODE — this publishes whatever the slot
    // holds, so a surviving authority state wins. Pinned by
    // `AFrontierExactLandingAtReplayEndIsWhatResimPublishes`. §13.
    void applyResimAll()
    {
        // Reads the prediction frontier — resim `postPredictionAll` writes there.
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

    // ⛔ The one SHIPPED reader of `StateCorrectionCache`'s provenance column. §14.
    //
    // ⛔ A DELIBERATE ONE-MEMBER VIEW — `logSlotProvenanceAll` has no production role. §14.
    //
    //
    // ⛔ Volume, routing and decides-nothing rulings all live at `logSlotProvenanceFor` (`LogOGResimProbe`) — read it before adding a third caller.
    class Diagnostics
    {
    public:
        explicit Diagnostics(const SimulationReconciliation& reconciliation)
            : m_reconciliation(reconciliation)
        {
        }

        // ⛔ CALL SITE 1 of 2 — after `applyResimAll` in `onPostGameSimulation`; emitting at prepare shows the map about to change.
        void logSlotProvenanceAll() const
        {
            if (!m_reconciliation.m_logger)
                return;

            m_reconciliation.m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
                using T = std::remove_reference_t<decltype(simulatable)>;
                m_reconciliation.logSlotProvenanceFor(id, m_reconciliation.getCacheFor<T>(id));
            });
        }

    private:
        const SimulationReconciliation& m_reconciliation;
    };

    Diagnostics getDiagnostics() const { return Diagnostics(*this); }

    void wipeAllForResync(uint32 newPredictionTick)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            SIMLOG(m_logger, "[TimeResync.WipeCache] id=%u newPredictionTick=%u",
                id, newPredictionTick);
            // ⛔ CALL SITE 2 of 2 — ONE WIPE, deliberately BEFORE it and in the SAME sweep.
            //
            // ⛔ After the wipe the map is all `.` by construction — the interesting map is the one the resync destroys. §14.
            //
            // ⛔ DELIBERATELY NOT in `SimulationManager`'s resync callback — four mock types would need this method. §14.
            logSlotProvenanceFor(id, getCacheFor<T>(id));
            getCacheFor<T>(id).wipeCache(newPredictionTick);
        });
    }

    //
    // ⛔ Do not re-add a resim collector here — it needs the stores, and they are not this class's business.

    // The cache input column — RETIRED. There are no input readers here.
    //
    // ⛔ `getLastCorrectionInput` IS GONE — it could only ever answer nullopt. §15.
    //
    // ⛔ `getLatestInput` IS ALSO GONE, with the column both readers read. §15.
    //
    // ⛔ `findInputCache` is NOT an input reader despite the name — a slot answers WHICH capture was applied, never what it was. §15.

    // The join key for `tick`: which capture the authority applied at that tick.
    //
    // ⛔ nullopt = outside the window; `kNoInputCaptureTick` = a slot naming no capture. Deliberately distinguishable. §16.
    //
    // Routed through `findInputCache` (nullable): safe on the authority.
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

    // ⛔ THE ONE QUERY `collectResimInputAll` consumes — see the `Absent` split, §1.
    //
    // ⛔ `getAppliedCaptureTick` reports the SLOT VALUE and cannot tell a no-capture
    // correction from an unreached slot; only this form is safe to dispatch on. §16.
    //
    // Routed through `findInputCache`: safe on the authority, where it answers NoSlot for every id.
    template <typename T>
    AppliedCaptureRef getAppliedCaptureTickRef(unsigned int id, uint32 simTick) const
    {
        const auto* cache = findInputCache<T>(id);
        if (cache == nullptr)
            return AppliedCaptureRef{};

        const uint32 idx = cache->getCacheIndex(simTick);
        if (idx == StateCorrectionCache<typename T::StateType, typename T::InputType>::InvalidCacheIndex)
            return AppliedCaptureRef{};

        // ⛔ An uncorrected slot is NOT a sentinel — check `containsCorrectTick` first.
        if (!cache->containsCorrectTick(idx))
            return AppliedCaptureRef{ AppliedCaptureRefKind::NoRef, kNoInputCaptureTick };

        const uint32 ref = cache->getAppliedCaptureTick(idx);
        return ref == kNoInputCaptureTick
            ? AppliedCaptureRef{ AppliedCaptureRefKind::Sentinel, kNoInputCaptureTick }
            : AppliedCaptureRef{ AppliedCaptureRefKind::Ref, ref };
    }

    // Returns the cache for this id, or nullptr (e.g. on the authority).
    //
    // ⛔ THE NAME IS HISTORICAL — `findInputCache` says nothing about inputs. §15.
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

    // Const overload, for `Diagnostics::logSlotProvenanceAll`.
    template <typename T>
    const auto& getCacheFor(unsigned int id) const
    {
        return std::get<CacheMapFor<T>>(m_caches).at(id);
    }

    // ⛔ THE ONE FORMATTING SITE for the slot-provenance map — both call sites share it. §14.
    //
    //
    // ⛔ VERBOSE, AND NOT NEGOTIABLE DOWNWARD — `LogOGResimProbe` ships at Warning, so this line does not exist by default.
    //
    // ⛔ AT MOST ONCE PER RESIM AND ONCE PER WIPE — the bound is STRUCTURAL, not a
    // throttle. Do not add a per-frame call. §14.
    //
    // ⛔ Raw slot-index order, NOT tick order — the column IS the ring, so a tick-ordered map is a second view that can disagree.
    //
    // ⛔ IT DECIDES NOTHING AND MUST NEVER BE MADE TO — machine-checked by `...TheProvenanceColumnCannotReachAnyProductionOutput`. §14.
    template <typename CacheT>
    void logSlotProvenanceFor(unsigned int id, const CacheT& cache) const
    {
        if (!m_logger)
            return;

        // ⛔ Sized from `StateBufferSize`, not a literal 60 — a ring resize must not silently truncate the map.
        char map[CacheT::StateBufferSize + 1u];
        for (uint32 slot = 0u; slot < static_cast<uint32>(CacheT::StateBufferSize); ++slot)
            map[slot] = slotStateProvenanceChar(cache.getDiagnostics().stateProvenance(slot));
        map[CacheT::StateBufferSize] = '\0';

        SIMLOG(m_logger, "[Verbose][ResimProbe.SlotMap] id=%u frontier=%u map=%s",
            id, cache.getPredictionTick(), map);
    }

    // ⛔ Walks the CACHE MAPS, not storage — a `forEachSimulatable` sweep would reach
    // nothing at composition time. Every per-tick sweep here stays storage-driven. §13.
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

    // ⛔ Remembered ONLY so `createCacheFor` can stamp a late cache; source of truth is `TimeConfig` (R-P1). §13.
    TimeConfig::ResimTriggerPolicy m_resimTriggerPolicy = TimeConfig{}.resimTriggerPolicy;
};

// SimulationReconciliationConcept

template <typename T, typename... SimulatableTs>
concept SimulationReconciliationConcept = requires(
    T& t, const SimulationTimeStep& step, uint32 tick)
{
    // THE OPENING half of the frontier pair — reconciliation-distinguishing.
    { t.allocateFrontierSlotsAll(step) };
    { t.postPredictionAll(step) };
    { t.postResimulationAll(step) };
    // ⛔ `checkDivergenceAll` takes the DEPTH POLICY (0 == no policy).
    { t.checkDivergenceAll(tick) } -> std::convertible_to<unsigned int>;
    { t.wipeAllForResync(tick) };
    { t.prepareResimAll(tick) };
    { t.applyResimAll() };
    // ⛔ `consumeResimAnchorsAll` and `setResimTriggerPolicy` are named here so a substitute lacking them fails at the concept, not the call site. §12, §13.
    { t.consumeResimAnchorsAll() } -> std::convertible_to<unsigned int>;
    { t.setResimTriggerPolicy(TimeConfig{}.resimTriggerPolicy) };
    // `collectResimInputAll` moved to SimulationInputResolutionConcept.
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
