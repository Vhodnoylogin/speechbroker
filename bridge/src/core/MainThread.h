#pragma once

#include <functional>

namespace Envoy
{
	// The seam "do it where it is safe".
	//
	// The core needs exactly one thing: to put work into the thread where it may
	// be done. Which thread that is belongs to whoever started the core. In the
	// game SKSE hands it over with its task interface; the test host sets up a
	// queue and drains it itself between its own steps, because "right where it
	// was called" would mean "in the thread of the scheduler" for a hold ceiling,
	// alongside taking in the next utterance. With no dispatcher at all the work
	// is done on the spot.
	//
	// The core used to call SKSE::GetTaskInterface directly, and that one line
	// made the auction impossible to run without the game.
	class MainThread
	{
	public:
		using Task = std::function<void()>;

		// Whoever knows how to get work into the main thread.
		class Dispatcher
		{
		public:
			virtual ~Dispatcher() = default;
			virtual void Post(Task a_task) = 0;
		};

		// With no dispatcher set, the work is done right here - in the thread it was
		// called from. For a single thread that is a strictly defined order; anyone
		// with more than one thread is expected to set a dispatcher, the way the
		// test host does.
		static void Install(Dispatcher* a_dispatcher);
		static void Post(Task a_task);
	};
}
