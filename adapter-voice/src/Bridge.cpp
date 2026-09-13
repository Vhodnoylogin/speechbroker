#include "Bridge.h"

namespace Voice
{
	Bridge& Bridge::Get()
	{
		static Bridge instance;
		return instance;
	}

	bool Bridge::Register(const EnvoyAPI::AdapterInfo& a_info, EnvoyAPI::JobCallback a_onJob, void* a_user)
	{
		if (!_envoy) {
			return false;
		}
		_id = a_info.id ? a_info.id : "";
		return _envoy->Register(a_info, a_onJob, a_user);
	}

	std::int32_t Bridge::PushUtterance(const EnvoyAPI::UtteranceIn& a_utterance)
	{
		if (!_envoy) {
			return 0;
		}
		return _envoy->PushUtterance(_id.c_str(), a_utterance);
	}

	void Bridge::PushSpeechDone(std::int32_t a_speechId, bool a_ok, bool a_interrupted)
	{
		if (!_envoy) {
			return;
		}
		_envoy->PushSpeechDone(_id.c_str(), a_speechId, a_ok, a_interrupted);
	}
}
