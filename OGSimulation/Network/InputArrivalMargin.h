#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/InputArrivalMargin-rationale.md · docs/InputArrivalMargin-guards.md

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "OGSimulation/Network/ConnectionTierTable.h"

struct InputArrivalWindowSummary
{
    uint32_t samples        = 0u;
    int32_t  marginP05Ticks = 0;
    int32_t  marginP50Ticks = 0;
    int32_t  marginP95Ticks = 0;
    int32_t  marginMinTicks = 0;
    uint32_t lateRefused    = 0u;
};

struct ArrivalMarginSignal
{
    int32_t  windowEndServerTick = 0;
    uint32_t samples             = 0u;
    int32_t  m0P05Ticks          = 0;
    int32_t  m0P50Ticks          = 0;
    int16_t  m0EwmaQ4            = 0;
    uint32_t lateFirstSight      = 0u;
    uint32_t lateResent          = 0u;
    int32_t  stepsPerBundleP50   = 0;
    int32_t  effectiveDelayTicks = 0;
    uint32_t backDateEpisodes    = 0u;
};

struct ArrivalBackDateOpening
{
    int32_t  m0P50Ticks          = 0;
    uint32_t sustainedWindows    = 0u;
    int32_t  thresholdTicks      = 0;
    int32_t  effectiveDelayTicks = 0;
};

template <ConnectionAddress Address>
class InputArrivalMargin
{
public:
    struct WindowLine
    {
        Address                               address;
        InputArrivalWindowSummary             summary;
        ArrivalMarginSignal                   arrivalSignal;
        std::optional<ArrivalBackDateOpening> backDateOpening;
    };

    using WindowLines = std::vector<WindowLine>;

    static constexpr int32_t kNoDrainYet        = std::numeric_limits<int32_t>::min();
    static constexpr int32_t kSeenMaskTicks     = 64;
    static constexpr int32_t kStepsPerBundleMin = 1;
    static constexpr int32_t kStepsPerBundleMax = 16;
    static constexpr double  kEwmaQ4Scale       = 16.0;

    static constexpr uint32_t kBackDateGraceWindows   = 5u;
    static constexpr uint32_t kBackDateSustainWindows = 5u;
    static constexpr uint32_t kBackDateMinSamples     = 60u;

    InputArrivalMargin(int32_t windowTicks, double tickFrequency, int32_t backDateThresholdTicks)
        : m_windowTicks(windowTicks)
        , m_ewmaAlpha(tickFrequency >= 1.0 ? 1.0 / tickFrequency : 1.0)
        , m_backDateThresholdTicks(backDateThresholdTicks)
    {
    }

    bool noteSighting(unsigned int ownerId, int32_t captureTick)
    {
        Owner& owner = m_owners[ownerId];
        if (!owner.sighted)
        {
            owner.sighted = true;
            owner.anchor  = captureTick;
            owner.seen    = 1u;
            return true;
        }
        const int64_t ahead = static_cast<int64_t>(captureTick) - static_cast<int64_t>(owner.anchor);
        if (ahead >= kSeenMaskTicks || ahead <= -kSeenMaskTicks)
        {
            owner.anchor = captureTick;
            owner.seen   = 1u;
            return true;
        }
        if (ahead > 0)
        {
            owner.seen   = (owner.seen << ahead) | 1u;
            owner.anchor = captureTick;
            return true;
        }
        const uint64_t bit   = uint64_t{ 1 } << (-ahead);
        const bool     first = (owner.seen & bit) == 0u;
        owner.seen |= bit;
        return first;
    }

    void noteArrival(const Address& addr, int32_t captureTick, int32_t effectiveDelay, int32_t nextUndrainedTick,
                     bool firstSight)
    {
        if (nextUndrainedTick == kNoDrainYet)
        {
            return;
        }
        Connection& connection = m_connections[addr];
        connection.margins.push_back(captureTick + effectiveDelay - nextUndrainedTick);
        if (firstSight)
        {
            recordM0(connection, captureTick - nextUndrainedTick);
            connection.lastSampleDelay = effectiveDelay;
        }
    }

    void noteLateRefused(const Address& addr)
    {
        ++m_connections[addr].lateRefused;
    }

    void noteLateFirstSight(const Address& addr, int32_t captureTick, int32_t nextUndrainedTick)
    {
        Connection& connection = m_connections[addr];
        ++connection.lateFirstSight;
        if (nextUndrainedTick != kNoDrainYet)
        {
            recordM0(connection, captureTick - nextUndrainedTick);
        }
    }

    void noteLateResent(const Address& addr)
    {
        ++m_connections[addr].lateResent;
    }

    void noteBundleNewest(const Address& addr, unsigned int ownerId, int32_t newestCaptureTick)
    {
        Owner& owner = m_owners[ownerId];
        if (!owner.bundleSighted)
        {
            owner.bundleSighted    = true;
            owner.lastBundleNewest = newestCaptureTick;
            return;
        }
        const int64_t steps =
            static_cast<int64_t>(newestCaptureTick) - static_cast<int64_t>(owner.lastBundleNewest);
        if (steps >= kStepsPerBundleMin && steps <= kStepsPerBundleMax)
        {
            m_connections[addr].stepsPerBundle.push_back(static_cast<int32_t>(steps));
        }
        if (steps > 0 || steps <= -kSeenMaskTicks)
        {
            owner.lastBundleNewest = newestCaptureTick;
        }
    }

    bool closeWindowIfDue(int32_t serverTick, WindowLines& out)
    {
        out.clear();
        if (m_windowTicks <= 0)
        {
            return false;
        }
        if (!m_windowStarted)
        {
            m_windowStarted   = true;
            m_windowStartTick = serverTick;
            return false;
        }
        if (serverTick - m_windowStartTick < m_windowTicks)
        {
            return false;
        }

        for (auto& [addr, connection] : m_connections)
        {
            if (connection.margins.empty() && connection.lateRefused == 0u)
            {
                connection.stepsPerBundle.clear();
                continue;
            }
            const InputArrivalWindowSummary summary = summarize(connection);
            ArrivalMarginSignal             windowSignal = summarizeSignal(connection, serverTick);
            const std::optional<ArrivalBackDateOpening> opening = trackBackDate(connection, windowSignal);
            windowSignal.backDateEpisodes = connection.backDateEpisodes;
            connection.latest       = summary;
            connection.latestSignal = windowSignal;
            connection.margins.clear();
            connection.lateRefused    = 0u;
            connection.m0.clear();
            connection.lateFirstSight = 0u;
            connection.lateResent     = 0u;
            connection.stepsPerBundle.clear();
            out.push_back(WindowLine{ addr, summary, windowSignal, opening });
        }
        m_windowStartTick = serverTick;
        return true;
    }

    std::optional<InputArrivalWindowSummary> latest(const Address& addr) const
    {
        const auto it = m_connections.find(addr);
        return (it == m_connections.end()) ? std::nullopt : it->second.latest;
    }

    std::optional<ArrivalMarginSignal> signal(const Address& addr) const
    {
        const auto it = m_connections.find(addr);
        return (it == m_connections.end()) ? std::nullopt : it->second.latestSignal;
    }

    void forget(const Address& addr)
    {
        m_connections.erase(addr);
    }

    void forgetOwner(unsigned int ownerId)
    {
        m_owners.erase(ownerId);
    }

    void forgetDeadWires()
    {
        std::vector<Address> dead;
        for (const auto& [addr, connection] : m_connections)
        {
            if (!addr.isAlive())
            {
                dead.push_back(addr);
            }
        }
        for (const Address& addr : dead)
        {
            forget(addr);
        }
    }

    int32_t windowTicks() const { return m_windowTicks; }

    double ewmaAlpha() const { return m_ewmaAlpha; }

    int32_t backDateThresholdTicks() const { return m_backDateThresholdTicks; }

    std::size_t connectionCount() const { return m_connections.size(); }

    std::size_t ownerCount() const { return m_owners.size(); }

    uint32_t openWindowSamples(const Address& addr) const
    {
        const auto it = m_connections.find(addr);
        return (it == m_connections.end()) ? 0u : static_cast<uint32_t>(it->second.margins.size());
    }

    void scribbleForFenceTest(uint32_t seed)
    {
        uint64_t state = (static_cast<uint64_t>(seed) + 1u) * 0x9E3779B97F4A7C15ull;
        const auto next = [&state]()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return state;
        };
        const auto draw = [&next](uint32_t span, int32_t base)
        {
            return base + static_cast<int32_t>(next() % span);
        };

        for (auto& [ownerId, owner] : m_owners)
        {
            owner.anchor           = owner.anchor + draw(40u, 1);
            owner.seen             = next();
            owner.lastBundleNewest = owner.lastBundleNewest + draw(9u, -4);
        }
        for (auto& [addr, connection] : m_connections)
        {
            connection.margins.push_back(draw(64u, -32));
            connection.m0.push_back(draw(64u, -32));
            connection.stepsPerBundle.push_back(draw(16u, 1));
            const uint32_t addedFirstSight = static_cast<uint32_t>(next() % 7u);
            const uint32_t addedResent     = static_cast<uint32_t>(next() % 7u);
            connection.lateFirstSight += addedFirstSight;
            connection.lateResent     += addedResent;
            connection.lateRefused    += addedFirstSight + addedResent;
            connection.m0Ewma          = static_cast<double>(draw(200u, -100));
            connection.ewmaSeeded      = true;
            connection.lastSampleDelay = draw(32u, 0);
            connection.windowsWithSamples  = static_cast<uint32_t>(next() % (kBackDateGraceWindows + 2u));
            connection.backDateStreak      = static_cast<uint32_t>(next() % kBackDateSustainWindows);
            connection.backDateEpisodeOpen = (next() & 1u) != 0u;
            connection.backDateEpisodes   += 1u + static_cast<uint32_t>(next() % 3u);
        }
        if (m_windowStarted && m_windowTicks > 0)
        {
            m_windowStartTick -= draw(static_cast<uint32_t>(m_windowTicks), 0);
        }
    }

private:
    struct Owner
    {
        bool     sighted          = false;
        int32_t  anchor           = 0;
        uint64_t seen             = 0u;
        bool     bundleSighted    = false;
        int32_t  lastBundleNewest = 0;
    };

    struct Connection
    {
        std::vector<int32_t>                     margins;
        uint32_t                                 lateRefused = 0u;
        std::optional<InputArrivalWindowSummary> latest;

        std::vector<int32_t>               m0;
        uint32_t                           lateFirstSight  = 0u;
        uint32_t                           lateResent      = 0u;
        double                             m0Ewma          = 0.0;
        bool                               ewmaSeeded      = false;
        int32_t                            lastSampleDelay = 0;
        std::vector<int32_t>               stepsPerBundle;
        std::optional<ArrivalMarginSignal> latestSignal;

        uint32_t windowsWithSamples  = 0u;
        uint32_t backDateStreak      = 0u;
        bool     backDateEpisodeOpen = false;
        uint32_t backDateEpisodes    = 0u;
    };

    static int32_t nearestRank(const std::vector<int32_t>& sorted, uint32_t percent)
    {
        const uint64_t scaled = static_cast<uint64_t>(percent) * static_cast<uint64_t>(sorted.size());
        const uint64_t rank   = std::max<uint64_t>((scaled + 99u) / 100u, 1u);
        return sorted[static_cast<std::size_t>(rank - 1u)];
    }

    static int16_t quantizeQ4(double ticks)
    {
        const double scaled = std::clamp(ticks * kEwmaQ4Scale,
                                         static_cast<double>(std::numeric_limits<int16_t>::min()),
                                         static_cast<double>(std::numeric_limits<int16_t>::max()));
        return static_cast<int16_t>(std::lround(scaled));
    }

    void recordM0(Connection& connection, int32_t m0)
    {
        connection.m0.push_back(m0);
        if (!connection.ewmaSeeded)
        {
            connection.ewmaSeeded = true;
            connection.m0Ewma     = static_cast<double>(m0);
            return;
        }
        connection.m0Ewma += m_ewmaAlpha * (static_cast<double>(m0) - connection.m0Ewma);
    }

    static InputArrivalWindowSummary summarize(Connection& connection)
    {
        InputArrivalWindowSummary summary;
        summary.lateRefused = connection.lateRefused;
        if (connection.margins.empty())
        {
            return summary;
        }
        std::sort(connection.margins.begin(), connection.margins.end());
        summary.samples        = static_cast<uint32_t>(connection.margins.size());
        summary.marginP05Ticks = nearestRank(connection.margins, 5u);
        summary.marginP50Ticks = nearestRank(connection.margins, 50u);
        summary.marginP95Ticks = nearestRank(connection.margins, 95u);
        summary.marginMinTicks = connection.margins.front();
        return summary;
    }

    static ArrivalMarginSignal summarizeSignal(Connection& connection, int32_t serverTick)
    {
        ArrivalMarginSignal result;
        result.windowEndServerTick = serverTick;
        result.lateFirstSight      = connection.lateFirstSight;
        result.lateResent          = connection.lateResent;
        result.effectiveDelayTicks = connection.lastSampleDelay;
        result.m0EwmaQ4            = connection.ewmaSeeded ? quantizeQ4(connection.m0Ewma) : int16_t{ 0 };
        if (!connection.m0.empty())
        {
            std::sort(connection.m0.begin(), connection.m0.end());
            result.samples    = static_cast<uint32_t>(connection.m0.size());
            result.m0P05Ticks = nearestRank(connection.m0, 5u);
            result.m0P50Ticks = nearestRank(connection.m0, 50u);
        }
        if (!connection.stepsPerBundle.empty())
        {
            std::sort(connection.stepsPerBundle.begin(), connection.stepsPerBundle.end());
            result.stepsPerBundleP50 = nearestRank(connection.stepsPerBundle, 50u);
        }
        return result;
    }

    std::optional<ArrivalBackDateOpening> trackBackDate(Connection& connection, const ArrivalMarginSignal& windowSignal) const
    {
        if (windowSignal.samples == 0u)
        {
            return std::nullopt;
        }
        connection.windowsWithSamples = std::min(connection.windowsWithSamples + 1u, kBackDateGraceWindows + 1u);
        const bool below = windowSignal.m0P50Ticks < m_backDateThresholdTicks;
        if (connection.backDateEpisodeOpen)
        {
            if (!below)
            {
                connection.backDateEpisodeOpen = false;
                connection.backDateStreak      = 0u;
            }
            return std::nullopt;
        }
        if (connection.windowsWithSamples <= kBackDateGraceWindows)
        {
            return std::nullopt;
        }
        if (!below || windowSignal.samples < kBackDateMinSamples)
        {
            connection.backDateStreak = 0u;
            return std::nullopt;
        }
        ++connection.backDateStreak;
        if (connection.backDateStreak < kBackDateSustainWindows)
        {
            return std::nullopt;
        }
        connection.backDateEpisodeOpen = true;
        ++connection.backDateEpisodes;
        return ArrivalBackDateOpening{ windowSignal.m0P50Ticks, connection.backDateStreak, m_backDateThresholdTicks,
                                       windowSignal.effectiveDelayTicks };
    }

    int32_t                                  m_windowTicks     = 0;
    double                                   m_ewmaAlpha       = 1.0;
    int32_t                                  m_backDateThresholdTicks = 0;
    bool                                     m_windowStarted   = false;
    int32_t                                  m_windowStartTick = 0;
    std::unordered_map<Address, Connection>  m_connections;
    std::unordered_map<unsigned int, Owner>  m_owners;
};
