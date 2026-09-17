// The settings of the shim, and the one thing to understand about them: THEY
// ARE PRIVATE TO THIS MOD.
//
// They used to be the interface. When a model mod was a data file, the adapter
// itself parsed models/whisper-ru.json and everything in it was contract - which
// is why the old listing could carry no address and no program, and why the old
// README spent a page explaining that rule. A model mod is a shim now, the
// contract is the C ABI in speechbroker-voice-model.h, and nothing outside this
// DLL reads this file. What it may hold is therefore whatever this shim needs,
// and the containment rule survives for a different reason, written at
// weightsRelative below.
//
// NOT ONE CONFIGURABLE VALUE IS A CONSTANT IN THE CODE. The defaults live in the
// initialisers here, and Load writes them out as files the first time it finds
// none, so that a player who wants to change something has something to open.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace WhisperRu
{
	// How the child is raised and what it is told. Everything here is a property
	// of OUR child; another shim built from this folder would have its own.
	struct ChildSettings
	{
		// Relative to the folder of the settings file. Both of these are checked
		// for containment exactly as the weights are.
		std::string exec{ "child/SpeechBrokerWhisperChild.exe" };

		// The backend library the child loads by FULL PATH at run time. The
		// child refuses to start when it is absent and says which file it
		// wanted; see child/README.md for what a person drops in.
		std::string library{ "child/whisper.dll" };

		// Dependencies of that library, loaded by full path BEFORE it. This list
		// is the whole reason the shim never touches PATH or
		// SetDefaultDllDirectories: a bare-name load later in the process finds
		// an already-loaded module without any policy change at all, and the
		// contract forbids the policy change because the mod that pays for it is
		// somebody else's.
		std::vector<std::string> preload;

		std::string device{ "cuda" };       // "cuda" or "cpu"; the child decides what it can honour
		std::int32_t beamSize{ 5 };
		std::int32_t threads{ 0 };          // 0 - let the backend choose

		// The vocabulary is advisory by contract and finite in fact: an
		// initial_prompt competes for a window that also has to hold the answer,
		// and a long one makes recognition worse. The reference kept 40.
		std::int32_t promptPhrases{ 40 };
	};

	// One model. Two of them ship in this mod - the draft and the accurate one -
	// and each registers with the adapter under its own id and raises its own
	// child.
	struct ModelSettings
	{
		std::string id;
		std::string name;
		std::string language{ "ru" };

		// The adapter knows only "asr" at contract version 1. It is a setting
		// rather than a constant because the field exists in the contract and a
		// later adapter may know more; it is NOT a place to claim "tts", which
		// is what the old turbo listing did and what Whisper cannot do.
		std::string provides{ "asr" };

		bool enabled{ true };

		// "fast" or "accurate". A hint, and the only thing it decides is whether
		// this model is asked on an interim pass until the adapter has measured
		// five latencies of it. Anything unrecognised is read as accurate, which
		// is the cautious reading the contract prescribes.
		std::string declaredClass{ "accurate" };

		// Binding, not a hint. False for both of ours: they are on this machine
		// and a pass costs a memcpy.
		bool finalOnly{ false };

		std::int32_t budgetMs{ 2500 };
		std::int32_t startupMs{ 60000 };
		std::int32_t stopMs{ 2000 };
		std::int32_t maxInFlight{ 2 };

		// WHERE THE WEIGHTS ARE, relative to the folder of this settings file,
		// and the containment rule is kept from the old listing with a new
		// reason. It is no longer about stopping a fifteen-line json from making
		// a service read anything on the disk - nobody else parses this file. It
		// is that the weights, the sums beside them and the archive a player
		// installs are one unit: a path leading out of the mod means the thing
		// that was verified and the thing that was shipped are not the same
		// thing, and neither the sums nor package.ps1 would notice.
		std::string weightsRelative{ "weights/whisper-ru-turbo" };

		// Refuse at Start when SHA256SUMS is missing, rather than merely when it
		// does not match. True in what ships; an author building their own model
		// mod turns it off until they have generated the sums.
		bool requireSums{ true };

		ChildSettings child;

		// Where this came from, for the log: whose file said this.
		std::filesystem::path source;
	};

	struct Settings
	{
		// The folder every relative path above is resolved against, and the
		// folder the settings files themselves live in.
		std::filesystem::path root;

		// "auto" - the language the game runs in. It decides only how the shim's
		// own log lines are rendered; what a model HEARS is ModelSettings above.
		std::string language{ "auto" };

		// debug, info, warn, error. The shim's own file log; what goes to the
		// adapter through Host::Log is not filtered here, because the adapter
		// owns that log and its own level.
		std::string logLevel{ "info" };

		std::vector<ModelSettings> models;
	};

	// Reads every *.json in a_root, one model per file, in file-name order.
	//
	// WRITES THE DEFAULTS when it finds none: two files, the draft and the
	// accurate one, exactly as they ship. A first run that leaves an empty
	// folder and a log line nobody reads is how a mod acquires a reputation for
	// doing nothing.
	//
	// It never throws. A file that does not parse is skipped with a log line and
	// the others are loaded: one broken model mod must not cost a person the
	// others, and the same is true one level down, inside one mod.
	Settings LoadSettings(const std::filesystem::path& a_root);

	// True when a_relative stays inside a_root. Exposed because the child exe,
	// the backend library and the weights all go through it and the refusal has
	// to name which one failed.
	bool IsContained(const std::filesystem::path& a_root, const std::string& a_relative,
		std::filesystem::path& a_resolved);
}
