#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Envoy
{
	// Parsed settings instead of pointers into a JSON document.
	//
	// The file is read twice a session - on first use and on ReloadSettings - and
	// in the thick of things the ready-made fields are used. Before this, the
	// order of participants was pulled out of the JSON on every comparison inside
	// the bid sort, allocating a vector of strings each time; the thresholds glued
	// a pointer together out of pieces on every bid. The topic order and the names
	// of the dialogue menus were likewise fetched by pointer for every utterance -
	// now they live here too.
	//
	// The value in a field initialiser is the only fallback in the code: Read
	// lands on it when the key is absent from the file. The reference set lives in
	// envoy.default.json and is normally what ends up here; repeating the number a
	// third time inside Read is forbidden, because three copies of one default
	// drift apart at the first edit.
	class Settings
	{
	public:
		static constexpr std::size_t kNoPriority = static_cast<std::size_t>(-1);

		static const Settings& Get();
		static void            Reload();

		std::int32_t bidWindowMs{ 750 };
		// How much risk we are willing to carry when handing an utterance over.
		// What is configured is the tolerance, not the completeness threshold: the
		// threshold is worked out from this and from who is in the room.
		//
		// The value was not picked by eye. Calibration over 22 marked-up recordings
		// put the SAFE completeness threshold at 0.33 - below it cut-off phrases do
		// occur, above it not one did. The tolerance is chosen so that for the
		// cheapest room - a single ordinary subscriber of weight 0.4 - the boundary
		// lands exactly there: 0.4 * (1 - 0.33). For an expensive room the same
		// formula gives a stricter boundary by itself, 0.73.
		float        holdTolerance{ 0.27f };
		float        minUtteranceScore{ 0.4f };
		bool         sharedWinsTie{ true };
		double       utteranceTtlSec{ 30.0 };
		std::size_t  utteranceMaxStored{ 64 };

		// The log. The level and the size limit are read from here rather than from
		// the raw document: they are changed while the game runs - from the in-game
		// menu and by ReloadSettings - and the value has to live in one place.
		std::string  logLevel{ "info" };
		std::int32_t logMaxSizeKb{ 4096 };
		// Whether to write the player's WORDS into the log. Off by default, and
		// that is not a detail: otherwise a transcript of everything a person says
		// aloud at home piles up in their log folder. It is turned on knowingly -
		// from the menu or from the file.
		bool         logSpeechText{ false };

		// Which language the text on screen is in. "auto" is the language the game
		// itself runs in; a name such as "english" or "russian" pins it, which is
		// what somebody playing a Russian build but wanting English text needs.
		// Read at load only - the engine wants a restart for this as well.
		std::string  language{ "auto" };

		// Which order to try the topics in, and which menus count as dialogue. The
		// order is a rule, not a list: channel comes before combat because an
		// explicit address into a channel outranks the surroundings it was made in.
		std::vector<std::string> topicOrder{ "channel", "dialogue", "menu", "combat", "world" };
		std::vector<std::string> dialogueMenuNames{ "Dialogue Menu" };

		// Who is named the source for a capability right in the settings; empty
		// means the source is chosen by order of registration.
		std::string PrimaryAdapter(const std::string& a_capability) const;

		// Cost class: 0 - a reversible action, 1 - an expensive one.
		float MinConfidence(std::int32_t a_costClass) const;
		float MinMargin(std::int32_t a_costClass) const;

		// What one subscriber's mistake costs if it is handed a fragment of a
		// phrase.
		float HoldWeight(std::int32_t a_costClass, bool a_revocable) const;
		// Longer than this an utterance of its length class is never held.
		std::int32_t HoldCeilingMs(std::int32_t a_lengthClass) const;

		// The participant's place in the order from the settings; kNoPriority means
		// it was not named.
		std::size_t PriorityIndex(const std::string& a_ns) const;

	private:
		Settings() = default;

		static Settings& Instance();
		void             Read();

		float                    _minConfidence[2]{ 0.55f, 0.75f };
		float                    _minMargin[2]{ 0.05f, 0.15f };
		// Revocable / ordinary reversible / expensive.
		float                    _holdWeight[3]{ 0.1f, 0.4f, 1.0f };
		// Short / middle / long.
		//
		// This is not a latency budget but insurance against a silent adapter. The
		// ceiling has to outlive the arrival of the continuation: if it is shorter
		// than the pause after which the engine hands over the next piece, the hold
		// ends before we learn the very thing we were holding for.
		std::int32_t             _holdCeilingMs[3]{ 2500, 1500, 800 };
		std::vector<std::string> _priority;
		std::unordered_map<std::string, std::string> _primary;
		bool                     _read{ false };
	};
}
