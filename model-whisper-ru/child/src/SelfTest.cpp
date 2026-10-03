#include "SelfTest.h"

#include "Recogniser.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace WhisperRu::Child
{
	namespace
	{
		constexpr std::int32_t kWanted = 16000;

		// A RIFF walk rather than a struct read, because the header is not one
		// fixed shape: writers put a fact chunk, or a LIST, or padding between
		// fmt and data, and a program that assumes data starts at byte 44 reads
		// a chunk header as its first eleven samples and hears a click.
		bool ReadWav(const std::filesystem::path& a_path, std::vector<float>& a_samples,
			std::int32_t& a_rate, std::string& a_why)
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				a_why = "cannot open " + a_path.string();
				return false;
			}
			std::vector<char> all((std::istreambuf_iterator<char>(file)),
				std::istreambuf_iterator<char>());
			if (all.size() < 12 || std::memcmp(all.data(), "RIFF", 4) != 0 ||
				std::memcmp(all.data() + 8, "WAVE", 4) != 0) {
				a_why = "not a RIFF/WAVE file: " + a_path.string();
				return false;
			}

			auto u16 = [&all](std::size_t at) {
				return static_cast<std::uint16_t>(
					static_cast<unsigned char>(all[at]) |
					(static_cast<unsigned char>(all[at + 1]) << 8));
			};
			auto u32 = [&all](std::size_t at) {
				return static_cast<std::uint32_t>(
					static_cast<unsigned char>(all[at]) |
					(static_cast<unsigned char>(all[at + 1]) << 8) |
					(static_cast<unsigned char>(all[at + 2]) << 16) |
					(static_cast<unsigned char>(all[at + 3]) << 24));
			};

			std::uint16_t format = 0;
			std::uint16_t channels = 0;
			std::uint16_t bits = 0;
			std::size_t dataAt = 0;
			std::size_t dataSize = 0;

			std::size_t at = 12;
			while (at + 8 <= all.size()) {
				const std::size_t size = u32(at + 4);
				const std::size_t body = at + 8;
				if (std::memcmp(all.data() + at, "fmt ", 4) == 0 && body + 16 <= all.size()) {
					format = u16(body);
					channels = u16(body + 2);
					a_rate = static_cast<std::int32_t>(u32(body + 4));
					bits = u16(body + 14);
				} else if (std::memcmp(all.data() + at, "data", 4) == 0) {
					dataAt = body;
					dataSize = (body + size <= all.size()) ? size : (all.size() - body);
				}
				at = body + size + (size & 1);  // chunks are word-aligned
			}

			if (dataAt == 0) {
				a_why = "no data chunk in " + a_path.string();
				return false;
			}
			if (channels != 1) {
				a_why = "the take has " + std::to_string(channels) + " channels and this wants mono";
				return false;
			}
			if (format == 1 && bits == 16) {
				a_samples.resize(dataSize / 2);
				for (std::size_t i = 0; i < a_samples.size(); ++i) {
					const auto raw = static_cast<std::int16_t>(u16(dataAt + i * 2));
					a_samples[i] = static_cast<float>(raw) / 32768.0f;
				}
				return true;
			}
			if (format == 3 && bits == 32) {
				a_samples.resize(dataSize / 4);
				std::memcpy(a_samples.data(), all.data() + dataAt, a_samples.size() * 4);
				return true;
			}
			a_why = "the take is format " + std::to_string(format) + " at " +
				std::to_string(bits) + " bits, and this reads 16-bit PCM or 32-bit float";
			return false;
		}

		// -1 is "I do not know" everywhere in this interface, and it has to LOOK
		// different from a number or the whole point of the sentinel is lost on
		// whoever reads the output.
		std::string Or(std::int32_t a_value)
		{
			return a_value < 0 ? std::string{ "-" } : std::to_string(a_value);
		}

		std::string Or(float a_value)
		{
			if (a_value < 0.0f) {
				return "-";
			}
			char buffer[16]{};
			std::snprintf(buffer, sizeof(buffer), "%.5f", static_cast<double>(a_value));
			return buffer;
		}
	}

	int RunSelfTest(Recogniser& a_recogniser, const std::filesystem::path& a_wav)
	{
		std::vector<float> samples;
		std::int32_t rate = 0;
		std::string why;
		if (!ReadWav(a_wav, samples, rate, why)) {
			std::printf("wav: %s\n", why.c_str());
			return 2;
		}
		if (rate != kWanted) {
			std::printf("wav: %s is at %d Hz and the contract fixes %d\n",
				a_wav.string().c_str(), rate, kWanted);
			return 2;
		}

		std::printf("take    %s\n", a_wav.filename().string().c_str());
		std::printf("samples %zu (%.2f s at %d Hz)\n", samples.size(),
			static_cast<double>(samples.size()) / kWanted, kWanted);

		std::vector<Piece> pieces;
		std::string failed;
		const auto began = std::chrono::steady_clock::now();
		const bool ok = a_recogniser.Recognise(samples, rate, pieces, failed);
		const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - began).count();

		if (!ok) {
			std::printf("refused %s\n", failed.c_str());
			return 3;
		}

		std::printf("took    %lld ms\n", static_cast<long long>(took));
		std::printf("pieces  %zu\n\n", pieces.size());
		for (const auto& piece : pieces) {
			std::printf("  [%6d..%6d ms] score %.3f  ends %s  nospeech %s  words %s  lastword %s  gap %s\n",
				piece.startMs, piece.endMs, static_cast<double>(piece.score),
				Or(piece.endsSentence).c_str(), Or(piece.noSpeechProb).c_str(),
				Or(piece.words).c_str(), Or(piece.lastWordProb).c_str(),
				Or(piece.medianGapMs).c_str());
			std::printf("  %s\n\n", piece.text.c_str());
		}
		return 0;
	}
}
