#include "Service.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <httplib.h>
#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <thread>

namespace Voice
{
	namespace
	{
		std::wstring Widen(const std::string& a_text)
		{
			if (a_text.empty()) {
				return {};
			}
			const int size = ::MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(),
				static_cast<int>(a_text.size()), nullptr, 0);
			std::wstring out(static_cast<std::size_t>(size), L'\0');
			::MultiByteToWideChar(CP_UTF8, 0, a_text.c_str(), static_cast<int>(a_text.size()),
				out.data(), size);
			return out;
		}
	}

	Endpoint Endpoint::Parse(const std::string& a_url)
	{
		Endpoint out;
		auto     rest = a_url;
		const auto scheme = rest.find("://");
		if (scheme != std::string::npos) {
			rest = rest.substr(scheme + 3);
		}
		const auto colon = rest.find(':');
		if (colon != std::string::npos) {
			out.host = rest.substr(0, colon);
			out.port = std::atoi(rest.c_str() + colon + 1);
		} else {
			out.host = rest;
		}
		return out;
	}

	Service::Service() :
		_settings(Config::Get().service),
		_where(Endpoint::Parse(Config::Get().service.url))
	{}

	bool Service::Alive() const
	{
		httplib::Client client(_where.host, _where.port);
		client.set_connection_timeout(Config::Get().healthTimeoutSec, 0);
		client.set_default_headers({ { kPass, _settings.token } });
		auto res = client.Get("/health");
		return res && res->status == 200;
	}

	void Service::Launch() const
	{
		if (!_settings.autoStart) {
			return;
		}
		const auto& start = *_settings.autoStart;
		if (!start.enabled || start.exec.empty()) {
			SKSE::log::warn("служба не отвечает, а поднимать её не разрешено");
			return;
		}

		// CreateProcessW не умеет запускать .cmd и .bat напрямую - это не
		// программы, а доводы для cmd.exe. Адаптер везёт свою службу именно
		// таким запускателем: он короткий, его видно глазами и он переживает
		// переезд игры на другой диск.
		const auto  script = start.exec.ends_with(".cmd") || start.exec.ends_with(".bat");
		std::string command = script ? "cmd.exe /c \"" + start.exec + "\"" :
		                               "\"" + start.exec + "\"";
		for (const auto& arg : start.args) {
			command += " \"" + arg + "\"";
		}

		// Погасить службу при выходе из игры некому: адаптер уходит вместе с
		// процессом игры и своего завершения выполнить не успевает. Поэтому
		// службе сообщается, за кем следить, и она гасится сама. Без этого она
		// переживает игру, держит микрофон и модель, а Mod Organizer из-за неё
		// считает игру запущенной и запрещает править состав сборки.
		if (!start.parentPidArg.empty()) {
			command += " " + start.parentPidArg + " " + std::to_string(::GetCurrentProcessId());
		}

		// Секрет этой сессии. Служба, которая его проверяет, не станет отвечать
		// чужой программе и не примет от неё текст на озвучку; служба, которая
		// о нём не знает, довод не заметит - доводов, которых она не понимает,
		// она не разбирает вовсе. Поэтому правило вводится без разрыва
		// совместимости, а не когда-нибудь потом.
		command += " --envoy-token " + _settings.token;

		auto wideDir = Widen(start.workingDir);
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION info{};

		SKSE::log::info("служба: поднимаю - {}", command);

		// Служба обязана выйти из объекта задания, в котором MO2 держит игру.
		// MO2 считает игру запущенной, пока не опустеет всё дерево процессов, а
		// служба сама не завершается никогда - без этого MO2 навсегда осталась бы
		// в состоянии "игра работает", и состав сборки стало бы нельзя менять.
		// Если задание запрещает выход, запускаем как получится: служба тогда
		// удержит MO2, и правильный порядок - поднимать её заранее, вне игры.
		const auto spawn = [&](DWORD a_flags) {
			auto line = Widen(command);   // CreateProcessW портит строку, поэтому каждый раз своя
			return ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, a_flags,
				nullptr, wideDir.empty() ? nullptr : wideDir.c_str(), &startup, &info) != FALSE;
		};

		if (!spawn(CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB)) {
			const auto why = ::GetLastError();
			SKSE::log::warn("служба: не выпустили из задания (код {}), запускаю внутри него - "
			                "MO2 будет считать игру запущенной, пока она жива", why);
			if (!spawn(CREATE_NO_WINDOW)) {
				SKSE::log::error("служба: запустить не удалось, код {}", ::GetLastError());
				return;
			}
		}
		::CloseHandle(info.hThread);
		::CloseHandle(info.hProcess);

		const auto deadline = std::chrono::steady_clock::now() +
		                      std::chrono::seconds(start.waitSec);
		while (std::chrono::steady_clock::now() < deadline) {
			if (Alive()) {
				SKSE::log::info("служба поднялась");
				return;
			}
			std::this_thread::sleep_for(std::chrono::seconds(start.pollSec));
		}
		SKSE::log::warn("служба не ответила за отведённое время");
	}
}
