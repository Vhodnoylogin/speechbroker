#pragma once

namespace Voice
{
	// Polling the service of the adapter: connect to it or bring it up, then
	// endlessly take what was recognised and carry it over to the bridge. Lives in
	// a thread of its own until the process of the game ends.
	//
	// There is one thread. There is one service: the microphone belongs to the
	// adapter, and the models inside the service work over one and the same sound -
	// which of them recognised an utterance is said in the answer, in the engine
	// field.
	void PollService();
}
