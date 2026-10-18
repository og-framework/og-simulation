#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/TickDriverThreadContext-rationale.md · docs/TickDriverThreadContext-guards.md

#include <concepts>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>

#include "OGSimulation/OGAssert.h"

template <typename L>
concept BasicLockable = requires(L& lock)
{
	lock.lock();
	lock.unlock();
};

namespace basicLockableSelfCheck
{
struct Conforming
{
	void lock();
	void unlock();
};

struct MissingUnlock
{
	void lock();
};

struct CapitalisedOnly
{
	void Lock();
	void Unlock();
};

static_assert(BasicLockable<Conforming>,
	"BasicLockable self-check: the reference shape must satisfy the concept");
static_assert(!BasicLockable<MissingUnlock>,
	"BasicLockable self-check: unlock is required, so a lock scope can release what it took");
static_assert(!BasicLockable<CapitalisedOnly>,
	"BasicLockable self-check: a Lock/Unlock-only mutex needs a lock/unlock wrapper before a world host can take it");
static_assert(BasicLockable<std::mutex>,
	"BasicLockable self-check: std::mutex models the concept, so an engine-free host can use it directly");
} // namespace basicLockableSelfCheck

class TickDriverThreadContext;

namespace tickDriverThread
{
inline constexpr uint8_t kAuthorityRoleBit = 1u;
inline constexpr uint8_t kClientRoleBit    = 2u;

static_assert((kAuthorityRoleBit & kClientRoleBit) == 0u && kAuthorityRoleBit != 0u && kClientRoleBit != 0u,
	"tickDriverThread: the two role bits are distinct single bits, so one thread can hold both roles' marks at once");

static_assert(kAuthorityRoleBit < kClientRoleBit,
	"tickDriverThread: the owner counts are indexed by the role bit, so the client's bit is the largest index");

inline thread_local uint8_t t_worldLockHeldRoles = 0u;
inline thread_local uint8_t t_stepRunningRoles   = 0u;

class OwnerThreadCounts
{
public:
	static uint32_t onThisThread(uint8_t role) { return t_counts[role]; }

private:
	friend class ::TickDriverThreadContext;

	static inline thread_local uint32_t t_counts[kClientRoleBit + 1u] = {};
};

inline thread_local uint64_t t_shadowQueryAccesses          = 0u;
inline thread_local uint64_t t_stepQueryOwnerThreadAccesses = 0u;

template <uint8_t RoleBit>
bool isOwnerThread()
{
	static_assert(RoleBit == kAuthorityRoleBit || RoleBit == kClientRoleBit,
		"tickDriverThread::isOwnerThread: the role bit is the authority's or the client's");
	return OwnerThreadCounts::onThisThread(RoleBit) > 0u;
}

template <uint8_t RoleBit>
bool accessFromStepOrWorldLock()
{
	return ((t_stepRunningRoles | t_worldLockHeldRoles) & RoleBit) != 0u;
}

template <uint8_t RoleBit, bool HostDiagnostics>
bool workerClientStepWorldAccess()
{
	if constexpr (HostDiagnostics)
	{
		if ((t_worldLockHeldRoles & RoleBit) == 0u && isOwnerThread<RoleBit>())
			++t_stepQueryOwnerThreadAccesses;
	}
	return accessFromStepOrWorldLock<RoleBit>();
}

template <uint8_t RoleBit, bool HostDiagnostics>
bool shadowAccessOnOwnerThread()
{
	if constexpr (HostDiagnostics)
	{
		++t_shadowQueryAccesses;
	}
	return isOwnerThread<RoleBit>();
}

template <uint8_t RoleBit>
bool accessFromStepWorldLockOrInlineOwnerThread()
{
	return accessFromStepOrWorldLock<RoleBit>() || isOwnerThread<RoleBit>();
}
} // namespace tickDriverThread

class TickDriverThreadContext
{
public:
	using AccessPredicate = bool (*)();

	TickDriverThreadContext() = default;

	~TickDriverThreadContext()
	{
		// ⛔G-02  docs/TickDriverThreadContext-guards.md
		if (m_configured && std::this_thread::get_id() == m_configuringThread)
			--tickDriverThread::OwnerThreadCounts::t_counts[m_roleBit];
	}

	TickDriverThreadContext(const TickDriverThreadContext&) = delete;
	TickDriverThreadContext& operator=(const TickDriverThreadContext&) = delete;

	template <bool HostDiagnostics>
	static AccessPredicate predicateFor(uint8_t role, bool inlineSteps)
	{
		using namespace tickDriverThread;
		if (role == kAuthorityRoleBit)
		{
			return inlineSteps ? &accessFromStepWorldLockOrInlineOwnerThread<kAuthorityRoleBit>
			                   : &accessFromStepOrWorldLock<kAuthorityRoleBit>;
		}
		return inlineSteps ? &accessFromStepWorldLockOrInlineOwnerThread<kClientRoleBit>
		                   : &workerClientStepWorldAccess<kClientRoleBit, HostDiagnostics>;
	}

	template <bool HostDiagnostics>
	static AccessPredicate shadowPredicateFor(uint8_t role)
	{
		using namespace tickDriverThread;
		return role == kAuthorityRoleBit ? &shadowAccessOnOwnerThread<kAuthorityRoleBit, HostDiagnostics>
		                                    : &shadowAccessOnOwnerThread<kClientRoleBit, HostDiagnostics>;
	}

	template <bool HostDiagnostics>
	void configure(uint8_t role, bool inlineSteps)
	{
		OG_CHECK(role == tickDriverThread::kAuthorityRoleBit || role == tickDriverThread::kClientRoleBit,
			"TickDriverThreadContext::configure: the role bit is the authority's (1) or the client's (2)");
		OG_CHECK(!m_configured,
			"TickDriverThreadContext::configure: a context is configured once; a second call would count its owner thread twice");
		m_roleBit           = role;
		m_stepsInline       = inlineSteps;
		m_configured        = true;
		m_configuringThread = std::this_thread::get_id();
		// ⛔G-01  docs/TickDriverThreadContext-guards.md
		++tickDriverThread::OwnerThreadCounts::t_counts[role];
		m_stepWorldPredicate = predicateFor<HostDiagnostics>(role, inlineSteps);
		m_shadowPredicate    = shadowPredicateFor<HostDiagnostics>(role);
	}

	uint8_t roleBit() const { return m_roleBit; }
	bool stepsInline() const { return m_stepsInline; }
	AccessPredicate stepWorldPredicate() const { return m_stepWorldPredicate; }
	AccessPredicate shadowPredicate() const { return m_shadowPredicate; }

	bool isWorldLockHeldOnThisThread() const { return (tickDriverThread::t_worldLockHeldRoles & m_roleBit) != 0u; }
	bool isStepRunningOnThisThread() const { return (tickDriverThread::t_stepRunningRoles & m_roleBit) != 0u; }
	bool isOwnerThreadForRole() const { return tickDriverThread::OwnerThreadCounts::onThisThread(m_roleBit) > 0u; }

	static uint64_t shadowQueryAccessesOnThisThread() { return tickDriverThread::t_shadowQueryAccesses; }
	static uint64_t stepQueryOwnerThreadAccessesOnThisThread() { return tickDriverThread::t_stepQueryOwnerThreadAccesses; }

private:
	uint8_t         m_roleBit            = 0u;
	bool            m_stepsInline        = false;
	bool            m_configured         = false;
	std::thread::id m_configuringThread;
	AccessPredicate m_stepWorldPredicate = nullptr;
	AccessPredicate m_shadowPredicate    = nullptr;
};

struct NoAccessScope
{
};

template <typename LockT, typename AccessScopeT = NoAccessScope>
	requires BasicLockable<LockT> && std::default_initializable<AccessScopeT>
class WorldLockScope
{
public:
	WorldLockScope(LockT& lock, const TickDriverThreadContext& context)
		: m_lock(checkedNotYetHeld(lock, context.roleBit()))
		, m_roleBit(context.roleBit())
	{
		tickDriverThread::t_worldLockHeldRoles |= m_roleBit;
	}

	~WorldLockScope()
	{
		tickDriverThread::t_worldLockHeldRoles &= static_cast<uint8_t>(~m_roleBit);
	}

	WorldLockScope(const WorldLockScope&) = delete;
	WorldLockScope& operator=(const WorldLockScope&) = delete;

private:
	static LockT& checkedNotYetHeld(LockT& lock, [[maybe_unused]] uint8_t roleBit)
	{
		OG_CHECK(roleBit == tickDriverThread::kAuthorityRoleBit || roleBit == tickDriverThread::kClientRoleBit,
			"WorldLockScope: the world lock was taken before TickDriverThreadContext::configure chose the role bit");
		OG_CHECK((tickDriverThread::t_worldLockHeldRoles & roleBit) == 0u,
			"WorldLockScope: this role's world lock is already held on this thread. The lock need not be recursive: a "
			"second lock may deadlock. Take it once, around the whole world write or the whole step");
		return lock;
	}

	std::lock_guard<LockT> m_lock;
	AccessScopeT           m_access;
	uint8_t                m_roleBit;
};

class StepScope
{
public:
	explicit StepScope(const TickDriverThreadContext& context)
		: m_roleBit(context.roleBit())
	{
		OG_CHECK((tickDriverThread::t_stepRunningRoles & m_roleBit) == 0u,
			"StepScope: a step of this role is already running on this thread: steps never nest");
		tickDriverThread::t_stepRunningRoles |= m_roleBit;
	}

	~StepScope()
	{
		tickDriverThread::t_stepRunningRoles &= static_cast<uint8_t>(~m_roleBit);
	}

	StepScope(const StepScope&) = delete;
	StepScope& operator=(const StepScope&) = delete;

private:
	uint8_t m_roleBit;
};
