#pragma once

#include <cstdint>
#include <string>

namespace Voice
{
	// Озвучить текст моделью speakModel и доложить мосту, чем кончилось.
	// Ждёт ответа службы, поэтому зваться должна из своего потока.
	void Speak(const std::string& a_text, std::int32_t a_speechId);
}
