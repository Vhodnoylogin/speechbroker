// whisper.cpp behind the seam, loaded rather than linked.
//
// WHY LOADED BY FULL PATH AND NOT LINKED. Linking would make this child refuse
// to START without whisper.dll beside it - a hard import failure, "the
// application was unable to start correctly (0xc000007b)", with nothing to read
// and no way to say which file was missing. Loading it ourselves turns the same
// situation into one sentence naming the file we wanted, which is the whole
// difference between a mod that is broken and a mod that tells you what to drop
// in.
//
// FULL PATH, and this is the rule the contract states for the shim and that is
// kept here as well: never PATH, never SetDefaultDllDirectories. In this process
// there is nobody else to hurt, but a dependency found by a search is a
// dependency nobody can name afterwards, and "which cudnn did it actually load"
// is the question this whole design exists to be able to answer.
//
// WHAT IS NOT HERE, said plainly rather than discovered. whisper.cpp is neither
// vendored nor built by this repository, and the inference call is therefore not
// wired: this file resolves the library and its entry points - which is a real
// check, and proves the DLL is whisper.cpp rather than something else wearing
// the name - and Recognise answers a failure saying so. What a person must drop
// in, and what is left to do, is in child/README.md.

#include "Recogniser.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <Windows.h>

namespace WhisperRu::Child
{
	namespace
	{
		// The entry points of whisper.cpp's C API that this backend needs. They
		// are resolved by NAME and not called through a declared signature: the
		// one that matters, whisper_full, takes a whisper_full_params BY VALUE,
		// and a struct of that size declared from memory rather than from
		// whisper.h is exactly the kind of guess this project does not make. So
		// their presence is checked - it is what tells a real whisper.dll from a
		// file somebody renamed - and the call itself waits for the header.
		constexpr const char* kEntryPoints[] = {
			"whisper_init_from_file_with_params",
			"whisper_free",
			"whisper_full",
			"whisper_full_default_params",
			"whisper_full_n_segments",
			"whisper_full_get_segment_text",
			"whisper_full_get_segment_t0",
			"whisper_full_get_segment_t1"
		};

		class WhisperRecogniser final : public Recogniser
		{
		public:
			bool Open(const Options& a_options, Refusal& a_why) override
			{
				// The dependencies first and by full path, in the order given:
				// a later bare-name load inside whisper.dll's own imports then
				// finds an already-loaded module and resolves without any search
				// policy being changed at all.
				for (const auto& one : a_options.preload) {
					std::error_code ec;
					if (!std::filesystem::exists(one, ec)) {
						a_why.key = "$SBWHISPERRU_LOG_PRELOAD_MISSING";
						a_why.args = { one.string() };
						return false;
					}
					if (!::LoadLibraryW(one.c_str())) {
						a_why.key = "$SBWHISPERRU_LOG_PRELOAD_FAILED";
						a_why.args = { one.string(), std::to_string(::GetLastError()) };
						return false;
					}
				}

				std::error_code ec;
				if (!std::filesystem::exists(a_options.library, ec)) {
					// The sentence a person actually needs: which file, by full
					// path, was wanted and is not there.
					a_why.key = "$SBWHISPERRU_LOG_BACKEND_MISSING";
					a_why.args = { a_options.library.string() };
					return false;
				}
				m_library = ::LoadLibraryW(a_options.library.c_str());
				if (!m_library) {
					a_why.key = "$SBWHISPERRU_LOG_BACKEND_UNLOADABLE";
					a_why.args = { a_options.library.string(), std::to_string(::GetLastError()) };
					return false;
				}

				for (const auto* name : kEntryPoints) {
					if (!::GetProcAddress(static_cast<HMODULE>(m_library), name)) {
						a_why.key = "$SBWHISPERRU_LOG_BACKEND_WRONG";
						a_why.args = { a_options.library.string(), name };
						return false;
					}
				}

				if (!std::filesystem::is_directory(a_options.weights, ec)) {
					a_why.key = "$SBWHISPERRU_LOG_WEIGHTS_MISSING";
					a_why.args = { a_options.weights.string() };
					return false;
				}
				m_options = a_options;
				return true;
			}

			void SetVocabulary(const std::vector<std::string>& a_phrases) override
			{
				// Kept rather than used, for now: it becomes the initial prompt
				// when the inference is wired. Clipping already happened in the
				// shim, against the contract's ceiling and the settings.
				m_vocabulary = a_phrases;
			}

			bool Recognise(const std::vector<float>& a_samples, std::int32_t a_sampleRate,
				std::vector<Piece>& a_pieces, std::string& a_failed) override
			{
				(void)a_samples;
				(void)a_sampleRate;
				a_pieces.clear();
				// A failure is an answer: it is counted against this model, it
				// costs it its standing, and it does not take the other models
				// down with it. Which is the right shape for "the backend is
				// present but this build cannot call it" - the adapter will
				// weigh this model down and carry on with whatever else is
				// installed, and the text says exactly what is missing.
				a_failed = "whisper.cpp inference is not wired in this build - see child/README.md";
				return false;
			}

		private:
			void*       m_library{ nullptr };
			Options     m_options;
			std::vector<std::string> m_vocabulary;
		};
	}

	std::unique_ptr<Recogniser> MakeWhisperRecogniser()
	{
		return std::make_unique<WhisperRecogniser>();
	}
}
