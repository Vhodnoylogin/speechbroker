#pragma once

#include "envoy-adapter.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace Voice
{
	// Наша сторона моста: интерфейс, который мост прислал сообщением SKSE,
	// и то, что он нам поручил. Единственное место, где адаптер держит
	// указатель на мост, - потоки опроса и озвучки ходят к мосту только отсюда.
	class Bridge
	{
	public:
		static Bridge& Get();

		void Attach(EnvoyAPI::IEnvoy* a_envoy) { _envoy = a_envoy; }
		void Detach() { _envoy = nullptr; }
		bool Ready() const { return _envoy != nullptr; }

		std::uint32_t Version() const { return _envoy->Version(); }

		// Имя из регистрации запоминается: под ним же потом идут реплики и
		// отчёты об озвучке, и второй раз спрашивать его у настроек незачем.
		bool Register(const EnvoyAPI::AdapterInfo& a_info, EnvoyAPI::JobCallback a_onJob, void* a_user);

		// Номер реплики у моста либо 0, если мост её не принял или его нет.
		std::int32_t PushUtterance(const EnvoyAPI::UtteranceIn& a_utterance);
		void         PushSpeechDone(std::int32_t a_speechId, bool a_ok, bool a_interrupted);

		// Мост назначил нас источником или велел замолчать.
		void SetListening(bool a_on) { _listening.store(a_on); }
		bool Listening() const { return _listening.load(); }

	private:
		Bridge() = default;

		EnvoyAPI::IEnvoy* _envoy{ nullptr };
		std::string       _id;
		std::atomic_bool  _listening{ true };
	};
}
