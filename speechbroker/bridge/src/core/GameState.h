#pragma once

#include <string>

namespace SpeechBroker
{
	// The seam "what is going on in the game right now".
	//
	// An utterance gets its topic from the state of the game, not from the sense
	// of the phrase: something said outside the dialogue window will never reach
	// the dialogue subscribers. The core itself knows nothing about the game and
	// asks through this seam.
	//
	// Outside the game the default source answers, and in it nothing happens:
	// menus closed, no pause, no combat. A check needs more than that, so it sets
	// a source of its own and says in it whatever it wants.
	class GameState
	{
	public:
		class Source
		{
		public:
			virtual ~Source() = default;

			virtual bool IsMenuOpen(const std::string& a_name) const = 0;
			virtual bool IsPaused() const = 0;
			virtual bool IsInCombat() const = 0;
		};

		static void          Install(Source* a_source);
		static const Source& Get();
	};
}
