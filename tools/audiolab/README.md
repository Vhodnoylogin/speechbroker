# audiolab — the sound studio and the measuring bench

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

**This is not a mod, and nothing here reaches a player.** It is a utility of Speech Broker, and it
lives under `tools/` for that reason: everything in a `tools/` folder of this module is for whoever
works on it, and the lay-out scripts never look here.

It exists because the module has to be tested against real speech. A model is judged by what it
does with a recording of a human voice, and a recording is worth nothing without the text of what
was actually said. The studio makes both halves and keeps them together.

**The engine that runs in the game is the C++ under [`adapter-voice/src/`](../../adapter-voice/src) —
the python engine here is reference tooling**, and it exists so that the calibration, the benches
and the offline replay can drive a whole recognition path from a wav file to the pieces a bridge
would receive, without the game, and so that every number in the adapter can be traced back to the
line of python it was measured with. What came out of those measurements is in
[docs/measurements.md](docs/measurements.md); why the branch that held them is being taken apart is
in [dissolving the voice branch](../../docs/dissolving-the-voice-branch.md).

What the module is as a whole is in the [description of the module](../../README.md).

## Everything that takes part in making sound

Listing the devices, recording, playing back, synthesis, the files on disk. It is one place
because sound is wanted in several different ways: a live stream for the recognition engine,
reference files for calibration, and recording and listening for a person.

    audiolab/devices.py    list the inputs and the outputs
    audiolab/capture.py    record from the chosen input, with a level
    audiolab/playback.py   play to the chosen output
    audiolab/synth.py      synthesis of speech
    audiolab/files.py      a WAV beside the text it was read from
    audiolab/studio.py     Studio - which is the API itself

    audiolab/engine/       the reference recognition engine, in python
    audiolab/calibrate.py  trust of a model, and where completeness divides
    audiolab/replay.py     a recording through the engine, block by block
    audiolab/prompts.py    walking a person through the list of prompts
    audiolab/manifest.py   the settings and the ground truth, as one object
    audiolab/cli/          one module per command, all of them thin

    server.py              HTTP over Studio, one route per method
    ui/index.html          the page - a skin that comes off

**The interface is pulled over from outside.** Everything the page can do, so can somebody who
does not have it:

```python
from audiolab import Studio
studio = Studio.load(pathlib.Path("."))
studio.choose(input_index=17)
studio.start("proba", "Close the door.")
...
studio.stop()
```

The test is simple: if an action cannot be performed without opening the page, it has leaked into
the interface, and that is a defect.

## The commands

    audiolab.cmd <command> [options]           or:  python -m audiolab <command>
    audiolab.cmd <command> --help              what that command takes
    audiolab.cmd                               the list of commands

| command | what it does | what it leaves |
|---|---|---|
| `record` | walks a person through the prompts of the manifest and records a take for each | takes in `takes/` |
| `listen` | records one take and runs the engine over it at once, again and again until Ctrl+C | takes, and the pieces on the screen |
| `calibrate` | how far each model may be believed, and where completeness divides the finished from the cut off | `reputations.json`, `reports/calibration.txt` |
| `bench` | the same measurement over the synthesised corpus, where the reference text is exact | `reports/engine-bench-bench.txt` |
| `corpus` | synthesises that corpus out of `bench.items` of the settings | wavs in `samples/bench/` |
| `replay` | one recording through the engine: the pieces, the completeness, the argument of the models | `reports/engine-replay.txt` |
| `scenario` | turns the live takes into a scenario the bridge is checked against | `bridge/tests/scenarios/live.json` |
| `prosody` | does our measurement of the tone tell a finished phrase from a cut-off one | `reports/prosody-check.txt` |

A report goes into `reports/` and not onto the screen, and for a reason: the takes and their
reference texts are Russian, and printing Cyrillic into a Windows console dies on cp1251. The folder
is not versioned — what is worth keeping out of a run goes into `docs/measurements.md` by hand.

`record --overwrite` re-records the takes that already exist; without it they are skipped, so an
interrupted walk can be resumed. `calibrate --live` records first and then measures exactly what was
just said. `scenario` writes where `scenario.out` of the settings points, and that file is derived:
it is in `.gitignore`, and the way to get it back is to run the command.

The studio — recording and listening by hand, through a page — is started separately, by
`studio.cmd`; see below.

**It has no environment of its own.** It used to borrow the one of the voice service, and that
service is gone:

    python -m venv .venv
    .venv\Scripts\pip install -r requirements.txt

`audiolab.cmd` and `studio.cmd` take `.venv` in this folder if it is there, otherwise whatever
`python` is on the path. On a machine where the environment lies elsewhere, set `AUDIOLAB_PYTHON`
to its interpreter — nothing in the repository has to know where that is.

## The settings, and the ground truth beside them

`audio.json` holds the settings of the tooling: the folder of takes, the corpora, the reports, and
the settings of the python model driver — which weights, on which device, with which beam. They are
the numbers the dissolved service ran with, kept so that a word error rate measured today is
comparable with what is written down in `docs/measurements.md`.

**The weights.** `weightsRoot` points at `../../model-whisper-ru/weights`, and each model names a
folder under it by the same id its listing in that mod uses — so the tooling and the mod look at the
same files, and neither one carries a path of its own. On a machine where the weights lie elsewhere,
set `AUDIOLAB_WEIGHTS`; it replaces the key whole. The same way `AUDIOLAB_ENGINE` replaces
`engineSettings`, which points at the settings of the adapter: the tuned numbers of the ears — the
pacer, the word classes, the weights of completeness — are read from there and are **not** copied
here, so that a replay always runs with the numbers the game runs with.

**`calibration.json` is the ground truth**, and `audio.json` names it by the `calibration` key. Trust
is checked against a reference TEXT, which lies beside every take; completeness is checked against a
reference LABEL — was the phrase cut off or finished — and no label can be got out of the sound. The
label normally comes from the name of the take, `-end` or `-open`, chosen while recording. The two
lists in that file carry the fourteen takes recorded before that rule existed, and **nothing else
remembers what they are**: without them the completeness corpus falls from twenty-two labelled takes
to eight, and the measurement in `docs/measurements.md` cannot be repeated. The list of prompts
`record` walks through lives there too, because a prompt that says whether its phrase is finished is
what puts the suffix on the name.

**`reputations.json`** is what the calibration produces: the trust of each model and the
distribution of its scores, out of which a raw number is turned into a place among the model's own
answers. The one here is the output of the last run of the service, shipped as the starting point.
**The adapter reads a copy shipped inside its own mod** — this file is the source it is published
from, not the file the game opens.

## The recordings

One recording is two files: `name.wav` and `name.txt` with the text that was read. An empty text
does not mean "there is no text" but "there is silence here", and calibration uses it to check
whether the model invents words out of nothing. A third file, `name.src`, remembers whether the
voice was live or synthesised — `liveOnly` in the settings leaves the synthesised ones out of a
measurement, because speech with an even tempo and no terminal fall of the tone would tune the
thresholds to an artefact.

`takes/` holds the forty-five recordings the checks of the bridge are built from - they are the
same phrases the scenarios in [bridge/tests](../../bridge/tests) quote, and the only cover the
Cyrillic case folding in the core of the bridge has. The WAV files go through Git LFS; the texts
beside them are text and are versioned as text.

`samples/bench/` holds the synthesised corpus instead: seven takes whose reference text is known
exactly, because we pronounced it ourselves. `corpus` rebuilds it, and it needs a piper voice —
the voice is **not** shipped here, it belonged to the service that is being dissolved and speech is
to become a model mod of its own. Put one in `voices/` (that folder is not versioned) or point the
`voice` key of `audio.json` wherever it lies.

The folder of takes is set in `audio.json` by the `takes` key and can be changed while the studio
runs: `set_folder(path)` without any interface, `pick_folder()` through a system dialog, `reveal()`
to open it in the file manager. The choice is remembered back into `audio.json`, as a relative path
when the folder lies inside the module - it has to travel with the project.

## The studio

Double-click `studio.cmd`, or from a terminal:

    studio.cmd
    studio.cmd --no-open      without opening a browser
    studio.cmd --port 8934    beside a studio that is already running

`http://127.0.0.1:8933/` opens. The terminal window must stay: the studio lives as long as it
does. Ctrl+C stops it, or close the window; after it stops the window waits for a key so that the
reason can be read. The same reason goes into `studio.log` - the studio once vanished together
with its window and there was nothing left to look at.

## The sample rate belongs to the device

Neither an input nor an output opens at a rate that happens to suit us. WASAPI devices in shared
mode work **only** at their own native rate and answer `Invalid sample rate` to anything else. So
the search always starts from the rate the device declared for itself, and for the "default"
choice it is taken from whichever device the system picked. A refusal at one rate is a step of the
search, not an error, and it is not shown to a person.
