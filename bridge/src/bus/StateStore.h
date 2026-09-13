#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Envoy
{
	// Реестр состояния мира. Ключ описывает вопрос, а не способ ответа.
	//
	// Значение либо есть, либо спросить некому - и потребитель обязан различать
	// это. Скриптовые поставщики публикуют значения при изменении, потому что
	// синхронно спросить Papyrus из C++ нельзя.
	class StateStore
	{
	public:
		enum Status : std::int32_t
		{
			kNoProvider = 0,
			kOk         = 1,
			kFailed     = 2
		};

		struct Entry
		{
			std::string                           type;
			std::string                           description;
			double                                ttlSec{ 0.0 };
			bool                                  hasValue{ false };
			bool                                  boolean{ false };
			std::int32_t                          integer{ 0 };
			float                                 number{ 0.0f };
			std::string                           text;
			std::uint32_t                         formId{ 0 };
			std::chrono::steady_clock::time_point stamp{ std::chrono::steady_clock::now() };
		};

		static StateStore& Get();

		// Пространство core зарезервировано за ядром: чужие писать в него не могут.
		bool Declare(const std::string& a_key, std::string a_type, double a_ttlSec, std::string a_description);
		void Retract(const std::string& a_key);

		bool PublishBool(const std::string& a_key, bool a_value);
		bool PublishInt(const std::string& a_key, std::int32_t a_value);
		bool PublishFloat(const std::string& a_key, float a_value);
		bool PublishString(const std::string& a_key, std::string a_value);
		bool PublishForm(const std::string& a_key, std::uint32_t a_formId);

		std::int32_t Status(const std::string& a_key) const;
		float        Age(const std::string& a_key) const;
		Entry        Value(const std::string& a_key) const;

		std::vector<std::string> Keys() const;

	private:
		StateStore() = default;

		Entry* Touch(const std::string& a_key);

		mutable std::mutex                     _mutex;
		std::unordered_map<std::string, Entry> _entries;
	};
}
