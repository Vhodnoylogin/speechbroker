# -*- coding: utf-8 -*-
"""The CUDA libraries on the DLL search path.

Reference tooling only; the model mod brings its own runtime up
(`model-whisper-ru`). See the package docstring.

CTranslate2 loads cublas and cudnn by bare name. Those folders used to be
registered by torch - as a side effect of merely importing it; without torch
nobody does it, and work on the GPU fails on the very first recognition with
"Library cublas64_12.dll is not found".

A module of its own because two callers need it: every script that drives the
python model driver, and the driver itself.
"""
import ctypes
import os
import pathlib
import site
import sys
from typing import List


def add_cuda_dlls() -> List[pathlib.Path]:
    """Find the nvidia/*/bin folders of this interpreter and make them visible to the loader."""
    bases = list(site.getsitepackages())
    bases.append(str(pathlib.Path(sys.prefix) / "Lib" / "site-packages"))

    dirs: List[pathlib.Path] = []
    for base in bases:
        nv = pathlib.Path(base) / "nvidia"
        if not nv.is_dir():
            continue
        dirs = [s / "bin" for s in sorted(nv.iterdir()) if (s / "bin").is_dir()]
        if dirs:
            break
    if not dirs:
        return []

    for d in dirs:
        try:
            os.add_dll_directory(str(d))
        except (OSError, AttributeError):
            pass

    # add_dll_directory only affects loads that agreed to search the user
    # directories; a bare LoadLibrary("cublas64_12.dll") does not look there.
    # PATH, on the other hand, is read in the ordinary way, and pre-loading by
    # full path makes a later request by name find the already loaded module.
    os.environ["PATH"] = os.pathsep.join([str(d) for d in dirs] +
                                         [os.environ.get("PATH", "")])
    for name in ("cublasLt64_12.dll", "cublas64_12.dll", "cudnn64_9.dll"):
        for d in dirs:
            p = d / name
            if p.exists():
                try:
                    ctypes.CDLL(str(p))
                except OSError:
                    pass
                break
    return dirs
