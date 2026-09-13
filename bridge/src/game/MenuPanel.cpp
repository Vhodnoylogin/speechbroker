#include "MenuPanel.h"

#include "core/Loc.h"
#include "core/Log.h"
#include "core/Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <filesystem>
#include <string>

// Deliberately last: the header is someone else's, it names RE::InputEvent in
// its own signatures and it pulls in windows.h, and CommonLibSSE has to be the
// one that sees that first.
//
// Its warnings are muted: the bridge is built with /W4 /WX, and a foreign header
// would otherwise fail the build over code we neither wrote nor may fix. Our own
// code is not covered by this - the pop puts the strictness back.
#pragma warning(push, 0)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

namespace Envoy
{
	namespace
	{
		// The levels are listed here rather than gathered from the log because
		// the order matters: from the most talkative to the most silent. That is
		// a meaning, not an alphabet.
		//
		// They are not translated either. These five words are the values of the
		// log.level key in the settings file, and a person reading the window has
		// to be able to type what they see into that file.
		constexpr const char* kLevels[] = { "trace", "debug", "info", "warning", "error" };

		int IndexOf(const std::string& a_level)
		{
			for (int i = 0; i < static_cast<int>(std::size(kLevels)); ++i) {
				if (a_level == kLevels[i]) {
					return i;
				}
			}
			return 2;  // info
		}

		void __stdcall RenderLog()
		{
			ImGuiMCP::Text(Loc::Get("$ENVOY_LOG_TITLE"));
			ImGuiMCP::Separator();

			ImGuiMCP::TextUnformatted(Loc::Get("$ENVOY_LOG_LEVEL"));

			int chosen = IndexOf(Log::Level());
			for (int i = 0; i < static_cast<int>(std::size(kLevels)); ++i) {
				if (ImGuiMCP::RadioButton(kLevels[i], &chosen, i)) {
					Log::SetLevel(kLevels[i]);
					SKSE::log::info("log level switched from the menu: {}", kLevels[i]);
				}
				if (i + 1 < static_cast<int>(std::size(kLevels))) {
					ImGuiMCP::SameLine();
				}
			}

			ImGuiMCP::Separator();

			bool speech = Log::ShowSpeech();
			if (ImGuiMCP::Checkbox(Loc::Get("$ENVOY_LOG_SPEECH"), &speech)) {
				Log::SetShowSpeech(speech);
				SKSE::log::info("writing the player's words to the log switched from the menu: {}",
					speech ? "on" : "off");
			}
			ImGuiMCP::TextUnformatted(Loc::Get("$ENVOY_LOG_SPEECH_HELP"));

			ImGuiMCP::Separator();
			ImGuiMCP::TextUnformatted(Loc::Get("$ENVOY_LOG_SESSION_HELP"));
		}
	}

	void MenuPanel::Install()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			SKSE::log::info("SKSE Menu Framework is not installed, so there is no window in the "
			                "game; the log is still set from the file");
			return;
		}
		// The section keeps the name of the mod on purpose: a player hunting for
		// it in a list of a dozen sections looks for the name on the Nexus page,
		// not for a translation of it.
		SKSEMenuFramework::SetSection("Envoy");
		SKSEMenuFramework::AddSectionItem(Loc::Get("$ENVOY_MENU_LOG"), RenderLog);
		SKSE::log::info("bridge window added to the mod menu");
	}
}
