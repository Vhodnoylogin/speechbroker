#include "Events.h"

#include <spdlog/spdlog.h>

namespace Envoy
{
	namespace
	{
		// Приёмник по умолчанию: событие записывается, а не рассылается.
		// Вне игры рассылать некому, и это не потеря - вопрос проверки в том,
		// какое событие мост решил послать и по какой реплике.
		class ToJournal final : public Events::Sink
		{
		public:
			void Send(const std::string& a_event, const std::string& a_string,
				float a_number) override
			{
				if (a_string.empty()) {
					spdlog::info("событие {} инициировано, реплика {}", a_event,
						static_cast<int>(a_number));
				} else {
					spdlog::info("событие {} инициировано, строка «{}», число {}", a_event,
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
