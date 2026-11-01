# Achievements

The game's own achievements: the 148 of the "Core" list, unlocked as you play, kept per player (not per save file), and shown by the launcher as they happen.
They are off unless the launcher turns them on.

## Super Mario Eclipse

An Eclipse build has Eclipse's own 128 achievements instead (`eclipse.json`, numbered from 1 as Sunshine's are; the launcher gives them their own file, and the `[achievement]` line says `"game":"eclipse"`).
Where Eclipse keeps its Shines and blue coins is its pause menu tracker's (Eclipse's `src/stage/tracker.cpp`): `game/ach_eclipse_flags.cpp` reads them as it does.
The castle paintings' Power Stars are Shines with event 86 (a Shine every file has), so they save nothing: `game/ach_eclipse_challenges.cpp` counts those taken this session, the 100-coin Shine left out.
Eclipse chooses the file its own way, so its play counts from the first stage after the title screen.
Areas and episodes are Eclipse's `stageArc.bin`'s; `SMS_ACHIEVEMENTS_TEST=survey` (with `SMS_ACHIEVEMENTS_DEBUG=1`) warps through them all and lists each stop's 1-Ups, Shines and star-named actors, which is where the counts in the challenges file come from.
Three are approximate: 124 and 126 count any talk where they ask for one, and 128's statue is placed by its plaque in the castle grounds' map.

## Turning them on

| Variable | What it does |
| --- | --- |
| `SMS_ACHIEVEMENTS` | The file unlocks are kept in (the launcher's, one per player). Unset: achievements are off. |
| `SMS_ACHIEVEMENTS_DEBUG=1` | Logs each stage, each unlock, unlocks refused because the play doesn't count, and each stage's 1-Ups with their slots. |

Each unlock adds a line `<id> <unix time>` to the file and prints, on stdout:

```
[achievement] {"id":12,"time":1761170000}
```

The launcher reads these lines as it reads `[presence]` lines, so its Achievements page updates at once.
A Super Mario Eclipse build has its own story, so achievements are off there.

## When something is missing

At each stage's start, every achievement that can be earned there looks for what it needs (a 1-Up's slot, a basket, a boss, the Cataquacks...).
If something isn't there, the achievement can't be earned in that stage, which is a bug: it is always reported on stderr, once per stage:

```
[achievements] warning: achievement 63 can't find the 1-Up at slot 0x3b8 in area 0x1 scenario 7, so it can't be earned here
```

The tour checks all of them in one run. With `SMS_ACHIEVEMENTS_DEBUG=1 SMS_ACHIEVEMENTS_TEST=tour`, the game warps through every stage and episode where an achievement needs something and ends with a summary (`tour done: ... missing`, then each `tour: MISSING ...`).
Run it after changes to the game's code (decomp patches, objects, managers) and before a release: the stages come from the player's own disc, so what it checks doesn't change otherwise.

`SMS_ACHIEVEMENTS_TEST=setflag@N:ID` (with `SMS_ACHIEVEMENTS_DEBUG=1`) sets save flag `ID` (hex) after `N` ticks of the first stage, to test an unlock and the launcher's side.
`SMS_ACHIEVEMENTS_TEST=banner:ID` shows achievement `ID`'s banner once a stage is playing, without unlocking it. The banner is drawn over the game's picture: `SMS_SHOTS` captures leave it out, `SMS_WINDOW_SHOTS` (with a window, `SDL_VIDEODRIVER=offscreen`) have it.

## What doesn't count

An achievement isn't unlocked by play after a save state is loaded or the warp menu is used (until the next file select), or while a cheat from the F9 menu is on (for the rest of that stage).
Modded stages or code (`SMS_MOD`, as the world editor's Play in game sets; `SMS_CODE_MODS`; a native mod that sets the save file's flags or warps the player) don't count either.
Loading a file that already has everything unlocks nothing: an achievement unlocks when you do the thing, as the reference set's do.

## The parts

| File | Achievements |
| --- | --- |
| `game/ach_core.cpp` | shared state and helpers (`achievements.h`): unlocking, where the player is, whether the play counts |
| `host/ach_store.cpp` | keeps unlocks in `SMS_ACHIEVEMENTS` and tells the launcher |
| `game/ach_banner.cpp` | the banner an unlock shows: the medal, the name and what was done in the game's font on a blue ribbon, and BADGE GET! in the game's big letters. It waits for talks and cutscenes to end. Names from `ach_names.inc`, made from `achievements.json` by `tools/gen_names.py` |
| `game/ach_eclipse_flags.cpp` | Super Mario Eclipse's 1-51: its areas' Shines and blue coins, Episode 7 everywhere, the races, the counts |
| `game/ach_eclipse_challenges.cpp` | Super Mario Eclipse's 1-Ups, Power Stars and challenges |
| `game/ach_survey.cpp` | the survey test aid |
| `host/ach_icon.cpp` | the banner's icon, from `SMS_ACHIEVEMENT_ICONS` (the launcher's `assets/achievements`) |
| `game/ach_flags.cpp` | 1-60: Shines (one, an area's episodes or secrets, the plaza's ten, the boathouse trades, the Shine count), blue coins, nozzle boxes, Yoshi, story flags. Table generated into `ach_flag_rules.inc` by `tools/gen_flag_rules.py` |
| `game/ach_oneups.cpp` | the 1-Ups placed in the stages: one, or all of an area's in one visit |
| `game/ach_plaza.cpp` | Delfino Plaza and the Airstrip's challenges, and the alternate ending |
| `game/ach_bianco_ricco.cpp` | Bianco Hills and Ricco Harbor's challenges |
| `game/ach_gelato_pinna_sirena.cpp` | Gelato Beach, Pinna Park and Sirena Beach's challenges |
| `game/ach_noki_pianta.cpp` | Noki Bay and Pianta Village's challenges |
| `game/ach_anywhere.cpp` | enemy combos and their 1-Up rewards, and Mario's tricks anywhere |

Ids are the Core list's numbers.
`achievements.json` has every achievement's name and what to do, as the banner and the launcher show them; the icons are `assets/achievements/<id>.png`, from the release `tools/mods/achievement-icons.json` pins.
What each achievement checks follows the RetroAchievements set for the game (github.com/timenoe/RAScripts), translated to the decomp's names; the set itself isn't in this repository (`tools/gen_flag_rules.py` takes it as an argument). The names, descriptions and icons are our own.
