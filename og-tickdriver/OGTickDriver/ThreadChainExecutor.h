#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/ThreadChainExecutor-rationale.md · docs/ThreadChainExecutor-guards.md

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

#include "OGSimulation/OGAssert.h"
#include "OGTickDriver/TickDriverConcepts.h"
#include "OGTickDriver/TickDriverLog.h"

template <typename H>
concept ExecutorThreadHost = requires(H& h, const char* name, int priority, void (*fn)(void*), void* arg)
{
	{ h.startThread(name, priority, fn, arg) };
};

namespace executorThreadHostSelfCheck
{
struct Conforming
{
	void startThread(const char*, int, void (*)(void*), void*);
};

struct ReturnsAThreadHandle
{
	int startThread(const char*, int, void (*)(void*), void*);
};

struct MissingStartThread
{
};

struct PriorityIsAScopedEnum
{
	enum class Priority : uint8_t
	{
		Normal,
	};

	void startThread(const char*, Priority, void (*)(void*), void*);
};

struct ThreadFunctionTakesNoArgument
{
	void startThread(const char*, int, void (*)());
};

static_assert(ExecutorThreadHost<Conforming>,
	"ExecutorThreadHost self-check: the reference shape must satisfy the concept");
static_assert(ExecutorThreadHost<ReturnsAThreadHandle>,
	"ExecutorThreadHost self-check: startThread's result is ignored, as og-net's NetHostServices ignores it, so a host "
	"that returns a thread handle still conforms");
static_assert(!ExecutorThreadHost<MissingStartThread>,
	"ExecutorThreadHost self-check: startThread (the host's thread factory) is required");
static_assert(!ExecutorThreadHost<PriorityIsAScopedEnum>,
	"ExecutorThreadHost self-check: the priority is a plain int, the host's own priority scale, as in og-net's "
	"NetHostServices");
static_assert(!ExecutorThreadHost<ThreadFunctionTakesNoArgument>,
	"ExecutorThreadHost self-check: the thread function takes the void* argument it is started with");
} // namespace executorThreadHostSelfCheck

class ThreadChainExecutor
{
public:
	struct Handle
	{
		const ThreadChainExecutor* executor = nullptr;
		uint64_t ticket = 0u;
	};

	struct Completion
	{
	};

	template <ExecutorThreadHost HostT>
	ThreadChainExecutor(HostT& host, const char* threadName, int threadPriority)
		: m_state(std::make_shared<State>())
	{
		auto* const workerState = new std::shared_ptr<State>(m_state);
		host.startThread(threadName, threadPriority, &ThreadChainExecutor::workerMain, workerState);
	}

	~ThreadChainExecutor() { stop(); }

	ThreadChainExecutor(const ThreadChainExecutor&) = delete;
	ThreadChainExecutor& operator=(const ThreadChainExecutor&) = delete;
	ThreadChainExecutor(ThreadChainExecutor&&) = delete;
	ThreadChainExecutor& operator=(ThreadChainExecutor&&) = delete;

	bool isPending(const Handle& handle) const
	{
		checkIssuedHere(handle);
		const std::lock_guard<std::mutex> lock(m_state->mutex);
		return handle.ticket > m_state->completed;
	}

	void waitFor(const Handle& handle)
	{
		checkIssuedHere(handle);
		std::unique_lock<std::mutex> lock(m_state->mutex);
		OG_CHECK(handle.ticket <= m_state->completed || std::this_thread::get_id() != m_state->workerThread,
			"ThreadChainExecutor::waitFor: the worker waited for a task it has not run. The worker runs the queue in order, one "
			"task at a time, so a task that waits for itself or for a later task waits forever");
		m_state->done.wait(lock, [this, &handle] { return handle.ticket <= m_state->completed; });
	}

	template <typename F>
	Handle dispatchAfter(const Handle& previous, F&& work)
	{
		checkIssuedHere(previous);
		std::function<void()> task(std::forward<F>(work));
		Handle issued{ this, 0u };
		{
			const std::lock_guard<std::mutex> lock(m_state->mutex);
			OG_CHECK(!m_state->stopRequested,
				"ThreadChainExecutor::dispatchAfter: a dispatch after stop. The worker has run its last task or is running it, "
				"so this task would never run and a wait for it would never return");
			issued.ticket = ++m_state->issued;
			m_state->queue.push_back(std::move(task));
		}
		m_state->work.notify_one();
		return issued;
	}

	template <typename F>
	bool deferUntil(Completion&, const Handle& handle, F&&)
	{
		checkIssuedHere(handle);
		// ⛔G-01  docs/ThreadChainExecutor-guards.md
		return false;
	}

	void stop()
	{
		{
			const std::lock_guard<std::mutex> lock(m_state->mutex);
			OG_CHECK(std::this_thread::get_id() != m_state->workerThread,
				"ThreadChainExecutor::stop: called on the worker, by one of its own tasks. stop waits for the worker to run the "
				"rest of the queue and exit, which it cannot do while this task waits");
			m_state->stopRequested = true;
		}
		m_state->work.notify_one();
		std::unique_lock<std::mutex> lock(m_state->mutex);
		m_state->done.wait(lock, [this] { return m_state->exited; });
	}

private:
	struct State
	{
		std::mutex mutex;
		std::condition_variable work;
		std::condition_variable done;
		std::deque<std::function<void()>> queue;
		uint64_t issued = 0u;
		uint64_t completed = 0u;
		uint32_t workers = 0u;
		std::thread::id workerThread;
		bool stopRequested = false;
		bool exited = false;
	};

	static void workerMain(void* workerState)
	{
		const std::shared_ptr<State> state = std::move(*static_cast<std::shared_ptr<State>*>(workerState));
		delete static_cast<std::shared_ptr<State>*>(workerState);

		{
			const std::lock_guard<std::mutex> lock(state->mutex);
			OG_CHECK(state->workers == 0u,
				"ThreadChainExecutor: a second worker started. The executor's FIFO order is its chain: a task dispatched after "
				"another runs after it because one thread runs the queue in order. A second worker runs two tasks at once");
			++state->workers;
			state->workerThread = std::this_thread::get_id();
		}

		for (;;)
		{
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(state->mutex);
				state->work.wait(lock, [&state] { return state->stopRequested || !state->queue.empty(); });
				if (state->queue.empty())
					break;
				task = std::move(state->queue.front());
				state->queue.pop_front();
			}
			task();
			task = nullptr;
			{
				const std::lock_guard<std::mutex> lock(state->mutex);
				++state->completed;
			}
			state->done.notify_all();
		}

		{
			const std::lock_guard<std::mutex> lock(state->mutex);
			state->exited = true;
		}
		state->done.notify_all();
	}

	void checkIssuedHere(const Handle& handle) const
	{
		OG_CHECK(handle.executor == nullptr || handle.executor == this,
			"ThreadChainExecutor: a handle from another executor. FIFO order is the chain only within one executor's queue; "
			"its ticket says nothing about this queue");
	}

	std::shared_ptr<State> m_state;
};

namespace threadChainExecutorSelfCheck
{
struct Engine
{
	static constexpr bool kHostDiagnostics = true;
	using Handle = ThreadChainExecutor::Handle;
	using Completion = ThreadChainExecutor::Completion;

	double nowSeconds() const;
	uint64_t frameNumber() const;
	bool logEnabled(HostLogLevel) const;
	void log(HostLogLevel, const char*);

	auto isPending(const Handle& handle) const -> decltype(std::declval<const ThreadChainExecutor&>().isPending(handle));
	auto waitFor(const Handle& handle) -> decltype(std::declval<ThreadChainExecutor&>().waitFor(handle));

	template <typename F>
	auto dispatchAfter(const Handle& previous, F&& work)
		-> decltype(std::declval<ThreadChainExecutor&>().dispatchAfter(previous, std::forward<F>(work)));

	template <typename F>
	auto deferUntil(Completion& completion, const Handle& handle, F&& continuation)
		-> decltype(std::declval<ThreadChainExecutor&>().deferUntil(completion, handle, std::forward<F>(continuation)));
};

static_assert(TickDriverEngine<Engine>,
	"ThreadChainExecutor self-check: an engine whose executor half forwards to a ThreadChainExecutor (isPending, waitFor, "
	"dispatchAfter, deferUntil, Handle, Completion) satisfies TickDriverEngine");
static_assert(!std::is_copy_constructible_v<ThreadChainExecutor> && !std::is_move_constructible_v<ThreadChainExecutor>,
	"ThreadChainExecutor self-check: an executor is neither copied nor moved; its handles name it by address");
} // namespace threadChainExecutorSelfCheck
