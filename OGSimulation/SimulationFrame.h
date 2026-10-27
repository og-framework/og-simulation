#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimulationFrame-rationale.md · docs/SimulationFrame-guards.md

#include <bit>
#include <cstdint>

#include "glm/geometric.hpp"
#include "glm/gtc/quaternion.hpp"
#include "glm/mat4x4.hpp"
#include "glm/vec2.hpp"
#include "glm/vec3.hpp"

namespace simFrameDetail
{
    constexpr glm::length_t axisIndex(const glm::vec3& axis)
    {
        return axis.x != 0.f ? 0 : (axis.y != 0.f ? 1 : 2);
    }

    constexpr bool axisIsNegative(const glm::vec3& axis)
    {
        return axis[axisIndex(axis)] < 0.f;
    }

    constexpr bool isSignedBasisAxis(const glm::vec3& axis)
    {
        const float component = axis[axisIndex(axis)];
        return (component == 1.f || component == -1.f) && glm::dot(axis, axis) == 1.f;
    }

    constexpr bool sameBits(float a, float b)
    {
        return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    }

    constexpr bool sameBits(const glm::vec2& a, const glm::vec2& b)
    {
        return sameBits(a.x, b.x) && sameBits(a.y, b.y);
    }

    constexpr bool sameBits(const glm::vec3& a, const glm::vec3& b)
    {
        return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.z, b.z);
    }

    constexpr glm::vec3 zeroProbe(const glm::vec3& axis, float zero)
    {
        glm::vec3 probe(1.f, 1.f, 1.f);
        probe[axisIndex(axis)] = zero;
        return probe;
    }

    constexpr float expectedPick(const glm::vec3& axis, float zero)
    {
        return axisIsNegative(axis) ? -zero : zero;
    }

    template <class Frame>
    constexpr glm::mat4 zeroForwardColumnProbe(float zero)
    {
        const glm::length_t index = axisIndex(Frame::kForward);
        const glm::vec4 zeros(zero, zero, zero, 1.f);
        const glm::vec4 ones(1.f, 1.f, 1.f, 1.f);
        return glm::mat4(index == 0 ? zeros : ones, index == 1 ? zeros : ones, index == 2 ? zeros : ones, ones);
    }

    template <class Frame>
    constexpr bool picksKeepTheSignOfZero(float zero)
    {
        const glm::vec3 alongForward = zeroProbe(Frame::kForward, zero);
        const glm::vec3 alongRight = zeroProbe(Frame::kRight, zero);
        const glm::vec3 alongLeft = zeroProbe(Frame::kLeft, zero);
        const glm::vec3 alongUp = zeroProbe(Frame::kUp, zero);
        const glm::vec2 zeroFirst(zero, 1.f);
        const glm::vec2 zeroSecond(1.f, zero);
        const float forwardZero = expectedPick(Frame::kForward, zero);
        return sameBits(Frame::height(alongUp), expectedPick(Frame::kUp, zero))
            && sameBits(Frame::forwardColumn(zeroForwardColumnProbe<Frame>(zero)), glm::vec3(forwardZero, forwardZero, forwardZero))
            && sameBits(Frame::planar(alongForward).x, expectedPick(Frame::kForward, zero))
            && sameBits(Frame::planar(alongLeft).y, expectedPick(Frame::kLeft, zero))
            && sameBits(Frame::forwardRight(alongForward).x, expectedPick(Frame::kForward, zero))
            && sameBits(Frame::forwardRight(alongRight).y, expectedPick(Frame::kRight, zero))
            && sameBits(Frame::turnComponent(zeroProbe(Frame::kTurnAxis, zero)), expectedPick(Frame::kTurnAxis, zero))
            && sameBits(Frame::alongUp(alongUp), zero)
            && sameBits(Frame::height(Frame::withHeight(glm::vec3(1.f, 1.f, 1.f), zero)), zero)
            && sameBits(Frame::planar(Frame::compose(zeroFirst, 1.f)), zeroFirst)
            && sameBits(Frame::planar(Frame::compose(zeroSecond, 1.f)), zeroSecond)
            && sameBits(Frame::height(Frame::compose(glm::vec2(1.f, 1.f), zero)), zero)
            && sameBits(Frame::forwardRight(Frame::fromForwardRight(zeroFirst, 1.f)), zeroFirst)
            && sameBits(Frame::forwardRight(Frame::fromForwardRight(zeroSecond, 1.f)), zeroSecond)
            && sameBits(Frame::height(Frame::fromForwardRight(glm::vec2(1.f, 1.f), zero)), zero)
            && sameBits(Frame::planar(Frame::flatten(alongForward)), Frame::planar(alongForward))
            && sameBits(Frame::planar(Frame::flatten(alongLeft)), Frame::planar(alongLeft))
            && sameBits(Frame::height(Frame::flatten(alongUp)), 0.f);
    }
} // namespace simFrameDetail

struct simFrame
{
    static constexpr glm::vec3 kUp = glm::vec3(0.f, 1.f, 0.f);
    static constexpr glm::vec3 kForward = glm::vec3(0.f, 0.f, -1.f);
    static constexpr glm::vec3 kRight = glm::vec3(1.f, 0.f, 0.f);
    static constexpr glm::vec3 kLeft = glm::vec3(-1.f, 0.f, 0.f);
    static constexpr glm::vec3 kTurnAxis = glm::vec3(0.f, 1.f, 0.f);
    static constexpr glm::vec3 kVerticalTurnAxis = glm::vec3(1.f, 0.f, 0.f);
    static constexpr float kChartOrientation = 1.f;
    static constexpr glm::length_t kUpIndex = simFrameDetail::axisIndex(kUp);

    static constexpr float kUeCentimetresPerUnit = 100.f;

private:
    static constexpr glm::length_t kForwardIndex = simFrameDetail::axisIndex(kForward);
    static constexpr glm::length_t kRightIndex = simFrameDetail::axisIndex(kRight);
    static constexpr glm::length_t kLeftIndex = simFrameDetail::axisIndex(kLeft);
    static constexpr glm::length_t kTurnIndex = simFrameDetail::axisIndex(kTurnAxis);
    static constexpr bool kForwardNegative = simFrameDetail::axisIsNegative(kForward);
    static constexpr bool kRightNegative = simFrameDetail::axisIsNegative(kRight);
    static constexpr bool kLeftNegative = simFrameDetail::axisIsNegative(kLeft);
    static constexpr bool kUpNegative = simFrameDetail::axisIsNegative(kUp);
    static constexpr bool kTurnNegative = simFrameDetail::axisIsNegative(kTurnAxis);

    static_assert(kLeft == -kRight, "SimulationFrame: kLeft must be -kRight");
    static_assert(kTurnAxis == glm::cross(kForward, kLeft) && kTurnAxis == kUp,
        "SimulationFrame: kTurnAxis must be cross(kForward, kLeft), which is kUp: the chart is (forward, left) and a positive "
        "planar angle turns counter-clockwise about +Y. Was cross(kForward, kRight), -Y, under chart P "
        "(og-sim-metres-yup task 26)");
    static_assert(kVerticalTurnAxis == glm::cross(kForward, kUp) && kVerticalTurnAxis == kRight,
        "SimulationFrame: kVerticalTurnAxis must be cross(kForward, kUp), which is kRight. Was cross(kUp, kForward), -X, "
        "under chart P (og-sim-metres-yup task 26)");
    static_assert(kChartOrientation == glm::dot(glm::cross(kForward, kLeft), kUp) && kChartOrientation == 1.f,
        "SimulationFrame: kChartOrientation must be dot(cross(kForward, kLeft), kUp), which is +1 for the native chart. "
        "Was -1, dot(cross(kForward, kRight), kUp), under chart P (og-sim-metres-yup task 26)");
    static_assert(simFrameDetail::isSignedBasisAxis(kUp) && simFrameDetail::isSignedBasisAxis(kForward)
            && simFrameDetail::isSignedBasisAxis(kRight) && simFrameDetail::isSignedBasisAxis(kLeft)
            && simFrameDetail::isSignedBasisAxis(kTurnAxis),
        "SimulationFrame: every axis is a signed basis vector, so every pick is a signed index pick");
    static_assert(kForwardIndex != kRightIndex && kForwardIndex != kUpIndex && kRightIndex != kUpIndex
            && kLeftIndex == kRightIndex,
        "SimulationFrame: forward, right and up are three different components, and left is right's component");

public:
    static constexpr float height(const glm::vec3& v)
    {
        return signedPick<kUpIndex, kUpNegative>(v);
    }

    static constexpr glm::vec3 withHeight(const glm::vec3& v, float h)
    {
        glm::vec3 out = v;
        signedPlace<kUpIndex, kUpNegative>(out, h);
        return out;
    }

    static constexpr glm::vec2 planar(const glm::vec3& v)
    {
        return glm::vec2(signedPick<kForwardIndex, kForwardNegative>(v), signedPick<kLeftIndex, kLeftNegative>(v));
    }

    static constexpr glm::vec3 compose(const glm::vec2& p, float h)
    {
        return place<kLeftIndex, kForwardNegative, kLeftNegative, kUpNegative>(p.x, p.y, h);
    }

    static constexpr glm::vec3 flatten(const glm::vec3& v)
    {
        glm::vec3 out = v;
        out[kUpIndex] = 0.f;
        return out;
    }

    static constexpr float turnComponent(const glm::vec3& a)
    {
        return signedPick<kTurnIndex, kTurnNegative>(a);
    }

    static constexpr glm::vec2 forwardRight(const glm::vec3& v)
    {
        return glm::vec2(signedPick<kForwardIndex, kForwardNegative>(v), signedPick<kRightIndex, kRightNegative>(v));
    }

    static constexpr glm::vec3 fromForwardRight(const glm::vec2& p, float h)
    {
        return place<kRightIndex, kForwardNegative, kRightNegative, kUpNegative>(p.x, p.y, h);
    }

    static constexpr float alongUp(const glm::vec3& diagonal)
    {
        return diagonal[kUpIndex];
    }

    static constexpr glm::vec3 forwardColumn(const glm::mat4& m)
    {
        const glm::vec3 column(m[kForwardIndex].x, m[kForwardIndex].y, m[kForwardIndex].z);
        return glm::vec3(signedPick<0, kForwardNegative>(column), signedPick<1, kForwardNegative>(column),
            signedPick<2, kForwardNegative>(column));
    }

private:
    template <glm::length_t Index, bool Negate>
    static constexpr float signedPick(const glm::vec3& v)
    {
        // ⛔G-01  docs/SimulationFrame-guards.md
        if constexpr (Negate)
        {
            return -v[Index];
        }
        else
        {
            return v[Index];
        }
    }

    template <glm::length_t Index, bool Negate>
    static constexpr void signedPlace(glm::vec3& out, float value)
    {
        if constexpr (Negate)
        {
            out[Index] = -value;
        }
        else
        {
            out[Index] = value;
        }
    }

    template <glm::length_t SideIndex, bool NegateForward, bool NegateSide, bool NegateUp>
    static constexpr glm::vec3 place(float forward, float side, float up)
    {
        glm::vec3 out(0.f, 0.f, 0.f);
        signedPlace<kForwardIndex, NegateForward>(out, forward);
        signedPlace<SideIndex, NegateSide>(out, side);
        signedPlace<kUpIndex, NegateUp>(out, up);
        return out;
    }
};

static_assert(simFrameDetail::picksKeepTheSignOfZero<simFrame>(-0.f) && simFrameDetail::picksKeepTheSignOfZero<simFrame>(0.f),
    "SimulationFrame G-01: every pick is a signed index pick, never a dot product, so the sign of zero survives");

static_assert(simFrameDetail::sameBits(simFrame::kUp, glm::vec3(0.f, 1.f, 0.f))
        && simFrameDetail::sameBits(simFrame::kForward, glm::vec3(0.f, 0.f, -1.f))
        && simFrameDetail::sameBits(simFrame::kRight, glm::vec3(1.f, 0.f, 0.f))
        && simFrameDetail::sameBits(simFrame::kLeft, glm::vec3(-1.f, 0.f, 0.f))
        && simFrameDetail::sameBits(simFrame::kTurnAxis, glm::vec3(0.f, 1.f, 0.f))
        && simFrameDetail::sameBits(simFrame::kVerticalTurnAxis, glm::vec3(1.f, 0.f, 0.f))
        && simFrameDetail::sameBits(simFrame::kChartOrientation, 1.f) && simFrame::kUpIndex == 1,
    "SimulationFrame: the simulation's frame is fixed: right-handed, +Y up, -Z forward, +X right, the native chart "
    "(forward, left; turns counter-clockwise about +Y). Was the axis-frame knob at YUpRightHanded, removed together with "
    "the Unreal frame it selected against, and chart P (turns about -Y) until og-sim-metres-yup task 26");

static_assert(simFrameDetail::sameBits(simFrame::kUeCentimetresPerUnit, 100.f),
    "SimulationFrame: the simulation's length unit is fixed: the metre, 100 Unreal centimetres. "
    "Was the units-per-metre knob at 1, removed together with the centimetre unit it selected against");
