#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace SpeechBroker
{
	// One thread for everything that waits.
	//
	// Each utterance used to start a detached thread of its own, only to sleep
	// through the bid window and wake the settling up. That is one thread per
	// phrase and dozens per conversation, all of them living on their own: there
	// is nothing to stop them with when the game exits, and what they touch may
	// by then be gone.
	//
	// A task runs in the thread of the scheduler. One that needs the thread of the
	// game is expected to put itself there - the scheduler knows nothing about
	// the game.
	class Scheduler
	{
	public:
		static Scheduler& Get();

		void After(std::chrono::milliseconds a_delay, std::function<void()> a_task);
		void Stop();

		~Scheduler();

	private:
		Scheduler() = default;

		void Run();

		struct Item
		{
			std::chrono::steady_clock::time_point at;
			std::function<void()>                 task;

			// A heap hands back the largest element and what we want is the nearest in
			// time, so the comparison is turned around.
			bool operator<(const Item& a_other) const { return at > a_other.at; }
		};

		std::mutex                                    _mutex;
		std::condition_variable                       _wake;
		std::priority_queue<Item, std::vector<Item>>  _queue;
		std::thread                                   _thread;
		bool                                          _running{ false };
	};
}
