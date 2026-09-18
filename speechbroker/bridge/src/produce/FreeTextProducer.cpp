#include "produce/FreeTextProducer.h"

namespace SpeechBroker
{
	FreeTextProducer::Answer FreeTextProducer::Ask(const Request&) const
	{
		// No payload is echoed back. A stub that returned the question as the
		// answer would look like a working loop to anybody testing it.
		Answer answer;
		answer.status = Status::kNotImplemented;
		answer.reason = "$SPEECHBROKER_REASON_FREETEXT_NOT_IMPLEMENTED";
		return answer;
	}
}
