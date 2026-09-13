# Envoy - the space of snapshot keys

*In Russian: [envoy-keys.ru.md](envoy-keys.ru.md). English is the source language; the other is a translation.*

A key describes **the question, not the way it is answered**. `core.target.looked` is "what the
attention of the player is pointed at": in a flat game that is a ray from the camera, in VR from
the gaze or the hand. One question, different implementations.

Every key answers with one of three states: a value / "nobody to ask" / "the provider could not".
A value always has an age; what counts as stale is decided by the subscriber.

## Namespaces

| Prefix | Who owns it |
|---|---|
| `core.*`     | the core of the bridge. Works in any edition of the game |
| `vr.*`       | providers of VR capabilities. May be absent |
| `physics.*`  | providers of physics |
| `envoy.*`    | the service of models itself |
| `<mod>.*`    | any mod, under its own name |

Writing into somebody else namespace is not allowed. An undeclared key is not accepted.

## The core

### Context
    core.context.topic              string  the topic chosen for the utterance
    core.context.menuOpen           bool
    core.context.menuName           string
    core.context.dialoguePartner    form    the other party, if dialogue is open
    core.context.loading            bool

### The player
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

### The hands
    core.hands.right                form    equipped in the right hand
    core.hands.left                 form
    core.hands.rightKind            string  spell | weapon | shield | torch | empty
    core.hands.leftKind             string
    core.hands.shout                form

### Attention and surroundings
    core.target.looked              form    what the attention is pointed at
    core.target.lookedDistance      float
    core.target.lookedHostile       bool
    core.target.lineOfSight         bool    EXPENSIVE
    core.actors.nearest             form    EXPENSIVE
    core.actors.nearestDistance     float   EXPENSIVE
    core.actors.hostileCount        int     EXPENSIVE
    core.actors.followerCount       int

### The world
    core.world.cell                 form
    core.world.cellName             string
    core.world.interior             bool
    core.world.location             form
    core.world.locationKeywords     string  comma separated
    core.world.gameHour             float   0..24
    core.world.weather              form

## The optional namespaces

    vr.hands.rightHeld              form    physically gripped      (HIGGS)
    vr.hands.leftHeld               form                            (HIGGS)
    vr.hands.grabbing               bool                            (HIGGS)
    vr.pose.crouching               bool    physically crouching    (VRIK)
    vr.gesture.last                 string  the recognised gesture  (VRIK)
    physics.contact.actor           form    who there is physical contact with (PLANCK)
    envoy.noiseLevel                float   the level of noise on the microphone
    envoy.engineBusy                bool

A key with no provider answers "nobody to ask" - on SE, on AE and on VR without the mod in
question. That is neither an error nor a reason to fall over.

## The expensive keys

The ones marked `EXPENSIVE` are worked out on request only and cached for the life of the
utterance. The list is in `snapshot.lazyKeys` of the settings, and so are the radius and the limit
of the actor scan.
