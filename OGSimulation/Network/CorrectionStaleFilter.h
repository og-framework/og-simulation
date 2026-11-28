#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/CorrectionStaleFilter-rationale.md · docs/CorrectionStaleFilter-guards.md

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

enum class StaleTickVerdict : std::uint8_t
{
    Admitted,
    Stale,
    Duplicate,
};

namespace staleTick
{
    constexpr StaleTickVerdict admit(std::optional<std::uint32_t>& newest, std::uint32_t tick)
    {
        // ⛔G-01  docs/CorrectionStaleFilter-guards.md
        if (newest.has_value() && tick <= *newest)
        {
            return tick == *newest ? StaleTickVerdict::Duplicate : StaleTickVerdict::Stale;
        }
        newest = tick;
        return StaleTickVerdict::Admitted;
    }

    struct DropCounts
    {
        std::uint64_t stale     = 0u;
        std::uint64_t duplicate = 0u;

        constexpr bool count(StaleTickVerdict verdict)
        {
            switch (verdict)
            {
            case StaleTickVerdict::Admitted:  return true;
            case StaleTickVerdict::Stale:     ++stale;     return false;
            case StaleTickVerdict::Duplicate: ++duplicate; return false;
            }
            return false;
        }
    };
}

class NewestTickFilter
{
public:
    constexpr bool admit(std::uint32_t tick) { return m_drops.count(staleTick::admit(m_newest, tick)); }

    constexpr void forget() { m_newest.reset(); }

    constexpr std::optional<std::uint32_t> newest() const { return m_newest; }
    constexpr std::uint64_t staleDropped() const { return m_drops.stale; }
    constexpr std::uint64_t duplicateDropped() const { return m_drops.duplicate; }

private:
    std::optional<std::uint32_t> m_newest;
    staleTick::DropCounts        m_drops;
};

class CorrectionStaleFilter
{
public:
    static constexpr std::size_t kSimIdCount = std::size_t{std::numeric_limits<std::uint8_t>::max()} + 1u;

    constexpr bool admit(std::uint8_t simId, std::uint32_t tick)
    {
        return m_drops.count(staleTick::admit(m_newest[simId], tick));
    }

    constexpr void forget(std::uint8_t simId) { m_newest[simId].reset(); }

    constexpr std::optional<std::uint32_t> newest(std::uint8_t simId) const { return m_newest[simId]; }
    constexpr std::uint64_t staleDropped() const { return m_drops.stale; }
    constexpr std::uint64_t duplicateDropped() const { return m_drops.duplicate; }

private:
    std::array<std::optional<std::uint32_t>, kSimIdCount> m_newest{};
    staleTick::DropCounts                                 m_drops;
};
