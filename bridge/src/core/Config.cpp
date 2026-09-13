#include "Config.h"

#include "EnvoyDefaults.h"

#include <fstream>

namespace Envoy
{
	Config& Config::Get()
	{
		static Config instance;
		return instance;
	}

	std::string_view Config::Describe(Origin a_origin)
	{
		switch (a_origin) {
		case Origin::Created: return "файл настроек создан из встроенного набора";
		case Origin::Merged:  return "файл настроек дополнен новыми ключами";
		case Origin::File:    return "файл настроек прочитан";
		case Origin::Broken:  return "файл настроек не разбирается, работаю на встроенных значениях";
		default:              return "работаю на встроенных значениях";
		}
	}

	void Config::MergeInto(nlohmann::json& a_base, const nlohmann::json& a_over)
	{
		if (!a_base.is_object() || !a_over.is_object()) {
			a_base = a_over;
			return;
		}

		for (auto it = a_over.begin(); it != a_over.end(); ++it) {
			auto found = a_base.find(it.key());
			if (found != a_base.end() && found->is_object() && it->is_object()) {
				MergeInto(*found, *it);
			} else {
				a_base[it.key()] = *it;
			}
		}
	}

	bool Config::Write() const
	{
		std::error_code ec;
		std::filesystem::create_directories(_path.parent_path(), ec);

		std::ofstream stream(_path, std::ios::binary | std::ios::trunc);
		if (!stream) {
			return false;
		}

		stream << _doc.dump(2) << '\n';
		return stream.good();
	}

	bool Config::Load(const std::filesystem::path& a_path)
	{
		_path = a_path;
		_error.clear();

		// Встроенный набор - всегда основа. Даже если дальше всё пойдёт не так,
		// плагин остаётся работоспособным.
		try {
			_doc = nlohmann::json::parse(kDefaultConfigJson);
		} catch (const std::exception& e) {
			_error = e.what();
			_origin = Origin::Baseline;
			return false;
		}

		std::error_code ec;
		if (!std::filesystem::exists(a_path, ec)) {
			_origin = Write() ? Origin::Created : Origin::Baseline;
			return true;
		}

		nlohmann::json fromFile;
		try {
			std::ifstream stream(a_path);
			stream >> fromFile;
		} catch (const std::exception& e) {
			// Файл трогать нельзя: там могут быть правки пользователя.
			_error = e.what();
			_origin = Origin::Broken;
			return true;
		}

		MergeInto(_doc, fromFile);

		if (_doc == fromFile) {
			_origin = Origin::File;
		} else {
			_origin = Write() ? Origin::Merged : Origin::File;
		}

		return true;
	}
}
