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

// pragma optimize off — debugger-friendliness across all build configs (breakpoints hit,
// locals visible, call-stack intact). OGSim-core convention.
#pragma optimize("", off)

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

    template <typename T>
    void pushPredictionInput(unsigned int id, const typename T::InputType& input)
    {
        getCacheFor<T>(id).pushPredictionInput(input);
    }

    // Called on StepKind::Skip — back-fills the skipped tick with a zero input and prior state.
    // skippedTick is step.getTick() - 1 (the tick that was jumped over).
    // Must push tick then input before state: pushPredictionState writes into the slot
    // for the current prediction tick, so the tick advance must happen first.
    template <typename T>
    void backfillSkippedTick(unsigned int id, uint32 skippedTick, const typename T::StateType& priorState)
    {
        SIMLOG(m_logger, "[TimeResync.BackfillSkipped] id=%u skippedTick=%u", id, skippedTick);
        auto& cache = getCacheFor<T>(id);
        cache.pushPredictionTick(skippedTick);
        cache.pushPredictionInput(typename T::InputType{});
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

    template <typename T, typename BufferT>
    void injectCorrectionState(unsigned int id, const BufferT& buffer)
    {
        typename T::StateType state;
        const uint32 tick = buffer.readInto(state);
        SIMLOG(m_logger, "[InjectCorrectionState] id=%u tick=%u", id, tick);
        getCacheFor<T>(id).tryInsertingCorrectState(std::move(state), tick);
    }

    template <typename T, typename BufferT>
    void injectCorrectionInput(unsigned int id, const BufferT& buffer)
    {
        typename T::InputType input;
        const uint32 tick = buffer.readInto(input);
        SIMLOG(m_logger, "[InjectCorrectionInput] id=%u tick=%u", id, tick);
        getCacheFor<T>(id).insertCorrectionInput(std::move(input), tick);
    }

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
    // Resim replay input — called by SimulationManager before integrate in resim.
    // Lives here (not on NetSync) because resim inputs come purely from the cache.
    // -----------------------------------------------------------------------

    ResolvedInputs<SimulatableTs...> collectResimInputAll(uint32 simTick)
    {
        ResolvedInputs<SimulatableTs...> inputs;
        m_storage.forEachSimulatable([&](unsigned int id, auto& simulatable) {
            using T = std::remove_reference_t<decltype(simulatable)>;
            auto& cache = getCacheFor<T>(id);
            auto& map   = std::get<std::unordered_map<unsigned int, typename T::InputType>>(inputs);
            const uint32 idx = cache.getCacheIndex(simTick);
            if (idx != StateCorrectionCache<typename T::StateType, typename T::InputType>::InvalidCacheIndex)
                map.emplace(id, cache.getInput(idx));
        });
        return inputs;
    }

    // -----------------------------------------------------------------------
    // Remote-client input lookup — called by SimulationNetSync::collectInputAll
    // for simulatables with no local input provider (remote client branch).
    // -----------------------------------------------------------------------

    template <typename T>
    std::optional<typename T::InputType> getLastCorrectionInput(unsigned int id)
    {
        return getCacheFor<T>(id).getLastCorrectionInput();
    }

    // Returns a pointer to the correction cache for the given simulatable type and id,
    // or nullptr if no cache exists (e.g. on the authority, where caches are not allocated).
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
    { t.collectResimInputAll(tick) } -> std::convertible_to<ResolvedInputs<SimulatableTs...>>;
};

#pragma optimize("", on)
// pragma optimize on — restore command-line optimization settings.
