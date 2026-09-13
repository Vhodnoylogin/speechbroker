#pragma once

#include "envoy-adapter.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace Voice
{
	// Our side of the bridge: the interface the bridge sent in an SKSE message, and
	// what it has told us to do. The one place where the adapter holds a pointer to
	// the bridge - the threads of polling and speaking reach the bridge only from
	// here.
	class Bridge
	{
	public:
		static Bridge& Get();

		void Attach(EnvoyAPI::IEnvoy* a_envoy) { _envoy = a_envoy; }
		void Detach() { _envoy = nullptr; }
		bool Ready() const { return _envoy != nullptr; }

		std::uint32_t Version() const { return _envoy->Version(); }

		// The name from the registration is remembered: the utterances and the reports
		// on speaking go under it later, and there is no point asking the settings for
		// it a second time.
		bool Register(const EnvoyAPI::AdapterInfo& a_info, EnvoyAPI::JobCallback a_onJob, void* a_user);

		// The number of the utterance at the bridge, or 0 if the bridge did not take it
		// or is not there.
		std::int32_t PushUtterance(const EnvoyAPI::UtteranceIn& a_utterance);
		void         PushSpeechDone(std::int32_t a_speechId, bool a_ok, bool a_interrupted);

		// The bridge made us the source, or told us to fall silent.
		void SetListening(bool a_on) { _listening.store(a_on); }
		bool Listening() const { return _listening.load(); }

	private:
		Bridge() = default;

		EnvoyAPI::IEnvoy* _envoy{ nullptr };
		std::string       _id;
		std::atomic_bool  _listening{ true };
	};
}
