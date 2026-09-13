#pragma once

#include "Utterance.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace Envoy
{
	// Реплики приходят из потока сервера, а читают их скрипты из главного потока,
	// поэтому всё под замком. Наружу отдаются копии: держать ссылку на запись,
	// которую в любой момент могут удалить по сроку, нельзя.
	class UtteranceStore
	{
	public:
		static UtteranceStore& Get();

		std::int32_t             Add(Utterance a_utterance);
		bool                     Update(std::int32_t a_id, const Utterance& a_utterance);
		// Уточнение от точной модели: текст и оценки заменяются, ставки и тема
		// сохраняются - реплика уже разослана и уже разыграна или разыгрывается.
		bool                     Refine(std::int32_t a_id, const Utterance& a_utterance);
		// Реплика отдаётся указателем на неизменяемый снимок, а не значением.
		// Прежде каждый вопрос о ней - текст, оценка, тема, совпадение, итог -
		// копировал её целиком вместе со ставками, победителями и картой
		// отказов; вопросов же на одну реплику приходится по десятку.
		std::shared_ptr<const Utterance> Find(std::int32_t a_id) const;

		bool AddBid(std::int32_t a_id, BidRecord a_bid);
		bool SetOutcome(std::int32_t a_id, std::vector<std::string> a_winners,
			std::map<std::string, std::string> a_denied, std::string a_outcome);

		void        Prune(double a_ttlSec, std::size_t a_maxStored);
		// Уборка по двум дешёвым условиям вместо сметания на каждую реплику:
		// либо накопилось больше предела, либо с прошлого раза прошла четверть
		// срока хранения. Обе проверки - за постоянное время.
		void        PruneIfDue(double a_ttlSec, std::size_t a_maxStored);
		std::size_t Count() const;

		bool  WonPrevious(const std::string& a_namespace) const;
		float SecondsSinceWin(const std::string& a_namespace) const;

	private:
		UtteranceStore() = default;

		// Копирование при записи: читатели держат снимок и живут с ним сколько
		// угодно, а писатель делает свою копию и подменяет указатель. Копия
		// выходит одна на запись вместо одной на каждый вопрос.
		template <class Fn>
		bool Mutate(std::int32_t a_id, Fn a_change)
		{
			auto it = _items.find(a_id);
			if (it == _items.end()) {
				return false;
			}
			auto copy = std::make_shared<Utterance>(*it->second);
			if (!a_change(*copy)) {
				return false;
			}
			it->second = std::move(copy);
			return true;
		}

		mutable std::mutex                                              _mutex;
		std::unordered_map<std::int32_t, std::shared_ptr<const Utterance>> _items;
		std::string                                    _lastAwardedTo;
		std::chrono::steady_clock::time_point          _lastAwardAt{};
		std::int32_t                                   _next{ 1 };
		// Когда убирались в прошлый раз - чтобы не сметать на каждую реплику.
		std::chrono::steady_clock::time_point           _lastPrune{};
	};
}
