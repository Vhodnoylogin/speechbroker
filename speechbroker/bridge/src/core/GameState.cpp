#include "GameState.h"

namespace SpeechBroker
{
	namespace
	{
		// A world in which nothing happens. This is not a refusal to answer but an
		// honest one: outside the game the menus really are closed and there really
		// is no combat.
		class Quiet final : public GameState::Source
		{
		public:
			bool IsMenuOpen(const std::string&) const override { return false; }
			bool IsPaused() const override { return false; }
			bool IsInCombat() const override { return false; }
		};

		Quiet                g_quiet;
		GameState::Source* g_source = nullptr;
	}

	void GameState::Install(Source* a_source)
	{
		g_source = a_source;
	}

	const GameState::Source& GameState::Get()
	{
		return g_source ? *g_source : static_cast<const Source&>(g_quiet);
	}
}
