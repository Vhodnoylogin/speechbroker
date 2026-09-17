#include "audio/Ring.h"

#include "Loc.h"

#include <algorithm>
#include <cstring>

namespace Voice
{
	namespace
	{
		// Where a cursor lands in the storage. The cursors themselves run forward
		// for ever and are folded here, at the point of use - twice per call, not
		// once per sample, which is why a division is affordable and a power-of-two
		// capacity is not worth demanding of the owner.
		inline std::size_t Slot(std::uint64_t a_cursor, std::size_t a_capacity) noexcept
		{
			return static_cast<std::size_t>(a_cursor % a_capacity);
		}

		// The two sides both copy a run that may straddle the fold, and both do it
		// the same way: as much as reaches the end of the storage, then the rest
		// from its beginning. Written once so that the producer and the consumer
		// cannot drift apart in how they wrap.
		//
		// memcpy and not a loop: Sample is trivially copyable, the runs are whole
		// blocks, and a copy is the only work either thread is allowed to do here.
		inline void CopySplit(Sample* a_into, const Sample* a_from, std::size_t a_count) noexcept
		{
			if (a_count != 0) {
				std::memcpy(a_into, a_from, a_count * sizeof(Sample));
			}
		}
	}

	Ring::Ring(std::size_t a_capacity) :
		_data(a_capacity),
		_capacity(a_capacity)
	{
		// THE ONE ALLOCATION. It happens here, before either thread exists, and
		// never again: the vector is never grown, never shrunk and never cleared -
		// Reset only moves the cursors. That is the whole difference from the
		// reference, which put an unbounded queue.Queue between the callback and
		// the consumer (engine/audio.py:78, and the same again in the shipped
		// service at voice-service.py:190). A queue that cannot overrun cannot
		// drop, so it can only grow, and inside the process of a game growing
		// without bound is the worse of the two failures. We take the overrun and
		// count it.
		//
		// Constructing the storage also zeroes it. That costs one pass over a few
		// hundred kilobytes at start-up and buys a guarantee: nothing can ever be
		// read out of this ring that was not either written by the producer or a
		// silent sample. A read past the write cursor is prevented below, but a
		// buffer of uninitialised floats one arithmetic slip away from the gate is
		// not a thing to keep in the house.
		if (_capacity == 0) {
			// Nothing will ever fit, so every sample the microphone gives will be
			// counted as a hole and the consumer will hear silence and nothing else.
			// Said out loud at once: the alternative is a session that looks alive,
			// logs an overrun a second for as long as it lasts and is never
			// diagnosed.
			Loc::Error("$SPEECHBROKERVOICE_LOG_RING_NO_ROOM");
		} else {
			Loc::Debug("$SPEECHBROKERVOICE_LOG_RING_READY", _capacity,
				(_capacity * sizeof(Sample)) / 1024);
		}
	}

	std::size_t Ring::Write(const Sample* a_from, std::size_t a_count) noexcept
	{
		if (a_count == 0) {
			return 0;
		}

		// A null block with a count is either a silent packet somebody decided to
		// hand over without a buffer or a defect one call up. Either way there is
		// nothing to copy, and the honest account is a hole: the consumer fills a
		// hole with exactly that many silent samples, so the sample clock - the
		// only clock this half has - keeps time either way, and the counter says
		// it happened instead of hiding it.
		if (a_from == nullptr || _capacity == 0) {
			NoteLost(a_count);
			return 0;
		}

		// OUR OWN CURSOR, and nobody else writes it, so it is read relaxed: this
		// thread wrote the value itself.
		const auto write = _write.load(std::memory_order_relaxed);

		// THE ACQUIRE. It pairs with the consumer's release store of _read at the
		// end of Read, and it is what makes the room safe to write into: everything
		// the consumer had copied out before it published that cursor is finished
		// and cannot still be in flight.
		const auto read = _read.load(std::memory_order_acquire);

		const auto used = static_cast<std::size_t>(write - read);
		const auto room = _capacity - used;
		const auto take = std::min(a_count, room);

		if (take != 0) {
			const auto head = Slot(write, _capacity);
			const auto first = std::min(take, _capacity - head);
			CopySplit(_data.data() + head, a_from, first);
			CopySplit(_data.data(), a_from + first, take - first);
		}

		// THE PUBLISH. Release, and it must come after the copies above: a
		// consumer that saw this cursor is entitled to every sample under it. On
		// this side the store is also what makes the samples visible at all - the
		// consumer's acquire load of _write is the other half of the pair.
		_write.store(write + take, std::memory_order_release);

		if (take < a_count) {
			// THE NEWEST GOES, NOT THE OLDEST. What did not fit is the tail of the
			// block, which is the most recent sound; nothing already in the ring is
			// touched. That is what lets the consumer say where the hole is - right
			// after the newest sample it will read - instead of discovering that the
			// middle of a turn was quietly replaced.
			NoteLost(a_count - take);
		}

		return take;
	}

	std::size_t Ring::Read(Sample* a_into, std::size_t a_count) noexcept
	{
		if (a_into == nullptr || a_count == 0 || _capacity == 0) {
			return 0;
		}

		// OUR OWN CURSOR: relaxed, this thread wrote it.
		const auto read = _read.load(std::memory_order_relaxed);

		// THE ACQUIRE. It pairs with the producer's release store of _write at the
		// end of Write. Everything under this cursor was copied in before the
		// producer published it, so reading it here cannot tear a block in half.
		const auto write = _write.load(std::memory_order_acquire);

		const auto used = static_cast<std::size_t>(write - read);
		const auto take = std::min(a_count, used);

		if (take != 0) {
			const auto tail = Slot(read, _capacity);
			const auto first = std::min(take, _capacity - tail);
			CopySplit(a_into, _data.data() + tail, first);
			CopySplit(a_into + first, _data.data(), take - first);
		}

		// THE PUBLISH, and the release matters as much as the one on the other
		// side: it hands the room back. A producer that sees this cursor may write
		// over those slots at once, so the store has to come after the copies out,
		// never before them.
		_read.store(read + take, std::memory_order_release);

		return take;
	}

	std::size_t Ring::Available() const noexcept
	{
		// The write cursor first and with acquire, for the same reason as in Read:
		// whatever is counted here has to be readable. An estimate the instant
		// after it is returned, and it is only ever used to size the next read.
		const auto write = _write.load(std::memory_order_acquire);
		const auto read = _read.load(std::memory_order_acquire);
		return static_cast<std::size_t>(write - read);
	}

	void Ring::NoteLost(std::size_t a_samples) noexcept
	{
		if (a_samples == 0) {
			return;
		}
		// Release, to pair with the acquire load in Lost(). It carries no data of
		// its own, but the consumer reads this counter as part of a protocol with
		// the producer, and half a pair is how a counter starts arriving out of
		// order with what it describes.
		//
		// No log here on purpose. This is entered on the capture pump, which may
		// not allocate, take a lock or format a line; the counter IS the report,
		// and the consumer - which is allowed to log - reads it after every read
		// and tells the story with the turn it belongs to.
		//
		// The header names the producer as the caller, and that is where it is
		// called from in ordinary work. A read-modify-write on one atomic is
		// nevertheless safe from either side, and it has to be: when the device
		// changes under us it is the CONSUMER that throws the remainder of the ring
		// away and has to account for it as a hole. That is the one place the
		// counter is raised from the other thread, and it costs nothing here.
		_lost.fetch_add(static_cast<std::uint64_t>(a_samples), std::memory_order_release);
	}

	void Ring::Reset() noexcept
	{
		// BOTH SIDES STOPPED. There is no counterpart running to pair an ordering
		// with, and the thread that starts the pump afterwards synchronises with
		// this one by being started, so relaxed is honest here rather than lax.
		//
		// The storage is left as it is: the cursors are what say what is in the
		// ring, so zeroing a few hundred kilobytes would only make a device change
		// slower. And the lost counter is left alone deliberately - it is the tally
		// of the session, and nothing that was lost before a device was reopened
		// stopped having been lost.
		_write.store(0, std::memory_order_relaxed);
		_read.store(0, std::memory_order_relaxed);
	}
}
