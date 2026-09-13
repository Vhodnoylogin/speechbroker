/* Envoy Framework - интерфейс адаптера. Версия 1.
 *
 * Адаптер - такой же мод, как мост: обычный плагин SKSE, живущий в том же
 * процессе игры. Поэтому говорят они вызовом функции, а не по сети.
 *
 * Наружу - к модели, которая частью игры быть не может, - смотрит только
 * адаптер, и транспорт туда выбирает он сам: HTTP, канал, встроенная
 * библиотека. Мосту об этом знать нечего.
 *
 * Рукопожатие такое же, как у HIGGS и PLANCK: мост рассылает сообщение SKSE
 * с указателем на интерфейс, адаптер его ловит.
 */
#pragma once

#include <cstdint>

namespace EnvoyAPI
{
	constexpr std::uint32_t kInterfaceVersion = 3;

	// Тип сообщения SKSE, которым мост отдаёт интерфейс. Отправитель - "Envoy".
	constexpr std::uint32_t kMessageInterface = 'ENVY';

	// Что адаптер прислал. Строки живут только на время вызова: мост копирует.
	struct UtteranceIn
	{
		const char*  text{ nullptr };
		const char*  language{ nullptr };
		const char*  engine{ nullptr };
		const char*  channel{ nullptr };
		float        score{ 0.0f };
		float        margin{ 0.0f };
		std::int32_t latencyMs{ 0 };
		std::int32_t durationMs{ 0 };
		bool         isFinal{ true };

		// 0 - новая реплика; иначе уточнение уже выданной. Так работает пара
		// "быстрая модель плюс точная": вторая уточняет результат первой.
		std::int32_t refinesId{ 0 };

		const char* const* altText{ nullptr };
		const float*       altScore{ nullptr };
		std::int32_t       altCount{ 0 };

		// --- версия контракта 3 ---------------------------------------------
		// Мост не читает этих полей у адаптера, объявившего версию младше.

		// Вероятность, что на этом куске предложение ЗАКОНЧИЛОСЬ. Знает это
		// только адаптер: паузу, интонацию и тон слышит он, а мост получает
		// уже текст. Единица - "закончено", и она же значение по умолчанию:
		// адаптер, который об этом ничего не знает, ведёт себя как прежде.
		float        complete{ 1.0f };

		// 0 короткая, 1 средняя, 2 длинная. Мост держит куски разных классов
		// разное время: у короткого продолжение приходит быстро, у длинного
		// ждать уже нечего.
		std::int32_t lengthClass{ 0 };

		// Номера реплик, которые этот кусок вобрал в себя. Номера - те, что
		// вернул PushUtterance: своей нумерации кусков мост не знает и знать
		// не должен. Придержанные из них будут выброшены не оглашёнными,
		// уже отданные - отозваны.
		const std::int32_t* supersedes{ nullptr };
		std::int32_t        supersedesCount{ 0 };
	};

	struct AdapterInfo
	{
		const char*   id{ nullptr };        // "voice"
		const char*   name{ nullptr };      // человекочитаемое
		const char*   provides{ nullptr };  // "asr,tts" - через запятую
		std::uint32_t contract{ kInterfaceVersion };
	};

	enum JobKind : std::int32_t
	{
		kJobListen     = 1,  // быть источником или замолчать
		kJobVocabulary = 2,  // объединённый словарь подписчиков изменился
		kJobSpeak      = 3,  // озвучить текст
		kJobStop       = 4,  // прекратить озвучку
		kJobAsk        = 5   // произвольный запрос к модели: содержимое мосту непрозрачно
	};

	struct Job
	{
		std::int32_t       kind{ 0 };
		bool               active{ false };    // для kJobListen
		const char*        text{ nullptr };    // текст для kJobSpeak, причина для kJobListen
		const char* const* phrases{ nullptr };  // для kJobVocabulary
		std::int32_t       phraseCount{ 0 };
		std::int32_t       speechId{ 0 };

		// Для kJobAsk. payload мост не разбирает и не проверяет: иначе каждая
		// новая модель означала бы правку моста.
		std::int32_t       requestId{ 0 };
		const char*        service{ nullptr };
		const char*        payload{ nullptr };
	};

	using JobCallback = void (*)(const Job& a_job, void* a_user);

	class IEnvoy
	{
	public:
		virtual ~IEnvoy() = default;

		virtual std::uint32_t Version() const = 0;

		// Мост держит по одной активной способности за раз; остальные адаптеры
		// получают kJobListen с active=false и обязаны отпустить своё устройство.
		virtual bool Register(const AdapterInfo& a_info, JobCallback a_onJob, void* a_user) = 0;
		virtual void Unregister(const char* a_id) = 0;

		// Возвращает номер реплики в игре или 0 при отказе.
		virtual std::int32_t PushUtterance(const char* a_adapterId, const UtteranceIn& a_utterance) = 0;

		// Кто сейчас источник по способности. Имя копируется в буфер
		// вызывающего: возвращать указатель на внутреннюю строку моста нельзя -
		// её перепишет следующий спросивший, а спрашивают из разных потоков.
		// false - источника нет либо буфер мал.
		virtual bool SourceOf(const char* a_capability, char* a_out,
			std::int32_t a_outSize) const = 0;

		// Обратное направление: адаптер сообщает, чем кончилось задание.
		virtual void PushAnswer(const char* a_adapterId, std::int32_t a_requestId, bool a_ok,
			const char* a_payload) = 0;
		virtual void PushSpeechDone(const char* a_adapterId, std::int32_t a_speechId, bool a_ok,
			bool a_interrupted) = 0;
	};
}
