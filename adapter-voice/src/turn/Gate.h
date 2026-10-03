#pragma once

#include "turn/Resample.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Voice
{
	// The unit every loudness threshold in the settings is written in.
	//
	// The reference measured loudness as sqrt(mean(square(block))) * 32768
	// (engine/audio.py:131, voice-service.py:249) - that is, it expressed a float
	// sample in the numbers of a 16-bit sample, because that is the scale the
	// thresholds were tuned on and the scale a person reading a log recognises.
	//
	// IT IS NOT A SETTING. It is the unit of minRmsFloor and minPeak, not a knob:
	// change it and every threshold below silently means something else. If a
	// reason ever appears to work in dB, the thresholds change with it, in one
	// commit, and this comment is the record of what they used to mean.
	inline constexpr float kRmsScale = 32768.0f;

	// Everything the gate is tuned by. The shipped values are voice.json:29-39 and
	// every one of them was paid for in a run of the game.
	struct VadSettings
	{
		// How long to listen to the room before deciding what silence sounds like.
		// voice.json:31; used at voice-service.py:246 (`t_end = time.time() +
		// v["noiseFloorSec"]`) - and there it is a WALL CLOCK, which is the one
		// thing that does not come across: here it is a count of samples, like
		// everything else. A person must not speak during it (engine/audio.py:127).
		double noiseFloorSec{ 1.0 };

		// The first packets after a stream opens are digital silence, so measuring
		// straight away gives a floor of zero and a threshold that means nothing.
		// voice-service.py:242-244 (`t_warm = time.time() + 0.5`) - a constant in
		// the reference, a setting here, because a constant in the code is the one
		// number a player cannot reach when their headset warms up slower.
		int warmUpMs{ 500 };

		// How far above the measured floor speech has to be. voice.json:32, used at
		// voice-service.py:251; the same default again at engine/audio.py:125.
		double startFactor{ 3.5 };

		// The threshold never goes below this, whatever the room measured.
		// voice.json:33, voice-service.py:251, engine/audio.py:126.
		double minRmsFloor{ 120.0 };

		// THE FLOOR IS MEASURED ONCE AND THEN LEFT ALONE, and that is a decision,
		// not an omission: speech raises the level, so a threshold that followed
		// the level would climb after the speaker and stop telling speech from
		// silence altogether (engine/audio.py:82-86 is the whole argument). Only a
		// new device starts a new measurement.

		// How long the sound has to stay above the threshold before a turn opens.
		// voice.json:34, voice-service.py:283.
		int startMs{ 120 };

		// How much sound from BEFORE the trigger is kept, so that the first word is
		// not clipped by the very threshold that noticed it. voice.json:35,
		// voice-service.py:280-282 ("rolling pre-roll: the words spoken before the
		// gate opened").
		//
		// NOTE, BECAUSE IT IS THE ONE PLACE THIS MODULE KNOWINGLY DIFFERS FROM THE
		// REFERENCE'S ENGINE PATH. The engine path of the reference used neither
		// this nor startMs: engine/engine.py:64-70 opens a turn on the first loud
		// block, full stop. That drops the onset of the first word and lets a
		// single click start a turn. Both mechanisms exist in the same python, in
		// the older whole-phrase path that was the one actually tuned in the game
		// (voice-service.py:277-284), and they are restored here deliberately.
		// Setting preRollMs and startMs to 0 reproduces the engine path exactly,
		// which is what makes this a choice a player can undo rather than a
		// difference they have to discover.
		int preRollMs{ 800 };

		// voice.json:36, voice-service.py:289 - the silence that ended a phrase on
		// the old whole-utterance path.
		//
		// IT DOES NOT END A TURN HERE. The turn is closed by
		// PacerSettings::endSilenceMs (engine/engine.py:78 through
		// engine/turn.py:45-46). The two ship with the same number and are two
		// different mechanisms; the field is kept because it is in the settings
		// file this module inherits, and the body says once in the log when a
		// player has set them apart, rather than obeying one and ignoring the
		// other in silence. A body author must never read this field in the cutting
		// path.
		int endSilenceMs{ 1600 };

		// The ceiling on a turn. voice.json:37, voice-service.py:289
		// (`or dur >= v["maxUttSec"]`).
		//
		// IT HAS A SECOND DUTY HERE THAT IT DID NOT HAVE IN THE REFERENCE: it is
		// where SpeechBrokerVoiceSession::maxRequestSamples comes from - 20 s at
		// 16 kHz float is 320000 samples, the 1.28 MB the contract names as the
		// largest buffer that can ever be sent. One number, two uses, and no second
		// knob that could be set to disagree with it. When a turn reaches it, the
		// turn ENDS - the pass carries final == 1 and the next sample starts a new
		// turnId. It is never a sliding window: buffer sample zero must stay turn
		// sample zero (contract, maxRequestSamples; engine/parts.py:31-33).
		double maxUttSec{ 20.0 };

		// Nothing shorter than this is worth handing to a model. voice.json:38,
		// voice-service.py:294.
		//
		// The contract has a floor of its own - 250 ms, from engine/engine.py:110-111
		// - and the two are the same rule at two scales, so the body takes the
		// STRICTER of them and there is only ever one floor in force. With the
		// shipped values that is this one, 350 ms.
		double minUttSec{ 0.35 };

		// A buffer whose loudest sample never reached this never had speech in it,
		// and Whisper invents sentences over silence - the reference caught it
		// producing "to be continued..." over nothing at all - even with its own
		// silence filter on. voice.json:39, voice-service.py:298;
		// the reason is written out at voice-service.py:296-297 and again at
		// engine/engine.py:102-105.
		//
		// This is the one judgement about "was there speech" the ears are allowed
		// to make, and it is a measurement of a peak, not an opinion about content.
		// It stops US from producing a silent pass; it does not licence anyone
		// above to refuse one, because the contract makes a quiet buffer lawful.
		double minPeak{ 250.0 };
	};

	// What one block sounded like, and what it did.
	struct Heard
	{
		float rms{ 0.0f };     // in the unit of kRmsScale
		bool  loud{ false };   // rms >= trigger
		bool  ready{ false };  // the floor has been measured; before that nothing above is meaningful
		bool  opens{ false };  // this block is the one that opens a turn
	};

	// The noise floor, the trigger, the warm-up discard, the pre-roll and the start
	// debounce. Everything that decides whether there is speech at all - and
	// nothing that decides what to do about it, which is the Pacer's and the
	// turn's.
	//
	// THREAD: the consumer's, and only the consumer's. It has state and it takes no
	// lock, because nobody else is allowed to touch it.
	//
	// IT ALLOCATES IN Reset AND NEVER IN Offer. The pre-roll ring and the scratch
	// for the floor measurement are sized when the gate is reset and are reused for
	// the life of the device. Offer runs fifty times a second for the whole session
	// and must cost the same every time.
	class Gate
	{
	public:
		explicit Gate(const VadSettings& a_settings);

		// Start again from the warm-up: a new device has a new room, a new
		// microphone and therefore a new floor. Also what a test calls between
		// files. Allocates.
		void Reset();

		// One block of 16 kHz mono. Never allocates, never logs.
		//
		// While the floor is being measured `ready` is false and `loud` and `opens`
		// mean nothing - the caller throws the block away rather than feeding it to
		// a turn. (Throwing it away is right: the person was asked not to speak.)
		Heard Offer(std::span<const Sample> a_block) noexcept;

		// Hand over the sound kept from before the trigger and empty the ring.
		// Called exactly when Offer said `opens`, and the samples go in at the
		// front of the new turn - they are its sample zero.
		//
		// APPENDS to a_into and does not clear it, for the same reason Resampler
		// does: the caller owns one buffer for the session.
		void TakePreRoll(std::vector<Sample>& a_into);

		// The turn is over: the gate may open another. Until this is called, Offer
		// never says `opens` again - a turn is open, and the pre-roll is not kept
		// while it is (voice-service.py:277-286 keeps the rolling buffer only in
		// the not-speaking branch).
		void Close() noexcept;

		bool  Open() const noexcept { return _open; }
		bool  Ready() const noexcept { return _ready; }
		float Floor() const noexcept { return _floor; }
		float Trigger() const noexcept { return _trigger; }

		// The two measurements, in the unit of kRmsScale. Static and pure: the
		// worker above uses Peak on a finished snapshot, and it must get the same
		// number the gate would.
		static float Rms(std::span<const Sample> a_block) noexcept;
		static float Peak(std::span<const Sample> a_block) noexcept;

	private:
		VadSettings _settings;

		// The thresholds, converted from the settings ONCE. Nothing in Offer
		// divides by a thousand.
		SampleIndex _warmUp{ 0 };
		SampleIndex _floorWindow{ 0 };
		SampleIndex _startNeeded{ 0 };
		std::size_t _preRollCapacity{ 0 };

		SampleIndex _seen{ 0 };     // samples since Reset, for the warm-up and the window
		SampleIndex _above{ 0 };    // how long the sound has been above the trigger
		bool        _ready{ false };
		bool        _open{ false };
		float       _floor{ 0.0f };
		float       _trigger{ 0.0f };

		std::vector<float>  _levels;   // per-block rms during the measurement; median at the end
		std::vector<Sample> _preRoll;  // a plain ring of the last preRollMs of sound
		std::size_t         _preRollAt{ 0 };
		std::size_t         _preRollHeld{ 0 };
	};
}
