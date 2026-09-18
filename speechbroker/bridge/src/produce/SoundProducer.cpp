#include "produce/SoundProducer.h"

namespace SpeechBroker
{
	SoundProducer::Answer SoundProducer::Make(const Request&) const
	{
		// Deliberately total: there is no half of this channel that works, and a
		// stub that answered anything else would be a lie a caller could build on.
		Answer answer;
		answer.status = Status::kNotImplemented;
		answer.reason = "$SPEECHBROKER_REASON_SOUND_NOT_IMPLEMENTED";
		return answer;
	}
}
