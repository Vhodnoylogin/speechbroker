# -*- coding: utf-8 -*-
"""VoiceRecognizeEngine - the assembly of the reference recognition engine.

Reference only: the runtime is `adapter-voice/src/turn/Ears.cpp` and the
dispatch half under `adapter-voice/src/models/`. See the package docstring.

Four duties, and no part holds knowledge it does not need:

    raw sound and its cutting by time       SpeechTurn + Pacer
    dealing pieces out to the models        ModelPool
    the argument of the models over sound   Arbiter
    the completeness of the tail            CompletenessJudge

The engine knows nothing of the bridge: it hands pieces out through a callback,
and who takes them is not its concern. The model drivers know neither the
bridge nor the microphone. The microphone knows nothing of the models.
"""
import pathlib
import threading
from concurrent.futures import ThreadPoolExecutor
from typing import Callable, List, Optional

from .arbiter import Arbiter
from .judge import CompletenessJudge
from .models import ModelPool
from .parts import Hypothesis, Slice
from .prosody import terminal_fall
from .reputation import Reputations
from .turn import Pacer, SpeechTurn


class VoiceRecognizeEngine:
    """The assembly. Owns all the parts and keeps the order of work."""

    def __init__(self, sample_rate: int, settings: dict,
                 log: Callable[[str], None],
                 on_slice: Callable[[Slice], None],
                 on_hypotheses: Callable[[int, List[Hypothesis]], None] = None):
        self.sample_rate = sample_rate
        self._log = log
        self._on_slice = on_slice
        self._on_hypotheses = on_hypotheses or (lambda *_: None)

        where = settings.get("reputationFile")
        self.reputations = Reputations(pathlib.Path(where) if where else None,
                                       float(settings.get("defaultTrust", 0.6)),
                                       int(settings.get("minCalibrationSamples", 10)))
        self.pool = ModelPool(log, self.reputations, sample_rate)
        self.pacer = Pacer(settings.get("pacer"))
        self.judge = CompletenessJudge(settings.get("completeness"))
        self.arbiter = Arbiter(self.reputations)
        self._classes = settings.get("segments", {})

        self._turn: Optional[SpeechTurn] = None
        self._next_id = 1
        self._silence_ms = 0
        self._since_run_ms = 0
        self._lock = threading.Lock()
        self._pool_threads = ThreadPoolExecutor(max_workers=4,
                                                thread_name_prefix="audiolab-model")

    # --- the input of sound --------------------------------------------------
    def feed(self, block, loud: bool, block_ms: int) -> None:
        """The next block of sound. `loud` is decided by whoever holds the microphone."""
        with self._lock:
            if self._turn is None:
                if not loud:
                    return
                self._turn = SpeechTurn(self.sample_rate, self._classes, self._next_id)
                self._silence_ms = 0
                self._since_run_ms = 0
                self._log("speaking turn started")

            self._turn.append(block)
            self._turn.note_level(loud, block_ms, block)
            self.pacer.note(loud)
            self._silence_ms = 0 if loud else self._silence_ms + block_ms
            self._since_run_ms += block_ms

            over = self.pacer.turn_over(self._silence_ms)
            run = over or self.pacer.should_run(self._silence_ms, self._since_run_ms)
            if not run:
                return
            self._since_run_ms = 0

        self._run_pass(final=over)

        if over:
            with self._lock:
                if self._turn is not None:
                    self._next_id = self._turn.next_id
                    self._turn = None
                    self._log("speaking turn ended")

    # --- a pass ------------------------------------------------------------
    def _run_pass(self, final: bool) -> None:
        with self._lock:
            if self._turn is None:
                return
            turn = self._turn
            audio = turn.audio()
            tail_silence = self._silence_ms

        # The trailing silence is not given to the models. The calibration
        # showed that on it Whisper invents text - "To be continued..." - and
        # does so even with the silence filter on. Where the speech ends the
        # engine knows itself, so it trims it itself.
        if tail_silence > 0:
            drop = int(tail_silence * self.sample_rate / 1000)
            if 0 < drop < len(audio):
                audio = audio[:-drop]
        if len(audio) < self.sample_rate // 4:
            return

        adapters = self.pool.for_pass(final)
        if not adapters:
            return

        # The models are asked at once, not in turn: their budgets differ
        # threefold, and asking in sequence would add the latencies up instead
        # of taking the largest.
        futures = [self._pool_threads.submit(a.recognize, audio, self.sample_rate)
                   for a in adapters]
        answers = []
        for future, adapter in zip(futures, adapters):
            rep = self.reputations.of(adapter.name, adapter.klass, adapter.budget_ms)
            try:
                answer = future.result(timeout=adapter.budget_ms / 1000.0 + 1.0)
            except Exception as exc:
                rep.note_failure(timeout=True)
                self._log("  model %s did not answer: %s" % (adapter.name, exc))
                continue
            if answer.failed:
                rep.note_failure()
                self._log("  model %s: ERROR %s" % (adapter.name, answer.failed))
                continue
            rep.note_call(answer.latency_ms, [f.score for f in answer.fragments])
            answers.append(answer)

        if not answers:
            return

        spans = self.arbiter.spans(answers)
        lane = self.arbiter.merge(answers)

        # The tone of the tail we measure ourselves: the punctuation of the
        # model is language, the tone is not.
        fall = None
        if spans:
            tail = spans[-1]
            at = int(tail.start_ms * self.sample_rate / 1000)
            to = min(len(audio), int(tail.end_ms * self.sample_rate / 1000))
            if to - at > self.sample_rate // 10:
                fall = terminal_fall(audio[at:to], self.sample_rate, turn.pitch)

        with self._lock:
            if self._turn is not turn:
                return                      # the turn ended meanwhile, the answer is late
            decisions = turn.reconcile(spans, lane, self.judge, tail_silence, fall)

        for kind, *rest in decisions:
            if kind == "slice":
                self._on_slice(rest[0])
            else:
                self._on_hypotheses(rest[0], rest[1])

    # --- housekeeping ------------------------------------------------------
    def set_vocabulary(self, phrases: List[str]) -> None:
        self.pool.set_vocabulary(phrases)

    def stop(self) -> None:
        self._pool_threads.shutdown(wait=False)
        for adapter in self.pool.all():
            adapter.stop()
