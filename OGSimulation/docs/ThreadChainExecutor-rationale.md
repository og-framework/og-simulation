<!-- SPDX-License-Identifier: MPL-2.0 -->
# `ThreadChainExecutor.h` — rationale

The narrative behind `ExecutorThreadHost` and `ThreadChainExecutor`. The header keeps its licence, its docs
pointer, its code and one guard tag; everything that explains it lives here. The one prohibition is in
`ThreadChainExecutor-guards.md`.

**If this file and `ThreadChainExecutor.h` disagree, the header is authoritative and this file is stale.**
Fix this file; do not soften the header to match it.

⚠ **This header is `og-simulation` core and ships standalone under MPL-2.0**, engine-free by construction:
STL only. Everything below is true to a reader with no game engine and no other repository.

---

## §1 What it is for

`SimulationTickDriver` reaches its host through a `TickDriverEngine`: a clock (`nowSeconds`, `frameNumber`), a
log sink (`logEnabled`, `log`) and an **executor**: `Handle`, `Completion`, `isPending`, `waitFor`,
`dispatchAfter` and `deferUntil`. In `FrameBounded` the driver hands each frame's step batch to the executor in
`startFrame` and waits for it in `endFrame`; its `stop` waits for the outstanding batches with `waitFor`.

A host with a task graph builds the executor from its graph's tasks and completion events. A host without one
(a port to an engine whose worker pool does not chain tasks) needs a portable executor instead.
`ThreadChainExecutor` is that executor half:

* **one worker thread**, started through the host's own thread factory (§2);
* **FIFO dispatch, which is the chain**: tasks run one at a time in dispatch order, so a task dispatched after
  another runs after it (§4);
* **no deferral**: `deferUntil` returns false, so `endFrame` blocks until the batch is done (§5);
* **`stop` joins**: it returns after the worker has run every queued task and left its loop (§6).

The host composes its engine from its own clock and log and this executor (§8).

## §2 The host's thread factory: `ExecutorThreadHost`

The executor starts its worker with `host.startThread(name, priority, fn, arg)`: a thread name, an `int`
priority, a `void (*)(void*)` thread function and its argument. This is the thread factory of og-net's
`NetHostServices` concept, restated here because og-simulation includes no other library's header. A host that
already serves og-net serves this executor with the same function.

* The result of `startThread` is ignored, as `NetHostServices` ignores it, so a host that returns a thread
  handle still conforms (self-check `ReturnsAThreadHandle`).
* The priority is the host's own scale, passed through unchanged. A scoped enum is refused
  (`PriorityIsAScopedEnum`), because the executor passes an `int`.
* The name is passed as the caller gave it and is read only during the call; a host that keeps it copies it.
* The executor starts **exactly one** thread, in its constructor, after every member is built. The host must
  start it: there is no failure path, and a thread that never runs leaves every `waitFor` and `stop` waiting.

The worker runs the executor's loop and returns when the executor stops. The host keeps the operating-system
thread and joins it at its own teardown; the executor has no thread object to join (§6).

## §3 Handles and tickets

A `Handle` is `{ executor, ticket }`. Each `dispatchAfter` issues the next ticket, from 1. The executor
counts the tasks it has completed, so a handle is pending while its ticket is above that count. The default
handle (`executor == nullptr`, ticket 0) is never pending, and waiting for it returns at once; the driver
passes it as "no previous batch".

`Handle` is an aggregate: default-constructible and copyable, as `TickDriverEngine` requires, because the
driver keeps the pending and the blocking handle at once.

Every handle a call receives must be the default handle or one this executor issued. A ticket from another
executor says nothing about this queue (§4), so `isPending`, `waitFor`, `dispatchAfter` and `deferUntil`
`OG_CHECK` it. The executor is neither copyable nor movable (a self-check `static_assert`), so the address in a
handle stays valid for the executor's life.

## §4 FIFO is the chain

`dispatchAfter(previous, work)` promises that `work` runs after `previous`. The driver relies on it in
`SimulationTickDriver::pumpAndRunSteps`: a frame's batch is dispatched after the previous frame's batch when
that one is still pending (block mode 0), so batches never overlap and the step driver is only ever run by one
thread at a time.

This executor keeps that promise without looking at `previous`: one worker runs the queue in dispatch order,
one task at a time, and `previous`, issued earlier by the same executor, was queued earlier. That holds only
with one worker and one queue:

* **One worker.** A second worker would run two tasks at once and could finish them out of order. The worker
  `OG_CHECK`s, under the lock, that it is the first to enter the loop.
* **One executor.** `previous` from another executor would not be in this queue. The issued-here check (§3)
  refuses it.

A task may dispatch another (it is queued behind the rest). A task that waits for itself or for a later task
on the worker would wait forever, because the worker is the only thread that can run it: `waitFor` `OG_CHECK`s
that a wait on the worker is for a task already completed.

## §5 No deferral

`SimulationTickDriver::endFrame` asks the executor to defer the post-physics pass until the batch is done, when
the host passes a completion event. If `deferUntil` returns false, `endFrame` waits for the batch with
`waitFor` and runs the pass itself, on the frame thread. This executor always returns false
(guard G-01).

The pass must run on the frame thread: it calls the game's `onPostPhysics`, writes the driver's window and
drains the step-cost ring, which the frame thread consumes. The executor has no frame-thread event loop to run
a held continuation from; running it on the worker would race the frame thread's next `startFrame`, and
holding it without running it would drop the pass.

So the frame thread blocks in `endFrame` until the frame's batch is done (block mode 1) or the previous one is
(block mode 0). That is the frame-bounded contract, and it is what the fakes' executor gives with its deferral
turned off.

## §6 Stop and the join

`stop()` sets the stop request and wakes the worker. The worker runs the rest of the queue, then leaves its
loop and signals that it exited; `stop` waits for that signal. So when `stop` returns:

* every task dispatched before it has run, and no task runs again;
* the worker touches nothing the executor's caller owns.

A second `stop` returns at once, and the destructor calls `stop`. `stop` called by one of the executor's own
tasks would wait for the worker that is running it: `stop` `OG_CHECK`s that it is not on the worker. A
dispatch after `stop` would queue a task that never runs, and a wait for it would never return:
`dispatchAfter` `OG_CHECK`s that no stop was requested.

**Why the state is shared.** The queue, the counters, the lock and the two condition variables live in one
`State` object that the executor and the worker share through a `std::shared_ptr`. The worker receives its own
`shared_ptr` (heap-allocated, because the thread function takes a `void*`) and drops it when it returns. After
signalling the exit the worker still releases its lock, notifies the waiters and destroys its pointer; with the
state shared, those accesses stay valid even when the executor is destroyed the moment `stop` returns. There is
no "the exit signal is the worker's last access" rule to keep.

The operating-system thread is the host's: it ends a few instructions after the signal, and the host joins it
when it tears down.

## §7 Threading and costs

* **One mutex** guards the queue, the counters, the worker's identity and the flags. Every read in the
  executor's own functions takes it, `isPending` included.
* **Tasks run outside the lock.** So a task may dispatch, and `isPending` and `dispatchAfter` from the frame
  thread never wait for a running batch.
* **A completion is counted after the task has run and after the task object, with its captures, has been
  destroyed.** So when `waitFor` returns, the task's work is visible to the waiter (the counter is written and
  read under the lock) and nothing of the task is left on the worker.
* **Wake-ups.** A dispatch and a stop wake the worker with `notify_one`: that condition has one waiter, the worker. A completion and
  the exit wake every waiter with `notify_all`, because the frame thread and a `stop` can wait at once.
* **Allocation.** Each dispatch stores its work in a `std::function`, which allocates when the callable is
  larger than its small buffer. The driver's batch is captured by value (its deadline array alone is
  `simulationTickDriver::kMaxStepsPerFrame` doubles), so a `FrameBounded` frame with steps allocates once. The
  queue is a `std::deque`, which allocates as it grows. The executor itself allocates nothing while a task runs.
* **The work must be copy-constructible**, because `std::function` is; the driver's work is.

## §8 Composing an engine

The executor is a half. A host's engine owns its clock and its log and forwards the executor half:
`Handle` and `Completion` are the executor's, and `isPending`, `waitFor`, `dispatchAfter` and `deferUntil`
forward to it. The self-check `threadChainExecutorSelfCheck::Engine` is that shape, with each forwarder's
return type taken from the executor's own function, and a `static_assert` checks it against
`TickDriverEngine`. A change to an executor function's signature or return type, or to the handle's
copyability, breaks it at compile time.

The host builds the executor before the driver and stops it after the driver's `stop`: the driver waits for
its outstanding batches in its `stop`, and the executor then exits with an empty queue.

## §9 What pins it

Compile time, in the header:
* `executorThreadHostSelfCheck`: one conforming host, one that returns a handle (accepted), and three refused
  shapes (no factory, a scoped-enum priority, a thread function without its argument);
* `threadChainExecutorSelfCheck`: the composed engine satisfies `TickDriverEngine`; the executor is neither
  copy- nor move-constructible.

At run time, `OG_CHECK`s: a second worker; a handle from another executor; a dispatch after stop; a wait on the
worker for a task not yet run; a stop on the worker.

The `[TickDriver]` cases in og-simulation-tests (`ThreadChainExecutorTest.cpp`), over a thread host that
starts each thread as a `std::thread` and records its name, priority and id:

* `ThreadChainExecutor.Host.StartsOneWorkerThroughTheHostWithItsNameAndPriority`: one `startThread` call with
  the given name and priority; the task runs on that thread, not the caller's; the thread function returns
  after the executor is destroyed.
* `ThreadChainExecutor.Fifo.TenThousandDispatchesRunInOrderOneAtATime`: 10,000 dispatches, every other one
  after the previous handle; every task runs on the worker, in dispatch order, never two at once; tickets are
  consecutive. With two workers the case fails on overlaps, tasks off the first worker and the order.
* `ThreadChainExecutor.Stop.JoinsAfterTheWorkerRanEveryQueuedTask`: a 20 ms first task and 99 more, then
  `stop`: all 100 ran, the first had finished, nothing is pending, and a second `stop` is harmless.
* `ThreadChainExecutor.Wait.ReturnsOnlyAfterTheTaskRanAndItsCapturesAreDestroyed`: after `waitFor`, the task's
  effect is visible and its captured `shared_ptr` is released.
* `ThreadChainExecutor.Deferral.DeferUntilNeverHoldsTheContinuation`: false for a pending handle and for the
  default handle; the continuation never runs.
* `ThreadChainExecutor.Conformance.AFrameBoundedClientGivesTheFakeChainedExecutorsStepTrace`, the host
  conformance case: a `FrameBounded` client driver over the fake kit's game, presenter and world host
  (`TickDriverFakes.h`) runs 900 scripted frames (frame lengths from 0.4 to 2.4 steps, so frames with no step and with several, one 250 ms hitch, client
  resimulations, occupancy pushes, shadow restores, frames with and without a completion event, block mode 1)
  once over the fake kit's `FakeEngine` with its worker thread and its deferral turned off, and once over this
  executor. The ordered event trace (every game hook, presenter and world-host door, world and manager call,
  dispatch, task run and log line, the first-step and window lines included), the host times, step start
  times, publish deadlines (compared bit for bit), published ticks, captured and run steps, and the dispatch,
  wait and deferral counts are equal. Each task is held in the test's engine until the frame thread waits for
  it, so both runs take the same `endFrame` path.

Two hidden `[.][OGSimDeath]` cases fire the issued-here and dispatch-after-stop checks on the test thread. The
three checks that fire on the worker have no case: the test runner does not survive a check reported from a
thread other than its own. Each was seen failing when it was written.

## §10 Provenance

**2026-11-19, new.** Added for a host without a chaining task graph: the executor half of a
`TickDriverEngine` on one worker thread from the host's thread factory, with the host conformance case
against the fake kit's chained executor. No game uses it yet; the Unreal host keeps its task-graph executor.
