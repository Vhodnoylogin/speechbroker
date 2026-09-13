#include "AdapterHost.h"

#include "bus/Auction.h"
#include "bus/SubscriptionRegistry.h"
#include "bus/UtteranceStore.h"
#include "core/Config.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Settings.h"

#include <SKSE/SKSE.h>

#include <algorithm>
#include <cstring>

namespace Envoy
{
	namespace
	{
		std::vector<std::string> Split(const char* a_list)
		{
			std::vector<std::string> out;
			if (!a_list) {
				return out;
			}
			std::string current;
			for (const char* p = a_list; *p; ++p) {
				if (*p == ',') {
					if (!current.empty()) {
						out.push_back(current);
					}
					current.clear();
				} else if (*p != ' ') {
					current.push_back(*p);
				}
			}
			if (!current.empty()) {
				out.push_back(current);
			}
			return out;
		}

		std::string Safe(const char* a_text) { return a_text ? std::string{ a_text } : std::string{}; }
	}

	AdapterHost& AdapterHost::Get()
	{
		static AdapterHost instance;
		return instance;
	}

	void AdapterHost::Outgoing::Send() const
	{
		if (!onJob) {
			return;
		}

		std::vector<const char*> raw;
		raw.reserve(phrases.size());
		for (const auto& phrase : phrases) {
			raw.push_back(phrase.c_str());
		}

		EnvoyAPI::Job job{};
		job.kind = kind;
		job.active = active;
		job.text = text.c_str();
		job.service = service.c_str();
		job.payload = payload.c_str();
		job.speechId = speechId;
		job.requestId = requestId;
		job.phrases = raw.empty() ? nullptr : raw.data();
		job.phraseCount = static_cast<std::int32_t>(raw.size());
		onJob(job, user);
	}

	void AdapterHost::Dispatch(const std::vector<Outgoing>& a_jobs)
	{
		for (const auto& job : a_jobs) {
			job.Send();
		}
	}

	bool AdapterHost::Register(const EnvoyAPI::AdapterInfo& a_info, EnvoyAPI::JobCallback a_onJob, void* a_user)
	{
		if (!a_info.id || !a_onJob) {
			return false;
		}
		// The handshake: the version is checked once, here, and remembered. An adapter
		// older than us - we work by its version and do not read fields that were not
		// in it. Newer - refused: there is no telling what it will send.
		if (a_info.contract < 1 || a_info.contract > EnvoyAPI::kInterfaceVersion) {
			SKSE::log::error("adapter {}: contract version {}, the bridge understands 1 to {}",
				a_info.id, a_info.contract, EnvoyAPI::kInterfaceVersion);
			return false;
		}

		std::vector<Outgoing> pending;
		{
			std::scoped_lock lock(_mutex);
			auto& entry = _adapters[a_info.id];
			if (entry.order == 0) {
				entry.order = ++_order;
			}
			entry.name = Safe(a_info.name);
			entry.provides = Split(a_info.provides);
			entry.onJob = a_onJob;
			entry.user = a_user;
			entry.contract = a_info.contract;
			pending = RecomputeSources();
		}
		Dispatch(pending);

		SKSE::log::info("adapter registered: {} ({}), contract {}",
			a_info.id, Safe(a_info.name), a_info.contract);

		const auto phrases = SubscriptionRegistry::Get().MergedVocabulary();
		SendVocabulary(phrases);
		return true;
	}

	void AdapterHost::Unregister(const char* a_id)
	{
		if (!a_id) {
			return;
		}
		std::vector<Outgoing> pending;
		{
			std::scoped_lock lock(_mutex);
			_adapters.erase(a_id);
			pending = RecomputeSources();
		}
		Dispatch(pending);
		SKSE::log::info("adapter left: {}", a_id);
	}

	std::unordered_map<std::string, std::string> AdapterHost::Choose(
		const std::unordered_map<std::string, Entry>&       a_adapters,
		const std::unordered_map<std::string, std::string>& a_overrides)
	{
		// One source per capability. The seniority is this: what the menu forced
		// (SetSource), failing that the name from the settings (adapters.primary),
		// failing that whoever registered first. One that is named but cannot do it,
		// or never turned up, changes nothing: the capability goes to the first in
		// order rather than being left without a source. A forcing shadows the name
		// from the settings entirely, even when it did not take: the player said
		// their word in the menu, and the file is no longer their master.
		const auto& settings = Settings::Get();

		std::unordered_map<std::string, std::string> chosen;
		for (const auto& [id, entry] : a_adapters) {
			for (const auto& capability : entry.provides) {
				const auto  forced = a_overrides.find(capability);
				const auto& named = forced != a_overrides.end() ? forced->second
				                                                : settings.PrimaryAdapter(capability);
				auto current = chosen.find(capability);
				if (current == chosen.end() || (!named.empty() && id == named)) {
					chosen[capability] = id;
					continue;
				}
				if (current->second != named &&
					entry.order < a_adapters.at(current->second).order) {
					chosen[capability] = id;
				}
			}
		}
		return chosen;
	}

	std::vector<AdapterHost::Outgoing> AdapterHost::RecomputeSources()
	{
		std::vector<Outgoing> pending;

		const auto chosen = Choose(_adapters, _overrides);
		_sources = chosen;

		for (auto& entry : _adapters) {
			bool active = false;
			std::string role;
			for (const auto& capability : entry.second.provides) {
				auto it = chosen.find(capability);
				if (it != chosen.end() && it->second == entry.first) {
					active = true;
					role = capability;
					break;
				}
			}
			if (active == entry.second.active) {
				continue;
			}
			entry.second.active = active;

			Outgoing out;
			out.onJob = entry.second.onJob;
			out.user = entry.second.user;
			out.kind = EnvoyAPI::kJobListen;
			out.active = active;
			out.text = active ? "made the source: " + role : "somebody else was made the source";
			pending.push_back(std::move(out));

			SKSE::log::info("adapter {}: {}", entry.first, active ? "source" : "in reserve");
		}

		return pending;
	}

	std::int32_t AdapterHost::PushUtterance(const char* a_adapterId, const EnvoyAPI::UtteranceIn& a_in)
	{
		Utterance utterance;
		utterance.isFinal = a_in.isFinal;
		utterance.text = Safe(a_in.text);
		utterance.score = a_in.score;
		utterance.margin = a_in.margin;
		utterance.language = Safe(a_in.language);
		utterance.engine = Safe(a_in.engine);
		utterance.channel = Safe(a_in.channel);
		utterance.latencyMs = a_in.latencyMs;
		utterance.durationMs = a_in.durationMs;

		// The fields of the third version are read only from one that declared it: at
		// that offset an older adapter has somebody else memory, not zeroes.
		std::vector<std::int32_t> swallowed;
		{
			std::scoped_lock lock(_mutex);
			auto entry = _adapters.find(Safe(a_adapterId));
			if (entry != _adapters.end() && entry->second.contract >= 3) {
				utterance.complete = a_in.complete;
				utterance.lengthClass = a_in.lengthClass;
				if (a_in.supersedes && a_in.supersedesCount > 0) {
					swallowed.assign(a_in.supersedes,
						a_in.supersedes + a_in.supersedesCount);
				}
			}
		}

		// A count without an array is a fault of the adapter, not a reason to bring
		// the game down: scores without texts were already skipped, texts without
		// scores were not.
		for (std::int32_t i = 0; a_in.altText && i < a_in.altCount; ++i) {
			utterance.alternatives.push_back({ Safe(a_in.altText[i]),
				a_in.altScore ? a_in.altScore[i] : 0.0f });
		}

		if (a_in.refinesId != 0) {
			// A refinement from the accurate model: the utterance already lives and has
			// already gone out.
			if (!UtteranceStore::Get().Refine(a_in.refinesId, utterance)) {
				return 0;
			}
			// The words themselves only if the person allowed it. Otherwise the log keeps
			// the number and the model, which still make a diagnosis possible.
			if (Log::ShowSpeech()) {
				SKSE::log::info("utterance {} refined by adapter {} ({}): {}",
					a_in.refinesId, Safe(a_adapterId), utterance.engine, utterance.text);
			} else {
				SKSE::log::info("utterance {} refined by adapter {} ({})",
					a_in.refinesId, Safe(a_adapterId), utterance.engine);
				SKSE::log::debug("utterance {}: {}", a_in.refinesId, utterance.text);
			}
			return a_in.refinesId;
		}

		const auto id = UtteranceStore::Get().Add(std::move(utterance));
		if (Log::ShowSpeech()) {
			SKSE::log::info("utterance {} from adapter {}: {}", id, Safe(a_adapterId), Safe(a_in.text));
		} else {
			SKSE::log::info("utterance {} from adapter {}", id, Safe(a_adapterId));
			SKSE::log::debug("utterance {}: {}", id, Safe(a_in.text));
		}

		// Absorption before taking in: if the new piece has swallowed a held one, that
		// one has to be thrown out BEFORE the fate of the new one is decided.
		MainThread::Post([id, swallowed = std::move(swallowed)]() {
			if (!swallowed.empty()) {
				Auctioneer::Get().Supersede(id, swallowed);
			}
			Auctioneer::Get().Receive(id);
		});
		return id;
	}

	bool AdapterHost::SourceOf(const char* a_capability, char* a_out, std::int32_t a_outSize) const
	{
		if (!a_out || a_outSize <= 0) {
			return false;
		}
		a_out[0] = 0;

		std::scoped_lock lock(_mutex);
		auto it = _sources.find(Safe(a_capability));
		if (it == _sources.end()) {
			return false;
		}
		// The name must not be cut short: that would make it the name of a different
		// adapter, and the one asking would never find out. An honest refusal is
		// better.
		if (static_cast<std::size_t>(a_outSize) <= it->second.size()) {
			return false;
		}
		std::memcpy(a_out, it->second.c_str(), it->second.size() + 1);
		return true;
	}

	bool AdapterHost::SetSource(const std::string& a_capability, const std::string& a_adapter)
	{
		std::vector<Outgoing> pending;
		{
			std::scoped_lock lock(_mutex);
			if (!a_adapter.empty() && _adapters.find(a_adapter) == _adapters.end()) {
				return false;
			}
			if (a_adapter.empty()) {
				_overrides.erase(a_capability);
			} else {
				_overrides[a_capability] = a_adapter;
			}
			pending = RecomputeSources();
		}
		Dispatch(pending);
		return true;
	}

	std::string AdapterHost::Source(const std::string& a_capability) const
	{
		std::scoped_lock lock(_mutex);
		auto it = _sources.find(a_capability);
		return it == _sources.end() ? std::string{} : it->second;
	}

	std::vector<std::string> AdapterHost::AdapterIds() const
	{
		std::scoped_lock lock(_mutex);
		std::vector<std::string> out;
		out.reserve(_adapters.size());
		for (const auto& entry : _adapters) {
			out.push_back(entry.first);
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	void AdapterHost::ReloadConfig()
	{
		Config::Get().Load(Config::Get().Path());
		// The snapshot of the settings lives apart from the document, so it has to be
		// rebuilt - otherwise rereading the file would change nothing.
		Settings::Reload();

		std::vector<Outgoing> pending;
		{
			std::scoped_lock lock(_mutex);
			pending = RecomputeSources();
		}
		Dispatch(pending);
	}

	void AdapterHost::SendVocabulary(const std::vector<std::string>& a_phrases)
	{
		std::vector<Outgoing> pending;
		{
			std::scoped_lock lock(_mutex);
			for (auto& entry : _adapters) {
				Outgoing out;
				out.onJob = entry.second.onJob;
				out.user = entry.second.user;
				out.kind = EnvoyAPI::kJobVocabulary;
				out.phrases = a_phrases;
				pending.push_back(std::move(out));
			}
		}
		Dispatch(pending);
	}

	std::int32_t AdapterHost::SendSpeak(const std::string& a_text, const std::string& a_voice,
		std::int32_t a_priority)
	{
		std::vector<Outgoing> pending;
		std::int32_t           speechId = 0;
		std::string            target;
		{
			std::scoped_lock lock(_mutex);

			auto source = _sources.find("tts");
			if (source == _sources.end()) {
				SKSE::log::warn("nobody to speak it: there is no tts source");
				return 0;
			}
			auto adapter = _adapters.find(source->second);
			if (adapter == _adapters.end()) {
				return 0;
			}

			speechId = _nextSpeech++;
			target = source->second;

			Outgoing out;
			out.onJob = adapter->second.onJob;
			out.user = adapter->second.user;
			out.kind = EnvoyAPI::kJobSpeak;
			out.text = a_text;
			out.service = a_voice;
			out.speechId = speechId;
			out.requestId = a_priority;
			pending.push_back(std::move(out));
		}
		Dispatch(pending);

		SKSE::log::info("speech {} -> adapter {}: {}", speechId, target, a_text);
		return speechId;
	}

	void AdapterHost::SendStop(std::int32_t a_speechId)
	{
		std::vector<Outgoing> pending;
		{
			std::scoped_lock lock(_mutex);
			for (auto& entry : _adapters) {
				Outgoing out;
				out.onJob = entry.second.onJob;
				out.user = entry.second.user;
				out.kind = EnvoyAPI::kJobStop;
				out.speechId = a_speechId;
				pending.push_back(std::move(out));
			}
		}
		Dispatch(pending);
	}

	std::int32_t AdapterHost::SendAsk(const std::string& a_service, const std::string& a_payload)
	{
		std::vector<Outgoing> pending;
		std::int32_t           requestId = 0;
		std::string            target;
		{
			std::scoped_lock lock(_mutex);

			// The capability is the name of the service: whoever declared "llm" is the one
			// that answers "llm".
			auto source = _sources.find(a_service);
			if (source == _sources.end()) {
				SKSE::log::warn("nobody to ask: there is no {} source", a_service);
				return 0;
			}
			auto adapter = _adapters.find(source->second);
			if (adapter == _adapters.end()) {
				return 0;
			}

			requestId = _nextRequest++;
			target = source->second;

			Outgoing out;
			out.onJob = adapter->second.onJob;
			out.user = adapter->second.user;
			out.kind = EnvoyAPI::kJobAsk;
			out.requestId = requestId;
			out.service = a_service;
			out.payload = a_payload;
			pending.push_back(std::move(out));
		}
		Dispatch(pending);

		SKSE::log::info("request {} to {} -> adapter {}", requestId, a_service, target);
		return requestId;
	}

	std::string AdapterHost::Answer(std::int32_t a_requestId) const
	{
		std::scoped_lock lock(_mutex);
		auto it = _answers.find(a_requestId);
		return it == _answers.end() ? std::string{} : it->second;
	}

	std::string AdapterHost::SpeechResult(std::int32_t a_speechId) const
	{
		std::scoped_lock lock(_mutex);
		auto it = _speechResults.find(a_speechId);
		return it == _speechResults.end() ? std::string{} : it->second;
	}

	void AdapterHost::PushAnswer(const char* a_adapterId, std::int32_t a_requestId, bool a_ok,
		const char* a_payload)
	{
		{
			std::scoped_lock lock(_mutex);
			_answers[a_requestId] = Safe(a_payload);
		}
		SKSE::log::info("answer {} from adapter {}: {}", a_requestId, Safe(a_adapterId),
			a_ok ? "success" : "failure");

		// Through the seams of the core rather than straight to SKSE: a seam has a
		// fallback path with a warning, a direct call has a silent loss.
		MainThread::Post([a_requestId]() {
			Events::Send("Envoy_Answer", "", static_cast<float>(a_requestId));
		});
	}

	void AdapterHost::PushSpeechDone(const char* a_adapterId, std::int32_t a_speechId, bool a_ok,
		bool a_interrupted)
	{
		const std::string how = a_interrupted ? "interrupted" : (a_ok ? "ok" : "failed");
		{
			std::scoped_lock lock(_mutex);
			_speechResults[a_speechId] = how;
		}
		SKSE::log::info("speech {} at adapter {}: {}", a_speechId, Safe(a_adapterId), how);

		MainThread::Post([a_speechId]() {
			Events::Send("Envoy_SpeechDone", "", static_cast<float>(a_speechId));
		});
	}
}
