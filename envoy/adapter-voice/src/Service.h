#pragma once

#include "Config.h"

#include <string>

namespace Voice
{
	// The address of the service in the form httplib understands it: the host and
	// the port apart, without a scheme.
	struct Endpoint
	{
		std::string host{ "127.0.0.1" };
		int         port{ 80 };

		static Endpoint Parse(const std::string& a_url);
	};

	// The service of the adapter: whether it is alive and how to bring it up if it
	// is not. There is one per game and it lies inside the mod of the adapter.
	//
	// It never kills a process that is not ours: if the service already answers, we
	// simply connect to it. A person may have started it themselves - in advance,
	// say, so that the model has time to load before they enter the game.
	class Service
	{
	public:
		Service();

		const Endpoint& Where() const { return _where; }

		// The name of the header the adapter shows the service its session secret
		// with. EVERY request has to carry it, or the rule protects the handshake
		// rather than the conversation. httplib itself is deliberately kept out of
		// this header: it pulls in windows.h, and CommonLibSSE has to see that first.
		static constexpr const char* kPass = "X-Envoy-Token";

		// /health answered 200.
		bool Alive() const;

		// Brings the service up and waits until it answers /health.
		void Launch() const;

	private:
		const ServiceSettings& _settings;
		Endpoint               _where;
	};
}
