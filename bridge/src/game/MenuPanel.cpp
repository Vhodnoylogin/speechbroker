#include "MenuPanel.h"

#include "core/Log.h"
#include "core/Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <filesystem>
#include <string>

// Идёт последним нарочно: заголовок чужой, объявляет RE::InputEvent в своих
// подписях и подключает windows.h, а CommonLibSSE обязан увидеть его первым.
//
// Предупреждения в нём глушим: мост собирается с /W4 /WX, и чужой заголовок
// иначе ронял бы сборку из-за кода, который мы не писали и не правим. На наш
// собственный код это не распространяется - pop возвращает строгость.
#pragma warning(push, 0)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

namespace Envoy
{
	namespace
	{
		// Уровни перечислены здесь, а не собраны из журнала: порядок в списке -
		// от самого подробного к самому молчаливому, и он осмысленный, а не
		// алфавитный.
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
			ImGuiMCP::Text("Журнал моста");
			ImGuiMCP::Separator();

			int chosen = IndexOf(Log::Level());
			for (int i = 0; i < static_cast<int>(std::size(kLevels)); ++i) {
				if (ImGuiMCP::RadioButton(kLevels[i], &chosen, i)) {
					Log::SetLevel(kLevels[i]);
					SKSE::log::info("уровень журнала переключён из меню: {}", kLevels[i]);
				}
				if (i + 1 < static_cast<int>(std::size(kLevels))) {
					ImGuiMCP::SameLine();
				}
			}

			ImGuiMCP::Separator();

			bool speech = Log::ShowSpeech();
			if (ImGuiMCP::Checkbox("Записывать распознанные слова", &speech)) {
				Log::SetShowSpeech(speech);
				SKSE::log::info("слова игрока в журнале переключены из меню: {}",
					speech ? "пишем" : "не пишем");
			}
			ImGuiMCP::TextUnformatted(
				"Выключено - в журнал идут номер реплики и модель, но не сами слова.\n"
				"Включённая запись оставляет на диске расшифровку всего сказанного вслух.");

			ImGuiMCP::Separator();
			ImGuiMCP::TextUnformatted(
				"Изменения действуют до конца сессии.\n"
				"Постоянное значение - ключи log.level и log.speechText\n"
				"в Data/SKSE/Plugins/envoy/envoy.json.");
		}
	}

	void MenuPanel::Install()
	{
		if (!SKSEMenuFramework::IsInstalled()) {
			SKSE::log::info("SKSE Menu Framework не установлен - окна в игре не будет, "
			                "журнал настраивается файлом");
			return;
		}
		SKSEMenuFramework::SetSection("Envoy");
		SKSEMenuFramework::AddSectionItem("Журнал", RenderLog);
		SKSE::log::info("окно моста добавлено в меню модов");
	}
}
