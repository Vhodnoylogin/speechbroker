#include "turn/Ears.h"

#include "Loc.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <thread>
#include <utility>

namespace Voice
{
	namespace
	{
		// THE ONE RATE THIS FILE ASSUMES, AND IT SIZES A BUFFER RATHER THAN DECIDING
		// ANYTHING.
		//
		// The ring holds DEVICE samples, and the rate of the device is not known
		// until a device opens - but the ring has to exist before the capture that
		// writes into it (turn/Ears.h, the declaration order is the construction
		// order and it is deliberate). So its capacity is CaptureSettings::ringMs at
		// the rate a shared-mode endpoint ordinarily gives, which is the rate that
		// number was sized against in the first place (audio/Capture.h: "4 s of
		// 48 kHz mono float is 768 kB").
		//
		// It is not a threshold and it reaches nothing that cuts: an endpoint that
		// comes up faster simply gets proportionally less time in the ring, and the
		// body says so in the log when it opens rather than leaving somebody to
		// wonder why the overruns started. It is not a setting either - ringMs is the
		// setting, and a second knob here could only ever come to disagree with it.
		constexpr std::uint32_t kRingSizingRate = 48000;

		// ONE FLOOR, NOT TWO, AND ONE CEILING, NOT TWO. Resolved here, at the one
		// moment both settings structs are in hand, so that nothing below ever has to
		// decide in the hot path which of two numbers is in force (turn/SpeechTurn.h,
		// CutRules).
		CutRules ResolveRules(const EarsSettings& a_settings)
		{
			CutRules rules;
			const auto fromTurn = MsToSamples(
				static_cast<std::uint64_t>(std::max(0, a_settings.turn.minPassMs)));
			const auto fromVad = SecondsToSamples(a_settings.vad.minUttSec);
			rules.minSamples = std::max(fromTurn, fromVad);
			rules.maxSamples = SecondsToSamples(a_settings.vad.maxUttSec);
			rules.minPeak = static_cast<float>(a_settings.vad.minPeak);
			return rules;
		}

		// How much sound the ring holds, in device samples. See kRingSizingRate.
		std::size_t RingSamples(const CaptureSettings& a_capture)
		{
			const auto ms = static_cast<std::uint64_t>(std::max(0, a_capture.ringMs));
			const auto want = ms * kRingSizingRate / 1000ULL;

			// A ring that cannot hold a couple of device periods is not a ring, it is
			// a guaranteed overrun on every block. The settings are obeyed everywhere
			// they can be obeyed and reported where they cannot - see Ears::Start,
			// which says this one out loud.
			const auto floorMs = static_cast<std::uint64_t>(std::max(1, a_capture.blockMs)) * 2ULL;
			const auto least = floorMs * kRingSizingRate / 1000ULL;
			return static_cast<std::size_t>(std::max(want, least));
		}

		// The ring capacity back in milliseconds, for the two lines that report it.
		std::uint64_t RingMsAt(std::size_t a_samples, std::uint32_t a_rate) noexcept
		{
			return a_rate == 0 ? 0ULL : static_cast<std::uint64_t>(a_samples) * 1000ULL / a_rate;
		}
	}

	Ears::Ears(EarsSettings a_settings) :
		_settings(std::move(a_settings)),
		_rules(ResolveRules(_settings)),
		_ring(RingSamples(_settings.capture)),
		_capture(_settings.capture, _ring),
		_gate(_settings.vad),
		_pacer(_settings.pacer),
		_turn(_settings.turn, _rules, _settings.prosody),
		_pitch(_settings.prosody),
		_judge(_settings.completeness)
	{
		// Every allocation of the session happens here or in a Reset, and never in
		// the per-block path. The pitch scratch is sized once; the three consumer
		// buffers are sized again for the real device rate when one opens
		// (Loop::Reshape), and are given a rate now only so that nothing is empty
		// before the first device is up.
		_pitch.Prepare();

		const auto block = static_cast<std::size_t>(
			MsToSamples(static_cast<std::uint64_t>(std::max(1, _settings.capture.blockMs))));
		_raw.assign(block, Sample{ 0 });
		_silence.assign(block, Sample{ 0 });
		_block.reserve(block);
	}

	Ears::~Ears()
	{
		Stop();
	}

	void Ears::OnPass(PassReady a_sink)
	{
		_sink = std::move(a_sink);
	}

	bool Ears::Start()
	{
		if (_running.load(std::memory_order_acquire)) {
			return true;
		}

		// THE CONSUMER LIVES IN A LOCAL CLASS, and that is not a flourish: the header
		// is fixed and declares no member for the loop, while the loop needs the
		// whole of the private state. A local class of a member function has exactly
		// the access that member function has, so this is the one shape that gives it
		// that state without touching the header five other files are compiling
		// against.
		//
		// Everything it carries of its own - the read chunk, the silence still owed,
		// whether the floor has been reported - belongs to the consumer thread alone
		// and to no other, which is why none of it is a member of Ears and why not
		// one of the four things it drives (Resampler, Gate, Pacer, SpeechTurn) has a
		// lock.
		struct Loop
		{
			Ears& e;

			// Device samples per read. One block of the device, so that the gate
			// judges loudness over the same span the reference did rather than over
			// whatever happened to be waiting in the ring.
			std::size_t readChunk{ 1 };

			// SILENCE STILL OWED, in device samples. An overrun drops the NEWEST
			// sound (audio/Ring.h), so the hole is at the end of what was just read:
			// it is paid down at the top of the following iterations, before any
			// further reading, which puts the silence back exactly where the sound
			// went missing.
			//
			// A HOLE IS FILLED, NOT SKIPPED. A hole that were counted and not filled
			// would shorten every time after it in the turn - a 500 ms overrun would
			// make the rest of the turn arrive 500 ms early, permanently and
			// silently.
			std::uint64_t owed{ 0 };

			bool          wasReady{ false };   // the floor has been reported
			bool          spentSaid{ false };  // the file source ran out and we said so
			std::uint64_t passesHere{ 0 };     // passes made in the open turn

			// ---------------------------------------------------------------- sizing

			// Sized once per capture epoch - a new device is a Reset and a Reset may
			// allocate. Nothing below this line ever allocates again.
			void Reshape(std::uint32_t a_sourceRate)
			{
				const auto blockMs = static_cast<std::uint64_t>(std::max(1, e._settings.capture.blockMs));
				readChunk = static_cast<std::size_t>(
					std::max<std::uint64_t>(1ULL,
						static_cast<std::uint64_t>(a_sourceRate) * blockMs / 1000ULL));

				// _raw does double duty and the two uses never overlap: it is the
				// read buffer, and at the one moment a turn opens it carries the
				// pre-roll into the turn - by which time the read it held has already
				// been resampled into _block. So it is reserved for the larger of the
				// two and neither use allocates. Two blocks of headroom over the
				// pre-roll, because a gate may keep its ring in whole blocks.
				const auto preRollRoom = static_cast<std::size_t>(
					MsToSamples(static_cast<std::uint64_t>(std::max(0, e._settings.vad.preRollMs))) +
					2ULL * MsToSamples(blockMs));
				e._raw.reserve(std::max(readChunk, preRollRoom));
				e._raw.assign(readChunk, Sample{ 0 });

				// The zeros a hole is filled with. One read chunk of them is enough:
				// a hole larger than that is paid down over several iterations, which
				// is also what keeps one iteration's work bounded however long the
				// machine stalled for.
				e._silence.assign(readChunk, Sample{ 0 });

				e._block.clear();
				e._block.reserve(e._resampler.Expect(readChunk) + 1U);
			}

			// ---------------------------------------------------------------- device

			// Everything in the ring belongs to the device that has just gone: read it
			// out and count it. Bounded by what was waiting when we asked, so a
			// producer that keeps writing cannot hold us here.
			std::uint64_t Drain()
			{
				std::uint64_t dropped = 0;
				for (auto left = e._ring.Available(); left > 0;) {
					const auto got = e._ring.Read(e._raw.data(), std::min(left, e._raw.size()));
					if (got == 0) {
						break;
					}
					dropped += got;
					left -= got;
				}
				return dropped;
			}

			// A DEVICE CHANGE IS A HOLE. The epoch is read first and the format
			// second, because Capture publishes them the other way round
			// (audio/Capture.h:289-298) - so a format taken after an epoch is at least
			// as new as that epoch.
			// Returns whether there was anything to do, so that a device which has
			// bumped its epoch and not yet published a format is waited for with the
			// idle sleep rather than spun on.
			bool NewEpoch(std::uint32_t a_epoch)
			{
				const auto format = e._capture.Format();
				if (format.sampleRate == 0) {
					// The epoch is up and the format is not published yet. Take it on
					// the next turn round rather than resampling by a rate of zero.
					return false;
				}

				const auto wasRate = e._resampler.SourceRate();
				const bool changed = e._epoch != 0;

				// A turn must never span two devices: close the open one properly,
				// with a final pass, so that the half above is not left waiting for a
				// pass that is never coming.
				EndTurn();

				const auto dropped = Drain();
				if (dropped > 0 && wasRate > 0) {
					e._lost.fetch_add(dropped * static_cast<std::uint64_t>(kTargetSampleRate) / wasRate,
						std::memory_order_relaxed);
				}

				// Absorb whatever the pump has counted as lost up to here instead of
				// filling it. Filling a hole keeps the clock of a turn honest, and
				// there is no turn left to keep honest: this one has just been closed
				// and the next one starts its clock at zero on the new device.
				e._seenLost = e._ring.Lost();
				owed = 0;

				e._resampler.Reset(format.sampleRate);

				// A new microphone is a new room: the floor is measured again, and the
				// warm-up runs first, because the first packets of a stream are
				// digital silence (turn/Gate.h, VadSettings::warmUpMs).
				e._gate.Reset();
				e._pacer.Reset();
				e._silenceRun = 0;
				e._sincePass = 0;
				e._serial = 0;
				wasReady = false;
				spentSaid = false;

				Reshape(format.sampleRate);
				e._epoch = a_epoch;

				if (changed) {
					Loc::Warn("$SPEECHBROKERVOICE_LOG_EARS_DEVICE_CHANGED", dropped);
				}
				Loc::Info("$SPEECHBROKERVOICE_LOG_EARS_DEVICE", e._capture.Name(),
					format.sampleRate, format.channels, kTargetSampleRate);
				if (format.sampleRate > kRingSizingRate) {
					Loc::Warn("$SPEECHBROKERVOICE_LOG_EARS_RING_SHORT", format.sampleRate,
						kRingSizingRate, RingMsAt(e._ring.Capacity(), format.sampleRate),
						e._settings.capture.ringMs);
				}
				return true;
			}

			// ------------------------------------------------------------------ turn

			void StartTurn()
			{
				e._turn.Restart(e._nextTurnId++);
				e._turns.fetch_add(1, std::memory_order_relaxed);
				e._pacer.Reset();
				e._silenceRun = 0;
				e._sincePass = 0;
				e._serial = 0;
				passesHere = 0;
				Loc::Debug("$SPEECHBROKERVOICE_LOG_EARS_TURN_BEGAN", e._turn.Id());
			}

			// The block that opened the gate is not fed separately: it is already
			// inside the pre-roll, exactly as it is in the reference, where the
			// triggering block is appended to the rolling buffer before the debounce
			// is tested (voice-service.py:280-284). Feeding it again would put 20 ms
			// of one word into the turn twice.
			//
			// Returns how many samples went in, because the clocks below are advanced
			// by what the turn actually received and not by the size of the block.
			std::size_t OpenTurn(std::size_t a_lost)
			{
				StartTurn();

				e._raw.clear();
				e._gate.TakePreRoll(e._raw);
				std::size_t added = e._raw.size();
				if (added > 0) {
					// One verdict for the whole pre-roll, and it is `loud`. The last
					// of it IS speech - the start debounce ran on it - and calling it
					// quiet would put a phantom anchor at the end of the pre-roll,
					// a hundred milliseconds after the word had already begun. An
					// anchor in the wrong place is worse than no anchor: the snap
					// slack is 350 ms and it would pull correct model times onto it.
					e._turn.Feed(e._raw, true, e._pitch);
				} else {
					// preRollMs == 0 keeps nothing, which is the engine path of the
					// reference exactly (engine/engine.py:64-70 opens a turn on the
					// first loud block and keeps no pre-roll at all). Then the block
					// that opened the gate is the turn's sample zero and goes in on
					// its own.
					e._turn.Feed(e._block, true, e._pitch);
					added = e._block.size();
				}
				if (a_lost > 0) {
					// Only the hole of THIS iteration is charged to the turn. The
					// pre-roll can carry older holes too, from before there was a turn
					// to charge them to; they are in the session's tally either way
					// (EarsStats::lost), and there is nobody to tell about them - the
					// reference had no notion of a hole at all.
					e._turn.NoteLost(a_lost);
				}

				e._raw.resize(readChunk);  // back to being the read buffer; no allocation
				return added;
			}

			// MAKE A PASS. A SERIAL IS SPENT WHEN A PASS IS ARMED, whether or not a
			// buffer comes out of it, and the gap it leaves in the numbers is the
			// record that it happened (contract, SpeechBrokerVoiceRequest::serial).
			//
			// The two refusals below are the reference's engine/engine.py:106-111 -
			// the tail trim and the under-250-ms skip - except that both now live
			// inside SpeechTurn::Cut, which is the one place a snapshot is made and
			// therefore the only place that can decide against making one.
			void RunPass(bool a_final)
			{
				const auto serial = ++e._serial;
				const auto outcome = e._turn.Cut(e._silenceRun, a_final, serial);
				switch (outcome.verdict) {
				case CutVerdict::Made:
					Deliver(outcome.pass);
					break;

				case CutVerdict::TooShort:
					{
						e._tooShort.fetch_add(1, std::memory_order_relaxed);
						const auto held = e._turn.Samples();
						const auto kept = held > e._silenceRun ? held - e._silenceRun : 0ULL;
						Loc::Debug("$SPEECHBROKERVOICE_LOG_EARS_PASS_SHORT", e._turn.Id(), serial,
							SamplesToMs(kept), SamplesToMs(e._rules.minSamples));
					}
					break;

				case CutVerdict::TooQuiet:
					e._tooQuiet.fetch_add(1, std::memory_order_relaxed);
					Loc::Debug("$SPEECHBROKERVOICE_LOG_EARS_PASS_QUIET", e._turn.Id(), serial,
						e._rules.minPeak);
					break;

				case CutVerdict::Empty:
				default:
					break;
				}
			}

			// THE SINK IS ENTERED ON THE CONSUMER THREAD. It must not block and it
			// must not call back into the ears; and it is wrapped, because an
			// exception escaping a thread procedure is std::terminate - a fail-fast
			// that nothing in the process observes (docs/model-host.md, "Supporting
			// pieces").
			void Deliver(const Pass& a_pass)
			{
				e._passes.fetch_add(1, std::memory_order_relaxed);
				++passesHere;

				Loc::Debug("$SPEECHBROKERVOICE_LOG_EARS_PASS", a_pass.turnId, a_pass.serial,
					Loc::Get(a_pass.final ? "$SPEECHBROKERVOICE_WORD_PASS_FINAL"
					                      : "$SPEECHBROKERVOICE_WORD_PASS_INTERIM"),
					a_pass.DurationMs(), a_pass.tailSilenceMs, a_pass.lostSamples);

				if (!e._sink) {
					return;
				}
				try {
					e._sink(a_pass);
				} catch (const std::exception& ex) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_EARS_SINK_THREW", a_pass.turnId, a_pass.serial,
						ex.what());
				} catch (...) {
					Loc::Error("$SPEECHBROKERVOICE_LOG_EARS_SINK_THREW", a_pass.turnId, a_pass.serial,
						Loc::Get("$SPEECHBROKERVOICE_WORD_DID_NOT_WORK"));
				}
			}

			void CloseTurn()
			{
				const auto id = e._turn.Id();
				const auto ms = e._turn.ElapsedMs();
				e._turn.Close();
				e._gate.Close();
				e._pacer.Reset();
				e._silenceRun = 0;
				e._sincePass = 0;
				e._serial = 0;
				Loc::Debug("$SPEECHBROKERVOICE_LOG_EARS_TURN_ENDED", id, ms, passesHere);
				passesHere = 0;
			}

			// A TURN THAT EMITTED ANYTHING MUST EMIT A FINAL PASS. Used where a turn
			// ends for a reason that is not the pacer's silence: a device changed, a
			// recording ran out, the ears are being stopped. SpeechTurn::Cut is the
			// one that guarantees a final pass comes out of a final cut, by handing
			// back the last snapshot again when the floor would refuse a fresh one.
			void EndTurn()
			{
				if (!e._turn.Open()) {
					return;
				}
				RunPass(true);
				CloseTurn();
			}

			// TURN SAMPLE ZERO IS BUFFER SAMPLE ZERO, ALWAYS. When a turn would pass
			// the ceiling the turn ENDS and the next sample starts a new turnId; the
			// window never slides, because every time in the contract - the anchors,
			// the snap slack, the matching against what has already gone out - is
			// measured from turn zero (contract, maxRequestSamples).
			void Ceiling()
			{
				Loc::Info("$SPEECHBROKERVOICE_LOG_EARS_CEILING", e._turn.Id(),
					SamplesToMs(e._rules.maxSamples));
				RunPass(true);
				e._turn.Close();

				// The gate is deliberately NOT closed here. Somebody has been speaking
				// for twenty seconds without a long enough pause; making them pass the
				// start debounce again would cut startMs out of the middle of a
				// sentence, and the pre-roll they would get is sound this turn has
				// already had. As far as the gate is concerned a turn is still open,
				// and it will be closed once, when the speaking really stops.
				StartTurn();
			}

			// ------------------------------------------------------------------ loop

			// One pass round the consumer loop, in the order turn/Ears.h sets out -
			// which is the order of engine/engine.py:61-92 and the order every
			// threshold in the settings was tuned against. Returns false when there
			// was nothing to do, which is the only case where the thread sleeps.
			bool Step()
			{
				// 1. THE SOUND. Either the silence still owed for a hole, or a read.
				// Never both in one iteration: that is what keeps the silence in the
				// place the sound went missing from.
				std::size_t got = 0;
				std::size_t zeros = 0;
				if (owed > 0) {
					zeros = static_cast<std::size_t>(std::min<std::uint64_t>(owed, readChunk));
					owed -= zeros;
				} else {
					got = e._ring.Read(e._raw.data(), std::min(readChunk, e._raw.size()));

					// AFTER the read, never before: what the counter gained belongs to
					// the END of what was just read (audio/Ring.h, the reading
					// protocol).
					const auto lost = e._ring.Lost() - e._seenLost;
					if (lost > 0) {
						e._seenLost += lost;
						owed += lost;
						Loc::Debug("$SPEECHBROKERVOICE_LOG_EARS_HOLE", lost);
					}
				}

				// 2. THE EPOCH. Read after the sound, because a changed epoch means
				// the sound just read belonged to the device that has gone - it is
				// thrown away along with the rest of the ring.
				const auto epoch = e._capture.Epoch();
				if (epoch != e._epoch) {
					return NewEpoch(epoch);
				}
				if (e._epoch == 0) {
					return false;  // nothing has opened yet
				}

				if (got == 0 && zeros == 0) {
					// The ring is empty, which is the ordinary case between device
					// periods. For a recording it can also mean there is no more of
					// it, and then the open turn has to be finished by us: no more
					// sound will ever arrive to make the pacer's silence run out.
					if (e._capture.Finished() && !spentSaid) {
						spentSaid = true;
						Loc::Info("$SPEECHBROKERVOICE_LOG_EARS_SOURCE_SPENT");
						EndTurn();
					}
					return false;
				}

				// 3. TO 16 kHz, THE REMAINDER CARRIED. The resampler appends and the
				// caller owns the buffer, so nothing here allocates.
				e._block.clear();
				if (zeros > 0) {
					e._resampler.Feed({ e._silence.data(), zeros }, e._block);
				} else {
					e._resampler.Feed({ e._raw.data(), got }, e._block);
				}
				if (e._block.empty()) {
					return true;  // the resampler is carrying it all into the next block
				}

				const auto lostOut = zeros > 0 ? e._block.size() : std::size_t{ 0 };
				e._captured.fetch_add(e._block.size(), std::memory_order_relaxed);
				if (lostOut > 0) {
					e._lost.fetch_add(lostOut, std::memory_order_relaxed);
				}

				// 4. THE GATE. While the floor is being measured the block is thrown
				// away and nothing else happens - the person was asked not to speak,
				// and `loud` and `opens` mean nothing until `ready`.
				const Heard heard = e._gate.Offer(e._block);
				if (!heard.ready) {
					return true;
				}
				if (!wasReady) {
					wasReady = true;
					Loc::Info("$SPEECHBROKERVOICE_LOG_EARS_FLOOR", e._gate.Floor(), e._gate.Trigger());
				}

				// 5 and 6. The two states of a turn, and they are alternatives for one
				// block: either this block opens a turn - and then it reaches the turn
				// inside the pre-roll - or it is fed to the turn that is already open.
				bool        loud = heard.loud;
				std::size_t added = 0;
				if (!e._turn.Open()) {
					if (!heard.opens) {
						return true;  // engine/engine.py:64-66
					}
					added = OpenTurn(lostOut);
					loud = true;  // a turn opens on sound above the trigger and on nothing else
				} else {
					// Asked BEFORE the block is fed, because the answer decides which
					// turn this block belongs to.
					if (e._turn.WouldOverflow(e._block.size())) {
						Ceiling();
					}
					e._turn.Feed(e._block, heard.loud, e._pitch);
					if (lostOut > 0) {
						e._turn.NoteLost(lostOut);
					}
					added = e._block.size();
				}

				// engine/engine.py:74-76, in milliseconds there and in samples here.
				e._pacer.Note(loud);
				e._silenceRun = loud ? 0 : e._silenceRun + added;
				e._sincePass += added;

				// 7. `over` FIRST, `run` SECOND, and the second is asked only when the
				// first said no - engine/engine.py:78-79 short-circuits exactly so,
				// and ShouldFire CONSUMES the pacer's arming. Asking it twice about
				// one block, or asking it about a turn that is already over, turns one
				// pass into twenty (engine/turn.py:32-43).
				const bool over = e._pacer.TurnOver(e._silenceRun);
				const bool run = over || e._pacer.ShouldFire(e._silenceRun, e._sincePass);
				if (!run) {
					return true;
				}
				e._sincePass = 0;  // engine/engine.py:82

				// 8. and 9. Cut with final = over, then close the turn if it is over.
				RunPass(over);
				if (over) {
					CloseTurn();
				}
				return true;
			}

			void Run()
			{
				const auto nap = std::chrono::milliseconds(std::max(0, e._settings.consumerPollMs));
				while (!e._stopping.load(std::memory_order_acquire)) {
					if (!Step()) {
						// THE CAPTURE CALLBACK NEVER SIGNALS: nothing that owns a
						// microphone may enter the kernel on our account. The whole
						// cost of that decision is this sleep, and it is one of the
						// two wall clocks this half is allowed - it hands its number
						// to nobody.
						std::this_thread::sleep_for(nap);
					}
				}

				// The thread is going away and a turn may still be open. The half
				// above may never replace an entry whose final is set and closes a
				// turn on it, so a turn that emitted anything leaves with a final
				// pass even here.
				EndTurn();
			}
		};

		// vad.endSilenceMs DOES NOT END A TURN - pacer.endSilenceMs does
		// (engine/engine.py:78 through engine/turn.py:45-46). The field ships in the
		// settings this module inherited, so a player who moves it must be told once
		// that it is not the one being obeyed, rather than left to wonder. Said here,
		// where both structs are in hand, and read nowhere in the cutting path.
		if (_settings.vad.endSilenceMs != _settings.pacer.endSilenceMs) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_EARS_END_SILENCE_SPLIT", _settings.vad.endSilenceMs,
				_settings.pacer.endSilenceMs);
		}
		if (_rules.maxSamples <= _rules.minSamples) {
			Loc::Warn("$SPEECHBROKERVOICE_LOG_EARS_CEILING_LOW", SamplesToMs(_rules.maxSamples),
				SamplesToMs(_rules.minSamples));
		}
		{
			const auto askedMs = static_cast<std::uint64_t>(std::max(0, _settings.capture.ringMs));
			const auto heldMs = RingMsAt(_ring.Capacity(), kRingSizingRate);
			if (heldMs > askedMs) {
				Loc::Warn("$SPEECHBROKERVOICE_LOG_EARS_RING_TINY", askedMs, heldMs);
			}
		}

		// The state of a session, put back where it starts. Everything here is the
		// consumer's, and the consumer does not exist yet.
		_stopping.store(false, std::memory_order_release);
		_seenLost = _ring.Lost();  // the tally of the session carries on; the debt does not
		_epoch = 0;
		_serial = 0;
		_silenceRun = 0;
		_sincePass = 0;
		_gate.Reset();
		_pacer.Reset();

		// NEVER FROM THE THREAD OF THE GAME: this walks the endpoints and may sit
		// through the reopen delays.
		if (!_capture.Start()) {
			Loc::Error("$SPEECHBROKERVOICE_LOG_EARS_NO_SOURCE");
			return false;
		}

		_running.store(true, std::memory_order_release);
		_consumer = std::thread([this] {
			Loop loop{ *this };
			loop.Run();
		});

		Loc::Info("$SPEECHBROKERVOICE_LOG_EARS_STARTED", SamplesToMs(_rules.maxSamples),
			SamplesToMs(_rules.minSamples), _rules.minPeak);
		return true;
	}

	void Ears::Stop() noexcept
	{
		// Idempotent, and the destructor calls it. The flag is the whole guard: a
		// second Stop finds it already set and does nothing.
		if (_stopping.exchange(true, std::memory_order_acq_rel)) {
			return;
		}

		// The sound first, so that the consumer sees the end of it, and the join
		// second. Both waits are bounded - one device period and one consumerPollMs -
		// which is exactly what this adapter is NOT allowed to assume about somebody
		// else's Stop (docs/model-host.md).
		_capture.Stop();
		if (_consumer.joinable()) {
			try {
				_consumer.join();
			} catch (...) {
				// A thread that cannot be joined is not a reason to take the process
				// down on the way out.
			}
		}
		_running.store(false, std::memory_order_release);

		const auto numbers = Numbers();
		Loc::Info("$SPEECHBROKERVOICE_LOG_EARS_STOPPED", numbers.captured, numbers.lost,
			numbers.turns, numbers.passes);
	}

	std::uint32_t Ears::MaxRequestSamples() const noexcept
	{
		// ONE CEILING, NOT TWO: the number that ends a turn is the number the
		// dispatch half publishes, and there is no second field that could drift.
		return static_cast<std::uint32_t>(_rules.maxSamples);
	}

	CaptureFormat Ears::OutputFormat() const noexcept
	{
		return CaptureFormat{ kTargetSampleRate, 1U };
	}

	CaptureFormat Ears::InputFormat() const noexcept
	{
		return _capture.Format();
	}

	EarsStats Ears::Numbers() const noexcept
	{
		EarsStats out;
		out.captured = _captured.load(std::memory_order_relaxed);
		out.lost = _lost.load(std::memory_order_relaxed);
		out.turns = _turns.load(std::memory_order_relaxed);
		out.passes = _passes.load(std::memory_order_relaxed);
		out.passesTooShort = _tooShort.load(std::memory_order_relaxed);
		out.passesTooQuiet = _tooQuiet.load(std::memory_order_relaxed);
		out.opens = static_cast<std::uint32_t>(_capture.Opens());

		// The floor and the trigger are written once per device, by the consumer,
		// and read here for a report. They are plain floats rather than atomics
		// because the header says so; the worst a reader can see is the pair from
		// either side of one Gate::Reset, and nothing decides anything on them.
		out.floor = _gate.Floor();
		out.trigger = _gate.Trigger();
		return out;
	}

	LengthClass Ears::Classify(std::uint32_t a_words) const noexcept
	{
		// Qualified, and it has to be: the member hides the free function of the same
		// name (engine/turn.py:124-129).
		return Voice::Classify(a_words, _settings.segments);
	}
}
