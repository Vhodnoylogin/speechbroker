#include "models/Collector.h"

#include <algorithm>
#include <utility>

namespace Voice::Models
{
	namespace
	{
		// The expected set is a handful of handles, so a linear scan under the
		// lock beats a hash - the header says so at the declaration, and these two
		// helpers are the whole of what "a handful" buys: no allocation, no
		// hashing, and both questions asked the same way.
		bool Holds(const std::vector<SpeechBrokerVoiceHandle>& a_handles,
			SpeechBrokerVoiceHandle a_handle) noexcept
		{
			return std::find(a_handles.begin(), a_handles.end(), a_handle) != a_handles.end();
		}

		bool Answered(const std::vector<Reading>& a_answers, SpeechBrokerVoiceHandle a_handle) noexcept
		{
			for (const auto& answer : a_answers) {
				if (answer.handle == a_handle) {
					return true;
				}
			}
			return false;
		}
	}

	Collector::Collector(std::int64_t a_utteranceId, Pass a_subject,
		std::vector<SpeechBrokerVoiceHandle> a_expected,
		std::optional<Clock::time_point> a_deadline) :
		_utteranceId(a_utteranceId),
		_subject(std::move(a_subject)),
		_deadline(a_deadline),
		_expected(std::move(a_expected))
	{
		// One slot per model of the pass, bought once. Deliver runs on a model's
		// own thread with our lock held, and an allocation there would hold every
		// other model answering at the same instant behind it.
		_answers.reserve(_expected.size());
	}

	bool Collector::Close() noexcept
	{
		// IT TAKES THE LOCK, AND THAT IS NOT BOOKKEEPING - IT IS THE HAND-OVER.
		//
		// The flag alone marks; the lock is what makes the mark mean "nobody is
		// writing the answers any more". Without it the scheduler can flip the
		// flag, return true, and post to a worker while a model's Deliver is
		// halfway through pushing its reading - the worker then reads Answers()
		// while a vector is being appended to, which is the one race this whole
		// object exists to prevent. With it, a Deliver either got in before the
		// close and is whole, or sees the flag and is refused.
		//
		// noexcept over a lock: std::mutex::lock throws only when the mutex itself
		// is broken, which is not a state this process could carry on in anyway.
		const std::lock_guard hold(_lock);
		return !_closed.exchange(true, std::memory_order_acq_rel);
	}

	bool Collector::Deliver(SpeechBrokerVoiceHandle a_handle, Reading&& a_reading)
	{
		const std::lock_guard hold(_lock);

		if (_closed.load(std::memory_order_acquire)) {
			return false;  // the caller answers STALE, which accuses the model of nothing
		}
		if (!Holds(_expected, a_handle)) {
			return false;
		}

		// A SECOND ANSWER TO ONE PASS IS REFUSED RATHER THAN ADDED. It is not in
		// the header's two sentences because it should not happen at all - the
		// contract gives a model one Complete per request - but the cost of it
		// happening is not a duplicate log line: the arbiter counts agreement by
		// distinct readings, so one model answering twice would agree with itself
		// and outvote two models that agreed with each other. STALE is exactly the
		// right thing to tell it, and it is what Deliver already says to an answer
		// nobody is waiting for.
		if (Answered(_answers, a_handle)) {
			return false;
		}

		_answers.push_back(std::move(a_reading));
		return true;
	}

	bool Collector::Remove(SpeechBrokerVoiceHandle a_handle, Removal /* a_why */)
	{
		// The reason stays in the signature although nothing here reads it: it is
		// the caller's log line, and a call site that had to name it cannot remove
		// a model without saying why. Idempotent, because two threads may drop the
		// same handle - a dispatch thread giving up its queued entry while the
		// model's own thread is inside Unregister.
		const std::lock_guard hold(_lock);

		const auto where = std::find(_expected.begin(), _expected.end(), a_handle);
		if (where == _expected.end()) {
			return false;
		}
		_expected.erase(where);
		return true;
	}

	bool Collector::AllAnswered() const
	{
		const std::lock_guard hold(_lock);

		for (const auto handle : _expected) {
			if (!Answered(_answers, handle)) {
				return false;
			}
		}

		// AN EMPTY EXPECTED SET IS ANSWERED. That is the case the header's note at
		// Remove is about: a pass whose last two participants both went away must
		// not sit until its deadline.
		return true;
	}

	std::vector<SpeechBrokerVoiceHandle> Collector::Missing() const
	{
		const std::lock_guard hold(_lock);

		std::vector<SpeechBrokerVoiceHandle> out;
		for (const auto handle : _expected) {
			if (!Answered(_answers, handle)) {
				out.push_back(handle);
			}
		}
		return out;
	}

	std::vector<SpeechBrokerVoiceHandle> Collector::Expected() const
	{
		const std::lock_guard hold(_lock);
		return _expected;
	}
}
