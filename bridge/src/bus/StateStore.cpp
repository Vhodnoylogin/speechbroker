#include "StateStore.h"

#include <algorithm>

namespace Envoy
{
	namespace
	{
		bool Reserved(const std::string& a_key)
		{
			return a_key.rfind("core.", 0) == 0;
		}
	}

	StateStore& StateStore::Get()
	{
		static StateStore instance;
		return instance;
	}

	bool StateStore::Declare(const std::string& a_key, std::string a_type, double a_ttlSec,
		std::string a_description)
	{
		if (a_key.empty() || Reserved(a_key)) {
			return false;
		}

		std::scoped_lock lock(_mutex);
		auto& entry = _entries[a_key];
		entry.type = std::move(a_type);
		entry.ttlSec = a_ttlSec;
		entry.description = std::move(a_description);
		return true;
	}

	void StateStore::Retract(const std::string& a_key)
	{
		std::scoped_lock lock(_mutex);
		_entries.erase(a_key);
	}

	StateStore::Entry* StateStore::Touch(const std::string& a_key)
	{
		// Незаявленный ключ не принимается: иначе пространство имён превратится
		// в свалку, и никто не сможет узнать, что вообще доступно.
		auto it = _entries.find(a_key);
		if (it == _entries.end()) {
			return nullptr;
		}
		it->second.hasValue = true;
		it->second.stamp = std::chrono::steady_clock::now();
		return &it->second;
	}

	bool StateStore::PublishBool(const std::string& a_key, bool a_value)
	{
		std::scoped_lock lock(_mutex);
		auto* entry = Touch(a_key);
		if (!entry) {
			return false;
		}
		entry->boolean = a_value;
		return true;
	}

	bool StateStore::PublishInt(const std::string& a_key, std::int32_t a_value)
	{
		std::scoped_lock lock(_mutex);
		auto* entry = Touch(a_key);
		if (!entry) {
			return false;
		}
		entry->integer = a_value;
		return true;
	}

	bool StateStore::PublishFloat(const std::string& a_key, float a_value)
	{
		std::scoped_lock lock(_mutex);
		auto* entry = Touch(a_key);
		if (!entry) {
			return false;
		}
		entry->number = a_value;
		return true;
	}

	bool StateStore::PublishString(const std::string& a_key, std::string a_value)
	{
		std::scoped_lock lock(_mutex);
		auto* entry = Touch(a_key);
		if (!entry) {
			return false;
		}
		entry->text = std::move(a_value);
		return true;
	}

	bool StateStore::PublishForm(const std::string& a_key, std::uint32_t a_formId)
	{
		std::scoped_lock lock(_mutex);
		auto* entry = Touch(a_key);
		if (!entry) {
			return false;
		}
		entry->formId = a_formId;
		return true;
	}

	std::int32_t StateStore::Status(const std::string& a_key) const
	{
		std::scoped_lock lock(_mutex);
		auto it = _entries.find(a_key);
		if (it == _entries.end() || !it->second.hasValue) {
			return kNoProvider;
		}
		return kOk;
	}

	float StateStore::Age(const std::string& a_key) const
	{
		std::scoped_lock lock(_mutex);
		auto it = _entries.find(a_key);
		if (it == _entries.end() || !it->second.hasValue) {
			return -1.0f;
		}
		const std::chrono::duration<float> age = std::chrono::steady_clock::now() - it->second.stamp;
		return age.count();
	}

	StateStore::Entry StateStore::Value(const std::string& a_key) const
	{
		std::scoped_lock lock(_mutex);
		auto it = _entries.find(a_key);
		return it == _entries.end() ? Entry{} : it->second;
	}

	std::vector<std::string> StateStore::Keys() const
	{
		std::scoped_lock lock(_mutex);
		std::vector<std::string> out;
		out.reserve(_entries.size());
		for (const auto& entry : _entries) {
			out.push_back(entry.first + " (" + entry.second.type + ") " + entry.second.description);
		}
		std::sort(out.begin(), out.end());
		return out;
	}
}
