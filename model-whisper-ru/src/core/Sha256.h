// Why a model mod hashes its own weights.
//
// It is measured, not feared: five hundred bytes flipped inside a converted
// model.bin produce NO error of any kind - not at load, not at inference - and
// a model that returns rubbish. Nothing above this line can tell that apart
// from a bad recording or a hard accent, because the contract has no channel
// for "my weights are wrong": an answer is text and a score, and rubbish has
// both. So the check has to be here, before the child is raised, or it does not
// exist at all.
//
// The sums are a file beside the weights, in the format sha256sum has written
// since 1999 - "<64 hex>  <relative path>", two spaces - so that a person can
// verify them with any tool they already have and tools/weights.ps1 is a
// convenience rather than the only way in.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace WhisperRu
{
	// A streaming SHA-256. Streaming rather than whole-file because a weights
	// file is gigabytes and reading one into memory to hash it would double the
	// peak of the very bring-up this is meant to protect.
	class Sha256
	{
	public:
		Sha256();

		void        Update(const std::uint8_t* a_bytes, std::size_t a_count);
		std::string Hex();  // finishes the digest; the object is spent afterwards

		// The whole of one file, lower-case hex. Empty on a file that could not
		// be read - the caller distinguishes that from a mismatch, because the
		// two are different faults with different advice.
		static std::string OfFile(const std::filesystem::path& a_path);

	private:
		void Block(const std::uint8_t* a_block);

		std::array<std::uint32_t, 8> m_state{};
		std::array<std::uint8_t, 64>  m_buffer{};
		std::uint64_t                 m_bits{ 0 };
		std::size_t                   m_held{ 0 };
	};

	// One line of a SHA256SUMS file, already split.
	struct SumEntry
	{
		std::string expected;  // lower-case hex
		std::string relative;  // path relative to the folder the sums file is in
	};

	struct VerifyResult
	{
		bool        ok{ false };
		int         checked{ 0 };
		std::string failedFile;   // the first file that did not match, or could not be read
		std::string failedReason; // "missing", "unreadable" or the hash we actually got
	};

	// Reads SHA256SUMS in a_folder and checks every file it names. A missing
	// sums file is NOT an error here - it answers ok with checked == 0, and the
	// caller decides whether that is allowed. That split exists because an
	// author building their own model mod should not be blocked from running it
	// before they have generated the sums, while a shipped mod sets
	// requireSums and turns the same absence into a refusal.
	VerifyResult VerifySums(const std::filesystem::path& a_folder);

	// Exposed because the child verifies nothing and the shim's Start needs to
	// say WHICH file is wrong, and because tools/weights.ps1 writes the same
	// format that this reads.
	std::vector<SumEntry> ReadSums(const std::filesystem::path& a_sumsFile);
}
