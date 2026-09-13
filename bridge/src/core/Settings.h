#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Envoy
{
	// Разобранные настройки вместо указателей в документе JSON.
	//
	// Файл читается дважды за сессию - при первом обращении и по ReloadSettings, -
	// а в бою берутся готовые поля. До этого порядок участников извлекался из
	// JSON на каждое сравнение внутри сортировки ставок, с выделением вектора
	// строк на каждое; пороги склеивали указатель из кусков на каждую ставку.
	// Порядок тем и имена окон диалога точно так же доставались указателем
	// на каждую реплику - теперь и они здесь.
	//
	// Значение в инициализаторе поля - единственное запасное в коде: на него
	// падает Read, если ключа в файле нет. Эталон живёт в envoy.default.json,
	// и в норме сюда попадает именно он; повторять число ещё и в Read нельзя -
	// три копии одного умолчания разойдутся при первой правке.
	class Settings
	{
	public:
		static constexpr std::size_t kNoPriority = static_cast<std::size_t>(-1);

		static const Settings& Get();
		static void            Reload();

		std::int32_t bidWindowMs{ 750 };
		// Сколько риска мы согласны терпеть, отдавая реплику. Настраивается
		// именно допуск, а не порог завершённости: порог считается от него
		// и от состава зала.
		//
		// Значение выбрано не на глаз. Калибровка по 22 размеченным записям
		// дала БЕЗОПАСНЫЙ порог завершённости 0.33 - ниже него оборванные
		// фразы встречаются, выше не встретилось ни одной. Допуск подобран
		// так, чтобы у самого дешёвого зала - одного обычного подписчика
		// с весом 0.4 - граница пришлась ровно на него: 0.4 * (1 - 0.33).
		// Для дорогого зала та же формула сама даёт границу строже, 0.73.
		float        holdTolerance{ 0.27f };
		float        minUtteranceScore{ 0.4f };
		bool         sharedWinsTie{ true };
		double       utteranceTtlSec{ 30.0 };
		std::size_t  utteranceMaxStored{ 64 };

		// Журнал. Уровень и предел размера читаются отсюда, а не из сырого
		// документа: их меняют на ходу - из меню в игре и по ReloadSettings, -
		// и значение должно быть в одном месте.
		std::string  logLevel{ "info" };
		std::int32_t logMaxSizeKb{ 4096 };
		// Писать ли в журнал СЛОВА игрока. По умолчанию нет, и это не мелочь:
		// иначе у человека в папке журналов копится расшифровка всего, что он
		// говорил вслух дома. Включается сознательно - из меню или из файла.
		bool         logSpeechText{ false };

		// В каком порядке пробовать темы и какие окна считать диалогом.
		// Порядок - правило, а не перечень: канал раньше боя, потому что явное
		// обращение в канал старше обстановки, в которой оно сделано.
		std::vector<std::string> topicOrder{ "channel", "dialogue", "menu", "combat", "world" };
		std::vector<std::string> dialogueMenuNames{ "Dialogue Menu" };

		// Кто назначен источником по способности прямо в настройках; пусто -
		// источник выбирается по порядку регистрации.
		std::string PrimaryAdapter(const std::string& a_capability) const;

		// Класс цены: 0 - обратимое действие, 1 - дорогое.
		float MinConfidence(std::int32_t a_costClass) const;
		float MinMargin(std::int32_t a_costClass) const;

		// Цена ошибки одного подписчика, если отдать ему обрывок фразы.
		float HoldWeight(std::int32_t a_costClass, bool a_revocable) const;
		// Дольше этого реплику своего класса длины не держим ни при чём.
		std::int32_t HoldCeilingMs(std::int32_t a_lengthClass) const;

		// Место участника в порядке из настроек; kNoPriority - не назван.
		std::size_t PriorityIndex(const std::string& a_ns) const;

	private:
		Settings() = default;

		static Settings& Instance();
		void             Read();

		float                    _minConfidence[2]{ 0.55f, 0.75f };
		float                    _minMargin[2]{ 0.05f, 0.15f };
		// Отзывчивый / обычный обратимый / дорогой.
		float                    _holdWeight[3]{ 0.1f, 0.4f, 1.0f };
		// Короткая / средняя / длинная.
		//
		// Это не бюджет задержки, а страховка от молчащего адаптера. Потолок
		// обязан пережить приход продолжения: если он короче, чем пауза, после
		// которой движок отдаёт следующий кусок, придержание кончится раньше,
		// чем мы узнаем то, ради чего держали.
		std::int32_t             _holdCeilingMs[3]{ 2500, 1500, 800 };
		std::vector<std::string> _priority;
		std::unordered_map<std::string, std::string> _primary;
		bool                     _read{ false };
	};
}
