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
// DeferredLifecycleQueue — A GAME->PHYSICS MARSHALLING POINT FOR LIFECYCLE.
// ⚠ Read who uses it, and the ruling that binds every other caller, below.
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
// THE RULE THIS OBJECT IMPOSES ON A CALLER THAT ADOPTS IT: the game thread
// ENQUEUES, the thread that steps the simulation APPLIES, and it applies
// BETWEEN STEPS — before the step's collect, never during it. A registration
// is therefore invisible on the step it was queued and whole on the next.
// ⚠ THAT IS THE WHOLE OF WHAT IT BUYS, AND IT IS LESS THAN SINGLE-THREADED
// ACCESS. It does NOT make the containers single-threaded by itself:
// registration also mutates SimulationNetSync's `m_authorityWriters`,
// `m_localInputSenders` and `m_redundancyCadences`, which `sendCorrectionAll`
// and `sendLocalInputToAuthorityAll` walk once per frame. Applying here is
// sound only where those walks run on the applying thread too.
//
// -------------------------------------------------------------------------
// WHO USES IT (og-input-latency task P2-17). og-sim-net's sessions each own
// one (`SimNetClient`, `SimNetServer`): the host enqueues a character's
// registration and unregistration there (`registerCharacter`,
// `unregisterCharacter`), and the session applies them at the top of its
// `drain()`, through `registerSimulatable` / `unregisterSimulatable`, with the
// session's own channel as the owner — no game-thread object is touched by the
// apply. On an IMMEDIATE client (`SimNetConfig::immediate`) that drain runs at
// step start on the step's thread and the input send runs at capture on the
// same thread (og-input-latency P2-16), so the apply point and every walk of
// the sender maps share one thread: the precondition below is met there. The
// server session applies at its drain too, which runs where the server drains
// (the game thread when frame-locked, the runner on the authority loop,
// og-input-latency P5-19). Hosts that do not enqueue keep calling the facades
// directly, as before; the Unreal host's wiring to the sessions' queues is
// og-input-latency P5-17 (the client) and P5-19 (the server on the loop).
//
// ⛔ THE EARLIER RULING STILL BINDS EVERY OTHER CALLER. The class was written
// for og-netcode-v2-field-defects task 3, wired into the og-brawler-unreal
// composition root, measured and then HELD at the user's direction, because
// adopting it on a client whose GAME thread still sent INVERTS the
// game/physics crossing instead of closing it: the physics thread becomes the
// sole writer of storage and the resolver's containers while the game-thread
// send ITERATES the sender maps, and an insert under a live iteration is a
// worse failure than the find it would replace.
//
// ⭐ THE PRECONDITION FOR ADOPTING IT: move or snapshot every game-thread reader
// of what registration writes first — the send path (og-input-latency P2-16)
// and the adapter's per-frame visualization, HUD and input-history readers
// (the display snapshot, og-input-latency P2-18 and P5-16). The reader list
// and the ruling are in docs/ThreadingCrossings.md, row 11.
//
// Its semantics are pinned by og-simulation-tests (RegistrationLifecycleTest.cpp),
// and the sessions' use of it by og-sim-net's tests (SimNetLifecycleQueueTest.cpp).
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
// ⛔ ONE PRODUCER, BUT NOT BECAUSE THE PUSH NEEDS IT. The enqueue sites are the
// host's game-thread lifecycle calls (the sessions' `registerCharacter` and
// `unregisterCharacter`). The push is a CAS loop and would tolerate more
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

    // CALL THIS ON THE THREAD THAT STEPS, BETWEEN STEPS — a precondition on the
    // caller. og-sim-net's sessions call it at the top of drain(); see the banner.
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
