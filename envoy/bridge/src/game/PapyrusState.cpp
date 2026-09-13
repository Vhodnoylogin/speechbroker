#include "PapyrusApi.h"

#include "bus/StateStore.h"
#include "core/Log.h"

#include <SKSE/SKSE.h>

// The snapshot of the world and the publishing of keys: everything a subscriber
// asks about the state of the game, and everything it can itself become a
// provider of. The only party this file talks to is StateStore.
namespace Envoy
{
	std::int32_t PapyrusApi::GetStateStatus(Tag, std::int32_t, Str a_key)
	{
		return StateStore::Get().Status(a_key.c_str());
	}

	float PapyrusApi::GetStateAge(Tag, std::int32_t, Str a_key)
	{
		return StateStore::Get().Age(a_key.c_str());
	}

	bool PapyrusApi::GetStateBool(Tag, std::int32_t, Str a_key, bool a_default)
	{
		auto entry = StateStore::Get().Value(a_key.c_str());
		return entry.hasValue ? entry.boolean : a_default;
	}

	std::int32_t PapyrusApi::GetStateInt(Tag, std::int32_t, Str a_key, std::int32_t a_default)
	{
		auto entry = StateStore::Get().Value(a_key.c_str());
		return entry.hasValue ? entry.integer : a_default;
	}

	float PapyrusApi::GetStateFloat(Tag, std::int32_t, Str a_key, float a_default)
	{
		auto entry = StateStore::Get().Value(a_key.c_str());
		return entry.hasValue ? entry.number : a_default;
	}

	RE::BSFixedString PapyrusApi::GetStateString(Tag, std::int32_t, Str a_key, Str a_default)
	{
		auto entry = StateStore::Get().Value(a_key.c_str());
		return entry.hasValue ? RE::BSFixedString{ entry.text } : a_default;
	}

	RE::TESForm* PapyrusApi::GetStateForm(Tag, std::int32_t, Str a_key)
	{
		auto entry = StateStore::Get().Value(a_key.c_str());
		return entry.hasValue ? RE::TESForm::LookupByID(entry.formId) : nullptr;
	}

	std::vector<RE::BSFixedString> PapyrusApi::GetKeys(Tag)
	{
		std::vector<RE::BSFixedString> out;
		for (const auto& key : StateStore::Get().Keys()) {
			out.emplace_back(key);
		}
		return out;
	}

	void PapyrusApi::DeclareKey(Tag, Str a_key, Str a_type, float a_ttlSec, Str a_description)
	{
		if (!StateStore::Get().Declare(a_key.c_str(), a_type.c_str(), a_ttlSec, a_description.c_str())) {
			Log::Warn("$ENVOY_LOG_KEY_REFUSED", a_key.c_str());
		}
	}

	void PapyrusApi::RetractKey(Tag, Str a_key) { StateStore::Get().Retract(a_key.c_str()); }

	void PapyrusApi::PublishBool(Tag, Str a_key, bool a_value)
	{
		StateStore::Get().PublishBool(a_key.c_str(), a_value);
	}

	void PapyrusApi::PublishInt(Tag, Str a_key, std::int32_t a_value)
	{
		StateStore::Get().PublishInt(a_key.c_str(), a_value);
	}

	void PapyrusApi::PublishFloat(Tag, Str a_key, float a_value)
	{
		StateStore::Get().PublishFloat(a_key.c_str(), a_value);
	}

	void PapyrusApi::PublishString(Tag, Str a_key, Str a_value)
	{
		StateStore::Get().PublishString(a_key.c_str(), a_value.c_str());
	}

	void PapyrusApi::PublishForm(Tag, Str a_key, RE::TESForm* a_value)
	{
		StateStore::Get().PublishForm(a_key.c_str(), a_value ? a_value->GetFormID() : 0);
	}
}
