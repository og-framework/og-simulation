#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/ClockDilationController-rationale.md · docs/ClockDilationController-guards.md

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "OGSimulation/OGAssert.h"
#include "OGSimulation/PCTimeManagement/ClientPredictionClock.h"
#include "OGSimulation/PCTimeManagement/TimeConfig.h"
#include "OGSimulation/TickDriverConcepts.h"
#include "OGSimulation/TickDriverLog.h"

enum class ClockDilationMode : uint8_t { Off, Shadow, On };

enum class ClockDilationSetpoint : uint8_t { OwnRtt, Common };

enum class DilationFreeze : uint8_t { None, ModeOff, SensorInvalid, Recentre, TransitionDebt, AwaitFirstRtt, AfterResync };

struct ClockDilationConfig
{
    ClockDilationMode     mode                     = ClockDilationMode::Off;
    ClockDilationSetpoint setpoint                 = ClockDilationSetpoint::OwnRtt;
    double                gainPerTick              = 0.005;
    double                maxDeviation             = 0.01;
    double                slewPerCall              = 0.002;
    double                quantum                  = 1e-4;
    double                changeHysteresisQuanta   = 2.0;
    double                targetTauSeconds         = 10.0;
    double                guardTauSeconds          = 2.0;
    double                guardLowTicks            = -0.5;
    double                guardHighTicks           = 2.5;
    double                freezeAfterResyncSeconds = 1.0;
    double                commonLeadTicks          = 3.65;
    double                targetOffsetTicks        = 0.0;
};

namespace clockDilation
{
inline constexpr double   kMaxFrameTicks   = 3.0;
inline constexpr uint32_t kFramePeakFrames = 4u;
inline constexpr uint32_t kFramePeakWindow = 8u;
} // namespace clockDilation

static_assert(clockDilation::kFramePeakFrames >= 1u && clockDilation::kFramePeakFrames <= clockDilation::kFramePeakWindow,
    "clockDilation::kFramePeakFrames: a frame period enters the frame peak once this many of the last kFramePeakWindow "
    "frames reach it, so it must be a count inside the window (rationale section 5.1)");
static_assert(clockDilation::kFramePeakFrames <= TimeConfig{}.gradualCorrectionRate,
    "clockDilation::kFramePeakFrames: a run of fewer long frames than this never enters the frame peak. Each long frame "
    "raises at most one clock read (its first advance), so such a run alone must stay below the clock's graduated count, "
    "gradualCorrectionRate out-of-band reads per correction (rationale section 5.1)");

static_assert(ClockDilationConfig{}.maxDeviation < 0.2,
    "ClockDilationConfig: the bracket bound. A render snapshot pair brackets the render time while "
    "(1 + maxDeviation) / (1 - maxDeviation) < 1.5, i.e. maxDeviation < 0.2 (rationale section 7).");
static_assert(1.0 - ClockDilationConfig{}.guardLowTicks + 1.0 + 0.5 <= TimeConfig{}.softDriftThresholdTicks,
    "ClockDilationConfig: the band guard's low edge caps the clock's mean first-advance read at 1 - guardLowTicks; "
    "one offset quantum (1 tick) plus the age-and-phase spread (0.5 tick) above that cap must stay inside the clock's "
    "quiet band, softDriftThresholdTicks (rationale section 5). Was the low-edge assert "
    "guardLowTicks >= 1 - softDriftThresholdTicks + 0.5, measured from the guard centre alone");
static_assert(clockDilation::kMaxFrameTicks + 0.5 - 1.0 + ClockDilationConfig{}.guardLowTicks
                  <= ClockDilationConfig{}.guardHighTicks,
    "ClockDilationConfig: the frame-aware low edge at the longest sampled frame, guard centre + "
    "(kMaxFrameTicks + 0.5 - 1) + guardLowTicks, must not cross the high edge, guard centre + guardHighTicks "
    "(rationale section 5).");
static_assert(ClockDilationConfig{}.guardHighTicks <= 1.0 + TimeConfig{}.softDriftThresholdTicks - 0.5,
    "ClockDilationConfig: the band guard's high edge must sit half a tick inside the clock's quiet band, "
    "whose high edge is 1 + softDriftThresholdTicks (rationale section 5).");
static_assert(ClockDilationConfig{}.guardLowTicks < ClockDilationConfig{}.guardHighTicks,
    "ClockDilationConfig: the band guard's interval must not be empty (rationale section 5).");
static_assert(ClockDilationConfig{}.quantum > 0.0,
    "ClockDilationConfig: the scale is rounded to a multiple of quantum, which must be positive (rationale section 6).");
static_assert(ClockDilationConfig{}.slewPerCall > 0.0
                  && ClockDilationConfig{}.slewPerCall <= ClockDilationConfig{}.maxDeviation,
    "ClockDilationConfig: one call's slew must move the scale and must not exceed the whole deviation "
    "(rationale section 6).");
static_assert(ClockDilationConfig{}.changeHysteresisQuanta >= 0.5
                  && ClockDilationConfig{}.changeHysteresisQuanta * ClockDilationConfig{}.quantum
                         <= ClockDilationConfig{}.slewPerCall,
    "ClockDilationConfig: the scale holds until the slewed value is changeHysteresisQuanta quanta away. Below half a "
    "quantum the rounding already holds it, so the rule would do nothing; above one slew step a full slew could never "
    "move the scale (rationale section 6).");

struct ClockDilationInputs
{
    double               clockNowSeconds        = 0.0;
    double               predictionTick         = 0.0;
    bool                 sensorValid            = false;
    double               serverTick             = 0.0;
    double               smoothedRttSeconds     = 0.0;
    double               smoothedJitterSeconds  = 0.0;
    double               jitterMultiplier       = 0.0;
    uint32_t             predOffsetFloorTicks   = 0u;
    double               tickFrequency          = 0.0;
    uint32_t             authorityTick          = 0u;
    uint32_t             predictionOffsetTicks  = 0u;
    ClockCorrectionPhase phase                  = {};
    ClockTargetRead      clockRead              = {};
    double               currentScale           = 1.0;
};

struct ClockDilationOutput
{
    double         scale       = 1.0;
    bool           apply       = false;
    double         leadTicks   = 0.0;
    double         targetTicks = 0.0;
    double         errorTicks  = 0.0;
    DilationFreeze freeze      = DilationFreeze::None;
    bool           guardBound  = false;
    double         stepLagTicks = 0.0;
    double         guardCentreTicks = 0.0;
};

class ClockDilationController
{
public:
    explicit ClockDilationController(const ClockDilationConfig& config = {})
        : m_config(config)
    {
    }

    ClockDilationOutput update(const ClockDilationInputs& in)
    {
        ClockDilationOutput out;
        if (m_config.mode == ClockDilationMode::Off)
        {
            out.freeze = DilationFreeze::ModeOff;
            return out;
        }

        noteHardResyncs(in);
        noteFrame(in);

        const double rawTarget = rawTargetTicks(in);
        if (in.sensorValid)
            filterTargetAndGuard(in, rawTarget);

        const double lag = meanStepLagTicks();
        out.leadTicks    = in.predictionTick - in.serverTick;
        out.stepLagTicks = lag;
        out.targetTicks  = rawTarget + lag;
        if (m_filtersSeeded)
        {
            const double low    = m_guardTicks + (meanFirstReadPhaseTicks() - 1.0) + m_config.guardLowTicks;
            const double high   = m_guardTicks + m_config.guardHighTicks;
            const double wanted = m_targetTicks + lag;
            out.targetTicks     = std::clamp(wanted, low, high);
            out.guardBound      = out.targetTicks != wanted;
            out.guardCentreTicks = m_guardTicks;
        }
        out.errorTicks = out.leadTicks - out.targetTicks;
        out.freeze     = freezeCause(in);

        const bool frozen = out.freeze != DilationFreeze::None;
        // ⛔G-01  docs/ClockDilationController-guards.md
        const double correction = m_config.gainPerTick * out.errorTicks;
        // ⛔G-02  docs/ClockDilationController-guards.md
        const double desired = frozen ? 1.0 : 1.0 - std::clamp(correction, -m_config.maxDeviation, m_config.maxDeviation);

        // ⛔G-04  docs/ClockDilationController-guards.md
        const double slewed = m_scale + std::clamp(desired - m_scale, -m_config.slewPerCall, m_config.slewPerCall);
        // ⛔G-07  docs/ClockDilationController-guards.md
        if (frozen || std::abs(slewed - m_scale) >= m_config.changeHysteresisQuanta * m_config.quantum)
            m_scale = std::round(slewed / m_config.quantum) * m_config.quantum;
        notePreviousCall(in);

        out.scale = m_scale;
        out.apply = m_config.mode == ClockDilationMode::On && m_scale != in.currentScale;
        return out;
    }

    double scale() const { return m_scale; }

    const ClockDilationConfig& config() const { return m_config; }

private:
    double rawTargetTicks(const ClockDilationInputs& in) const
    {
        if (m_config.setpoint == ClockDilationSetpoint::Common)
        {
            // ⛔G-09  docs/ClockDilationController-guards.md
            return m_config.commonLeadTicks + m_config.targetOffsetTicks;
        }

        const double f          = in.tickFrequency;
        const double floorTicks = static_cast<double>(in.predOffsetFloorTicks);
        const double margin     = (in.smoothedRttSeconds + in.jitterMultiplier * in.smoothedJitterSeconds) * f;
        const double halfRtt    = (in.smoothedRttSeconds / 2.0) * f;
        // ⛔G-03  docs/ClockDilationController-guards.md
        return std::max(floorTicks, margin + 0.5) - halfRtt - 0.5 + m_config.targetOffsetTicks;
    }

    std::optional<double> guardSample(const ClockDilationInputs& in) const
    {
        if (!m_hasPreviousCall || !m_previousSensorValid || in.clockRead.advances == m_previousReadAdvances)
            return std::nullopt;
        // ∴D-03  docs/ClockDilationController-rationale.md
        return static_cast<double>(in.clockRead.targetTick) - m_previousServerTick;
    }

    void filterTargetAndGuard(const ClockDilationInputs& in, double rawTarget)
    {
        const std::optional<double> sample = guardSample(in);
        if (!m_filtersSeeded)
        {
            m_targetTicks      = rawTarget;
            m_guardTicks       = sample.value_or(static_cast<double>(in.predictionOffsetTicks)
                                                 - (in.serverTick - static_cast<double>(in.authorityTick)));
            m_lastGuardSeconds = in.clockNowSeconds;
            m_filtersSeeded    = true;
        }
        else
        {
            const double dt = std::max(0.0, in.clockNowSeconds - m_lastFilterSeconds);
            m_targetTicks += (1.0 - std::exp(-dt / m_config.targetTauSeconds)) * (rawTarget - m_targetTicks);
            if (sample.has_value())
            {
                const double guardDt = std::max(0.0, in.clockNowSeconds - m_lastGuardSeconds);
                m_guardTicks += (1.0 - std::exp(-guardDt / m_config.guardTauSeconds)) * (*sample - m_guardTicks);
                m_lastGuardSeconds = in.clockNowSeconds;
            }
        }
        m_lastFilterSeconds = in.clockNowSeconds;
    }

    void notePreviousCall(const ClockDilationInputs& in)
    {
        m_hasPreviousCall      = true;
        m_previousSensorValid  = in.sensorValid;
        m_previousServerTick   = in.serverTick;
        m_previousReadAdvances = in.clockRead.advances;
    }

    void noteHardResyncs(const ClockDilationInputs& in)
    {
        if (in.phase.hardResyncs == m_hardResyncs)
            return;
        m_hardResyncs       = in.phase.hardResyncs;
        m_hardResyncSeconds = in.clockNowSeconds;
        m_sawHardResync     = true;
    }

    void noteFrame(const ClockDilationInputs& in)
    {
        if (m_hasLastCall)
        {
            const double frameTicks = (in.clockNowSeconds - m_lastCallSeconds) * in.tickFrequency;
            // ⛔G-06  docs/ClockDilationController-guards.md
            if (frameTicks <= clockDilation::kMaxFrameTicks)
            {
                if (!m_frameSeeded)
                {
                    m_framePeakTicks = frameTicks;
                    m_frameMeanTicks = frameTicks;
                    m_recentFrameTicks.fill(frameTicks);
                    m_frameSeeded    = true;
                }
                else
                {
                    const double frameSeconds = frameTicks / in.tickFrequency;
                    const double gain         = 1.0 - std::exp(-frameSeconds / m_config.guardTauSeconds);
                    const double sustained    = sustainedFrameTicks(frameTicks);
                    m_framePeakTicks = std::max(sustained, m_framePeakTicks + gain * (sustained - m_framePeakTicks));
                    const double meanGain = 1.0 - std::exp(-frameSeconds / m_config.targetTauSeconds);
                    m_frameMeanTicks += meanGain * (frameTicks - m_frameMeanTicks);
                }
            }
        }
        m_hasLastCall     = true;
        m_lastCallSeconds = in.clockNowSeconds;
    }

    double sustainedFrameTicks(double frameTicks)
    {
        m_recentFrameTicks[m_nextRecentFrame] = frameTicks;
        m_nextRecentFrame = (m_nextRecentFrame + 1u) % clockDilation::kFramePeakWindow;
        std::array<double, clockDilation::kFramePeakWindow> ranked = m_recentFrameTicks;
        const auto reached = ranked.begin() + (clockDilation::kFramePeakWindow - clockDilation::kFramePeakFrames);
        std::nth_element(ranked.begin(), reached, ranked.end());
        // ⛔G-08  docs/ClockDilationController-guards.md
        return std::min(frameTicks, *reached);
    }

    double meanFirstReadPhaseTicks() const
    {
        // ∴D-01  docs/ClockDilationController-rationale.md
        return m_framePeakTicks >= 1.0 ? m_framePeakTicks + 0.5 : 1.0 + m_framePeakTicks / 2.0;
    }

    double meanStepLagTicks() const
    {
        // ∴D-02  docs/ClockDilationController-rationale.md
        return m_frameSeeded ? m_frameMeanTicks / 2.0 : 0.0;
    }

    DilationFreeze freezeCause(const ClockDilationInputs& in) const
    {
        if (!in.sensorValid)
            return DilationFreeze::SensorInvalid;
        if (in.phase.recentreArmed || in.phase.recentring)
            return DilationFreeze::Recentre;
        if (in.phase.transitionDebt)
            return DilationFreeze::TransitionDebt;
        if (in.phase.awaitsFirstRtt)
            return DilationFreeze::AwaitFirstRtt;
        if (m_sawHardResync && in.clockNowSeconds - m_hardResyncSeconds < m_config.freezeAfterResyncSeconds)
            return DilationFreeze::AfterResync;
        return DilationFreeze::None;
    }

    ClockDilationConfig m_config;
    double              m_scale             = 1.0;
    bool                m_filtersSeeded     = false;
    double              m_targetTicks       = 0.0;
    double              m_guardTicks        = 0.0;
    double              m_lastFilterSeconds = 0.0;
    uint32_t            m_hardResyncs       = 0u;
    double              m_hardResyncSeconds = 0.0;
    bool                m_sawHardResync     = false;
    bool                m_hasLastCall       = false;
    double              m_lastCallSeconds   = 0.0;
    bool                m_frameSeeded       = false;
    double              m_framePeakTicks    = 0.0;
    double              m_frameMeanTicks    = 0.0;
    std::array<double, clockDilation::kFramePeakWindow> m_recentFrameTicks{};
    uint32_t            m_nextRecentFrame   = 0u;
    double              m_lastGuardSeconds  = 0.0;
    bool                m_hasPreviousCall      = false;
    bool                m_previousSensorValid  = false;
    double              m_previousServerTick   = 0.0;
    uint32_t            m_previousReadAdvances = 0u;
};

struct ClockDilationSensorCounters
{
    uint32_t acceptedSamples   = 0u;
    uint32_t rejectedSamples   = 0u;
    uint32_t reseeds           = 0u;
    double   lastSampleSeconds = 0.0;
};

namespace clockDilation
{
inline constexpr double   kWindowSeconds     = 10.0;
inline constexpr uint32_t kMaxKeptSamples    = 1024u;
inline constexpr size_t   kWindowLineReserve = 1024u;

static_assert(kMaxKeptSamples >= 2u && kMaxKeptSamples % 2u == 0u,
    "clockDilation::kMaxKeptSamples: a full series keeps every second sample, so the capacity must be even");

using KeptValues = std::array<double, kMaxKeptSamples>;

class KeptSeries
{
public:
    void add(double value)
    {
        const uint64_t index = m_seen++;
        if (index % m_stride != 0u)
            return;
        if (m_count == kMaxKeptSamples)
        {
            for (uint32_t kept = 0u; kept < kMaxKeptSamples / 2u; ++kept)
                m_values[kept] = m_values[2u * kept];
            m_count = kMaxKeptSamples / 2u;
            m_stride *= 2u;
            if (index % m_stride != 0u)
                return;
        }
        m_values[m_count++] = value;
    }

    void clear()
    {
        m_count  = 0u;
        m_seen   = 0u;
        m_stride = 1u;
    }

    uint32_t count() const { return m_count; }
    uint64_t stride() const { return m_stride; }

    const KeptValues& sortedInto(KeptValues& scratch) const
    {
        std::copy(m_values.begin(), m_values.begin() + m_count, scratch.begin());
        std::sort(scratch.begin(), scratch.begin() + m_count);
        return scratch;
    }

private:
    KeptValues m_values{};
    uint32_t   m_count  = 0u;
    uint64_t   m_seen   = 0u;
    uint64_t   m_stride = 1u;
};

inline double nearestRank(const KeptValues& sorted, uint32_t count, uint32_t percent)
{
    if (count == 0u)
        return 0.0;
    const uint64_t rank = (static_cast<uint64_t>(percent) * count + 99u) / 100u;
    return sorted[static_cast<size_t>(std::clamp<uint64_t>(rank, 1u, count) - 1u)];
}

inline const char* modeWord(ClockDilationMode mode)
{
    return mode == ClockDilationMode::On ? "On" : mode == ClockDilationMode::Shadow ? "Shadow" : "Off";
}

inline const char* setpointWord(ClockDilationSetpoint setpoint)
{
    return setpoint == ClockDilationSetpoint::Common ? "Common" : "OwnRtt";
}
} // namespace clockDilation

class ClockDilationWindow
{
public:
    explicit ClockDilationWindow(const ClockDilationConfig& config)
        : m_mode(config.mode)
        , m_setpoint(config.setpoint)
    {
        m_line.reserve(clockDilation::kWindowLineReserve);
    }

    ClockDilationWindow(const ClockDilationWindow&) = delete;
    ClockDilationWindow& operator=(const ClockDilationWindow&) = delete;

    void note(const ClockDilationOutput& out, double doorRateScale, const ClockDilationSensorCounters& sensor,
              double clockNowSeconds, double serverTick, double phaseBlindServerTick)
    {
        if (!m_hasStart)
        {
            m_startSeconds = clockNowSeconds;
            m_hasStart     = true;
        }
        ++m_calls;

        if (out.freeze == DilationFreeze::None)
        {
            m_lead.add(out.leadTicks);
            m_error.add(out.errorTicks);
            m_targetSum += out.targetTicks;
            ++m_unfrozen;
            m_errorAbsMax = std::max(m_errorAbsMax, std::abs(out.errorTicks));
        }
        else
        {
            ++m_frozen;
            countFreeze(out.freeze);
        }
        m_guardBound += out.guardBound ? 1u : 0u;
        m_applied += out.apply ? 1u : 0u;

        m_wouldChanges += out.scale != m_lastScale ? 1u : 0u;
        m_lastScale     = out.scale;

        const bool first = m_calls == 1u;
        m_scaleMin  = first ? out.scale : std::min(m_scaleMin, out.scale);
        m_scaleMax  = first ? out.scale : std::max(m_scaleMax, out.scale);
        m_scaleSum += out.scale;
        m_scaleDevPercent.add(std::abs(out.scale - 1.0) * 100.0);
        m_rateMin = first ? doorRateScale : std::min(m_rateMin, doorRateScale);
        m_rateMax = first ? doorRateScale : std::max(m_rateMax, doorRateScale);

        m_sensor = sensor;
        if (sensor.acceptedSamples > 0u)
            m_sensorAgeMsMax = std::max(m_sensorAgeMsMax, (clockNowSeconds - sensor.lastSampleSeconds) * 1000.0);

        m_lastClockSeconds     = clockNowSeconds;
        m_serverTick           = serverTick;
        m_phaseBlindServerTick = phaseBlindServerTick;
        m_stepLagTicks         = out.stepLagTicks;
    }

    bool isDue(double clockNowSeconds) const
    {
        return m_hasStart && clockNowSeconds - m_startSeconds >= clockDilation::kWindowSeconds;
    }

    const char* formatLine()
    {
        using clockDilation::nearestRank;
        const uint32_t leads  = m_lead.count();
        const uint32_t errors = m_error.count();
        const uint32_t devs   = m_scaleDevPercent.count();

        const clockDilation::KeptValues& lead = m_lead.sortedInto(m_scratch);
        const double leadP5  = nearestRank(lead, leads, 5u);
        const double leadP50 = nearestRank(lead, leads, 50u);
        const double leadP95 = nearestRank(lead, leads, 95u);
        const clockDilation::KeptValues& error = m_error.sortedInto(m_scratch);
        const double errorP5  = nearestRank(error, errors, 5u);
        const double errorP50 = nearestRank(error, errors, 50u);
        const double errorP95 = nearestRank(error, errors, 95u);
        const double devP99   = nearestRank(m_scaleDevPercent.sortedInto(m_scratch), devs, 99u);

        return formatHostLine(m_line,
            "[Dilation.Window] mode=%s setpoint=%s calls=%u lead=%.3f/%.3f/%.3f target=%.3f err=%.3f/%.3f/%.3f "
            "errAbsMax=%.3f wouldScale=%.5f/%.5f/%.5f wouldScaleDevP99=%.4f rateScale=%.5f/%.5f applied=%u frozen=%u "
            "frozenBy=sensor:%u,recentre:%u,debt:%u,firstRtt:%u,resync:%u guardBound=%u sensorSamples=%u "
            "sensorRejected=%u sensorReseeds=%u sensorAgeMsMax=%.1f sHatClock=%.6f sHat=%.3f sHatNoAge=%.3f wouldChanges=%u "
            "stepLag=%.3f",
            clockDilation::modeWord(m_mode), clockDilation::setpointWord(m_setpoint), m_calls,
            leadP5, leadP50, leadP95,
            m_unfrozen > 0u ? m_targetSum / static_cast<double>(m_unfrozen) : 0.0,
            errorP5, errorP50, errorP95, m_errorAbsMax,
            m_scaleMin, m_calls > 0u ? m_scaleSum / static_cast<double>(m_calls) : 0.0, m_scaleMax, devP99,
            m_rateMin, m_rateMax, m_applied, m_frozen,
            m_frozenBySensor, m_frozenByRecentre, m_frozenByDebt, m_frozenByFirstRtt, m_frozenByResync, m_guardBound,
            m_sensor.acceptedSamples - m_sensorAtStart.acceptedSamples,
            m_sensor.rejectedSamples - m_sensorAtStart.rejectedSamples,
            m_sensor.reseeds - m_sensorAtStart.reseeds,
            m_sensorAgeMsMax, m_lastClockSeconds, m_serverTick, m_phaseBlindServerTick, m_wouldChanges, m_stepLagTicks);
    }

    void restart()
    {
        m_startSeconds     = m_lastClockSeconds;
        m_calls            = 0u;
        m_unfrozen         = 0u;
        m_frozen           = 0u;
        m_frozenBySensor   = 0u;
        m_frozenByRecentre = 0u;
        m_frozenByDebt     = 0u;
        m_frozenByFirstRtt = 0u;
        m_frozenByResync   = 0u;
        m_guardBound       = 0u;
        m_applied          = 0u;
        m_wouldChanges     = 0u;
        m_targetSum        = 0.0;
        m_errorAbsMax      = 0.0;
        m_scaleMin         = 1.0;
        m_scaleMax         = 1.0;
        m_scaleSum         = 0.0;
        m_rateMin          = 1.0;
        m_rateMax          = 1.0;
        m_sensorAgeMsMax   = 0.0;
        m_sensorAtStart    = m_sensor;
        m_lead.clear();
        m_error.clear();
        m_scaleDevPercent.clear();
    }

    uint32_t calls() const { return m_calls; }

private:
    void countFreeze(DilationFreeze freeze)
    {
        switch (freeze)
        {
        case DilationFreeze::SensorInvalid: ++m_frozenBySensor; break;
        case DilationFreeze::Recentre: ++m_frozenByRecentre; break;
        case DilationFreeze::TransitionDebt: ++m_frozenByDebt; break;
        case DilationFreeze::AwaitFirstRtt: ++m_frozenByFirstRtt; break;
        case DilationFreeze::AfterResync: ++m_frozenByResync; break;
        case DilationFreeze::None:
        case DilationFreeze::ModeOff: break;
        }
    }

    ClockDilationMode           m_mode;
    ClockDilationSetpoint       m_setpoint;
    bool                        m_hasStart             = false;
    double                      m_startSeconds         = 0.0;
    uint32_t                    m_calls                = 0u;
    uint32_t                    m_unfrozen             = 0u;
    uint32_t                    m_frozen               = 0u;
    uint32_t                    m_frozenBySensor       = 0u;
    uint32_t                    m_frozenByRecentre     = 0u;
    uint32_t                    m_frozenByDebt         = 0u;
    uint32_t                    m_frozenByFirstRtt     = 0u;
    uint32_t                    m_frozenByResync       = 0u;
    uint32_t                    m_guardBound           = 0u;
    uint32_t                    m_applied              = 0u;
    uint32_t                    m_wouldChanges         = 0u;
    double                      m_lastScale            = 1.0;
    double                      m_targetSum            = 0.0;
    double                      m_errorAbsMax          = 0.0;
    double                      m_scaleMin             = 1.0;
    double                      m_scaleMax             = 1.0;
    double                      m_scaleSum             = 0.0;
    double                      m_rateMin              = 1.0;
    double                      m_rateMax              = 1.0;
    double                      m_sensorAgeMsMax       = 0.0;
    ClockDilationSensorCounters m_sensor;
    ClockDilationSensorCounters m_sensorAtStart;
    double                      m_lastClockSeconds     = 0.0;
    double                      m_serverTick           = 0.0;
    double                      m_phaseBlindServerTick = 0.0;
    double                      m_stepLagTicks         = 0.0;
    clockDilation::KeptSeries   m_lead;
    clockDilation::KeptSeries   m_error;
    clockDilation::KeptSeries   m_scaleDevPercent;
    clockDilation::KeptValues   m_scratch{};
    HostLine                    m_line;
};

namespace clockDilation
{
template <class ManagerT>
ClockDilationOutput beforeClientPump(TickRateDoor& door, double hostNowSeconds, double clockNowSeconds,
                                     const ManagerT& manager, ClockDilationController& controller,
                                     ClockDilationWindow& window)
{
    OG_CHECK(manager.runsPrediction(),
        "clockDilation::beforeClientPump runs on a predicting client only: the authority's step rate is the reference "
        "every client's lead is measured against, and it has no estimator or prediction clock to read. Was the "
        "authority guard of the client stage's setRateScale call");

    const auto&  estimator  = manager.getNetworkEstimator();
    const auto&  sensor     = estimator.serverTickEstimator();
    const auto&  time       = manager.getTimeConfig();
    const double serverTick = sensor.serverTickAt(clockNowSeconds);

    ClockDilationInputs in;
    in.clockNowSeconds       = clockNowSeconds;
    in.predictionTick        = static_cast<double>(door.tickOffset() + static_cast<int64_t>(door.physicsStepCount()))
                               + (hostNowSeconds - door.nextDeadline()) / door.currentInterval();
    in.sensorValid           = sensor.isValid(clockNowSeconds);
    in.serverTick            = serverTick;
    in.smoothedRttSeconds    = estimator.getSmoothedRTT();
    in.smoothedJitterSeconds = estimator.getSmoothedJitter();
    in.jitterMultiplier      = estimator.activeJitterMultiplier();
    in.predOffsetFloorTicks  = time.predOffsetFloorTicks;
    in.tickFrequency         = time.tickFrequency;
    in.authorityTick         = estimator.getLastAuthorityTick();
    in.predictionOffsetTicks = estimator.getPredictionOffsetTicks();
    in.phase                 = manager.getClientClock().publishedCorrectionPhase();
    in.clockRead             = manager.getClientClock().publishedTargetRead();
    in.currentScale          = door.rateScale();

    const ClockDilationOutput out = controller.update(in);
    // ⛔G-05  docs/ClockDilationController-guards.md
    if (out.apply)
        door.setRateScale(out.scale);

    ClockDilationSensorCounters counters;
    counters.acceptedSamples   = sensor.acceptedSamples();
    counters.rejectedSamples   = sensor.rejectedSamples();
    counters.reseeds           = sensor.reseeds();
    counters.lastSampleSeconds = sensor.lastSampleSeconds();
    window.note(out, door.rateScale(), counters, clockNowSeconds, serverTick,
                sensor.phaseBlindServerTickAt(clockNowSeconds));
    return out;
}
} // namespace clockDilation
