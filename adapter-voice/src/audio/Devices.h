#pragma once

#include <string>
#include <vector>

namespace Voice
{
	// How many times to try the whole list of candidates before giving up, and how
	// long to wait between the rounds.
	//
	// Both numbers come from the reference, and the pause is there for a reason
	// that is easy to lose: a process that has just been killed still holds the
	// endpoint for a moment, and the first round after a restart fails on a device
	// that is perfectly fine (voice-service.py:199 `for attempt in range(3)`,
	// voice-service.py:218 `time.sleep(1.5)` with the comment "a just-killed
	// process can still hold the endpoint").
	struct ReopenPolicy
	{
		int tries{ 3 };       // voice-service.py:199
		int delayMs{ 1500 };  // voice-service.py:218
	};

	// Which microphone to take.
	//
	// NAMES, NOT NUMBERS. Device indices are not stable between reboots and names
	// are, so the settings hold names and they are resolved at every start -
	// engine/audio.py:10-11 says so in as many words, and it is why this struct has
	// a list of patterns rather than an id.
	//
	// The list is in order of preference and a pattern is a case-insensitive
	// substring of the friendly name of the endpoint (engine/audio.py:44-45). The
	// first pattern that matches anything wins, and inside one pattern the
	// endpoints keep the order the system gave them.
	struct DeviceSettings
	{
		// voice.json:5-11 - the shipped list, in the order the player wrote it.
		std::vector<std::string> input{
			"Steam Streaming Microphone",
			"Quest",
			"Oculus",
			"LG Monitor HP MIC",
			"Realtek HD Audio Mic"
		};

		// voice.json:17-21. The reference went through PortAudio, where one physical
		// microphone is visible through several sound subsystems that fail in
		// different ways, and this list said which subsystem to prefer
		// (engine/audio.py:29-34, :46-47).
		//
		// HERE THERE IS ONLY WASAPI: this is a plugin inside the game and it talks
		// to the audio engine of Windows directly. The field is kept, read and
		// reported because it ships in the settings file that this module inherits,
		// and a player who edits a key must not be ignored in silence - the body
		// says once in the log that it names an interface this build does not have.
		// It must never change behaviour quietly.
		std::vector<std::string> inputApi{
			"Windows WASAPI",
			"MME",
			"Windows DirectSound"
		};

		// When not one pattern matched, take the default capture endpoint of the
		// system. The reference ends its candidate list with exactly this
		// (engine/audio.py:90). A player who wants "this microphone or none" sets
		// it to false.
		bool allowDefault{ true };

		ReopenPolicy reopen;
	};

	// One capture endpoint, named in the two ways the two sides need it: the id is
	// what the system opens, the name is what a person reads in the log.
	//
	// The id is a wide string because that is what WASAPI hands out. It does not
	// leave this pair of files - nothing above audio/ is allowed to hold one, and
	// nothing below is allowed to reason about the name.
	struct Device
	{
		std::wstring id;
		std::string  name;             // UTF-8, for the log
		bool         isDefault{ false };
	};

	// Naming the microphones and putting them in order. It opens nothing: opening,
	// and holding open, belongs to Capture, which is the one thing that owns the
	// device.
	//
	// THREADS. Inputs() touches COM and must therefore run on a thread that has
	// initialised it - in practice the capture pump, which does so for itself.
	// Rank() touches nothing, which is the point of splitting it out: the choosing
	// of a microphone is the part that has to be testable without a microphone.
	class Devices
	{
	public:
		// Every active capture endpoint of the machine. An empty list is not an
		// error here - the body says so in the log and Capture decides what to do.
		//
		// CAPTURE PUMP THREAD (COM initialised).
		static std::vector<Device> Inputs();

		// The candidates the settings ask for, best first: every endpoint matching
		// the first pattern, then the second, and so on, with the default endpoint
		// last when allowDefault is set and it is not already in the list.
		//
		// Pure: it is given the endpoints rather than asking for them. ANY THREAD.
		static std::vector<Device> Rank(const std::vector<Device>& a_all,
			const DeviceSettings& a_settings);
	};
}
