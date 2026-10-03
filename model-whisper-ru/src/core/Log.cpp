#include "Log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace WhisperRu::Log
{
	namespace
	{
		std::mutex                                   g_lock;
		std::unordered_map<std::string, std::string> g_table;
		std::ofstream                                g_file;
		std::int32_t                                 g_level{ kInfo };

		std::string Stamp()
		{
			const auto now = std::chrono::system_clock::now();
			const auto seconds = std::chrono::system_clock::to_time_t(now);
			const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
				now.time_since_epoch()) % 1000;
			std::tm parts{};
			localtime_s(&parts, &seconds);
			char text[32]{};
			std::snprintf(text, sizeof(text), "%02d:%02d:%02d.%03d",
				parts.tm_hour, parts.tm_min, parts.tm_sec, static_cast<int>(millis.count()));
			return text;
		}

		const char* LevelName(std::int32_t a_level)
		{
			switch (a_level) {
			case kDebug: return "debug";
			case kWarn:  return "warn";
			case kError: return "error";
			default:     return "info";
			}
		}
	}

	void LoadTable(const std::filesystem::path& a_folder, const std::string& a_name,
		const std::string& a_language)
	{
		std::unordered_map<std::string, std::string> table;
		const auto path = a_folder / (a_name + "." + a_language + ".txt");
		std::ifstream file(path, std::ios::binary);
		if (file) {
			std::string line;
			while (std::getline(file, line)) {
				if (!line.empty() && line.back() == '\r') {
					line.pop_back();
				}
				// A UTF-8 byte order mark on the first line would otherwise
				// become part of the first key, and the first key is the one
				// nobody notices is broken.
				if (table.empty() && line.size() >= 3 &&
					static_cast<unsigned char>(line[0]) == 0xEF &&
					static_cast<unsigned char>(line[1]) == 0xBB &&
					static_cast<unsigned char>(line[2]) == 0xBF) {
					line.erase(0, 3);
				}
				if (line.empty() || line[0] == '#') {
					continue;
				}
				const auto tab = line.find('\t');
				if (tab == std::string::npos) {
					continue;
				}
				table.emplace(line.substr(0, tab), line.substr(tab + 1));
			}
		}

		std::lock_guard guard(g_lock);
		g_table = std::move(table);
	}

	void OpenFile(const std::filesystem::path& a_path)
	{
		std::error_code ec;
		std::filesystem::create_directories(a_path.parent_path(), ec);
		std::lock_guard guard(g_lock);
		g_file.open(a_path, std::ios::binary | std::ios::trunc);
	}

	void SetLevel(const std::string& a_level)
	{
		std::int32_t level = kInfo;
		if (a_level == "debug") {
			level = kDebug;
		} else if (a_level == "warn") {
			level = kWarn;
		} else if (a_level == "error") {
			level = kError;
		}
		std::lock_guard guard(g_lock);
		g_level = level;
	}

	const std::string& Text(const std::string& a_key)
	{
		std::lock_guard guard(g_lock);
		const auto found = g_table.find(a_key);
		if (found != g_table.end()) {
			return found->second;
		}
		// The key itself, and it is returned by reference out of the table so
		// that the caller may hold it: inserting it keeps that reference alive
		// for the life of the process and makes the second lookup a hit.
		return g_table.emplace(a_key, a_key).first->second;
	}

	std::string Render(const std::string& a_key, const std::vector<std::string>& a_args)
	{
		const std::string pattern = Text(a_key);
		std::string out;
		out.reserve(pattern.size() + 32);
		for (std::size_t at = 0; at < pattern.size(); ++at) {
			if (pattern[at] != '{') {
				out.push_back(pattern[at]);
				continue;
			}
			const auto close = pattern.find('}', at);
			if (close == std::string::npos) {
				out.push_back(pattern[at]);
				continue;
			}
			const auto digits = pattern.substr(at + 1, close - at - 1);
			// Three digits is more indices than any line here will ever have,
			// and the bound is what keeps the conversion below from having to
			// throw on "{99999999999999999999}".
			bool numeric = !digits.empty() && digits.size() <= 3;
			for (const char c : digits) {
				numeric = numeric && c >= '0' && c <= '9';
			}
			if (!numeric) {
				out.push_back(pattern[at]);
				continue;
			}
			const auto index = static_cast<std::size_t>(std::stoul(digits));
			if (index < a_args.size()) {
				out.append(a_args[index]);
			} else {
				// No argument for it. The placeholder stays, visibly wrong, and
				// the rest of the line still reads.
				out.append(pattern, at, close - at + 1);
			}
			at = close;
		}
		return out;
	}

	void Say(std::int32_t a_level, const std::string& a_key, const std::vector<std::string>& a_args)
	{
		{
			std::lock_guard guard(g_lock);
			if (a_level < g_level || !g_file.is_open()) {
				return;
			}
		}
		const auto line = Render(a_key, a_args);
		std::lock_guard guard(g_lock);
		if (!g_file.is_open()) {
			return;
		}
		g_file << Stamp() << " [" << LevelName(a_level) << "] " << line << '\n';
		// Flushed on every line on purpose. What this log is for is the run that
		// ended with the game gone and no crash report - the very case the child
		// process exists for - and a buffered last line is the one that would
		// have said why.
		g_file.flush();
	}

	std::string ToText(const std::string& a_value) { return a_value; }
	std::string ToText(const char* a_value) { return a_value ? a_value : ""; }
	std::string ToText(const std::filesystem::path& a_value) { return a_value.string(); }
	std::string ToText(std::int32_t a_value) { return std::to_string(a_value); }
	std::string ToText(std::uint32_t a_value) { return std::to_string(a_value); }
	std::string ToText(std::int64_t a_value) { return std::to_string(a_value); }
	std::string ToText(bool a_value) { return a_value ? "1" : "0"; }

	std::string ToText(double a_value)
	{
		std::ostringstream out;
		out.precision(3);
		out << std::fixed << a_value;
		return out.str();
	}
}
