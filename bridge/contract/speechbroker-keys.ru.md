# Speech Broker — пространство ключей снимка

*По-английски: [speechbroker-keys.md](speechbroker-keys.md). Канонический язык - английский, эта страница переводная.*

Ключ описывает **вопрос, а не способ ответа**. `core.target.looked` — «на что направлено внимание
игрока»: в плоской игре это луч из камеры, в VR из взгляда или руки. Вопрос один, реализации разные.

Каждый ключ отвечает одним из трёх состояний: значение / «спросить некому» / «поставщик не смог».
У значения всегда есть возраст; что считать протухшим, решает подписчик.

## Пространства имён

| Префикс | Кто владеет |
|---|---|
| `core.*`     | ядро моста. Работает в любой редакции игры |
| `vr.*`       | поставщики VR-возможностей. Могут отсутствовать |
| `physics.*`  | поставщики физики |
| `speechbroker.*`    | сама служба моделей |
| `<мод>.*`    | любой мод под своим именем |

Писать в чужое пространство нельзя. Незаявленный ключ не принимается.

## Ядро

### Контекст
    core.context.topic              string  выбранная тема реплики
    core.context.menuOpen           bool
    core.context.menuName           string
    core.context.dialoguePartner    form    собеседник, если открыт диалог
    core.context.loading            bool

### Игрок
    core.player.inCombat            bool
    core.player.sneaking            bool
    core.player.swimming            bool
    core.player.mounted             bool
    core.player.sitting             bool
    core.player.weaponDrawn         bool
    core.player.health              float
    core.player.healthPct           float   0..1
    core.player.magickaPct          float
    core.player.staminaPct          float
    core.player.level               int

### Руки
    core.hands.right                form    экипировано в правой
    core.hands.left                 form
    core.hands.rightKind            string  spell | weapon | shield | torch | empty
    core.hands.leftKind             string
    core.hands.shout                form

### Внимание и окружение
    core.target.looked              form    на что направлено внимание
    core.target.lookedDistance      float
    core.target.lookedHostile       bool
    core.target.lineOfSight         bool    ДОРОГОЙ
    core.actors.nearest             form    ДОРОГОЙ
    core.actors.nearestDistance     float   ДОРОГОЙ
    core.actors.hostileCount        int     ДОРОГОЙ
    core.actors.followerCount       int

### Мир
    core.world.cell                 form
    core.world.cellName             string
    core.world.interior             bool
    core.world.location             form
    core.world.locationKeywords     string  через запятую
    core.world.gameHour             float   0..24
    core.world.weather              form

## Необязательные пространства

    vr.hands.rightHeld              form    физически зажатое   (HIGGS)
    vr.hands.leftHeld               form                        (HIGGS)
    vr.hands.grabbing               bool                        (HIGGS)
    vr.pose.crouching               bool    физическое приседание (VRIK)
    vr.gesture.last                 string  распознанный жест     (VRIK)
    physics.contact.actor           form    с кем физический контакт (PLANCK)
    speechbroker.noiseLevel                float   уровень шума на микрофоне
    speechbroker.engineBusy                bool

Ключ без поставщика отвечает «спросить некому» — на SE, на AE и на VR без соответствующего мода.
Это не ошибка и не повод падать.

## Дорогие ключи

Помеченные `ДОРОГОЙ` считаются только по запросу и кэшируются на время жизни реплики.
Список — в `snapshot.lazyKeys` конфигурации, радиус и предел перебора актёров тоже.
