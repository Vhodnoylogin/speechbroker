# Speech Broker

Repository: https://github.com/Vhodnoylogin/speechbroker
Default branch: main

This repository owns the complete Speech Broker system:
- `bridge/`: auction core, SKSE bridge and public SDK.
- `adapter-voice/`: audio capture, segmentation, model dispatch and model SDK.
- `model-whisper-ru/`: Whisper RU plugin and child process.
- `subscribers/demo/`: test subscriber.
- `tools/audiolab/`: offline audio tooling.

Read the affected module README and its CLAUDE.md if present before editing. Preserve each module's build and deployment layout. Dependencies across modules are consumed through their installed SDKs, even though the sources now share a repository. Coordinate changes to SDK versions across the system in the single Speech Broker chat and journal. Other mods live in separate repositories. The original Skyrim-Mods branch and the former component repositories are historical sources whose history has been merged here. Before committing or pushing, check git remote -v and identify this repository explicitly. Do not commit credentials, build outputs, external model weights, runtime libraries or local caches.
