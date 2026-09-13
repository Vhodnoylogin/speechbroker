#pragma once

#include <string>

namespace Envoy
{
	// The seam "the event went out to the subscribers".
	//
	// An event is a doorbell: a name, a string and a number, nothing more fits
	// into the mechanism of SKSE, and that decides the whole shape of the
	// contract. Where exactly to ring, the core does not know: in the game it is
	// a Papyrus broadcast, outside the game a line in the log. That the bell was
	// rung is all the core needs.
	//
	// The default sink does write it down: "event X raised". For a check that is
	// enough - what occupies us is which event the bridge decided to send and on
	// which utterance, not how somebody else answered it from a script.
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
