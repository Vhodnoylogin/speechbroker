// THE BACKEND SEAM. One interface, and behind it whatever actually recognises
// speech.
//
// It is here because the child process is the only part of this mod that a
// third-party runtime ever touches, and the runtime is the part most likely to
// be replaced: whisper.cpp today, something else tomorrow, a CPU build on a
// machine with no CUDA. Everything on the near side of this interface - the
// protocol, the framing, the exit codes, the watchdog - is ours and does not
// change when the far side does.
//
// IT IS ALSO WHERE THE SENTINELS ARE DECIDED, and that is the subtle part.
// -1 means "I do not know" and the adapter degrades to a flat guess; 0 is a
// CLAIM. lastWordProb is the dangerous one: the adapter picks one model's
// segmentation as the lane onto which every other model's text is matched, and
// it picks from among the models that carry word timings, tested as exactly
// lastWordProb and medianGapMs. A backend that divides a segment's time
// proportionally by string length - which is what you do when the model returns
// no word timings - MUST leave both at -1, or it wins the lane with boundaries
// that move between passes. Only fill them from a real alignment to the audio.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace WhisperRu::Child
{
	// A refusal that a person can act on: a localisation key of this module's
	// own table and its arguments. The child renders NOTHING - it has no table
	// and wants none. The key and the arguments travel up the pipe as a Log
	// frame, the shim hands them to the adapter's Host::Log verbatim, and the
	// adapter writes them into its log. So a new message here needs no edit
	// anywhere but localization/.
	struct Refusal
	{
		std::string              key;
		std::vector<std::string> args;
	};

	struct Piece
	{
		std::int32_t startMs{ 0 };
		std::int32_t endMs{ 0 };
		std::string  text;

		// The model's own confidence in this piece, as a probability in [0, 1],
		// higher meaning better - exp(avg_logprob) for Whisper. NOT normalised
		// and NOT multiplied by any opinion the model holds of itself: trust was
		// once multiplied in here, and a model that was right nine times in ten
		// cut a tenth off every number it produced until correct phrases fell
		// under the threshold.
		float        score{ 0.0f };

		std::int32_t endsSentence{ -1 };
		float        lastWordProb{ -1.0f };
		float        noSpeechProb{ -1.0f };
		std::int32_t medianGapMs{ -1 };
		std::int32_t words{ -1 };
	};

	class Recogniser
	{
	public:
		struct Options
		{
			std::filesystem::path              weights;
			std::filesystem::path              library;
			std::vector<std::filesystem::path> preload;
			std::string                        device;
			std::string                        computeType;
			std::string                        language;
			std::int32_t                       beamSize{ 5 };
			std::int32_t                       threads{ 0 };
		};

		virtual ~Recogniser() = default;

		// Brings the backend up. False means the child must leave, and a_why
		// says which file it wanted - the one question a person asks when a
		// model mod does nothing.
		virtual bool Open(const Options& a_options, Refusal& a_why) = 0;

		// Advisory. Clipping is expected and is not a failure.
		virtual void SetVocabulary(const std::vector<std::string>& a_phrases) = 0;

		// One whole buffer, from sample zero, mono float32 at a_sampleRate. It
		// re-reads all of it and gives back its own full segmentation of it -
		// never a delta, because the adapter has to be able to compare any two
		// answers with each other.
		//
		// A BUFFER THAT IS QUIET THROUGHOUT IS LAWFUL and must be answered like
		// any other: the adapter sends one second of digital silence as an
		// ordinary request straight after start-up, and a model that answers it
		// with text is recorded as one that invents. Do not refuse it, do not
		// trim it to nothing, and do not try to recognise the probe - the honest
		// answer to silence is no fragments at all.
		virtual bool Recognise(const std::vector<float>& a_samples, std::int32_t a_sampleRate,
			std::vector<Piece>& a_pieces, std::string& a_failed) = 0;
	};

	// The one implementation: whisper.cpp's C API, loaded from a DLL by full
	// path at run time. Nothing links against it, and this child runs - and
	// refuses, legibly - on a machine that has never had it.
	std::unique_ptr<Recogniser> MakeWhisperRecogniser();
}
