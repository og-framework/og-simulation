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

enum class AxisFrame : uint8_t
{
    UeZUpLeftHanded = 0,
    YUpRightHanded = 1,
};

inline constexpr AxisFrame kAxisFrame = AxisFrame::YUpRightHanded;
inline constexpr float kUnitsPerMetre = 1.f;

static_assert(kUnitsPerMetre == 100.f || kUnitsPerMetre == 1.f,
    "SimulationFrame: kUnitsPerMetre is 100 (centimetres) or 1 (metres); the legacy wrappers know no other unit");

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
        const glm::vec3 alongUp = zeroProbe(Frame::kUp, zero);
        const glm::vec2 zeroFirst(zero, 1.f);
        const glm::vec2 zeroSecond(1.f, zero);
        const float forwardZero = expectedPick(Frame::kForward, zero);
        return sameBits(Frame::height(alongUp), expectedPick(Frame::kUp, zero))
            && sameBits(Frame::forwardColumn(zeroForwardColumnProbe<Frame>(zero)), glm::vec3(forwardZero, forwardZero, forwardZero))
            && sameBits(Frame::planar(alongForward).x, expectedPick(Frame::kForward, zero))
            && sameBits(Frame::planar(alongRight).y, expectedPick(Frame::kRight, zero))
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
            && sameBits(Frame::planar(Frame::flatten(alongRight)), Frame::planar(alongRight))
            && sameBits(Frame::height(Frame::flatten(alongUp)), 0.f);
    }
} // namespace simFrameDetail

template <AxisFrame A, float U>
struct BasicSimFrame
{
    static_assert(A == AxisFrame::UeZUpLeftHanded || A == AxisFrame::YUpRightHanded,
        "SimulationFrame: BasicSimFrame knows exactly two axis frames");
    static_assert(U == 100.f || U == 1.f, "SimulationFrame: BasicSimFrame knows exactly two units, 100 or 1 per metre");

    static constexpr glm::vec3 kUp = A == AxisFrame::UeZUpLeftHanded ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, 1.f, 0.f);
    static constexpr glm::vec3 kForward = A == AxisFrame::UeZUpLeftHanded ? glm::vec3(1.f, 0.f, 0.f) : glm::vec3(0.f, 0.f, -1.f);
    static constexpr glm::vec3 kRight = A == AxisFrame::UeZUpLeftHanded ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(1.f, 0.f, 0.f);
    static constexpr glm::vec3 kTurnAxis = A == AxisFrame::UeZUpLeftHanded ? glm::vec3(0.f, 0.f, 1.f) : glm::vec3(0.f, -1.f, 0.f);
    static constexpr glm::vec3 kVerticalTurnAxis = A == AxisFrame::UeZUpLeftHanded ? glm::vec3(0.f, 1.f, 0.f) : glm::vec3(-1.f, 0.f, 0.f);
    static constexpr float kChartOrientation = A == AxisFrame::UeZUpLeftHanded ? 1.f : -1.f;
    static constexpr glm::length_t kUpIndex = simFrameDetail::axisIndex(kUp);

    static constexpr float kUnitsPerCentimetre = U / 100.f;
    static constexpr float kUeCentimetresPerUnit = 100.f / U;

    static constexpr const char* kLengthFieldSuffix = U == 100.f ? "Cm" : "M";

private:
    static constexpr bool kIsCentimetres = U == 100.f;
    static constexpr glm::length_t kForwardIndex = simFrameDetail::axisIndex(kForward);
    static constexpr glm::length_t kRightIndex = simFrameDetail::axisIndex(kRight);
    static constexpr glm::length_t kTurnIndex = simFrameDetail::axisIndex(kTurnAxis);
    static constexpr bool kForwardNegative = simFrameDetail::axisIsNegative(kForward);
    static constexpr bool kRightNegative = simFrameDetail::axisIsNegative(kRight);
    static constexpr bool kUpNegative = simFrameDetail::axisIsNegative(kUp);
    static constexpr bool kTurnNegative = simFrameDetail::axisIsNegative(kTurnAxis);
    static constexpr bool kChartNegative = kChartOrientation < 0.f;

    static_assert(kTurnAxis == glm::cross(kForward, kRight), "SimulationFrame: kTurnAxis must be cross(kForward, kRight)");
    static_assert(kVerticalTurnAxis == glm::cross(kUp, kForward),
        "SimulationFrame: kVerticalTurnAxis must be cross(kUp, kForward)");
    static_assert(kChartOrientation == glm::dot(glm::cross(kForward, kRight), kUp),
        "SimulationFrame: kChartOrientation must be dot(cross(kForward, kRight), kUp)");
    static_assert(simFrameDetail::isSignedBasisAxis(kUp) && simFrameDetail::isSignedBasisAxis(kForward)
            && simFrameDetail::isSignedBasisAxis(kRight) && simFrameDetail::isSignedBasisAxis(kTurnAxis),
        "SimulationFrame: every axis is a signed basis vector, so every pick is a signed index pick");
    static_assert(kForwardIndex != kRightIndex && kForwardIndex != kUpIndex && kRightIndex != kUpIndex,
        "SimulationFrame: forward, right and up are three different components");

public:
    static constexpr int lengthDecimals(int n)
    {
        return kIsCentimetres ? n : n + 2;
    }

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
        return glm::vec2(signedPick<kForwardIndex, kForwardNegative>(v), signedPick<kRightIndex, kRightNegative>(v));
    }

    static constexpr glm::vec3 compose(const glm::vec2& p, float h)
    {
        return place<kForwardNegative, kRightNegative, kUpNegative>(p.x, p.y, h);
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
        return place<kForwardNegative, kRightNegative, kUpNegative>(p.x, p.y, h);
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

    static constexpr float legacyCm(float cm)
    {
        if constexpr (kIsCentimetres)
        {
            return cm;
        }
        else
        {
            return narrowToFloat(double(cm) / 100.0);
        }
    }

    static constexpr double legacyCm(double cm)
    {
        if constexpr (kIsCentimetres)
        {
            return cm;
        }
        else
        {
            return cm / 100.0;
        }
    }

    static constexpr float legacyCm2(float cm2)
    {
        if constexpr (kIsCentimetres)
        {
            return cm2;
        }
        else
        {
            return narrowToFloat(double(cm2) / 10000.0);
        }
    }

    static constexpr float legacyPerCm3(float perCm3)
    {
        if constexpr (kIsCentimetres)
        {
            return perCm3;
        }
        else
        {
            return narrowToFloat(double(perCm3) * 1000000.0);
        }
    }

    static constexpr glm::vec2 legacyPlanarCm(float x, float y)
    {
        return glm::vec2(legacyCm(x), legacyCm(y));
    }

    static constexpr glm::vec3 legacyAxes(float x, float y, float z)
    {
        return place<kForwardNegative, kRightNegative, kUpNegative>(x, y, z);
    }

    static constexpr glm::vec3 legacyAxesUnsigned(float x, float y, float z)
    {
        return place<false, false, false>(x, y, z);
    }

    static constexpr glm::vec3 legacyAxial(float x, float y, float z)
    {
        return place<kForwardNegative != kChartNegative, kRightNegative != kChartNegative, kUpNegative != kChartNegative>(x, y, z);
    }

    static constexpr glm::vec3 legacyPosition(float x, float y, float z)
    {
        return legacyAxes(legacyCm(x), legacyCm(y), legacyCm(z));
    }

    static constexpr glm::vec3 legacyExtents(float x, float y, float z)
    {
        return legacyAxesUnsigned(legacyCm(x), legacyCm(y), legacyCm(z));
    }

    static constexpr glm::vec3 legacyTorque(float x, float y, float z)
    {
        return legacyAxial(legacyCm2(x), legacyCm2(y), legacyCm2(z));
    }

    static constexpr glm::vec3 legacyInertia(float ixx, float iyy, float izz)
    {
        return legacyAxesUnsigned(legacyCm2(ixx), legacyCm2(iyy), legacyCm2(izz));
    }

    static constexpr glm::quat legacyRotation(float w, float x, float y, float z)
    {
        const glm::vec3 axial = legacyAxial(x, y, z);
        return glm::quat(w, axial.x, axial.y, axial.z);
    }

    static constexpr double toLegacyCm(double units)
    {
        if constexpr (kIsCentimetres)
        {
            return units;
        }
        else
        {
            return units * 100.0;
        }
    }

    static constexpr double toLegacyCm(float units)
    {
        return toLegacyCm(double(units));
    }

    static constexpr glm::dvec3 toLegacyAxes(const glm::vec3& v)
    {
        return glm::dvec3(signedPick<kForwardIndex, kForwardNegative>(v), signedPick<kRightIndex, kRightNegative>(v),
            signedPick<kUpIndex, kUpNegative>(v));
    }

    static constexpr glm::dvec3 toLegacyAxial(const glm::vec3& v)
    {
        return glm::dvec3(signedPick<kForwardIndex, kForwardNegative != kChartNegative>(v),
            signedPick<kRightIndex, kRightNegative != kChartNegative>(v), signedPick<kUpIndex, kUpNegative != kChartNegative>(v));
    }

    static constexpr glm::dvec3 toLegacyPosition(const glm::vec3& p)
    {
        const glm::dvec3 axes = toLegacyAxes(p);
        return glm::dvec3(toLegacyCm(axes.x), toLegacyCm(axes.y), toLegacyCm(axes.z));
    }

    static constexpr glm::dquat toLegacyRotation(const glm::quat& q)
    {
        const glm::dvec3 axial = toLegacyAxial(glm::vec3(q.x, q.y, q.z));
        return glm::dquat(double(q.w), axial.x, axial.y, axial.z);
    }

    static constexpr glm::dvec2 toLegacyPlanar(const glm::vec2& p)
    {
        return glm::dvec2(double(p.x), double(p.y));
    }

private:
    static constexpr float narrowToFloat(double value)
    {
        return std::bit_cast<float>(std::bit_cast<std::uint32_t>(static_cast<float>(value)));
    }

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

    template <bool NegateForward, bool NegateRight, bool NegateUp>
    static constexpr glm::vec3 place(float forward, float right, float up)
    {
        glm::vec3 out(0.f, 0.f, 0.f);
        signedPlace<kForwardIndex, NegateForward>(out, forward);
        signedPlace<kRightIndex, NegateRight>(out, right);
        signedPlace<kUpIndex, NegateUp>(out, up);
        return out;
    }
};

using simFrame = BasicSimFrame<kAxisFrame, kUnitsPerMetre>;

static_assert(simFrameDetail::picksKeepTheSignOfZero<simFrame>(-0.f) && simFrameDetail::picksKeepTheSignOfZero<simFrame>(0.f),
    "SimulationFrame G-01: every pick is a signed index pick, never a dot product, so the sign of zero survives");

static_assert(BasicSimFrame<kAxisFrame, 1.f>::legacyCm(30.f) == 0.3f && BasicSimFrame<kAxisFrame, 1.f>::legacyCm(42.f) == 0.42f,
    "SimulationFrame: a metre wrapper narrows through narrowToFloat, so constant evaluation and run time agree on its float");
