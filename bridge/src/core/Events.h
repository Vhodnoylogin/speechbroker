#pragma once

#include <string>

namespace Envoy
{
	// Шов «событие ушло подписчикам».
	//
	// Событие - звонок в дверь: имя, строка и число, больше в механизм SKSE
	// не влезает, и это определяет всю форму контракта. Куда именно звонить,
	// ядро не знает: в игре это рассылка Papyrus, вне игры - строка в журнале.
	// Ядру достаточно того, что звонок сделан.
	//
	// Приёмник по умолчанию именно записывает: «событие X инициировано». Для
	// проверки этого хватает - нас занимает, какое событие мост решил послать
	// и по какой реплике, а не то, как на него ответил чужой скрипт.
	class Events
	{
	public:
		class Sink
		{
		public:
			virtual ~Sink() = default;
			virtual void Send(const std::string& a_event, const std::string& a_string,
				float a_number) = 0;
		};

		static void Install(Sink* a_sink);
		static void Send(const std::string& a_event, const std::string& a_string, float a_number);
	};
}
