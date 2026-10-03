# -*- coding: utf-8 -*-
"""The model driver of the tooling, and the pool that deals sound out to drivers.

Reference only. In the game a model is a model mod behind the contract
`adapter-voice/contract/speechbroker-voice-model.h`, and the dealing out is
`adapter-voice/src/models/`. See the package docstring.

What stays alive here is `WhisperAdapter`: the faster-whisper driver the benches
and the calibration run the weights with. It is the same code the service used,
so a word error rate measured by it is comparable with the ones recorded in
docs/measurements.md - which is the reason to keep it rather than rewrite it.

A driver knows its model and the way to reach it - weights on disk, a local
port, a child process, a remote service. The engine knows nothing of that and
sees only four methods, exactly as the bridge does not know how the engine
takes its sound.
"""
import pathlib
import re
import time
from typing import Callable, Dict, List, Optional

from .parts import Fragment, ModelAnswer
from .reputation import Reputations

# The terminal marks. The ellipsis is left out on purpose: the model puts it
# where it is itself unsure that the phrase ended.
TERMINAL = ".!?"


class ModelAdapter:
    """The link to one recognition model."""

    def __init__(self, name: str, settings: dict):
        self.name = name
        self.settings = settings
        self.klass = settings.get("class", "accurate")      # fast | accurate
        self.budget_ms = int(settings.get("budgetMs", 2000))
        self.ready = False
        self.vocabulary: List[str] = []

    # --- the life of the model ---------------------------------------------
    def start(self) -> bool:
        """Bring the model up, or attach to one already up."""
        raise NotImplementedError

    def stop(self) -> None:
        pass

    def set_vocabulary(self, phrases: List[str]) -> None:
        """A hint: which phrases to expect. The model is free not to use it."""
        self.vocabulary = list(phrases)

    # --- work --------------------------------------------------------------
    def recognize(self, pcm, sample_rate: int) -> ModelAnswer:
        """Re-read ALL the sound given and return its division into fragments."""
        raise NotImplementedError

    def __repr__(self) -> str:
        return "<%s %s, budget %d ms>" % (self.name, self.klass, self.budget_ms)


class WhisperAdapter(ModelAdapter):
    """faster-whisper: weights on disk, work on the GPU.

    One model - one driver. The shared cache of weights below is only for the
    case when two drivers look at ONE path, so as not to load the same thing
    twice; as a design device it is no good - the calibration showed that two
    settings over the same weights give a difference in latency of one and a
    half times and the same accuracy, that is no division into fast and
    accurate at all.

    `weights` is the folder that holds `model.bin` and the tokenizer of a
    CTranslate2 conversion; whoever constructs the driver resolves it
    (`audiolab.manifest`), the driver itself does not know where models live.
    """

    _shared: Dict[str, object] = {}     # the weights are loaded once per path

    def __init__(self, name: str, settings: dict, weights: pathlib.Path):
        super().__init__(name, settings)
        self.weights = pathlib.Path(weights)
        self.model = None

    def start(self) -> bool:
        from faster_whisper import WhisperModel

        if not (self.weights / "model.bin").exists():
            raise FileNotFoundError(
                "no model.bin under %s - see `weightsRoot` and `models.%s.weights` in "
                "audio.json, or the AUDIOLAB_WEIGHTS override" % (self.weights, self.name))
        path = str(self.weights.resolve())
        key = "%s|%s|%s" % (path, self.settings.get("device", "cuda"),
                            self.settings.get("compute", "float16"))
        if key not in WhisperAdapter._shared:
            WhisperAdapter._shared[key] = WhisperModel(
                path,
                device=self.settings.get("device", "cuda"),
                compute_type=self.settings.get("compute", "float16"))
        self.model = WhisperAdapter._shared[key]
        self.ready = True
        return True

    def recognize(self, pcm, sample_rate: int) -> ModelAnswer:
        started = time.time()
        try:
            segments, _ = self.model.transcribe(
                pcm,
                language=self.settings.get("language") or None,
                beam_size=int(self.settings.get("beamSize", 1)),
                word_timestamps=bool(self.settings.get("wordTimestamps", False)),
                condition_on_previous_text=False,
                vad_filter=bool(self.settings.get("vadFilter", True)),
                initial_prompt=self._prompt())
            fragments = []
            for seg in segments:
                fragments.extend(self._split(seg))
        except Exception as exc:                       # one bad utterance must not
            return ModelAnswer(self.name, 0, [], str(exc))   # bring the whole engine down

        return ModelAnswer(self.name, int((time.time() - started) * 1000), fragments)

    def _prompt(self) -> Optional[str]:
        """The vocabulary as a prompt: the model hears more readily what it is told to expect."""
        if not self.vocabulary or not self.settings.get("usePrompt", True):
            return None
        return ". ".join(self.vocabulary[:40])

    def _split(self, seg) -> List[Fragment]:
        """Break a segment of the model into sentences.

        The segment boundaries of Whisper do not always match sentence
        boundaries: short ones it puts into one segment, and at the edge of its
        thirty-second window it cuts mid-phrase. So we cut additionally on the
        terminal mark, and take the time of the cut from the word timings when
        there are any.
        """
        words = list(getattr(seg, "words", None) or [])
        text = seg.text.strip()
        if not text:
            return []

        common = dict(
            score=float(_prob(seg.avg_logprob)),
            no_speech_prob=float(getattr(seg, "no_speech_prob", 0.0) or 0.0),
        )

        if not words:
            # Without word timings we cut on the marks only, and divide the time
            # proportionally to the length of the piece: there is nowhere to
            # take anything more accurate from.
            pieces = _sentences(text)
            if len(pieces) <= 1:
                return [Fragment(int(seg.start * 1000), int(seg.end * 1000), text,
                                 ends_sentence=text[-1] in TERMINAL,
                                 words=len(text.split()), **common)]
            out, at = [], seg.start
            span = (seg.end - seg.start) / max(1, len(text))
            for piece in pieces:
                end = at + span * len(piece)
                out.append(Fragment(int(at * 1000), int(end * 1000), piece.strip(),
                                    ends_sentence=piece.strip()[-1] in TERMINAL,
                                    words=len(piece.split()), **common))
                at = end
            return out

        # With word timings: accumulate words until a terminal mark is met.
        out, bucket = [], []
        for word in words:
            bucket.append(word)
            if word.word.strip().endswith(tuple(TERMINAL)):
                out.append(_from_words(bucket, True, common))
                bucket = []
        if bucket:
            out.append(_from_words(bucket, False, common))
        return out


def _from_words(bucket, ends: bool, common: dict) -> Fragment:
    text = "".join(w.word for w in bucket).strip()
    gaps = [int((b.start - a.end) * 1000) for a, b in zip(bucket, bucket[1:])]
    gaps = [g for g in gaps if g >= 0]
    return Fragment(
        start_ms=int(bucket[0].start * 1000),
        end_ms=int(bucket[-1].end * 1000),
        text=text,
        ends_sentence=ends,
        last_word_prob=float(getattr(bucket[-1], "probability", -1.0) or -1.0),
        median_gap_ms=int(sorted(gaps)[len(gaps) // 2]) if gaps else 0,
        words=len(bucket),
        **common)


def _sentences(text: str) -> List[str]:
    parts = re.split(r"(?<=[.!?])\s+", text)
    return [p for p in parts if p.strip()]


def _prob(avg_logprob) -> float:
    import math
    try:
        return math.exp(float(avg_logprob))
    except Exception:
        return 0.0


class ModelPool:
    """Who is registered, and who gets what.

    Dealing out goes by class: short pieces to the fast ones, long ones to the
    accurate ones. A model that did not come up is excluded from the dealing
    and this is said out loud: working silently without the accurate model is
    not allowed, otherwise the analysis of quality becomes guesswork.
    """

    def __init__(self, log: Callable[[str], None], reputations: Reputations = None,
                 sample_rate: int = 16000):
        self._log = log
        self._adapters: List[ModelAdapter] = []
        self.reputations = reputations or Reputations()
        self._sample_rate = sample_rate

    def register(self, adapter: ModelAdapter) -> bool:
        try:
            if not adapter.start():
                raise RuntimeError("did not come up")
        except Exception as exc:
            self._log("  model %s did NOT come up: %s" % (adapter.name, exc))
            return False

        self._adapters.append(adapter)
        rep = self.reputations.of(adapter.name, adapter.klass, adapter.budget_ms)
        self._log("  model %s ready: declared class %s, budget %d ms"
                  % (adapter.name, adapter.klass, adapter.budget_ms))
        self._probe_silence(adapter, rep)
        return True

    def _probe_silence(self, adapter: ModelAdapter, rep) -> None:
        """Slip in a buffer known to be empty and see what comes back.

        The cheapest honesty check there is, and it catches the most harmful
        kind of defect. Models come to us as other people's programs: there is
        no reason to believe the declaration, while silence we make ourselves
        and know for certain there is nothing in it. Whisper stumbles on this
        check regularly - in the very first run it produced "To be continued..."
        on silence.
        """
        import numpy as np

        quiet = np.zeros(self._sample_rate, dtype="float32")
        answer = adapter.recognize(quiet, self._sample_rate)
        invented = bool(answer.fragments) and any(f.text.strip() for f in answer.fragments)
        rep.note_silence(invented)
        if invented:
            said = " | ".join(f.text.strip() for f in answer.fragments if f.text.strip())
            self._log("    INVENTS ON SILENCE: %s" % said[:80])
        else:
            self._log("    passed the silence check")

    def set_vocabulary(self, phrases: List[str]) -> None:
        for adapter in self._adapters:
            adapter.set_vocabulary(phrases)

    def all(self) -> List[ModelAdapter]:
        return list(self._adapters)

    def measured_class(self, adapter: ModelAdapter) -> str:
        """The class by measurement, not by declaration: anything can be promised."""
        return self.reputations.of(adapter.name).measured_class()

    def for_pass(self, final: bool) -> List[ModelAdapter]:
        """Whom to ask on this pass.

        Intermediate passes - the fast ones only: their answer is wanted now,
        and the accurate one would not make it anyway. The last pass of the
        turn - everybody: there is nowhere left to hurry, and quality matters
        more here.
        """
        if final:
            return self.all()
        fast = [a for a in self._adapters if self.measured_class(a) == "fast"]
        return fast or self.all()

    def __len__(self) -> int:
        return len(self._adapters)
