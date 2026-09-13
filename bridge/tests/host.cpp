// Envoy Framework - прогон аукциона без игры.
//
//     envoy-host [--subscribers <папка>] [--scenario <файл>] [--config <файл>] [--report <файл>]
//
// Ядро здесь то же самое, что внутри игры: те же аукцион, хранилище реплик,
// выбор темы и словари. Разница ровно в трёх ответах, которые вместо Skyrim
// даёт этот хост:
//
//   работа        - складывается в очередь, которую главный поток вычерпывает сам;
//   состояние     - берётся из сценария, а не у движка;
//   события       - пишутся в журнал, а не рассылаются Papyrus.
//
// Подписчики здесь тоже не настоящие. От них нужны только объявленные данные -
// на какие темы подписаны и какой словарь заявили. Как подписчик поступит
// с выигрышем, проверку не занимает: вопрос в том, КОМУ мост отдал фрагмент
// и почему, а не в том, что победитель потом сделал.

#include "bus/Auction.h"
#include "bus/SubscriptionRegistry.h"
#include "bus/Utterance.h"
#include "bus/UtteranceStore.h"
#include "core/Config.h"
#include "core/GameState.h"
#include "core/Log.h"
#include "core/MainThread.h"
#include "core/Scheduler.h"
#include "core/Settings.h"

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
// Без этого windows.h объявляет макросы min и max, и любое std::min
// перестаёт разбираться - ошибка при этом указывает не на макрос.
#	define NOMINMAX
#	include <windows.h>
#endif

namespace
{
	namespace fs = std::filesystem;
	using json = nlohmann::json;

	// Состояние игры по сценарию.
	//
	// Вне игры её нет, но выкинуть состояние из проверки нельзя: тема выбирается
	// именно по нему, и «сказано в бою» против «сказано в мире» - это разные
	// наборы подписчиков, а не оттенок.
	class ScriptedState final : public Envoy::GameState::Source
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

	// Главный поток хоста.
	//
	// Без диспетчера ядро делает работу на месте вызова - но зовут его не
	// только отсюда. Потолок придержания срабатывает в потоке планировщика,
	// и «на месте» означало бы «в том потоке»: отпуск реплики шёл бы
	// параллельно с приёмом следующей и с нашими ставками. Два прогона одного
	// двоичного файла давали на одну реплику то 7 ставок, то 5 - смотря кто
	// успевал первым. Здесь работа складывается в очередь, а вычерпывает её
	// главный поток между своими шагами: порядок становится строго
	// определённым, и в ядре не остаётся ничего, что делалось бы из двух
	// потоков сразу.
	class MainQueue final : public Envoy::MainThread::Dispatcher
	{
	public:
		void Post(Envoy::MainThread::Task a_task) override
		{
			std::scoped_lock lock(_mutex);
			_queue.push_back(std::move(a_task));
		}

		// Выполнить всё накопленное. Задача вправе поставить следующую,
		// поэтому очередь забирается целиком, а замок на время работы отпущен.
		void Drain()
		{
			std::vector<Envoy::MainThread::Task> batch;
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
		std::vector<Envoy::MainThread::Task> _queue;
	};

	// Тестовый подписчик: только то, что он о себе объявил.
	struct TestSubscriber
	{
		std::string              ns;
		std::vector<std::string> topics;
		std::vector<std::string> vocabulary;
		std::int32_t             costClass{ 0 };   // 0 обратимое, 1 дорогое
		bool                     greedy{ false };
		// Умеет ли отменить сделанное. Отзывчивому мост отдаёт незаконченную
		// фразу сразу: он способен исправиться. Неотзывчивому - только когда
		// уверен, что фраза кончилась.
		bool                     revocable{ false };
	};

	std::string CostName(std::int32_t a_class)
	{
		return a_class == 1 ? "дорогое" : "обратимое";
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
			spdlog::error("не разобран {}: {}", a_path.string(), exc.what());
			return json{};
		}
	}

	// Состав участников: кто объявился и что о себе сказал.
	class Roster
	{
	public:
		bool Load(const fs::path& a_dir)
		{
			if (!fs::exists(a_dir)) {
				spdlog::error("нет папки подписчиков: {}", a_dir.string());
				return false;
			}

			std::vector<fs::path> files;
			for (const auto& entry : fs::directory_iterator(a_dir)) {
				if (entry.is_regular_file() && entry.path().extension() == ".json") {
					files.push_back(entry.path());
				}
			}
			// По имени файла: порядок объявления не должен зависеть от того,
			// как файловая система решила их отдать.
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

		// Объявить всех мосту - так же, как это сделал бы скрипт каждого.
		void Declare() const
		{
			auto& registry = Envoy::SubscriptionRegistry::Get();
			for (const auto& sub : _subs) {
				registry.Subscribe(sub.ns, sub.topics);
				// Род объявляется отдельно от подписки: решение придержать реплику
				// принимается ДО того, как кто-либо успел заявиться, и опираться
				// на ставки в нём нельзя.
				registry.Declare(sub.ns, sub.costClass, sub.revocable);
				if (!sub.vocabulary.empty()) {
					registry.SetVocabulary(sub.ns, sub.vocabulary);
				}

				std::string topics;
				for (const auto& topic : sub.topics) {
					topics += topics.empty() ? topic : ", " + topic;
				}
				spdlog::info("подписчик {}: темы [{}], фраз {}, {}{}{}", sub.ns, topics,
					sub.vocabulary.size(), CostName(sub.costClass),
					sub.greedy ? ", жадный" : "",
					sub.revocable ? ", отзывчивый" : "");
			}
		}

		// Жадность реестру не объявляют - это свойство ставки, а не подписки, -
		// поэтому хост берёт её из своего описания подписчика.
		bool Greedy(const std::string& a_ns) const
		{
			const auto it = std::find_if(_subs.begin(), _subs.end(),
				[&](const TestSubscriber& s) { return s.ns == a_ns; });
			return it != _subs.end() && it->greedy;
		}

	private:
		std::vector<TestSubscriber> _subs;
	};

	// Прогон сценария: шаги, ставки за подписчиков, ожидание придержанных
	// и отчёт по каждой реплике.
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

			// Сценарий кончился, но потолки придержанных могли ещё не истечь. Ждём их
			// и дописываем итог: реплика, отпущенная последней, должна быть в отчёте
			// так же, как все прочие.
			if (!_waiting.empty()) {
				spdlog::info("ждём {} придержанных реплик", _waiting.size());
				// Сколько ждать, говорят настройки: дольше самого длинного потолка
				// придержание не длится, дальше - окно ставок и запас на очередь.
				const auto& settings = Envoy::Settings::Get();
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
			report << "=== прогон аукциона без игры ===\n\n";
			report << "сценарий: " << a_scenarioName << "\n";
			report << "окно ставок: " << Envoy::Settings::Get().bidWindowMs << " мс\n\n";

			// Теперь, когда решилось всё, дочитываем итоги из снимка.
			std::size_t heldCount = 0, droppedCount = 0;
			for (const auto& told : _history) {
				const auto found = _snapshot.find(told.id);
				const Envoy::Utterance* done = found == _snapshot.end() ? nullptr : &found->second;

				report << "реплика " << told.id << ": «" << (done ? done->text : std::string{}) << "»\n";
				if (!told.source.empty()) {
					report << "    запись    : " << told.source << "  (эталон «"
					       << told.reference << "»)\n";
				}
				if (told.hadComplete) {
					report << "    кусок     : " << told.sliceId << ", завершённость "
					       << told.complete << "\n";
				}
				for (const auto& line : told.swallowed) {
					report << "    поглощает : " << line << "\n";
				}
				report << "    тема      : " << told.topic << "\n";

				if (done && !done->holdReason.empty()) {
					++heldCount;
					report << "    придержана: " << done->holdReason << "\n";
					if (done->supersededBy != 0) {
						++droppedCount;
						report << "    ВЫБРОШЕНА : не оглашалась, её поглотила реплика "
						       << done->supersededBy << "\n";
					}
				}

				if (done) {
					report << "    ставок    : " << done->bids.size() << "\n";
					for (const auto& bid : done->bids) {
						report << "        " << bid.ns << "  " << bid.confidence
						       << "  " << CostName(bid.costClass)
						       << (bid.greedy ? ", жадный" : "")
						       << "  фраза «" << bid.phrase << "»\n";
					}
					std::string winners;
					for (const auto& who : done->winners) {
						winners += winners.empty() ? who : ", " + who;
					}
					report << "    выиграл   : " << (winners.empty() ? "никто" : winners) << "\n";
					for (const auto& [who, why] : done->denied) {
						report << "    отказано  : " << who << " - " << why << "\n";
					}
					report << "    итог      : " << done->outcome << "\n";
				}
				report << "\n";
			}

			report << "=== придержание ===\n";
			report << "придержано реплик: " << heldCount << " из " << _history.size() << "\n";
			report << "из них выброшено не оглашёнными: " << droppedCount
			       << " - столько раз обрывок фразы НЕ ушёл никому\n\n";
			return report.str();
		}

	private:
		// Что известно о реплике только на её шаге. Итог дочитывается из хранилища
		// в конце: у придержанной он появляется позже, чем шаг заканчивается.
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

		// Ожидание идёт долями, и между долями главный поток делает две вещи:
		// вычерпывает очередь ядра и торгует за отпущенных. Доля - это шаг
		// опоздания на торги, а не настройка: окно ставок открывается в тот миг,
		// когда придержанную отпустили, и проверка не должна его проспать.
		static constexpr std::int64_t kPollSliceMs = 50;
		// Запас на то, что итог ставится в очередь из потока планировщика
		// и до следующей доли не виден. Несколько долей с избытком.
		static constexpr std::int64_t kQueueSlackMs = 250;

		void Step(const json& a_step)
		{
			_state.Reset();
			// Проверяем именно объект, а не наличие ключа: сценарий, собранный
			// программой, вполне может положить туда пустоту, и разбирать её как
			// объект значит уронить весь прогон на одном шаге.
			if (a_step.contains("state") && a_step["state"].is_object()) {
				const auto& s = a_step["state"];
				_state.menu = s.value("menu", std::string{});
				_state.paused = s.value("paused", false);
				_state.combat = s.value("combat", false);
			}

			// Номер куска у движка - свой в каждой записи, номер реплики - сквозной.
			// Связь между ними нужна, чтобы длинный кусок мог сказать, какие короткие
			// он поглощает: именно здесь и видно, успел ли мост отдать команду до того,
			// как выяснилось, что фраза ещё не кончилась.
			const auto source = a_step.value("source", std::string{});
			if (source != _currentSource) {
				_currentSource = source;
				_sliceToId.clear();
				_lastEmitMs = 0;
			}

			// Куски одной записи проигрываются по меткам времени, а не подряд.
			// Иначе продолжение приходит мгновенно, и придержание выглядит
			// работающим там, где в жизни успел бы истечь потолок.
			const auto emitMs = a_step.value("emitMs", 0);
			if (emitMs > _lastEmitMs) {
				Wait(emitMs - _lastEmitMs);
				_lastEmitMs = emitMs;
			}

			Envoy::Utterance utterance;
			utterance.text = a_step.value("text", std::string{});
			utterance.score = a_step.value("score", 0.9f);
			utterance.margin = a_step.value("margin", 0.3f);
			utterance.channel = a_step.value("channel", std::string{});
			utterance.language = a_step.value("language", std::string{ "ru" });
			utterance.engine = a_step.value("engine", std::string{ "host" });
			utterance.lengthClass = a_step.value("lengthClass", 0);
			utterance.sliceId = a_step.value("sliceId", 0);
			utterance.durationMs = a_step.value("durationMs", 0);
			// Единица по умолчанию: сценарий, не знающий о завершённости, ведёт
			// себя как прежде - всё приходит законченным и ничего не держится.
			utterance.complete = a_step.value("complete", 1.0f);
			utterance.isFinal = true;

			// Прочие гипотезы движка. Аукцион их пока не спрашивает, но реплика
			// обязана нести их целиком: подписчик вправе увидеть, что фразу можно
			// понять иначе, а мост не вправе решать это за него.
			if (a_step.contains("alternatives")) {
				for (const auto& alt : a_step["alternatives"]) {
					utterance.alternatives.push_back(Envoy::Alternative{
						alt.value("text", std::string{}), alt.value("score", 0.0f) });
				}
			}

			const auto id = Envoy::UtteranceStore::Get().Add(utterance);
			if (utterance.sliceId != 0) {
				_sliceToId[utterance.sliceId] = id;
			}

			// Длинный кусок поглощает короткие, из которых он собран. Решает это
			// мост, а не хост: придержанные он выбросит не оглашёнными,
			// а уже отданные - отзовёт.
			std::vector<std::int32_t> older;
			if (a_step.contains("supersedes")) {
				for (const auto& mark : a_step["supersedes"]) {
					const auto found = _sliceToId.find(mark.get<int>());
					if (found != _sliceToId.end()) {
						older.push_back(found->second);
					}
				}
			}

			spdlog::info("--- реплика {}: «{}» (завершённость {:.2f}) ---", id, utterance.text,
				utterance.complete);

			std::vector<std::string> swallowed;
			for (const auto mark : older) {
				auto was = Envoy::UtteranceStore::Get().Find(mark);
				if (!was) {
					continue;
				}
				std::string who;
				for (const auto& winner : was->winners) {
					who += who.empty() ? winner : ", " + winner;
				}
				swallowed.push_back("реплика " + std::to_string(mark) +
					(was->held ? " - придержана, выброшена не оглашённой"
					           : (who.empty() ? " - её никто не получил"
					                          : " - НО ОНА УЖЕ ОТДАНА: " + who)));
			}
			if (!older.empty()) {
				Envoy::Auctioneer::Get().Supersede(id, older);
			}

			// Приём вместо оглашения. Мост сам решит, огласить реплику сейчас или
			// придержать, пока не станет ясно, кончилась ли фраза.
			Envoy::Auctioneer::Get().Receive(id);

			auto offered = Envoy::UtteranceStore::Get().Find(id);
			const std::string topic = offered ? offered->topic : std::string{ "?" };
			const bool held = offered && offered->held;

			if (held) {
				// Придержанную не торгуем и не ждём: в жизни движок в это время
				// продолжает работать, и продолжение может прийти раньше, чем
				// истечёт потолок. Ставки за неё поставим, если её всё-таки отпустят.
				_waiting.push_back(id);
				spdlog::info("реплика {} придержана - ставки не собираем", id);
			} else {
				PlaceBids(id, utterance.text, utterance.score, topic);
				// Итог подводит сам аукционист по истечении окна ставок, из потока
				// планировщика. Ждём его, а не подводим за него.
				Wait(Envoy::Settings::Get().bidWindowMs + kQueueSlackMs);
			}

			// Итог сюда не пишем. Придержанная реплика решится позже - когда её
			// поглотят или отпустят по потолку, - и снимок, сделанный сейчас,
			// показал бы её вечно нерешённой. Всё, что известно только на этом
			// шаге, запоминаем; остальное дочитаем из хранилища в конце.
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

		// Ставки вместо скриптов Papyrus. Кто в зале - подписчики темы, чьи словари
		// узнали фразу, - отвечает сам реестр, тем же вопросом, которым пользуется
		// придержание; своей копии правила «слышит ли подписчик тему» у хоста нет.
		// Уверенность - та же оценка, по которой придержание судит, дошёл бы
		// подписчик до порога. Настоящий подписчик считает её сам; здесь взято
		// простейшее защитимое правило, потому что поведение подписчиков проверку
		// не занимает.
		void PlaceBids(std::int32_t a_id, const std::string& a_text, float a_score,
			const std::string& a_topic) const
		{
			const auto heard = Envoy::SubscriptionRegistry::Get().Audience(a_topic, a_text);
			for (const auto& who : heard) {
				const bool  greedy = _roster.Greedy(who.ns);
				const float confidence = who.match.Confidence(a_score);
				Envoy::UtteranceStore::Get().AddBid(a_id,
					Envoy::BidRecord{ who.ns, confidence, who.costClass, greedy, who.match.phrase });
				spdlog::info("ставка {}: уверенность {:.2f} (слышимость {:.2f} x словарь {:.2f}), "
				             "фраза «{}», {}{}",
					who.ns, confidence, a_score, who.match.score, who.match.phrase,
					CostName(who.costClass), greedy ? ", жадный" : "");
			}
			if (heard.empty()) {
				spdlog::info("ставок нет - никто из подписчиков темы {} не узнал фразу", a_topic);
			}
		}

		// Придержанную реплику могли отпустить по потолку, пока мы занимались
		// следующим куском. Оглашение случилось - значит пора торговать.
		void CatchUp()
		{
			std::vector<std::int32_t> still;
			for (const auto id : _waiting) {
				auto item = Envoy::UtteranceStore::Get().Find(id);
				if (!item || item->supersededBy != 0) {
					continue;   // поглощена - судьба решена, торговать нечего
				}
				if (item->held) {
					still.push_back(id);
					continue;
				}
				spdlog::info("реплика {} отпущена - торгуем с опозданием", id);
				PlaceBids(id, item->text, item->score, item->topic);
			}
			_waiting.swap(still);
		}

		// Последнее известное состояние каждой реплики.
		//
		// Хранилище моста - живой кеш, а не журнал: оно чистится по сроку и по
		// числу, и к концу прогона ранние реплики из него уже вымыты. Это верно
		// для моста и неверно для проверки, которой нужен итог по КАЖДОЙ. Поэтому
		// снимок держим у себя, а не требуем от хранилища быть тем, чем оно не является.
		void Keep()
		{
			for (const auto& told : _history) {
				if (auto item = Envoy::UtteranceStore::Get().Find(told.id)) {
					_snapshot[told.id] = *item;
				}
			}
		}

		// Ждать надо не одним сном, а короткими долями с работой между ними:
		// вычерпать очередь ядра, поторговать за отпущенных, обновить снимок.
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
		std::vector<std::int32_t>   _waiting;   // придержанные, чьей судьбы мы ещё не знаем
		std::vector<Told>           _history;
		std::map<std::int32_t, Envoy::Utterance> _snapshot;
	};
}

int main(int argc, char** argv)
{
#ifdef _WIN32
	::SetConsoleOutputCP(CP_UTF8);
#endif

	// Файл настроек хост заводит себе сам: встроенный эталон разворачивается
	// рядом с исполняемым, и правила аукциона получаются ровно те же, что
	// в игре, - без единого пути, заданного здесь. Именно рядом с исполняемым,
	// а не в текущей папке: запуск из корня репозитория оставлял файл в нём.
	fs::path subscribersDir = ENVOY_TEST_DIR "/subscribers";
	fs::path scenarioFile = ENVOY_TEST_DIR "/scenarios/default.json";
	fs::path configFile = fs::absolute(argv[0]).parent_path() / "envoy-host.json";
	fs::path reportFile;

	for (int i = 1; i + 1 < argc; ++i) {
		const std::string key = argv[i];
		if (key == "--subscribers") {
			subscribersDir = argv[++i];
		} else if (key == "--scenario") {
			scenarioFile = argv[++i];
		} else if (key == "--config") {
			configFile = argv[++i];
		} else if (key == "--report") {
			reportFile = argv[++i];
		}
	}

	Envoy::Log::ToConsole("info");

	auto& config = Envoy::Config::Get();
	config.Load(configFile);
	spdlog::info("{}: {}", Envoy::Config::Describe(config.Source()), config.Path().string());

	// Два из трёх швов ядра отвечает хост; события остаются в журнале.
	static ScriptedState state;
	static MainQueue     main;
	Envoy::GameState::Install(&state);
	Envoy::MainThread::Install(&main);

	Roster roster;
	if (!roster.Load(subscribersDir)) {
		spdlog::error("ни одного подписчика - проверять нечего");
		return 2;
	}
	roster.Declare();

	const auto scenario = ReadJson(scenarioFile);
	const auto steps = scenario.contains("steps") ? scenario["steps"] : json::array();
	if (steps.empty()) {
		spdlog::error("в сценарии нет шагов: {}", scenarioFile.string());
		return 2;
	}
	const auto scenarioName = scenario.value("name", scenarioFile.stem().string());
	spdlog::info("сценарий «{}», шагов {}", scenarioName, steps.size());

	Run run(state, main, roster);
	run.Play(steps);
	Envoy::Scheduler::Get().Stop();

	const auto text = run.Report(scenarioName);
	if (!reportFile.empty()) {
		std::ofstream out(reportFile, std::ios::binary);
		out << text;
		spdlog::info("отчёт: {}", reportFile.string());
	} else {
		std::fputs(text.c_str(), stdout);
	}
	return 0;
}
