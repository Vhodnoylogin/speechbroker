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
// WHY THE HEADERS ARE VENDORED AND PINNED. whisper_full takes a
// whisper_full_params BY VALUE. That struct is a hundred and sixty-odd bytes of
// fields in a fixed order, and a copy of it written from memory rather than from
// the real header is a guess whose failure mode is silent: the fields land at
// the wrong offsets and the model is asked for something nobody typed. So
// vendor/whisper.cpp/ holds whisper.h and the four ggml headers it pulls in,
// unmodified, at the exact build named by kPinnedBuild.
//
// AND BECAUSE PINNING IS A PROMISE THE DLL DOES NOT MAKE, the agreement is
// MEASURED at start-up rather than assumed - see LayoutAgrees below. A player
// who drops in a whisper.dll from some other build gets one sentence naming the
// mismatch, not a model that answers rubbish.

#include "Recogniser.h"

#ifndef WIN32_LEAN_AND_MEAN
	#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
	#define NOMINMAX
#endif
#include <Windows.h>

#include "whisper.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <thread>

namespace WhisperRu::Child
{
	namespace
	{
		// The whisper.cpp build vendor/whisper.cpp/ was taken from. It appears in
		// the refusal when the layout check fails, because "your whisper.dll is
		// not the one this was built against" is useless without saying which one
		// that was.
		constexpr const char* kPinnedBuild = "b5130";

		// Whisper's own times are centiseconds. Nothing above this file knows
		// that, and nothing should: the seam speaks milliseconds.
		constexpr std::int64_t kCentisecondMs = 10;

		// The entry points this backend calls, resolved by name. The list is also
		// the check that tells a real whisper.dll from a file somebody renamed:
		// all of them or none.
		struct Api
		{
			whisper_context_params* (*contextDefaults)(void){ nullptr };
			void (*freeContextParams)(whisper_context_params*){ nullptr };
			whisper_context* (*initFromFile)(const char*, whisper_context_params){ nullptr };
			void (*freeContext)(whisper_context*){ nullptr };
			whisper_full_params* (*fullDefaults)(whisper_sampling_strategy){ nullptr };
			void (*freeParams)(whisper_full_params*){ nullptr };
			int (*full)(whisper_context*, whisper_full_params, const float*, int){ nullptr };
			int (*segments)(whisper_context*){ nullptr };
			std::int64_t (*segmentT0)(whisper_context*, int){ nullptr };
			std::int64_t (*segmentT1)(whisper_context*, int){ nullptr };
			const char* (*segmentText)(whisper_context*, int){ nullptr };
			float (*segmentNoSpeech)(whisper_context*, int){ nullptr };
			int (*tokens)(whisper_context*, int){ nullptr };
			whisper_token_data (*tokenData)(whisper_context*, int, int){ nullptr };
			const char* (*tokenText)(whisper_context*, int, int){ nullptr };
			whisper_token (*tokenEot)(whisper_context*){ nullptr };
			void (*logSet)(ggml_log_callback, void*){ nullptr };

			// These two are ggml's rather than whisper's, and they live in
			// ggml.dll. See the comment over LoadComputeBackends.
			void (*backendLoadAllFromPath)(const char*){ nullptr };
			std::size_t (*backendRegCount)(void){ nullptr };
		};

		template <typename T>
		bool Bind(HMODULE a_module, const char* a_name, T& a_slot, const char*& a_missing)
		{
			const auto* found = ::GetProcAddress(a_module, a_name);
			if (!found) {
				a_missing = a_name;
				return false;
			}
			a_slot = reinterpret_cast<T>(found);
			return true;
		}

		// whisper.cpp writes its own running commentary to stderr. This child's
		// stdout IS the pipe and its stderr is nobody's, so the chatter is not
		// dangerous - it is merely noise in a place a person looks when something
		// has gone wrong. Swallowed rather than forwarded: what this mod has to
		// say it says through Log frames, in the player's language.
		void SwallowLog(ggml_log_level, const char*, void*)
		{
		}

		// DOES THE DLL AGREE WITH THE HEADERS WE WERE BUILT AGAINST?
		//
		// The DLL fills a whisper_full_params of its own and hands back a pointer
		// to it. If its field order matches ours, every field below reads as the
		// sane default it is; if the order has moved, pointers read as garbage
		// and floats read as nonsense. The fields are chosen to be spread from
		// the first byte of the struct to the last, so a shift anywhere shows up.
		//
		// This is measurement, not faith, and it is the only thing standing
		// between a mismatched build and a model that quietly answers rubbish.
		bool LayoutAgrees(const Api& a_api)
		{
			auto* greedy = a_api.fullDefaults(WHISPER_SAMPLING_GREEDY);
			auto* beam = a_api.fullDefaults(WHISPER_SAMPLING_BEAM_SEARCH);
			if (!greedy || !beam) {
				if (greedy) {
					a_api.freeParams(greedy);
				}
				if (beam) {
					a_api.freeParams(beam);
				}
				return false;
			}

			const bool agrees =
				// the first field
				greedy->strategy == WHISPER_SAMPLING_GREEDY &&
				beam->strategy == WHISPER_SAMPLING_BEAM_SEARCH &&
				// integers near the front
				greedy->n_threads >= 1 && greedy->n_threads <= 1024 &&
				greedy->n_max_text_ctx > 0 && greedy->n_max_text_ctx <= 65536 &&
				greedy->offset_ms == 0 && greedy->duration_ms == 0 &&
				// pointers in the middle: all null by default, and a shifted
				// layout makes at least one of them a wild value
				greedy->suppress_regex == nullptr &&
				greedy->initial_prompt == nullptr &&
				greedy->prompt_tokens == nullptr &&
				greedy->prompt_n_tokens == 0 &&
				greedy->language != nullptr && std::strlen(greedy->language) <= 8 &&
				// floats past the middle, each with a range it cannot leave
				greedy->temperature >= 0.0f && greedy->temperature <= 2.0f &&
				greedy->max_initial_ts > 0.0f && greedy->max_initial_ts <= 100.0f &&
				greedy->temperature_inc >= 0.0f && greedy->temperature_inc <= 2.0f &&
				greedy->entropy_thold > 0.0f && greedy->entropy_thold < 100.0f &&
				greedy->no_speech_thold > 0.0f && greedy->no_speech_thold <= 1.0f &&
				// the nested struct after them
				beam->beam_search.beam_size >= 1 && beam->beam_search.beam_size <= 64 &&
				// the callbacks after that
				greedy->new_segment_callback == nullptr &&
				greedy->abort_callback == nullptr &&
				greedy->logits_filter_callback == nullptr &&
				// and the last fields of all
				greedy->n_grammar_rules == 0 &&
				greedy->vad == false &&
				greedy->vad_model_path == nullptr;

			a_api.freeParams(greedy);
			a_api.freeParams(beam);
			return agrees;
		}

		// The one weights file, found rather than named. The folder is the unit -
		// SHA256SUMS lives in it and is checked against it - and a GGML model is
		// one file inside that folder, so the folder holding none, or holding
		// two, is a question for a person rather than a choice for us.
		bool SoleWeightsFile(const std::filesystem::path& a_folder, std::filesystem::path& a_file,
			std::size_t& a_found)
		{
			a_found = 0;
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(a_folder, ec)) {
				if (!entry.is_regular_file(ec)) {
					continue;
				}
				if (entry.path().extension() == ".bin") {
					if (a_found == 0) {
						a_file = entry.path();
					}
					++a_found;
				}
			}
			return a_found == 1;
		}

		std::string Trimmed(const char* a_text)
		{
			if (!a_text) {
				return {};
			}
			std::string out{ a_text };
			const auto first = out.find_first_not_of(" \t\r\n");
			if (first == std::string::npos) {
				return {};
			}
			const auto last = out.find_last_not_of(" \t\r\n");
			return out.substr(first, last - first + 1);
		}

		// Whisper punctuates, and its full stop is a CLAIM about the sentence
		// rather than a guess about the audio - which is why this may answer 1
		// and 0 rather than the -1 that means "I do not know". The ellipsis is
		// deliberately not an ending: it is the one mark that says the opposite.
		std::int32_t EndsSentence(const std::string& a_text)
		{
			if (a_text.empty()) {
				return -1;
			}
			// MEASURED, not imagined: large-v3-turbo returns whole quoted
			// sentences - "«Что происходит? Или лучше вернуться?»" - and a check
			// that looked at the last byte saw the closing guillemet and called a
			// finished question unfinished. The adapter would then have held a
			// complete phrase back and the player would have waited for nothing.
			// So the closers come off first, and they are multi-byte in UTF-8.
			static constexpr const char* kClosers[] = {
				"\xC2\xBB",      // »
				"\xE2\x80\x9D",  // ”
				"\xE2\x80\x99",  // ’
				"\"", "'", ")", "]", "}"
			};
			std::string_view text{ a_text };
			for (bool trimming = true; trimming && !text.empty();) {
				trimming = false;
				for (const auto* closer : kClosers) {
					const std::string_view one{ closer };
					if (text.size() > one.size() && text.ends_with(one)) {
						text.remove_suffix(one.size());
						trimming = true;
						break;
					}
				}
			}
			if (text.empty()) {
				return -1;
			}
			// The ellipsis is the one mark that says the opposite of an ending,
			// in both of the spellings a model produces.
			if (text.ends_with("\xE2\x80\xA6") || text.ends_with("...")) {
				return 0;
			}
			const char last = text.back();
			return (last == '.' || last == '?' || last == '!') ? 1 : 0;
		}

		class WhisperRecogniser final : public Recogniser
		{
		public:
			~WhisperRecogniser() override
			{
				if (m_context && m_api.freeContext) {
					m_api.freeContext(m_context);
					m_context = nullptr;
				}
			}

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
				m_library = ::LoadLibraryExW(a_options.library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				if (!m_library) {
					a_why.key = "$SBWHISPERRU_LOG_BACKEND_UNLOADABLE";
					a_why.args = { a_options.library.string(), std::to_string(::GetLastError()) };
					return false;
				}

				auto* const module = static_cast<HMODULE>(m_library);
				const char* missing = nullptr;
				const bool bound =
					Bind(module, "whisper_context_default_params_by_ref", m_api.contextDefaults, missing) &&
					Bind(module, "whisper_free_context_params", m_api.freeContextParams, missing) &&
					Bind(module, "whisper_init_from_file_with_params", m_api.initFromFile, missing) &&
					Bind(module, "whisper_free", m_api.freeContext, missing) &&
					Bind(module, "whisper_full_default_params_by_ref", m_api.fullDefaults, missing) &&
					Bind(module, "whisper_free_params", m_api.freeParams, missing) &&
					Bind(module, "whisper_full", m_api.full, missing) &&
					Bind(module, "whisper_full_n_segments", m_api.segments, missing) &&
					Bind(module, "whisper_full_get_segment_t0", m_api.segmentT0, missing) &&
					Bind(module, "whisper_full_get_segment_t1", m_api.segmentT1, missing) &&
					Bind(module, "whisper_full_get_segment_text", m_api.segmentText, missing) &&
					Bind(module, "whisper_full_get_segment_no_speech_prob", m_api.segmentNoSpeech, missing) &&
					Bind(module, "whisper_full_n_tokens", m_api.tokens, missing) &&
					Bind(module, "whisper_full_get_token_data", m_api.tokenData, missing) &&
					Bind(module, "whisper_full_get_token_text", m_api.tokenText, missing) &&
					Bind(module, "whisper_token_eot", m_api.tokenEot, missing) &&
					Bind(module, "whisper_log_set", m_api.logSet, missing);
				if (!bound) {
					a_why.key = "$SBWHISPERRU_LOG_BACKEND_WRONG";
					a_why.args = { a_options.library.string(), missing };
					return false;
				}

				m_api.logSet(&SwallowLog, nullptr);

				if (!LoadComputeBackends(a_options.library, a_why)) {
					return false;
				}

				if (!LayoutAgrees(m_api)) {
					a_why.key = "$SBWHISPERRU_LOG_BACKEND_MISMATCH";
					a_why.args = { a_options.library.string(), kPinnedBuild };
					return false;
				}

				if (!std::filesystem::is_directory(a_options.weights, ec)) {
					a_why.key = "$SBWHISPERRU_LOG_WEIGHTS_MISSING";
					a_why.args = { a_options.weights.string() };
					return false;
				}
				std::size_t found = 0;
				if (!SoleWeightsFile(a_options.weights, m_weightsFile, found)) {
					a_why.key = "$SBWHISPERRU_LOG_WEIGHTS_NOT_ONE";
					a_why.args = { a_options.weights.string(), std::to_string(found) };
					return false;
				}

				auto* cparams = m_api.contextDefaults();
				if (!cparams) {
					a_why.key = "$SBWHISPERRU_LOG_BACKEND_MISMATCH";
					a_why.args = { a_options.library.string(), kPinnedBuild };
					return false;
				}
				// "cuda" is what the settings call it and what a person means; a
				// CPU-only build of whisper.cpp simply has no GPU backend
				// registered and runs on the processor, which is the right
				// outcome rather than a refusal - the model is slower, the
				// adapter measures that, and the standing follows the measurement.
				cparams->use_gpu = (a_options.device != "cpu");
				cparams->gpu_device = 0;
				m_context = m_api.initFromFile(m_weightsFile.string().c_str(), *cparams);
				m_api.freeContextParams(cparams);
				if (!m_context) {
					a_why.key = "$SBWHISPERRU_LOG_WEIGHTS_UNREADABLE";
					a_why.args = { m_weightsFile.string() };
					return false;
				}

				m_options = a_options;
				return true;
			}

			void SetVocabulary(const std::vector<std::string>& a_phrases) override
			{
				// Whisper takes the vocabulary as an initial PROMPT - a piece of
				// text it pretends it has already heard - and not as a list it
				// searches. Clipping already happened in the shim, against the
				// contract's ceiling and the settings, because a long prompt
				// competes for the same window that has to hold the answer.
				m_prompt.clear();
				for (const auto& one : a_phrases) {
					if (one.empty()) {
						continue;
					}
					if (!m_prompt.empty()) {
						m_prompt += ", ";
					}
					m_prompt += one;
				}
			}

			bool Recognise(const std::vector<float>& a_samples, std::int32_t a_sampleRate,
				std::vector<Piece>& a_pieces, std::string& a_failed) override
			{
				a_pieces.clear();

				if (a_sampleRate != WHISPER_SAMPLE_RATE) {
					// Not a degradation to resample around: the contract fixes
					// 16 kHz precisely so that nobody has to guess what the other
					// side meant, and a buffer at another rate is a bug upstream.
					a_failed = "the buffer is at " + std::to_string(a_sampleRate) +
						" Hz and this backend takes " + std::to_string(WHISPER_SAMPLE_RATE);
					return false;
				}
				if (!m_context) {
					a_failed = "the backend was never opened";
					return false;
				}
				if (a_samples.empty()) {
					// Lawful, and the honest answer is no fragments. Handing
					// whisper.cpp a zero-length buffer is not.
					return true;
				}

				const bool beams = m_options.beamSize > 1;
				auto* params = m_api.fullDefaults(
					beams ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
				if (!params) {
					a_failed = "the backend would not give its default parameters";
					return false;
				}

				params->n_threads = Threads();
				params->translate = false;
				// EVERY PASS IS A COMPLETE RE-READING FROM SAMPLE ZERO, so the
				// decoder must not be told what the previous one produced. With
				// context carried, the same audio read twice gives two different
				// answers, and the arbiter's whole job - comparing two readings
				// of one sound - stops meaning anything.
				params->no_context = true;
				params->single_segment = false;
				params->print_special = false;
				params->print_progress = false;
				params->print_realtime = false;
				params->print_timestamps = false;
				// The word timings the lane is picked on. Without this the piece
				// must leave lastWordProb and medianGapMs at -1, and this backend
				// never gets to hold the lane.
				params->token_timestamps = true;
				params->suppress_blank = true;
				if (beams) {
					params->beam_search.beam_size = m_options.beamSize;
				}
				if (!m_options.language.empty()) {
					params->language = m_options.language.c_str();
				}
				if (!m_prompt.empty()) {
					params->initial_prompt = m_prompt.c_str();
				}

				const int rc = m_api.full(m_context, *params, a_samples.data(),
					static_cast<int>(a_samples.size()));
				m_api.freeParams(params);
				if (rc != 0) {
					a_failed = "whisper_full failed with " + std::to_string(rc);
					return false;
				}

				const int count = m_api.segments(m_context);
				a_pieces.reserve(static_cast<std::size_t>(count < 0 ? 0 : count));
				for (int i = 0; i < count; ++i) {
					Piece piece;
					piece.startMs = static_cast<std::int32_t>(m_api.segmentT0(m_context, i) * kCentisecondMs);
					piece.endMs = static_cast<std::int32_t>(m_api.segmentT1(m_context, i) * kCentisecondMs);
					piece.text = Trimmed(m_api.segmentText(m_context, i));
					if (piece.text.empty()) {
						continue;
					}
					piece.endsSentence = EndsSentence(piece.text);
					// Whisper computes this ONCE PER THIRTY-SECOND WINDOW, not per
					// segment, so every piece cut out of one window carries the
					// same number. It is passed on as the model's own claim and
					// nothing is read into it here - measured on this build, it
					// sits around 2e-5 whether the take is speech or pure silence,
					// which is exactly why the adapter has a silence probe of its
					// own rather than trusting this field.
					piece.noSpeechProb = m_api.segmentNoSpeech(m_context, i);
					Measure(i, piece);
					a_pieces.push_back(std::move(piece));
				}
				return true;
			}

		private:
			// THE TRAP THAT COSTS AN AFTERNOON IF IT IS NOT WRITTEN DOWN.
			//
			// whisper.dll computes nothing by itself. Since ggml went modular,
			// every compute device - the processor, CUDA, Vulkan - lives in a
			// ggml-<name>.dll of its own and has to be REGISTERED before a model
			// is loaded. whisper.dll does not do it; whisper-cli.exe does it in
			// its own start-up code, which is why the command-line tool works
			// beside the very same DLLs that leave us with nothing.
			//
			// And the failure is the worst shape a failure has: with no device
			// registered, loading the model trips GGML_ASSERT(device) deep
			// inside ggml and ABORTS THE PROCESS. Not an error return, not an
			// exception - the child is simply gone, and the shim is left holding
			// an exit code for a question nobody asked.
			//
			// So the registration is done here, explicitly, NAMING THE DIRECTORY
			// - which is the same rule the rest of this file keeps: ggml is
			// perfectly willing to go looking on its own, and a backend found by
			// a search is a backend nobody can name afterwards. Then the count is
			// read back, because "it registered nothing" has to become a sentence
			// rather than an abort.
			bool LoadComputeBackends(const std::filesystem::path& a_library, Refusal& a_why)
			{
				const auto folder = a_library.parent_path();
				const auto ggml = folder / "ggml.dll";

				std::error_code ec;
				if (!std::filesystem::exists(ggml, ec)) {
					a_why.key = "$SBWHISPERRU_LOG_GGML_MISSING";
					a_why.args = { ggml.string() };
					return false;
				}
				// ALTERED_SEARCH_PATH so that ggml.dll's own siblings are looked
				// for beside IT rather than beside whoever started us.
				m_ggml = ::LoadLibraryExW(ggml.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				if (!m_ggml) {
					a_why.key = "$SBWHISPERRU_LOG_BACKEND_UNLOADABLE";
					a_why.args = { ggml.string(), std::to_string(::GetLastError()) };
					return false;
				}

				auto* const module = static_cast<HMODULE>(m_ggml);
				const char* missing = nullptr;
				if (!Bind(module, "ggml_backend_load_all_from_path", m_api.backendLoadAllFromPath, missing) ||
					!Bind(module, "ggml_backend_reg_count", m_api.backendRegCount, missing)) {
					a_why.key = "$SBWHISPERRU_LOG_BACKEND_WRONG";
					a_why.args = { ggml.string(), missing };
					return false;
				}

				m_api.backendLoadAllFromPath(folder.string().c_str());
				if (m_api.backendRegCount() == 0) {
					a_why.key = "$SBWHISPERRU_LOG_NO_COMPUTE";
					a_why.args = { folder.string() };
					return false;
				}
				return true;
			}

			int Threads() const
			{
				if (m_options.threads > 0) {
					return m_options.threads;
				}
				const auto available = static_cast<int>(std::thread::hardware_concurrency());
				return std::clamp(available > 0 ? available : 4, 1, 8);
			}

			// Everything a piece knows that comes from the tokens rather than
			// from the segment: the score, the word timings, and the count.
			void Measure(int a_segment, Piece& a_piece) const
			{
				const int total = m_api.tokens(m_context, a_segment);
				const whisper_token eot = m_api.tokenEot(m_context);

				double logSum = 0.0;
				int counted = 0;
				int words = 0;
				float lastProb = -1.0f;
				std::int64_t previousEnd = -1;
				std::vector<std::int32_t> gaps;

				for (int t = 0; t < total; ++t) {
					const auto data = m_api.tokenData(m_context, a_segment, t);
					// Special tokens are the timestamps and the control markers.
					// They carry no probability a person would recognise and they
					// are not words, so they take part in nothing below.
					if (data.id >= eot) {
						continue;
					}
					logSum += static_cast<double>(data.plog);
					++counted;
					lastProb = data.p;

					const char* text = m_api.tokenText(m_context, a_segment, t);
					if (counted == 1 || (text && text[0] == ' ')) {
						++words;
					}
					if (previousEnd >= 0 && data.t0 >= previousEnd) {
						gaps.push_back(static_cast<std::int32_t>((data.t0 - previousEnd) * kCentisecondMs));
					}
					previousEnd = data.t1;
				}

				if (counted > 0) {
					// exp of the mean log probability: the model's own confidence
					// and nothing else. Trust was once multiplied in here, and a
					// model that was right nine times in ten cut a tenth off
					// every number it produced until correct phrases fell under
					// the threshold.
					a_piece.score = static_cast<float>(std::exp(logSum / counted));
					a_piece.words = words;
					a_piece.lastWordProb = lastProb;
				}
				if (!gaps.empty()) {
					const auto middle = gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2);
					std::nth_element(gaps.begin(), middle, gaps.end());
					a_piece.medianGapMs = *middle;
				}
			}

			void*                    m_library{ nullptr };
			void*                    m_ggml{ nullptr };
			Api                      m_api;
			whisper_context*         m_context{ nullptr };
			std::filesystem::path    m_weightsFile;
			Options                  m_options;
			std::string              m_prompt;
		};
	}

	std::unique_ptr<Recogniser> MakeWhisperRecogniser()
	{
		return std::make_unique<WhisperRecogniser>();
	}
}
