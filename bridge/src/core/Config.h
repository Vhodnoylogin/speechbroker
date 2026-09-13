#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace Envoy
{
	// Единственная точка чтения настроек.
	//
	// Значения не задаются в коде россыпью: эталонный набор целиком вкомпилирован
	// из config/envoy.default.json. При первом запуске он превращается в файл рядом
	// с плагином, при следующих - служит основой, поверх которой ложится файл
	// пользователя. Поэтому новые ключи после обновления появляются у пользователя
	// сами, а испорченный файл не роняет плагин и не затирается.
	class Config
	{
	public:
		enum class Origin
		{
			Baseline,   // файла не было и создать не удалось - работаем на встроенном
			Created,    // файла не было, создали из встроенного
			Merged,     // файл прочитан и дополнен недостающими ключами
			File,       // файл прочитан, дополнять нечего
			Broken      // файл есть, но не разбирается - работаем на встроенном, файл не тронут
		};

		static Config& Get();

		bool Load(const std::filesystem::path& a_path);

		Origin                       Source() const { return _origin; }
		const std::filesystem::path& Path() const { return _path; }
		const std::string&           Error() const { return _error; }
		const nlohmann::json&        Raw() const { return _doc; }

		// Доступ по указателю JSON, например "/hub/port".
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
