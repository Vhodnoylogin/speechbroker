#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Voice
{
	// One sample, everywhere in this half of the adapter: 32-bit float, mono,
	// nominally [-1, 1]. It is not a choice made here - the contract has exactly
	// one sample format at version 1 (SPEECHBROKERVOICE_FMT_FLOAT32), and the
	// pointer handed to a model is a const float*. Anything that changes this type
	// changes the contract, so it is written down once and used by name.
	using Sample = float;

	// The distance at which two atomics stop sharing a cache line. Not a setting:
	// it is a property of the processor, and a player has nothing to tune here.
	// The write cursor and the read cursor are touched by two different threads
	// several thousand times a second; on one line they would trade the line back
	// and forth on every block for no reason at all.
	inline constexpr std::size_t kCacheLine = 64;

	// The one place where the microphone and the rest of the adapter meet.
	//
	// EXACTLY ONE PRODUCER AND EXACTLY ONE CONSUMER, and neither may be swapped for
	// two. The producer is the capture pump - the WASAPI callback, or the thread
	// that stands in for it when sound comes from a file. The consumer is the ears'
	// own thread. Write is called from the first and from nowhere else; Read from
	// the second and from nowhere else. That is what lets both of them be free of
	// locks: with one writer and one reader, two atomic cursors are enough, and the
	// capture callback never enters the kernel.
	//
	// THE CALLBACK MUST NOT ALLOCATE, LOCK OR LOG, so the ring does none of those
	// three things after construction. The storage is allocated once, in the
	// constructor, and is never grown, never shrunk and never reallocated - which
	// is also why the capacity is fixed rather than "as much as arrives". The
	// reference had an unbounded queue here (voice-service.py:190); a queue that
	// cannot overrun can only grow, and inside the process of a game growing
	// without bound is the worse of the two failures. We take the overrun and count
	// it instead.
	//
	// WHAT AN OVERRUN MEANS AND WHERE THE HOLE IS. When a write does not fit, the
	// part that does not fit is DROPPED - the newest sound, not the oldest. Nothing
	// already in the ring is overwritten, so the consumer never reads a torn block,
	// and the hole is at a known place: immediately after the newest sample the
	// ring holds. Hence the reading protocol, which a body author must follow
	// exactly:
	//
	//     const auto got  = ring.Read(into, want);
	//     const auto lost = ring.Lost() - seenLost;   // AFTER the read, never before
	//     seenLost += lost;
	//     // `lost` samples are missing at the END of what was just read
	//
	// The consumer then feeds `lost` samples of silence into the resampler in place
	// of the hole. It does NOT simply skip them: the sample count is the only clock
	// in this module, and a hole that is counted but not filled would silently
	// shorten every time after it - a 500 ms overrun would make the rest of the
	// turn arrive 500 ms early, for ever. The filled hole is honest, it keeps the
	// clock, and its size goes on to the contract as Request::lostSamples.
	//
	// The placement is right to within one read, which is a block or so. The
	// producer may write again between the drop and the consumer's read, and then
	// the hole is reported a block later than it happened. That is said out loud
	// rather than hidden; an overrun is already a defect being reported, and a
	// block of misplacement inside it changes nothing anybody can act on.
	class Ring
	{
	public:
		// a_capacity is in samples, and it is what the ring will hold for ever
		// after. The owner builds it before either thread exists.
		explicit Ring(std::size_t a_capacity);

		Ring(const Ring&) = delete;
		Ring(Ring&&) = delete;
		Ring& operator=(const Ring&) = delete;
		Ring& operator=(Ring&&) = delete;

		// PRODUCER THREAD ONLY. Copies what fits, drops the rest and counts it into
		// Lost(). Returns how many samples were taken, which the caller does not
		// have to look at: the counter is the report. Never blocks, never
		// allocates, never logs - it is called from the capture callback.
		std::size_t Write(const Sample* a_from, std::size_t a_count) noexcept;

		// CONSUMER THREAD ONLY. Takes at most a_count samples, returns how many.
		// Zero means the ring is empty, which is the ordinary case between device
		// periods and is not an error.
		std::size_t Read(Sample* a_into, std::size_t a_count) noexcept;

		// CONSUMER THREAD. How much is waiting. An estimate the instant after it is
		// returned - the producer keeps writing - and it is only ever used to size
		// the next read, never to reason about time.
		std::size_t Available() const noexcept;

		std::size_t Capacity() const noexcept { return _capacity; }

		// ANY THREAD. The running total of samples lost, for the life of the ring.
		// Monotone: the consumer keeps its own copy of what it has already seen and
		// works with the difference.
		std::uint64_t Lost() const noexcept { return _lost.load(std::memory_order_acquire); }

		// PRODUCER THREAD. Sound the ring never saw: a WASAPI packet flagged
		// AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, or the remainder thrown away when
		// a device is reopened at another rate. It belongs in the same counter as
		// an overrun - from the far side of the contract a hole is a hole, and
		// Request::lostSamples asks how many samples are missing, not whose fault
		// they were.
		void NoteLost(std::size_t a_samples) noexcept;

		// BOTH SIDES STOPPED. Throws away what is in the ring and puts the cursors
		// back to zero; the lost counter is NOT cleared, because it is the session's
		// tally and nothing in it stopped being true. Used when a device is
		// reopened, never while the pump is running.
		void Reset() noexcept;

	private:
		std::vector<Sample> _data;       // allocated once, in the constructor
		std::size_t         _capacity{ 0 };

		// The cursors run forward without wrapping and are taken modulo the
		// capacity at the point of use: that way "full" and "empty" are told apart
		// by the difference alone, with no spare slot and no extra flag.
		alignas(kCacheLine) std::atomic<std::uint64_t> _write{ 0 };
		alignas(kCacheLine) std::atomic<std::uint64_t> _read{ 0 };
		alignas(kCacheLine) std::atomic<std::uint64_t> _lost{ 0 };
	};
}
