# -*- coding: utf-8 -*-
"""What speech is made of on its way from the microphone to the bridge.

Reference only: the runtime is `adapter-voice/contract/speechbroker-voice-model.h`
and `adapter-voice/src/turn/`. See the package docstring.

One rule holds the whole scheme together: a piece is identified by its TIME
inside the speaking turn, not by its text and not by an ordinal. Then "model A
takes the piece to stand alone, model B takes it to be the start of the next
one" stops being an argument about meaning and becomes a disagreement over one
number - where the boundary lies.
"""
from dataclasses import dataclass, field
from typing import List, Optional


@dataclass
class Hypothesis:
    """One guess at what was said."""

    text: str
    score: float = 0.0
    model: str = ""          # who guessed it
    agreed: int = 1          # how many models said the same

    def key(self) -> str:
        """The key for comparing hypotheses across models: case and punctuation do not count."""
        return "".join(c for c in self.text.lower() if c.isalnum() or c.isspace()).strip()


@dataclass
class Fragment:
    """A piece of the turn as one model saw it.

    The bounds are milliseconds from the start of the speaking turn, not from
    the start of the buffer: the buffer and the turn begin together, but this
    way there are fewer chances to get it wrong.
    """

    start_ms: int
    end_ms: int
    text: str
    score: float = 0.0
    ends_sentence: bool = False   # the model closed it with . ! or ?
    last_word_prob: float = -1.0  # the available stand-in for "the probability of the full stop"
    no_speech_prob: float = 0.0
    median_gap_ms: int = 0        # the median gap between the words inside it
    words: int = 0

    @property
    def duration_ms(self) -> int:
        return self.end_ms - self.start_ms


@dataclass
class ModelAnswer:
    """A complete re-reading of the whole turn by one model.

    COMPLETE is the point: the models are always given the buffer from zero, so
    any two answers are comparable with each other and with what has already
    been handed out. The question "did it return B or A+B" does not exist - it
    always returns everything, cut its own way.
    """

    model: str
    latency_ms: int
    fragments: List[Fragment] = field(default_factory=list)
    failed: Optional[str] = None


@dataclass
class Slice:
    """What goes to the bridge. Nothing else leaves the engine."""

    id: int
    start_ms: int
    end_ms: int
    hypotheses: List[Hypothesis] = field(default_factory=list)
    complete: float = 1.0
    supersedes: List[int] = field(default_factory=list)
    speech_elapsed_ms: int = 0
    length_class: str = "short"    # short | middle | long
    # When the piece was EMITTED, counted from the start of the sound. Not the
    # same as end_ms: between the end of the speech and the emission lies the
    # pause by which the engine understood that the piece was ready to go. The
    # difference between the emissions of two pieces is exactly the time the
    # bridge may spend holding the first one back.
    emitted_ms: int = 0

    @property
    def text(self) -> str:
        return self.hypotheses[0].text if self.hypotheses else ""

    @property
    def duration_ms(self) -> int:
        return self.end_ms - self.start_ms

    def as_json(self) -> dict:
        return {
            "id": self.id,
            "text": self.text,
            "hypotheses": [{"text": h.text, "score": round(h.score, 3),
                            "model": h.model, "agreed": h.agreed} for h in self.hypotheses],
            "score": round(self.hypotheses[0].score, 3) if self.hypotheses else 0.0,
            "complete": round(self.complete, 3),
            "supersedes": self.supersedes,
            "speechElapsedMs": self.speech_elapsed_ms,
            "durationMs": self.duration_ms,
            "lengthClass": self.length_class,
            "emittedMs": self.emitted_ms,
        }
