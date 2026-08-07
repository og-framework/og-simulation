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

    template <typename T>
    void createCacheFor(unsigned int id)
    {
        std::get<CacheMapFor<T>>(m_caches).emplace(
            id,
            StateCorrectionCache<typename T::StateType, typename T::InputType>(m_logger));
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
    // pushPredictionState targets. Also flips m_isResimulated on the slot so
    // getLastResimulationTick / needsResimulation can see the resim has
    // progressed — the direct editState write the pre-rework code used skipped
    // this bit.
    void postResimulationAll(const SimulationTimeStep& step)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            SIMLOG(m_logger, "[Resim.Post] id=%u tick=%u", id, step.getTick());
            getCacheFor<T>(id).tryInsertingResimulatedState(
                typename T::StateType(simulatable.getAllState().getState()),
                step.getTick());
        });
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

    unsigned int checkDivergenceAll()
    {
        unsigned int correctionTick = std::numeric_limits<unsigned int>::max();
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& cache = getCacheFor<T>(id);
            const bool needsResim = cache.needsResimulation();
            const uint32 lastResimTick = cache.getLastResimulationTick();
            const uint32 predictionTick = cache.getPredictionTick();
            SIMLOG(m_logger,
                "[ResimCheck.Check] id=%u needsResim=%d lastResimTick=%u predictionTick=%u",
                id, needsResim ? 1 : 0, lastResimTick, predictionTick);
            if (needsResim)
                correctionTick = std::min(correctionTick,
                    static_cast<unsigned int>(lastResimTick));
        });
        return correctionTick == std::numeric_limits<unsigned int>::max() ? 0u : correctionTick;
    }

    // -----------------------------------------------------------------------
    // Resim restore — called by SimulationManager before resim replay
    // -----------------------------------------------------------------------

    void prepareResimAll(uint32 simTick)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& cache = getCacheFor<T>(id);
            const uint32 idx = cache.getCacheIndex(simTick);
            const bool found = idx != StateCorrectionCache<typename T::StateType, typename T::InputType>::InvalidCacheIndex;
            SIMLOG(m_logger, "[ResimCheck.PrepareRestore] id=%u simTick=%u found=%d",
                id, simTick, found ? 1 : 0);
            if (found)
                simulatable.editAllState().editState() = cache.getState(idx);
        });
    }

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

    // -----------------------------------------------------------------------
    // Resim wipe — called via clock callback on hard resync
    // -----------------------------------------------------------------------

    void wipeAllForResync(uint32 newPredictionTick)
    {
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            SIMLOG(m_logger, "[TimeResync.WipeCache] id=%u newPredictionTick=%u",
                id, newPredictionTick);
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

    std::tuple<CacheMapFor<SimulatableTs>...>  m_caches;
    SimulationObjectStorage<SimulatableTs...>& m_storage;
    std::function<void(const char*)>           m_logger;
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
    { t.checkDivergenceAll() } -> std::convertible_to<unsigned int>;
    { t.wipeAllForResync(tick) };
    { t.prepareResimAll(tick) };
    { t.applyResimAll() };
    // [T6] collectResimInputAll MOVED to SimulationNetSyncConcept — resim input
    // resolution is no longer a cache-only operation. See the relocation note in
    // the class body.
};

#pragma optimize("", on)
// pragma optimize on.
