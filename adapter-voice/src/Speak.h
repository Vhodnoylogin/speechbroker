#pragma once

#include <cstdint>
#include <string>

namespace Voice
{
	// Speak the text with the speakModel model and report to the bridge how it
	// ended. It waits for the answer of the service, so it has to be called from a
	// thread of its own.
	void Speak(const std::string& a_text, std::int32_t a_speechId);
}
