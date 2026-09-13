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
			SKSE::log::warn("the service does not answer, and starting it is not allowed");
			return;
		}

		// CreateProcessW cannot start a .cmd or a .bat directly - they are not
		// programs but arguments for cmd.exe. The adapter carries its service with
		// exactly such a starter: it is short, it can be read by eye and it survives
		// the game moving to another disk.
		const auto  script = start.exec.ends_with(".cmd") || start.exec.ends_with(".bat");
		std::string command = script ? "cmd.exe /c \"" + start.exec + "\"" :
		                               "\"" + start.exec + "\"";
		for (const auto& arg : start.args) {
			command += " \"" + arg + "\"";
		}

		// There is nobody to put the service out when the game exits: the adapter
		// leaves together with the process of the game and never gets the chance to
		// shut itself down. So the service is told who to watch, and it puts itself
		// out. Without this it outlives the game, holds the microphone and the model,
		// and Mod Organizer counts the game as running because of it and forbids any
		// change to the make-up of the build.
		if (!start.parentPidArg.empty()) {
			command += " " + start.parentPidArg + " " + std::to_string(::GetCurrentProcessId());
		}

		// The secret of this session. A service that checks it will not answer
		// somebody else program and will not take text to speak from it; a service
		// that knows nothing about it will not notice the argument - arguments it does
		// not understand it does not parse at all. So the rule comes in without
		// breaking compatibility, rather than some day later.
		command += " --envoy-token " + _settings.token;

		auto wideDir = Widen(start.workingDir);
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION info{};

		SKSE::log::info("service: starting it - {}", command);

		// The service has to leave the job object MO2 keeps the game in. MO2 counts
		// the game as running until the whole tree of processes is empty, and the
		// service never ends by itself - without this MO2 would stay in the state
		// "the game is running" forever, and the make-up of the build could not be
		// changed. If the job forbids leaving, we start it as best we can: the service
		// will then hold MO2, and the right order is to bring it up beforehand,
		// outside the game.
		const auto spawn = [&](DWORD a_flags) {
			auto line = Widen(command);   // CreateProcessW spoils the string, so a fresh one every time
			return ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, a_flags,
				nullptr, wideDir.empty() ? nullptr : wideDir.c_str(), &startup, &info) != FALSE;
		};

		if (!spawn(CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB)) {
			const auto why = ::GetLastError();
			SKSE::log::warn("service: not let out of the job (code {}), starting it inside - "
			                "MO2 will count the game as running for as long as it lives", why);
			if (!spawn(CREATE_NO_WINDOW)) {
				SKSE::log::error("service: could not start it, code {}", ::GetLastError());
				return;
			}
		}
		::CloseHandle(info.hThread);
		::CloseHandle(info.hProcess);

		const auto deadline = std::chrono::steady_clock::now() +
		                      std::chrono::seconds(start.waitSec);
		while (std::chrono::steady_clock::now() < deadline) {
			if (Alive()) {
				SKSE::log::info("the service is up");
				return;
			}
			std::this_thread::sleep_for(std::chrono::seconds(start.pollSec));
		}
		SKSE::log::warn("the service did not answer within the time allowed");
	}
}
