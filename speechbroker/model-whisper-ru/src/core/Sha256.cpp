#include "Sha256.h"

#include <cstdio>
#include <cstring>

namespace WhisperRu
{
	namespace
	{
		constexpr std::uint32_t kK[64] = {
			0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
			0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
			0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
			0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
			0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
			0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
			0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
			0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
		};

		constexpr std::uint32_t Ror(std::uint32_t a_value, std::uint32_t a_bits)
		{
			return (a_value >> a_bits) | (a_value << (32 - a_bits));
		}

		std::string ToHex(const std::uint8_t* a_bytes, std::size_t a_count)
		{
			static constexpr char kDigits[] = "0123456789abcdef";
			std::string out;
			out.resize(a_count * 2);
			for (std::size_t i = 0; i < a_count; ++i) {
				out[i * 2] = kDigits[(a_bytes[i] >> 4) & 0x0F];
				out[i * 2 + 1] = kDigits[a_bytes[i] & 0x0F];
			}
			return out;
		}

		std::string Lower(std::string a_value)
		{
			for (auto& c : a_value) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c - 'A' + 'a');
				}
			}
			return a_value;
		}
	}

	Sha256::Sha256()
	{
		m_state = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
			        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
	}

	void Sha256::Block(const std::uint8_t* a_block)
	{
		std::uint32_t w[64]{};
		for (std::size_t i = 0; i < 16; ++i) {
			w[i] = (static_cast<std::uint32_t>(a_block[i * 4]) << 24) |
			       (static_cast<std::uint32_t>(a_block[i * 4 + 1]) << 16) |
			       (static_cast<std::uint32_t>(a_block[i * 4 + 2]) << 8) |
			       static_cast<std::uint32_t>(a_block[i * 4 + 3]);
		}
		for (std::size_t i = 16; i < 64; ++i) {
			const std::uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const std::uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}

		std::uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
		std::uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
		for (std::size_t i = 0; i < 64; ++i) {
			const std::uint32_t s1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
			const std::uint32_t ch = (e & f) ^ (~e & g);
			const std::uint32_t t1 = h + s1 + ch + kK[i] + w[i];
			const std::uint32_t s0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
			const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			const std::uint32_t t2 = s0 + maj;
			h = g; g = f; f = e; e = d + t1;
			d = c; c = b; b = a; a = t1 + t2;
		}
		m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
		m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
	}

	void Sha256::Update(const std::uint8_t* a_bytes, std::size_t a_count)
	{
		if (!a_bytes) {
			return;
		}
		m_bits += static_cast<std::uint64_t>(a_count) * 8;
		while (a_count != 0) {
			const std::size_t room = 64 - m_held;
			const std::size_t take = a_count < room ? a_count : room;
			std::memcpy(m_buffer.data() + m_held, a_bytes, take);
			m_held += take;
			a_bytes += take;
			a_count -= take;
			if (m_held == 64) {
				Block(m_buffer.data());
				m_held = 0;
			}
		}
	}

	std::string Sha256::Hex()
	{
		const std::uint64_t bits = m_bits;
		std::uint8_t one = 0x80;
		Update(&one, 1);
		m_bits = bits;  // the padding is not part of the message length
		std::uint8_t zero = 0;
		while (m_held != 56) {
			Update(&zero, 1);
			m_bits = bits;
		}
		std::uint8_t tail[8]{};
		for (std::size_t i = 0; i < 8; ++i) {
			tail[i] = static_cast<std::uint8_t>((bits >> (56 - i * 8)) & 0xFFu);
		}
		Update(tail, 8);

		std::uint8_t digest[32]{};
		for (std::size_t i = 0; i < 8; ++i) {
			digest[i * 4] = static_cast<std::uint8_t>((m_state[i] >> 24) & 0xFFu);
			digest[i * 4 + 1] = static_cast<std::uint8_t>((m_state[i] >> 16) & 0xFFu);
			digest[i * 4 + 2] = static_cast<std::uint8_t>((m_state[i] >> 8) & 0xFFu);
			digest[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i] & 0xFFu);
		}
		return ToHex(digest, sizeof(digest));
	}

	std::string Sha256::OfFile(const std::filesystem::path& a_path)
	{
		std::FILE* file = nullptr;
		if (_wfopen_s(&file, a_path.c_str(), L"rb") != 0 || !file) {
			return {};
		}
		Sha256 hash;
		// A megabyte at a time. Small enough not to matter next to the weights
		// themselves, large enough that a two-gigabyte file is two thousand
		// reads rather than half a million.
		std::vector<std::uint8_t> block(1024 * 1024);
		for (;;) {
			const std::size_t read = std::fread(block.data(), 1, block.size(), file);
			if (read != 0) {
				hash.Update(block.data(), read);
			}
			if (read != block.size()) {
				break;
			}
		}
		const bool bad = std::ferror(file) != 0;
		std::fclose(file);
		if (bad) {
			return {};
		}
		return hash.Hex();
	}

	std::vector<SumEntry> ReadSums(const std::filesystem::path& a_sumsFile)
	{
		std::vector<SumEntry> entries;
		std::FILE* file = nullptr;
		if (_wfopen_s(&file, a_sumsFile.c_str(), L"rb") != 0 || !file) {
			return entries;
		}
		std::string text;
		char block[4096];
		for (;;) {
			const std::size_t read = std::fread(block, 1, sizeof(block), file);
			text.append(block, read);
			if (read != sizeof(block)) {
				break;
			}
		}
		std::fclose(file);

		std::size_t at = 0;
		while (at < text.size()) {
			auto end = text.find('\n', at);
			if (end == std::string::npos) {
				end = text.size();
			}
			std::string line = text.substr(at, end - at);
			at = end + 1;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
				line.pop_back();
			}
			if (line.empty() || line[0] == '#') {
				continue;
			}
			// "<64 hex>  <path>" - sha256sum writes two spaces, or a space and a
			// star for its binary mode. Both are accepted, because a person will
			// paste in whatever their own tool produced.
			const auto space = line.find(' ');
			if (space != 64) {
				continue;
			}
			std::size_t pathAt = space;
			while (pathAt < line.size() && (line[pathAt] == ' ' || line[pathAt] == '*')) {
				++pathAt;
			}
			if (pathAt >= line.size()) {
				continue;
			}
			SumEntry entry;
			entry.expected = Lower(line.substr(0, 64));
			entry.relative = line.substr(pathAt);
			entries.push_back(std::move(entry));
		}
		return entries;
	}

	VerifyResult VerifySums(const std::filesystem::path& a_folder)
	{
		VerifyResult result;
		std::error_code ec;
		const auto sumsFile = a_folder / "SHA256SUMS";
		if (!std::filesystem::exists(sumsFile, ec)) {
			result.ok = true;  // absent is not wrong here; the caller decides
			return result;
		}

		const auto entries = ReadSums(sumsFile);
		for (const auto& entry : entries) {
			// A sums file is data that arrived with somebody else's download, so
			// it does not get to name a path outside the folder it sits in.
			if (entry.relative.find("..") != std::string::npos ||
				entry.relative.find(':') != std::string::npos ||
				entry.relative.front() == '/' || entry.relative.front() == '\\') {
				result.failedFile = entry.relative;
				result.failedReason = "outside";
				return result;
			}
			const auto path = a_folder / std::filesystem::path(entry.relative);
			if (!std::filesystem::exists(path, ec)) {
				result.failedFile = entry.relative;
				result.failedReason = "missing";
				return result;
			}
			const auto actual = Sha256::OfFile(path);
			if (actual.empty()) {
				result.failedFile = entry.relative;
				result.failedReason = "unreadable";
				return result;
			}
			if (actual != entry.expected) {
				result.failedFile = entry.relative;
				result.failedReason = actual;
				return result;
			}
			++result.checked;
		}
		result.ok = true;
		return result;
	}
}
