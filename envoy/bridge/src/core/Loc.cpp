#include "Loc.h"

#include "LocStrings.h"
#include "envoy-loc.h"

#include <spdlog/spdlog.h>

namespace Envoy
{
	namespace
	{
		EnvoyLoc::Table g_table;
	}

	void Loc::Load(const std::filesystem::path& a_dir, const std::string& a_language)
	{
		g_table.Load(a_dir, a_language, kEnvoyDefaultStrings, std::size(kEnvoyDefaultStrings));

		// Said in English on purpose, and it is the only line in the bridge that is.
		// It reports whether the translation loaded at all, so it cannot itself
		// depend on the translation having loaded.
		spdlog::info("localisation: {}, {} strings from {} files in {}", g_table.Language(),
			g_table.Count(), g_table.Files(), a_dir.string());
		if (g_table.Files() == 0) {
			spdlog::warn("localisation: no Envoy*_{}.txt found, running on the built-in English",
				g_table.Language());
		}
	}

	const char* Loc::Get(const char* a_key)
	{
		return g_table.Get(a_key);
	}

	const std::string& Loc::Language()
	{
		return g_table.Language();
	}

	std::size_t Loc::Count()
	{
		return g_table.Count();
	}

	std::size_t Loc::Files()
	{
		return g_table.Files();
	}
}
