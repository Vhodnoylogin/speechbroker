# Speech Broker

Speech Broker captures player speech, asks the installed recognition models, auctions each utterance and delivers it to a subscribing mod. The complete system lives in this repository on `main`.

[System architecture and build order](speechbroker/README.md) · [Русский](README.ru.md)

| Component | Source and documentation |
|---|---|
| SKSE bridge and auction core | [speechbroker/bridge/](speechbroker/bridge/README.md) |
| Voice Adapter: capture, segmentation and model dispatch | [speechbroker/adapter-voice/](speechbroker/adapter-voice/README.md) |
| Whisper RU: model plugin and child process | [speechbroker/model-whisper-ru/](speechbroker/model-whisper-ru/README.md) |
| Demo Subscriber: test Papyrus quests | [speechbroker/subscribers/demo/](speechbroker/subscribers/demo/README.md) |
| Audiolab: recording and offline measurements | [speechbroker/tools/audiolab/](speechbroker/tools/audiolab/README.md) |

Each component keeps its own build and deployment scripts. The bridge publishes the installed SDK consumed by the adapter and subscribers; the adapter publishes the model SDK. Sharing a repository does not change these contracts.

The histories of the former component repositories and the original `Skyrim-Mods/speechbroker` branch are retained in `main`. Binary plugins and audio recordings use Git LFS (`git lfs install` before cloning). External model weights, runtime libraries, build output and credentials stay outside Git; follow the module recipes.
