#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace Envoy
{
	// Один поток на все отложенные дела.
	//
	// Прежде каждая реплика заводила свой отсоединённый поток - только чтобы
	// поспать окно ставок и разбудить подведение итога. На фразу это поток,
	// на разговор - десятки, и все они живут сами по себе: остановить их при
	// выходе из игры нечем, а трогают они то, чего к тому времени может уже
	// не быть.
	//
	// Задача выполняется в потоке планировщика. Той, которой нужен игровой
	// поток, полагается положить себя туда самой - планировщик про игру
	// не знает ничего.
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

			// Куча отдаёт наибольший элемент, а нам нужен ближайший по времени,
			// поэтому сравнение перевёрнуто.
			bool operator<(const Item& a_other) const { return at > a_other.at; }
		};

		std::mutex                                    _mutex;
		std::condition_variable                       _wake;
		std::priority_queue<Item, std::vector<Item>>  _queue;
		std::thread                                   _thread;
		bool                                          _running{ false };
	};
}
