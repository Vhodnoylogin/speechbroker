#include "Bridge.h"

namespace Voice
{
	Bridge& Bridge::Get()
	{
		static Bridge instance;
		return instance;
	}

	bool Bridge::Register(const SpeechBrokerAPI::AdapterInfo& a_info, SpeechBrokerAPI::JobCallback a_onJob, void* a_user)
	{
		if (!_speechbroker) {
			return false;
		}
		_id = a_info.id ? a_info.id : "";
		return _speechbroker->Register(a_info, a_onJob, a_user);
	}

	std::int32_t Bridge::PushUtterance(const SpeechBrokerAPI::UtteranceIn& a_utterance)
	{
		if (!_speechbroker) {
			return 0;
		}
		return _speechbroker->PushUtterance(_id.c_str(), a_utterance);
	}

	void Bridge::PushSpeechDone(std::int32_t a_speechId, bool a_ok, bool a_interrupted)
	{
		if (!_speechbroker) {
			return;
		}
		_speechbroker->PushSpeechDone(_id.c_str(), a_speechId, a_ok, a_interrupted);
	}
}
