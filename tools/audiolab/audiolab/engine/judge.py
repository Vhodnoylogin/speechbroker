# -*- coding: utf-8 -*-
"""Whether the sentence is finished.

Reference only: the runtime is `adapter-voice/src/turn/Completeness.cpp`.
See the package docstring.

The main thing here is not the formula but how little of it is needed. Inside
one re-reading every fragment but the last is finished BY CONSTRUCTION: speech
follows it, so the speaker finished it. The uncertainty belongs to the tail
alone, and all the arithmetic below concerns one fragment per pass.
"""
from .parts import Fragment


class CompletenessJudge:
    """The only place where the weights and thresholds of completeness live."""

    def __init__(self, settings: dict = None):
        s = settings or {}
        # A pause accepted as the end of a phrase is relative to the tempo of the
        # speaker: an absolute threshold would be right for one speed of speech only.
        self.pause_factor = float(s.get("pauseFactor", 2.5))
        self.with_mark = float(s.get("withTerminalMark", 0.80))
        self.no_mark = float(s.get("withoutTerminalMark", 0.20))
        self.junk_below = float(s.get("junkNoSpeechProb", 0.6))
        self.pitch_weight = float(s.get("pitchWeight", 0.5))

    def judge(self, fragment: Fragment, has_speech_after: bool,
              silence_after_ms: int, terminal_fall: float = None) -> float:
        """The probability that the sentence ended on this fragment."""

        # A fact outweighs any feature: if there is speech further on in this
        # same reading, the fragment is finished and there is nothing to compute.
        if has_speech_after:
            return 1.0

        # An invention of the model on silence: its punctuation means nothing.
        if fragment.no_speech_prob >= self.junk_below:
            return 0.0

        # The punctuation of the model is only ONE input, and not the main one:
        # with Whisper it is three quarters language. It has seen millions of
        # texts where a lone noun stands with a full stop, and it will put one
        # after "Fireball" no matter how it sounded.
        value = self.with_mark if fragment.ends_sentence else self.no_mark

        # Our own measurement of the tone has no language prior: a fall to the
        # bottom of the speaker's range is a measurement, not a guess at how
        # people usually write. So it weighs more than the punctuation mark.
        if terminal_fall is not None:
            value = (1.0 - self.pitch_weight) * value + self.pitch_weight * terminal_fall

        # The confidence of the model in the last word is the available stand-in
        # for "the probability of the full stop itself": the pure probability of
        # the mark is not handed out.
        if fragment.last_word_prob >= 0.0:
            value *= 0.5 + 0.5 * fragment.last_word_prob

        # The pause after the fragment, against the tempo of speech inside it.
        if silence_after_ms > 0 and fragment.median_gap_ms > 0:
            ratio = silence_after_ms / max(1, fragment.median_gap_ms)
            if ratio >= self.pause_factor:
                value += (1.0 - value) * 0.5
            elif ratio < 1.0:
                value *= 0.5

        return max(0.0, min(1.0, value))
