#include "MainThread.h"

namespace Envoy
{
	namespace
	{
		MainThread::Dispatcher* g_dispatcher = nullptr;
	}

	void MainThread::Install(Dispatcher* a_dispatcher)
	{
		g_dispatcher = a_dispatcher;
	}

	void MainThread::Post(Task a_task)
	{
		if (!a_task) {
			return;
		}

		if (g_dispatcher) {
			g_dispatcher->Post(std::move(a_task));
			return;
		}

		a_task();
	}
}
