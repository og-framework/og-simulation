#pragma once
// SPDX-License-Identifier: MPL-2.0

#include "OGTypes.h"
#include "OGAssert.h"
#include "OGExport.h"
#include <algorithm>
#include <optional>
#include <functional>
#include "OGSimulation/SimulationTypes.h"

#pragma optimize( "", off )

// Replaces bool isStalling / bool isSkipping pair on SimulationTimeStep.
// isResimulation stays a separate axis — resim is driven by Chaos, not the clock.
enum class StepKind : uint8_t
{
    Normal,     // sim tick advances by 1
    Stall,      // sim tick does not advance
    Skip,       // sim tick jumps by >1; previous tick must be back-filled
    HardResync  // caches wiped; treated like Normal for the integrate step
};

inline const char* stepKindName(StepKind k)
{
    switch (k)
    {
        case StepKind::Normal:     return "Normal";
        case StepKind::Stall:      return "Stall";
        case StepKind::Skip:       return "Skip";
        case StepKind::HardResync: return "HardResync";
    }
    return "?";
}

class SimulationTimeStep
{
public:
	SimulationTimeStep() = delete;

	// Normal or HardResync step (no stall/skip).
	SimulationTimeStep(unsigned int tick, bool isResimulating)
		: m_currentTick(tick)
		, m_isResimulating(isResimulating)
		, m_stepKind(StepKind::Normal)
		, m_deltaSeconds(0.f)
	{
	}

	// Full constructor with explicit StepKind.
	SimulationTimeStep(unsigned int tick, bool isResimulating, StepKind stepKind, float deltaSeconds = 0.f)
		: m_currentTick(tick)
		, m_isResimulating(isResimulating)
		, m_stepKind(stepKind)
		, m_deltaSeconds(deltaSeconds)
	{
	}

	// Legacy bool-pair constructor — kept for call-site compatibility during the split.
	SimulationTimeStep(unsigned int tick, bool isResimulating, bool isStalling, bool isSkipping)
		: m_currentTick(tick)
		, m_isResimulating(isResimulating)
		, m_stepKind(isStalling ? StepKind::Stall : (isSkipping ? StepKind::Skip : StepKind::Normal))
		, m_deltaSeconds(0.f)
	{
	}

	// Legacy bool-pair constructor with delta seconds.
	SimulationTimeStep(unsigned int tick, bool isResimulating, bool isStalling, bool isSkipping, float deltaSeconds)
		: m_currentTick(tick)
		, m_isResimulating(isResimulating)
		, m_stepKind(isStalling ? StepKind::Stall : (isSkipping ? StepKind::Skip : StepKind::Normal))
		, m_deltaSeconds(deltaSeconds)
	{
	}

    unsigned int getTick() const { return m_currentTick; }
	bool getIsResimulating() const { return m_isResimulating; }
	StepKind getStepKind() const { return m_stepKind; }
	// Legacy accessors — delegate to StepKind for backward compatibility during migration.
	bool getIsStalling() const { return m_stepKind == StepKind::Stall; }
	bool getIsSkipping() const { return m_stepKind == StepKind::Skip; }
	float getDeltaSeconds() const { return m_deltaSeconds; }

private:
    unsigned int m_currentTick = 0;
    bool m_isResimulating = false;
	StepKind m_stepKind = StepKind::Normal;
	float m_deltaSeconds = 0.f;
};

class AuthoritySimulationTimeManager
{
public:
	OGSIMULATION_API AuthoritySimulationTimeManager();

    unsigned int getTick() const { return m_currentTick; }
    void advanceTick() { m_currentTick++; }

    OGSIMULATION_API SimulationTimeStep getAuthoritySimulationStep() const;

    static uint32 constexpr SyncSize() { return sizeof(PlainType<decltype(m_currentTick)>); }

    template <typename SyncedBuffer>
    static uint32 writeStateToSyncedBuffer(const AuthoritySimulationTimeManager& state, SyncedBuffer& buffer, uint32 byteIterator)
    {
        uint32 internlByteIterator = byteIterator;
        buffer.writeToBuffer(internlByteIterator, state.getTick());
        internlByteIterator = internlByteIterator + sizeof(PlainType<decltype(std::declval<AuthoritySimulationTimeManager>().getTick())>);
        
        OG_CHECK(SyncSize() == internlByteIterator - byteIterator, "wrote to syncedBuffer incorrectly");

        return internlByteIterator - byteIterator;
    }

    template <typename SyncedBuffer>
    static SimulationTimeStep readStateFromSyncedBuffer(const SyncedBuffer& buffer, uint32 byteIterator)
    {
        uint32 internlByteIterator = byteIterator;
        const uint32 authoritativeTick = buffer.template readFromBuffer<PlainType<decltype(std::declval<AuthoritySimulationTimeManager>().getTick())>>(internlByteIterator);
        internlByteIterator = internlByteIterator + sizeof(PlainType<decltype(std::declval<AuthoritySimulationTimeManager>().getTick())>);

        OG_CHECK(SyncSize() == internlByteIterator - byteIterator, "read from syncedBuffer incorrectly");

        return SimulationTimeStep(authoritativeTick, false);
    }

private:

	unsigned int m_currentTick = 0;
}; 


class PredictedAndCorrectionSimulationTimeManager
{
public:
    OGSIMULATION_API PredictedAndCorrectionSimulationTimeManager(double tickFrequency);
    void advancePrediction() 
    {

        if (m_predictionTick == m_resimulationTick)
            ++m_resimulationTick;

        ++m_predictionTick;
    }
    void advanceResimulation() 
    { 
        ++m_resimulationTick; 
    }

    uint32 getPredictionTick() const { return m_predictionTick; }
    uint32 getResimulationTick() const { return m_resimulationTick; }


    void startResimulation(uint32 tick) 
    { 
        m_resimulationTick = tick; 
    }

    void finishResimulation()
    {
        m_resimulationTick = m_predictionTick;
    }


    OGSIMULATION_API void recordAuthorityTick(uint32 tick);
    OGSIMULATION_API uint32 getExpectedPredictionTick();

    SimulationTimeStep getPredictionSimulationStep() const;
    SimulationTimeStep getReSimulationStep() const;

    void setRoundTripTime(double roundTripTime) { m_roundTripTime = roundTripTime; }


	using OnPredictionResyncWithAuthorityCallback = std::function<void(uint32)>;
	static constexpr size_t InvalidCallbackId = std::numeric_limits<size_t>::max();
    unsigned int registerOnPredictionResyncWithAuthorityCallback(OnPredictionResyncWithAuthorityCallback callback)
    {
        m_onPredictionResyncWithAuthority.push_back(callback);
		return m_onPredictionResyncWithAuthority.size() - 1;
    }

    void unregisterOnPredictionResyncWithAuthorityCallback(unsigned int callbackId)
    {
		std::swap(m_onPredictionResyncWithAuthority[callbackId], m_onPredictionResyncWithAuthority.back());
		m_onPredictionResyncWithAuthority.pop_back();
    }


private:
    PredictedAndCorrectionSimulationTimeManager() = delete;


    uint32 m_predictionTick;
    uint32 m_resimulationTick;
    uint32 m_authorityTick;
    double m_roundTripTime;


    const double m_tickFrequency;

    std::vector<std::function<void(uint32)>> m_onPredictionResyncWithAuthority;

};

#pragma optimize( "", on )
