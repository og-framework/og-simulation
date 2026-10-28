#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SurfaceFrame-rationale.md · docs/SurfaceFrame-guards.md

#include <cmath>

#include "glm/geometric.hpp"
#include "glm/gtc/quaternion.hpp"
#include "glm/mat3x3.hpp"
#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"

#include "OGSimulation/OGAssert.h"
#include "OGSimulation/SimulationFrame.h"

namespace surfaceFrame
{
    inline constexpr float kFlatTolerance = 1.e-6f;

    constexpr bool isFlat(const glm::vec3& n)
    {
        const glm::vec3 across = simFrame::flatten(n);
        return glm::dot(across, across) <= kFlatTolerance * kFlatTolerance;
    }

    namespace detail
    {
        template <bool Inverse>
        glm::vec3 tilt(const glm::vec3& n, const glm::vec3& v)
        {
            // ⛔G-01  docs/SurfaceFrame-guards.md
            if (isFlat(n))
            {
                return v;
            }
            const float c = glm::dot(simFrame::kUp, n);
            OG_CHECK(c > 0.f, "surfaceFrame: the surface normal must point up, dot(kUp, n) > 0 (a walkable normal has dot(kUp, n) >= cos 45 deg)");
            glm::vec3 k = glm::cross(simFrame::kUp, n);
            if constexpr (Inverse)
            {
                k = -k;
            }
            return v + glm::cross(k, v) + glm::cross(k, glm::cross(k, v)) * (1.f / (1.f + c));
        }
    } // namespace detail

    inline glm::vec3 tiltToSurface(const glm::vec3& n, const glm::vec3& v)
    {
        return detail::tilt<false>(n, v);
    }

    inline glm::vec3 tiltFromSurface(const glm::vec3& n, const glm::vec3& v)
    {
        return detail::tilt<true>(n, v);
    }

    inline glm::mat3 tiltMatrix(const glm::vec3& n)
    {
        glm::mat3 out(1.f);
        for (glm::length_t column = 0; column < 3; ++column)
        {
            out[column] = tiltToSurface(n, out[column]);
        }
        return out;
    }

    constexpr glm::vec3 surfaceUpOf(const glm::mat4& pose)
    {
        const glm::vec3 column(pose[simFrame::kUpIndex].x, pose[simFrame::kUpIndex].y, pose[simFrame::kUpIndex].z);
        if constexpr (simFrameDetail::axisIsNegative(simFrame::kUp))
        {
            return -column;
        }
        else
        {
            return column;
        }
    }

    struct AngleAxis
    {
        float angle;
        glm::vec3 axis;
    };

    inline AngleAxis toAngleAxis(const glm::mat3& r)
    {
        glm::quat q = glm::normalize(glm::quat_cast(r));
        if (q.w < 0.f)
        {
            q = -q;
        }
        const glm::vec3 xyz(q.x, q.y, q.z);
        const float s = glm::length(xyz);
        // ⛔G-02  docs/SurfaceFrame-guards.md
        const float angle = 2.f * std::atan2(s, q.w);
        const glm::vec3 axis = s > 0.f ? xyz / s : simFrame::kUp;
        return AngleAxis{angle, axis};
    }
} // namespace surfaceFrame
