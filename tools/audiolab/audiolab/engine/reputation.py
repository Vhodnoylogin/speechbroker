# -*- coding: utf-8 -*-
"""The reputation of a model: what was MEASURED against what was DECLARED.

Reference only: the runtime is `adapter-voice/src/models/Reputation.cpp`, and
that is where routing by reputation happens in the game. See the package docstring.

Two halves live here, and only one of them has a job in the tooling.

THE HALF WITH A JOB: `Reputations.load`/`save` and the fields `trust`,
`declared_class` and `calibration_scores`. That is what PRODUCES `reputations.json`
from a calibration run - the trust per model and the distribution of its scores
on the labelled corpus, which the adapter ships inside its mod and reads at
start (Reputation.h, `ReputationSettings::file`). `audiolab.calibrate.remember`
is the writer.

THE OTHER HALF - the observations, `normalize`, `weight`, `measured_class` - is
kept only so that the reference engine replays a take exactly the way it was
measured: the arbiter needs `normalize` and `weight` to fold hypotheses, and
without them the replay reports would not match the numbers in
docs/measurements.md. Nothing in the game reads any of it from python.

The reasoning, kept from the service. Models come to us as external programs.
We did not write them, we know almost nothing about them, and all we know is
what we made their author declare in the contract - and there is no reason to
believe they neither erred nor cheated. Hence the rule: DECLARED IS A HINT,
MEASURED IS A FACT. Pieces are dealt out by reputation, not by declaration.
An adapter that promised 400 ms and gives 2000 quietly moves into the accurate
class; one that invents on silence loses weight.

What a reputation does NOT do: it does not judge the correctness of a
transcription on live speech - there is no reference there. Correctness is
measured separately, on a corpus known in advance (`audiolab.calibrate`), and
arrives here as the field `trust`.
"""
import json
import pathlib
from dataclasses import dataclass, field
from typing import Dict, List, Optional

KEEP = 200          # how many of the latest observations are remembered


@dataclass
class ModelReputation:
    """The observations of one model. Belongs to the engine, not to the driver."""

    name: str
    declared_class: str = "accurate"
    declared_budget_ms: int = 2000

    calls: int = 0
    failures: int = 0
    timeouts: int = 0
    latencies: List[int] = field(default_factory=list)
    scores: List[float] = field(default_factory=list)

    silence_probes: int = 0
    silence_inventions: int = 0

    # Trust from the calibration against a reference: 1.0 - the transcription
    # matched word for word, 0.0 - nothing matched. None until calibrated.
    trust: Optional[float] = None

    # What to believe until there has been a calibration. The value comes from
    # the settings: it used to be 0.6 right in the code, and every score of any
    # model we simply had not measured yet was silently cut by 40%.
    default_trust: float = 0.6

    # The distribution of scores taken at calibration. This is NOT a measurement
    # of the last run but a reference: by it the raw number of the model is
    # turned into its place among its own answers. It lives between runs so
    # that normalisation works from the FIRST phrase of a session, not the tenth.
    calibration_scores: List[float] = field(default_factory=list)

    # --- observations ------------------------------------------------------
    def note_call(self, latency_ms: int, scores: List[float]) -> None:
        self.calls += 1
        self.latencies = (self.latencies + [latency_ms])[-KEEP:]
        self.scores = (self.scores + list(scores))[-KEEP:]

    def note_failure(self, timeout: bool = False) -> None:
        self.calls += 1
        self.failures += 1
        if timeout:
            self.timeouts += 1

    def note_silence(self, invented: bool) -> None:
        self.silence_probes += 1
        if invented:
            self.silence_inventions += 1

    # --- measured ----------------------------------------------------------
    def latency(self, percentile: float = 0.9) -> int:
        if not self.latencies:
            return self.declared_budget_ms
        ordered = sorted(self.latencies)
        return ordered[min(len(ordered) - 1, int(len(ordered) * percentile))]

    def measured_class(self, fast_below_ms: int = 800) -> str:
        """The class by measurement, not by declaration."""
        if len(self.latencies) < 5:
            return self.declared_class
        return "fast" if self.latency(0.9) <= fast_below_ms else "accurate"

    def invention_rate(self) -> float:
        return self.silence_inventions / self.silence_probes if self.silence_probes else 0.0

    # Below this many observations there is nothing to normalise against: a
    # place in a distribution of three points is not a measurement but the look
    # of one. It is a threshold of trust in the CORPUS, not a waiting time: the
    # calibration hands over all the recordings it found in the folder at once,
    # and normalisation works from the first phrase.
    min_sample: int = 10

    def distribution(self) -> List[float]:
        """What to normalise against: the live observations or the calibration.

        Live ones are more accurate - they are from this microphone and this
        room - but while there are few of them, the distribution from the
        calibration works. Without it normalisation stayed silent until the
        tenth phrase, and the same command at the start of a session and a
        minute later got different numbers.
        """
        if len(self.scores) >= self.min_sample:
            return self.scores
        return self.calibration_scores

    def normalize(self, raw: float) -> float:
        """Turn the score of a model into its place in its OWN distribution.

        The raw numbers of different models are not comparable: 0.9 from one
        and 0.9 from another mean different things. A place in one's own
        distribution is comparable: "higher than in four cases out of five"
        means the same for anybody.
        """
        pool = self.distribution()
        if len(pool) < self.min_sample:
            return raw
        below = sum(1 for s in pool if s <= raw)
        return below / float(len(pool))

    def weight(self) -> float:
        """How much to believe this model IN AN ARGUMENT.

        This is the weight of a vote, not a multiplier of the score. The score
        itself used to be multiplied by it, and it came out that a model right
        nine times out of ten cuts a tenth off every number of its own: a phrase
        recognised word for word drifted further from the threshold the more
        honest the calibration was. Lowering a score for being right is
        nonsense, and it ended here.
        """
        value = self.trust if self.trust is not None else self.default_trust
        if self.calls:
            value *= 1.0 - min(0.5, self.failures / float(self.calls))
        value *= 1.0 - min(0.8, self.invention_rate())
        return max(0.05, min(1.0, value))


class Reputations:
    """All reputations at once, kept between runs. This is what writes reputations.json."""

    def __init__(self, path: pathlib.Path = None, default_trust: float = 0.6,
                 min_sample: int = 10):
        self._path = path
        self._default_trust = default_trust
        self._min_sample = min_sample
        self._by_name: Dict[str, ModelReputation] = {}
        if path and path.exists():
            self.load()

    @property
    def path(self) -> Optional[pathlib.Path]:
        return self._path

    def of(self, name: str, declared_class: str = "accurate",
           declared_budget_ms: int = 2000) -> ModelReputation:
        rep = self._by_name.get(name)
        if rep is None:
            rep = ModelReputation(name, declared_class, declared_budget_ms)
            rep.default_trust = self._default_trust
            rep.min_sample = self._min_sample
            self._by_name[name] = rep
        return rep

    def all(self) -> List[ModelReputation]:
        return list(self._by_name.values())

    def load(self) -> None:
        try:
            raw = json.loads(self._path.read_text(encoding="utf-8"))
        except Exception:
            return
        for name, item in raw.items():
            if name.startswith("_") or not isinstance(item, dict):
                continue
            rep = self.of(name)
            rep.trust = item.get("trust")
            rep.declared_class = item.get("declaredClass", rep.declared_class)
            rep.calibration_scores = list(item.get("calibrationScores", []))

    def save(self) -> None:
        """Only the long-lived is saved outward - the trust from calibration.

        The latencies and the share of inventions of the last run are not worth
        carrying over: the hardware and the room change, and a saved number
        would look like a measurement without being one.

        The keys are the ids of the models exactly as their listings declare
        them (`model-whisper-ru/models/*.json`): the adapter looks a model up
        by its id, and a file keyed any other way is a file it cannot use.
        """
        if not self._path:
            return
        out = {rep.name: {"trust": rep.trust,
                          "declaredClass": rep.declared_class,
                          "calibrationScores": [round(s, 4) for s in rep.calibration_scores]}
               for rep in self.all()}
        self._path.write_text(json.dumps(out, ensure_ascii=False, indent=2) + "\n",
                              encoding="utf-8")
