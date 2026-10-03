#pragma once

#include "models/ModelHost.h"  // HostSettings, and EarsSettings through turn/Ears.h

#include <string>

namespace Voice
{
	// The settings of the adapter. The file is read once when the plugin loads and
	// is parsed into fields straight away: the threads of the ears and of every
	// model take ready values rather than looking keys up in json while somebody is
	// speaking.
	//
	// The defaults of the fields are what is put in when a key is missing from the
	// file, and they are the shipping behaviour. A file written before a key
	// existed behaves exactly as it did.
	//
	// THERE IS NO SERVICE HERE ANY MORE, and that is the whole shape of this file.
	// Until 17.09 the adapter was an HTTP client: it brought up a python service,
	// polled it for utterances and asked it to speak, and a "model" was a json
	// listing that service read. Both are gone. The microphone belongs to the
	// adapter, in this process (src/audio, src/turn), and a model is an SKSE plugin
	// that registers through the C ABI in contract/speechbroker-voice-model.h. So
	// there is no url, no token, no autoStart, no listing folder and no speakModel:
	// a model is installed, not configured, and nothing about it is written here.
	class Config
	{
	public:
		// Reads the settings file. A refusal means our own file did not parse, and
		// that is fatal: a mistake in it is ours. There is nothing else to refuse
		// for - no models are read here, because no model is declared here.
		static bool Load();

		static const Config& Get();

		// What the adapter calls itself at the bridge.
		std::string adapterId{ "voice" };
		std::string adapterName;

		// What it declares to the bridge. "asr" and only "asr": this adapter hears.
		// Speaking is a capability of an adapter that has a speech model behind it,
		// and promising the bridge speech we cannot produce takes the work away
		// from an adapter that can (see OnJob, kJobSpeak, in main.cpp).
		//
		// It is NOT worked out from the installed models any more, and cannot be:
		// models register at kDataLoaded, long after the adapter has introduced
		// itself to the bridge at kPostPostLoad. A model-less installation is
		// therefore an adapter that declares hearing and hears nothing, and the log
		// says so in as many words rather than leaving a silent microphone to look
		// like a fault of ours.
		std::string adapterProvides{ "asr" };

		// Which language the adapter writes its log in. "auto" is the language the
		// game runs in. It is a key of its own and not borrowed from the bridge
		// because the adapter is a DLL of its own and writes its first lines before
		// it has met the bridge; a player who pins a language does it in both files.
		std::string language{ "auto" };

		// How many recent "our slice id -> the bridge's utterance id" translations
		// to remember. A piece that swallows earlier ones names them by OUR numbers,
		// and the bridge understands only its own; the translation lives in the
		// adapter because it is the only side that knows both. 0 - no limit.
		//
		// It must not be dropped whole when it fills: a long piece arriving right
		// after a drop would not find the short ones it swallowed, the absorption
		// would silently not happen, and the bridge would announce both the pieces
		// and the whole phrase. Only the oldest go.
		int idMapLimit{ 256 };

		// The ears: the microphone and the cutting of the stream into passes.
		//
		// It is a field of Config and not a reader of it: nothing under src/audio or
		// src/turn includes this file, which is what lets the whole listening half
		// be built and run from a wav, outside the game.
		EarsSettings ears;

		// The dispatch half: who may register, how a model's life is run, how the
		// standings are kept and how an argument between models is settled.
		//
		// The same shape and the same reason as ears - nothing under src/models
		// includes this file either, so that half can be exercised with a table of
		// numbers and no game anywhere near it.
		Models::HostSettings models;

	private:
		Config() = default;

		static Config& Mutable();
	};
}
