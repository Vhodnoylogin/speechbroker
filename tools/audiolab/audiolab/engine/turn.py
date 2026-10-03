# -*- coding: utf-8 -*-
"""One speaking turn: what has been said, what has been handed out, and when to ask the models.

Reference only: the runtime is `adapter-voice/src/turn/{Pacer,SpeechTurn}.cpp`.
See the package docstring.

A turn lives from the first loud block to a long silence. All that time it
accumulates sound from zero and remembers which pieces have already gone to the
bridge. The models always receive the whole turn, so each of their answers is a
complete re-reading, comparable both with the other answers and with what has
already been handed out.
"""
from typing import List, Optional, Tuple

from .parts import Fragment, Hypothesis, Slice
from .prosody import PitchRange, track_f0


class Pacer:
    """When to ask the models.

    Three rules, and the first is the main one: a short pause. It costs nothing
    when there is none - people speak without a break, so there will be one
    pass, at the end, exactly as before. The word ceiling is for those who never
    pause at all: without it a long unbroken speech would give a single pass
    and the whole delay in one go.
    """

    def __init__(self, settings: dict = None):
        s = settings or {}
        self.slice_silence_ms = int(s.get("sliceSilenceMs", 300))
        self.end_silence_ms = int(s.get("endSilenceMs", 1600))
        self.max_span_ms = int(s.get("maxSpanMs", 4000))

        self._armed = False

    def note(self, loud: bool) -> None:
        """Speech arms the pacer. Without this the pause would fire on EVERY
        block of silence in a row: the condition "silence longer than the
        threshold" stays true until the end of the turn, and the first pass
        turned into twenty."""
        if loud:
            self._armed = True

    def should_run(self, silence_ms: int, since_last_run_ms: int) -> bool:
        if self._armed and silence_ms >= self.slice_silence_ms:
            self._armed = False
            return True
        return since_last_run_ms >= self.max_span_ms

    def turn_over(self, silence_ms: int) -> bool:
        return silence_ms >= self.end_silence_ms


class SpeechTurn:
    """The sound of the turn, the clock of the turn, and the memory of what has been handed out."""

    def __init__(self, sample_rate: int, classes: dict = None, next_id: int = 1):
        c = classes or {}
        self.sample_rate = sample_rate
        self.short_max_words = int(c.get("shortMaxWords", 3))
        self.middle_max_words = int(c.get("middleMaxWords", 8))
        self.emitted: List[Slice] = []
        self._next_id = next_id
        self._pcm: List = []
        self._samples = 0

        # The anchors are the edges of the pauses, measured FROM THE SOUND. They
        # are the most stable numbers we have: computed from energy, they depend
        # on no model and do not drift between passes. The timings of the models
        # float by hundreds of milliseconds, so the identification of pieces
        # stands on the anchors and not on them.
        self.anchors: List[int] = [0]
        self.pitch = PitchRange()
        self._quiet_ms = 0
        self._was_quiet = True

    # --- sound -------------------------------------------------------------
    def append(self, block) -> None:
        self._pcm.append(block)
        self._samples += len(block)

    def note_level(self, loud: bool, block_ms: int, block=None) -> None:
        """Note the loudness of a block: this is where the anchors and the range of the voice come from."""
        at = self.elapsed_ms
        if loud:
            if self._was_quiet and self._quiet_ms >= 150:
                self._anchor(at - block_ms)      # speech resumed
            self._quiet_ms = 0
            self._was_quiet = False
            if block is not None:
                self.pitch.add(track_f0(block, self.sample_rate))
        else:
            if not self._was_quiet:
                self._anchor(at - block_ms)      # speech broke off
            self._quiet_ms += block_ms
            self._was_quiet = True

    def _anchor(self, ms: int) -> None:
        ms = max(0, ms)
        if not self.anchors or abs(self.anchors[-1] - ms) > 60:
            self.anchors.append(ms)

    def snap(self, ms: int, slack_ms: int = 350) -> int:
        """Pull a model's time to the nearest anchor.

        The model decides WHERE the boundary runs; the engine decides WHAT
        NUMBERS name it. Without this the same "fireball" arrived as [0..842],
        [0..1305] and [0..580] - and counted as a new piece every time.
        """
        best, best_gap = ms, slack_ms + 1
        for anchor in self.anchors + [self.elapsed_ms]:
            gap = abs(anchor - ms)
            if gap < best_gap:
                best, best_gap = anchor, gap
        return best

    def audio(self):
        import numpy as np
        return np.concatenate(self._pcm) if self._pcm else np.zeros(0, dtype="float32")

    @property
    def elapsed_ms(self) -> int:
        return int(self._samples * 1000 / self.sample_rate)

    @property
    def next_id(self) -> int:
        return self._next_id

    # --- reconciliation ----------------------------------------------------
    def classify(self, words: int) -> str:
        if words <= self.short_max_words:
            return "short"
        if words <= self.middle_max_words:
            return "middle"
        return "long"

    def reconcile(self, spans: List[Fragment], lane: List[List[Hypothesis]],
                  judge, tail_silence_ms: int, terminal_fall: float = None) -> List[Tuple]:
        """Reconcile a fresh re-reading with what has already been handed out.

        Returns decisions of one of two kinds:
            ("slice", Slice)                  - a new piece for the bridge
            ("hypotheses", id, [Hypothesis])  - new guesses about an old piece

        Note what is NOT here: the engine does not decide whether a span has
        been spent. It only reports honestly which pieces the new one swallows,
        and what to do about it - cancel a held one or leave an issued one -
        only the bridge knows: it holds the state of the play, we do not.
        """
        decisions: List[Tuple] = []

        for i, frag in enumerate(spans):
            hypotheses = lane[i] if i < len(lane) else []
            if not hypotheses:
                continue

            frag.start_ms = self.snap(frag.start_ms)
            frag.end_ms = self.snap(frag.end_ms)
            if frag.end_ms <= frag.start_ms:
                continue

            has_after = i + 1 < len(spans)
            silence_after = (spans[i + 1].start_ms - frag.end_ms) if has_after else tail_silence_ms
            complete = judge.judge(frag, has_after, silence_after,
                                   None if has_after else terminal_fall)

            same = self._same_span(frag)
            if same is not None:
                if _same_text(same.hypotheses, hypotheses):
                    continue                       # no news, stay silent
                decisions.append(("hypotheses", same.id, hypotheses))
                continue

            covered = self._covered_by(frag)
            piece = Slice(
                id=self._next_id,
                start_ms=frag.start_ms,
                end_ms=frag.end_ms,
                hypotheses=hypotheses,
                complete=complete,
                supersedes=[s.id for s in covered],
                speech_elapsed_ms=frag.end_ms,
                length_class=self.classify(frag.words or len(hypotheses[0].text.split())))
            self._next_id += 1
            self.emitted.append(piece)
            decisions.append(("slice", piece))

        return decisions

    # --- search by time ----------------------------------------------------
    def _same_span(self, frag: Fragment, slack_ms: int = 200) -> Optional[Slice]:
        """An already issued piece in the same place. The bounds of the models
        wander by tens of milliseconds from pass to pass, hence the tolerance."""
        for piece in self.emitted:
            if (abs(piece.start_ms - frag.start_ms) <= slack_ms and
                    abs(piece.end_ms - frag.end_ms) <= slack_ms):
                return piece
        return None

    def _covered_by(self, frag: Fragment, slack_ms: int = 200) -> List[Slice]:
        """Issued pieces lying wholly inside the new fragment."""
        out = []
        for piece in self.emitted:
            if (piece.start_ms >= frag.start_ms - slack_ms and
                    piece.end_ms <= frag.end_ms + slack_ms and
                    piece.duration_ms < frag.duration_ms - slack_ms):
                out.append(piece)
        return out


def _same_text(old: List[Hypothesis], fresh: List[Hypothesis]) -> bool:
    if not old or not fresh:
        return False
    return old[0].key() == fresh[0].key()
