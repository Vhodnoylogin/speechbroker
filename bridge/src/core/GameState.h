#pragma once

#include <string>

namespace Envoy
{
	// Шов «что сейчас происходит в игре».
	//
	// Тема реплики выбирается по состоянию игры, а не по смыслу фразы: сказанное
	// вне окна диалога до подписчиков диалога не дойдёт никогда. Само ядро об
	// игре не знает ничего и спрашивает через этот шов.
	//
	// Вне игры отвечает источник по умолчанию, у которого не происходит ничего:
	// меню закрыты, пауза снята, боя нет. Проверке этого мало, поэтому она
	// ставит свой источник и говорит в нём, что хочет.
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
