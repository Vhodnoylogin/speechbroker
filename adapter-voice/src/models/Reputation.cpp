#include "models/Reputation.h"

// THE ONE PLACE IN THIS FILE THE CONTRACT APPEARS, AND IT COMPILES TO NOTHING.
//
// Reputation.h says out loud that nothing in it includes the model header, so
// that a standing can be exercised with a table of numbers and no game anywhere
// near it. Speed is a restatement of SpeechBrokerVoiceClass, and a restatement
// is a copy that can drift: the two static_asserts below are the whole reason
// the include is here, and they are the only use of it in this translation unit.
#include "../../contract/speechbroker-voice-model.h"

#include "Loc.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace Voice::Models
{
	static_assert(static_cast<std::int32_t>(Speed::Fast) == SPEECHBROKERVOICE_CLASS_FAST,
		"Speed::Fast has drifted away from the contract's SPEECHBROKERVOICE_CLASS_FAST");
	static_assert(static_cast<std::int32_t>(Speed::Accurate) == SPEECHBROKERVOICE_CLASS_ACCURATE,
		"Speed::Accurate has drifted away from the contract's SPEECHBROKERVOICE_CLASS_ACCURATE");

	namespace
	{
		// EVERY PUBLIC ENTRY TAKES THE RECORD'S LOCK, SO NO PUBLIC ENTRY MAY CALL
		// ANOTHER. std::mutex is not recursive, and MeasuredClass needs a latency,
		// Weight needs an invention rate and Report needs all of them. The
		// arithmetic therefore lives in free functions over plain numbers, which
		// are called with the lock already held - and which, being free functions
		// over numbers, are also the honest statement of what this file is.

		// engine/reputation.py:83-87. The percentile index of the reference, with
		// its two guards kept: the empty list falls back to what the model
		// DECLARED, and the index is pulled inside the array rather than trusted.
		std::int32_t Percentile(const std::vector<std::int32_t>& a_sorted, double a_percentile,
			std::int32_t a_fallback)
		{
			if (a_sorted.empty()) {
				return a_fallback;
			}

			const double scaled = static_cast<double>(a_sorted.size()) * a_percentile;

			// int() in Python truncates towards zero and a negative percentile is a
			// typo in somebody's settings, not a reason to index backwards.
			std::size_t index = (std::isfinite(scaled) && scaled > 0.0) ?
				static_cast<std::size_t>(scaled) :
				0U;
			if (index >= a_sorted.size()) {
				index = a_sorted.size() - 1U;  // engine/reputation.py:87, min(n - 1, ...)
			}
			return a_sorted[index];
		}

		// A sorted COPY, as the reference took one (engine/reputation.py:86): the
		// ring is kept in arrival order because the oldest observation has to be
		// the one that falls off the end, and sorting it in place would destroy
		// exactly that.
		std::int32_t PercentileOf(const std::vector<std::int32_t>& a_ring, double a_percentile,
			std::int32_t a_fallback)
		{
			if (a_ring.empty()) {
				return a_fallback;
			}
			std::vector<std::int32_t> ordered(a_ring);
			std::sort(ordered.begin(), ordered.end());
			return Percentile(ordered, a_percentile, a_fallback);
		}

		// engine/reputation.py:95-96. No probe is not a rate of zero in any
		// meaningful sense, but zero is what the reference returned and what
		// Weight() then multiplies by, so it stays zero.
		double InventionOf(std::uint64_t a_probes, std::uint64_t a_inventions) noexcept
		{
			return a_probes ? static_cast<double>(a_inventions) / static_cast<double>(a_probes) : 0.0;
		}

		// engine/reputation.py:104-127. The pool is the live scores once there are
		// enough of them and the calibration otherwise, and BELOW minSample THERE
		// IS NO NORMALISATION AT ALL - the raw number goes on as it came, because
		// the adapter must not claim a ranking it cannot compute.
		float NormalizeIn(const std::vector<float>& a_live, const std::vector<float>& a_calibration,
			int a_minSample, float a_raw)
		{
			const std::size_t need = a_minSample > 0 ? static_cast<std::size_t>(a_minSample) : 0U;
			const std::vector<float>& pool = a_live.size() >= need ? a_live : a_calibration;
			if (pool.size() < need || pool.empty()) {
				return a_raw;
			}

			std::size_t below = 0;
			for (const float score : pool) {
				if (score <= a_raw) {
					++below;
				}
			}
			return static_cast<float>(static_cast<double>(below) / static_cast<double>(pool.size()));
		}

		// Trim a ring to the last `keep` observations. The reference wrote it as
		// `(ring + [new])[-KEEP:]`, which rebuilt the whole list every time; the
		// erase from the front is the same result and does not allocate.
		template <class T>
		void Trim(std::vector<T>& a_ring, int a_keep)
		{
			const std::size_t keep = a_keep > 0 ? static_cast<std::size_t>(a_keep) : 0U;
			if (a_ring.size() > keep) {
				a_ring.erase(a_ring.begin(),
					a_ring.begin() + static_cast<std::ptrdiff_t>(a_ring.size() - keep));
			}
		}

		// What the calibration file spells a class as. The word is the reference's
		// (engine/reputation.py:198 reads `declaredClass` back as it saved it), and
		// an unknown word leaves the declaration alone rather than guessing.
		bool SpeedFromWord(const std::string& a_word, Speed& a_out) noexcept
		{
			if (a_word == "fast") {
				a_out = Speed::Fast;
				return true;
			}
			if (a_word == "accurate") {
				a_out = Speed::Accurate;
				return true;
			}
			return false;
		}
	}

	// ---------------------------------------------------------------- Reputation

	Reputation::Reputation(std::string a_id, const ReputationSettings& a_settings) :
		_id(std::move(a_id)),
		_settings(a_settings)
	{
		// The rings never grow past `keep`, so the whole memory of a standing is
		// bought once here rather than a few bytes at a time under the lock while
		// somebody is speaking.
		if (_settings.keep > 0) {
			_latencies.reserve(static_cast<std::size_t>(_settings.keep));
			_scores.reserve(static_cast<std::size_t>(_settings.keep));
		}
	}

	void Reputation::Declare(Speed a_class, std::uint32_t a_budgetMs)
	{
		const std::lock_guard hold(_lock);
		_declaredClass = a_class;
		_declaredBudgetMs = a_budgetMs;
	}

	void Reputation::NoteCall(std::int32_t a_latencyMs, const std::vector<float>& a_scores)
	{
		const std::lock_guard hold(_lock);

		// engine/reputation.py:60-63. One call, one latency, and EVERY fragment's
		// score - the distribution being built is a distribution of scores and not
		// of answers, which is why an answer of five fragments contributes five.
		++_calls;
		_latencies.push_back(a_latencyMs);
		_scores.insert(_scores.end(), a_scores.begin(), a_scores.end());

		Trim(_latencies, _settings.keep);
		Trim(_scores, _settings.keep);
	}

	void Reputation::NoteFailure(bool a_timeout)
	{
		const std::lock_guard hold(_lock);

		// engine/reputation.py:65-69. A failure is a CALL: the denominator of the
		// failure rate has to count the times the model was asked, or a model that
		// only ever fails would divide by nothing.
		++_calls;
		++_failures;
		if (a_timeout) {
			++_timeouts;
		}
	}

	void Reputation::NoteDropped()
	{
		// Counted, and counted HERE and nowhere else - see the note at the
		// declaration. It is visible in the log and in the roster, and it is
		// deliberately absent from Weight(), which is the "punish" the contract
		// forbids for a pass the model was reached too late to answer.
		const std::lock_guard hold(_lock);
		++_dropped;
	}

	void Reputation::NoteSilenceProbe(bool a_invented)
	{
		const std::lock_guard hold(_lock);
		++_silenceProbes;
		if (a_invented) {
			++_silenceInventions;
		}
	}

	void Reputation::NoteAgreement(bool a_agreed)
	{
		const std::lock_guard hold(_lock);
		if (a_agreed) {
			++_agreed;
		} else {
			++_disagreed;
		}
	}

	std::int32_t Reputation::Latency(double a_percentile) const
	{
		const std::lock_guard hold(_lock);
		return PercentileOf(_latencies, a_percentile, static_cast<std::int32_t>(_declaredBudgetMs));
	}

	Speed Reputation::MeasuredClass() const
	{
		const std::lock_guard hold(_lock);

		// engine/reputation.py:89-93. Until the samples are there the DECLARATION
		// is the whole interim roster, and since latencies are not carried between
		// runs, "until then" is the start of every session.
		const std::size_t need = _settings.latencySamplesNeeded > 0 ?
			static_cast<std::size_t>(_settings.latencySamplesNeeded) :
			0U;
		if (_latencies.size() < need) {
			return _declaredClass;
		}

		const std::int32_t measured = PercentileOf(_latencies, _settings.latencyPercentile,
			static_cast<std::int32_t>(_declaredBudgetMs));
		return measured <= _settings.fastBelowMs ? Speed::Fast : Speed::Accurate;
	}

	std::size_t Reputation::LatencySamples() const
	{
		const std::lock_guard hold(_lock);
		return _latencies.size();
	}

	double Reputation::InventionRate() const
	{
		const std::lock_guard hold(_lock);
		return InventionOf(_silenceProbes, _silenceInventions);
	}

	float Reputation::Normalize(float a_raw) const
	{
		const std::lock_guard hold(_lock);
		return NormalizeIn(_scores, _calibrationScores, _settings.minSample, a_raw);
	}

	double Reputation::Weight() const
	{
		const std::lock_guard hold(_lock);

		// engine/reputation.py:138-142, and the order of the three lines is the
		// order it had: the trust we were given, cut by how often the model fails,
		// cut by how often it invents over silence, and floored so that one
		// wretched session cannot silence a model for the rest of the run.
		double value = _trust ? *_trust : _settings.defaultTrust;

		if (_calls) {
			value *= 1.0 - std::min(_settings.maxFailurePenalty,
				static_cast<double>(_failures) / static_cast<double>(_calls));
		}
		value *= 1.0 - std::min(_settings.maxInventionPenalty,
			InventionOf(_silenceProbes, _silenceInventions));

		// max(minWeight, min(1.0, value)) written as a clamp. A trust above one out
		// of a hand-edited file is brought down by the same line.
		return std::clamp(value, _settings.minWeight, 1.0);
	}

	std::int32_t Reputation::DeadlineMs(std::int32_t a_defaultBudgetMs, std::int32_t a_bootstrapSlackMs) const
	{
		const std::lock_guard hold(_lock);

		// A model that declared nothing gets the adapter's own budget rather than
		// zero: zero would be a deadline already spent at the instant it was made.
		const std::int32_t declared = _declaredBudgetMs != 0 ?
			static_cast<std::int32_t>(_declaredBudgetMs) :
			a_defaultBudgetMs;

		const std::size_t need = _settings.latencySamplesNeeded > 0 ?
			static_cast<std::size_t>(_settings.latencySamplesNeeded) :
			0U;

		// THE BOOTSTRAP IS THE DECLARATION PLUS SLACK, which is engine/engine.py:126
		// (`timeout=adapter.budget_ms / 1000.0 + 1.0`) in milliseconds. Once the
		// measurement exists it replaces the declaration whole - the model is what
		// it does, not what it said.
		if (_latencies.size() < need) {
			return declared + a_bootstrapSlackMs;
		}
		return PercentileOf(_latencies, _settings.latencyPercentile, declared);
	}

	Reputation::Numbers Reputation::Report() const
	{
		const std::lock_guard hold(_lock);

		Numbers out;
		out.id = _id;
		out.declaredClass = _declaredClass;
		out.declaredBudgetMs = _declaredBudgetMs;

		const std::int32_t fallback = static_cast<std::int32_t>(_declaredBudgetMs);
		out.latencyP50 = PercentileOf(_latencies, 0.5, fallback);

		// The field is named for the shipped percentile; the NUMBER is the one the
		// roster actually routes by, so that a tuned latencyPercentile cannot make
		// the log disagree with the behaviour it is supposed to explain.
		out.latencyP90 = PercentileOf(_latencies, _settings.latencyPercentile, fallback);

		const std::size_t need = _settings.latencySamplesNeeded > 0 ?
			static_cast<std::size_t>(_settings.latencySamplesNeeded) :
			0U;
		out.measuredClass = _latencies.size() < need ?
			_declaredClass :
			(out.latencyP90 <= _settings.fastBelowMs ? Speed::Fast : Speed::Accurate);

		out.calls = _calls;
		out.failures = _failures;
		out.timeouts = _timeouts;
		out.dropped = _dropped;
		out.silenceProbes = _silenceProbes;
		out.silenceInventions = _silenceInventions;
		out.agreed = _agreed;
		out.disagreed = _disagreed;

		double weight = _trust ? *_trust : _settings.defaultTrust;
		if (_calls) {
			weight *= 1.0 - std::min(_settings.maxFailurePenalty,
				static_cast<double>(_failures) / static_cast<double>(_calls));
		}
		weight *= 1.0 - std::min(_settings.maxInventionPenalty,
			InventionOf(_silenceProbes, _silenceInventions));
		out.weight = std::clamp(weight, _settings.minWeight, 1.0);

		// Calibrated means SOMEBODY MEASURED THIS MODEL against a known corpus.
		// It is not "has a weight" - every model has a weight - and it is not
		// "has scores": an uncalibrated model still has a default trust.
		out.calibrated = _trust.has_value();
		out.calibrationSamples = _calibrationScores.size();
		return out;
	}

	// --------------------------------------------------------------- Reputations

	Reputations::Reputations(ReputationSettings a_settings) :
		_settings(std::move(a_settings))
	{}

	Reputation& Reputations::Of(const std::string& a_id)
	{
		const std::lock_guard hold(_lock);

		// try_emplace builds the record IN PLACE - Reputation is neither copyable
		// nor movable on purpose - and does not build one at all when the id is
		// already there, which is the ordinary case on every answer.
		const auto [where, made] = _byId.try_emplace(a_id, a_id, _settings);
		static_cast<void>(made);
		return where->second;
	}

	bool Reputations::Load(const std::filesystem::path& a_path)
	{
		// FAILURE IS NOT AN ERROR HERE AND NOTHING LEAVES THIS FUNCTION. Every
		// path out of it is a bool: no file, an unreadable file, a file of the
		// wrong shape, a file from an older version. All of them leave every
		// standing uncalibrated, which is a state the arithmetic already handles,
		// and the caller says one line about it.
		try {
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				Loc::Info("$SPEECHBROKERVOICE_LOG_REPUTATIONS_MISSING", a_path.string());
				return false;
			}

			// Not the throwing overload: a calibration somebody hand-edited is the
			// likely case, not the exceptional one.
			const nlohmann::json root = nlohmann::json::parse(file, nullptr, false, true);
			if (root.is_discarded() || !root.is_object()) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_REPUTATIONS_SHAPE", a_path.string());
				return false;
			}

			std::size_t models = 0;
			std::size_t trusted = 0;

			// key()/value() rather than a structured binding over items(): the
			// proxy of the library supports both, and the named calls are the
			// spelling that does not depend on which version of it was fetched.
			for (auto entry = root.begin(); entry != root.end(); ++entry) {
				const std::string& id = entry.key();
				const nlohmann::json& item = entry.value();
				if (id.empty() || !item.is_object()) {
					continue;
				}

				Reputation& record = Of(id);
				const std::lock_guard hold(record._lock);
				++models;

				// A missing trust is NOT a trust of zero. It is the third state the
				// header names, and it is what defaultTrust exists for.
				if (const auto trust = item.find("trust");
					trust != item.end() && trust->is_number()) {
					record._trust = trust->get<double>();
					++trusted;
				}

				if (const auto declared = item.find("declaredClass");
					declared != item.end() && declared->is_string()) {
					Speed said = record._declaredClass;
					if (SpeedFromWord(declared->get<std::string>(), said)) {
						record._declaredClass = said;
					} else {
						Loc::Warn("$SPEECHBROKERVOICE_LOG_REPUTATIONS_CLASS_UNKNOWN", id,
							declared->get<std::string>());
					}
				}

				if (const auto scores = item.find("calibrationScores");
					scores != item.end() && scores->is_array()) {
					record._calibrationScores.clear();
					record._calibrationScores.reserve(scores->size());
					for (const auto& score : *scores) {
						if (score.is_number()) {
							record._calibrationScores.push_back(score.get<float>());
						}
					}
				}
			}

			Loc::Info("$SPEECHBROKERVOICE_LOG_REPUTATIONS_LOADED", a_path.string(), models, trusted);
			return models != 0;
		} catch (const std::exception& problem) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REPUTATIONS_BROKEN", a_path.string(),
				std::string(problem.what()));
			return false;
		} catch (...) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_REPUTATIONS_BROKEN", a_path.string(),
				std::string("unknown"));
			return false;
		}
	}

	std::vector<Reputation::Numbers> Reputations::Report() const
	{
		// THE TWO LOCKS ARE NOT HELD TOGETHER. The map's lock is taken to collect
		// the addresses and released; each record's own lock is taken afterwards,
		// one at a time, inside Report(). The addresses stay good because the map
		// is node-based and nothing is ever erased from it.
		std::vector<const Reputation*> records;
		{
			const std::lock_guard hold(_lock);
			records.reserve(_byId.size());
			for (const auto& [id, record] : _byId) {
				static_cast<void>(id);
				records.push_back(&record);
			}
		}

		std::vector<Reputation::Numbers> out;
		out.reserve(records.size());
		for (const Reputation* record : records) {
			out.push_back(record->Report());
		}

		// An unordered map hands its entries out in whatever order it likes, and a
		// log that reorders itself between two sessions is a log nobody can diff.
		std::sort(out.begin(), out.end(),
			[](const Reputation::Numbers& a_left, const Reputation::Numbers& a_right) {
				return a_left.id < a_right.id;
			});
		return out;
	}
}
