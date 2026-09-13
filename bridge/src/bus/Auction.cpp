#include "Auction.h"

#include "TopicRouter.h"
#include "UtteranceStore.h"
#include "Hold.h"
#include "core/Events.h"
#include "core/MainThread.h"
#include "core/Scheduler.h"
#include "core/Settings.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace Envoy
{
	Auction::Result Auction::Share(const std::vector<BidRecord>& a_survivors,
		const BidRecord& a_top) const
	{
		Result result;

		const char* stranger = a_top.phrase.empty()
		                           ? "команда победителя неизвестна - награда не делится"
		                           : "узнал другую команду - эта не его";

		for (const auto& bid : a_survivors) {
			const bool same = bid.ns == a_top.ns ||
			                  (!a_top.phrase.empty() && bid.phrase == a_top.phrase);
			if (!same) {
				result.denied[bid.ns] = stranger;
			} else if (bid.greedy) {
				result.denied[bid.ns] = "проиграл, а делиться отказался";
			} else {
				result.winners.push_back(bid.ns);
			}
		}
		return result;
	}

	Auction::Result Auction::Exclusive(const std::string& a_ns, const std::string& a_reason,
		const std::vector<BidRecord>& a_scope) const
	{
		Result result;
		result.winners.push_back(a_ns);
		result.reason = a_reason;
		for (const auto& bid : a_scope) {
			if (bid.ns != a_ns) {
				result.denied[bid.ns] = a_reason;
			}
		}
		return result;
	}

	std::vector<BidRecord> Auction::Survivors(Result& a_result) const
	{
		std::vector<BidRecord> survivors;
		for (const auto& bid : _utterance.bids) {
			if (bid.confidence >= Settings::Get().MinConfidence(bid.costClass)) {
				survivors.push_back(bid);
			} else {
				a_result.denied[bid.ns] = "уверенность ниже порога своего класса";
			}
		}
		return survivors;
	}

	std::vector<BidRecord> Auction::Tied(const std::vector<BidRecord>& a_survivors, float a_need) const
	{
		std::vector<BidRecord> tied;
		for (const auto& bid : a_survivors) {
			if (a_survivors.front().confidence - bid.confidence < a_need) {
				tied.push_back(bid);
			}
		}
		return tied;
	}

	bool Auction::SameCommand(const std::vector<BidRecord>& a_tied) const
	{
		if (a_tied.front().phrase.empty()) {
			return false;
		}
		for (const auto& bid : a_tied) {
			if (bid.phrase != a_tied.front().phrase) {
				return false;
			}
		}
		return true;
	}

	Auction::Result Auction::BreakTie(const std::vector<BidRecord>& a_survivors, float a_need) const
	{
		Result     result;
		const auto tied = Tied(a_survivors, a_need);

		// Порядок из настроек - прямое указание игрока, и оно старше любых наших
		// рассуждений: если он назвал, кто здесь главный, спорить не о чем.
		auto             best = Settings::kNoPriority;
		std::size_t      count = 0;
		const BidRecord* chosen = nullptr;
		for (const auto& bid : tied) {
			const auto place = Settings::Get().PriorityIndex(bid.ns);
			if (place < best) {
				best = place;
				count = 1;
				chosen = &bid;
			} else if (place == best) {
				++count;
			}
		}
		if (best != Settings::kNoPriority && count == 1) {
			return Exclusive(chosen->ns, "ставки неразличимы, спор решён порядком из настроек",
				a_survivors);
		}

		// Порядок молчит. Дальше всё зависит от того, об одном ли спор. Разные
		// команды при неразличимой уверенности - это двусмысленная реплика:
		// понять её можно двояко, и оба понимания равно правдоподобны. Сделать
		// по ней хоть что-нибудь значит угадывать.
		if (!SameCommand(tied)) {
			result.reason = "фразу поняли по-разному и одинаково уверенно - реплика двусмысленна";
			for (const auto& bid : a_survivors) {
				result.denied[bid.ns] = result.reason;
			}
			return result;
		}

		// Двусмысленности нет: все узнали одну и ту же команду, и спор идёт не
		// о том, что сказано, а о том, чья это команда. Уверенность его не решит
		// никогда, поэтому решает объявленная готовность делиться. Жадный при
		// этом ничего не теряет: он сам объявил "мне одному или никак" и при
		// чужой победе выбывает по собственному условию.
		if (Settings::Get().sharedWinsTie) {
			// Спорщики уже проверены на одну команду выше, но раздача идёт
			// по всем выжившим: отставший на целый запас тоже может узнать
			// ту же фразу, и отказывать ему не за что. А узнавший другую
			// не получит награду, даже если порог прошёл.
			auto shared = Share(a_survivors, tied.front());
			if (!shared.winners.empty()) {
				shared.reason = "одну команду просят несколько, порядок не задан - её делают те, кто делится";
				return shared;
			}
		}

		result.reason = "одну команду просят несколько, и все требуют её себе - не делает никто";
		for (const auto& bid : a_survivors) {
			result.denied[bid.ns] = result.reason;
		}
		return result;
	}

	Auction::Result Auction::Decide() const
	{
		Result result;

		if (_utterance.score < Settings::Get().minUtteranceScore) {
			result.reason = "реплика расслышана хуже порога";
			for (const auto& bid : _utterance.bids) {
				result.denied[bid.ns] = result.reason;
			}
			return result;
		}

		// Отсутствие ставок и провал ставок - разные вещи, и сводить их к одной
		// строке журнала значит лгать в диагностике: в прогоне 04.09 такую
		// строку получили 34 реплики из 36, и ни у одной ставок не было.
		if (_utterance.bids.empty()) {
			result.reason = "никто не заявился";
			return result;
		}

		auto survivors = Survivors(result);
		if (survivors.empty()) {
			result.reason = "ни одна ставка не прошла порог уверенности";
			return result;
		}

		std::sort(survivors.begin(), survivors.end(), [](const BidRecord& a, const BidRecord& b) {
			if (a.confidence != b.confidence) {
				return a.confidence > b.confidence;
			}
			const auto pa = Settings::Get().PriorityIndex(a.ns);
			const auto pb = Settings::Get().PriorityIndex(b.ns);
			return pa != pb ? pa < pb : a.ns < b.ns;
		});

		const auto top = survivors.front();

		// Дальше спорят только выжившие, и вердикт касается только их.
		Result verdict;
		const auto need = Settings::Get().MinMargin(top.costClass);
		if (survivors.size() > 1 && top.confidence - survivors[1].confidence < need) {
			verdict = BreakTie(survivors, need);
		} else if (top.greedy) {
			verdict = Exclusive(top.ns, "победитель жадный - результат только ему", survivors);
		} else {
			verdict = Share(survivors, top);
			verdict.reason = verdict.winners.size() > 1
			                     ? "победитель делится - результат достался всем, кто узнал ту же команду"
			                     : "победитель делится, делить не с кем";
		}

		// Выбывшие по порогу в круг спора не входили, и в вердикте их нет:
		// их причина записана раньше и просто добавляется. Так слияние одно
		// на все исходы; прежде их было два с разной семантикой, и в ветке
		// порядка выбывший по порогу получал чужую причину.
		verdict.denied.insert(result.denied.begin(), result.denied.end());
		return verdict;
	}

	Auctioneer& Auctioneer::Get()
	{
		static Auctioneer instance;
		return instance;
	}

	void Auctioneer::Receive(std::int32_t a_id)
	{
		auto stored = UtteranceStore::Get().Find(a_id);
		if (!stored) {
			return;
		}

		// Тема выбирается ДО решения о придержании: зал зависит от неё, и
		// сказанное в бою слышат не те, кто слышит сказанное в мире.
		//
		// И выбирается один раз. Тема принадлежит моменту, когда фраза была
		// сказана, а не моменту оглашения: придержанную реплику отпускают через
		// секунды, когда бой мог кончиться или открыться меню. Прежде Offer
		// выбирал тему заново, и в живом прогоне 07.09 «фаербол», сказанный
		// в бою и придержанный по залу боя, ушёл событием Envoy_Speech_World -
		// придержание считало риск по одному залу, оглашение шло другому.
		auto item = *stored;
		item.topic = TopicRouter::Pick(item);
		UtteranceStore::Get().Update(a_id, item);

		const auto verdict = Hold::Judge(item);
		if (!verdict.hold) {
			if (verdict.audience > 0) {
				spdlog::info("реплика {} отдаётся сразу: {}", a_id, verdict.reason);
			}
			Offer(a_id);
			return;
		}

		item.held = true;
		item.holdReason = verdict.reason;
		UtteranceStore::Get().Update(a_id, item);
		spdlog::info("реплика {} ПРИДЕРЖАНА: {}", a_id, verdict.reason);

		// Потолок - не главный путь, а страховка. Обычно удержание кончается
		// раньше: либо приходит продолжение и обрывок выбрасывается вовсе,
		// либо человек замолкает, и движок сам присылает законченную реплику.
		if (verdict.ceilingMs > 0) {
			Scheduler::Get().After(std::chrono::milliseconds(verdict.ceilingMs), [a_id]() {
				MainThread::Post([a_id]() {
					Auctioneer::Get().Release(a_id, "истёк потолок класса длины");
				});
			});
		}
	}

	void Auctioneer::Release(std::int32_t a_id, const std::string& a_why)
	{
		auto stored = UtteranceStore::Get().Find(a_id);
		if (!stored || !stored->held) {
			return;
		}
		if (stored->supersededBy != 0) {
			// Продолжение успело прийти: обрывок больше не разыгрывается.
			return;
		}

		auto item = *stored;
		item.held = false;
		UtteranceStore::Get().Update(a_id, item);
		spdlog::info("реплика {} отпущена: {}", a_id, a_why);
		Offer(a_id);
	}

	void Auctioneer::Supersede(std::int32_t a_newId, const std::vector<std::int32_t>& a_older)
	{
		for (const auto older : a_older) {
			auto stored = UtteranceStore::Get().Find(older);
			if (!stored || stored->supersededBy != 0) {
				continue;
			}

			auto item = *stored;
			item.supersededBy = a_newId;
			const bool wasHeld = item.held;
			item.held = false;
			UtteranceStore::Get().Update(older, item);

			if (wasHeld) {
				// Придержали и не прогадали: фраза продолжилась, а обрывок
				// так и не ушёл никуда. Ради этого случая всё и делалось.
				spdlog::info("реплика {} выброшена не оглашённой: её поглотила {}",
					older, a_newId);
				continue;
			}

			if (!item.winners.empty()) {
				// Успели отдать. Отменить сделанное мост не может - он не знает,
				// что именно подписчик сделал, - но обязан сказать. Знает, как
				// исправиться, только сам победитель.
				std::string who;
				for (const auto& winner : item.winners) {
					who += who.empty() ? winner : ", " + winner;
				}
				spdlog::warn("реплика {} была отдана ({}) и поглощена репликой {} - отзыв",
					older, who, a_newId);
				Events::Send("Envoy_Revoked", "", static_cast<float>(older));
			}
		}
	}

	void Auctioneer::Offer(std::int32_t a_id)
	{
		auto stored = UtteranceStore::Get().Find(a_id);
		if (!stored) {
			return;
		}

		// Тема уже выбрана при приёме и здесь не пересматривается: см. Receive.
		auto item = *stored;
		item.offeredAt = std::chrono::steady_clock::now();
		UtteranceStore::Get().Update(a_id, item);

		// Уборка идёт здесь же: чаще реплик в хранилище ничего не происходит,
		// а отдельный поток-уборщик пришлось бы ещё и останавливать при выходе.
		UtteranceStore::Get().PruneIfDue(Settings::Get().utteranceTtlSec,
			Settings::Get().utteranceMaxStored);

		// Наблюдатели видят каждую реплику независимо от темы - именно так мод
		// может показать, что до него что-то не дошло и почему.
		// Событие - звонок в дверь: в нём только номер реплики. Тему подписчик
		// узнаёт по имени события, а для Envoy_Speech_Any - вызовом GetTopic.
		// Текст всегда берётся из моста: точная модель может уточнить его уже
		// после рассылки, и копия в событии разошлась бы с истиной.
		Events::Send("Envoy_Speech_Any", "", static_cast<float>(a_id));
		Events::Send(TopicRouter::EventName(item.topic), "", static_cast<float>(a_id));

		Scheduler::Get().After(std::chrono::milliseconds(Settings::Get().bidWindowMs), [a_id]() {
			MainThread::Post([a_id]() { Auctioneer::Get().Settle(a_id); });
		});
	}

	void Auctioneer::Settle(std::int32_t a_id)
	{
		auto stored = UtteranceStore::Get().Find(a_id);
		if (!stored || stored->awarded) {
			return;
		}

		const auto result = Auction{ *stored }.Decide();

		std::string outcome;
		for (const auto& winner : result.winners) {
			outcome += outcome.empty() ? winner : ", " + winner;
		}
		if (outcome.empty()) {
			outcome = "никто";
		}

		UtteranceStore::Get().SetOutcome(a_id, result.winners, result.denied,
			outcome + " - " + result.reason);

		spdlog::info("реплика {} тема {} ставок {} -> {} ({})", a_id, stored->topic,
			stored->bids.size(), outcome, result.reason);

		// По одной рассылке на исход, а не на получателя: имени в событии больше
		// нет, и каждый участник сам спрашивает IsWinner или GetDenyReason.
		// Три имени сохранены не ради содержимого - оно у всех одно, - а ради
		// условия: Envoy_Award молчит, когда никто не выиграл, Envoy_Denied -
		// когда никому не отказано, а Envoy_Settled звучит всегда.
		if (!result.winners.empty()) {
			Events::Send("Envoy_Award", "", static_cast<float>(a_id));
		}
		if (!result.denied.empty()) {
			Events::Send("Envoy_Denied", "", static_cast<float>(a_id));
		}
		Events::Send("Envoy_Settled", "", static_cast<float>(a_id));
	}
}
