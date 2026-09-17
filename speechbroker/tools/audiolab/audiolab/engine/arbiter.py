# -*- coding: utf-8 -*-
"""The argument of several models over the same sound.

Reference only: the runtime is `adapter-voice/src/models/Arbiter.cpp`.
See the package docstring.

Only two things are comparable to the arbiter. The scores of different models
are not comparable with each other - 0.9 from one and 0.9 from another mean
different things, and calibrating them against each other is on the conscience
of whoever tunes them. What IS comparable:

  * AGREEMENT - an independent match of the text between two models is the
    best evidence there is here at all;
  * the CLASS of the model from the settings - what to believe, all else equal.
"""
from typing import Dict, List

from .parts import Fragment, Hypothesis, ModelAnswer


class Arbiter:
    """Folds the re-readings of several models into one lane of fragments."""

    def __init__(self, reputations=None):
        # The weight comes from the REPUTATION, not from the settings: there is
        # no reason to believe the declared class, while the measured behaviour
        # and the trust from calibration are ours.
        self._reputations = reputations

    def base(self, answers: List[ModelAnswer]) -> ModelAnswer:
        """Whose answer sets the lane of spans.

        Not the one that cut finest - that was a mistake. Fineness of cutting
        says nothing about the accuracy of the boundaries, and the choice
        "finest of all" changed from pass to pass: the lane slid, and the
        hypotheses of one phrase stuck to the span of another.

        The lane is set by the model WITH WORD TIMINGS. Its boundaries come
        from aligning the sound, not from dividing proportionally to the length
        of the string, so they are reproducible - and the lane has to be
        reproducible, otherwise nothing already handed out can be recognised
        on it.
        """
        alive = [a for a in answers if a.failed is None and a.fragments]
        if not alive:
            return None
        timed = [a for a in alive
                 if any(f.median_gap_ms > 0 or f.last_word_prob >= 0.0 for f in a.fragments)]
        pool = timed or alive
        return max(pool, key=lambda a: len(a.fragments))

    def merge(self, answers: List[ModelAnswer], overlap_ms: int = 250) -> List[List[Hypothesis]]:
        """Fold the answers into a lane: a list of spans, each with its hypotheses."""
        alive = [a for a in answers if a.failed is None and a.fragments]
        base = self.base(answers)
        if base is None:
            return []

        lane: List[List[Hypothesis]] = []

        for frag in base.fragments:
            bucket: List[Hypothesis] = []
            for answer in alive:
                match = _overlapping(answer.fragments, frag, overlap_ms)
                if match is None:
                    continue
                bucket.append(Hypothesis(text=match.text,
                                         score=self._score(answer, match.score),
                                         model=answer.model))
            lane.append(self._fold(bucket))
        return lane

    def spans(self, answers: List[ModelAnswer]) -> List[Fragment]:
        """The spans of the lane, in the same order as merge."""
        base = self.base(answers)
        return list(base.fragments) if base else []

    def _score(self, answer: ModelAnswer, raw: float) -> float:
        """The raw number of a model into a comparable quantity.

        What is comparable is the place in the model's OWN distribution: "higher
        than in four cases out of five" means the same thing for anybody, while
        0.9 from one model and 0.9 from another mean different things.

        Trust is no longer here. It decides whose voice weighs more IN THE
        ARGUMENT, and those are different things: multiplying the score by it,
        we lowered the confidence for the model being OFTEN RIGHT - and a
        phrase recognised word for word never reached the threshold of the
        auction.
        """
        if self._reputations is None:
            return raw
        return self._reputations.of(answer.model).normalize(raw)

    def _weight(self, model: str) -> float:
        """The weight of this model's voice in the argument."""
        if self._reputations is None:
            return 1.0
        return self._reputations.of(model).weight()

    def _fold(self, bucket: List[Hypothesis]) -> List[Hypothesis]:
        """The same text from different models is not doubled but strengthened.

        Two models that arrived independently at one string are a far stronger
        ground than one confident model. So matching hypotheses are merged into
        one, their score is the largest of them, and the number of those who
        agreed goes as a separate number: the bridge and a subscriber are
        entitled to look at it.
        """
        folded: Dict[str, Hypothesis] = {}
        for guess in bucket:
            key = guess.key()
            if not key:
                continue
            seen = folded.get(key)
            if seen is None:
                folded[key] = Hypothesis(guess.text, guess.score, guess.model, 1)
            else:
                seen.agreed += 1
                # Whose spelling to keep is decided by the weighted score, but
                # the score is kept as it is: trust picks the winner of the
                # argument, it does not lower the confidence it goes to the
                # bridge with.
                if guess.score * self._weight(guess.model) > \
                        seen.score * self._weight(seen.model):
                    seen.score, seen.text, seen.model = guess.score, guess.text, guess.model

        out = list(folded.values())
        # Agreement first, then the weighted score: two models outweigh one
        # confident one, and of two loners the one we trust more weighs more.
        out.sort(key=lambda h: (h.agreed, h.score * self._weight(h.model)), reverse=True)
        return out


def _overlapping(fragments: List[Fragment], target: Fragment, slack_ms: int):
    """The fragment of another answer that covers the same span of time."""
    best, best_overlap = None, 0
    for frag in fragments:
        overlap = min(frag.end_ms, target.end_ms) - max(frag.start_ms, target.start_ms)
        if overlap > best_overlap:
            best, best_overlap = frag, overlap
    # Half of the span, not "any amount at all": a weak overlap means the model
    # was talking about the neighbouring phrase, and its text is out of place there.
    if best is None or best_overlap < max(slack_ms, target.duration_ms // 2):
        return None
    return best
