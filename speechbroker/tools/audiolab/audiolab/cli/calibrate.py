# -*- coding: utf-8 -*-
"""How far each model may be believed - measured on speech whose reference is known.

    python -m audiolab calibrate                     over the corpus of takes
    python -m audiolab calibrate --live              record the prompts first, then measure them
    python -m audiolab calibrate --what completeness only the division of finished from cut-off
    python -m audiolab calibrate --corpus samples/bench

In ordinary work there is nothing to check a transcription against: there is no
reference, and "the model is confident" means only "the model is confident".
Here the reference is known exactly - it lies beside the recording - and so both
quantities are measurable: how right the model is on speech, and whether it
keeps quiet on silence. Their product is TRUST, and it goes into
`reputations.json`, out of which the adapter reads a copy shipped inside its own
mod. Without it, comparing 0.9 of one model with 0.9 of another is meaningless.

Completeness is measured by a different truth and cannot use the live input: the
label - cut off or finished - comes from the NAME of the take, and names given
on the fly carry none. That is what `--live` records into the corpus for: the
next run measures the same voice by files.
"""
import argparse
from typing import List

from . import load, studio, write_report
from .. import calibrate as measure
from ..manifest import Manifest
from ..prompts import pick_input, record_prompts


def run(manifest: Manifest, corpus, what: str, only: List[str], report: str,
        title: str, live_only: bool = None) -> int:
    """The measurement itself, shared with `bench`.

    `live_only` is the one thing the two callers disagree about: over the corpus
    of takes the synthesised ones are left out, because a threshold tuned on
    even tempo and flat intonation is tuned to an artefact; over the reference
    corpus there is nothing BUT synthesis, and it is exactly what is wanted
    there - the reference text is known because we pronounced it ourselves.
    """
    live_only = manifest.live_only if live_only is None else live_only
    trials = measure.trials_from_folder(corpus, only=only, live_only=live_only)
    if not trials and what != "completeness":
        print("not one recording with a reference text in %s" % corpus)
        return 2

    spoken = sum(0 if t.is_silence else 1 for t in trials)
    print("trials: %d (speech %d, silence %d)" % (len(trials), spoken, len(trials) - spoken))

    lines = ["=== %s ===" % title, "",
             "corpus: %s" % corpus,
             "trials: %d (speech %d, silence %d)" % (len(trials), spoken, len(trials) - spoken)]

    if what in ("trust", "both"):
        reputations = manifest.reputations()
        before = {rep.name: rep.trust for rep in reputations.all()}
        for adapter in manifest.adapters():
            adapter.start()
            verdict = measure.measure(adapter, trials)
            measure.remember(reputations, verdict)
            rep = reputations.of(adapter.name)
            lines.extend(measure.report(verdict, rep.measured_class()))
            was = before.get(adapter.name)
            print("  %-16s trust %.2f%s   weight %.2f   p50 %d ms"
                  % (adapter.name, verdict.trust,
                     "" if was is None else "  (was %.2f)" % was,
                     rep.weight(), verdict.latency_p50))
        reputations.save()
        lines.append("")
        lines.append("Trust saved into %s - the engine reads it at start-up and normalises"
                     % reputations.path)
        lines.append("the scores of the models with it. The adapter reads a copy shipped")
        lines.append("inside its own mod, not this file.")

    # --- completeness --------------------------------------------------------
    # A different measure and a different input: trust is checked against a
    # reference TEXT, completeness against a reference LABEL. The label comes
    # from the name of the take, so this half can only be measured by files.
    if what in ("completeness", "both"):
        labelled = measure.labelled_from_folder(corpus, manifest.completeness,
                                                live_only=live_only)
        if not labelled:
            print("\nnot one labelled take: none ending in -end or -open, and none")
            print("in the lists of `completeness` in the manifest")
        else:
            done = sum(1 for item in labelled if item.finished)
            print("\ncompleteness: %d labelled takes (finished %d, cut off %d)"
                  % (len(labelled), done, len(labelled) - done))
            result = measure.measure_completeness(labelled, manifest.engine_settings(),
                                                  manifest.adapters, manifest.vocabulary)
            lines.extend(measure.completeness_report(result))
            if result.works:
                print("  the classes are separated, margin %.2f, threshold %.2f"
                      % (result.gap, result.threshold))
            else:
                print("  the classes OVERLAP: finished ones go down to %.2f, cut-off ones"
                      % result.lowest_finished)
                print("  go up to %.2f - a threshold dividing them without error does not"
                      % result.highest_open)
                print("  exist")
            print("  safe threshold %.2f: not one cut-off phrase goes through"
                  % result.safe_threshold)

    write_report(manifest, report, lines)
    return 0


def main(argv: List[str] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="audiolab calibrate",
        description="Measure the trust of every model, and where completeness divides.")
    parser.add_argument("--corpus", default=None,
                        help="the folder of takes to measure; by default the one in the settings")
    parser.add_argument("--what", default="both", choices=["trust", "completeness", "both"],
                        help="what to measure (default: both)")
    parser.add_argument("--live", action="store_true",
                        help="record the prompts first and measure exactly what was just said")
    parser.add_argument("--input", type=int, default=None, help="the number of the input, for --live")
    parser.add_argument("--prefix", default="", help="a prefix for the names of the takes, for --live")
    parser.add_argument("--overwrite", action="store_true",
                        help="re-record the takes that already exist, for --live")
    parser.add_argument("--report", default="calibration.txt",
                        help="the name of the report inside the folder of reports")
    args = parser.parse_args(argv)

    manifest = load()
    corpus = manifest.resolve(args.corpus) if args.corpus else manifest.corpus
    if not corpus.exists():
        print("no such corpus: %s" % corpus)
        return 2

    only = None
    if args.live:
        made = studio(manifest)
        print("=== trust: live speech ===")
        print("The takes stay in %s: the next run can be made by files," % corpus)
        print("without opening one's mouth.")
        pick_input(made, args.input)
        only = record_prompts(made, manifest.prompts, corpus, args.prefix,
                              manifest.completeness, args.overwrite)
        made.recorder.close()
        if not only:
            print("nothing was recorded - there is nothing to measure")
            return 2
        print("\nrecorded %d takes. Measuring..." % len(only))
    else:
        print("=== trust: by the recordings in %s ===" % corpus)

    return run(manifest, corpus, args.what, only, args.report, "trust of the models")


if __name__ == "__main__":
    raise SystemExit(main())
