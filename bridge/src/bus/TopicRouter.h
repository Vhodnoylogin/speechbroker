#pragma once

#include <cstdint>
#include <string>

namespace Envoy
{
	struct Utterance;

	// Тема выбирается по факту состояния игры, а не по смыслу фразы: реплика,
	// сказанная вне окна диалога, до подписчиков диалога не дойдёт никогда.
	class TopicRouter
	{
	public:
		// Вызывать только из главного потока: читает состояние игры.
		static std::string Pick(const Utterance& a_utterance);
		static std::string EventName(const std::string& a_topic);
	};
}
