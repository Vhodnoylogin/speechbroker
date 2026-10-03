// The wire between the shim and its child process. Both sides compile THIS
// file - the shim from src/core, the child from child/CMakeLists.txt - so the
// layout cannot drift between them the way two hand-written parsers do.
//
// It is written down in prose in docs/child-protocol.md, and the prose is the
// specification: somebody must be able to write another child against it
// without reading a line of this header. Where the two disagree, the prose is
// wrong and is to be corrected, because it is the one a stranger reads.
//
// WHY A BINARY FRAME AND NOT JSON OR A LINE PROTOCOL. What crosses here is
// float samples - 640 kB on a ten-second final pass, and every pass carries the
// buffer from sample zero (the contract says why). Base64 in json would add a
// third to that and a parse of it to every pass, for a message whose only
// variable-length parts are the samples and a few strings. And a text protocol
// has no natural frame: the moment the child writes one unexpected line to
// stdout the stream is desynchronised for ever, whereas a length-prefixed frame
// with a magic word resynchronises or fails loudly.
//
// EVERYTHING IS LITTLE-ENDIAN AND FIXED-WIDTH, and the sizes are asserted at the
// foot of this file. The child may be rebuilt by somebody else with another
// compiler; that is the same reason the contract with the adapter is plain C.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace WhisperRu::Wire
{
	// 'S' 'B' 'V' 'C' read as a little-endian u32. Written as a number rather
	// than as a four-character literal for the same reason the contract's own
	// message id is: a multi-character literal has an implementation-defined
	// value and is not even the same thing in C as in C++.
	inline constexpr std::uint32_t kMagic = 0x43564253u;

	// It grows by one, never branches - the same rule the contract keeps. The
	// child announces the version it speaks in its Hello, and the shim refuses a
	// child it cannot read rather than guessing.
	inline constexpr std::uint16_t kVersion = 1;

	inline constexpr std::uint32_t kHeaderBytes = 16;

	// A ceiling that exists so that a desynchronised stream cannot ask for a
	// gigabyte. 64 MiB is sixteen times the largest lawful request (the
	// contract's maxRequestSamples is 20 s, 1.28 MB), which leaves room for a
	// setting to be raised without touching this number.
	inline constexpr std::uint32_t kMaxBodyBytes = 64u * 1024u * 1024u;

	// The same ceiling the contract puts on its own strings, so that a string
	// the child sends can be handed to the adapter unmeasured.
	inline constexpr std::uint32_t kMaxStringBytes = 4096;

	// The same ceiling the contract puts on one answer's fragment array.
	inline constexpr std::int32_t kMaxFragments = 4096;

	enum class Type : std::uint16_t
	{
		Hello      = 1,  // child -> shim, once: I am up and my model is loaded
		Request    = 3,  // shim -> child: one whole buffer, from sample zero
		Reply      = 4,  // child -> shim: one reading of that whole buffer
		Vocabulary = 5,  // shim -> child: advisory phrases
		Cancel     = 6,  // shim -> child: advisory, the answer is no longer wanted
		Log        = 7,  // child -> shim: a localisation key and its arguments
		Bye        = 8   // shim -> child: stop; the child exits 0
	};

	// The status of a Reply. These are the three the contract lets an answer
	// carry, with the same numbers, so that nothing has to be translated in the
	// middle: SpeechBrokerVoiceStatus OK, CANCELLED, FAILED.
	inline constexpr std::int32_t kStatusOk        = 1;
	inline constexpr std::int32_t kStatusCancelled = 5;
	inline constexpr std::int32_t kStatusFailed    = 6;

	// The sentinels. -1 where a value is not known, NEVER zero - zero is a
	// claim, and the contract explains at length which of these claims is
	// dangerous (lastWordProb) and which is merely ambiguous (medianGapMs).
	inline constexpr std::int32_t kUnknownInt   = -1;
	inline constexpr float        kUnknownFloat = -1.0f;

	struct Header
	{
		std::uint32_t magic{ kMagic };
		std::uint16_t version{ kVersion };
		std::uint16_t type{ 0 };
		std::uint32_t bodyBytes{ 0 };
		std::uint32_t reserved{ 0 };
	};

	// One piece of the answer. The fields are the contract's fragment, in the
	// contract's order, minus nothing: the child is where the sentinels are
	// decided, and the shim must not invent a value the child did not send.
	struct Fragment
	{
		std::int32_t startMs{ 0 };
		std::int32_t endMs{ 0 };
		float        score{ 0.0f };
		std::int32_t endsSentence{ kUnknownInt };
		float        lastWordProb{ kUnknownFloat };
		float        noSpeechProb{ kUnknownFloat };
		std::int32_t medianGapMs{ kUnknownInt };
		std::int32_t words{ kUnknownInt };
		std::string  text;
	};

	struct Hello
	{
		std::uint32_t protocolVersion{ kVersion };
		std::uint32_t maxSamples{ 0 };  // the longest buffer this child will take
		std::string   backend;          // "whisper.cpp" - for the log, never routed on
		std::string   modelId;          // echoed from the command line, so a mix-up is visible
	};

	struct Request
	{
		std::int64_t       utteranceId{ 0 };
		std::int64_t       turnId{ 0 };
		std::int32_t       serial{ 0 };
		std::int32_t       final{ 0 };
		std::uint32_t      lostSamples{ 0 };
		std::uint32_t      deadlineMs{ 0 };
		std::vector<float> samples;
	};

	struct Reply
	{
		std::int64_t          utteranceId{ 0 };
		std::int32_t          serial{ 0 };
		std::int32_t          status{ kStatusFailed };
		std::int32_t          latencyMs{ kUnknownInt };
		std::uint32_t         lostSamples{ 0 };
		std::vector<Fragment> fragments;
		std::string           failed;  // empty unless status is failed
	};

	struct LogLine
	{
		std::int32_t             level{ 1 };
		std::string              key;
		std::vector<std::string> args;
	};

	// ------------------------------------------------------------------ write
	// Each of these returns one complete frame, header included, ready to be
	// handed to a single write. Building the whole frame before writing is not
	// tidiness: a header written separately from its body is a torn frame the
	// moment anything goes wrong between the two writes, and the reader on the
	// far side would then be desynchronised rather than merely starved.
	std::vector<std::uint8_t> EncodeHello(const Hello& a_hello);
	std::vector<std::uint8_t> EncodeRequest(const Request& a_request);
	std::vector<std::uint8_t> EncodeReply(const Reply& a_reply);
	std::vector<std::uint8_t> EncodeVocabulary(const std::vector<std::string>& a_phrases);
	std::vector<std::uint8_t> EncodeCancel(std::int64_t a_utteranceId);
	std::vector<std::uint8_t> EncodeLog(const LogLine& a_line);
	std::vector<std::uint8_t> EncodeBye();

	// ------------------------------------------------------------------- read
	// A header is read from exactly kHeaderBytes; false means it is not one of
	// ours - wrong magic, a version we cannot read, or a body above the ceiling
	// - and the caller must then treat the stream as lost rather than skip.
	bool DecodeHeader(const std::uint8_t* a_bytes, std::size_t a_count, Header& a_out);

	// Every body decoder takes the body alone, without the header, and answers
	// false on anything it cannot read whole. None of them throws: the far side
	// of this pipe is a process that may have been replaced by somebody else's.
	bool DecodeHello(const std::uint8_t* a_body, std::size_t a_count, Hello& a_out);
	bool DecodeRequest(const std::uint8_t* a_body, std::size_t a_count, Request& a_out);
	bool DecodeReply(const std::uint8_t* a_body, std::size_t a_count, Reply& a_out);
	bool DecodeVocabulary(const std::uint8_t* a_body, std::size_t a_count, std::vector<std::string>& a_out);
	bool DecodeCancel(const std::uint8_t* a_body, std::size_t a_count, std::int64_t& a_out);
	bool DecodeLog(const std::uint8_t* a_body, std::size_t a_count, LogLine& a_out);

	static_assert(sizeof(float) == 4, "the wire carries float32 samples");
	static_assert(sizeof(std::int64_t) == 8, "the wire carries 64-bit ids");
}
