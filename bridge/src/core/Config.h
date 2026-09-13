#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace Envoy
{
	// The single place settings are read.
	//
	// Values are not scattered through the code: the whole reference set is
	// compiled in from config/envoy.default.json. On the first launch it becomes a
	// file next to the plugin; on later ones it serves as the base the user's file
	// is laid over. That is why new keys appear at the user's end by themselves
	// after an update, and why a damaged file neither brings the plugin down nor
	// gets overwritten.
	class Config
	{
	public:
		enum class Origin
		{
			Baseline,   // no file, and it could not be created - running on the built-in set
			Created,    // no file, so one was created from the built-in set
			Merged,     // file read and topped up with the keys it was missing
			File,       // file read, nothing to top up
			Broken      // file is there but does not parse - built-in set, file left alone
		};

		static Config& Get();

		bool Load(const std::filesystem::path& a_path);

		Origin                       Source() const { return _origin; }
		const std::filesystem::path& Path() const { return _path; }
		const std::string&           Error() const { return _error; }
		const nlohmann::json&        Raw() const { return _doc; }

		// Access by JSON pointer, for instance "/hub/port".
		template <class T>
		std::optional<T> Value(std::string_view a_pointer) const
		{
			try {
				const auto ptr = nlohmann::json::json_pointer{ std::string{ a_pointer } };
				return _doc.at(ptr).get<T>();
			} catch (const std::exception&) {
				return std::nullopt;
			}
		}

		static std::string_view Describe(Origin a_origin);

	private:
		Config() = default;

		static void MergeInto(nlohmann::json& a_base, const nlohmann::json& a_over);
		bool        Write() const;

		nlohmann::json        _doc;
		std::filesystem::path _path;
		std::string           _error;
		Origin                _origin{ Origin::Baseline };
	};
}
