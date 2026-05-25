// SPDX-License-Identifier: MPL-2.0
#include "NetworkTimeEstimator.h"

#include <cstdio>

#pragma optimize( "", off )

NetworkTimeEstimator::NetworkTimeEstimator(const TimeConfig& config, LoggerFn logger)
    : m_config(config)
    , m_smoothedRTT(0.0)
    , m_smoothedJitter(0.0)
    , m_hasFirstSample(false)
    , m_logger(std::move(logger))
{
}

void NetworkTimeEstimator::updateRTT(double rawRTTSeconds)
{
    if (!m_hasFirstSample)
    {
        m_smoothedRTT    = rawRTTSeconds;
        m_smoothedJitter = 0.0;
        m_hasFirstSample = true;
    }
    else
    {
        const double alpha  = m_config.rttSmoothingAlpha;
        const double alphaJ = m_config.jitterSmoothingAlpha;

        const double delta  = rawRTTSeconds >= m_smoothedRTT
                                ? rawRTTSeconds - m_smoothedRTT
                                : m_smoothedRTT - rawRTTSeconds;

        m_smoothedRTT    = alpha  * rawRTTSeconds + (1.0 - alpha)  * m_smoothedRTT;
        m_smoothedJitter = alphaJ * delta          + (1.0 - alphaJ) * m_smoothedJitter;
    }
}

void NetworkTimeEstimator::recordAuthorityTick(unsigned int serverTick)
{
    m_authorityTick.store(serverTick, std::memory_order_relaxed);

    if (m_logger)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[Verbose] PCTM NTE: authorityTick=%u smoothedRTT=%.3fms smoothedJitter=%.3fms targetPredTick=%u",
            m_authorityTick.load(std::memory_order_relaxed),
            m_smoothedRTT    * 1000.0,
            m_smoothedJitter * 1000.0,
            getTargetPredictionTick());
        m_logger(buf);
    }
}

unsigned int NetworkTimeEstimator::getPredictionOffsetTicks() const
{
    if (!m_hasFirstSample)
        return 0;

    const double rawOffset = (m_smoothedRTT + m_config.jitterMultiplier * m_smoothedJitter)
                             * m_config.tickFrequency;

    // ceil rounds up so predictions arrive slightly early rather than late.
    const double ceiled = std::ceil(rawOffset);
    return ceiled > 0.0 ? static_cast<unsigned int>(ceiled) : 0u;
}

unsigned int NetworkTimeEstimator::getTargetPredictionTick() const
{
    return m_authorityTick.load(std::memory_order_relaxed) + getPredictionOffsetTicks();
}

unsigned int NetworkTimeEstimator::getLastAuthorityTick() const
{
    return m_authorityTick.load(std::memory_order_relaxed);
}

double NetworkTimeEstimator::getSmoothedRTT() const
{
    return m_smoothedRTT;
}

double NetworkTimeEstimator::getSmoothedJitter() const
{
    return m_smoothedJitter;
}

#pragma optimize( "", on )
