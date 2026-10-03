# -*- coding: utf-8 -*-
"""The trustworthiness of a model: measured on speech whose reference is KNOWN IN ADVANCE.

Why this exists apart from ordinary work. On live speech there is nothing to
check the correctness of a transcription against - there is no reference, and
"the model is confident" means only "the model is confident". Here there is a
reference: the text is known exactly, and so both are measurable - how right
the model is, and whether it invents on silence.

Out of those two quantities TRUST is made - the number the adapter later
normalises the scores of the model with in live work. Without it comparing 0.9
of one model with 0.9 of another is meaningless. The product of this module
is `reputations.json`; the adapter reads a copy shipped inside its mod.

The module knows nothing of the microphone or the files: it takes trials - a
pair of "reference and sound" - and returns measurements. Where the sound came
from is decided by the caller: a recording from the microphone and a walk over
a folder give the same trials.
"""
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, List, Optional

import numpy as np

from .engine.models import ModelAdapter
from .files import Library

RATE = 16000


@dataclass
class Trial:
    """One trial: what should have sounded and what did.

    An empty reference is not the absence of text but the claim "there is
    silence here", and the trial checks whether the model invents on nothing.
    """

    name: str
    reference: str
    pcm: np.ndarray

    @property
    def is_silence(self) -> bool:
        return not self.reference.strip()


@dataclass
class Outcome:
    """What came out of one trial with one model."""

    name: str
    reference: str
    heard: str
    latency_ms: int
    error_rate: float = 0.0     # the share of words that would have to be corrected
    invented: bool = False      # spoke up on silence
    scores: List[float] = field(default_factory=list)   # the scores of the fragments

    @property
    def is_silence(self) -> bool:
        return not self.reference.strip()

    def as_line(self) -> str:
        if self.is_silence:
            return "  %-24s silence -> %s   %d ms" % (
                self.name,
                ('INVENTED: "%s"' % self.heard[:44]) if self.invented else "quiet",
                self.latency_ms)
        mark = "exact" if self.error_rate == 0 else "errors %.0f%%" % (self.error_rate * 100)
        return "  %-24s %-11s %d ms" % (self.name, mark, self.latency_ms)


@dataclass
class Verdict:
    """The outcome for one model."""

    model: str
    declared_class: str
    declared_budget_ms: int
    accuracy: float = 0.0       # 1.0 - transcribed word for word
    honesty: float = 1.0        # 1.0 - stayed quiet on silence
    trust: float = 0.0
    latency_p50: int = 0
    latency_p90: int = 0
    spoken: int = 0
    probes: int = 0
    inventions: int = 0
    outcomes: List[Outcome] = field(default_factory=list)
    # The distribution of the model's scores on this corpus. It is the second
    # reason calibration exists: by it a raw number is turned into a place
    # among the model's own answers, and that makes the models comparable.
    scores: List[float] = field(default_factory=list)


def words(text: str) -> List[str]:
    keep = "".join(c if c.isalnum() or c.isspace() else " " for c in text.lower())
    return keep.split()


def wer(reference: str, got: str) -> float:
    """The share of words that would have to be corrected. 0 - matched word for word."""
    a, b = words(reference), words(got)
    if not a:
        return 0.0 if not b else 1.0
    prev = list(range(len(b) + 1))
    for i in range(1, len(a) + 1):
        cur = [i] + [0] * len(b)
        for j in range(1, len(b) + 1):
            cur[j] = min(cur[j - 1] + 1, prev[j] + 1,
                         prev[j - 1] + (0 if a[i - 1] == b[j - 1] else 1))
        prev = cur
    return prev[len(b)] / float(len(a))


def trim_tail(pcm: np.ndarray, block_ms: int = 32, floor: float = 0.004) -> np.ndarray:
    """Cut the trailing silence - exactly as the engine does before calling a model."""
    step = int(RATE * block_ms / 1000)
    last = 0
    for at in range(0, max(0, len(pcm) - step), step):
        if float(np.sqrt(np.mean(np.square(pcm[at:at + step])))) >= floor:
            last = at + step
    return pcm[:last] if last else pcm


def trials_from_folder(folder, only: Optional[List[str]] = None,
                       live_only: bool = False) -> List[Trial]:
    """Trials out of a folder of takes: `name.wav` beside `name.txt`.

    `only` - take the named takes alone; that is how the live mode measures
    exactly what the person has just said, without mixing in the old.
    """
    library = Library(Path(folder))
    out = []
    for take in library.by_name():
        if only is not None and take.name not in only:
            continue
        if live_only and not take.live:
            continue
        out.append(Trial(take.name, take.reference, library.read(take.name)))
    return out


def measure(adapter: ModelAdapter, trials: List[Trial]) -> Verdict:
    """Run one model over all the trials and fold the measurements."""
    verdict = Verdict(adapter.name, adapter.klass, adapter.budget_ms)
    errors, latencies = [], []

    for trial in trials:
        pcm = trial.pcm if trial.is_silence else trim_tail(trial.pcm)
        answer = adapter.recognize(pcm, RATE)
        heard = " ".join(f.text for f in answer.fragments).strip()
        latencies.append(answer.latency_ms)

        outcome = Outcome(trial.name, trial.reference, heard, answer.latency_ms)
        outcome.scores = [f.score for f in answer.fragments]
        if not trial.is_silence:
            verdict.scores.extend(outcome.scores)
        if trial.is_silence:
            verdict.probes += 1
            outcome.invented = bool(heard)
            verdict.inventions += 1 if outcome.invented else 0
        else:
            verdict.spoken += 1
            outcome.error_rate = wer(trial.reference, heard)
            errors.append(outcome.error_rate)
        verdict.outcomes.append(outcome)

    verdict.accuracy = 1.0 - min(1.0, sum(errors) / len(errors)) if errors else 0.0
    verdict.honesty = 1.0 - (verdict.inventions / verdict.probes) if verdict.probes else 1.0
    # Trust is a product, not an average: a model that invents on silence is
    # useless at any accuracy, and the other way round.
    verdict.trust = round(verdict.accuracy * verdict.honesty, 3)
    ordered = sorted(latencies)
    if ordered:
        verdict.latency_p50 = ordered[len(ordered) // 2]
        verdict.latency_p90 = ordered[min(len(ordered) - 1, int(len(ordered) * 0.9))]
    return verdict


def remember(reputations, verdict: Verdict) -> None:
    """Write what was measured into the reputation of the model."""
    rep = reputations.of(verdict.model, verdict.declared_class, verdict.declared_budget_ms)
    rep.trust = verdict.trust
    rep.declared_class = verdict.declared_class
    rep.calibration_scores = list(verdict.scores)
    for outcome in verdict.outcomes:
        rep.note_call(outcome.latency_ms, [])
    for _ in range(verdict.probes):
        rep.note_silence(False)
    rep.silence_inventions = verdict.inventions


def report(verdict: Verdict, measured_class: str = "") -> List[str]:
    lines = ["", "--- %s (declared %s, budget %d ms) ---"
             % (verdict.model, verdict.declared_class, verdict.declared_budget_ms)]
    for outcome in verdict.outcomes:
        lines.append(outcome.as_line())
        if outcome.error_rate:
            lines.append("      reference: %s" % outcome.reference)
            lines.append("      heard    : %s" % outcome.heard)
    lines.append("  total: accuracy %.2f, honesty %.2f -> TRUST %.2f;"
                 " latency p50 %d ms, p90 %d ms%s"
                 % (verdict.accuracy, verdict.honesty, verdict.trust,
                    verdict.latency_p50, verdict.latency_p90,
                    ", measured class %s" % measured_class if measured_class else ""))
    if verdict.scores:
        ordered = sorted(verdict.scores)
        lines.append("  score distribution: %d observations, lower quarter %.2f,"
                     " median %.2f, upper quarter %.2f"
                     % (len(ordered), ordered[len(ordered) // 4],
                        ordered[len(ordered) // 2], ordered[3 * len(ordered) // 4]))
    return lines


# ===========================================================================
# Completeness: did the speaker finish - measured on takes whose truth is known
# ===========================================================================
#
# Trust is measured against a reference TEXT, completeness against a reference
# LABEL: about a take it is known in advance whether the phrase is cut off or
# finished. Without such a label there is nothing to check completeness
# against, and the first measurement showed why it is needed: the numbers of
# phrases known to be finished and phrases known to be cut off overlapped
# entirely, that is the feature carried no signal at all while looking as if
# it worked.


@dataclass
class Labelled:
    """A take about which it is known in advance whether its phrase is finished."""

    name: str
    finished: bool
    pcm: np.ndarray


@dataclass
class Point:
    """One measurement: what the engine showed against what was actually so."""

    name: str
    finished: bool
    complete: Optional[float]
    text: str = ""
    slices: int = 0

    @property
    def judged(self) -> bool:
        return self.complete is not None


@dataclass
class Separation:
    """How far the feature tells the two classes apart at all."""

    points: List[Point] = field(default_factory=list)
    threshold: float = 0.5
    accuracy: float = 0.0
    gap: float = 0.0            # the margin between the classes; negative - overlap
    lowest_finished: float = 0.0
    highest_open: float = 0.0

    # The threshold at which NOT ONE cut-off phrase goes through.
    #
    # The two mistakes cost differently, and measuring them with one accuracy
    # is wrong. Holding a finished phrase back loses fractions of a second: the
    # hold ends with silence anyway. Letting a cut-off one through performs an
    # action on a fragment, that is exactly what the holding exists to prevent.
    # So the threshold is chosen not by accuracy but by the price of a miss.
    safe_threshold: float = 0.0
    delayed_by_safe: int = 0        # how many finished ones will wait extra

    @property
    def works(self) -> bool:
        """The feature is fit only if the classes are SEPARATED, not merely guessed.

        Accuracy by itself deceives: with nine finished out of ten, the
        threshold "everything is finished" gives 0.9 and tells nothing apart.
        """
        return self.gap > 0.0


def labelled_from_folder(folder, marks: dict, live_only: bool = True) -> List[Labelled]:
    """The takes whose truth is known.

    The label comes from the name: ending in `openSuffix` - the phrase is cut
    off, in `endSuffix` - finished. The name is chosen by the person at the
    time of recording, so the label needs neither a separate file nor a
    separate action. The lists `finished`/`unfinished` in the manifest add what
    was recorded before this rule appeared.
    """
    named_finished = set(marks.get("finished", []))
    named_open = set(marks.get("unfinished", []))
    end_suffix = marks.get("endSuffix", "-end")
    open_suffix = marks.get("openSuffix", "-open")
    library = Library(Path(folder))
    out = []

    for take in library.by_name():
        if live_only and not take.live:
            continue
        stem = take.name
        if stem in named_open or stem.endswith(open_suffix):
            is_finished = False
        elif stem in named_finished or stem.endswith(end_suffix):
            is_finished = True
        else:
            # Not labelled - not measured. Guessing the truth inside a check of
            # the truth is not allowed.
            continue
        out.append(Labelled(stem, is_finished, library.read(stem)))
    return out


def measure_completeness(labelled: List[Labelled], settings: dict,
                         make_adapters: Callable[[], List[ModelAdapter]],
                         vocabulary: List[str] = None) -> Separation:
    """Run the reference engine over the labelled takes and fold the measurements."""
    from . import replay

    result = Separation()
    for item in labelled:
        pieces = replay.run(item.pcm, settings, make_adapters(), vocabulary)
        tail = replay.last_slice(pieces)
        result.points.append(Point(item.name, item.finished,
                                   tail.complete if tail else None,
                                   tail.text if tail else "", len(pieces)))
    _separate(result)
    return result


def _separate(result: Separation) -> None:
    """Find the threshold that best divides the classes, and say honestly whether it divides them."""
    judged = [p for p in result.points if p.judged]
    if not judged:
        return

    finished = [p.complete for p in judged if p.finished]
    opened = [p.complete for p in judged if not p.finished]
    if not finished or not opened:
        return

    # Every midpoint between observations is tried: there are no thresholds
    # between them that could do better.
    values = sorted({p.complete for p in judged})
    best, best_hits = 0.5, -1
    for left, right in zip(values, values[1:] + [values[-1] + 0.01]):
        candidate = (left + right) / 2.0
        hits = sum(1 for p in judged
                   if (p.complete >= candidate) == p.finished)
        if hits > best_hits:
            best, best_hits = candidate, hits

    result.threshold = round(best, 3)
    result.accuracy = best_hits / float(len(judged))
    result.lowest_finished = min(finished)
    result.highest_open = max(opened)
    result.gap = round(result.lowest_finished - result.highest_open, 3)

    # The safe threshold - just above the most "confident" of the cut-off ones.
    result.safe_threshold = round(result.highest_open + 0.01, 3)
    result.delayed_by_safe = sum(1 for v in finished if v < result.safe_threshold)


def completeness_report(result: Separation) -> List[str]:
    lines = ["", "=== completeness: measured against known ===", "",
             "%-18s %-10s %8s  %s" % ("take", "truth", "engine", "tail piece")]
    for point in sorted(result.points, key=lambda p: (p.finished, p.complete or -1)):
        lines.append("%-18s %-10s %8s  %s" % (
            point.name,
            "finished" if point.finished else "cut off",
            "%.2f" % point.complete if point.judged else "no pieces",
            point.text[:40]))

    judged = [p for p in result.points if p.judged]
    lines.append("")
    if not judged:
        lines.append("The engine produced not one piece - nothing to measure.")
        return lines

    finished_count = sum(1 for p in judged if p.finished)
    lines.append("Threshold by accuracy %.2f: %.0f%% of %d decisions right."
                 % (result.threshold, result.accuracy * 100, len(judged)))
    if result.works:
        lines.append("The classes are SEPARATED: the lowest finished %.2f is above the highest"
                     " cut-off %.2f, margin %.2f."
                     % (result.lowest_finished, result.highest_open, result.gap))
    else:
        lines.append("The classes overlap: finished ones go down to %.2f, cut-off ones"
                     " go up to %.2f." % (result.lowest_finished, result.highest_open))

    lines.append("")
    lines.append("But accuracy is not the measure here. Holding a finished phrase back costs")
    lines.append("a fraction of a second: the hold ends with silence anyway. Letting a cut-off")
    lines.append("one through performs an action on a fragment, the very thing the holding")
    lines.append("exists to prevent. The mistakes cost differently, and the threshold has to")
    lines.append("be taken by the price of a miss, not by the share of matches.")
    lines.append("")
    lines.append("SAFE THRESHOLD %.2f: not one cut-off phrase goes through."
                 % result.safe_threshold)
    lines.append("The price - %d finished of %d wait extra, until the end of the silence."
                 % (result.delayed_by_safe, finished_count))
    return lines
