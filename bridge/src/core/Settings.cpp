#include "Settings.h"

#include "Config.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace Envoy
{
	Settings& Settings::Instance()
	{
		static Settings instance;
		if (!instance._read) {
			instance.Read();
		}
		return instance;
	}

	const Settings& Settings::Get()
	{
		return Instance();
	}

	void Settings::Reload()
	{
		Instance().Read();
	}

	float Settings::MinConfidence(std::int32_t a_costClass) const
	{
		return _minConfidence[a_costClass == 1 ? 1 : 0];
	}

	float Settings::MinMargin(std::int32_t a_costClass) const
	{
		return _minMargin[a_costClass == 1 ? 1 : 0];
	}

	float Settings::HoldWeight(std::int32_t a_costClass, bool a_revocable) const
	{
		// Expensive stays expensive even when it declares itself revocable: a
		// thrown spell cannot be called back, whatever the mod claims about it.
		if (a_costClass >= 1) {
			return _holdWeight[2];
		}
		return a_revocable ? _holdWeight[0] : _holdWeight[1];
	}

	std::int32_t Settings::HoldCeilingMs(std::int32_t a_lengthClass) const
	{
		const auto index = a_lengthClass < 0 ? 0 : (a_lengthClass > 2 ? 2 : a_lengthClass);
		return _holdCeilingMs[index];
	}

	std::string Settings::PrimaryAdapter(const std::string& a_capability) const
	{
		const auto it = _primary.find(a_capability);
		return it == _primary.end() ? std::string{} : it->second;
	}

	std::size_t Settings::PriorityIndex(const std::string& a_ns) const
	{
		const auto it = std::find(_priority.begin(), _priority.end(), a_ns);
		return it == _priority.end() ? kNoPriority
		                             : static_cast<std::size_t>(std::distance(_priority.begin(), it));
	}

	void Settings::Read()
	{
		const auto& cfg = Config::Get();

		// value_or(the field) - the fallback comes from the field initialiser, and
		// there is no second copy of the number here.
		bidWindowMs        = cfg.Value<std::int32_t>("/auction/bidWindowMs").value_or(bidWindowMs);
		minUtteranceScore  = cfg.Value<float>("/auction/minUtteranceScore").value_or(minUtteranceScore);
		sharedWinsTie      = cfg.Value<bool>("/auction/sharedWinsTie").value_or(sharedWinsTie);
		_minConfidence[0]  = cfg.Value<float>("/auction/minConfidence/reversible").value_or(_minConfidence[0]);
		_minConfidence[1]  = cfg.Value<float>("/auction/minConfidence/costly").value_or(_minConfidence[1]);
		_minMargin[0]      = cfg.Value<float>("/auction/minMargin/reversible").value_or(_minMargin[0]);
		_minMargin[1]      = cfg.Value<float>("/auction/minMargin/costly").value_or(_minMargin[1]);
		_priority          = cfg.Value<std::vector<std::string>>("/auction/priority").value_or(_priority);
		holdTolerance      = cfg.Value<float>("/auction/hold/tolerance").value_or(holdTolerance);
		_holdWeight[0]     = cfg.Value<float>("/auction/hold/weight/revocable").value_or(_holdWeight[0]);
		_holdWeight[1]     = cfg.Value<float>("/auction/hold/weight/plain").value_or(_holdWeight[1]);
		_holdWeight[2]     = cfg.Value<float>("/auction/hold/weight/costly").value_or(_holdWeight[2]);
		_holdCeilingMs[0]  = cfg.Value<std::int32_t>("/auction/hold/ceilingMs/short").value_or(_holdCeilingMs[0]);
		_holdCeilingMs[1]  = cfg.Value<std::int32_t>("/auction/hold/ceilingMs/middle").value_or(_holdCeilingMs[1]);
		_holdCeilingMs[2]  = cfg.Value<std::int32_t>("/auction/hold/ceilingMs/long").value_or(_holdCeilingMs[2]);
		utteranceTtlSec    = cfg.Value<double>("/utterance/ttlSec").value_or(utteranceTtlSec);
		utteranceMaxStored = cfg.Value<std::size_t>("/utterance/maxStored").value_or(utteranceMaxStored);
		topicOrder         = cfg.Value<std::vector<std::string>>("/topics/order").value_or(topicOrder);
		dialogueMenuNames  = cfg.Value<std::vector<std::string>>("/topics/dialogueMenuNames")
		                        .value_or(dialogueMenuNames);
		_primary           = cfg.Value<std::unordered_map<std::string, std::string>>("/adapters/primary")
		                        .value_or(_primary);
		logLevel           = cfg.Value<std::string>("/log/level").value_or(logLevel);
		logMaxSizeKb       = cfg.Value<std::int32_t>("/log/maxSizeKb").value_or(logMaxSizeKb);
		logSpeechText      = cfg.Value<bool>("/log/speechText").value_or(logSpeechText);
		language           = cfg.Value<std::string>("/language").value_or(language);

		_read = true;

		// There are more keys in the file than the bridge reads today: the rest are
		// points of extension, and by sight they are indistinguishable from working
		// settings. So the ones that were read are named out loud - otherwise
		// somebody tuning the thing turns a knob that is connected to nothing and
		// never finds out.
		spdlog::info(
			"settings read: bid window {} ms, utterance threshold {:.2f}, "
			"confidence {:.2f}/{:.2f}, margin {:.2f}/{:.2f}, sharing takes a tie: {}, "
			"{} participants in the order, kept {:.0f} s and at most {} utterances, "
			"hold tolerance {:.2f}, cost of a mistake {:.2f}/{:.2f}/{:.2f}, "
			"ceiling {}/{}/{} ms",
			bidWindowMs, minUtteranceScore, _minConfidence[0], _minConfidence[1],
			_minMargin[0], _minMargin[1], sharedWinsTie ? "yes" : "no", _priority.size(),
			utteranceTtlSec, utteranceMaxStored, holdTolerance,
			_holdWeight[0], _holdWeight[1], _holdWeight[2],
			_holdCeilingMs[0], _holdCeilingMs[1], _holdCeilingMs[2]);
	}
}
