#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Voice::Models
{
	// WHAT WE MEASURED, AGAINST WHAT WE WERE TOLD.
	//
	// A model mod is somebody else's program. We did not write it, we know almost
	// nothing about it, and all we know is what the contract made its author
	// declare - and there is no ground at all for believing the author neither
	// erred nor cheated. Hence the rule this whole file exists to enforce:
	// DECLARED IS A HINT, MEASURED IS A FACT. Routing goes by what was measured
	// (engine/reputation.py:1-14).
	//
	// WHAT IT DOES NOT DO: it does not judge how CORRECT a transcription is. On
	// live speech there is no reference to judge against. Correctness is measured
	// separately, on a known corpus, and arrives here as `trust` from
	// reputations.json (engine/reputation.py:12-14, engine-bench.py).
	//
	// NOTHING HERE INCLUDES THE CONTRACT, and that is deliberate rather than
	// incidental: a standing is arithmetic over numbers, and keeping the model
	// header out of it means this file can be exercised with a table of numbers
	// and no game, no bridge and no model anywhere near it - the same property
	// audio/ and turn/ have.
	//
	// THREAD: any. Every public entry takes the one lock below. The arbiter reads
	// it from several workers at once while a dispatch thread is writing a
	// latency into it, so this is not optional and it is not per-model.

	// The tuned numbers of a standing. Every one of them is a constant in the
	// reference and a settings field here, because every one of them will be
	// tuned again.
	struct ReputationSettings
	{
		// Where the calibration lives, relative to the folder of the adapter and
		// refused if it leaves it (the same ResolveInside the service settings go
		// through, Config.cpp). Empty means no calibration file, and then
		// normalisation falls back to the raw number until ten live scores exist.
		//
		// IT NEEDS A SHIPPING PATH INTO THE MOD or the calibration fallback is
		// dead along with it (docs/model-host.md, last paragraph).
		std::string file{ "reputations.json" };

		// How many of the most recent observations are kept per model.
		// engine/reputation.py:21 (`KEEP = 200`).
		int keep{ 200 };

		// What to believe about a model that has never been calibrated. It was
		// 0.6 written into the code of the reference, and every score of every
		// model we simply had not measured yet was silently cut by two fifths
		// (engine/reputation.py:57-60).
		double defaultTrust{ 0.6 };

		// Below this many observations there is nothing to normalise against: a
		// place in a distribution of three points is not a measurement, it is the
		// look of one. It is a threshold of trust in the CORPUS and not a waiting
		// time (engine/reputation.py:129-133).
		//
		// THE CONTRACT FIXES THE SCORE SCALE AT [0, 1] BECAUSE OF THIS NUMBER.
		// Under it the raw numbers of different models are compared directly, so
		// a log-probability or a 0-100 confidence would win or lose on scale
		// alone (contract, SpeechBrokerVoiceFragment::score).
		int minSample{ 10 };

		// Which percentile of the measured latencies stands for "how long this
		// model takes". engine/reputation.py:83-86.
		double latencyPercentile{ 0.9 };

		// At or below this measured p90 a model is fast. engine/reputation.py:88.
		int fastBelowMs{ 800 };

		// How many latency samples are needed before the measurement replaces the
		// declaration. Until then declaredClass IS the interim roster, and since
		// latencies are not carried between runs, "until then" is the start of
		// every session (engine/reputation.py:89-93, contract ModelInfo::declaredClass).
		int latencySamplesNeeded{ 5 };

		// Ceilings on how far a bad record may pull a weight down, so that one
		// wretched session cannot silence a model entirely.
		// engine/reputation.py:154-156.
		double maxFailurePenalty{ 0.5 };
		double maxInventionPenalty{ 0.8 };
		double minWeight{ 0.05 };
	};

	// Which class a model is in for the purpose of the interim roster. It is the
	// contract's SpeechBrokerVoiceClass, restated here so that this file needs no
	// contract; the two are kept equal by a static_assert in the body.
	enum class Speed : std::int32_t
	{
		Fast = 1,
		Accurate = 2
	};

	// One model's whole record. It is owned by Reputations and handed out only by
	// reference under its lock - never copied out, because a copy would be a
	// snapshot that goes on being read after the model has moved on.
	class Reputation
	{
	public:
		Reputation(std::string a_id, const ReputationSettings& a_settings);

		const std::string& Id() const noexcept { return _id; }

		// What the model said about itself at Register. Set once, never measured
		// over, and kept because it is the whole interim roster until five
		// latencies exist.
		void Declare(Speed a_class, std::uint32_t a_budgetMs);

		// --- observations, all of them from a dispatch thread or a worker ------

		// One answer that arrived in time. a_scores is every fragment's score, as
		// the reference fed it (engine/reputation.py:63-66).
		void NoteCall(std::int32_t a_latencyMs, const std::vector<float>& a_scores);

		// An answer that did not arrive, or arrived FAILED. a_timeout separates
		// "was asked and stayed silent past the deadline" from "was asked and
		// said it went wrong"; both count as a failure, only the first counts as
		// a timeout (engine/reputation.py:68-72).
		void NoteFailure(bool a_timeout);

		// A queued request that was never sent - replaced by a later serial of
		// the same turn, or its budget was already spent when Submit was reached.
		//
		// THIS IS THE ONE PLACE THE CONTRACT APPEARS TO CONTRADICT ITSELF, so the
		// resolution is written down rather than left to whoever reads it next.
		//   - Submit's note and docs/model-host.md, "Dispatch": a dropped entry is
		//     "COUNTED against that model in the same ledger as a timeout",
		//     because a model that is always too slow to be submitted to must not
		//     look statistically perfect.
		//   - deadlineMs's note and docs/model-host.md, "The deadline": "YOU ARE
		//     NEVER CHARGED A TIMEOUT FOR A PASS YOU WERE REACHED TOO LATE TO
		//     ANSWER. The standing that routes models is not allowed to punish a
		//     model for the adapter's own queueing."
		// Both are kept by counting drops in this ledger, beside the timeouts,
		// where the log and the roster can see them - and by keeping them OUT of
		// Weight(), which is the "punish" the second sentence forbids. It is
		// exactly the treatment BUSY gets, and for exactly the same reason.
		void NoteDropped();

		// The silence probe. a_invented is true when the model answered a buffer
		// of digital silence with text. Never a latency sample and never a
		// timeout: the first inference of a session pays for workspace and
		// autotune, and charging that to a model would cut the standing of every
		// heavy model before it had answered a single real utterance (contract,
		// "The probe, and why it carries no flag").
		void NoteSilenceProbe(bool a_invented);

		// Two models independently produced the same text over the same stretch,
		// or did not. Fed by the arbiter's fold.
		void NoteAgreement(bool a_agreed);

		// --- what was measured -------------------------------------------------

		// The declared budget when nothing has been measured yet, so that a
		// caller never has to test for emptiness. engine/reputation.py:83-86.
		std::int32_t Latency(double a_percentile) const;

		// Measured once latencySamplesNeeded samples exist, declared before that.
		// engine/reputation.py:88-93.
		Speed MeasuredClass() const;

		// How many latency samples there are, so that the deadline bootstrap can
		// ask the question the roster asks and get the same answer.
		std::size_t LatencySamples() const;

		double InventionRate() const;

		// A model's raw score turned into its place in ITS OWN distribution:
		// "higher than four cases out of five" means the same thing from anybody,
		// where 0.9 from one model and 0.9 from another do not.
		//
		// BELOW minSample OBSERVATIONS IT RETURNS THE RAW NUMBER. That is not a
		// defect to be fixed by pretending: the adapter must not claim a ranking
		// it cannot compute (engine/reputation.py:135-146).
		float Normalize(float a_raw) const;

		// HOW MUCH THIS MODEL'S VOICE IS WORTH IN AN ARGUMENT. A weight of a
		// vote, NOT a multiplier on a score.
		//
		// It used to multiply the score itself, and then a model that was right
		// nine times in ten cut a tenth off every number it produced: a phrase
		// recognised word for word drifted further from the threshold the more
		// honest its calibration had been. Lowering a score for being correct is
		// nonsense, and it ended there (engine/reputation.py:148-162).
		double Weight() const;

		// The deadline a pass is given when this model is in it:
		// budgetMs + slack until latencySamplesNeeded samples exist, then the
		// measured p90 (engine/engine.py:126, engine/reputation.py:83-93).
		// a_defaultBudgetMs stands in when the model declared 0.
		std::int32_t DeadlineMs(std::int32_t a_defaultBudgetMs, std::int32_t a_bootstrapSlackMs) const;

		// For the log and for a report. A plain copy of the counters, so that
		// nothing outside holds a reference into the record.
		struct Numbers
		{
			std::string   id;
			Speed         declaredClass{ Speed::Accurate };
			Speed         measuredClass{ Speed::Accurate };
			std::uint32_t declaredBudgetMs{ 0 };
			std::int32_t  latencyP50{ 0 };
			std::int32_t  latencyP90{ 0 };
			std::uint64_t calls{ 0 };
			std::uint64_t failures{ 0 };
			std::uint64_t timeouts{ 0 };
			std::uint64_t dropped{ 0 };
			std::uint64_t silenceProbes{ 0 };
			std::uint64_t silenceInventions{ 0 };
			std::uint64_t agreed{ 0 };
			std::uint64_t disagreed{ 0 };
			double        weight{ 0.0 };
			bool          calibrated{ false };
			std::size_t   calibrationSamples{ 0 };
		};
		Numbers Report() const;

		// A record is neither copied nor moved. Of() hands out a reference that
		// several threads then hold at once, and a copy of a standing is a
		// snapshot that goes on being read after the model has moved on.
		Reputation(const Reputation&) = delete;
		Reputation(Reputation&&) = delete;
		Reputation& operator=(const Reputation&) = delete;
		Reputation& operator=(Reputation&&) = delete;

	private:
		friend class Reputations;

		// ONE LOCK PER RECORD, NOT ONE OVER THE TABLE. Of() returns a reference
		// and the caller then reads it whenever it likes, so the lock that
		// protects the counters has to travel with the counters; a lock living in
		// Reputations could not be taken by Weight(). Reputations::_lock guards
		// the MAP and nothing else, and the two are never held together for
		// longer than the insertion of a new record.
		//
		// mutable because Latency, Normalize, Weight and Report are const and
		// every one of them takes it.
		mutable std::mutex _lock;

		std::string        _id;
		ReputationSettings _settings;

		Speed         _declaredClass{ Speed::Accurate };
		std::uint32_t _declaredBudgetMs{ 0 };

		std::uint64_t _calls{ 0 };
		std::uint64_t _failures{ 0 };
		std::uint64_t _timeouts{ 0 };

		// Kept apart from _failures on purpose - see NoteDropped.
		std::uint64_t _dropped{ 0 };

		// Rings of the last ReputationSettings::keep observations.
		std::vector<std::int32_t> _latencies;
		std::vector<float>        _scores;

		std::uint64_t _silenceProbes{ 0 };
		std::uint64_t _silenceInventions{ 0 };
		std::uint64_t _agreed{ 0 };
		std::uint64_t _disagreed{ 0 };

		// From the calibration file. Empty means never calibrated, which is a
		// third state and not a trust of zero.
		std::optional<double> _trust;
		std::vector<float>    _calibrationScores;
	};

	// Every standing at once, and the one lock over them.
	class Reputations
	{
	public:
		explicit Reputations(ReputationSettings a_settings);

		// The record for this id, made on first sight. Ids are the contract's -
		// short, no spaces, and the thing every answer is signed with.
		//
		// THREAD: any. The reference is stable for the life of the process: the
		// map is node-based and nothing is ever erased from it, because a model
		// that unregistered may still be named by an answer already in flight and
		// by the log line that records it.
		Reputation& Of(const std::string& a_id);

		// Read a calibration file: trust, the declared class as it was last seen,
		// and the score distribution the normalisation falls back to.
		//
		// FAILURE IS NOT AN ERROR. No file, a file that does not parse, a file
		// from an older version - all of them leave every standing uncalibrated,
		// which is a state the arithmetic already handles. Returns false so that
		// the caller can say one line about it; it must not refuse to run.
		//
		// THREAD: the worker that brings this half up, before any model has been
		// registered. Never the game thread - it reads a file.
		bool Load(const std::filesystem::path& a_path);

		// THERE IS NO Save, AND THAT IS A DECISION RATHER THAN AN OMISSION.
		//
		// Two reasons, and either alone is enough. Latencies and invention rates
		// are deliberately not carried between runs: hardware and surroundings
		// change, and a stored number would look like a measurement without being
		// one (engine/reputation.py:201-207). And the adapter has nowhere to run
		// a save: SKSE sends no shutdown message, a plugin dies with the process,
		// and writing a file from a static destructor is writing it under the
		// loader lock. What IS long-lived - trust and the calibration
		// distribution - is produced by the calibration run, not by play, and
		// belongs to whatever produced it.

		// Everything, for one log line at the end of a session or on request.
		// THREAD: any.
		std::vector<Reputation::Numbers> Report() const;

		const ReputationSettings& Settings() const noexcept { return _settings; }

	private:
		ReputationSettings _settings;

		// Guards THE MAP only - the lookup and the insertion of a new record.
		// Never held while a record's own counters are read: each record carries
		// its own lock, for the reason written at Reputation::_lock.
		mutable std::mutex _lock;

		// Node-based deliberately: Of() hands out a reference that must stay
		// valid while another thread inserts. Never erased, because a model that
		// unregistered may still be named by an answer already in flight.
		// Reputation is neither copyable nor movable, so entries are built in
		// place with try_emplace.
		std::unordered_map<std::string, Reputation> _byId;
	};
}
