# What was measured, and what came out

These are the numbers the `voice` branch produced before it was taken apart, kept here because the
branch is being deleted and they are the only record of what this stack actually did. Everything
below was measured with `faster-whisper` on a GPU, with the settings that now live in `audio.json`
under `models` — the same weights, device, compute type, beam and budget — so a run made today can
be compared with a run made then. Change one of those and the comparison is void.

The reports the numbers come from were written on **6 September 2026**, each at its own hour; the
live session is **7–9 September 2026**. The machine and the driver were not written down, and that
is a gap: from now on a run should say what it ran on.

The word error rate is the share of words that would have to be corrected — 0 means word for word.
Trust is `accuracy × honesty`, a product and not an average, because a model that invents on silence
is useless at any accuracy and the other way round.

## The reference corpus: seven synthesised takes

Measured 6 Sep 2026, 14:36. The reference text is known exactly because we pronounced it ourselves.

| take | words | small: error | small: ms | turbo: error | turbo: ms |
|---|---|---|---|---|---|
| cmd-cover | 2 | exact | 249 | exact | 105 |
| cmd-fireball | 1 | 100% | 39 | 100% | 85 |
| cmd-retreat | 1 | exact | 40 | exact | 82 |
| fireball-is-a-school | 4 | 25% | 81 | 25% | 99 |
| four-sentences | 9 | 44% | 118 | 11% | 192 |
| long-question | 10 | 10% | 96 | exact | 157 |
| silence | — | kept quiet | 4 | kept quiet | 5 |
| **total** | | **accuracy 0.70, honesty 1.00, trust 0.70** | **p50 81, p90 249** | **accuracy 0.77, honesty 1.00, trust 0.77** | **p50 99, p90 192** |

`cmd-fireball` is the one both models fail completely: «Фаербол» comes back as «Поебал». It is a
made-up word of the game, and no amount of accuracy elsewhere helps — that is what the vocabulary
hint exists for (below).

## The corpus of live takes

| when | corpus | trials | small | turbo |
|---|---|---|---|---|
| 6 Sep 2026, 22:00 | 30 takes | 30 | accuracy 0.64, trust 0.64, p50 73, p90 136 | accuracy 0.91, trust 0.91, p50 110, p90 148 |
| 6 Sep 2026, 23:03 | 41 takes (40 speech, 1 silence) | 41 | accuracy 0.58, trust 0.58, p50 64, p90 105 | accuracy 0.86, trust 0.86, p50 97, p90 120 |

Honesty was 1.00 in every run: neither model has ever invented words on the silence probe of the
corpus, though whisper does it readily on other silence — in the very first run of the pool the
silence check came back with "To be continued...". The check stays.

The distribution of the scores, from the run of 23:03 — this is what the adapter normalises a raw
number with, and the second reason calibration exists:

| model | observations | lower quarter | median | upper quarter |
|---|---|---|---|---|
| whisper-small | 43 | 0.57 | 0.71 | 0.73 |
| whisper-turbo | 43 | 0.68 | 0.79 | 0.89 |

`reputations.json` shipped beside this file is the output of that last run: trust 0.585 for
whisper-small and 0.863 for whisper-turbo, with the score distribution of each. The adapter reads a
copy shipped inside its own mod, not this one.

**p90 was not printed by the report of the day** — only p50 was. The values above were recomputed
from the per-take latencies in the same reports, by the same rule the code uses. The report written
by `audiolab calibrate` now prints both.

## Both models answer as fast

Declared: `whisper-small` fast with a budget of 700 ms, `whisper-turbo` accurate with 2500 ms.
Measured: p50 of 81 and 99 ms on the reference corpus, 64 and 97 on the live one — both fall into
the *fast* class by measurement, and the whole draft-and-refine machinery is therefore idle. That is
a reason to measure again on a slower model, not a reason to drop the pair.

## Completeness: where the threshold is

Measured 6 Sep 2026, 23:03, over **22 labelled takes — 16 finished, 6 cut off**. Fourteen of those
labels exist nowhere but in `calibration.json`; without that file the corpus is 8 takes and the
measurement below cannot be repeated.

| | value |
|---|---|
| threshold by accuracy | 0.26 — 86% of 22 decisions right |
| lowest finished | 0.09 |
| highest cut off | 0.32 |
| margin | **negative: the classes overlap** |
| **safe threshold** | **0.33 — not one cut-off phrase goes through** |
| price of the safe threshold | 3 finished takes of 16 wait until the end of the silence |

Accuracy is the wrong measure here and the safe threshold is the right one. Holding a finished
phrase back costs a fraction of a second — the hold ends with silence anyway. Letting a cut-off one
through performs an action on a fragment, which is the very thing the holding exists to prevent. The
two mistakes cost differently, so the threshold is taken by the price of a miss.

The overlap itself is the finding worth keeping: `fireball-en` and `fireball-ru`, both finished, score
0.09 and 0.10 — lower than every cut-off take but one. A single number does not divide these classes.

## The tone: the measurement of the terminal fall

Measured 6 Sep 2026, 19:39, over the 14 takes that existed then. A fall of 1.00 means the voice sat
down on the floor of its own range; 0.00 means it stayed up.

| take | truth | fall |
|---|---|---|
| door-closed | finished | 1.00 |
| door-continued | finished | 0.98 |
| door-unfinished | cut off | 0.77 |
| mentioned | finished | 0.88 |
| cmd-retreat | finished | 0.94 |
| two-commands, fireball-ru, fireball-en, middle-question, long-question, cmd-cover | finished | 1.00 |
| проба-синтеза | synthesised | **0.00** |
| long-speech, silence | — | no verdict |

Two things to keep. The decisive pair separates — 1.00 and 0.98 against 0.77 — but by 0.21, which is
not much for a feature meant to carry a decision on its own. And the synthesised take falls to 0.00:
synthesis has no terminal fall at all, which is why completeness is never tuned on it and why
`liveOnly` is on by default.

## The vocabulary hint

Measured 6 Sep 2026, 14:47. Three ways of telling the model which words to expect, on the three
takes that contain the invented word «фаербол»:

| | without a hint | with a hint |
|---|---|---|
| fireball-is-a-school, small | «Пайербол. Это школа огня.» | «фаербол, это школа огня» |
| four-sentences, turbo | «Каербал. Отступаем…» | «фаербол, отступаем, прикрой меня, ты зачем надел эту броню?» |
| cmd-fireball, either | «Поебал» | «Поебал» — unchanged |

`initial_prompt` and `hotwords` gave identical output on every take, so there is no reason to prefer
one. The hint fixes the word inside a phrase and never fixes it alone: a single word has no context
for the hint to work against. And it is not free — with the hint the small model dropped the first
three sentences of `four-sentences` entirely and answered with the fourth alone. In the settings as
they stand `usePrompt` is on for the draft model and off for the accurate one; this measurement is
the whole of what is known about that choice, and it does not obviously support it.

## What the live service actually emitted

`session.jsonl`, 7–9 September 2026, read once and then dropped: **597 utterances** handed out over
three days of play.

| | |
|---|---|
| score | median 0.60, from 0.01 to 1.00 |
| completeness | median 0.59; **72 of 597 below the safe threshold of 0.33** |
| pieces carrying alternatives | 342 of 597 — the models disagreed on more than half |
| pieces superseding an earlier one | 213 of 597 — a re-reading changed what had been said |
| length classes | short 185, middle 239, long 173 |
| empty text | none |

The last two rows are the ones that matter for the dispatch: superseding is not a rare case to be
handled for completeness' sake, it is a third of everything the engine gives out, and more than half
of the pieces arrive with something to argue about.
