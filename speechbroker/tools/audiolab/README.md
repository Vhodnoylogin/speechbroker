# audiolab — the sound studio

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

**This is not a mod, and nothing here reaches a player.** It is a utility of Speech Broker, and it
lives under `tools/` for that reason: everything in a `tools/` folder of this module is for whoever
works on it, and the lay-out scripts never look here.

It exists because the module has to be tested against real speech. A model is judged by what it
does with a recording of a human voice, and a recording is worth nothing without the text of what
was actually said. The studio makes both halves and keeps them together.

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

## Running it

Double-click `studio.cmd`, or from a terminal:

    studio.cmd
    studio.cmd --no-open      without opening a browser
    studio.cmd --port 8934    beside a studio that is already running

`http://127.0.0.1:8933/` opens. The terminal window must stay: the studio lives as long as it
does. Ctrl+C stops it, or close the window; after it stops the window waits for a key so that the
reason can be read. The same reason goes into `studio.log` - the studio once vanished together
with its window and there was nothing left to look at.

**It has no environment of its own.** It borrows the one of the voice service, where
`sounddevice`, `numpy` and `piper` are already installed. That service lives in a module of its
own, on the `voice` branch, and where it lies is a property of the machine rather than of this
repository - so the path is not written down here. Point the interpreter of that environment at
`server.py` and it starts the same way `studio.cmd` starts it.

## The recordings

One recording is two files: `name.wav` and `name.txt` with the text that was read. An empty text
does not mean "there is no text" but "there is silence here", and calibration uses it to check
whether the model invents words out of nothing. A third file, `name.src`, remembers whether the
voice was live or synthesised.

`takes/` holds the forty-five recordings the checks of the bridge are built from - they are the
same phrases the scenarios in [bridge/tests](../../bridge/tests) quote, and the only cover the
Cyrillic case folding in the core of the bridge has. The WAV files go through Git LFS; the texts
beside them are text and are versioned as text.

The folder is set in `audio.json` by the `takes` key and can be changed while the studio runs:
`set_folder(path)` without any interface, `pick_folder()` through a system dialog, `reveal()` to
open it in the file manager. The choice is remembered back into `audio.json`, as a relative path
when the folder lies inside the module - it has to travel with the project.

## The sample rate belongs to the device

Neither an input nor an output opens at a rate that happens to suit us. WASAPI devices in shared
mode work **only** at their own native rate and answer `Invalid sample rate` to anything else. So
the search always starts from the rate the device declared for itself, and for the "default"
choice it is taken from whichever device the system picked. A refusal at one rate is a step of the
search, not an error, and it is not shown to a person.
