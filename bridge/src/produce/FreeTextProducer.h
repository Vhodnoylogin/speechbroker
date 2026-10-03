#pragma once

#include <cstdint>
#include <string>

namespace SpeechBroker
{
	// Text between a subscriber and a model, which the broker carries and does not
	// read. NOT WRITTEN YET.
	//
	// The shape only, declared so the gap is visible from the code. It was part of
	// the design from the first day - the `Ask` channel - and it is one of the five
	// parts of the broker; see docs/architecture.md.
	//
	// What it will do: a subscriber puts a question to a model in whatever form the
	// two of them have agreed on, and the answer comes back. The broker reads ONLY
	// the meta of the packet - who asked, whom they asked, and who is to receive the
	// answer. The payload it neither parses nor validates nor logs.
	//
	// Why the payload stays closed. The moment the broker understands the text it
	// becomes a party to the conversation: it would have a format to keep, a version
	// to migrate and an opinion about what a well-formed question looks like. Two
	// mods agreeing between themselves need none of that from us, and every pair
	// that wants something different would have to wait for us to add it.
	//
	// What follows: the broker cannot answer a question, cannot retry one on its
	// own judgement and cannot merge two. It routes and it times out.
	//
	// Until it is written, Ask answers kNotImplemented to everything.
	class FreeTextProducer
	{
	public:
		enum class Status : std::int32_t
		{
			kNotImplemented = 0,   // the channel is declared, not built
			kOk             = 1,
			kNoModel        = 2,   // the model asked for is not in the build
			kTimedOut       = 3,
			kFailed         = 4
		};

		struct Request
		{
			std::string askedBy;    // the plugin that asks
			std::string askedOf;    // the model mod it asks - by name, chosen by the caller
			std::string answerTo;   // who receives the answer; empty means the asker
			std::string payload;    // carried, never read
		};

		struct Answer
		{
			Status      status{ Status::kNotImplemented };
			std::string payload;   // carried, never read; empty unless kOk
			std::string reason;    // a localisation key, never a sentence
		};

		// Not written. Answers kNotImplemented for every request.
		[[nodiscard]] Answer Ask(const Request& request) const;

		// False until it is built; ask before composing a question.
		[[nodiscard]] static constexpr bool Available() noexcept { return false; }
	};
}
