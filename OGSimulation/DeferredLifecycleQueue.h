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
// DeferredLifecycleQueue — A MARSHALLING POINT FOR LIFECYCLE, ONTO THE STEP'S THREAD.
// ⚠ Read its precondition, and the ruling that binds every caller, below.
//
// Layer: OGSimulation. Adapter-agnostic, UE/Chaos-free.
//
// WHAT IT IS FOR. Simulation lifecycle — `registerSimulatable`,
// `unregisterSimulatable` and everything they reach — mutates
// `SimulationObjectStorage`'s map and five `SimulationInputResolution` maps.
// The per-tick collect walks those same containers on the step's thread. A
// host's lifecycle calls come from whatever thread it registers on. Running the
// two concurrently is what put `std::out_of_range` from
// `SimulationInputResolution::collectInputForCharacter` on a shipped client's
// crash reporter: `forEachSimulatable` reached a half-registered id, and a
// concurrent `unordered_map` rehash under a reader is undefined behaviour
// whatever the ordering.
//
// THE RULE THIS OBJECT IMPOSES ON A CALLER THAT ADOPTS IT: the caller
// ENQUEUES, the thread that steps the simulation APPLIES, and it applies
// BETWEEN STEPS — before the step's collect, never during it. A registration
// is therefore invisible on the step it was queued and whole on the next.
// ⚠ THAT IS THE WHOLE OF WHAT IT BUYS, AND IT IS LESS THAN SINGLE-THREADED
// ACCESS. It does NOT make the containers single-threaded by itself:
// registration also mutates SimulationNetSync's `m_authorityWriters`,
// `m_localInputSenders` and `m_redundancyCadences`, which `sendCorrectionAll`
// and `sendLocalInputToAuthorityAll` walk on every send. Applying here is
// sound only where those walks run on the applying thread too.
//
// -------------------------------------------------------------------------
// ITS USE. Each adopter applies at its own drain, through
// `registerSimulatable` / `unregisterSimulatable`, with an owner of its own, so
// the apply touches no object of the enqueuing thread's. Who adopts it, and how
// each meets the precondition below, is docs/ThreadingCrossings.md row 11
// (og-input-latency P2-17 onward).
//
// ⛔ THE EARLIER RULING STILL BINDS EVERY CALLER. The class was written
// for og-netcode-v2-field-defects task 3, wired into the og-brawler-unreal
// composition root, measured and then HELD at the user's direction, because
// adopting it on a client whose sends ran on another thread than the apply
// INVERTS the crossing instead of closing it: the step's thread becomes the
// sole writer of storage and the resolver's containers while the other
// thread's send ITERATES the sender maps, and an insert under a live iteration
// is a worse failure than the find it would replace.
//
// ⭐ THE PRECONDITION FOR ADOPTING IT: move or snapshot every reader on another
// thread of what registration writes first — the send path (og-input-latency
// P2-16) and the per-frame visualization, HUD and input-history readers (the
// display snapshot, og-input-latency P2-18 and P5-16). The reader list and the
// ruling are in docs/ThreadingCrossings.md, row 11.
//
// Its semantics are pinned by og-simulation-tests (RegistrationLifecycleTest.cpp),
// and the sessions' use of it by og-sim-net's tests (SimNetLifecycleQueueTest.cpp).
// -------------------------------------------------------------------------
//
// ⛔ IT IS NOT A LOCK, AND MUST NOT BECOME ONE. A mutex over the containers was
// considered and rejected at the bug report: the step's thread would block
// inside the step on an arbitrary amount of the enqueuing thread's registration
// work, and it would still leave `forEachSimulatable` free to see a
// half-registered id. This is a Treiber-style intrusive stack — one atomic
// compare-exchange to push, one atomic exchange to take the whole chain — so
// neither thread can wait on the other at all.
//
// ⛔ ANY NUMBER OF PRODUCERS, ONE CONSUMER. The enqueue sites are a host's
// lifecycle calls, which may come from more than one thread (a register from
// one, an unregister from another); the push is a CAS loop and tolerates
// that. The single CONSUMER is the load-bearing half, because
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
/// ⛔ THE NODE IS ALLOCATED ON THE ENQUEUING THREAD AND FREED ON THE STEP'S THREAD,
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

    // ANY PRODUCER THREAD. Publishes the job; touches nothing the step's thread reads
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

    // CALL THIS ON THE THREAD THAT STEPS, BETWEEN STEPS — a precondition on the
    // caller. An adopter calls it at the top of its drain; see the banner.
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

    // Any thread. A hint, not a fence: a producer may be mid-push.
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
