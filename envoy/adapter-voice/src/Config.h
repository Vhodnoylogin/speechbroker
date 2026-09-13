#pragma once

#include <optional>
#include <string>
#include <vector>

namespace Voice
{
	// How to bring the service up when it does not answer /health.
	struct AutoStart
	{
		bool                     enabled{ false };
		std::string              exec;
		std::vector<std::string> args;
		std::string              workingDir;
		std::string              parentPidArg;
		int                      waitSec{ 60 };  // how long to wait for a started service to answer
		int                      pollSec{ 1 };   // how often to ask it /health while waiting
	};

	// The service of the adapter. There is one per game and it rides INSIDE the mod
	// of the adapter: the microphone belongs to the adapter, not to the models.
	// Otherwise every model would carry its own capture of sound, and two
	// installed mods would fight over the device.
	//
	// Hence the property that matters most to the player: they install the adapter
	// like any other mod and everything comes up by itself - there is nothing to
	// start separately.
	struct ServiceSettings
	{
		std::string              url{ "http://127.0.0.1:8931" };
		int                      listenTimeoutSec{ 30 };
		std::optional<AutoStart> autoStart;

		// The one-off secret of this session. The adapter makes it up at load, hands
		// it to the service at startup and sends it in a header on every request:
		// whoever took the port can neither listen in on the game nor feed it text.
		std::string token;
	};

	// One installed model - what somebody else mod brought along.
	//
	// There is neither an address nor a program in the listing: a model is not a
	// service but a recogniser. The sound is given to it by the service of the
	// adapter, and the weights are its business too. All the adapter wants from
	// the listing is three things: what the model is called in the answers of the
	// service, whether it gives a draft or a final answer, and what it can do at
	// all.
	struct Model
	{
		std::string id;
		std::string name;            // what to call it in the log
		std::string language;
		bool        enabled{ false };
		bool        fast{ false };   // class == "fast": gives a draft the accurate one will refine
		bool        hears{ true };   // provides contains asr
		bool        speaks{ false }; // provides contains tts
		std::string source;          // the listing file: into the log, so that whose mod it is shows
	};

	// The settings of the adapter. The file is read once when the plugin loads and
	// is parsed into fields straight away: the threads of polling and speaking take
	// ready values rather than looking keys up in json on every utterance.
	//
	// The defaults of the fields are what is put in when a key is missing from the
	// file. They are the same values that were wired into the code before they
	// became keys, so a file without the new keys behaves exactly as before.
	class Config
	{
	public:
		// Reads its own settings file, then the folder of models. A refusal here means
		// one thing only: our own file did not parse. The absence of models is not a
		// refusal - the adapter comes up empty and says so in the log, because a model
		// is installed as a separate mod and there may be none.
		static bool Load();

		static const Config& Get();

		// The model by the name the service signed its answer with. An unknown name
		// gives nullptr: the service is entitled to return something the adapter does
		// not know about, and keeping quiet about that is not allowed.
		const Model* Find(const std::string& a_engineId) const;

		// Who is to speak. An empty speakModel means "the first one that speaks", and
		// that is the right default: the name of a particular model in the settings of
		// the adapter would tie it back to somebody else mod.
		const Model* SpeakingModel() const;

		std::string        adapterId{ "voice" };
		std::string        adapterName;
		std::string        adapterProvides;  // worked out from the installed models, not taken from the file
		ServiceSettings    service;
		std::vector<Model> models;
		std::string        speakModel;

		// Which language the adapter writes its log in. "auto" is the language the
		// game runs in. It is a key of its own and not borrowed from the bridge
		// because the adapter is a DLL of its own and writes its first lines before
		// it has met the bridge; a player who pins a language does it in both files.
		std::string        language{ "auto" };

		int correlateMs{ 2500 };     // an accurate answer in this window after a draft counts as its refinement
		int retryDelayMs{ 2000 };    // the pause after a failed /listen
		int healthTimeoutSec{ 2 };   // how long to wait for a connection on /health
		int listenGraceSec{ 10 };    // how much longer than the deadline of the service to wait for /listen
		int idleSleepMs{ 1000 };     // the step of waiting while the bridge keeps us in reserve
		int sayTimeoutSec{ 120 };    // how long to wait for an answer to /say
		int idMapLimit{ 256 };       // how many recent "service number -> bridge number" translations to remember per model; 0 - no limit

	private:
		Config() = default;

		static Config& Mutable();
	};
}
