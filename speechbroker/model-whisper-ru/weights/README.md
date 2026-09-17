# The weights

*In Russian: [README.ru.md](README.ru.md). English is the source language; the other is a translation.*

**The weight files are not in this repository and are not meant to be.** This folder is the RECIPE:
`SOURCE` says where each set comes from, `SHA256SUMS` says which bytes are the right ones, and this
file says the rest. The bytes themselves you fetch once.

The bytes lie **outside the repository**, in the modding root beside MO2 and SSEEdit, by the same
rule as every other thing somebody else wrote:

```
D:\Skyrim VR Modding\Speech Models\ggml\<id>\        what whisper.cpp reads - this module
D:\Skyrim VR Modding\Speech Models\ctranslate2\         what faster-whisper reads - tools/audiolab
D:\Skyrim VR Modding\Speech Models\piper\               the voices of the synthesis
```

Split by format on purpose: one folder for both engines is how a CTranslate2 conversion ends
up signed as a GGML model and fetched from the wrong repository. The path is one key -
`weightsStore` in `config/build.json` - and nothing else in the module knows it.


And they do not need versioning. These are unmodified public releases: the same bytes for everybody,
fetched by name, with a checksum that proves it. A plugin or a recorded take is the opposite — it
exists nowhere else — and those stay in LFS for that reason.

## The format is GGML, and this is the part that goes wrong

whisper.cpp reads **GGML**: one `.bin` file, whole. It cannot read a **CTranslate2** conversion — the
faster-whisper format, a `model.bin` with `config.json`, `tokenizer.json` and `vocabulary.json`
beside it — and the two are easy to confuse, because the big file is called `model.bin` in both.

If the folder holds more than one `.bin`, the child refuses and says so rather than guessing.

## What to fetch

Everything below is from [ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp) on
Hugging Face, which is the whisper.cpp project's own model repository. The files are public and need
no account.

| Set | File | Size | Declared as |
|---|---|---|---|
| `whisper-ru-turbo` | `ggml-large-v3-turbo.bin` | 1.62 GB | the **accurate** model, asked on every final pass |
| `whisper-ru-small` | `ggml-small.bin` | 488 MB | the **fast** model, asked on interim passes too |

```
tools\weights.ps1 -Fetch                        both sets, into the store
tools\weights.ps1 -Fetch -Id whisper-ru-small   one of them
tools\weights.ps1                               verify what is there against SHA256SUMS
```

`-Fetch` reads `SOURCE` and `SHA256SUMS` from this folder, downloads into the store, copies the sums
in beside the bytes and verifies. By hand it is the same two files:

```
curl -L -o "D:\Skyrim VR Modding\Speech Models\ggml\whisper-ru-turbo\ggml-large-v3-turbo.bin" ^
    https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin
curl -L -o "D:\Skyrim VR Modding\Speech Models\ggml\whisper-ru-small\ggml-small.bin" ^
    https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin
```

The sums that must come out, and they are in `SHA256SUMS` beside each set already:

```
1fc70f774d38eb169993ac391eea357ef47c88757ef72ee5943879b7e8e2bc69  ggml-large-v3-turbo.bin
1be3a9b2063867b937e64e2ec7483364a79917e157fa98c5d94b5c1fffea987b  ggml-small.bin
```

## Why the sums are versioned when the weights are not

**Five hundred bytes flipped inside a model file produce no error of any kind** — not at load, not at
inference — and a model that answers rubbish. Nothing above the shim can tell that from a bad
recording or a hard accent, because an answer is a string and a score, and rubbish has both. So the
check has to happen before the child is ever raised, and the thing it checks against has to survive a
plain clone as readable text.

The shim does it itself at `Start`, in `src/core/Sha256.cpp`, against this same file. With
`requireSums` true in a model's settings it refuses to start when the file is merely *absent*, not
only when it fails to match. `tools/weights.ps1` is the convenience and the way to create the file;
it is not the only way to read it. The format is the one `sha256sum` has written since 1999 — sixty-four
hex characters, two spaces, the file name — so any tool a person already has can check the same file.

## Using something else

Any GGML whisper model works. The quantised ones are much smaller for very little accuracy:
`ggml-large-v3-turbo-q5_0.bin` is 574 MB against 1.62 GB, `ggml-small-q5_1.bin` is 190 MB against
488 MB. Both are in the same Hugging Face repository.

To swap one in: put the new `.bin` in the set's folder **as the only `.bin` there**, then
`tools\weights.ps1 -Write -Id <set>` to record its sum. Nothing else changes — the quantisation is
baked into the file, and there is no setting for it.

A set of your own needs a listing of its own beside `models\`: copy `models\whisper-ru.json`, give it
a new `id`, and point its `weights` at `weights\<your id>` — that path names the RECIPE and may not
leave the module, because the sums and the settings are ours and are versioned together. The bytes it
stands for are found under `weightsStore` by the same name.

## What a player installs

Not this. What a player installs is the archive `tools/package.ps1` builds, with the weights already
in it. `git clone` was never meant to be a download channel, which is the other half of why these
files do not belong in the repository.
