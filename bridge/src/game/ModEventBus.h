#pragma once

#include <string>

namespace Envoy
{
	// Delivering events to the subscribers. An event carries only a name, a string
	// and a number - nothing more fits into the mechanism of SKSE, and that
	// decides the whole shape of the contract.
	class ModEventBus
	{
	public:
		static void Send(const std::string& a_event, const std::string& a_string, float a_number);
	};
}
