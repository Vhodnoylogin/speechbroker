#pragma once

#include <string>

namespace Envoy
{
	// Рассылка событий подписчикам. Событие несёт только имя, строку и число -
	// больше в механизм SKSE не влезает, и это определяет всю форму контракта.
	class ModEventBus
	{
	public:
		static void Send(const std::string& a_event, const std::string& a_string, float a_number);
	};
}
