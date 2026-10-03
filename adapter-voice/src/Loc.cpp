#include "Loc.h"

#include "LocStrings.h"

namespace Voice
{
	namespace
	{
		SpeechBrokerLoc::Table g_table;
	}

	void Loc::Load(const std::filesystem::path& a_dir, const std::string& a_language)
	{
		g_table.Load(a_dir, a_language, kSpeechBrokerDefaultStrings, std::size(kSpeechBrokerDefaultStrings));

		// In English on purpose, and the only line of the adapter that is: it reports
		// whether the translation loaded, so it cannot depend on the translation
		// having loaded.
		spdlog::info("localisation: {}, {} strings from {} files in {}", g_table.Language(),
			g_table.Count(), g_table.Files(), a_dir.string());
	}

	const char* Loc::Get(const char* a_key)
	{
		return g_table.Get(a_key);
	}

	const std::string& Loc::Language()
	{
		return g_table.Language();
	}
}
