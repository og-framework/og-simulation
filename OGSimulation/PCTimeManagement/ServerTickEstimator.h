#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/ServerTickEstimator-rationale.md

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "OGSimulation/PCTimeManagement/TimeConfig.h"

struct ServerTickEstimatorConfig
{
    double   tickFrequency        = TimeConfig{}.tickFrequency;
    double   filterTauSeconds     = 5.0;
    double   rttGateMultiplier    = 1.5;
    double   rttGateMarginSeconds = 0.010;
    uint32_t samplesToValid       = 20u;
    double   staleAfterSeconds    = 1.0;
    double   reseedJumpTicks      = 3.0;
    uint32_t reseedRunSamples     = 5u;
};

class ServerTickEstimator
{
public:
    static constexpr uint32_t kNoServerTick = 0xFFFFFFFFu;
    static constexpr double   kMaxServerTickAgeSeconds = 2147483648.0 / 1e6;

    explicit ServerTickEstimator(const ServerTickEstimatorConfig& config = {})
        : m_config(config)
    {
    }

    void noteSample(uint32_t serverTick, double serverTickAgeSeconds, double rttSeconds, double t3Seconds,
                    double smoothedRttSeconds)
    {
        if (serverTick == kNoServerTick || !std::isfinite(serverTickAgeSeconds) || !std::isfinite(rttSeconds)
            || !std::isfinite(t3Seconds) || !std::isfinite(smoothedRttSeconds) || serverTickAgeSeconds < 0.0
            || serverTickAgeSeconds >= kMaxServerTickAgeSeconds)
            return;

        if (smoothedRttSeconds > 0.0
            && rttSeconds > m_config.rttGateMultiplier * smoothedRttSeconds + m_config.rttGateMarginSeconds)
        {
            m_rejectedSamples.fetch_add(1u, std::memory_order_relaxed);
            return;
        }

        const double f          = m_config.tickFrequency;
        const double tick       = static_cast<double>(serverTick);
        const double halfRtt    = rttSeconds / 2.0;
        const double clockTicks = t3Seconds * f;
        const double dt         = std::max(0.0, t3Seconds - m_lastSampleSeconds.load(std::memory_order_relaxed));
        const double gain       = 1.0 - std::exp(-dt / m_config.filterTauSeconds);

        m_lastSampleSeconds.store(t3Seconds, std::memory_order_relaxed);
        if (m_offset.note(tick + (serverTickAgeSeconds + halfRtt) * f - clockTicks, gain, m_config))
            m_reseeds.fetch_add(1u, std::memory_order_relaxed);
        m_phaseBlindOffset.note(tick + (0.5 / f + halfRtt) * f - clockTicks, gain, m_config);

        m_acceptedSamples.fetch_add(1u, std::memory_order_relaxed);
    }

    bool isValid(double nowSeconds) const
    {
        return m_offset.samplesSinceSeed.load(std::memory_order_acquire) >= m_config.samplesToValid
               && nowSeconds - m_lastSampleSeconds.load(std::memory_order_relaxed) <= m_config.staleAfterSeconds;
    }

    double serverTickAt(double nowSeconds) const
    {
        return nowSeconds * m_config.tickFrequency + m_offset.publishedTicks();
    }

    double phaseBlindServerTickAt(double nowSeconds) const
    {
        return nowSeconds * m_config.tickFrequency + m_phaseBlindOffset.publishedTicks();
    }

    uint32_t acceptedSamples() const { return m_acceptedSamples.load(std::memory_order_relaxed); }
    uint32_t rejectedSamples() const { return m_rejectedSamples.load(std::memory_order_relaxed); }
    uint32_t reseeds() const { return m_reseeds.load(std::memory_order_relaxed); }
    double   lastSampleSeconds() const { return m_lastSampleSeconds.load(std::memory_order_relaxed); }

    const ServerTickEstimatorConfig& config() const { return m_config; }

private:
    static_assert(std::atomic<double>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free,
        "ServerTickEstimator: readers on other threads acquire a filter's count, then load the atomics the one "
        "sample writer stored before releasing it; a platform without lock-free 8- and 4-byte atomics must fail "
        "here, not take a lock on the reader.");

    struct OffsetFilter
    {
        std::atomic<double>   ticks{0.0};
        std::atomic<uint32_t> samplesSinceSeed{0u};
        uint32_t              jumpRun = 0u;

        double publishedTicks() const
        {
            static_cast<void>(samplesSinceSeed.load(std::memory_order_acquire));
            return ticks.load(std::memory_order_relaxed);
        }

        bool note(double sampleTicks, double gain, const ServerTickEstimatorConfig& config)
        {
            const double   current = ticks.load(std::memory_order_relaxed);
            const uint32_t count   = samplesSinceSeed.load(std::memory_order_relaxed);
            if (count == 0u)
            {
                seed(sampleTicks);
                return false;
            }
            if (std::abs(sampleTicks - current) > config.reseedJumpTicks)
            {
                if (++jumpRun < config.reseedRunSamples)
                    return false;
                seed(sampleTicks);
                return true;
            }
            jumpRun = 0u;
            ticks.store(current + gain * (sampleTicks - current), std::memory_order_relaxed);
            samplesSinceSeed.store(count + 1u, std::memory_order_release);
            return false;
        }

        void seed(double sampleTicks)
        {
            ticks.store(sampleTicks, std::memory_order_relaxed);
            samplesSinceSeed.store(1u, std::memory_order_release);
            jumpRun = 0u;
        }
    };

    ServerTickEstimatorConfig m_config;
    OffsetFilter              m_offset;
    OffsetFilter              m_phaseBlindOffset;
    std::atomic<double>       m_lastSampleSeconds{0.0};
    std::atomic<uint32_t>     m_acceptedSamples{0u};
    std::atomic<uint32_t>     m_rejectedSamples{0u};
    std::atomic<uint32_t>     m_reseeds{0u};
};
