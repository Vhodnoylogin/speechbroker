#include "GameState.h"

namespace Envoy
{
	namespace
	{
		// Мир, в котором не происходит ничего. Это не отказ отвечать, а честный
		// ответ: вне игры меню действительно закрыты и боя действительно нет.
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
