#pragma once

#include "Config.h"

#include <string>

namespace Voice
{
	// Адрес службы в том виде, в каком его понимает httplib: хост и порт
	// отдельно, без схемы.
	struct Endpoint
	{
		std::string host{ "127.0.0.1" };
		int         port{ 80 };

		static Endpoint Parse(const std::string& a_url);
	};

	// Служба адаптера: жива ли она и как её поднять, если нет. Она одна на игру
	// и лежит внутри мода адаптера.
	//
	// Чужой процесс никогда не убивает: если служба уже отвечает, к ней просто
	// подключаются. Человек мог поднять её сам - например, заранее, чтобы модель
	// успела загрузиться до входа в игру.
	class Service
	{
	public:
		Service();

		const Endpoint& Where() const { return _where; }

		// Имя заголовка, которым адаптер предъявляет службе секрет сессии.
		// Его обязан нести КАЖДЫЙ запрос, иначе правило защищает рукопожатие,
		// а не разговор. Сам httplib в этот заголовок не тянем нарочно: он
		// подключает windows.h, а CommonLibSSE обязан увидеть его первым.
		static constexpr const char* kPass = "X-Envoy-Token";

		// /health ответил 200.
		bool Alive() const;

		// Поднимает службу и ждёт, пока она ответит на /health.
		void Launch() const;

	private:
		const ServiceSettings& _settings;
		Endpoint               _where;
	};
}
