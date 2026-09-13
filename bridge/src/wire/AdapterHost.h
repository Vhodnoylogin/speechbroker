#pragma once

#include "envoy-adapter.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Envoy
{
	// Приёмная моста для адаптеров. Мост не знает ни одной внешней программы:
	// адаптеры - такие же моды, они приходят сами и называются сами.
	//
	// Задания отдаются вызовом обратной функции прямо в потоке вызывающего,
	// поэтому адаптер обязан лишь положить задание в свою очередь и вернуться.
	//
	// Замок при этом всегда отпущен: задания собираются под ним, а рассылаются
	// после. Иначе адаптер, ответивший мосту из обработчика, вставал бы намертво.
	class AdapterHost final : public EnvoyAPI::IEnvoy
	{
	public:
		static AdapterHost& Get();

		std::uint32_t Version() const override { return EnvoyAPI::kInterfaceVersion; }

		bool         Register(const EnvoyAPI::AdapterInfo& a_info, EnvoyAPI::JobCallback a_onJob, void* a_user) override;
		void         Unregister(const char* a_id) override;
		std::int32_t PushUtterance(const char* a_adapterId, const EnvoyAPI::UtteranceIn& a_utterance) override;
		bool         SourceOf(const char* a_capability, char* a_out,
		                 std::int32_t a_outSize) const override;

		// Для меню и скриптов.
		bool                     SetSource(const std::string& a_capability, const std::string& a_adapter);
		std::string              Source(const std::string& a_capability) const;
		std::vector<std::string> AdapterIds() const;
		void                     ReloadConfig();

		// Ответ модели и исход озвучки: событие несёт только номер, поэтому
		// подписчик приходит за содержимым сюда.
		std::string Answer(std::int32_t a_requestId) const;
		std::string SpeechResult(std::int32_t a_speechId) const;

		void PushAnswer(const char* a_adapterId, std::int32_t a_requestId, bool a_ok,
			const char* a_payload) override;
		void PushSpeechDone(const char* a_adapterId, std::int32_t a_speechId, bool a_ok,
			bool a_interrupted) override;

		void         SendVocabulary(const std::vector<std::string>& a_phrases);
		std::int32_t SendSpeak(const std::string& a_text, const std::string& a_voice, std::int32_t a_priority);
		void         SendStop(std::int32_t a_speechId);
		std::int32_t SendAsk(const std::string& a_service, const std::string& a_payload);

	private:
		AdapterHost() = default;

		struct Entry
		{
			std::string              name;
			std::vector<std::string> provides;
			EnvoyAPI::JobCallback    onJob{ nullptr };
			void*                    user{ nullptr };
			std::uint64_t            order{ 0 };
			bool                     active{ false };
			// Версия контракта, объявленная при рукопожатии. Мост не читает
			// полей, которых в этой версии ещё не было.
			std::uint32_t            contract{ 0 };
		};

		// Задание, собранное под замком и разосланное уже без него. Строки
		// принадлежат ему самому: указатели внутри EnvoyAPI::Job живут только
		// на время вызова, а вызов случается после того, как замок отпущен.
		struct Outgoing
		{
			EnvoyAPI::JobCallback    onJob{ nullptr };
			void*                    user{ nullptr };
			std::int32_t             kind{ 0 };
			bool                     active{ false };
			std::string              text;
			std::string              service;
			std::string              payload;
			std::int32_t             speechId{ 0 };
			std::int32_t             requestId{ 0 };
			std::vector<std::string> phrases;

			void Send() const;
		};

		static void Dispatch(const std::vector<Outgoing>& a_jobs);

		// Правило «кто источник по способности», отделённое от последствий.
		// Чистая функция от состава адаптеров, принуждений из меню и имён из
		// настроек: её можно прочитать и проверить, не думая о замках,
		// заданиях и журнале. Прежде правило было переплетено с ними.
		static std::unordered_map<std::string, std::string> Choose(
			const std::unordered_map<std::string, Entry>&       a_adapters,
			const std::unordered_map<std::string, std::string>& a_overrides);

		// Применяет Choose: переключает активность и возвращает задания вместо
		// того, чтобы их рассылать. Вызывающий обязан отпустить замок и только
		// потом звать Dispatch.
		[[nodiscard]] std::vector<Outgoing> RecomputeSources();

		mutable std::mutex                           _mutex;
		std::unordered_map<std::string, Entry>       _adapters;
		std::unordered_map<std::string, std::string> _sources;
		std::unordered_map<std::string, std::string> _overrides;

		// Событие несёт только номер, поэтому сам ответ и исход озвучки должны
		// где-то лежать, пока подписчик за ними не придёт.
		std::unordered_map<std::int32_t, std::string> _answers;
		std::unordered_map<std::int32_t, std::string> _speechResults;
		std::uint64_t                                _order{ 0 };
		std::int32_t                                 _nextSpeech{ 1 };
		std::int32_t                                 _nextRequest{ 1 };
	};
}
