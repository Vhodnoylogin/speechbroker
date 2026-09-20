// Speech Broker - running the auction without the game.
//
//     speechbroker-host [--subscribers <folder>] [--scenario <file>] [--config <file>] [--report <file>]
//
// The core here is the very same one that runs inside the game: the same
// auction, the same store of utterances, the same choice of topic and the same
// vocabularies. The difference is exactly three answers which this host gives
// instead of Skyrim:
//
//   work      - is put into a queue the main thread drains itself;
//   state     - is taken from the scenario rather than from the engine;
//   events    - are written to the log rather than broadcast to Papyrus.
//
// The subscribers here are not real either. All that is wanted from them is
// what they declared - which topics they are subscribed to and which
// vocabulary they announced. What a subscriber does with a win is not what
// the check is about: the question is WHO the bridge gave the fragment to and
// why, not what the winner did afterwards.

#include "bus/Auction.h"
#include "bus/SubscriptionRegistry.h"
#include "bus/Utterance.h"
#include "bus/UtteranceStore.h"
#include "core/Config.h"
#include "core/GameLanguage.h"
#include "core/GameState.h"
#include "core/Loc.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Scheduler.h"
#include "core/Settings.h"
#include "market/MarketRegistry.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
// Without this windows.h declares the macros min and max, and any std::min
// stops parsing - with an error that does not point at the macro.
#	define NOMINMAX
#	include <windows.h>
#endif

namespace
{
	namespace fs = std::filesystem;
	using json = nlohmann::json;

	// The state of the game, from the scenario.
	//
	// Outside the game there is none, but the state cannot be dropped from the
	// check: the topic is chosen precisely by it, and "said in combat" against
	// "said out in the world" are different sets of subscribers, not a shade of
	// meaning.
	class ScriptedState final : public SpeechBroker::GameState::Source
	{
	public:
		std::string menu;
		bool        paused{ false };
		bool        combat{ false };

		void Reset()
		{
			menu.clear();
			paused = false;
			combat = false;
		}

		bool IsMenuOpen(const std::string& a_name) const override
		{
			return !menu.empty() && a_name == menu;
		}
		bool IsPaused() const override { return paused; }
		bool IsInCombat() const override { return combat; }
	};

	// The main thread of the host.
	//
	// With no dispatcher the core does the work right where it was called - but it
	// is not called only from here. The hold ceiling fires in the thread of the
	// scheduler, and "on the spot" would mean "in that thread": letting an
	// utterance go would run alongside taking in the next one and alongside our
	// bids. Two runs of one binary gave now 7 bids on an utterance and now 5,
	// depending on who got there first. Here the work is put into a queue and the
	// main thread drains it between its own steps: the order becomes strictly
	// defined, and nothing is left in the core that would be done from two threads
	// at once.
	// --- two markets, and what they were handed ------------------------------
	//
	// A market is a MOD, and until now the bridge had a contract with no caller at
	// all: the register compiled and nothing had ever registered with it. These
	// two do nothing but write down what arrived. They are named so that sorting
	// puts them in a known order, because the contract promises that the order of
	// offering does not depend on who loaded first - and a promise nobody
	// measured is a wish.
	struct MarketWitness
	{
		std::string              id;
		std::vector<std::string> seen;
	};

	MarketWitness            marketOpen{ "aaa-open", {} };
	MarketWitness            marketAuction{ "zzz-auction", {} };
	std::vector<std::string> marketOrder;

	void OnMarketOffer(const SpeechBrokerMarketAPI::Packet& a_packet, void* a_user)
	{
		auto* witness = static_cast<MarketWitness*>(a_user);
		marketOrder.push_back(witness->id);

		std::string world;
		for (std::int32_t i = 0; i < a_packet.stateCount; ++i) {
			if (!world.empty()) {
				world += ", ";
			}
			world += std::string(a_packet.stateKeys[i]) + "=" + a_packet.stateValues[i];
		}

		witness->seen.push_back(
			"utterance " + std::to_string(a_packet.id) +
			"  topic " + (a_packet.topic ? a_packet.topic : "") +
			"  handedOut " + std::to_string(a_packet.handedOut) +
			"  world [" + world + "]" +
			"  text '" + (a_packet.text ? a_packet.text : "") + "'");

		// The first market takes it. The second is offered the SAME packet in the
		// same round and must still read handedOut 0, because that field is a
		// snapshot taken before anybody was asked - while asking the register
		// there and then must say 1. Both halves of that rule are printed.
		if (witness->id == "aaa-open") {
			SpeechBroker::UtteranceStore::Get().MarkHandedOut(a_packet.id, witness->id);
			witness->seen.back() += "  -> took it";
		} else {
			witness->seen.back() += "  -> asked the register: " +
				std::to_string(SpeechBroker::MarketRegistry::Get().HandOutState(a_packet.id));
		}
	}


	class MainQueue final : public SpeechBroker::MainThread::Dispatcher
	{
	public:
		void Post(SpeechBroker::MainThread::Task a_task) override
		{
			std::scoped_lock lock(_mutex);
			_queue.push_back(std::move(a_task));
		}

		// Run everything that has piled up. A task is entitled to queue the next one,
		// so the queue is taken whole and the lock is let go for the duration.
		void Drain()
		{
			std::vector<SpeechBroker::MainThread::Task> batch;
			{
				std::scoped_lock lock(_mutex);
				batch.swap(_queue);
			}
			for (auto& task : batch) {
				task();
			}
		}

	private:
		std::mutex                           _mutex;
		std::vector<SpeechBroker::MainThread::Task> _queue;
	};

	// A test subscriber: nothing but what it declared about itself.
	struct TestSubscriber
	{
		std::string              ns;
		std::vector<std::string> topics;
		std::vector<std::string> vocabulary;
		std::int32_t             costClass{ 0 };   // 0 reversible, 1 expensive
		bool                     greedy{ false };
		// Whether it can undo what it did. To a revocable one the bridge hands an
		// unfinished phrase straight away: it can put itself right. To a
		// non-revocable one only when it is sure the phrase has ended.
		bool                     revocable{ false };
	};

	std::string CostName(std::int32_t a_class)
	{
		return a_class == 1 ? "expensive" : "reversible";
	}

	json ReadJson(const fs::path& a_path)
	{
		std::ifstream in(a_path, std::ios::binary);
		if (!in) {
			return json{};
		}
		try {
			return json::parse(in);
		} catch (const std::exception& exc) {
			spdlog::error("cannot parse {}: {}", a_path.string(), exc.what());
			return json{};
		}
	}

	// The cast of participants: who declared themselves and what they said about
	// themselves.
	class Roster
	{
	public:
		bool Load(const fs::path& a_dir)
		{
			if (!fs::exists(a_dir)) {
				spdlog::error("no subscribers folder: {}", a_dir.string());
				return false;
			}

			std::vector<fs::path> files;
			for (const auto& entry : fs::directory_iterator(a_dir)) {
				if (entry.is_regular_file() && entry.path().extension() == ".json") {
					files.push_back(entry.path());
				}
			}
			// By file name: the order of declaration must not depend on how the file
			// system felt like handing them over.
			std::sort(files.begin(), files.end());

			for (const auto& file : files) {
				const auto doc = ReadJson(file);
				if (!doc.is_object()) {
					continue;
				}

				TestSubscriber sub;
				sub.ns = doc.value("namespace", file.stem().string());
				sub.topics = doc.value("topics", std::vector<std::string>{ "world" });
				sub.vocabulary = doc.value("vocabulary", std::vector<std::string>{});
				sub.costClass = doc.value("cost", std::string{ "reversible" }) == "costly" ? 1 : 0;
				sub.greedy = doc.value("greedy", false);
				sub.revocable = doc.value("revocable", false);
				_subs.push_back(std::move(sub));
			}
			return !_subs.empty();
		}

		// Declare them all to the bridge - just as the script of each would.
		void Declare() const
		{
			auto& registry = SpeechBroker::SubscriptionRegistry::Get();
			for (const auto& sub : _subs) {
				registry.Subscribe(sub.ns, sub.topics);
				// The kind is declared separately from the subscription: the decision to hold
				// an utterance is taken BEFORE anybody has managed to bid, and it cannot lean
				// on bids.
				registry.Declare(sub.ns, sub.costClass, sub.revocable);
				if (!sub.vocabulary.empty()) {
					registry.SetVocabulary(sub.ns, sub.vocabulary);
				}

				std::string topics;
				for (const auto& topic : sub.topics) {
					topics += topics.empty() ? topic : ", " + topic;
				}
				spdlog::info("subscriber {}: topics [{}], phrases {}, {}{}{}", sub.ns, topics,
					sub.vocabulary.size(), CostName(sub.costClass),
					sub.greedy ? ", greedy" : "",
					sub.revocable ? ", revocable" : "");
			}
		}

		// Greed is not declared to the registry - it is a property of a bid, not of a
		// subscription - so the host takes it from its own description of the
		// subscriber.
		bool Greedy(const std::string& a_ns) const
		{
			const auto it = std::find_if(_subs.begin(), _subs.end(),
				[&](const TestSubscriber& s) { return s.ns == a_ns; });
			return it != _subs.end() && it->greedy;
		}

	private:
		std::vector<TestSubscriber> _subs;
	};

	// Running a scenario: the steps, the bids on behalf of the subscribers, the
	// wait for the held ones and a report on every utterance.
	class Run
	{
	public:
		Run(ScriptedState& a_state, MainQueue& a_main, const Roster& a_roster) :
			_state(a_state), _main(a_main), _roster(a_roster)
		{}

		void Play(const json& a_steps)
		{
			for (const auto& step : a_steps) {
				Step(step);
			}

			// The scenario has ended, but the ceilings of the held ones may not have run
			// out yet. We wait for them and add the outcome: an utterance let go last
			// has to be in the report just like all the others.
			if (!_waiting.empty()) {
				spdlog::info("waiting for {} held utterances", _waiting.size());
				// How long to wait is said by the settings: a hold never lasts longer than
				// the longest ceiling, and after that come the bid window and some room for
				// the queue.
				const auto& settings = SpeechBroker::Settings::Get();
				const auto  ceiling = std::max({ settings.HoldCeilingMs(0),
                                                    settings.HoldCeilingMs(1),
                                                    settings.HoldCeilingMs(2) });
				Wait(ceiling + kQueueSlackMs + settings.bidWindowMs + kQueueSlackMs);
			}

			Keep();
		}

		std::string Report(const std::string& a_scenarioName) const
		{
			std::ostringstream report;
			report << "=== auction run without the game ===\n\n";
			report << "scenario: " << a_scenarioName << "\n";
			report << "bid window: " << SpeechBroker::Settings::Get().bidWindowMs << " ms\n\n";

			// Now that everything is settled, the outcomes are read back from the snapshot.
			std::size_t heldCount = 0, droppedCount = 0;
			for (const auto& told : _history) {
				const auto found = _snapshot.find(told.id);
				const SpeechBroker::Utterance* done = found == _snapshot.end() ? nullptr : &found->second;

				report << "utterance " << told.id << ": '" << (done ? done->text : std::string{}) << "'\n";
				if (!told.source.empty()) {
					report << "    source    : " << told.source << "  (reference '"
					       << told.reference << "')\n";
				}
				if (told.hadComplete) {
					report << "    piece     : " << told.sliceId << ", completeness "
					       << told.complete << "\n";
				}
				for (const auto& line : told.swallowed) {
					report << "    absorbs   : " << line << "\n";
				}
				report << "    topic     : " << told.topic << "\n";

				// What the collectors of world state read when this was spoken. It is
				// printed for every utterance, because the point of the channel is that
				// the reading belongs to the MOMENT: a line that showed up only when it
				// looked interesting would prove nothing about the ones where it did not.
				if (done && !done->state.empty()) {
					std::string readings;
					for (const auto& entry : done->state) {
						if (!readings.empty()) {
							readings += ", ";
						}
						readings += entry.first + "=" + entry.second;
					}
					report << "    world     : " << readings << "\n";
				}

				if (done && !done->holdReason.empty()) {
					++heldCount;
					report << "    held      : " << done->holdReason << "\n";
					if (done->supersededBy != 0) {
						++droppedCount;
						report << "    DROPPED   : never announced, absorbed by utterance "
						       << done->supersededBy << "\n";
					}
				}

				if (done) {
					report << "    bids      : " << done->bids.size() << "\n";
					for (const auto& bid : done->bids) {
						report << "        " << bid.ns << "  " << bid.confidence
						       << "  " << CostName(bid.costClass)
						       << (bid.greedy ? ", greedy" : "")
						       << "  phrase '" << bid.phrase << "'\n";
					}
					std::string winners;
					for (const auto& who : done->winners) {
						winners += winners.empty() ? who : ", " + who;
					}
					report << "    won       : " << (winners.empty() ? "nobody" : winners) << "\n";
					for (const auto& [who, why] : done->denied) {
						report << "    denied    : " << who << " - " << why << "\n";
					}
					report << "    outcome   : " << done->outcome << "\n";
				}
				report << "\n";
			}

			report << "=== holding ===\n";
			report << "utterances held: " << heldCount << " of " << _history.size() << "\n";
			report << "of those dropped unannounced: " << droppedCount
			       << " - that many times a fragment of a phrase did NOT go to anybody\n\n";
			return report.str();
		}

	private:
		// What is known about an utterance at its own step alone. The outcome is read
		// back from the store at the end: for a held one it appears later than the
		// step finishes.
		struct Told
		{
			std::int32_t             id{ 0 };
			std::string              source;
			std::string              reference;
			std::string              topic;
			std::int32_t             sliceId{ 0 };
			float                    complete{ 1.0f };
			bool                     hadComplete{ false };
			std::vector<std::string> swallowed;
		};

		// The waiting goes in slices, and between the slices the main thread does two
		// things: it drains the queue of the core and bids for the ones let go. A
		// slice is a step of lateness to the bidding, not a setting: the bid window
		// opens the moment a held utterance is let go, and the check must not sleep
		// through it.
		static constexpr std::int64_t kPollSliceMs = 50;
		// Room for the fact that the outcome is queued from the thread of the
		// scheduler and is not visible until the next slice. A few slices over.
		static constexpr std::int64_t kQueueSlackMs = 250;

		void Step(const json& a_step)
		{
			_state.Reset();
			// We check the object itself rather than the presence of the key: a scenario
			// put together by a program may well leave emptiness there, and parsing that
			// as an object would mean bringing the whole run down on one step.
			if (a_step.contains("state") && a_step["state"].is_object()) {
				const auto& s = a_step["state"];
				_state.menu = s.value("menu", std::string{});
				_state.paused = s.value("paused", false);
				_state.combat = s.value("combat", false);
			}

			// The number of a piece at the engine is its own in every recording, while the
			// number of an utterance runs across all of them. The link between the two is
			// needed so that a long piece can say which short ones it absorbs: that is
			// exactly where it shows whether the bridge managed to hand a command over
			// before it turned out the phrase had not ended.
			const auto source = a_step.value("source", std::string{});
			if (source != _currentSource) {
				_currentSource = source;
				_sliceToId.clear();
				_lastEmitMs = 0;
			}

			// The pieces of one recording are played by their timestamps and not one after
			// another. Otherwise the continuation arrives instantly and holding looks as
			// though it works where in life the ceiling would have run out.
			const auto emitMs = a_step.value("emitMs", 0);
			if (emitMs > _lastEmitMs) {
				Wait(emitMs - _lastEmitMs);
				_lastEmitMs = emitMs;
			}

			SpeechBroker::Utterance utterance;
			utterance.text = a_step.value("text", std::string{});
			utterance.score = a_step.value("score", 0.9f);
			utterance.margin = a_step.value("margin", 0.3f);
			utterance.channel = a_step.value("channel", std::string{});
			utterance.language = a_step.value("language", std::string{ "ru" });
			utterance.engine = a_step.value("engine", std::string{ "host" });
			utterance.lengthClass = a_step.value("lengthClass", 0);
			utterance.sliceId = a_step.value("sliceId", 0);
			utterance.durationMs = a_step.value("durationMs", 0);
			// The default of one: a scenario that knows nothing about completeness behaves
			// as before - everything arrives finished and nothing is held.
			utterance.complete = a_step.value("complete", 1.0f);
			utterance.isFinal = true;

			// The other hypotheses of the engine. The auction does not ask for them yet,
			// but an utterance is obliged to carry them whole: a subscriber is entitled
			// to see that the phrase can be understood otherwise, and the bridge is not
			// entitled to decide that for it.
			if (a_step.contains("alternatives")) {
				for (const auto& alt : a_step["alternatives"]) {
					utterance.alternatives.push_back(SpeechBroker::Alternative{
						alt.value("text", std::string{}), alt.value("score", 0.0f) });
				}
			}

			const auto id = SpeechBroker::UtteranceStore::Get().Add(utterance);
			if (utterance.sliceId != 0) {
				_sliceToId[utterance.sliceId] = id;
			}

			// A long piece absorbs the short ones it is made of. That is decided by the
			// bridge and not by the host: the held ones it will throw away unannounced,
			// and the ones already handed over it will revoke.
			std::vector<std::int32_t> older;
			if (a_step.contains("supersedes")) {
				for (const auto& mark : a_step["supersedes"]) {
					const auto found = _sliceToId.find(mark.get<int>());
					if (found != _sliceToId.end()) {
						older.push_back(found->second);
					}
				}
			}

			spdlog::info("--- utterance {}: '{}' (completeness {:.2f}) ---", id, utterance.text,
				utterance.complete);

			std::vector<std::string> swallowed;
			for (const auto mark : older) {
				auto was = SpeechBroker::UtteranceStore::Get().Find(mark);
				if (!was) {
					continue;
				}
				std::string who;
				for (const auto& winner : was->winners) {
					who += who.empty() ? winner : ", " + winner;
				}
				swallowed.push_back("utterance " + std::to_string(mark) +
					(was->held ? " - held, dropped unannounced"
					           : (who.empty() ? " - nobody got it"
					                          : " - BUT IT WAS ALREADY HANDED OVER: " + who)));
			}
			if (!older.empty()) {
				SpeechBroker::Auctioneer::Get().Supersede(id, older);
			}

			// Taking in instead of announcing. The bridge will decide for itself whether
			// to announce the utterance now or hold it until it is clear whether the
			// phrase has ended.
			SpeechBroker::Auctioneer::Get().Receive(id);

			auto offered = SpeechBroker::UtteranceStore::Get().Find(id);
			const std::string topic = offered ? offered->topic : std::string{ "?" };
			const bool held = offered && offered->held;

			if (held) {
				// A held one is neither bid on nor waited for: in life the engine goes on
				// working at this time, and the continuation may arrive before the ceiling
				// runs out. We will bid on it if it is let go after all.
				_waiting.push_back(id);
				spdlog::info("utterance {} is held - no bids are gathered", id);
			} else {
				PlaceBids(id, utterance.text, utterance.score, topic);
				// The outcome is settled by the auctioneer itself once the bid window has run
				// out, from the thread of the scheduler. We wait for it rather than settle
				// on its behalf.
				Wait(SpeechBroker::Settings::Get().bidWindowMs + kQueueSlackMs);
			}

			// The outcome is not written here. A held utterance is settled later - when it
			// is absorbed or let go on the ceiling - and a snapshot taken now would show
			// it forever unsettled. Everything known only at this step is remembered;
			// the rest is read back from the store at the end.
			Told told;
			told.id = id;
			told.source = source;
			told.reference = a_step.value("reference", std::string{});
			told.sliceId = utterance.sliceId;
			told.complete = utterance.complete;
			told.topic = topic;
			told.swallowed = std::move(swallowed);
			told.hadComplete = a_step.contains("complete");
			_history.push_back(std::move(told));
		}

		// Bids instead of Papyrus scripts. Who is in the room - the subscribers of the
		// topic whose vocabularies recognised the phrase - is answered by the
		// registry itself, by the same question holding uses; the host has no copy
		// of its own of the rule "does this subscriber hear this topic".
		// The confidence is the same estimate by which holding judges whether a
		// subscriber would reach the threshold. A real subscriber works it out
		// itself; here the simplest defensible rule is taken, because the behaviour
		// of subscribers is not what the check is about.
		void PlaceBids(std::int32_t a_id, const std::string& a_text, float a_score,
			const std::string& a_topic) const
		{
			const auto heard = SpeechBroker::SubscriptionRegistry::Get().Audience(a_topic, a_text);
			for (const auto& who : heard) {
				const bool  greedy = _roster.Greedy(who.ns);
				const float confidence = who.match.Confidence(a_score);
				SpeechBroker::UtteranceStore::Get().AddBid(a_id,
					SpeechBroker::BidRecord{ who.ns, confidence, who.costClass, greedy, who.match.phrase });
				spdlog::info("bid {}: confidence {:.2f} (audibility {:.2f} x vocabulary {:.2f}), "
				             "phrase '{}', {}{}",
					who.ns, confidence, a_score, who.match.score, who.match.phrase,
					CostName(who.costClass), greedy ? ", greedy" : "");
			}
			if (heard.empty()) {
				spdlog::info("no bids - not one subscriber of topic {} recognised the phrase", a_topic);
			}
		}

		// A held utterance may have been let go on the ceiling while we were busy with
		// the next piece. The announcement happened - so it is time to bid.
		void CatchUp()
		{
			std::vector<std::int32_t> still;
			for (const auto id : _waiting) {
				auto item = SpeechBroker::UtteranceStore::Get().Find(id);
				if (!item || item->supersededBy != 0) {
					continue;   // absorbed - its fate is settled, nothing to bid on
				}
				if (item->held) {
					still.push_back(id);
					continue;
				}
				spdlog::info("utterance {} let go - bidding late", id);
				PlaceBids(id, item->text, item->score, item->topic);
			}
			_waiting.swap(still);
		}

		// The last known state of every utterance.
		//
		// The store of the bridge is a live cache, not a journal: it is cleaned by
		// deadline and by count, and by the end of a run the early utterances have
		// been washed out of it. That is right for the bridge and wrong for a check,
		// which needs the outcome of EVERY one. So the snapshot is kept here rather
		// than demanding the store be something it is not.
		void Keep()
		{
			for (const auto& told : _history) {
				if (auto item = SpeechBroker::UtteranceStore::Get().Find(told.id)) {
					_snapshot[told.id] = *item;
				}
			}
		}

		// The waiting must not be one sleep but short slices with work between them:
		// drain the queue of the core, bid for the ones let go, refresh the snapshot.
		void Wait(std::int64_t a_ms)
		{
			for (std::int64_t left = a_ms; left > 0; left -= kPollSliceMs) {
				std::this_thread::sleep_for(
					std::chrono::milliseconds(left < kPollSliceMs ? left : kPollSliceMs));
				_main.Drain();
				CatchUp();
				Keep();
			}
		}

		ScriptedState& _state;
		MainQueue&     _main;
		const Roster&  _roster;

		std::string                 _currentSource;
		std::map<int, std::int32_t> _sliceToId;
		std::int32_t                _lastEmitMs{ 0 };
		std::vector<std::int32_t>   _waiting;   // the held ones whose fate we do not know yet
		std::vector<Told>           _history;
		std::map<std::int32_t, SpeechBroker::Utterance> _snapshot;
	};
}

// --------------------------------------------------------------- the strings
//
// THE TABLE OF TRANSLATIONS, CHECKED OUTSIDE THE GAME. It is here because of
// what it cost: the folder scan compared five characters of the prefix against
// the word "speechbroker", which can never be equal, so every table in every run
// loaded nothing, every $-key came back as itself, and a subscriber registered
// THE KEY as the phrase it was listening for. Nobody could say a word the mod
// would recognise. One assertion below would have caught it on the day it was
// written.
namespace
{
	void WriteUtf16(const fs::path& a_path, const std::string& a_utf8)
	{
		std::ofstream out(a_path, std::ios::binary);
		out.put(static_cast<char>(0xFF));
		out.put(static_cast<char>(0xFE));
		// The tables are ASCII and Cyrillic, both inside the basic plane, so one
		// unit per code point is exact here.
		std::u32string wide;
		for (std::size_t i = 0; i < a_utf8.size();) {
			const auto byte = static_cast<unsigned char>(a_utf8[i]);
			char32_t   unit = byte;
			std::size_t take = 1;
			if (byte >= 0xF0) { unit = byte & 0x07u; take = 4; }
			else if (byte >= 0xE0) { unit = byte & 0x0Fu; take = 3; }
			else if (byte >= 0xC0) { unit = byte & 0x1Fu; take = 2; }
			for (std::size_t k = 1; k < take && i + k < a_utf8.size(); ++k) {
				unit = (unit << 6) | (static_cast<unsigned char>(a_utf8[i + k]) & 0x3Fu);
			}
			i += take;
			wide.push_back(unit);
		}
		for (const auto unit : wide) {
			out.put(static_cast<char>(unit & 0xFF));
			out.put(static_cast<char>((unit >> 8) & 0xFF));
		}
	}

	void WriteUtf8(const fs::path& a_path, const std::string& a_utf8)
	{
		std::ofstream out(a_path, std::ios::binary);
		out.write(a_utf8.data(), static_cast<std::streamsize>(a_utf8.size()));
	}

	bool Says(const char* a_key, const std::string& a_expected)
	{
		const std::string got = SpeechBroker::Loc::Get(a_key);
		const bool        ok = got == a_expected;
		std::printf("  %-4s %-38s %s\n", ok ? "ok" : "FAIL", a_key, got.c_str());
		return ok;
	}

	// The same question against two REAL ini files, named on the command line:
	//     speechbroker-host --loc "<game>\Skyrim.ini" "<documents>\Skyrim.ini"
	// It answers with the language those two files settle on, which is the one the
	// plugin will use. Nothing is hard-coded here on purpose - the paths differ on
	// every machine, and a check that only works on one machine is not a check.
	int RunLanguageOfFiles(const fs::path& a_besideTheExe, const fs::path& a_inDocuments)
	{
		const auto beside = SpeechBroker::GameLanguage::FromIniFile(a_besideTheExe);
		const auto mine = SpeechBroker::GameLanguage::FromIniFile(a_inDocuments);
		const auto answer = SpeechBroker::GameLanguage::Of(a_besideTheExe, a_inDocuments);

		std::printf("beside the exe : %-10s %s\n", beside.empty() ? "(nothing)" : beside.c_str(),
			a_besideTheExe.string().c_str());
		std::printf("in documents   : %-10s %s\n", mine.empty() ? "(nothing)" : mine.c_str(),
			a_inDocuments.string().c_str());
		std::printf("the plugin will read SpeechBroker*_%s.txt\n",
			answer.empty() ? "english (nothing named one)" : answer.c_str());
		return answer.empty() ? 1 : 0;
	}

	int RunLocCheck()
	{
		const auto dir = fs::temp_directory_path() / "speechbroker-loc-check";
		std::error_code ec;
		fs::remove_all(dir, ec);
		fs::create_directories(dir, ec);

		// Ours, and a subscriber's - the case that matters, because a subscriber's
		// keys live in a file this plugin has never heard of.
		WriteUtf16(dir / "SpeechBroker_english.txt", "$SPEECHBROKER_CHECK_OURS\tours\n");
		WriteUtf16(dir / "SpeechBrokerDemo_english.txt", "$SPEECHBROKERDEMO_WORD_DOOR\tclose the door\n");
		// A translator who saved out of an ordinary editor.
		WriteUtf8(dir / "SpeechBrokerPlain_english.txt", "$SPEECHBROKER_CHECK_UTF8\tplain\n");
		// Another language, and another mod entirely: neither may be absorbed.
		WriteUtf16(dir / "SpeechBrokerDemo_russian.txt", "$SPEECHBROKERDEMO_WORD_DOOR\tзакрой дверь\n");
		WriteUtf16(dir / "Unrelated_english.txt", "$SPEECHBROKER_CHECK_ALIEN\talien\n");

		SpeechBroker::Loc::Load(dir, "english");

		std::printf("translations: %zu strings from %zu files, language %s\n",
			SpeechBroker::Loc::Count(), SpeechBroker::Loc::Files(),
			SpeechBroker::Loc::Language().c_str());

		bool ok = true;
		ok = Says("$SPEECHBROKER_CHECK_OURS", "ours") && ok;
		ok = Says("$SPEECHBROKERDEMO_WORD_DOOR", "close the door") && ok;
		ok = Says("$SPEECHBROKER_CHECK_UTF8", "plain") && ok;
		ok = Says("$SPEECHBROKER_CHECK_ALIEN", "$SPEECHBROKER_CHECK_ALIEN") && ok;
		ok = Says("$SPEECHBROKER_CHECK_NOBODY", "$SPEECHBROKER_CHECK_NOBODY") && ok;

		if (SpeechBroker::Loc::Files() != 3) {
			std::printf("  FAIL three files were laid out for this language, %zu were read\n",
				SpeechBroker::Loc::Files());
			ok = false;
		}

		// And the language the files are chosen BY. A wrong answer here loads the
		// neighbouring file and hands a Russian player English words, which is
		// what happened for a whole evening on 20.09.2026.
		const auto language = [&ok](const char* a_what, const std::string& a_got, const std::string& a_want) {
			const bool good = a_got == a_want;
			std::printf("  %-4s %-38s %s\n", good ? "ok" : "FAIL", a_what, a_got.empty() ? "(nothing)" : a_got.c_str());
			ok = good && ok;
		};
		language("sLanguage under [General]",
			SpeechBroker::GameLanguage::FromIniText("[General]\r\nsLanguage=RUSSIAN\r\nuGridsToLoad=5\r\n"), "russian");
		language("a commented-out one is not taken",
			SpeechBroker::GameLanguage::FromIniText("[General]\r\n;sLanguage=GERMAN\r\nsLanguage=russian ; mine\r\n"), "russian");
		language("a foreign section is left alone",
			SpeechBroker::GameLanguage::FromIniText("[Archive]\r\nsLanguage=GERMAN\r\n"), "");
		language("nothing said is nothing answered",
			SpeechBroker::GameLanguage::FromIniText("[General]\r\nuGridsToLoad=5\r\n"), "");

		fs::remove_all(dir, ec);
		std::printf("%s\n", ok ? "the table reads what is put in front of it" : "THE TABLE IS BROKEN");
		return ok ? 0 : 1;
	}
}

int main(int argc, char** argv)
{
#ifdef _WIN32
	::SetConsoleOutputCP(CP_UTF8);
#endif

	for (int i = 1; i < argc; ++i) {
		if (std::string(argv[i]) != "--loc") {
			continue;
		}
		// Two paths after it mean "tell me what these two real files settle on";
		// nothing after it means the self-contained check.
		if (i + 2 < argc) {
			return RunLanguageOfFiles(argv[i + 1], argv[i + 2]);
		}
		return RunLocCheck();
	}

	// The host makes itself a settings file: the built-in reference unfolds next to
	// the executable, and the rules of the auction come out exactly the same as
	// in the game - without a single path set here. Next to the executable
	// precisely: running from the root of the repository used to leave the file
	// in it.
	fs::path subscribersDir = SPEECHBROKER_TEST_DIR "/subscribers";
	fs::path scenarioFile = SPEECHBROKER_TEST_DIR "/scenarios/default.json";
	fs::path configFile = fs::absolute(argv[0]).parent_path() / "speechbroker-host.json";
	bool     withMarkets = false;
	fs::path reportFile;

	for (int i = 1; i + 1 < argc; ++i) {
		const std::string key = argv[i];
		if (key == "--subscribers") {
			subscribersDir = argv[++i];
		} else if (key == "--scenario") {
			scenarioFile = argv[++i];
		} else if (key == "--config") {
			configFile = argv[++i];
		} else if (key == "--markets") {
			withMarkets = true;
		} else if (key == "--report") {
			reportFile = argv[++i];
		}
	}

	SpeechBroker::Log::ToConsole("info");

	// The text of the module, out of the set compiled into it: there is no folder
	// of translations next to the host and no need for one. The point is that the
	// report reads as sentences rather than as a list of keys - a check is read by
	// a person, and one whose report needs decoding proves nothing.
	SpeechBroker::Loc::Load({}, "english");

	auto& config = SpeechBroker::Config::Get();
	config.Load(configFile);
	spdlog::info("{}: {}", SpeechBroker::Config::Describe(config.Source()), config.Path().string());

	// Two of the three seams of the core are answered by the host; the events stay
	// in the log.
	static ScriptedState state;
	static MainQueue     main;
	SpeechBroker::GameState::Install(&state);
	SpeechBroker::MainThread::Install(&main);

	Roster roster;
	if (!roster.Load(subscribersDir)) {
		spdlog::error("not a single subscriber - nothing to check");
		return 2;
	}
	roster.Declare();

	const auto scenario = ReadJson(scenarioFile);
	const auto steps = scenario.contains("steps") ? scenario["steps"] : json::array();
	if (steps.empty()) {
		spdlog::error("the scenario has no steps: {}", scenarioFile.string());
		return 2;
	}
	const auto scenarioName = scenario.value("name", scenarioFile.stem().string());
	spdlog::info("scenario '{}', {} steps", scenarioName, steps.size());

	if (withMarkets) {
		// Registered in the WRONG order on purpose: zzz first, aaa second. If the
		// offering comes out sorted anyway, the promise holds.
		SpeechBrokerMarketAPI::MarketInfo info;
		info.name = "a market that only remembers";
		info.id = marketAuction.id.c_str();
		SpeechBroker::MarketRegistry::Get().Register(info, &OnMarketOffer, &marketAuction);
		info.id = marketOpen.id.c_str();
		SpeechBroker::MarketRegistry::Get().Register(info, &OnMarketOffer, &marketOpen);

		SpeechBrokerMarketAPI::MarketInfo again;
		again.id = marketOpen.id.c_str();
		again.name = "a second claim to a name already taken";
		const bool refusedTwice =
			!SpeechBroker::MarketRegistry::Get().Register(again, &OnMarketOffer, &marketOpen);

		SpeechBrokerMarketAPI::MarketInfo ancient;
		ancient.id = "built-against-yesterday";
		ancient.name = "a market built against another version of the contract";
		ancient.contract = 0;
		const bool refusedOld =
			!SpeechBroker::MarketRegistry::Get().Register(ancient, &OnMarketOffer, &marketOpen);

		spdlog::info("markets: {} registered, duplicate name refused: {}, wrong contract refused: {}",
			SpeechBroker::MarketRegistry::Get().Count(), refusedTwice, refusedOld);
	}


	Run run(state, main, roster);
	run.Play(steps);
	SpeechBroker::Scheduler::Get().Stop();

	const auto text = run.Report(scenarioName);
	if (!reportFile.empty()) {
		std::ofstream out(reportFile, std::ios::binary);
		out << text;
		spdlog::info("report: {}", reportFile.string());
	} else {
		std::fputs(text.c_str(), stdout);
	}

	if (withMarkets) {
		std::string order;
		for (const auto& one : marketOrder) {
			if (!order.empty()) {
				order += ", ";
			}
			order += one;
		}
		std::fputs("\n=== markets ===\n", stdout);
		std::printf("offering order: %s\n", order.c_str());
		for (const auto* witness : { &marketOpen, &marketAuction }) {
			std::printf("%s, %zu packet(s)\n", witness->id.c_str(), witness->seen.size());
			for (const auto& line : witness->seen) {
				std::printf("    %s\n", line.c_str());
			}
		}
	}

	return 0;
}
