#pragma once
// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2021 Jorrit Rouwe
// docs/SimTrig-rationale.md

#include <bit>
#include <cmath>
#include <cstdint>

#include "glm/common.hpp"
#include "glm/geometric.hpp"
#include "glm/mat4x4.hpp"
#include "glm/vec2.hpp"
#include "glm/vec3.hpp"

namespace simTrig
{
    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kHalfPi = 0.5f * kPi;
    inline constexpr float kQuarterPi = 0.25f * kPi;

    struct SinCos
    {
        float sin;
        float cos;
    };

    namespace detail
    {
        inline constexpr std::uint32_t kSignBit = 0x80000000u;

        inline std::uint32_t signOf(float x)
        {
            return std::bit_cast<std::uint32_t>(x) & kSignBit;
        }

        inline float xorSign(float x, std::uint32_t sign)
        {
            return std::bit_cast<float>(std::bit_cast<std::uint32_t>(x) ^ sign);
        }

        inline std::uint32_t truncateToQuadrant(float value)
        {
            const std::int32_t quadrant = value < 2147483648.f ? static_cast<std::int32_t>(value) : INT32_MIN;
            return static_cast<std::uint32_t>(quadrant);
        }

        struct Reduced
        {
            float x;
            std::uint32_t quadrant;
        };

        inline Reduced reduceToQuadrant(float absX)
        {
            const std::uint32_t quadrant = truncateToQuadrant(0.6366197723675814f * absX + 0.5f);
            const float floatQuadrant = static_cast<float>(static_cast<std::int32_t>(quadrant));
            const float x = ((absX - floatQuadrant * 1.5703125f) - floatQuadrant * 0.0004837512969970703125f)
                - floatQuadrant * 7.549789948768648e-8f;
            return Reduced{x, quadrant};
        }
    } // namespace detail

    inline SinCos sinCos(float angle)
    {
        std::uint32_t sinSign = detail::signOf(angle);
        const detail::Reduced reduced = detail::reduceToQuadrant(detail::xorSign(angle, sinSign));
        const float x = reduced.x;
        const float x2 = x * x;

        const float taylorCos = ((2.443315711809948e-5f * x2 - 1.388731625493765e-3f) * x2 + 4.166664568298827e-2f) * x2 * x2
            - 0.5f * x2 + 1.0f;
        const float taylorSin = ((-1.9515295891e-4f * x2 + 8.3321608736e-3f) * x2 - 1.6666654611e-1f) * x2 * x + x;

        const std::uint32_t bit1 = reduced.quadrant << 31;
        const std::uint32_t bit2 = (reduced.quadrant << 30) & detail::kSignBit;

        const float s = bit1 != 0u ? taylorCos : taylorSin;
        const float c = bit1 != 0u ? taylorSin : taylorCos;

        sinSign ^= bit2;
        const std::uint32_t cosSign = bit1 ^ bit2;
        return SinCos{detail::xorSign(s, sinSign), detail::xorSign(c, cosSign)};
    }

    inline float sin(float angle)
    {
        return sinCos(angle).sin;
    }

    inline float cos(float angle)
    {
        return sinCos(angle).cos;
    }

    inline float tan(float angle)
    {
        const std::uint32_t tanSign = detail::signOf(angle);
        const detail::Reduced reduced = detail::reduceToQuadrant(detail::xorSign(angle, tanSign));
        const float x = reduced.x;
        const float x2 = x * x;

        float t = (((((9.38540185543e-3f * x2 + 3.11992232697e-3f) * x2 + 2.44301354525e-2f) * x2 + 5.34112807005e-2f) * x2
            + 1.33387994085e-1f) * x2 + 3.33331568548e-1f) * x2 * x + x;

        if ((reduced.quadrant << 31) != 0u)
        {
            t = -1.0f / t;
        }
        return detail::xorSign(t, tanSign);
    }

    inline float asin(float value)
    {
        const std::uint32_t asinSign = detail::signOf(value);
        float a = detail::xorSign(value, asinSign);
        a = a < 1.0f ? a : 1.0f;

        const bool greater = a > 0.5f;
        const float z2 = 0.5f * (1.0f - a);
        const float z = greater ? z2 : a * a;
        const float x = greater ? std::sqrt(z2) : a;

        float r = ((((4.2163199048e-2f * z + 2.4181311049e-2f) * z + 4.5470025998e-2f) * z + 7.4953002686e-2f) * z
            + 1.6666752422e-1f) * z * x + x;
        r = greater ? kHalfPi - (r + r) : r;
        return detail::xorSign(r, asinSign);
    }

    inline float acos(float value)
    {
        return kHalfPi - asin(value);
    }

    inline float atan(float value)
    {
        const std::uint32_t atanSign = detail::signOf(value);
        const float absValue = detail::xorSign(value, atanSign);
        float x = absValue;
        float y = 0.0f;

        if (absValue > 0.4142135623730950f)
        {
            x = (absValue - 1.0f) / (absValue + 1.0f);
            y = kQuarterPi;
        }
        if (absValue > 2.414213562373095f)
        {
            x = -1.0f / absValue;
            y = kHalfPi;
        }

        const float z = x * x;
        y = y + ((((8.05374449538e-2f * z - 1.38776856032e-1f) * z + 1.99777106478e-1f) * z - 3.33329491539e-1f) * z * x + x);
        return detail::xorSign(y, atanSign);
    }

    inline float atan2(float y, float x)
    {
        const std::uint32_t ySign = detail::signOf(y);
        const float yAbs = detail::xorSign(y, ySign);
        const std::uint32_t xSign = detail::signOf(x);
        const float xAbs = detail::xorSign(x, xSign);

        const bool xIsNumerator = xAbs < yAbs;
        const float numerator = xIsNumerator ? xAbs : yAbs;
        const float denominator = xIsNumerator ? yAbs : xAbs;
        float angle = atan(numerator / denominator);

        angle = xIsNumerator ? kHalfPi - angle : angle;
        angle = angle - (xSign != 0u ? kPi : 0.0f);
        return detail::xorSign(angle, xSign ^ ySign);
    }

    inline glm::mat4 rotate(const glm::mat4& m, float angle, const glm::vec3& v)
    {
        const SinCos sc = sinCos(angle);
        const float c = sc.cos;
        const float s = sc.sin;

        const glm::vec3 axis(glm::normalize(v));
        const glm::vec3 temp((1.0f - c) * axis);

        glm::mat4 rotation(1.0f);
        rotation[0][0] = c + temp[0] * axis[0];
        rotation[0][1] = temp[0] * axis[1] + s * axis[2];
        rotation[0][2] = temp[0] * axis[2] - s * axis[1];

        rotation[1][0] = temp[1] * axis[0] - s * axis[2];
        rotation[1][1] = c + temp[1] * axis[1];
        rotation[1][2] = temp[1] * axis[2] + s * axis[0];

        rotation[2][0] = temp[2] * axis[0] + s * axis[1];
        rotation[2][1] = temp[2] * axis[1] - s * axis[0];
        rotation[2][2] = c + temp[2] * axis[2];

        glm::mat4 result;
        result[0] = m[0] * rotation[0][0] + m[1] * rotation[0][1] + m[2] * rotation[0][2];
        result[1] = m[0] * rotation[1][0] + m[1] * rotation[1][1] + m[2] * rotation[1][2];
        result[2] = m[0] * rotation[2][0] + m[1] * rotation[2][1] + m[2] * rotation[2][2];
        result[3] = m[3];
        return result;
    }

    inline float orientedAngle(const glm::vec2& from, const glm::vec2& to)
    {
        const float angle = acos(glm::clamp(glm::dot(from, to), -1.0f, 1.0f));
        const float partialCross = from.x * to.y - to.x * from.y;
        return partialCross > 0.0f ? angle : -angle;
    }
} // namespace simTrig
