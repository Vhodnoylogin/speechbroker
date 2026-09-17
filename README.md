# Skyrim Mods

Our own development for Skyrim: mods, tools and services, one repository, **a branch per piece
of work**.

*Эта страница на русском: [README.ru.md](README.ru.md).*

| Branch | What it is |
|---|---|
| `mo2aibridge` | **MO2 ApI Bridge** — a local HTTP bridge to a running Mod Organizer 2, so that scripts and AI agents can read and change the setup without clicking through its window |
| `envoy` | **Envoy Framework** — an SKSE message bus: the bridge itself plus a voice adapter and a demo subscriber |
| `voice` | the voice service: speech recognition and synthesis |
| `master` | the trunk: these rules, the shared ignore list, and the modules that are finished |

**Each module documents itself.** Open its branch and start from the module's `README.md` —
every module carries an English `README.md` beside a Russian `README.ru.md`, at each level it
has, plus a `CLAUDE.md` manifest addressed to an AI assistant that gets the folder with no
history behind it. The detailed documentation lives next to the code it describes, not here.

## How the branches are laid out

Every module lives on its own branch, permanently checked out into its own working copy:

| Folder | Branch | What it is |
|---|---|---|
| `dev\repo\` | `master` | the trunk; this is where `.git` physically lives |
| `dev\envoy\` | `envoy` | Envoy Framework: the bridge, the voice adapter and the model mod |
| `dev\voice\` | `voice` | the voice service |
| `dev\wt-mo2aibridge\` | `mo2aibridge` | the MO2 ApI Bridge plugin |
| `dev\wt\<task>\` | temporary | a branch for one task inside a module |

That layout buys the main thing: files of two different mods **cannot** end up in one commit —
on a module's branch its neighbour's files simply are not there.

## A temporary copy for one task

```
git -C dev\repo worktree add -b envoy-protocol ..\wt\envoy-protocol envoy
git -C dev\wt\envoy-protocol commit -m "..."
git -C dev\envoy merge envoy-protocol
git -C dev\repo worktree remove ..\wt\envoy-protocol
```

The same branch cannot be checked out into two copies at once — git refuses, and that is a
protection rather than an obstacle.

## A branch inherits the trunk, and that is normal

A new branch forks from `master`, so everything already finished on the trunk turns up in its
working copy. **It must not be deleted.** A commit that deletes it is an ordinary change: merged
back into the trunk, it will carry those files out of there too, and a "clean" branch turns out
to be deferred damage to master.

The rule "a module's branch holds no neighbour's files" is about what is **added and edited** on
the branch, not about what came from the trunk. If the layout itself needs to change, that is a
decision about the shared history, and the user makes it, not the branch.

## A fresh clone

The modules in progress appear after a `git worktree add` for each branch.
