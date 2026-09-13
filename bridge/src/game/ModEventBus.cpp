#include "ModEventBus.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <mutex>

namespace Envoy
{
	void ModEventBus::Send(const std::string& a_event, const std::string& a_string, float a_number)
	{
		auto* source = SKSE::GetModCallbackEventSource();

		// Направление "мост -> Papyrus" в VR ни разу не подтверждено: обратное
		// работает (подписчики зарегистрировались), а сюда доказательств нет.
		// Один раз за запуск говорим прямо, есть ли вообще куда слать.
		static std::once_flag once;
		std::call_once(once, [source] {
			if (source) {
				SKSE::log::info("рассылка событий Papyrus готова");
			} else {
				SKSE::log::error("рассылка событий Papyrus недоступна: "
				                 "ни одно событие до скриптов не дойдёт");
			}
		});

		if (!source) {
			return;
		}

		SKSE::ModCallbackEvent event{
			RE::BSFixedString{ a_event },
			RE::BSFixedString{ a_string },
			a_number,
			nullptr
		};
		source->SendEvent(&event);
	}
}
