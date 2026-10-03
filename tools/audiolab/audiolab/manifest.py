# -*- coding: utf-8 -*-
"""The settings of the tooling and the labelling manifest, as one object.

Two files, because they are two different kinds of thing, and neither of them
exists at run time - both belong to whoever measures the module:

  * `audio.json` - THE SETTINGS. The folder of takes, the corpora, the reports,
    and the settings of the python model driver: which weights, on which device,
    with which beam - the same numbers the service ran with, so that a word
    error rate measured today is comparable with docs/measurements.md.
  * `calibration.json` - THE GROUND TRUTH, named by the `calibration` key of
    the settings. Which takes are finished phrases and which are cut off
    (`completeness.finished`/`unfinished`), for the takes recorded before the
    `-end`/`-open` naming convention existed, and the list of prompts a person
    is walked through. This is the only record of those labels anywhere;
    without it the completeness corpus falls from twenty-two labelled takes to
    eight. Nothing may drop it.

The ground truth is merged OVER the settings, so one Manifest answers for both
and a caller never has to know which file a key came from.

Every path in either file is relative to the folder of audiolab. Two overrides
are documented and nothing else is read from the environment:

    AUDIOLAB_WEIGHTS   the folder the weights lie in, instead of `weightsRoot`
    AUDIOLAB_ENGINE    the adapter's settings file, instead of `engineSettings`

The tuned numbers of the reference engine - the pacer, the word classes, the
completeness weights - are NOT copied here. They are read from the adapter's
own `speechbroker-voice.json` (`ears.pacer`, `ears.segments`,
`ears.completeness`), so that a replay always runs with the numbers the game
runs with; the python defaults equal them and take over if the file is absent.
"""
import json
import os
import pathlib
from typing import Callable, Dict, List, Optional

from .engine.cuda import add_cuda_dlls
from .engine.models import ModelAdapter, WhisperAdapter
from .engine.reputation import Reputations

SETTINGS = "audio.json"
GROUND_TRUTH = "calibration.json"       # unless `calibration` in the settings says otherwise
_cuda_ready = False


class Manifest:
    def __init__(self, root: pathlib.Path, data: dict, path: pathlib.Path):
        self.root = root
        self.data = data
        self.path = path

    @staticmethod
    def load(root: pathlib.Path) -> "Manifest":
        path = root / SETTINGS
        data = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}

        where = pathlib.Path(str(data.get("calibration", GROUND_TRUTH)))
        truth = where if where.is_absolute() else (root / where)
        if not truth.exists():
            raise FileNotFoundError("no %s beside %s - the labelling manifest is part of "
                                    "the repository, restore it" % (truth.name, root))
        data.update(json.loads(truth.read_text(encoding="utf-8")))
        return Manifest(root, data, path)

    def resolve(self, where) -> pathlib.Path:
        path = pathlib.Path(str(where))
        return path if path.is_absolute() else (self.root / path)

    # --- where things are ---------------------------------------------------
    @property
    def corpus(self) -> pathlib.Path:
        # `takes` is the same folder the studio records into, and it is named
        # by that key because the studio writes it back when a person changes
        # it. A measurement reads what was just recorded and no other place.
        return self.resolve(self.data.get("corpus", self.data.get("takes", "takes")))

    @property
    def reports(self) -> pathlib.Path:
        out = self.resolve(self.data.get("reports", "reports"))
        out.mkdir(parents=True, exist_ok=True)
        return out

    @property
    def live_only(self) -> bool:
        return bool(self.data.get("liveOnly", True))

    @property
    def weights_root(self) -> pathlib.Path:
        override = os.environ.get("AUDIOLAB_WEIGHTS")
        return pathlib.Path(override) if override else self.resolve(
            self.data.get("weightsRoot", "../../model-whisper-ru/weights"))

    @property
    def engine_file(self) -> Optional[pathlib.Path]:
        override = os.environ.get("AUDIOLAB_ENGINE")
        where = override or self.data.get("engineSettings")
        return self.resolve(where) if where else None

    # --- the models ---------------------------------------------------------
    @property
    def models(self) -> Dict[str, dict]:
        return {name: settings for name, settings in self.data.get("models", {}).items()
                if not name.startswith("_")}

    def weights_of(self, name: str) -> pathlib.Path:
        settings = self.models[name]
        return self.weights_root / settings.get("weights", name)

    def adapters(self) -> List[ModelAdapter]:
        """Fresh, not yet started drivers for every model in the manifest.

        The CUDA libraries are put on the path here, once, because this is the
        only place a driver is ever made: a script cannot forget to do it.
        """
        global _cuda_ready
        if not _cuda_ready:
            add_cuda_dlls()
            _cuda_ready = True
        return [WhisperAdapter(name, settings, self.weights_of(name))
                for name, settings in self.models.items()]

    @property
    def vocabulary(self) -> List[str]:
        return list(self.data.get("vocabulary", []))

    # --- the reference engine -----------------------------------------------
    @property
    def reputation_settings(self) -> dict:
        return dict(self.data.get("reputations", {}))

    @property
    def reputations_file(self) -> pathlib.Path:
        return self.resolve(self.reputation_settings.get("file", "reputations.json"))

    def reputations(self) -> Reputations:
        s = self.reputation_settings
        return Reputations(self.reputations_file,
                           float(s.get("defaultTrust", 0.6)),
                           int(s.get("minCalibrationSamples", 10)))

    def engine_settings(self) -> dict:
        """What `VoiceRecognizeEngine` is constructed with.

        The tuned numbers come from the adapter's file when it is there; a
        missing file is not an error, because the defaults built into the
        python classes are the same numbers.
        """
        ears = {}
        where = self.engine_file
        if where and where.exists():
            try:
                ears = json.loads(where.read_text(encoding="utf-8")).get("ears", {})
            except Exception:
                ears = {}
        s = self.reputation_settings
        return {
            "pacer": ears.get("pacer", {}),
            "segments": ears.get("segments", {}),
            "completeness": ears.get("completeness", {}),
            "reputationFile": str(self.reputations_file),
            "defaultTrust": float(s.get("defaultTrust", 0.6)),
            "minCalibrationSamples": int(s.get("minCalibrationSamples", 10)),
        }

    # --- the ground truth and the prompts -----------------------------------
    @property
    def completeness(self) -> dict:
        marks = dict(self.data.get("completeness", {}))
        marks.setdefault("endSuffix", "-end")
        marks.setdefault("openSuffix", "-open")
        marks.setdefault("finished", [])
        marks.setdefault("unfinished", [])
        return marks

    @property
    def prompts(self) -> List[dict]:
        return [p for p in self.data.get("prompts", []) if isinstance(p, dict)]

    @property
    def bench(self) -> dict:
        bench = dict(self.data.get("bench", {}))
        bench["folder"] = self.resolve(bench.get("folder", "samples/bench"))
        bench.setdefault("items", {})
        return bench

    @property
    def scenario(self) -> dict:
        """Where the scenario built out of live sound goes.

        A key and not a hard-coded path: the bridge is a separate part of the
        module and may be checked out anywhere. The stage - which take belongs
        to which state of the game - is optional; sound says nothing about it.
        """
        out = dict(self.data.get("scenario", {}))
        out["out"] = self.resolve(out.get("out", "../../bridge/tests/scenarios/live.json"))
        out["stage"] = self.resolve(out["stage"]) if out.get("stage") else None
        return out


AdapterFactory = Callable[[], List[ModelAdapter]]
