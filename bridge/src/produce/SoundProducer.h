#pragma once

#include <cstdint>
#include <string>

namespace SpeechBroker
{
	// Sound made out of text. NOT WRITTEN YET.
	//
	// This is the shape and nothing else, declared here so that the gap is visible
	// from the code rather than only from a page of documentation. It was part of
	// the design from the first day - the `Speak` channel - and it is one of the
	// five parts of the broker; see docs/architecture.md.
	//
	// What it will do: a subscriber asks for a line to be spoken, the producer
	// hands the text to a model mod, takes the sound back, writes it into a file
	// and answers with the file to use. The subscriber plays that file itself.
	//
	// Why a FILE and not the samples. The game plays sound from files, and a mod
	// that receives raw samples has to write them to a file anyway - in Papyrus,
	// where it cannot. Answering with a path puts the one awkward step in the one
	// place that can do it well, and lets the producer keep a file it has already
	// made instead of asking a model twice for the same line.
	//
	// Why it is not a market. Nothing is argued over: the request names its own
	// author and the answer goes back to the one who asked. A market exists where
	// one utterance has several candidates, and here it has none.
	//
	// Until it is written, Make answers kNotImplemented to everything. Nothing
	// constructs it and nothing registers it, so no subscriber can reach it by
	// accident and mistake an empty answer for a broken model.
	class SoundProducer
	{
	public:
		enum class Status : std::int32_t
		{
			kNotImplemented = 0,   // the channel is declared, not built
			kOk             = 1,
			kNoModel        = 2,   // nobody in the build can speak
			kFailed         = 3
		};

		struct Request
		{
			std::string askedBy;    // the plugin that wants the line, for the log and the answer
			std::string text;       // what is to be spoken; the producer does not read it
			std::string voice;      // which voice; empty means the model decides
			std::string language;   // the language of the text, empty means the game's
		};

		struct Answer
		{
			Status      status{ Status::kNotImplemented };
			std::string file;     // where the sound was written; empty unless kOk
			std::string reason;   // a localisation key, never a sentence
		};

		// Not written. Answers kNotImplemented for every request, and says so by
		// key so that whatever displays it can say it in the player's language.
		[[nodiscard]] Answer Make(const Request& request) const;

		// Whether this channel can be used at all. False until it is built, and it
		// exists so that a caller asks BEFORE building a request rather than
		// reading a failure afterwards.
		[[nodiscard]] static constexpr bool Available() noexcept { return false; }
	};
}
