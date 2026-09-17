#include "Wire.h"

#include <cstring>

namespace WhisperRu::Wire
{
	namespace
	{
		// Little-endian by hand rather than by memcpy of the native layout. The
		// two are the same on x64 and will stay the same, but a protocol whose
		// byte order is "whatever the compiler did" has no written form at all,
		// and docs/child-protocol.md has to name the bytes.
		void PutU32(std::vector<std::uint8_t>& a_out, std::uint32_t a_value)
		{
			a_out.push_back(static_cast<std::uint8_t>(a_value & 0xFFu));
			a_out.push_back(static_cast<std::uint8_t>((a_value >> 8) & 0xFFu));
			a_out.push_back(static_cast<std::uint8_t>((a_value >> 16) & 0xFFu));
			a_out.push_back(static_cast<std::uint8_t>((a_value >> 24) & 0xFFu));
		}

		void PutU16(std::vector<std::uint8_t>& a_out, std::uint16_t a_value)
		{
			a_out.push_back(static_cast<std::uint8_t>(a_value & 0xFFu));
			a_out.push_back(static_cast<std::uint8_t>((a_value >> 8) & 0xFFu));
		}

		void PutI32(std::vector<std::uint8_t>& a_out, std::int32_t a_value)
		{
			PutU32(a_out, static_cast<std::uint32_t>(a_value));
		}

		void PutU64(std::vector<std::uint8_t>& a_out, std::uint64_t a_value)
		{
			PutU32(a_out, static_cast<std::uint32_t>(a_value & 0xFFFFFFFFu));
			PutU32(a_out, static_cast<std::uint32_t>((a_value >> 32) & 0xFFFFFFFFu));
		}

		void PutI64(std::vector<std::uint8_t>& a_out, std::int64_t a_value)
		{
			PutU64(a_out, static_cast<std::uint64_t>(a_value));
		}

		void PutF32(std::vector<std::uint8_t>& a_out, float a_value)
		{
			std::uint32_t bits = 0;
			std::memcpy(&bits, &a_value, sizeof(bits));
			PutU32(a_out, bits);
		}

		// A string is a byte count and then the bytes, never a terminator. The
		// contract's ceiling is applied HERE, on the way out, so that a string
		// which crossed this wire needs no second measuring before it is handed
		// to the adapter. Longer is truncated rather than refused, exactly as
		// the contract does with its own strings: losing text beats losing the
		// answer that carried it.
		void PutString(std::vector<std::uint8_t>& a_out, const std::string& a_value)
		{
			auto bytes = a_value.size();
			if (bytes > kMaxStringBytes - 1) {
				bytes = kMaxStringBytes - 1;
			}
			PutU32(a_out, static_cast<std::uint32_t>(bytes));
			a_out.insert(a_out.end(), a_value.begin(), a_value.begin() + static_cast<std::ptrdiff_t>(bytes));
		}

		// The reader is a cursor over a body that may have been written by
		// somebody else's child. Every read is bounds-checked and the first
		// failure poisons the cursor, so a decoder can read its whole body and
		// ask once at the end whether any of it was real.
		class Cursor
		{
		public:
			Cursor(const std::uint8_t* a_bytes, std::size_t a_count) :
				m_bytes(a_bytes), m_count(a_count) {}

			bool Ok() const { return m_ok; }

			std::uint32_t U32()
			{
				if (!Take(4)) {
					return 0;
				}
				const auto* at = m_bytes + m_at - 4;
				return static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
				       (static_cast<std::uint32_t>(at[2]) << 16) | (static_cast<std::uint32_t>(at[3]) << 24);
			}

			std::uint16_t U16()
			{
				if (!Take(2)) {
					return 0;
				}
				const auto* at = m_bytes + m_at - 2;
				return static_cast<std::uint16_t>(static_cast<std::uint16_t>(at[0]) |
					static_cast<std::uint16_t>(static_cast<std::uint16_t>(at[1]) << 8));
			}

			std::int32_t I32() { return static_cast<std::int32_t>(U32()); }

			std::int64_t I64()
			{
				const std::uint64_t low = U32();
				const std::uint64_t high = U32();
				return static_cast<std::int64_t>(low | (high << 32));
			}

			float F32()
			{
				const std::uint32_t bits = U32();
				float value = 0.0f;
				std::memcpy(&value, &bits, sizeof(value));
				return value;
			}

			std::string String()
			{
				const std::uint32_t bytes = U32();
				if (!m_ok || bytes >= kMaxStringBytes || !Take(bytes)) {
					m_ok = false;
					return {};
				}
				return std::string(reinterpret_cast<const char*>(m_bytes + m_at - bytes), bytes);
			}

			// The samples are the one place a copy is worth avoiding by hand:
			// 320 000 floats on a final pass.
			bool Floats(std::uint32_t a_count, std::vector<float>& a_out)
			{
				const std::size_t bytes = static_cast<std::size_t>(a_count) * sizeof(float);
				if (!Take(bytes)) {
					return false;
				}
				a_out.resize(a_count);
				if (a_count != 0) {
					std::memcpy(a_out.data(), m_bytes + m_at - bytes, bytes);
				}
				return true;
			}

		private:
			bool Take(std::size_t a_bytes)
			{
				if (!m_ok || m_count - m_at < a_bytes) {
					m_ok = false;
					return false;
				}
				m_at += a_bytes;
				return true;
			}

			const std::uint8_t* m_bytes{ nullptr };
			std::size_t         m_count{ 0 };
			std::size_t         m_at{ 0 };
			bool                m_ok{ true };
		};

		std::vector<std::uint8_t> Frame(Type a_type, std::vector<std::uint8_t>&& a_body)
		{
			std::vector<std::uint8_t> out;
			out.reserve(kHeaderBytes + a_body.size());
			PutU32(out, kMagic);
			PutU16(out, kVersion);
			PutU16(out, static_cast<std::uint16_t>(a_type));
			PutU32(out, static_cast<std::uint32_t>(a_body.size()));
			PutU32(out, 0);
			out.insert(out.end(), a_body.begin(), a_body.end());
			return out;
		}
	}

	std::vector<std::uint8_t> EncodeHello(const Hello& a_hello)
	{
		std::vector<std::uint8_t> body;
		PutU32(body, a_hello.protocolVersion);
		PutU32(body, a_hello.maxSamples);
		PutString(body, a_hello.backend);
		PutString(body, a_hello.modelId);
		return Frame(Type::Hello, std::move(body));
	}

	std::vector<std::uint8_t> EncodeRequest(const Request& a_request)
	{
		std::vector<std::uint8_t> body;
		body.reserve(32 + a_request.samples.size() * sizeof(float));
		PutI64(body, a_request.utteranceId);
		PutI64(body, a_request.turnId);
		PutI32(body, a_request.serial);
		PutI32(body, a_request.final);
		PutU32(body, a_request.lostSamples);
		PutU32(body, a_request.deadlineMs);
		PutU32(body, static_cast<std::uint32_t>(a_request.samples.size()));
		const auto at = body.size();
		body.resize(at + a_request.samples.size() * sizeof(float));
		if (!a_request.samples.empty()) {
			std::memcpy(body.data() + at, a_request.samples.data(), a_request.samples.size() * sizeof(float));
		}
		return Frame(Type::Request, std::move(body));
	}

	std::vector<std::uint8_t> EncodeReply(const Reply& a_reply)
	{
		std::vector<std::uint8_t> body;
		PutI64(body, a_reply.utteranceId);
		PutI32(body, a_reply.serial);
		PutI32(body, a_reply.status);
		PutI32(body, a_reply.latencyMs);
		PutU32(body, a_reply.lostSamples);
		PutI32(body, static_cast<std::int32_t>(a_reply.fragments.size()));
		for (const auto& fragment : a_reply.fragments) {
			PutI32(body, fragment.startMs);
			PutI32(body, fragment.endMs);
			PutF32(body, fragment.score);
			PutI32(body, fragment.endsSentence);
			PutF32(body, fragment.lastWordProb);
			PutF32(body, fragment.noSpeechProb);
			PutI32(body, fragment.medianGapMs);
			PutI32(body, fragment.words);
			PutString(body, fragment.text);
		}
		PutString(body, a_reply.failed);
		return Frame(Type::Reply, std::move(body));
	}

	std::vector<std::uint8_t> EncodeVocabulary(const std::vector<std::string>& a_phrases)
	{
		std::vector<std::uint8_t> body;
		PutI32(body, static_cast<std::int32_t>(a_phrases.size()));
		for (const auto& phrase : a_phrases) {
			PutString(body, phrase);
		}
		return Frame(Type::Vocabulary, std::move(body));
	}

	std::vector<std::uint8_t> EncodeCancel(std::int64_t a_utteranceId)
	{
		std::vector<std::uint8_t> body;
		PutI64(body, a_utteranceId);
		return Frame(Type::Cancel, std::move(body));
	}

	std::vector<std::uint8_t> EncodeLog(const LogLine& a_line)
	{
		std::vector<std::uint8_t> body;
		PutI32(body, a_line.level);
		PutString(body, a_line.key);
		PutI32(body, static_cast<std::int32_t>(a_line.args.size()));
		for (const auto& arg : a_line.args) {
			PutString(body, arg);
		}
		return Frame(Type::Log, std::move(body));
	}

	std::vector<std::uint8_t> EncodeBye()
	{
		return Frame(Type::Bye, {});
	}

	bool DecodeHeader(const std::uint8_t* a_bytes, std::size_t a_count, Header& a_out)
	{
		if (!a_bytes || a_count < kHeaderBytes) {
			return false;
		}
		Cursor cursor(a_bytes, a_count);
		a_out.magic = cursor.U32();
		a_out.version = cursor.U16();
		a_out.type = cursor.U16();
		a_out.bodyBytes = cursor.U32();
		a_out.reserved = cursor.U32();
		if (!cursor.Ok() || a_out.magic != kMagic) {
			return false;
		}
		// A version ABOVE ours is refused rather than read through: this is the
		// small end of the same argument the contract settles at length. The
		// shim and its child ship in one archive, so there is no compatibility
		// to keep here and a mismatch means somebody put the wrong exe in.
		return a_out.version == kVersion && a_out.bodyBytes <= kMaxBodyBytes;
	}

	bool DecodeHello(const std::uint8_t* a_body, std::size_t a_count, Hello& a_out)
	{
		Cursor cursor(a_body, a_count);
		a_out.protocolVersion = cursor.U32();
		a_out.maxSamples = cursor.U32();
		a_out.backend = cursor.String();
		a_out.modelId = cursor.String();
		return cursor.Ok();
	}

	bool DecodeRequest(const std::uint8_t* a_body, std::size_t a_count, Request& a_out)
	{
		Cursor cursor(a_body, a_count);
		a_out.utteranceId = cursor.I64();
		a_out.turnId = cursor.I64();
		a_out.serial = cursor.I32();
		a_out.final = cursor.I32();
		a_out.lostSamples = cursor.U32();
		a_out.deadlineMs = cursor.U32();
		const std::uint32_t samples = cursor.U32();
		if (!cursor.Ok()) {
			return false;
		}
		// The count is checked against what is actually in the body before a
		// byte of it is trusted: a desynchronised stream otherwise asks for an
		// allocation of four gigabytes and the child dies of it.
		if (static_cast<std::size_t>(samples) * sizeof(float) > a_count) {
			return false;
		}
		return cursor.Floats(samples, a_out.samples) && cursor.Ok();
	}

	bool DecodeReply(const std::uint8_t* a_body, std::size_t a_count, Reply& a_out)
	{
		Cursor cursor(a_body, a_count);
		a_out.utteranceId = cursor.I64();
		a_out.serial = cursor.I32();
		a_out.status = cursor.I32();
		a_out.latencyMs = cursor.I32();
		a_out.lostSamples = cursor.U32();
		const std::int32_t count = cursor.I32();
		if (!cursor.Ok() || count < 0 || count > kMaxFragments) {
			return false;
		}
		a_out.fragments.clear();
		a_out.fragments.reserve(static_cast<std::size_t>(count));
		for (std::int32_t i = 0; i < count; ++i) {
			Fragment fragment;
			fragment.startMs = cursor.I32();
			fragment.endMs = cursor.I32();
			fragment.score = cursor.F32();
			fragment.endsSentence = cursor.I32();
			fragment.lastWordProb = cursor.F32();
			fragment.noSpeechProb = cursor.F32();
			fragment.medianGapMs = cursor.I32();
			fragment.words = cursor.I32();
			fragment.text = cursor.String();
			if (!cursor.Ok()) {
				return false;
			}
			a_out.fragments.push_back(std::move(fragment));
		}
		a_out.failed = cursor.String();
		return cursor.Ok();
	}

	bool DecodeVocabulary(const std::uint8_t* a_body, std::size_t a_count, std::vector<std::string>& a_out)
	{
		Cursor cursor(a_body, a_count);
		const std::int32_t count = cursor.I32();
		if (!cursor.Ok() || count < 0) {
			return false;
		}
		a_out.clear();
		for (std::int32_t i = 0; i < count; ++i) {
			a_out.push_back(cursor.String());
			if (!cursor.Ok()) {
				return false;
			}
		}
		return true;
	}

	bool DecodeCancel(const std::uint8_t* a_body, std::size_t a_count, std::int64_t& a_out)
	{
		Cursor cursor(a_body, a_count);
		a_out = cursor.I64();
		return cursor.Ok();
	}

	bool DecodeLog(const std::uint8_t* a_body, std::size_t a_count, LogLine& a_out)
	{
		Cursor cursor(a_body, a_count);
		a_out.level = cursor.I32();
		a_out.key = cursor.String();
		const std::int32_t count = cursor.I32();
		// The adapter drops a log call whose argument count is out of range, so
		// the ceiling is applied here where the line can still be salvaged: an
		// over-long argument list is truncated, not thrown away.
		if (!cursor.Ok() || count < 0 || count > 16) {
			return false;
		}
		a_out.args.clear();
		for (std::int32_t i = 0; i < count; ++i) {
			a_out.args.push_back(cursor.String());
			if (!cursor.Ok()) {
				return false;
			}
		}
		return true;
	}
}
