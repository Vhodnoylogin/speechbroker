#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Envoy
{
	struct VocabularyMatch
	{
		std::string phrase;
		float       score{ 0.0f };
		float       margin{ 0.0f };

		// С какой уверенностью подписчик заявился бы на реплику, расслышанную
		// с такой оценкой. Произведение двух разных величин: насколько хорошо
		// реплику расслышали и насколько она похожа на объявленную фразу.
		// Одного совпадения со словарём мало: невнятно сказанная команда
		// совпадает с ним ровно так же, как чётко сказанная.
		//
		// Точного числа мост не знает - его считает сам подписчик. Но эта
		// оценка достаточно близка, чтобы понять, дошёл бы он до порога своего
		// класса, и ею пользуются двое: придержание, решая, кто в зале, и хост
		// проверки, ставя за подписчиков. Прежде формула жила у каждого своя.
		float Confidence(float a_utteranceScore) const { return score * a_utteranceScore; }
	};

	// Подписчик, который узнал фразу, вместе с тем, что он о себе объявил.
	//
	// Род объявляется при подписке, а не в ставке: решение придержать
	// принимается ДО того, как кто-либо успел заявиться, и опираться на ставки
	// в нём нельзя.
	struct Listener
	{
		std::string     ns;
		VocabularyMatch match;
		std::int32_t    costClass{ 0 };   // 0 - обратимое действие, 1 - дорогое
		// Умеет ли отменить сделанное. Отзывчивому можно отдавать раньше:
		// он способен исправиться, если фраза окажется незаконченной.
		bool            revocable{ false };
	};

	// Фраза словаря вместе со своей приведённой формой. Приведение делается
	// один раз, при объявлении словаря: прежде оно повторялось на каждый вопрос
	// о совпадении, то есть по разу на подписчика на каждую реплику.
	struct Phrase
	{
		std::string    text;        // как объявил мод - её и возвращаем наружу
		std::u32string normalized;  // по ней сравниваем
	};

	// Кто на что подписан и какие фразы объявил. Словарями владеет мост:
	// сопоставление делается здесь, на C++, чтобы не заставлять Papyrus
	// возиться со строками - он это делает плохо и медленно.
	class SubscriptionRegistry
	{
	public:
		static SubscriptionRegistry& Get();

		void Subscribe(const std::string& a_ns, std::vector<std::string> a_topics);
		// Что подписчик о себе объявил. Отдельным вызовом, а не параметрами
		// подписки: у того, кто звал Subscribe вчера, сегодня ничего
		// не меняется, а объявление можно уточнить, не переподписываясь.
		void Declare(const std::string& a_ns, std::int32_t a_costClass, bool a_revocable);
		void Unsubscribe(const std::string& a_ns);
		void SetActive(const std::string& a_ns, bool a_active);
		void SetVocabulary(const std::string& a_ns, std::vector<std::string> a_phrases);
		void ClearVocabulary(const std::string& a_ns);

		VocabularyMatch          Match(const std::string& a_ns, const std::string& a_text) const;
		// Кто в зале: подписчики этой темы, чьи словари узнали фразу.
		std::vector<Listener>    Audience(const std::string& a_topic,
			const std::string& a_text) const;
		std::vector<std::string> MergedVocabulary() const;
		// Кто объявился и на какие темы - чтобы участник мог показать это игроку.
		std::vector<std::string> Namespaces() const;
		std::vector<std::string> TopicsOf(const std::string& a_ns) const;
		std::size_t              Count() const;

	private:
		SubscriptionRegistry() = default;

		struct Entry
		{
			std::vector<std::string> topics;
			std::vector<Phrase>      vocabulary;
			bool                     active{ true };
			std::int32_t             costClass{ 0 };
			bool                     revocable{ false };
		};

		// Совпадение со словарём одной записи. Вынесено затем, что спрашивают
		// об этом двумя путями - об одном подписчике и обо всём зале, - и две
		// копии одного перебора успели бы разойтись.
		static VocabularyMatch BestOf(const Entry& a_entry, const std::u32string& a_text);

		// Доходит ли до этого подписчика оглашение такой темы.
		static bool Hears(const Entry& a_entry, const std::string& a_topic);

		mutable std::mutex                     _mutex;
		std::unordered_map<std::string, Entry> _entries;
	};
}
