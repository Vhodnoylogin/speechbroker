// ONE WAV THROUGH THE REAL PATH, WITHOUT THE GAME AND WITHOUT THE SHIM.
//
// The child otherwise speaks nothing but the binary protocol, which means that
// until now the only way to find out whether recognition works at all was to
// lay the mod out, start Skyrim, put on a headset and talk. That is minutes per
// attempt for a question that takes a second to answer, and a loop that long
// stops being used - which is how a backend ends up shipped untested.
//
// So: --wav <file> opens the backend exactly as the protocol path opens it -
// the same preload, the same library, the same layout check, the same weights -
// runs one buffer through Recognise, and prints what came back. Nothing here is
// a mock. If this prints the words that were spoken, the mod recognises speech.
//
// It is not a test harness and does not judge the answer: what is correct for a
// given take is the business of tools/audiolab, which owns the reference texts.
// This says what the backend said.
#pragma once

#include <filesystem>

namespace WhisperRu::Child
{
	class Recogniser;

	// Reads a 16 kHz mono wav, runs it through an already-opened recogniser and
	// writes the pieces to stdout as text. Returns the process exit code.
	int RunSelfTest(Recogniser& a_recogniser, const std::filesystem::path& a_wav);
}
