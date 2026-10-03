# Speech Broker

Speech Broker захватывает речь игрока, обращается к установленным моделям распознавания, разыгрывает реплику между модами и доставляет её подписчику. Вся система находится в этом репозитории, ветка `main`.

[Архитектура системы и порядок сборки](speechbroker/README.ru.md) · [English](README.md)

| Компонент | Исходники и описание |
|---|---|
| SKSE-мост и ядро аукциона | [speechbroker/bridge/](speechbroker/bridge/README.ru.md) |
| Voice Adapter: захват, нарезка и раздача моделям | [speechbroker/adapter-voice/](speechbroker/adapter-voice/README.ru.md) |
| Whisper RU: модель-плагин и дочерний процесс | [speechbroker/model-whisper-ru/](speechbroker/model-whisper-ru/README.ru.md) |
| Demo Subscriber: тестовые квесты Papyrus | [speechbroker/subscribers/demo/](speechbroker/subscribers/demo/README.ru.md) |
| Audiolab: записи и внеигровые измерения | [speechbroker/tools/audiolab/](speechbroker/tools/audiolab/README.ru.md) |

У каждого компонента свои сборка и раскладка. Мост публикует установленный SDK для адаптера и подписчиков; адаптер — SDK моделей. Общий репозиторий сохраняет эти границы контрактов.

Истории прежних отдельных репозиториев компонентов и исходной ветки `Skyrim-Mods/speechbroker` сохранены в `main`. Двоичные плагины и записи звука хранятся через Git LFS (`git lfs install` перед клонированием). Веса моделей, внешние библиотеки, результаты сборки и ключи остаются вне Git; порядок подготовки описан в модулях.
