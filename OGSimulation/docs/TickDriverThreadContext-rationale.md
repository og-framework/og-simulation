<!-- SPDX-License-Identifier: MPL-2.0 -->
# `TickDriverThreadContext.h` — rationale

The narrative behind `BasicLockable`, the `tickDriverThread` role bits, thread-local masks, counters and
predicates, `TickDriverThreadContext`, `NoAccessScope`, `WorldLockScope` and `StepScope`. The header
keeps its licence, its docs pointer, its code, one guard tag and its `static_assert` and `OG_CHECK`
messages; everything that explains it lives here. Prohibitions are in `TickDriverThreadContext-guards.md`.

**If this file and `TickDriverThreadContext.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by
construction: STL only. Everything below is true to a reader with no game engine and no other
repository.

---

## §1 What it is for

A host that steps a physics world on one thread and touches it from another needs to answer one
question on every world call: *may this thread touch this world now?* A physics adapter asks it
through a context-free predicate, a plain `bool (*)()` (`TickDriverThreadContext::AccessPredicate`),
and asserts the answer. This header holds the thread facts the predicate reads and the scopes that set
them:
- two **role bits**, one for the authority's world and one for a client's (§2);
- two **thread-local masks** with one bit per role: "this thread holds the role's world lock"
  (`t_worldLockHeldRoles`) and "a step of the role is running on this thread" (`t_stepRunningRoles`);
- a **thread-local owner count** per role: "how many live contexts of the role were configured on this
  thread" (`OwnerThreadCounts`, §3);
- the **predicates** built from them (§4) and `TickDriverThreadContext`, which picks the predicates for a
  role and a stepping mode;
- `WorldLockScope` and `StepScope`, the RAII scopes that set the first two masks (§5);
- two diagnostic **counters** (§6).

The predicates are context-free, so the only state they can consult is the calling thread's. That is
why every fact here is thread-local, the owner count included.

## §2 Role bits: why the masks are keyed per role

One process can run an authority's world and a client's world on the same thread, for example a host
that runs a server and a client together for testing. With one process-wide "lock held" or "step
running" flag, the authority's inline step would satisfy the client's predicate on that thread, and a
world write under one role's lock would make the other role's adapter accept a call it must refuse. So
each mask has one bit per role (`kAuthorityRoleBit` = 1, `kClientRoleBit` = 2; the header's
`static_assert` keeps them distinct single bits), and each predicate is a template instantiated on its
role's bit. `TickDriverThreadContext.Predicates.TwoRoleBitsOnOneThreadStayApart` holds one role's lock
and step on a thread and checks that the other role's predicates refuse.

`WorldLockScope`'s "not already held" check tests the scope's own role bit only. A thread that holds
both roles' locks at once is therefore not flagged. That nesting cannot deadlock as long as a step
takes only its own role's lock.

## §3 The owner count (guards G-01 and G-02)

`TickDriverThreadContext::configure(role, inlineSteps)` records the role, the stepping mode and the
calling thread, adds one to the role's owner count **for the calling thread**, and fixes the context's
two predicates (`stepWorldPredicate` and `shadowPredicate`). `tickDriverThread::isOwnerThread<Bit>()`
is "the role's count on this thread is above zero". It stands where an engine host would ask "is this
the engine's main thread": the host calls `configure` at world build, on the thread that will own the
world, before it creates any adapter whose calls the predicate checks. A task that an executor runs on
the waiting owner thread itself (a blocking wait that runs the task in place) sees the owner's count,
as it would see the engine's own thread query.

The count must be raised as soon as `configure` returns (G-01):
`TickDriverThreadContext.Configure.PredicateHoldsForTheOwnerThreadBeforeAnyDriver` checks each
predicate kind straight after `configure`, with no driver and no lock.
`TickDriverThreadContext.IsOwnerThread.OnlyOnTheConfiguringThread` checks that a `std::thread` does
not see it.

**Why a count and not a bit.** One thread can own several live contexts of the same role at once: a
process that runs several client worlds on its main thread, or a host that builds a new world before it
destroys the old one. The predicates are context-free (§1), so all of those contexts read the same
thread-local fact. With one bit per role, destroying any one of them would clear the fact for all the
others: their inline step predicate and their shadow predicate would go false on the owner thread, and
the next owner-thread world call outside the lock would fail its adapter's assertion. With a count,
each context takes away only the one it added.
`TickDriverThreadContext.OwnerMark.SameRoleContextsOnOneThreadAreCounted` configures two contexts of
one role on one thread and destroys the first; the second's `isOwnerThreadForRole` and both of its
predicates still hold, and they fail once the second is destroyed too.

**The destructor (G-02).** The destructor subtracts one from the role's count only if the context was
configured **and** it runs on the thread that configured it. On any other thread it does nothing: it
neither subtracts nor checks.
- The count is thread-local, so a subtraction on another thread would come off *that* thread's count.
  The owner thread's count would stay raised anyway, and the destroying thread's count, zero until then,
  would wrap to the largest `uint32_t`: that thread would pass every owner query of the role from then
  on. `TickDriverThreadContext.OwnerMark.DestroyedOnAnotherThreadLeavesBothThreadsAlone` configures a
  context on one thread, destroys it on a second, and checks both threads' counts.
- A check would turn a teardown that is otherwise harmless into a failure. The header cannot know which
  thread a host's teardown runs on, and nothing obliges a host to destroy its world host on the thread
  that built it. A count left behind on the owner thread only keeps that thread passing the role's
  owner query. That is what a main-thread query does for the whole life of a process, so it is the
  behaviour this count replaced. The world-lock and step masks are not affected.
- An unconfigured context added nothing, so it subtracts nothing. `m_configured` says so directly; its
  default `std::thread::id` matches no thread as well.

A test process that reuses one thread for many cases destroys each context on the thread that
configured it, so one case's count does not leak into the next
(`TickDriverThreadContext.OwnerMark.ClearedWhenTheContextGoes`). The context cannot be copied, so it
subtracts at most once. `configure` is called once per context, and it `OG_CHECK`s that it was not
called before: a second call would add a second count that the destructor never takes away.

Only `TickDriverThreadContext` writes the counts. `OwnerThreadCounts` keeps them private and names the
context as its one friend, so a write from anywhere else does not compile. Everything else reads them
through `OwnerThreadCounts::onThisThread`, `isOwnerThread` or `isOwnerThreadForRole`. The counts are
indexed by the role bit (the header's `static_assert` keeps the client's bit the largest index).
`configure` accepts only the two role bits, so index 0 is never raised and an unconfigured context's
`isOwnerThreadForRole` is false.

## §4 The predicates

`TickDriverThreadContext::predicateFor<HostDiagnostics>(role, inlineSteps)` picks the step world's
predicate:

| role and mode | predicate | accepts |
|---|---|---|
| authority, inline | `accessFromStepWorldLockOrInlineOwnerThread` | a running step, the role's lock, or the owner thread |
| authority, not inline | `accessFromStepOrWorldLock` | a running step or the role's lock |
| client, inline | `accessFromStepWorldLockOrInlineOwnerThread` | a running step, the role's lock, or the owner thread |
| client, on a worker | `workerClientStepWorldAccess` | a running step or the role's lock; counts owner-thread calls outside the lock (§6) |

An inline host steps on the owner thread and queries the world between steps on the same thread, so
its predicate accepts the owner thread. A worker client steps on another thread, so its step world
refuses the owner thread outside the lock; the owner thread reaches it only under the lock (a world
write such as a bind) and otherwise reads a second world of its own.
`shadowPredicateFor<HostDiagnostics>(role)` is that second world's predicate,
`shadowAccessOnOwnerThread`: the owner thread only.

## §5 The scopes

`WorldLockScope<LockT, AccessScopeT>` is the world lock for one role, taken for a whole step or a whole
world write:
1. it checks that the context has a role (a lock taken before `configure` is an `OG_CHECK`) and that
   this thread does not already hold the role's lock. The lock need not be recursive, so a second lock
   on the same thread may deadlock; the check stops it first;
2. it locks (`std::lock_guard<LockT>`);
3. it opens the optional access scope, a default-constructed `AccessScopeT`, for a physics backend that
   keeps its own "world access is open on this thread" marker (`NoAccessScope` when it keeps none);
4. it sets the role's bit in `t_worldLockHeldRoles`.

Destruction runs in reverse: the bit, then the access scope, then the unlock.

`LockT` must model `BasicLockable`: `lock()` and `unlock()`, as the standard library's lockables do, so
`std::mutex` works directly. An engine mutex with only `Lock`/`Unlock` needs a small wrapper; the
self-check pins that both ways (`basicLockableSelfCheck::CapitalisedOnly`), and the class's `requires`
clause refuses the bare mutex at compile time.

`StepScope` marks "a step of this role is running on this thread" for the duration of one step. Steps
never nest, so it `OG_CHECK`s the bit before it sets it.

`TickDriverThreadContext::isWorldLockHeldOnThisThread` and `isStepRunningOnThisThread` read the two
masks for the context's role, for a host's own step-context checks; `isOwnerThreadForRole` reads the
owner count (§3).

## §6 The diagnostic counters

`t_shadowQueryAccesses` counts every call of a shadow predicate on the thread. `t_stepQueryOwnerThreadAccesses`
counts worker-client step-world calls that the owner thread makes outside the role's lock. Both are read
through `shadowQueryAccessesOnThisThread` and `stepQueryOwnerThreadAccessesOnThisThread`; a host reads
them on the owner thread and reports deltas.

The counting is behind `if constexpr (HostDiagnostics)` inside the predicate templates, where
`HostDiagnostics` is a template argument of `configure`, `predicateFor` and `shadowPredicateFor`. A host
passes its engine's diagnostics constant; with it off, the predicates contain no counter code at all.
Both settings can be instantiated in one build, and
`TickDriverThreadContext.Diagnostics.CountersOnlyWhenEnabled` does.

## §7 One copy per shared library

The masks and counters are `inline thread_local` variables in a header. A program that links one copy
of the code that uses them has one copy of each. On Windows, every DLL that compiles a use of them gets
**its own copy**: measured on 2026-10-16 (MSVC 2022), an `inline thread_local int` set by one DLL read 0
from a second DLL and from the executable. So the scopes and the predicates that must agree must be
compiled into the same shared library. A host that splits its code over several libraries keeps every
read and write of these masks in template code instantiated in one of them, and never in a non-inline
function of another.

The owner counts are thread-locals too (a `static inline thread_local` member of `OwnerThreadCounts`), and
the same one-library rule holds for every `configure`, destructor and owner query of a context.

## §8 Provenance

Transcribed on 2026-10-16 from the anonymous namespace of the brawler's Jolt backend host
(JoltBackendUImpl.cpp at commit a47b6f9, the historical name of TickDriverBackendUImpl.cpp, whose code is the
header TickDriverBackendUImpl.h since og-tickdriver task 15e): the two role-bit constants, the two masks and two counters, the
four predicate functions, the predicate chooser (joltWorldAccessCheckFor), the held check
(checkedNotYetHeld) and the two scope classes. The differences:
- the owner count and `isOwnerThread` replace the engine's game-thread query; `configure` raises the
  count and chooses the predicates, and the destructor lowers it on the configuring thread only;
- the lock scope is a template over a `BasicLockable` lock and an access scope, and takes the context;
- the counters are behind `if constexpr` instead of a preprocessor build switch;
- the shadow predicate is instantiated on the context's role bit;
- the context reads the held and running masks for its role, which the backend host did in two
  member functions.

Nothing in this file depends on any document outside this submodule.
