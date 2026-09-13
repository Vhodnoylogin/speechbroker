#include "Events.h"

#include <spdlog/spdlog.h>

namespace Envoy
{
	namespace
	{
		// The default sink: the event is written down, not broadcast. Outside the
		// game there is nobody to broadcast to, and that is no loss - the question
		// under check is which event the bridge decided to send and on which
		// utterance.
		class ToJournal final : public Events::Sink
		{
		public:
			void Send(const std::string& a_event, const std::string& a_string,
				float a_number) override
			{
				if (a_string.empty()) {
					spdlog::info("event {} raised, utterance {}", a_event,
						static_cast<int>(a_number));
				} else {
					spdlog::info("event {} raised, string '{}', number {}", a_event,
						a_string, a_number);
				}
			}
		};

		ToJournal     g_journal;
		Events::Sink* g_sink = nullptr;
	}

	void Events::Install(Sink* a_sink)
	{
		g_sink = a_sink;
	}

	void Events::Send(const std::string& a_event, const std::string& a_string, float a_number)
	{
		if (g_sink) {
			g_sink->Send(a_event, a_string, a_number);
			return;
		}
		g_journal.Send(a_event, a_string, a_number);
	}
}
