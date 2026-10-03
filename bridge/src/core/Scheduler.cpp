#include "Scheduler.h"

namespace SpeechBroker
{
	Scheduler& Scheduler::Get()
	{
		static Scheduler instance;
		return instance;
	}

	Scheduler::~Scheduler()
	{
		Stop();
	}

	void Scheduler::After(std::chrono::milliseconds a_delay, std::function<void()> a_task)
	{
		if (!a_task) {
			return;
		}

		{
			std::scoped_lock lock(_mutex);
			_queue.push(Item{ std::chrono::steady_clock::now() + a_delay, std::move(a_task) });
			if (!_running) {
				_running = true;
				_thread = std::thread([this] { Run(); });
			}
		}
		_wake.notify_one();
	}

	void Scheduler::Stop()
	{
		{
			std::scoped_lock lock(_mutex);
			if (!_running) {
				return;
			}
			_running = false;
		}
		_wake.notify_all();
		if (_thread.joinable()) {
			_thread.join();
		}
	}

	void Scheduler::Run()
	{
		std::unique_lock lock(_mutex);
		while (_running) {
			if (_queue.empty()) {
				_wake.wait(lock);
				continue;
			}

			const auto at = _queue.top().at;
			if (std::chrono::steady_clock::now() < at) {
				// Waiting exactly until the nearest task is due rather than a fixed step: a
				// new task may fall earlier, and then we are woken.
				_wake.wait_until(lock, at);
				continue;
			}

			auto task = std::move(const_cast<Item&>(_queue.top()).task);
			_queue.pop();

			// The task runs with the lock let go: it is entitled to queue the next one.
			lock.unlock();
			task();
			lock.lock();
		}
	}
}
