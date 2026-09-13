#include "ModEventBus.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <mutex>

namespace Envoy
{
	void ModEventBus::Send(const std::string& a_event, const std::string& a_string, float a_number)
	{
		auto* source = SKSE::GetModCallbackEventSource();

		// The direction "bridge -> Papyrus" has never once been confirmed in VR: the
		// other way round works (the subscribers did register), but there is no proof
		// for this one. Once per launch we say plainly whether there is anywhere to
		// send at all.
		static std::once_flag once;
		std::call_once(once, [source] {
			if (source) {
				SKSE::log::info("Papyrus event delivery is ready");
			} else {
				SKSE::log::error("Papyrus event delivery is unavailable: "
				                 "not one event will reach the scripts");
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
