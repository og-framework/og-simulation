#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

#include "OGSimulation/CompilerControl.h"

// pragma optimize off — debugger-friendliness; rationale in SimulationManager.h.
OGSIM_OPTIMIZE_OFF

// ---------------------------------------------------------------------------
// DeferredLifecycleQueue — A GAME->PHYSICS MARSHALLING POINT FOR LIFECYCLE,
// BUILT AND TESTED BUT NOT WIRED. ⚠ Read the ruling below before you use it.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
//
// WHAT IT IS FOR. Simulation lifecycle — `registerSimulatable`,
// `unregisterSimulatable` and everything they reach — mutates
// `SimulationObjectStorage`'s map and five `SimulationInputResolution` maps.
// The per-tick collect walks those same containers on the PHYSICS thread. The
// composition root drives lifecycle from the GAME thread. Running the two
// concurrently is what put `std::out_of_range` from
// `SimulationInputResolution::collectInputForCharacter` on a shipped client's
// crash reporter: `forEachSimulatable` reached a half-registered id, and a
// concurrent `unordered_map` rehash under a reader is undefined behaviour
// whatever the ordering.
//
// THE RULE THIS OBJECT IMPOSES ON A CALLER THAT ADOPTS IT — a CONTRACT on how
// it is to be called, NOT a description of anything happening in this tree
// today: the game thread ENQUEUES, the physics thread APPLIES, and it applies
// BETWEEN STEPS — before the step's collect, never during it. A registration
// is therefore invisible on the step it was queued and whole on the next.
// ⚠ THAT IS THE WHOLE OF WHAT IT BUYS, AND IT IS LESS THAN SINGLE-THREADED
// ACCESS. It does NOT make the containers single-threaded, even once adopted:
// registration also mutates `m_authorityWriters` (SimulationNetSync.h:477) and
// `m_localInputSenders` (:379), which the GAME thread range-fors every tick at
// :599 and :656 — so adopting this queue would put a physics-thread insert
// under a live game-thread iteration. That is the held ruling below, and it is
// the reason this class has no consumer.
//
// -------------------------------------------------------------------------
// ⛔⛔ NOTHING IN THIS REPOSITORY USES THIS CLASS YET, AND THAT IS A RULING,
// NOT AN OVERSIGHT. Read this before wiring it into an adapter.
//
// It was written, wired into the og-brawler-unreal composition root, built and
// measured for og-netcode-v2-field-defects task 3, and then HELD at the user's
// direction, because adopting it INVERTS the game/physics crossing instead of
// closing it. Making the physics thread the sole writer of storage and the
// resolver's containers moves the exposure onto the GAME-thread readers — and
// two of those ITERATE rather than look up: SimulationNetSync::sendCorrectionAll
// walks m_authorityWriters and sendLocalInputToAuthorityAll walks
// m_localInputSenders, both once per tick. An insert under a live iteration is
// a worse failure than the find it would replace.
//
// ⭐ THE PRECONDITION FOR ADOPTING IT: marshal or snapshot the game-thread send
// path first, then the adapter's per-frame visualization and HUD readers. The
// full reader list and the ruling are in docs/ThreadingCrossings.md, row 11.
//
// The class ships anyway, with its semantics pinned by og-simulation-tests
// (RegistrationLifecycleTest.cpp), so the adoption step is a wiring change
// against a tested primitive rather than a design task done twice.
// -------------------------------------------------------------------------
//
// ⛔ IT IS NOT A LOCK, AND MUST NOT BECOME ONE. A mutex over the containers was
// considered and rejected at the bug report: the physics thread would block
// inside the physics step on an arbitrary amount of game-thread registration
// work, and it would still leave `forEachSimulatable` free to see a
// half-registered id. This is a Treiber-style intrusive stack — one atomic
// compare-exchange to push, one atomic exchange to take the whole chain — so
// neither thread can wait on the other at all.
//
// ⛔ ONE PRODUCER, BUT NOT BECAUSE THE PUSH NEEDS IT. Both enqueue sites are
// game-thread lifecycle calls. The push is a CAS loop and would tolerate more
// producers; the single CONSUMER is the load-bearing half, because
// `applyPending` takes the entire chain and each node is thereafter owned
// exclusively by the caller. Taking the whole chain is also what makes ABA
// unreachable — no node is ever popped individually.
//
// ⛔ FIFO, AND THE REVERSAL IS WHAT BUYS IT. A push-down stack hands back the
// newest node first; `applyPending` reverses the taken chain before running it.
// Order is load-bearing: a register and the unregister that follows it are two
// jobs for the same id, and applying them the other way round leaks the
// registration for the rest of the session.
//
// ⛔ MOVE-ONLY PAYLOADS ARE THE POINT, so this cannot be a vector of
// `std::function`. A registration job owns the simulatable by value and a
// simulatable is not copyable; `std::function` requires CopyConstructible and
// `std::move_only_function` is C++23, one standard past this core.
//
// ⛔ THE NODE IS ALLOCATED ON THE GAME THREAD AND FREED ON THE PHYSICS THREAD,
// once per character lifetime event. That is deliberate and priced: the
// alternative is a fixed-capacity ring, whose overflow behaviour on the
// unregister side has no safe answer — an unregistration that cannot be queued
// cannot be retried, because its owner is already leaving.
// ---------------------------------------------------------------------------
class DeferredLifecycleQueue
{
public:
    DeferredLifecycleQueue() = default;

    // ⛔ DISCARDS, NEVER APPLIES. A queue destroyed with work still in it is a
    // composition root shutting down; running a registration against
    // half-destructed peers would be worse than dropping it.
    ~DeferredLifecycleQueue()
    {
        Node* chain = m_head.exchange(nullptr, std::memory_order_acquire);
        while (chain != nullptr)
        {
            Node* next = chain->next;
            delete chain;
            chain = next;
        }
    }

    DeferredLifecycleQueue(const DeferredLifecycleQueue&)            = delete;
    DeferredLifecycleQueue& operator=(const DeferredLifecycleQueue&) = delete;
    DeferredLifecycleQueue(DeferredLifecycleQueue&&)                 = delete;
    DeferredLifecycleQueue& operator=(DeferredLifecycleQueue&&)      = delete;

    // GAME THREAD. Publishes the job; touches nothing the physics thread reads
    // except the head pointer.
    template <typename FnT>
    void enqueue(FnT&& fn)
    {
        using JobT = TypedNode<std::decay_t<FnT>>;
        Node* node = new JobT(std::forward<FnT>(fn));

        Node* head = m_head.load(std::memory_order_relaxed);
        do
        {
            node->next = head;
        }
        // ⛔ RELEASE ON SUCCESS — it is what publishes the captured payload, not
        // just the pointer, to whichever thread next takes the chain.
        while (!m_head.compare_exchange_weak(head, node,
                                             std::memory_order_release,
                                             std::memory_order_relaxed));
    }

    // CALL THIS ON THE PHYSICS THREAD, BETWEEN STEPS — a precondition on the
    // caller, not a claim about one. (No caller exists; see the banner.)
    // Returns how many jobs ran.
    // ⛔ THE ACQUIRE PAIRS WITH enqueue's RELEASE and is the whole synchronisation.
    unsigned int applyPending()
    {
        Node* taken = m_head.exchange(nullptr, std::memory_order_acquire);

        Node* ordered = nullptr;
        while (taken != nullptr)
        {
            Node* next  = taken->next;
            taken->next = ordered;
            ordered     = taken;
            taken       = next;
        }

        unsigned int applied = 0u;
        while (ordered != nullptr)
        {
            Node* next = ordered->next;
            ordered->apply();
            delete ordered;
            ++applied;
            ordered = next;
        }
        return applied;
    }

    // Either thread. A hint, not a fence: the game thread may be mid-push.
    bool hasPending() const
    {
        return m_head.load(std::memory_order_acquire) != nullptr;
    }

private:
    struct Node
    {
        Node*        next = nullptr;
        virtual ~Node()   = default;
        virtual void apply() = 0;
    };

    template <typename FnT>
    struct TypedNode final : Node
    {
        template <typename U>
        explicit TypedNode(U&& f) : fn(std::forward<U>(f)) {}
        void apply() override { fn(); }
        FnT fn;
    };

    std::atomic<Node*> m_head{ nullptr };
};

OGSIM_OPTIMIZE_ON
// pragma optimize on.
