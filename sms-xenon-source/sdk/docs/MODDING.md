# Native mods

A native mod is a program of your own that the game loads at start and runs inside itself.
It hears of what happens in the game (a stage loads, Mario jumps, a coin is taken, a key is pressed), reads and changes the game (Mario's position, health and water, the save file's flags, the camera), draws over the game's picture (panels, text, images, lines and markers in the world), and keeps settings the player changes in a shared **Mods menu** (F1).

Several mods run together.
They all go through the same API, which puts their handlers in order, stacks their panels instead of letting them overlap, warns about keys two of them want, and hands the controllers to one menu at a time.

This page is the whole manual: how to [write one](#quick-start), [what a mod folder holds](#the-mod-folder), [how mods live together](#living-together), the [events](#events) and [functions](#the-api) in full, mods [in Lua](#lua-scripts), [server mods](#server-mods) that run a multiplayer game mode from the host, [game numbers](#game-reference) such as stage and flag ids, [debugging](#debugging), and the [design](#design-notes) behind it all, including what is planned.

The API itself is one C header, [`include/sms_mod.h`](../include/sms_mod.h), with every function and payload documented where it is declared. [`include/sms_mod.hpp`](../include/sms_mod.hpp) adds a header-only C++ layer. Complete example mods, and templates to start your own from, are in [sms-mod-templates](https://github.com/chasem-dev/sms-mod-templates).

Contents:
[Languages](#languages) ·
[Quick start](#quick-start) ·
[The mod folder](#the-mod-folder) ·
[Life of a mod](#life-of-a-mod) ·
[Living together](#living-together) ·
[Events](#events) ·
[The API](#the-api) ·
[Drawing](#drawing) ·
[Input](#input) ·
[Settings and the Mods menu](#settings-and-the-mods-menu) ·
[Talking to other mods](#talking-to-other-mods) ·
[Lua scripts](#lua-scripts) ·
[Server mods](#server-mods) ·
[Time, units and frame rate](#time-units-and-frame-rate) ·
[Multiplayer, save states, Eclipse](#multiplayer-save-states-and-eclipse) ·
[Game reference](#game-reference) ·
[Debugging](#debugging) ·
[Compatibility](#compatibility) ·
[Pre-releases](#pre-releases) ·
[Design notes](#design-notes) ·
[Adding to the API](#adding-to-the-api)

## Languages

The API is a C ABI: a mod is a shared library (`.so` on Linux, `.dll` on Windows, `.dylib` on macOS) that exports two functions, and it reaches the game only through tables of function pointers the game hands it.
It links against nothing of the game's, so it builds without the game's sources and keeps working across game updates (see [Compatibility](#compatibility)).
Any language that can make such a library works:

| Language | How | Notes |
| --- | --- | --- |
| **C** | include `sms_mod.h` | The reference. `trainer-hud` and `stage-timer` are written in C. |
| **C++** | include `sms_mod.hpp` (C++11) | Lambdas for handlers, payloads by type, a class for the mod's state; exceptions are caught before they reach the game. `hello` is written in C++. |
| **Rust** | `bindgen` on `sms_mod.h`, a `cdylib` crate | Export `sms_mod_api_version` and `sms_mod_load` with `#[no_mangle] pub extern "C"`; see [Rust](#rust). |
| **Zig, Odin, Nim, D, …** | their C import or FFI | Anything that can export C functions and call C function pointers. |
| **Lua** | `runtime = lua` in `mod.ini`, a `main.lua` | The `lua` mod runs script mods: no compiler, no binary per platform, reloaded on save. See [Lua scripts](#lua-scripts); `lua-challenges` is written in Lua. |
| **Other script languages** | a native mod hosting the runtime | `core->host_runtime` lets a mod run script mods of a language it embeds, as the lua mod does ([Other languages](#other-languages)). |

A native mod runs inside the game with the game's rights; players should install mods they trust, as with any program.

### Rust

A Rust mod is a `cdylib` with bindings generated from the header:

```toml
# Cargo.toml
[lib]
crate-type = ["cdylib"]
[build-dependencies]
bindgen = "0.69"
```

```rust
// build.rs: bindgen::Builder::default().header("sms-mod-sdk/include/sms_mod.h") ... .write_to_file(out.join("sms.rs"))
include!(concat!(env!("OUT_DIR"), "/sms.rs"));

static mut API: *const SmsApi = std::ptr::null();

#[no_mangle]
pub extern "C" fn sms_mod_api_version() -> u32 { (SMS_MOD_API_MAJOR << 16) | SMS_MOD_API_MINOR }

#[no_mangle]
pub extern "C" fn sms_mod_load(m: *mut SmsMod, api: *const SmsApi) -> i32 {
    unsafe {
        API = api;
        let core = &*(*api).core;
        (core.subscribe.unwrap())(m, SmsEventType_SMS_EV_DRAW, Some(on_draw), std::ptr::null_mut(), 0);
    }
    0
}

unsafe extern "C" fn on_draw(_m: *mut SmsMod, _ev: *mut SmsEvent, _u: *mut std::ffi::c_void) -> i32 {
    let d = &*(*API).draw;
    (d.text.unwrap())(20.0, 400.0, 14.0, 0xFFFFFFFF, 1, c"Hello from Rust".as_ptr());
    0
}
```

Do not let a panic unwind into the game: wrap handler bodies in `std::panic::catch_unwind`, or build with `panic = "abort"`.
The game never builds this crate itself; this outline has not been run against a game build.

## Quick start

1. **Start from a template.** [sms-mod-templates](https://github.com/chasem-dev/sms-mod-templates) has a folder for each language to copy as your mod's own repository: [`templates/cpp`](https://github.com/chasem-dev/sms-mod-templates/tree/main/templates/cpp), [`templates/c`](https://github.com/chasem-dev/sms-mod-templates/tree/main/templates/c) and [`templates/lua`](https://github.com/chasem-dev/sms-mod-templates/tree/main/templates/lua).
   Each builds with CMake, fetching this SDK by its version tag, and has a GitHub Actions workflow that builds the mod for every platform and zips it for a release.
   The [examples](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples) are complete mods to read: `hello` (C++) is the smallest; `trainer-hud` (C) shows almost everything.
2. **Build it.** The SDK's CMake helper lays the mod out as the game loads it:
   ```sh
   cmake -S . -B build -DSMS_MODS_OUT=/path/to/the/game/mods -DSMS_MOD_ARCH=32
   cmake --build build
   ```
   `SMS_MOD_ARCH=32` is for the default Linux game (`build/linux-32`), which is a 32-bit program; a mod must have the game's pointer size.
   For a 64-bit game (`SMS_ARCH=64`, Windows, macOS) leave it out.
   In a CMake project of your own, get the SDK with FetchContent and call `sms_add_mod(<folder with mod.ini> <sources>)`:
   ```cmake
   include(FetchContent)
   FetchContent_Declare(sms_mod_sdk
     GIT_REPOSITORY https://github.com/chasem-dev/sms-mod-sdk.git
     GIT_TAG v1.5.0)
   FetchContent_MakeAvailable(sms_mod_sdk)
   sms_add_mod(mod src/my_mod.c)
   ```
   `-DFETCHCONTENT_SOURCE_DIR_SMS_MOD_SDK=<path>` builds against a checkout of the SDK instead.
   Without CMake, one command does: `cc -shared -fPIC -fvisibility=hidden -O2 -I sms-mod-sdk/include mymod.c -o mods/mymod/bin/linux-x86_64/mymod.so` (add `-m32` and use `linux-x86` for the 32-bit game).
3. **Run the game.** Every folder in `mods/` with a `mod.ini` is loaded. The log says so:
   ```
   [mods] API 1.1: 3 native mod(s) in ../../mods (linux-x86); settings in ~/.local/share/sms-port/mods
   [mod:hello] loaded Hello 1.0.0 by SMS PC port examples
   [mod:hello] hello from DELFINO PLAZA, episode 1
   ```
4. **Press F1** in the game for the Mods menu: each mod, whether it loaded (and why not), a switch for it, and its settings.

The smallest mod in C:

```c
#include <sms_mod.h>

static const SmsApi* api;

static int on_draw(SmsMod* mod, SmsEvent* ev, void* user)
{
    const SmsEvDraw* d = (const SmsEvDraw*)ev->data;
    if (d->mode == SMS_MODE_STAGE)
        api->draw->text(20, 440, 14, SMS_RGB(255, 255, 255), SMS_TEXT_SHADOW, "Hello, Delfino!");
    return SMS_CONTINUE;
}

SMS_MOD_DEFINE()

SMS_MOD_EXTERN_C SMS_MOD_EXPORT int sms_mod_load(SmsMod* mod, const SmsApi* a)
{
    api = a;
    api->core->subscribe(mod, SMS_EV_DRAW, on_draw, NULL, SMS_PRIORITY_NORMAL);
    return 0; /* loaded */
}
```

The same in C++:

```cpp
#include <sms_mod.hpp>

struct Hello : sms::Mod {
    using sms::Mod::Mod;
    int load() override {
        on<SmsEvDraw>(SMS_EV_DRAW, [this](SmsEvDraw& d) {
            if (d.mode == SMS_MODE_STAGE)
                draw().text(20, 440, 14, SMS_RGB(255, 255, 255), SMS_TEXT_SHADOW, "Hello, Delfino!");
        });
        return 0;
    }
};
SMS_MOD_CLASS(Hello)
```

## The mod folder

```
mods/
  trainer-hud/
    mod.ini                         what the mod is (required)
    bin/linux-x86/trainer_hud.so    the library, one per platform
    bin/linux-x86_64/trainer_hud.so
    bin/windows-x86_64/trainer_hud.dll
    bin/macos-arm64/trainer_hud.dylib
    logo.png                        anything else the mod reads (draw->image_load, core->mod_dir)
```

The game looks for the library in `bin/<platform>/`, then `<platform>/`, then the mod's folder itself, so one folder can carry every platform.
The platforms are `linux-x86` (the default 32-bit Linux game), `linux-x86_64`, `linux-arm64`, `windows-x86_64`, `windows-x86`, `macos-x86_64` and `macos-arm64`; the log names the one it wants.
A mod folder may sit beside the other things in `mods/` (texture packs, Eclipse, language packs); only folders with a `mod.ini` are native mods.

### mod.ini

`key = value` lines; `#` starts a comment.

| Key | Meaning | Default |
| --- | --- | --- |
| `id` | Unique name: letters, digits, `-`, `_`, `.`. Settings, services, logs and other mods' `after`/`requires` use it. | the folder's name |
| `name` | Shown in the Mods menu. | the id |
| `version` | Shown in the Mods menu and the log. | `0` |
| `author`, `description` | Shown in the Mods menu. | |
| `library` | The library's file name without `.so`/`.dll`/`.dylib`. | the id |
| `after` | Mods (ids, comma separated) to load before this one when they are there. | |
| `before` | Mods to load after this one when they are there. | |
| `requires` | Mods that must be loaded first; without them this mod is not loaded. | |
| `conflicts` | Mods this one cannot run with; the one that would load second is not loaded. | |
| `priority` | Load order among mods with no `after`/`before` between them: lower first, then by id. | `0` |
| `runtime` | A script mod: the language that runs it (`lua`). It has no library; the native mod hosting that runtime starts it ([Lua scripts](#lua-scripts)). | |
| `script` | A script mod's file, in its folder. | `main.<runtime>` |
| `gameplay` | `1` when the mod changes how the game plays (rather than only showing things). The log lists such mods; multiplayer will compare them between players (see [Multiplayer](#multiplayer-save-states-and-eclipse)). | `0` |
| `side` | `server`: a [server mod](#server-mods), loaded only in a game hosting multiplayer (players need nothing); `local`: wherever it is installed. | `local` |
| `client_api` | A server mod: the mod API players' games need (`1.2`: the commands it sends). The host refuses older games, saying why ([Versions](#versions)). | any |

## Life of a mod

1. **Found.** At the first frame the game reads every `mods/*/mod.ini` (or the folders in `SMS_NATIVE_MODS_DIR`).
   Mods switched off in the Mods menu or listed in `native_mods_disabled` are left alone.
2. **Ordered.** `requires` and `after` load first, `before` later; otherwise by `priority`, then id. A cycle is logged and broken by priority.
3. **Loaded.** Native mods first, then script mods (`runtime = lua`), each in order. For a script mod, the native mod hosting its runtime starts it; without one, it is not loaded and the Mods menu says what it needs. For a native mod, the game opens the library, checks `sms_mod_api_version()` (a mod made for another major version is refused), and calls `sms_mod_load(mod, api)`.
   Subscribe, add options, bind keys, load images and provide services there.
   Return 0 to load; anything else refuses (logged, and shown in the Mods menu).
4. **Running.** Events arrive on the game thread, every frame, tick and happening the mod subscribed to.
5. **Unloaded** at exit, when the player switches the mod off in the Mods menu, or for a [hot reload](#debugging).
   The game calls `sms_mod_unload(mod)` if the mod has one, then drops everything the mod registered: its handlers, services, key bindings, options, images and the input capture.
   It then closes the library, so a mod must stop any thread it started in `sms_mod_unload`.
   A mod switched on again in the Mods menu loads at once, mid-game: `game->state` says whether a stage is already being played.
   Script mods stop before the mod running them unloads, and start again when it is back.

A mod whose handler throws a C++ exception through the API is switched off and logged; one that crashes takes the game with it, as any code in the process would.

## Living together

The API is built so mods that do different things never get in each other's way, and so it is clear what happens when two want the same thing.

- **One thread.** Every API function (but `core->log`) and every handler runs on the game thread, between or inside the game's own steps. Do slow work (files, networks) on threads of your own and hand the results over; never call the API from them.
- **Ordered handlers.** Each event reaches its handlers by priority (`SMS_PRIORITY_FIRST` -1000, `EARLY` -100, `NORMAL` 0, `LATE` 100, `MONITOR` 1000), then load order, then the order they subscribed. `MONITOR` handlers watch the outcome and must not change it.
- **Three kinds of event.** An event's documentation says which it is:
  - *Changeable*: handlers may change fields of the payload; each sees the changes of the ones before (an exit's destination, the camera's field of view, the controllers).
  - *Cancellable* (a veto): returning `SMS_CANCEL` stops what the game was about to do (a hit, a stage exit, damage). Every handler still runs; later ones see `ev->cancelled` and `ev->cancelled_by` (the id of the first mod that cancelled), so a `MONITOR` handler can tell a hit that landed from one another mod stopped.
  - *Consumable*: the first handler to return `SMS_CANCEL` takes it, and no later handler or the game sees it (a key press, the player's model).
- **Screen space.** Take the rectangles you draw panels in from `draw->hud_slot(mod, anchor, w, h, &rect)`. Panels at one anchor stack, in handler order, and a panel that is hidden simply takes no slot, so the others move up. The slots start clear of the game's own HUD.
- **Keys.** Ask for keys with `input->bind_key(mod, "action", "F3")`. The player can change each in the mod's settings (`key.action = F6`), and the game warns in the log when the key is one of the port's own (the Mods menu, F5–F11, the backtick, Escape, every key in `bindings.txt`) or another mod's.
  Consume your key's events in `SMS_EV_KEY` so the game's controls do not also see them.
- **Menus.** A mod with a menu of its own calls `input->capture(mod, 1)` while it is open: the game then sees no buttons or sticks, keys reach only that mod, and other mods' `capture` calls fail until it lets go. Most mods need no menu: [options](#settings-and-the-mods-menu) appear in the shared Mods menu.
- **Settings and files** are per mod: `config->*` keeps `key = value` settings in the mod's data folder, `core->data_dir(mod)` is a folder of its own to write, and `core->mod_dir(mod)` is its install folder (treat it as read only; the launcher may replace it).
- **Names.** Custom events and services are named by convention `<mod id>.<name>`, so two mods never pick the same one.

## Events

Subscribe with `core->subscribe(mod, type, fn, user, priority)`; `fn(mod, ev, user)` gets an `SmsEvent` whose `data` points to the payload struct named below (check `ev->size` against `sizeof` the struct before reading fields added in a later minor version).

| Event | When | Payload | Handlers may |
| --- | --- | --- | --- |
| `SMS_EV_FRAME` | the top of every frame: title, file select, stages, movies | `SmsEvFrame` frame number, game mode | |
| `SMS_EV_TICK` | every simulation tick of a stage, after everything moved, before drawing; not while the pause menu, map or a save screen is up | `SmsEvTick` tick, `dt`, ticks per second | move things |
| `SMS_EV_DRAW` | once a frame, after `SMS_EV_FRAME` | `SmsEvDraw` canvas size, frame, mode | draw (the only place) |
| `SMS_EV_STAGE_LOADED` | a stage's objects are loaded and set up | `SmsEvStage` area, episode | |
| `SMS_EV_STAGE_EXIT_REQUEST` | the game asks to leave for another area (pipe, painting, script) | `SmsEvStageExit` code, area, episode, the asking actor | cancel (stay) |
| `SMS_EV_STAGE_LEAVING` | the stage is about to change, any way out | `SmsEvStage` where it goes, and (1.4) why: `reason`, `shine` | change area/episode |
| `SMS_EV_STAGE_LOADING` | (1.5) a stage is about to load, its episode chosen (after a world's episode select, after the game picks the plaza's episode; every load `SMS_EV_STAGE_LOADED` comes for) | `SmsEvStageLoading` the stage, why the last was left (`reason`, `shine`), and where Mario comes from (`from_area`, `from_episode`) | change area/episode (that stage loads instead) and from_area/from_episode |
| `SMS_EV_STAGE_START` | (1.4) the stage's opening is over (its cutscene, the episode's name, "Go!") and the player has the controls | `SmsEvStage` the stage, and how it was reached | |
| `SMS_EV_LIFE_LOST` | Mario lost a life and has lives left | `SmsEvStage` where he goes | change area/episode |
| `SMS_EV_GAME_OVER` | Mario lost his last life | none | cancel (carry on as after any lost life) |
| `SMS_EV_COINS_RESET` | a lost life is about to reset the coins | none | cancel (keep the coins) |
| `SMS_EV_FILE_SELECTED` | a file was chosen at file select | `SmsEvStage` where it starts | change area/episode |
| `SMS_EV_ITEM_COLLECTED` | an item was picked up: coin, red or blue coin, other `TItem`s | `SmsEvItem` item, taker, kind, actor type, position | |
| `SMS_EV_PAD_READ` | the controllers were read, before the game sees them | `SmsEvPadRead` the four pads | change the pads |
| `SMS_EV_KEY` | a key went down, repeated or up in the window | `SmsEvKey` scancode, down, repeat, modifiers | consume |
| `SMS_EV_CUTSCENE` | a camera cutscene was asked for | `SmsEvCutscene` camera name, centre, frames, actor | |
| `SMS_EV_GOOP_CLEANED` | goop is about to be cleaned at a place | `SmsEvGoop` position, size | |
| `SMS_EV_OBJECT_HIT` | the player or their water hit an object (sprayed, stomped, punched, ground-pounded) | `SmsEvHit` object, sender, [message](#hit-messages) | cancel (the object never hears of it) |
| `SMS_EV_SAVE_STATE_REQUEST` | a save state was asked for | `SmsEvSaveState` reason | cancel (set `reason` for the message shown) |
| `SMS_EV_HIDE_PLAYER_MODEL` | Mario's models are about to be drawn | `SmsEvObject` Mario | consume (leave them out; draw him your own way) |
| `SMS_EV_PLAYER_STATUS` | Mario's [status](#mario-statuses) (his action) is about to change | `SmsEvPlayerStatus` from, to, arg, forced | change `to`/`arg`; cancel (he stays as he is) |
| `SMS_EV_PLAYER_NOZZLE` | (1.5) FLUDD is about to change [nozzle](#nozzles), or a nozzle's box is about to be set up as the stage loads | `SmsEvPlayerNozzle` from, to, animate, [reason](#nozzles), box | change `to`; cancel (the nozzle stays; a box is not in the stage) |
| `SMS_EV_PLAYER_DAMAGE` | Mario is about to lose health, or air under water | `SmsEvDamage` amount, underwater, (1.5) `source`: the enemy or object that hurt him | change `amount`; cancel (he loses nothing) |
| `SMS_EV_SHINE_COLLECTED` | a Shine's flag was set for the first time on this file | `SmsEvShine` shine number, total | |
| `SMS_EV_CAMERA` | the game camera placed itself this tick and is about to make its matrices | `SmsEvCamera` position, target, fovy, aspect | change position, target, fovy |
| `SMS_EV_OBJECT_MOVE` | an object is about to move this tick (`after` 0), or has (`after` 1) | `SmsEvObjectMove` object, after | (many per tick: subscribe only when needed) |
| `SMS_EV_MOD_LOADED` | another mod loaded; on subscribing, once for each mod already loaded | `SmsEvMod` id, name, version | |
| `SMS_EV_SHUTDOWN` | the game is closing | none | save |
| `SMS_EV_NET_PLAYER_JOINED`, `_LEFT` | a player joined or left a multiplayer game | `SmsEvNetPlayer` | |
| `SMS_EV_NET_PLAYER_ARRIVED` | (host) a player arrived in the stage session it plays | `SmsEvNetPlayer` | bring them up to date ([Server mods](#server-mods)) |
| `SMS_EV_NET_SESSION` | (host) a new stage session, which everyone follows | `SmsEvNetSession` | set the stage up |
| `SMS_EV_CUSTOM_BASE` and up | mods' own events, from `core->event_id(name)` | whatever the sender says | cancel |

#### Events for every kind of mod (1.5)

API 1.5 adds events in the manner of other games' modding APIs (an entity damaged, a player joining the world): what Mario does, what happens to the stage's actors, and what the game does around them, each general enough for many mods. Those of Mario are only for the player's Mario, never Shadow Mario. Check `ev->size` against `sizeof` the payload as for any event; a game older than 1.5 never sends them.

| Event | When | Payload | Handlers may |
| --- | --- | --- | --- |
| **Mario** | | | |
| `SMS_EV_PLAYER_SPAWN` | Mario is about to be put at the stage's start, as it loads (before `SMS_EV_STAGE_LOADED`) | `SmsEvPlayerSpawn` position, yaw, `has_position`, `kind` (`SMS_START_*`: standing, rolling in, back from a world, the coaster) | change `position` and `yaw`, or set `has_position` 1 to place him in a stage that has no start |
| `SMS_EV_PLAYER_STATUS_CHANGED` | Mario's [status](#mario-statuses) changed (`SMS_EV_PLAYER_STATUS` asks; this tells what he took) | `SmsEvPlayerStatus` from, to, arg | |
| `SMS_EV_PLAYER_JUMP` | Mario left the ground: a jump, second or triple jump, back flip, side flip, long jump, wall jump, spin jump, dive, a stomp's bounce, the rocket | `SmsEvPlayerJump` the jump's status, the one before, position, velocity | |
| `SMS_EV_PLAYER_LAND` | Mario touched the ground after being in the air (not water: `SMS_EV_PLAYER_WATER`) | `SmsEvPlayerLand` status, the one in the air, `fall` (from the highest point), `speed`, position | |
| `SMS_EV_PLAYER_HIT` | something touched or hit Mario: an enemy's attack (each tick it touches him), fire, a coin or an item, a Blooper ridden | `SmsEvHit` object (Mario), sender, [message](#hit-messages) | cancel (he ignores it: no damage, no pickup) |
| `SMS_EV_PLAYER_HEAL` | Mario is about to gain health (a coin, a heart, fruit, a 1-up, a Shine, his cap) or, under water, air | `SmsEvDamage` amount, underwater | change `amount`; cancel (he gains nothing) |
| `SMS_EV_PLAYER_DEATH` | Mario lost his life, as he falls (before `SMS_EV_LIFE_LOST` or `SMS_EV_GAME_OVER`) | `SmsEvPlayerDeath` [cause](#death-causes), position, `source` (what took his last health, when an enemy did) | |
| `SMS_EV_PLAYER_MOUNT` | Mario got on or off Yoshi or a Blooper | `SmsEvPlayerMount` mount (`SMS_MOUNT_YOSHI`, `_BLOOPER`), on, the Blooper, colour | |
| `SMS_EV_PLAYER_WATER` | Mario went into the water or came out of it | `SmsEvPlayerWater` entered, the surface's height, position | |
| `SMS_EV_YOSHI_HATCH` | Yoshi hatched from his egg | `SmsEvYoshi` the fruit that hatched him, his colour (`SMS_YOSHI_*`), position | |
| `SMS_EV_YOSHI_EAT` | Yoshi ate a fruit (his colour may have changed) | `SmsEvYoshi` | |
| **Actors** | | | |
| `SMS_EV_ACTOR_SPAWN` | an actor came into the stage while it is played: an enemy placed (again) by its generator, an item made to appear (a coin out of a defeated enemy, a 1-up); not those the stage loads with | `SmsEvActor` object, actor type (`SMS_ACTOR_*` bits), position | |
| `SMS_EV_ACTOR_DAMAGE` | the player hit an enemy (sprayed, stomped, ground-pounded, punched) and it lost health | `SmsEvActorDamage` object, attacker (Mario or his water), actor type, [message](#hit-messages), health before and after, most health, position | |
| `SMS_EV_ACTOR_DEATH` | an enemy was defeated (as it leaves its coins), a boss beaten, or an enemy killed by the stage (`forced`: a pit) | `SmsEvActor` object, actor type, position, `other` (the last of the player's hits on it, or NULL), `forced` | |
| `SMS_EV_SHINE_APPEAR` | a Shine appears during play: an event's, a boss's, the red coins' | `SmsEvShineActor` object, shine, `had` (the file has it), position | |
| `SMS_EV_SHINE_GET` | Mario took a Shine: every time, one the file has too (before its Shine Get; `SMS_EV_SHINE_COLLECTED` comes as a new one is counted) | `SmsEvShineActor` | |
| `SMS_EV_MAP_EVENT` | one of the stage's map events is set off: its goop was cleaned, and a building rises or a gate opens | `SmsEvObject` the event | |
| **The game** | | | |
| `SMS_EV_FLAG_CHANGED` | a save-file [flag](#flags) is about to change: coins, lives, Shines, blue and red coins, events (not when a file is loaded or the stage's flags reset at once) | `SmsEvFlag` id, old, value | change `value`; cancel (it stays) |
| `SMS_EV_SOUND` | a sound effect is about to play: every one, Mario's voice, steps, enemies, menus (many a tick: subscribe only when needed) | `SmsEvSound` id (the game's `MSD_SE_*`), system, position | change `id`; cancel (not played) |
| `SMS_EV_MUSIC` | music is about to start (`start` 1) or stop (0) | `SmsEvMusic` id (`MSD_BGM_*`), start | starting: change `id`; cancel (not played) |
| `SMS_EV_TALK` | a talk with an NPC (or a sign) starts, shows a message, or ends | `SmsEvTalk` what (`SMS_TALK_START`, `_MESSAGE`, `_END`), npc, message, last | change `message` (another of the stage's or the game's) |
| `SMS_EV_CUTSCENE_END` | a camera cutscene (`SMS_EV_CUTSCENE`'s) is over | `SmsEvCutsceneEnd` name, actor, `forced` (cut short) | |
| `SMS_EV_PAUSE` | the pause menu, the map or a save screen opened or closed: the stage stands still meanwhile | `SmsEvPause` paused, kind (`SMS_PAUSE_MENU`, `_MAP`, `_SAVE`) | |
| `SMS_EV_MODE_CHANGED` | the [game mode](#game) changed: boot, the title, a stage, a movie | `SmsEvMode` from, to | |
| `SMS_EV_SAVE_PROMPT` | the stage is about to ask the player to save (after a Shine, a blue coin, a nozzle's box, Yoshi) | `SmsEvSavePrompt` kind (`SMS_SAVE_*`) | cancel (it does not ask; the stage goes on as after it) |
| `SMS_EV_GAME_SAVED` | the game was saved: the pause menu's save, the prompt, auto save | `SmsEvGameSaved` file | |
| `SMS_EV_TIMER` | the HUD's timer (a timed event's, the red coins') started, stopped, or a countdown ran out | `SmsEvTimer` what (`SMS_TIMER_STARTED`, `_STOPPED`, `_OUT`), time (hundredths of a second) | |
| `SMS_EV_SCRIPT_CALL` | a stage's event script calls one of the game's functions (`"startTimer"`, `"setNextStage"`, `"appearShine"`...; many a tick: the scripts poll) | `SmsEvScriptCall` name, owner, argc | |
| `SMS_EV_SAVE_FILE` | (1.5) a save file was loaded or started anew at file select, saved, copied or erased | `SmsEvSaveFile` what (`SMS_SAVE_FILE_LOADED`, `_NEW`, `_SAVED`, `_COPIED`, `_ERASED`), file (0..2), from (a copy's) | read and write its [files](#files) |
| `SMS_EV_NOZZLE_FIRE` | (1.5) FLUDD's trigger is about to fire: a burst (A while R is held, with the spray) or the pressure built up (rocket, hover, turbo) | `SmsEvNozzleFire` nozzle, how (`SMS_FIRE_BURST`, `_PRESSURE`) | cancel (it does not fire) |
| `SMS_EV_SHINE_SETUP` | (1.5) a Shine is made as its stage loads | `SmsEvShineSetup` object, shine, collected, model | name a model file of the mod's for it |
| `SMS_EV_YOSHI_EGG` | (1.5) whether Yoshi's egg is there: an egg the stage places, the plaza's egg event, that event's egg | `SmsEvYoshiEgg` kind (`SMS_EGG_STAGE`, `_PLAZA_EVENT`, `_EVENT_EGG`), present, object | change `present` |
| `SMS_EV_EPISODE_SHINE` | (1.5) the episode select or the pause menu asks which Shine an episode stands for | `SmsEvEpisodeShine` area, episode, shine | change `shine` |
| `SMS_EV_FILE_STARTING` | (1.5) a file chosen at file select is about to start: again at each step of the file select while a mod cancels it, until the game leaves for its first stage | `SmsEvFileStarting` file (-1 without a memory card), is_new | cancel (the file select waits this frame) |
| `SMS_EV_FILE_MENU` | (1.6) a save file's menu at file select (START, COPY, ERASE, SCORE) opened, ready for the player's choice, or closed | `SmsEvFileMenu` file, is_empty, open, item (`SMS_FILE_MENU_START`, `_COPY`, `_ERASE`, `_SCORE`; closed without a choice -1) | |

Notes on them:

- **Before and after.** Events that may be changed or cancelled come before the game acts (`PLAYER_HEAL`, `PLAYER_HIT`, `FLAG_CHANGED`, `SOUND`, `MUSIC`, `SAVE_PROMPT`); the others tell what happened. `SMS_EV_PLAYER_STATUS` asks and may be refused by the game; `SMS_EV_PLAYER_STATUS_CHANGED`, `JUMP` and `LAND` come only for what he did.
- **Damage and its source.** `SmsEvDamage.source` (check `ev->size`) is the enemy or object whose hit takes Mario's health, so a mod tells an enemy's attack from fire, goop or a hard landing (`source` NULL). `SMS_EV_PLAYER_DEATH`'s `source` is the same for the last health he lost. No damage comes once he has lost his life.
- **Enemies' health.** `SMS_EV_ACTOR_DAMAGE` comes for the player's hits that take an enemy's health at once. Enemies keep their own rules: a stomp that defeats a small enemy takes it to 1 and then its defeat, a boss loses health on its own weak spots. `game->object_health` reads it at any time. Every defeated small enemy comes through `SMS_EV_ACTOR_DEATH`, and these bosses as they are beaten: Petey Piranha, Gooper Blooper, King Boo, Eely-Mouth and the hot Chain Chomp. A few enemies that explode in their own way (Bob-ombs, Bullet Bills) and the other bosses do not; the Shine a boss leaves comes through `SMS_EV_SHINE_APPEAR`.
- **The spawn.** `SMS_EV_PLAYER_SPAWN` changes where the stage puts Mario as it would its own start: a stage whose opening turns Mario (a cutscene facing the camera) may still turn him. The Pinna Park coaster (`SMS_START_COASTER`) takes no place. (1.5) `kind` may be changed to `SMS_START_WAIT`, `_ROLL` or `_RETURN`: he starts that way, and a coaster start changed so is a start like any other stage's, without the coaster (and then takes a place). Nothing is changed to the coaster.
- **FLUDD's trigger.** `SMS_EV_NOZZLE_FIRE` comes only for the player's Mario, as the trigger would fire: a burst once each time A is pressed while R is held (asked again at the next A when cancelled), the pressure's when it is full (cancelled, it is spent until R is let go, as after a launch). Holding R to spray, and the hover nozzle hovering, are not the trigger's and go on.
- **A Shine's model.** `SMS_EV_SHINE_SETUP`'s `model` names a model file (`.bmd`) of the mod's, relative to its folder (`core->mod_dir`) or a full path; the Shine takes it instead of the game's (`collected`: the see-through one the game gives a Shine the file has). The Shine's animations play on it, so it keeps the Shine model's joints and materials: a Shine recoloured, say. A file is loaded once a stage, so many Shines may name the same one; a file that is not a model is left (the log says why). `game->object_set_color(shine, SMS_COLOR_MATERIAL, colour)` tints a model whose materials use their material colour (the game's Shine does); call it every tick to animate it. Every Shine of the same model shares its colours.
- **Yoshi's egg.** `SMS_EV_YOSHI_EGG` comes as an egg loads (`SMS_EGG_STAGE`: there when the file has Yoshi's Shine, 0x21; in the plaza, once Yoshi has hatched there, flag 0x1038F), as the plaza's episode 8 sets up its egg event (`SMS_EGG_PLAZA_EVENT`: when Yoshi has not hatched and the file has Yoshi's Shine; the event writes flag 0x60003, which `SMS_EV_FLAG_CHANGED` may change), and as that event's own egg loads (`SMS_EGG_EVENT_EGG`: when the event was set up).
- **Before a game starts.** `SMS_EV_FILE_STARTING` comes once a file is chosen (a new game or one loaded, after `SMS_EV_SAVE_FILE`'s `LOADED` or `NEW`), and again at each step of the file select (more than once a frame: time a wait with `core->time`, not by counting) while a mod cancels it; cancel it and the file select waits as it is, drawn as ever, for that step. That is the time for a mod's own screen before a game: its options, a seed. Open your menu when it first comes (for a new game only: `is_new`), take the controllers (`input->capture`), cancel every frame while the menu is up, and stop cancelling when the player is done: the game starts then, with `SMS_EV_FILE_SELECTED` (where it starts, still changeable) next. The game waits as long as you cancel; a mod switched off or unloaded stops holding it. On a multiplayer client the wait delays its joining the host.
- **A file's menu.** (1.6) `SMS_EV_FILE_MENU` comes when the player jumps under a file's block and its menu opens (`open` 1), once it takes the player's choice, and when it closes (`open` 0): `item` the item chosen, or -1 for B (or the memory card taken out). It comes before the game acts on the choice, which it does once the menu has folded away. `is_empty` is a file with no game ("New"), whose menu offers START alone. A mod's screen for a new game may open here instead of at `SMS_EV_FILE_STARTING`, so the player can still go back: while the menu is open, take the controllers (`input->capture`) and draw your screen over it; when the player presses A or B on your screen, close it and let go of the controllers in your `SMS_EV_PAD_READ` handler that frame (`input->capture(mod, 0)`). The press then reaches the game: A chooses the item the cursor is on (START for an empty file), B closes the menu with nothing chosen.
- **Episodes' Shines.** `SMS_EV_EPISODE_SHINE` is the game's own table (`world->episode_shine`) where the episode select and the pause menu read it: the episode's name, whether it shows collected, and so how many episodes the select offers (an episode past the last collected one is the newest). The select asks for each episode as it opens and as its cursor moves; return quickly. Which episode a stage loads is not changed (`SMS_EV_STAGE_LOADING` does that).
- **Flags and sounds are many.** `SMS_EV_FLAG_CHANGED` comes for every flag the game writes one at a time (several as a stage loads); `SMS_EV_SOUND` and `SMS_EV_SCRIPT_CALL` come many times a tick. Filter by `id` or `name` and return quickly.
- **Object names.** Actors' names are the stage's, often Japanese (Shift-JIS); their actor type is the way to tell them apart: `SMS_ACTOR_ENEMY`, `SMS_ACTOR_BOSS`, `SMS_ACTOR_ITEM` (coins 0x2000000E, Shines 0x20000013), `SMS_ACTOR_OBJECT`, and `game->object_type`.

#### Death causes

`SmsEvPlayerDeath.cause` (Lua `ev.cause`): `SMS_DEATH_HEALTH` (`"health"`: no health left, from an enemy, fire, a hard fall, or too long off the map), `SMS_DEATH_DROWN` (`"drown"`), `SMS_DEATH_PIT` (`"pit"`: a bottomless pit's floor of death), `SMS_DEATH_GOOP` (`"goop"`: sinking or deadly goop), `SMS_DEATH_ELECTRIC` (`"electric"`), `SMS_DEATH_CRASH` (`"crash"`: a Blooper into a wall), `SMS_DEATH_OTHER` (`"other"`: a stage's script, a timed event failed, the coaster).

Notes:

- The title screen's background is a stage being played (area 15, Delfino Plaza): `SMS_EV_STAGE_LOADED` comes for it, and Mario exists. Leave area 15 out to act only in the player's own game.
- `SMS_EV_PLAYER_STATUS` and `SMS_EV_PLAYER_DAMAGE` are only sent for the player's Mario, not Shadow Mario (who is a Mario too).
- `SMS_EV_CAMERA`: a new position or target changes only what is drawn this tick; the camera keeps following Mario as before. A new field of view is given to the camera for the tick (its culling and screen effects use it too) and taken back the next tick, unless the camera set one itself in the meantime.
- `SMS_EV_TICK` and `SMS_EV_FRAME` are not the same clock: at 60 or 120 frames per second, see [Time](#time-units-and-frame-rate).
- `SMS_EV_PAD_READ`: `input->pad` still answers with the controllers as read, before any mod's change.
- `SmsEvStage.reason` (1.4; check `ev->size`) says why the game leaves a stage (`SMS_EV_STAGE_LEAVING`, `SMS_EV_LIFE_LOST`, `SMS_EV_FILE_SELECTED`) or how it came to this one (`SMS_EV_STAGE_LOADED`, `SMS_EV_STAGE_START`): `SMS_LEAVE_WAY_OUT` (a pipe, a painting, a door, an event), `SMS_LEAVE_SHINE` (a Shine ended the stage: its Shine Get and its save, by the prompt or Auto Save, are over; `shine` is which), `SMS_LEAVE_EXIT_AREA` (the pause menu), `SMS_LEAVE_LIFE_LOST`, `SMS_LEAVE_GAME_OVER`, `SMS_LEAVE_WARP` (a mod's `game->warp`), `SMS_LEAVE_FILE` (file select), `SMS_LEAVE_OTHER`.
- `SMS_EV_STAGE_LEAVING` to a world's way in (`SMS_AREA_WORLD`) with an episode named goes straight to it, past the episode select, as if it had been chosen there: the game records the episode and starts the coins afresh. Episode `SMS_EPISODE_SELECT` (255) shows the episode select. Handlers that change where the game goes run before the multiplayer extension reads it, so on a host the players follow the stage chosen.
- `SMS_EV_STAGE_LEAVING` comes as the game sets off, before it knows the episode of a world's way in (its episode select is still to come: `SMS_EPISODE_SELECT`) or of the plaza, Hotel Delfino, Pinna Park and Casino Delfino, whose episode the game picks from the file. `SMS_EV_STAGE_LOADING` (1.5) comes once that is known, just before the stage loads: once a load, for every stage loaded (the title's too, area 15), before the movie some stages open with (the Airstrip's, a secret course's), so the movie is the stage's that loads. A changed `area`/`episode` loads instead, as if the game had chosen it: a world's episode is recorded as its episode select records it, a new visit's coins start afresh, and the movie is that stage's. An area and episode the game has no stage for are not taken, nor is the title (area 15), to or from. `from_area`/`from_episode` are where the game has Mario come from: the stage before, or after a world's episode select that world with `SMS_EPISODE_SELECT`. The plaza starts Mario where that world's way in is (in front of its painting, by its pipe, off the boat), and some stages read it for how he arrives; change them for him to come from elsewhere. A lost life reloads the stage with `SMS_EV_STAGE_LOADING` again (`reason` `SMS_LEAVE_LIFE_LOST`), with the stage that loaded: a mod that maps stages to others checks `reason`, or it maps them twice. In a multiplayer game the host's change is everyone's: the others load the host's stage (when it comes in time, instead of theirs; otherwise they go there after).

## The API

`sms_mod_load` receives `const SmsApi* api`. Its tables:

| Table | For |
| --- | --- |
| `api->core` | logging, events, services, the mod's folders, the game build, time |
| `api->game` | the game's state, save-file flags, warping, objects, stage names |
| `api->player` | Mario: reading and changing him |
| `api->camera` | the game camera, and projecting world points onto the canvas |
| `api->draw` | drawing over the game |
| `api->input` | controllers, keyboard, key bindings, taking the input for a menu |
| `api->config` | settings and the Mods menu's options |
| `api->audio` | the game's sound effects |
| `api->native` | the game's own objects, for mods built against the decomp's headers |
| `api->net` | (1.2) the multiplayer game: its players, and on the host the commands to their games ([Server mods](#server-mods)) |
| `api->world` | (1.4) the game's worlds, their areas and episodes, and its camera cutscenes ([world](#world)) |
| `api->files` | (1.5) the mod's own files beside the memory card, and per save file ([files](#files)) |

Every table starts with `size`, the size the game was built with. Tables only grow at their ends, so a mod checks a function from a later minor version is there with `SMS_API_HAS(api->draw, some_new_function)` before calling it.
Structs a mod passes in for the game to fill (`SmsGameState`, `SmsPlayer`, `SmsCamera`, `SmsPad`) start with a `size` the mod sets to `sizeof` the struct; the game fills only that much, so an old mod works with a newer game and the other way round.

### core

| Function | Does |
| --- | --- |
| `log(mod, level, fmt, ...)` | printf to the game's log as `[mod:<id>]`. `SMS_LOG_DEBUG` lines show only with `SMS_NATIVE_MODS_DEBUG=1`. The one function any thread may call. |
| `subscribe(mod, type, fn, user, priority)` | hear of an event; returns a handle (> 0) |
| `unsubscribe(mod, handle)` | stop; safe inside a handler |
| `event_id(name)` | the id of a custom event: every mod gets the same id for the same name |
| `emit(mod, type, data, size)` | send a custom event; returns 1 if a handler cancelled it. The game's own events cannot be sent. |
| `provide(mod, name, version, iface)` | offer an interface (usually a struct of function pointers) to other mods; the first mod to provide a name keeps it |
| `require(mod, name, min_version)` | another mod's interface at that version or later, or NULL |
| `mod_id(mod)`, `mod_dir(mod)`, `data_dir(mod)` | the mod's id, its install folder, and a folder of its own to write (made on first use) |
| `is_loaded(id)` | whether another mod is loaded |
| `game_build()`, `pointer_size()` | the game's build (`<commit>` or `<commit>-eclipse`) and 4 or 8 |
| `time()` | seconds since start, steady |
| `mod_info(mod, key)` | a value from a mod's `mod.ini` (its own keys too), or NULL (API 1.1) |
| `host_runtime(mod, runtime, start, stop, user)` | run the script mods of a language: `start(host, script, user)` for each `runtime = <name>` mod, `stop` as it unloads (API 1.1; see [Other languages](#other-languages)) |

### game

| Function | Does |
| --- | --- |
| `state(&s)` | `SmsGameState`: mode (`SMS_MODE_BOOT`, `TITLE`, `MENU`, `STAGE`, `MOVIE`, `OTHER`), area and episode (-1 without a stage), `stage_ready`, frame and tick counts, ticks per second, `eclipse`, the save file, `paused` (the pause menu, map or a save screen) |
| `flag_get(id)`, `flag_set(id, value)` | the save file's [flags](#flags) |
| `coins()`, `lives()`, `shines()`, `blue_coins()`, `has_shine(n)` | shorthands over the flags |
| `warp(area, episode)` | leave for a stage as a pipe would; 0 when no stage is being played |
| `find_object(name)` | an object by the name the stage gives it, or NULL |
| `object_name(obj)` | an object's name (often Japanese, Shift-JIS) |
| `object_position(obj, &pos)`, `object_set_position(obj, &pos)` | for objects with a place in the world; 0 for others |
| `area_name(area)` | the stage's display name in the game's language ("DELFINO PLAZA"), or "" |
| `object_count(lo, hi, kind)`, `object_info(lo, hi, kind, index, &info)` | (1.4) the stage's objects of actor types `lo` to `hi` and of a kind (`"WoodBlockCone"`; `"@7"` the copy `SMS_CMD_SPAWN` made with id 7; `""` any), picked as [server commands](#one-object) pick them: how many, and the `index`th one's actor type, kind, position, rotation, scale, whether it is in the stage (`alive`), hidden by a command, or a copy (its id). On a host, this is the copy of the stage its server mods judge by |
| `ground(&p, &g)` | (1.4) the ground at a point, as Mario finds it: the floor below (from a little above `p`), its collision type, whether it is water's surface, the water's surface over the point (`has_water`, `water_y`) and the roof above; 0 off the stage's map |
| `object_type(obj)` | (1.5) an object's actor type (`SMS_ACTOR_*` bits and its number), 0 when it has none |
| `object_health(obj, &max)` | (1.5) an enemy's health now, and its most (`max` may be NULL); -1 when it is not one with health |
| `object_set_color(obj, which, colour)` | (1.5) one of the colours of an object's model, in all its materials: `SMS_COLOR_MATERIAL` (its material colour, with alpha) or `SMS_COLOR_TEV0`..`TEV2` (TEV colour registers). What changes is what the model's materials make of that colour; a model's own colour animation sets it again as it plays. Every object of the same model shares them. 0 when it has no model |
| `set_hud_hidden(mod, mask)`, `hud_hidden()` | (1.5) hide parts of the game's own HUD in this game, single player included: `mask` is [`SMS_HUD_*`](#commands) bits ORed (`COINS`, `SHINES`, `BLUE_COINS`, `HEALTH`, `WATER`, `LIVES`, `RED_COINS`; 0 shows them again). The parts are left out of the HUD's drawing only: the HUD carries on underneath (its animations, sounds, FLUDD's messages), the counters' sparkles go with them, and a part shown again is as the game has it. Each mod's mask is its own and goes when the mod unloads; it lasts across stages. `hud_hidden` is the bits hidden now, whoever hides them (a mod, a server, or the player's own HUD settings where the game has them). Panels a mod draws are not affected |

Objects (`SmsObject`) come from events and `find_object`; they are good until the stage changes. Do not keep them across `SMS_EV_STAGE_LEAVING`.

### player

`get(&p)` fills an `SmsPlayer`: `valid` (0 outside stages), position, velocity, forward speed, yaw in degrees and the raw face angles, health and maximum, FLUDD's water and maximum, whether he has FLUDD and its second [nozzle](#nozzles), his [status](#mario-statuses) and how long he has had it, `on_ground`, `in_water`, `on_yoshi`, the ground below him, and the game's object for him; and (1.5, check `p.size`) the nozzle FLUDD has out (`current_nozzle`) and what he wears (`shirt`, `sunglasses`, `helmet`).

`set_position`, `set_velocity`, `set_forward_speed`, `set_health` (clamped to 0..max), `set_water` (clamped to 0..max) change him; `set_status(status, arg)` asks for a status as the game does (it may refuse). Each returns 0 when there is no Mario.

API 1.5 (`SMS_API_HAS(api->player, set_outfit)`):

- `set_nozzle(nozzle, animate)` switches FLUDD to a nozzle as the game does (it becomes the second nozzle; 0, the spray, leaves the second one as it is). Pass `animate` 1, as the game does: 0 changes the nozzle without putting the nozzle button's swap in step with it. `SMS_EV_PLAYER_NOZZLE` handlers hear of it as `SMS_NOZZLE_SWITCH` and may refuse it, the calling mod's too. Returns 1 when FLUDD has that nozzle out then; 0 with no FLUDD, on Yoshi, or a number that is not a nozzle (3, Yoshi's, comes only with him).
- `set_fludd(has)` gives Mario FLUDD or takes it away, as the stages without FLUDD do (no spray, no nozzle, FLUDD not drawn). The nozzles stay as they were for when he has it again.
- `set_outfit(shirt, sunglasses, helmet)` puts on (1) or takes off (0) the aloha shirt, the sunglasses and the diving helmet, each -1 to leave it as it is. The game still changes them its own way (its events; the helmet in the dive stages).
- (1.5, `SMS_API_HAS(api->player, set_yaw)`) `set_yaw(degrees)` turns Mario to face that way at once (0 faces +Z, 90 faces +X, as `SmsPlayer.yaw`), his model too, so FLUDD sprays that way: a first-person camera keeps him facing its aim while he sprays. The game turns him its own way again as he moves.
- (1.5) `set_aim(on, yaw, pitch)`: while `on`, his body and FLUDD face `yaw` whichever way he walks (the stick still moves him), and FLUDD tilts `pitch` degrees up (negative down) whatever his stance, so FLUDD sprays that way: spraying where a first-person view looks, walking sideways too. `set_aim(0, 0, 0)` gives him his own aim back; a mod that unloads drops its aim.
- (1.5) `set_hidden_parts(parts)`: parts of him left out while the rest is drawn, `SMS_PLAYER_PART_HEAD` (face, hair, cap) and `SMS_PLAYER_PART_FLUDD` (it still sprays), ORed; 0 draws all of him. A first-person view that shows his body looking down. Each mod's parts are its own and go when it unloads. `SMS_EV_HIDE_PLAYER_MODEL` still leaves all of him out.

### camera

`get(&c)` fills an `SmsCamera`: position, target, up, vertical field of view, aspect, near and far planes, and the view (3×4, world to camera) and projection (4×4) matrices it drew the last frame with.
`world_to_canvas(&p, &x, &y, &depth)` gives where a world point is on the [canvas](#drawing) and returns 1 when it is in front of the camera.

### world

API 1.4 (`SMS_API_HAS(api, world)`). The game's map of its stages, as its menus read it. An **area** is one stage (`SmsEvStage.area`, [the numbers](#areas)); a **world** is a place the plaza leads to, with its areas (Sirena Beach and Hotel Delfino), its secret courses and its eight **episodes**, each with its Shine. A world is named by the area the plaza leads into: Bianco Hills is 2 wherever in it you are.

| Function | Does |
| --- | --- |
| `area_kind(area)` | what an area is: `SMS_AREA_AIRSTRIP`, `PLAZA`, `WORLD` (a world's way in, with its episode select), `WORLD_PART` (another of its areas), `SECRET` (a secret course), `FINAL` (Corona Mountain), `TITLE` (area 15), `NONE` |
| `world_of(area)` | the world an area belongs to (2 for each of Bianco Hills' areas and secret courses; 1 for the plaza's; 0 the Airstrip; 52 Corona Mountain), or -1 |
| `episode_count(area)` | its world's episodes: 8 for the seven worlds, 0 elsewhere |
| `episode_shine(area, episode)` | the Shine (0–119) an episode is for, or -1 |
| `episode_name(area, episode, out, size)` | the episode's name as the game shows it, one line of ASCII; returns its length (0: none) |
| `next_episode(area)` | the episode the world's episode select offers first: the one after the newest whose Shine the file has (the first with none, the last once it is had) |
| `shine_episode(shine, &area)` | the episode a Shine is the story Shine of, and its world in `area` (may be NULL); -1 for 100-coin, blue coin and the plaza's Shines |
| `has_cutscene(name)` | the stage has a camera cutscene by that name (the names `SMS_EV_CUTSCENE` reports: `"camera_sirena_gate_in"`) |
| `play_cutscene(name, focus, frames)` | play one as the game plays its own (the player waits, the HUD hides), centred on `focus` (NULL: as it was made), `frames` long (-1: its own length); 1 when it is on its way, 0 outside play or while another cutscene, a talk or a menu is on |
| `cutscene_playing()` | a cutscene (the game's or a mod's) or a talk is on |

Super Mario Eclipse numbers its stages its own way: in its builds every area is `SMS_AREA_NONE` and has no episodes.

An episode of its own, straight after a Shine, past the plaza and the episode select ([Seamless Episodes](https://github.com/chasem-dev/sms-seamless-episodes) is this, with its settings):

```c
static int on_leaving(SmsMod* mod, SmsEvent* ev, void* user)
{
    SmsEvStage* to = (SmsEvStage*)ev->data;
    SmsGameState st = {sizeof st};
    if (ev->size < sizeof *to || to->reason != SMS_LEAVE_SHINE || to->area != 1)
        return SMS_CONTINUE; /* not a Shine's way back to the plaza */
    api->game->state(&st);
    int world = api->world->world_of(st.area);
    if (api->world->episode_count(world) > 0) {
        to->area = world;
        to->episode = api->world->next_episode(world); /* its opening plays as ever */
    }
    return SMS_CONTINUE;
}
```

### files

API 1.5 (`SMS_API_HAS(api, files)`): a mod's own files, kept beside the memory card in `<card folder>-mods/<mod id>/` (`card-a-mods/<id>/` by default), so they go with the player's card wherever it is kept (the launcher's save backups include them). A multiplayer guest's copy of the host's card has a folder of its own.

| Function | |
| --- | --- |
| `dir(mod)` | the mod's folder there (made on first use) |
| `read(mod, scope, name, buf, cap)` | the file's size, or -1 when there is none; up to `cap` bytes into `buf` (`buf` NULL asks the size) |
| `write(mod, scope, name, data, size)` | 1 written, or kept for the save; 0 not (a bad name or scope, too big) |
| `remove(mod, scope, name)` | 1 when it was there |

`scope`:

- `SMS_FILES_MOD`: the mod's own files, written at once (the settings, a mod's records across files).
- `0`, `1`, `2`: save file A, B or C of the game being played (Super Mario Sunshine's and Super Mario Eclipse's kept apart, in `<id>/<game code>-<A|B|C>/`), kept as the card keeps the file:
  - What the mod writes to the file being played is kept and written when the game saves that file (the pause menu, the save prompt, auto save). A game quit without saving leaves its files as they were, as the card is. Reading the file being played gives what the mod wrote since.
  - A new game in a file starts with none (its old files go at its first save).
  - Copying or erasing a file at file select copies or erases every mod's files of it.
  - Writing to a file that is not the one being played writes it at once.
- `SMS_FILES_CURRENT`: the file being played (from its load or new game at file select; none before).

Names are letters, digits, `.`, `_` and `-`, 1 to 64 characters, not starting with `.`; a file is at most 1 MiB. `SMS_EV_SAVE_FILE` tells when a file is loaded (`SMS_SAVE_FILE_LOADED`: read its files there), started anew (`SMS_SAVE_FILE_NEW`: write its first ones), saved, copied or erased.

```c
static int onSaveFile(SmsMod* mod, SmsEvent* ev, void* user)
{
	const SmsEvSaveFile* f = (const SmsEvSaveFile*)ev->data;
	if (f->what == SMS_SAVE_FILE_NEW) {
		uint32_t seed = new_seed();
		api->files->write(mod, SMS_FILES_CURRENT, "seed.bin", &seed, sizeof seed); /* saved with the file */
	} else if (f->what == SMS_SAVE_FILE_LOADED) {
		if (api->files->read(mod, SMS_FILES_CURRENT, "seed.bin", &g_seed, sizeof g_seed) != sizeof g_seed)
			g_seed = new_seed(); /* a file from before the mod */
	}
	return 0;
}
```

`core->data_dir` is still the place for what is not the player's progress (caches, logs); `config` keeps settings there.

### draw, input, config, audio

See [Drawing](#drawing), [Input](#input), [Settings](#settings-and-the-mods-menu).
`audio->play_system_sound(id)` plays one of the game's system sound effects (its `MSD_SE_SY_*` numbers in the decomp's `MSound` headers).

### native

For mods that go beyond the API: `native->get("gpMarioOriginal")` (also `gpMarDirector`, `gpApplication`, `gpCamera`, `gpItemManager`, `gpMap`, `TFlagManager`) returns the game's own object.
Reading it needs the decomp's headers and the same compiler, pointer size and layout as the game, which change between builds: check `native->build()` (`<commit>/<platform>`) against the one you built for, and refuse to load otherwise.
Prefer asking for the event or function you need (see [Adding to the API](#adding-to-the-api)).

## Drawing

Draw only inside `SMS_EV_DRAW`. Calls anywhere else are ignored (and logged once).

**The canvas** is the game's picture: 480 units high and as wide as the picture's aspect (640 at 4:3, about 853 with the port's 16:9 widescreen), from its top left. `draw->canvas(&w, &h)` and the `SMS_EV_DRAW` payload give its size.
Whatever the window's size, a mod draws in canvas units, and the game draws it at the window's full resolution over the game's picture and under the port's own menus (the debug overlay, the cheat and warp menus, the quit prompt), with the Mods menu over every mod.
Mods draw in the order of their `SMS_EV_DRAW` handlers: a later one draws over an earlier one.
The drawing appears in the window (and `SMS_WINDOW_SHOTS` captures), not in `SMS_SHOTS` (the game's picture alone).

| Function | Draws |
| --- | --- |
| `rect`, `rect_outline`, `rect_gradient` (top to bottom) | rectangles |
| `line(x0, y0, x1, y1, thickness, c)`, `triangle`, `circle`, `circle_outline` | shapes |
| `text(x, y, size, c, flags, s)`, `text_width(size, s)` | text, `size` the line's height; flags `SMS_TEXT_SHADOW`, `SMS_TEXT_OUTLINE`, `SMS_TEXT_CENTER`, `SMS_TEXT_RIGHT`; `\n` breaks lines. ASCII only (other characters show as `?`). |
| `image_load(mod, "file.png")`, `image_create(mod, rgba, w, h)` | an image (PNG, JPEG, TGA or BMP from the mod's folder, or RGBA8 pixels, row 0 at the top); freed with `image_free` or when the mod unloads |
| `image(img, x, y, w, h, tint)`, `image_region(img, x, y, w, h, u0, v0, u1, v1, tint)`, `image_size` | images, smoothly scaled (mipmapped) |
| `clip_push(x, y, w, h)`, `clip_pop()` | draw only inside a rectangle; nest freely |
| `world_line(&a, &b, thickness, c)`, `world_box(&centre, &half, thickness, c)`, `world_ring(&centre, radius, thickness, c)` | lines in the world through the game camera, cut at the camera's near plane; over the scene, not hidden behind walls |
| `world_text(&p, size, scaled, c, flags, s)` | text standing on a world point, smaller with distance when `scaled` |
| `hud_slot(mod, anchor, w, h, &rect)` | a place for a w × h panel this frame (below) |

Colours are `0xRRGGBBAA`: `SMS_RGB(r, g, b)` and `SMS_RGBA(r, g, b, a)`.

**HUD slots.** The canvas has nine anchors, `SMS_ANCHOR_TOP_LEFT` to `SMS_ANCHOR_BOTTOM_RIGHT`.
Each frame, `hud_slot` hands out rectangles at an anchor one below the other (above the other at the bottom anchors) in the order the mods ask, 6 units apart, and returns 0 when the next one would not fit: past the canvas's edge, or into the panels another anchor on the same side gave out (a side's top and bottom stacks never meet).
The stacks start clear of the game's HUD: the left column below the Shine and coin counters, the top middle below the lives, the right column below the health meter, and the bottom ones above the water gauge and the talk boxes.
Panels that ask every frame and are hidden by skipping the call let the others move into their place.

**Stepping aside.** The mods' drawing is over everything the game draws, its pause menu and map included. Skip HUD panels while `game->state` says `paused` (the pause menu, map or a save screen) and outside `SMS_MODE_STAGE`, as the examples do.

```
TOP_LEFT       TOP        TOP_RIGHT
LEFT          CENTER          RIGHT
BOTTOM_LEFT   BOTTOM   BOTTOM_RIGHT
```

## Input

| Function | Does |
| --- | --- |
| `pad(port, &p)` | controller `port` (0–3) as read this frame: held buttons, buttons pressed and released this frame, sticks (-1..1, up is +y), triggers (0..1). Before any mod's `SMS_EV_PAD_READ` change and before a capture blanks it. |
| `key_down(sc)`, `key_pressed(sc)` | a key is down; went down this frame |
| `key_from_name("F3")`, `key_name(sc)` | SDL key names ("F3", "Space", "Left Shift", "A") and scancodes |
| `bind_key(mod, action, default)` | the key for an action: the player's `key.<action>` setting or the default, with clash warnings |
| `capture(mod, on)`, `captured_by()` | take the controllers and keyboard for a menu; who has them |

Buttons are the GameCube's: `SMS_PAD_A`, `B`, `X`, `Y`, `Z`, `L`, `R`, `START`, `UP`, `DOWN`, `LEFT`, `RIGHT`.
Keys arrive in `SMS_EV_KEY` as SDL scancodes, after the port's own keys and menus have taken theirs and before the game's controls.

## Settings and the Mods menu

Each mod's settings are `key = value` lines in `<data folder>/<id>/settings.ini` (on Linux `~/.local/share/sms-port/mods/<id>/settings.ini`; `%APPDATA%\sms-port\mods` on Windows), read on first use and written within a couple of seconds of a change, at `config->save`, and at exit.
`config->get_int / get_float / get_string(mod, key, default)` read them and `set_*` change them.

`config->add_option(mod, &option)` puts a setting in the **Mods menu** (F1):

| `type` | Shows | Changes with |
| --- | --- | --- |
| `SMS_OPTION_BOOL` | ON / OFF | Enter, Left, Right |
| `SMS_OPTION_INT`, `SMS_OPTION_FLOAT` | the number | Left / Right by `step`, within `min`..`max` |
| `SMS_OPTION_CHOICE` | one of `choices` (the setting is its index) | Left / Right |
| `SMS_OPTION_ACTION` | a button | Enter calls `on_change` |

The value is kept under `key`; `def` is shown until the player changes it; `on_change(mod, key, user)` is called after each change.
The menu copies the option, so its strings need not outlive the call.

The menu also lists every mod found, with its version, description, or why it did not load, and a switch: switching a mod off unloads it at once and remembers it (in `<data folder>/native-mods.ini`) until it is switched on again.
It is opened and closed with F1 (`SMS_NATIVE_MODS_MENU_KEY`), and Escape closes it; it takes the keyboard and the first controller while open (arrows or D-pad or stick, Enter or A, Escape or B).

## Talking to other mods

**Custom events**, for news: the sender and every listener get one id from `core->event_id("stage-timer.split")`, and `core->emit` sends a payload to the listeners.
Neither needs the other to be there; `stage-timer` sends a split at each Shine and `trainer-hud` shows it as a pop-up when both are loaded.
Put a struct in the payload and check `ev->size` before reading it.

**Services**, for asking: a mod offers a struct of functions with `core->provide(mod, "radar.api", 1, &iface)`, and another fetches it with `core->require(mod, "radar.api", 1)`.
List the provider in your `mod.ini`'s `after` (or `requires`, if you cannot work without it) so it is loaded first.
Bump the version when the struct changes incompatibly, and only add fields at its end otherwise.

## Lua scripts

Mods can be written in Lua 5.4 instead of a compiled language.
The **lua** mod, a native mod that embeds Lua, runs every folder in `mods/` whose `mod.ini` says `runtime = lua`.
It is optional, and not part of the game: install it once from this SDK's [releases](https://github.com/chasem-dev/sms-mod-sdk/releases) (one zip, with its library for every platform; unzip it into `mods/`), or with the port's `python3 tools/mods/get.py lua`. Its source is [`lua/`](../lua).
Without it, script mods are listed in the Mods menu as needing it.
A script mod is a mod like any other: it has its own id, settings, options in the Mods menu, keys, HUD slots, place in the load order and `[mod:<id>]` log lines, and it can be switched off, reloaded and ordered with `after` and `requires`.
It needs no compiler and no library per platform: the same folder runs on every computer the lua mod is installed on.

[`templates/lua`](https://github.com/chasem-dev/sms-mod-templates/tree/main/templates/lua) is a script mod to start from, and [`examples/lua-challenges`](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/lua-challenges) a complete one: a challenge against the clock each time a stage loads, with a panel, world markers, sounds, settings, a key, a module of its own, and the C `stage-timer` mod's events read from Lua.

### A script mod

```
mods/
  lua/                         the lua mod, installed once
    mod.ini
    bin/linux-x86/lua_host.so
  my-script/
    mod.ini
    main.lua
    helpers.lua                require("helpers")
```

```ini
# mods/my-script/mod.ini
id = my-script
name = My script
version = 1.0.0
runtime = lua
script = main.lua
```

```lua
-- mods/my-script/main.lua: runs once when the mod loads
local coins = 0
sms.option{ key = "show", label = "Show the counter", type = "bool", default = true }

sms.on("stage_loaded", function(ev)
  coins = 0
  sms.log("now in", sms.area_name(ev.area), "episode", ev.episode + 1)
end)

sms.on("item_collected", function(ev)
  if ev.kind == "coin" then coins = coins + 1 end
end)

sms.on("draw", function(ev)
  if ev.mode ~= "stage" or sms.state().paused or not sms.setting("show", true) then return end
  local x, y, w, h = sms.draw.hud_slot("bottom_left", 150, 22)
  if not x then return end
  sms.draw.rect(x, y, w, h, sms.rgba(0, 0, 0, 160))
  sms.draw.text(x + 8, y + 5, 13, sms.rgb(255, 230, 120), "Coins here: " .. coins, sms.SHADOW)
end)
```

The script's top level runs once as the mod loads; register handlers and options there.
An error at the top level refuses the mod (the log has the error and its traceback); an error in a handler is logged (the first five, then a note) and the game goes on.
With `SMS_NATIVE_MODS_HOT=1`, saving the script loads it again in place: a new Lua state, its settings kept.

Each script has a Lua state of its own; scripts share nothing but what they say through events (below).
They have Lua's whole standard library, `io` and `os` included, so a script mod is as trusted as a native one; `require` finds modules in the script's folder only, and only Lua source (no compiled chunks, no C modules) is loaded.

### Events in Lua

`sms.on(name, function(ev) ... end [, priority])` subscribes and returns a handle for `sms.off(handle)`.
The names are the C events' without `SMS_EV_`, in lower case: `frame`, `tick`, `draw`, `stage_loaded`, `stage_exit_request`, `stage_leaving`, `life_lost`, `game_over`, `coins_reset`, `file_selected`, `item_collected`, `pad_read`, `key`, `cutscene`, `goop_cleaned`, `object_hit`, `save_state_request`, `hide_player_model`, `player_status`, `player_damage`, `shine_collected`, `camera`, `object_move`, `mod_loaded`, `shutdown`, and for multiplayer `net_player_joined`, `net_player_left`, `net_player_arrived`, `net_session` (`{slot, name, character}`; the session's `{area, episode, session}`), (1.4) `stage_start`, and (1.5) `player_nozzle` (`{from, to, animate, reason, box}`, `reason` `"switch"`, `"swap"`, `"box"` or `"stage_start"`; set `ev.to` to change it), and (1.5) `player_spawn`, `player_status_changed`, `player_jump`, `player_land`, `player_hit`, `player_heal`, `player_death`, `player_mount`, `player_water`, `yoshi_hatch`, `yoshi_eat`, `actor_spawn`, `actor_damage`, `actor_death`, `shine_appear`, `shine_get`, `map_event`, `flag_changed`, `sound`, `music`, `talk`, `cutscene_end`, `pause`, `mode_changed`, `save_prompt`, `game_saved`, `timer`, `script_call` (fields below), (1.5) `stage_loading` (`{area, episode, reason, shine, from_area, from_episode}`), and (1.5) `save_file`, `nozzle_fire`, `shine_setup`, `yoshi_egg`, `episode_shine`, and (1.5) `file_starting`, and (1.6) `file_menu` (fields below). Stage events (`stage_loaded`, `stage_leaving`, `stage_start`, `life_lost`, `file_selected`) carry `{area, episode, reason, shine}`, `reason` a name: `"shine"`, `"exit_area"`, `"life_lost"`, `"game_over"`, `"way_out"`, `"warp"`, `"file"` or `"other"`.
Any other name is a custom event (`"stage-timer.split"`).
Priorities: `sms.FIRST`, `sms.EARLY`, `sms.NORMAL` (the default), `sms.LATE`, `sms.MONITOR`.

The handler gets a table: `ev.type` (the name), `ev.cancelled` and `ev.cancelled_by`, and the payload's fields.
**Return `true` to cancel** a cancellable event or consume a consumable one (the [same rules](#living-together) as in C).
**Change the fields** of a changeable event's table and the game takes them when the handler returns.

| Event | Fields (writable in bold) |
| --- | --- |
| `frame` | `frame`, `mode` |
| `tick` | `tick`, `dt`, `ticks_per_second` |
| `draw` | `w`, `h` (the canvas), `frame`, `mode` |
| `stage_loaded` | `area`, `episode` |
| `stage_leaving`, `life_lost`, `file_selected` | **`area`**, **`episode`** |
| `stage_loading` | **`area`**, **`episode`**, `reason`, `shine`, **`from_area`**, **`from_episode`** |
| `stage_exit_request` | `code`, `area`, `episode`, `actor` |
| `item_collected` | `item`, `taker`, `kind` (`"coin"`, `"red_coin"`, `"blue_coin"`, `"other"`), `actor_type`, `position` |
| `pad_read` | **`pads`**: four tables of **`buttons`**, **`stick_x`**, **`stick_y`**, **`cstick_x`**, **`cstick_y`** (-128..127), **`trigger_l`**, **`trigger_r`**, **`analog_a`**, **`analog_b`** (0..255), `connected` |
| `key` | `scancode`, `key` (its name), `down`, `repeated`, `shift`, `ctrl`, `alt` |
| `cutscene` | `name`, `position` (when it has one), `frames`, `actor` |
| `goop_cleaned` | `position`, `size` |
| `object_hit` | `object`, `sender`, `message` |
| `save_state_request` | **`reason`** (shown when you cancel) |
| `hide_player_model` | `object` |
| `player_status` | `from`, **`to`**, **`arg`**, `forced` |
| `player_damage` | **`amount`**, `underwater` |
| `shine_collected` | `shine`, `total` |
| `camera` | **`position`**, **`target`**, **`fovy`**, `aspect` |
| `object_move` | `object`, `after` |
| `mod_loaded` | `id`, `name`, `version` |
| `player_spawn` | **`position`**, **`yaw`**, **`has_position`** (a new `position` sets it), **`kind`** (`"wait"`, `"roll"`, `"return"`, `"coaster"`; set `"wait"`, `"roll"` or `"return"`) |
| `player_status_changed` | `from`, `to`, `arg` |
| `player_jump` | `status`, `from`, `position`, `velocity` |
| `player_land` | `status`, `from`, `fall`, `speed`, `position` |
| `player_hit` | `object`, `sender`, `message` |
| `player_damage` (1.5), `player_heal` | **`amount`**, `underwater`, `source` |
| `player_death` | `cause` (`"health"`, `"drown"`, `"pit"`, `"goop"`, `"electric"`, `"crash"`, `"other"`), `position`, `source` |
| `player_mount` | `mount` (`"yoshi"`, `"blooper"`), `on`, `object`, `color` |
| `player_water` | `entered`, `surface_y`, `position` |
| `yoshi_hatch`, `yoshi_eat` | `fruit`, `color`, `position` |
| `actor_spawn`, `actor_death` | `object`, `actor_type`, `position`, `other`, `forced` |
| `actor_damage` | `object`, `attacker`, `actor_type`, `message`, `health_before`, `health`, `max_health`, `position` |
| `shine_appear`, `shine_get` | `object`, `shine`, `had`, `position` |
| `map_event` | `object` |
| `flag_changed` | `id`, `old`, **`value`** |
| `sound` | **`id`**, `system`, `position` (when it has one) |
| `music` | **`id`** (when starting), `start` |
| `talk` | `what` (`"start"`, `"message"`, `"end"`), `npc`, and for a message **`message`**, `last` |
| `cutscene_end` | `name`, `actor`, `forced` |
| `pause` | `paused`, `kind` (`"menu"`, `"map"`, `"save"`) |
| `mode_changed` | `from`, `to` (as `mode`) |
| `save_prompt` | `kind` (`"shine"`, `"blue_coin"`, `"nozzle"`, `"yoshi"`) |
| `game_saved` | `file` |
| `save_file` | `what` (`"loaded"`, `"new"`, `"saved"`, `"copied"`, `"erased"`), `file`, `from` (a copy's) |
| `nozzle_fire` | `nozzle`, `how` (`"burst"`, `"pressure"`); return `true` to stop it |
| `shine_setup` | `object`, `shine`, `collected`, **`model`** (a `.bmd` file: relative to the script mod's folder, or a full path; `false` the game's) |
| `yoshi_egg` | `kind` (`"stage"`, `"plaza_event"`, `"event_egg"`), **`present`**, `object` |
| `episode_shine` | `area`, `episode`, **`shine`** |
| `file_starting` | `file`, `is_new`; return `true` to keep the file select waiting this frame |
| `file_menu` | (1.6) `file`, `is_empty`, `open`, `item` (`"start"`, `"copy"`, `"erase"`, `"score"`; `nil` closed without a choice) |
| `timer` | `what` (`"started"`, `"stopped"`, `"out"`), `time` |
| `script_call` | `name`, `owner`, `argc` |
| a custom event | `data`: the payload's bytes as a string |

`mode` is `"boot"`, `"title"`, `"menu"`, `"stage"`, `"movie"` or `"other"`.
Positions are tables `{x =, y =, z =}`; functions that take one also take `{x, y, z}`.
Objects are light userdata, for `sms.object_name`, `sms.object_position` and `sms.set_object_position`.

### The sms table

| Function | Does |
| --- | --- |
| `sms.on(name, fn [, priority])`, `sms.off(handle)` | subscribe; unsubscribe |
| `sms.log(...)`, `sms.warn(...)`, `sms.error(...)`, `sms.debug(...)` | the game's log, as `[mod:<id>]`; `print` is `sms.log` |
| `sms.event_id(name)`, `sms.emit(name [, data])` | a custom event's id; send one, with a string of bytes as its payload (returns whether a handler cancelled it) |
| `sms.time()` | seconds since start |
| `sms.is_loaded(id)` | whether another mod is loaded |
| `sms.data_dir()`, `sms.info(key)` | a folder the script may write; a value from its `mod.ini` |
| `sms.id`, `sms.dir`, `sms.name`, `sms.version`, `sms.game_build`, `sms.lua_version` | about the script and the game |
| `sms.unload` | set it to a function, and it is called as the script stops |
| `sms.state()` | `{mode, stage_ready, area, episode, frame, tick, ticks_per_second, eclipse, save_file, paused}` |
| `sms.flag(id)`, `sms.set_flag(id, value)` | the save file's [flags](#flags) |
| `sms.coins()`, `sms.lives()`, `sms.shines()`, `sms.blue_coins()`, `sms.has_shine(n)` | shorthands |
| `sms.warp(area [, episode])` | leave for a stage |
| `sms.area_name(area)` | the stage's name |
| `sms.find_object(name)`, `sms.object_name(o)`, `sms.object_position(o)`, `sms.set_object_position(o, pos)` | objects |
| `sms.mario()` | `nil` outside stages, or `{position, velocity, forward_speed, yaw, health, max_health, water, max_water, has_fludd, nozzle, status, status_timer, on_ground, in_water, on_yoshi, floor, object}`, and (1.5) `current_nozzle, shirt, sunglasses, helmet` |
| `sms.set_mario{position =, velocity =, forward_speed =, health =, water =}` | change what is given |
| `sms.set_status(status [, arg])` | ask for a [status](#mario-statuses) |
| `sms.set_yaw(degrees)`, `sms.set_aim(degrees)`, `sms.set_hidden_parts{head =, fludd =}` | (1.5) Mario faces that way at once; `sms.set_aim(yaw, pitch)`: his body and FLUDD aim that way whichever way he walks (`sms.set_aim(nil)` gives his aim back); each returns whether it was done |
| `sms.set_nozzle(nozzle [, animate])`, `sms.set_fludd(has)`, `sms.set_outfit{shirt =, sunglasses =, helmet =}` | (1.5) FLUDD's [nozzle](#nozzles) (`animate` true unless given), FLUDD or not, what he wears (a piece not given stays as it is; `sms.set_outfit(shirt, sunglasses, helmet)` too, `nil` for as it is); each returns whether it was done |
| `sms.camera()`, `sms.to_canvas(pos)` | `{position, target, up, fovy, aspect, near, far}`; a world point's `x, y, depth` on the canvas, or nothing behind the camera |
| `sms.play_sound(id)` | a system sound: `sms.SOUND_COIN`, `SOUND_NEWS`, `SOUND_SHINE`, `SOUND_DAMAGE`, `SOUND_HEAL`, `SOUND_APPEAR`, `SOUND_CURSOR`, `SOUND_DECIDE`, `SOUND_CANCEL`, or any `MSD_SE_SY_*` number |
| `sms.pad([port])` | `{connected, held, pressed, released, stick_x, stick_y, cstick_x, cstick_y, trigger_l, trigger_r}`; test buttons with `pad.pressed & sms.PAD_A ~= 0` (`PAD_A`, `B`, `X`, `Y`, `Z`, `L`, `R`, `START`, `UP`, `DOWN`, `LEFT`, `RIGHT`) |
| `sms.key_down(key)`, `sms.key_pressed(key)` | a key (a name such as `"F3"`, or a scancode) is down; went down this frame |
| `sms.key(name)`, `sms.key_name(scancode)` | names and scancodes |
| `sms.bind_key(action, default)` | the scancode for an action (the player's `key.<action>` setting or the default), with clash warnings |
| `sms.capture([on])`, `sms.captured_by()` | take the controllers and keyboard for a menu |
| `sms.setting(key, default)` | a setting, read as the default's type (boolean, integer, number or string) |
| `sms.set_setting(key, value)`, `sms.save_settings()` | change one; write them now |
| `sms.option{key, label, type, default, min, max, step, choices, on_change}` | a setting in the Mods menu; `type` is `"bool"`, `"int"`, `"float"`, `"choice"` (with `choices = {"a", "b"}`) or `"action"`; `on_change(key)` after each change |
| `sms.rgb(r, g, b)`, `sms.rgba(r, g, b, a)` | colours (or write `0xRRGGBBAA`) |
| `sms.object_count(from, to, kind)`, `sms.object_info(from, to, kind, index)`, `sms.ground(pos)` | (1.4) the stage's objects (a table: `type`, `kind`, `position`, `rotation`, `scale`, `alive`, `hidden`, `spawned`), and the ground at a point (`floor`, `floor_type`, `water`, `water_y`, `roof`) |
| `sms.object_type(o)`, `sms.object_health(o)` | (1.5) an object's actor type; an enemy's health and its most (`local hp, max = sms.object_health(o)`; nothing when it has none) |
| `sms.set_object_color(o, which, colour)` | (1.5) one of an object's model's colours: `which` `"material"`, `"tev0"`, `"tev1"` or `"tev2"` (see `game->object_set_color`) |
| `sms.files_read(name [, file])`, `sms.files_write(name, data [, file])`, `sms.files_remove(name [, file])`, `sms.files_dir()` | (1.5) the mod's [files](#files): `file` nil the mod's own, `0`..`2` a save file, `"current"` the one played. `files_read` gives the bytes as a string (or nil); `files_write` whether it was written (or kept for the save) |
| `sms.set_hud_hidden{ coins = true, health = true, ... }`, `sms.hud_hidden()` | (1.5) hide parts of the game's own HUD (`coins`, `shines`, `blue_coins`, `health`, `water`, `lives`, `red_coins`, or `all = true`; an empty table shows them again); returns whether the game can. `sms.hud_hidden()` is a table of what is hidden now |
| `sms.world.kind(area)`, `sms.world.of(area)` | (1.4) what an area is (`"airstrip"`, `"plaza"`, `"world"`, `"world_part"`, `"secret"`, `"final"`, `"title"`, `"none"`); its world, or `nil` ([world](#world)) |
| `sms.world.episodes(area)`, `sms.world.episode_shine(area, ep)`, `sms.world.episode_name(area, ep)`, `sms.world.next_episode(area)`, `sms.world.shine_episode(shine)` | its world's episodes: how many; an episode's Shine and name (`nil`: none); the one the episode select offers; a Shine's episode and world (`nil`: not an episode's) |
| `sms.world.has_cutscene(name)`, `sms.world.play_cutscene(name [, focus [, frames]])`, `sms.world.cutscene_playing()` | the stage's camera cutscenes |
| `sms.cube_count(set)`, `sms.in_cube(set, index, pos)` | the stage's trigger cubes: how many a set has; whether a point is in one (`nil`: no such cube). Sets: `"camera"`, `"area"`, `"fast_a"`, `"fast_b"`, `"fast_c"`, `"mirror"`, `"wire"`, `"stream"`, `"shadow"`, `"sound_change"`, `"sound_effect"` |

`sms.net` is the multiplayer game ([Server mods](#server-mods)):

| Function | |
| --- | --- |
| `sms.net.role()`, `sms.net.dedicated()`, `sms.net.slot()`, `sms.net.session()` | `"off"`, `"host"` or `"client"`; the host is a server; this game's slot; the stage session |
| `sms.net.players()` | the slots in the game, a list |
| `sms.net.player(slot)` | `nil`, or `{slot, server, character, name, color, in_stage, position, yaw, visible, surfing, yoshi, spectating, paused, cutscene, ping, game_version, api_version, pad}` (`pad`, 1.4: `{buttons, stick_x, stick_y, cstick_x, cstick_y, trigger_l, trigger_r}`) |
| `sms.net.set_look(slot, look)`, `sms.net.look(slot)` | (1.4) host: a player's colours forced (`nil`: their own again); anyone's as they are now |
| `sms.net.set_respawn(slot, how, pos, yaw)` | (1.4) host: where a player comes back as they arrive: `"warp"`, `"blooper"` or `"none"` |
| `sms.net.send(to, cmd)` | host: a [command](#commands) to `"all"`, a slot or a list of slots, as `{type = "banner", text = ..., color = ..., delay = seconds, sticky = true}`; returns the slots it went to as bits |
| `sms.net.load_stage(area [, episode])` | host: everyone to that stage afresh |
| `sms.net.set_mode(name)`, `sms.net.set_status(text)` | host: what the launcher's list of games shows |

Constants for commands: `sms.MUSIC_EVENT`, `sms.MUSIC_FANFARE_RACE` (and the `SOUND_*` above).

`sms.draw` mirrors the [C drawing](#drawing), in canvas units, only inside `draw` handlers. Colours default to white.

| Function | Draws |
| --- | --- |
| `canvas()` | returns `w, h` |
| `rect(x, y, w, h, c)`, `rect_outline(x, y, w, h, thickness, c)`, `gradient(x, y, w, h, top, bottom)` | rectangles |
| `line(x0, y0, x1, y1, thickness, c)`, `triangle(x0, y0, x1, y1, x2, y2, c)`, `circle(x, y, r, c)`, `circle_outline(x, y, r, thickness, c)` | shapes |
| `text(x, y, size, c, text [, flags])`, `text_width(size, text)` | text; returns its width. Flags: `sms.SHADOW`, `sms.OUTLINE`, `sms.CENTER`, `sms.RIGHT` (add them) |
| `image_load(path)`, `image_create(w, h, rgba)`, `image_free(img)`, `image_size(img)` | images: a file in the mod's folder, or a string of `w * h * 4` RGBA bytes |
| `image(img, x, y, w, h [, tint])`, `image_region(img, x, y, w, h, u0, v0, u1, v1 [, tint])` | draw one |
| `clip_push(x, y, w, h)`, `clip_pop()` | clipping |
| `world_line(a, b [, thickness, c])`, `world_box(centre, half [, thickness, c])`, `world_ring(centre, r [, thickness, c])` | the world |
| `world_text(pos, size, c, text [, flags [, scaled]])` | text on a world point |
| `hud_slot(anchor, w, h)` | `x, y, w, h` of a panel's place, or nothing when it does not fit; anchors `"top_left"`, `"top"`, `"top_right"`, `"left"`, `"center"`, `"right"`, `"bottom_left"`, `"bottom"`, `"bottom_right"` |

### Lua and C mods together

Lua and C mods hear the same events in one order and share HUD slots, keys and the Mods menu.
They talk through custom events: a payload is bytes, so pack C structs with `string.pack` and read them with `string.unpack`.
The C `stage-timer` sends `struct { float seconds; int shine; }` as `"stage-timer.split"`, and `lua-challenges` reads it:

```lua
sms.on("stage-timer.split", function(ev)
  local seconds, shine = string.unpack("<fi4", ev.data)
end)
```

and a script can send one a C mod reads: `sms.emit("my-script.score", string.pack("<i4f", 12, 3.5))`.

### Other languages

The lua mod is an ordinary native mod using two functions any mod can: `core->host_runtime(mod, "lua", start, stop, user)` makes it the runner of `runtime = lua` mods, and `core->mod_info(script, "script")` names a script's file.
A mod embedding another language (Python, Wren, JavaScript) does the same with its own runtime name; the game gives each script mod its own `SmsMod`, so whatever the script calls through the API is its own.

## Server mods

A **server mod** runs on the host of a multiplayer game only, a dedicated server as a rule, and players join with the plain game: nothing to install. It is how a game mode is made: the [Squid Race](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/squid-race) (C) and [Red Light, Green Light](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/lua-red-light) (Lua) are two.

It works as a Minecraft server's plugins do. The server decides **when** and **where**: the rules, the clock, who is where, who won. Every player's game decides **how it looks**: it draws the banner, runs the clock, puts its player on the start line and onto a Blooper, with the game's own models, sounds and HUD. What passes between them is a small, fixed set of **commands** that every build of the game carries out, so a server mod can make any number of game modes from them without the players needing anything but the game.

### How it works

```
  the host: a dedicated server, or a player hosting           each player: the plain game
  +-----------------------------------------+   commands  +------------------------------+
  | the stage, loaded (its cubes, its        | ---------> | carries them out: the banner, |
  |   actors) and every player's updates     |  (net->     |   the clock, warp, hold, onto |
  | server mods (side = server):             |   send)     |   a Blooper, watch a player,  |
  |   the rules, the clock, who is where     | <--------- |   the stage's actors...       |
  |   api->net, game->in_cube                |  where they | and draws it all itself       |
  +-----------------------------------------+   are, joins +------------------------------+
```

- The host has the stage loaded as everyone does, so a server mod asks its own copy of the stage (its trigger cubes, `game->in_cube`) about the players, whose positions come from their games' updates, 30 or so a second (`net->player`).
- The players' games send nothing a server mod wrote: no code, no models, no drawing. Everything they do for it is one of the commands below, all in the game already. A mod's own drawing (`api->draw`) is for the game it runs in only.
- Players need the same game build as the server, as multiplayer already requires; a game with API 1.2 or later carries out commands. They need no mods.

### A server mod

`side = server` in its `mod.ini` makes a mod a server mod: it loads only in a game hosting multiplayer, and anywhere else the Mods menu lists it, off, saying so. A server mod may be native or Lua (the Lua host must be installed on the server). A player who hosts runs the server mods too, and plays along: commands to slot 0 are carried out in the host's own game.

A **dedicated server** is a host that is not a player (`SMS_MULTIPLAYER_DEDICATED=1`): its own Mario is out of play and unseen, it is not counted among the players, and the first player to join plays Mario. It needs no file: from the title it goes straight to the stage a server mod asks for with `net->load_stage` (or `SMS_MULTIPLAYER_STAGE=area,episode`, else Delfino Plaza):

```sh
SMS_HEADLESS=1 SMS_AUDIO=0 SMS_TEXTURE_PACKS=0 \
SMS_MULTIPLAYER=host SMS_MULTIPLAYER_DEDICATED=1 \
SMS_NATIVE_MODS_DIR=/path/to/server-mods \
./sms GMSE01.iso
```

It needs about 600 MB of memory, and players reach it on UDP 24002 (`SMS_MULTIPLAYER_PORT`), as any hosted game.

Mods load once multiplayer has started, so `net->role()` is right while they load; still, do the server's work in events (`SMS_EV_FRAME`, the `SMS_EV_NET_*` events) rather than in `sms_mod_load`.

### api->net

| Function | |
| --- | --- |
| `role()` | `SMS_NET_OFF`, `SMS_NET_HOST` or `SMS_NET_CLIENT` |
| `dedicated()` | the host is a server, not a player |
| `local_slot()` | this game's player (0 is the host); -1 before a client has a slot |
| `players()` | bit n set: slot n is in the game (a server's slot 0 too) |
| `player(slot, &p)` | 1 and `SmsNetPlayer p` filled when that slot is in the game (set `p.size` first) |
| `session()` | the stage session being played (0: none). Each time the host starts a stage, or loads it afresh, it opens a new one |
| `send(to, &cmd)` | host: a command to players' games; returns the players it went to |
| `load_stage(area, episode)` | host: everyone to that stage afresh, a new session even for the stage played now. A dedicated server at the title goes there; a hosting player, after the file select |
| `set_mode(name)`, `set_status(text)` | host: what the launcher's list of games shows, the mode's name (`"squid-race"`, 15 characters) and a line of its state (`"Squid Race: racing"`) |
| `set_look(slot, look)`, `look(slot, out, size)` | (1.4) host: a player's Customize Mario colours forced, Mario's and FLUDD's (`"hat=00d0ff;shirt=00d0ff;overalls=ff8000;fluddBody=ff00ff"`, the launcher's region names): in their own game (their own Mario repainted at once) and every other, for newcomers too, until set again; `NULL` gives them their own back. `look` reads anyone's as it is now |
| `set_respawn(slot, how, &p, yaw)` | (1.4) host: where a player comes back each time they arrive in the session (just joined, back after a lost life): `SMS_RESPAWN_WARP` (there), `SMS_RESPAWN_BLOOPER` (onto a Blooper there) or `SMS_RESPAWN_NONE` (the stage's own start), after the session's sticky commands. It lasts until set again or the player leaves |
| `set_collision(on)`, `collision()` | (1.5) host: players collide with each other (1) or pass through (0), in every game, from now on; -1 gives back the host's own setting (its settings menu's Player Collision, `SMS_MULTIPLAYER_COLLISION=1`), which is what holds until a server mod sets it. `collision()`: whether they collide now, in any game ([Water fights](#water-fights)) |

`SmsNetPlayer`:

| Field | |
| --- | --- |
| `slot`, `server` | its slot; whether it is the dedicated server itself (no player) |
| `character`, `name`, `color` | `SMS_NET_CHAR_*` (Mario, Luigi, Wario, Metal, Fire, Purple, Orange, Ice); the name they chose, else the character's; the character's colour for text about them |
| `in_stage` | (host) arrived in the session it plays. A player loading, or still in the last stage, is not |
| `position`, `yaw` | where they are and which way they face, as their last update said |
| `flags` | `SMS_NETP_VISIBLE` (in play), `SURFING` (on a Blooper), `YOSHI`, `SPECTATING` (out of lives), `PAUSED`, `CUTSCENE` |
| `ping` | round trip in ms (-1 unknown) |
| `game_version`, `api_version` | (1.3) what their game said when it joined: its version (the launcher's, `"2026.10.18.1"`) and the mod API it carries (`(major << 16) + minor`); the host knows everyone's, any game its own |
| `pad` | (1.4) what they press, as their last update said: `SmsPadStatus` (`SMS_PAD_*` buttons, the sticks, the triggers), their controller as read, before a server holds it; `err` -1 when their game is older and does not say |

### Commands

`net->send(to, &cmd)`: `to` is `SMS_NET_ALL`, `SMS_NET_SLOT(n)`, or slots ORed together. An `SmsCmd` (`size = sizeof(SmsCmd)`, then `type`) fills the fields its type names; Lua's `sms.net.send(to, { type = "...", ... })` takes the named fields, and `to` as `"all"`, a slot, or a list of slots.

| Type | C fields | Lua fields | In each player's game |
| --- | --- | --- | --- |
| `SMS_CMD_BANNER` | `i[0]` colour, `text` | `color`, `text` | big text in the middle, in that colour; `""` hides it |
| `SMS_CMD_NOTICE` | `text` | `text` | a line of news at the top, for a few seconds |
| `SMS_CMD_STATUS` | `text` | `text` | a standing line at the top, shown while there is no news; `""` none |
| `SMS_CMD_TIMER` | `i[0]` `SMS_TIMER_HIDE` / `SHOW` / `UP` / `DOWN`, `i[1]` hundredths | `mode` (`"hide"`, `"show"`, `"up"`, `"down"`), `seconds` | the HUD's clock, stopped at the time or counting up or down from it by itself |
| `SMS_CMD_READY_GO` | `i[0]` ms until "Go!" | `go_in` (seconds) | the game's "Ready... Go!", "Go!" at that moment whatever the frame rate |
| `SMS_CMD_SOUND` | `i[0]` a system sound (`SMS_SOUND_*`); (1.4) `i[1]` 1 and `f[0..2]` a place | `id`, `position` | the sound; with a place, there in the world (heard from where the player is) |
| `SMS_CMD_MUSIC` | `i[0]` background music (`SMS_MUSIC_*`, the game's `MSD_BGM_*`); 0 stops it | `id` | the music |
| `SMS_CMD_WARP` | `f[0..2]` place, `f[3]` facing | `position`, `yaw` | the player there, as a warp puts them |
| `SMS_CMD_HOLD` | `i[0]` 1 or 0 | `on` | the player's controls held (a Blooper they ride stops), or theirs again |
| `SMS_CMD_MOUNT` | `i[0]..i[1]` actor types, `f[0..2]`, `f[3]` | `from`, `to` (the three Bloopers by default), `position`, `yaw` | the player onto a Blooper of their own game, there |
| `SMS_CMD_WATCH` | `i[0]` a slot, or -1 | `slot` | the player out of play, the camera riding with that player (as when out of lives); -1: back in play |
| `SMS_CMD_ACTORS` | `i[0]..i[1]` actor types, `i[2]` `SMS_ACTORS_SHOW` / `HIDE` / `KILL` | `from`, `to`, `state` (`"show"`, `"hide"`, `"kill"`) | the stage's actors of those types: back, out of the stage (unseen, touching nothing) until shown, or gone (a Shine: `0x20000013`) |
| `SMS_CMD_STOP_SCRIPT` | `text` the script's resource | `text` | the stage's event script loaded from there stopped (`"/scene/map/sp/gessoride.sb"`: Ricco's Blooper course's time trial) |
| `SMS_CMD_STAY` | `i[0]` 1 or 0 | `on` | the player's own ways out of the stage (pipes, doors, Exit Area) lead nowhere: only the host moves them on |
| `SMS_CMD_OBJECT` (1.4) | `i[0]..i[1]` actor types, `text` the object's kind, `i[2]` which (0 first; -1 each), `i[3]` `SMS_OBJECT_*`, `f[0..3]`, `f2[0]` | `from`, `to` (any type by default), `kind`, `index` (-1 by default: each), `what` (`"place"`, `"speed"`, `"phase"`, `"show"`, `"hide"`, `"scale"`, `"turn"`, `"move"`, `"kill"`), `position`, `yaw`, `times`, `seconds`, `size`, `turn` | one of the stage's objects ([below](#one-object)) |
| `SMS_CMD_SPAWN` (1.4) | a copy: `i[0]..i[1]` types, `text` kind, `i[2]` which; an item: types 0 and 0, `text` its name; `i[3]` its id, `f[0..2]` place, `f[3]` turn | `from`, `to`, `kind`, `index`, or `item`; `id`, `position`, `yaw` | a new object in the stage ([below](#spawning)) |
| `SMS_CMD_PLAYER` (1.4) | `i[0]` `SMS_PLAYER_*`, `i[1]` a value, `f[0..2]` a direction, `f[3]` a speed | `action` (`"lose_life"`, `"launch"`, `"dismount"`, `"health"`, `"water"`, `"nozzle"`, `"add_water"`, `"water_use"`, `"fludd"`), `value`, `direction`, `speed` | the player: the game's own miss now; thrown along a direction (40 a big push); off a Blooper or Yoshi; health 0 to 8; FLUDD's tank in percent; a nozzle (0 spray, 1 rocket, 2 underwater, 4 hover, 5 turbo); (1.5) percent of a full tank added (`SMS_PLAYER_ADD_WATER`, less when negative); FLUDD's water used at a percent of its own rate for the session (`SMS_PLAYER_WATER_USE`: 200 twice as fast); FLUDD on the player's back (1) or off it (0) (`SMS_PLAYER_FLUDD`) |
| `SMS_CMD_RULES` (1.4) | `i[0]` `SMS_RULE_*` bits | `no_fludd`, `no_damage`, `no_pause`, `no_pickups`, `water_hits`, `no_refill` | what the player may do: FLUDD sprays nothing; no health lost (falls still cost a life); no pause menu or map; coins and items stay where they are; (1.5) FLUDD's water hits the other players ([Water fights](#water-fights)); (1.5) FLUDD fills up only as the server says (`SMS_PLAYER_WATER`, `SMS_PLAYER_ADD_WATER`), not from the water around, bottles, fruit, 1-ups or a barrel. 0: everything again |
| `SMS_CMD_BOARD` (1.4) | `i[0]` a row (0 to 7; -1 every row), `i[1]` colour, `text` | `row`, `color`, `text` | a board at the left: that row's text in its colour (white by default); `""` clears it. Sticky per row: standings, a lap counter |
| `SMS_CMD_HUD` (1.4) | `i[0]` `SMS_HUD_*` bits | `coins`, `shines`, `blue_coins`, `health`, `water`, `lives`, `red_coins`, `all` | those parts of the HUD hidden (0: all back) |
| `SMS_CMD_MARKER` (1.4) | `i[0]` a marker (0 to 7), `i[1]` a slot or -1, `i[2]` colour, `i[3]` 1 to take it away, `f[0..2]` a place, `text` | `id`, `slot`, `color`, `remove`, `position`, `text` | a marker on the picture over a place, or over a player: its text in a pill underlined in its colour, or an arrow pointing down when `""` |
| `SMS_CMD_SCREEN` (1.4) | `i[0]` `SMS_SCREEN_*`, `i[1]` colour (its alpha the strongest), `f[0]` seconds | `effect` (`"clear"`, `"fade_out"`, `"fade_in"`, `"flash"`), `color`, `seconds` | the picture fades out to the colour (and stays), in from it, or flashes it; the text the server shows stays over it |
| `SMS_CMD_SHAKE` (1.4) | `f[0]` how hard (1 a hard hit, up to 5), `f[1]` seconds | `strength`, `seconds` | the camera shakes, dying away |
| `SMS_CMD_RUMBLE` (1.4) | `i[0]` 1 hard, 0 weak, `i[1]` times | `hard`, `times` | the player's controller rumbles (as their own rumble setting allows) |
| `SMS_CMD_BOUNDS` (1.5) | `f[0..2]` the middle, `f[3]` the radius (0: none), `i[0]` `SMS_BOUNDS_RING`, `i[1]` its colour, `f2[0]` seconds to glide there | `position`, `radius`, `ring`, `color`, `seconds` | an arena's edge: the player is kept within the radius of the middle, across the ground, pushed back in as they reach it (riders too); with a ring, a solid line marks it on the ground and the water. With seconds, the edge glides from where it was to the new radius, the same in every game |
| `SMS_CMD_SPLASH` (1.5) | `i[0]` the player splashed, `i[1]` their meter, `i[2]` its full (0: no meter), `i[3]` `SMS_SPLASH_DOWN`, `SMS_SPLASH_QUIET`, `f[0..2]` where the water hit, `f[3]` how long they are flushed, seconds (0: 0.8) | `slot`, `meter`, `full`, `down`, `quiet`, `position`, `flush` | that player splashed as Shadow Mario is ([Water fights](#water-fights)); send it to everyone, each game shows it on its copy of them |

Every command also takes:

- **`delay_ms`** (Lua `delay`, seconds): carried out that long after it was sent, at the same moment in every game, each taking off half its round trip to the host. Send a start ahead of time with the time left as its delay, and everyone starts together.
- **`SMS_CMD_STICKY`** (Lua `sticky = true`): kept for the session and carried out again for each player who arrives in it later: a newcomer, or a player back after a lost life (whose stage loads afresh). A sticky command replaces the last one of its kind sent to the same players (of its type; for `ACTORS`, its types; for `STOP_SCRIPT`, its script; for `OBJECT`, its object and what it does, `SHOW` and `HIDE` being one; for `SPAWN`, its id; for `PLAYER`, its action; for `BOARD`, its row; for `MARKER`, its marker). It goes at once only to players already in the session; the others get it as they arrive. A session keeps up to 192.
- **`SMS_CMD_UNSTICK`** (1.4; Lua `unstick = true`): sends nothing, and forgets the sticky command of its kind (as above, to the same players), so newcomers no longer get it. What it did in the games already there stays; send the opposite for that.
- **`f2[0..3]`** (1.4): four more numbers after `text`, the commands' `f[4..7]`, taken when `size` covers them (a mod built for 1.4 or later always does). `SMS_OBJECT_MOVE`'s seconds are `f2[0]`.

**One object** (`SMS_CMD_OBJECT`, API 1.4). The stage's objects of actor types `i[0]` to `i[1]` whose kind, as the stage names them, is `text` (a map object's name in the scene: `"WoodBlockCone"`, `"normallift"`, `"rollblocky"`; `""` any kind), and of those the `i[2]`th, in the order every game loads them (0 first), or each of them (-1). The game's log says what each one did, and where the first of them was (`[server] object ...`). `i[3]` says what:

| `i[3]` | Takes | What |
| --- | --- | --- |
| `SMS_OBJECT_PLACE` | `f[0..2]` place, `f[3]` turn (degrees about the vertical; 0 as the stage has it) | there, its collision too, and that is where it goes back to; not one on a rail (its rail says where it goes) |
| `SMS_OBJECT_SPEED` | `f[0]` times its own speed, up to 20 | a spinner turns faster or slower (the other way round when negative), a lift or moving platform goes along its rail faster or slower (0.05 at least) |
| `SMS_OBJECT_PHASE` | `f[0]` seconds, up to 60 | back to how the stage loaded it, then that much of its own movement at once (a lift along its rail, a spinner round): where it would be by then, the same in every game whenever it loaded the stage. Send it with a `delay_ms` for the moment it should be so (the start of a race) |
| `SMS_OBJECT_SHOW`, `SMS_OBJECT_HIDE` | | back; out of the stage (unseen, nothing to touch or stand on) until shown |
| `SMS_OBJECT_SCALE` | `f[0..2]` its size about x, y, z (1 as made) | bigger or smaller, its collision with it |
| `SMS_OBJECT_TURN` | `f[0..2]` its turn about x, y, z, degrees | turned every way (not one on a rail) |
| `SMS_OBJECT_MOVE` | `f[0..2]` a place, `f[3]` its turn about the vertical, `f2[0]` seconds | glides there (easing in and out), a movement tick at a time, and that is where it goes back to; its collision is set up there when it arrives (not one on a rail) |
| `SMS_OBJECT_KILL` | | gone for this stage (a coin collected, as the game takes things away for good) |

Spinners are the `TRollBlock`s, lifts and platforms the `TRailMapObj`s on a rail; speed and phase do nothing to other objects.

### Spawning

`SMS_CMD_SPAWN` (1.4) puts a new object in every player's stage: a **copy** of one the stage has, or an **item**.

- **A copy** of any object the stage loaded (a block, a lift, a Blooper, an NPC, an enemy), picked as `SMS_CMD_OBJECT` picks them (`i[0]..i[1]` types, `text` its kind, `i[2]` which), at `f[0..2]` turned `f[3]` degrees. The game makes it as the stage's loader made the original, from the original's own record in the stage's `scene.bin` with the new place: its manager takes it, it is checked against Mario as the rest are, and it moves, animates and draws as they do. Objects the game made itself (not from the stage's file) cannot be copied.
- **An item**, with types 0 and 0 and its name in `text` (`SMS_SPAWN_COIN`, `"coin"`, any stage; and others while one of their kind is in the stage, as its model is loaded then: `"coin_red"`, `"1up"`, `"bottle_large"`, `"FruitBanana"` and the other fruits...). A name the stage has no model for is refused (the game's log says so).
- **Its id**, `i[3]` (1 or more, the mod's own), names it from then on: `SMS_CMD_OBJECT` and `game->object_info` with the kind `"@<id>"` (`"@7"`) reach it, to move, turn, scale, hide or kill it. A spawn with an id that is there already moves that one instead.
- **Sticky**, as every spawn should be: a player arriving later, or back after a lost life, gets the same objects, in the same order (so multiplayer's item and enemy sync line up), and the later commands to them after.
- **Limits:** a stage has up to 128 spawns, 12 of them copies of objects with collision (blocks, lifts: the map's collision lists are its own, fixed), and only while the stage's memory has room (the game's log says when not). Nothing a stage makes is freed before it ends; kill what is done with rather than spawning anew. A copy keeps the original's name, so the stage's own scripts still find the original.

A command's effects last for the session it came in: when the host moves everyone on (`load_stage`, or the group leaving through a pipe), each game starts afresh, and the host replays the new session's sticky commands as each player arrives.

### Events

| Event | When | Payload |
| --- | --- | --- |
| `SMS_EV_NET_PLAYER_JOINED` | a player joined the game (every game hears it) | `SmsEvNetPlayer`: `slot`, `name`, `character` |
| `SMS_EV_NET_PLAYER_LEFT` | a player left | `SmsEvNetPlayer` |
| `SMS_EV_NET_PLAYER_ARRIVED` | (host) a player arrived in the session it plays: just joined, back after a lost life, or following everyone to a new stage. The session's sticky commands have been sent them | `SmsEvNetPlayer` |
| `SMS_EV_NET_SESSION` | (host) it opened a new session, which everyone follows: set the stage up here | `SmsEvNetSession`: `area`, `episode`, `session` |

| `SMS_EV_NET_PLAYER_SPLASHED` | (1.5; host) a player's water hit another player, while `SMS_RULE_WATER_HITS` holds | `SmsEvNetSplash`: `slot` (the player hit), `by` (whose water), `drops` (how many hit since the last report), `position` |

In Lua they are `net_player_joined`, `net_player_left`, `net_player_arrived`, `net_session` and `net_player_splashed`, with the same fields.

### Water fights

(API 1.5.) A server mod can make FLUDD a water pistol: players spray each other, and whoever is hit reacts as Shadow Mario does when Mario sprays him in his chases.

- **`SMS_RULE_WATER_HITS`** (`SMS_CMD_RULES`, to everyone) has FLUDD's water hit the other players. Each game judges its own player's water, as it judges what that water hits of the stage's: a drop that reaches another player stops there, and the hits are sent to the host, gathered up, about ten times a second while they keep coming. Every other game stops its copy of the drop at that player too, so nobody sees water go through anyone.
- **`SMS_EV_NET_PLAYER_SPLASHED`** on the host: who was hit (`slot`), by whom (`by`), how many drops since the last report (`drops`), and where. A spray held on a player from close by lands about a hundred drops a second, fewer from far off or on the move (movement runs 120 ticks a second at any frame rate, so the count does not change with it); the hover nozzle's two streams count twice. Nothing happens to anyone until a mod says so: keep each player's score of hits, or a meter, and decide.
- **`SMS_CMD_SPLASH`**, sent to everyone, shows a hit: Shadow Mario's water splash and hit sound where the water hit (`f[0..2]`), the player's model flushed for a moment after the last hit (0.8 seconds, or `f[3]` seconds), and, when `i[2]` is above 0, his water meter over their head, the blue bar `i[1]` of `i[2]` full, shown for twenty seconds after the last splash. With `SMS_SPLASH_DOWN` the player is knocked down where they stand, thrown back from the water as Shadow Mario falls; the mod then does what being down means (a lost life, watching the others). `SMS_SPLASH_QUIET` changes the meter without a splash.
- **`net->set_collision(1)`** makes players solid to each other, which suits a fight: nobody hides inside anyone, running into a player bumps you back, and you land on heads as on an enemy's, bouncing off.

**Water as ammunition** (1.5): `SMS_RULE_NO_REFILL` stops FLUDD filling up by itself, so the server hands water out: a starting tank (`SMS_PLAYER_WATER`), pickups of its own (a bottle it spawns where the stage has bottles, `SMS_CMD_SPAWN`'s `bottle_large`, or a marker over a spot; whoever reaches it gets `SMS_PLAYER_ADD_WATER`, as much as the mod likes: the bottle's own refill is kept off by the rule, its sparkle and sound stay), and `SMS_PLAYER_WATER_USE` to make a tank last less long. `SMS_PLAYER_FLUDD` takes FLUDD off a player's back (a lobby) and gives it back.

**An arena** (1.5): `SMS_CMD_BOUNDS` keeps everyone within a circle, drawn as a solid line on the ground with `SMS_BOUNDS_RING`; send a smaller radius with `f2[0]` seconds and the edge closes in smoothly, pushing players before it.

A water fight's mod, in short: `SMS_RULE_WATER_HITS | SMS_RULE_NO_DAMAGE` at the start, a meter per player, `SMS_CMD_SPLASH` for each `SMS_EV_NET_PLAYER_SPLASHED` with the meter left, and `SMS_SPLASH_DOWN` when it is empty. The [Splash Battle](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/splash-battle) example is one, last player standing wins.

### A small one

A server mod that keeps everyone in Gelato Beach, greets each player, and keeps a line at the top for all. In C:

```c
#include <sms_mod.h>
#include <string.h>

static const SmsApi* api;

static void send_text(uint32_t to, uint32_t type, uint32_t flags, const char* text)
{
    SmsCmd c;
    memset(&c, 0, sizeof c);
    c.size  = sizeof c;
    c.type  = type;
    c.flags = flags;
    c.text  = text;
    api->net->send(to, &c);
}

static int on_session(SmsMod* mod, SmsEvent* ev, void* user)
{
    SmsCmd stay;
    (void)mod; (void)ev; (void)user;
    memset(&stay, 0, sizeof stay);
    stay.size = sizeof stay; stay.type = SMS_CMD_STAY; stay.flags = SMS_CMD_STICKY; stay.i[0] = 1;
    api->net->send(SMS_NET_ALL, &stay);
    send_text(SMS_NET_ALL, SMS_CMD_STATUS, SMS_CMD_STICKY, "Beach party: everyone stays on Gelato Beach");
    return SMS_CONTINUE;
}

static int on_arrived(SmsMod* mod, SmsEvent* ev, void* user)
{
    const SmsEvNetPlayer* p = (const SmsEvNetPlayer*)ev->data;
    (void)mod; (void)user;
    send_text(SMS_NET_SLOT(p->slot), SMS_CMD_NOTICE, 0, "Welcome to the beach party!");
    return SMS_CONTINUE;
}

SMS_MOD_DEFINE()

SMS_MOD_EXTERN_C SMS_MOD_EXPORT int sms_mod_load(SmsMod* mod, const SmsApi* a)
{
    api = a;
    if (!SMS_API_HAS(api, net))
        return 1;
    api->net->load_stage(4, 0); /* Gelato Beach, for everyone */
    api->core->subscribe(mod, SMS_EV_NET_SESSION, on_session, NULL, SMS_PRIORITY_NORMAL);
    api->core->subscribe(mod, SMS_EV_NET_PLAYER_ARRIVED, on_arrived, NULL, SMS_PRIORITY_NORMAL);
    return 0;
}
```

The same in Lua (`runtime = lua`, `side = server`):

```lua
sms.net.load_stage(4, 0) -- Gelato Beach, for everyone

sms.on("net_session", function()
  sms.net.send("all", { type = "stay", on = true, sticky = true })
  sms.net.send("all", { type = "status", text = "Beach party: everyone stays on Gelato Beach", sticky = true })
end)

sms.on("net_player_arrived", function(ev)
  sms.net.send(ev.slot, { type = "notice", text = "Welcome to the beach party, " .. ev.name .. "!" })
end)
```

### Patterns

- **Set the stage up at `SMS_EV_NET_SESSION`, sticky:** stop its script, hide what should not be there yet, keep players in. Players who arrive later, or come back after a lost life, get it all again.
- **Judge on the server.** Keep the game mode's state in the mod: the phase, the clock (`core->time()`), each player's progress. Where players are comes from `net->player`; the stage's own trigger cubes say what they are in (`game->in_cube`, as event scripts ask). Tell each player only what they need to see.
- **Line moments up with `delay_ms`.** The Squid Race sends its start three seconds early (onto a Blooper, the clock, the music), each with the time left as its delay, and puts "Go!" there with `READY_GO`'s time.
- **React to arrivals.** `SMS_EV_NET_PLAYER_ARRIVED` covers a player joining mid-round and one back after falling in: bring them up to date (the clock as it stands, a Blooper at the start).
- **Mind the moment after a command.** A player's next update shows what a command did a round trip later; leave a player alone for a moment after warping them (Red Light, Green Light does, for a second and a half).
- **Lives and flags** are the host's to set (`game->flag_set` on the host): the Squid Race keeps everyone's lives up, so a race has no game over.
- **Tell the launcher** what is on: `set_mode` once, `set_status` as the state changes.

### Versions

By default a host takes only players on exactly its build of the game. A host may let versions drift instead, for beta builds that change nothing between players, and may ask for at least some versions. Players' games carry the protocol number (which must always match: it is how the games understand each other), their build, their version and the mod API they carry; the host checks them as they join, and its answer to the launcher's probe says what it takes, so the launcher can tell a player before starting the game.

| Setting (the host's environment) | |
| --- | --- |
| `SMS_MULTIPLAYER_VERSIONS=exact` | the host's own build only (the default) |
| `SMS_MULTIPLAYER_VERSIONS=drift` | any build of the same protocol and the same Eclipse setting |
| `SMS_MULTIPLAYER_MIN_VERSION=2026.10.18.1` | and at least this game version (a game that does not say its version is refused) |
| `SMS_MULTIPLAYER_MIN_LAUNCHER=0.1.62` | and at least this launcher |

A game's version is the launcher's (`SMS_GAME_VERSION`, which the launcher sets when it starts the game; a build without it says the date of its last commit); versions compare a number at a time, a beta (`1.2.0-beta`) before its release.

A server mod says what it needs of the players' games with `client_api = 1.2` in its `mod.ini`: with versions drifting, an older game might not know a command the mod sends, so the host refuses games older than the highest `client_api` of its server mods. `net->player()` gives each player's `game_version` and `api_version` for anything finer.

### Limits, and what is next

- **The commands are what players' games can do.** A new kind of command needs a new game build (an API minor version), and the players need it too, as for any change to the game; a server mod checks `SMS_API_HAS` as any mod does.
- **Players' worlds have only what the game has:** a server mod can copy and spawn what the stage has (its objects, its items) and mark places with text, but not bring models of its own. Planned: actors the stage does not have, from other stages' files.
- **The host believes players' positions,** as multiplayer believes their games: a changed game could claim to be anywhere. Fine among friends; a public server cannot stop cheating this way.
- **Players' input** reaches a server mod as each player's controller in their updates (`SmsNetPlayer.pad`, 1.4), 30 or so a second: enough for votes and "ready", not for frame-exact timing. Planned: companion mods players install alongside (`side = both`), for what a server mod alone cannot show.

## Time, units and frame rate

- **Frames and ticks.** `SMS_EV_FRAME` and `SMS_EV_DRAW` come once a displayed frame. `SMS_EV_TICK` comes once a simulation step of a stage, `ticks_per_second` (30, 60 or 120, from the port's `frame_rate` setting) times a second while the stage runs, and not while it is paused or loading. Count game time in ticks (or `dt`, the seconds a tick), not in frames.
- **Real time.** `core->time()` is seconds since start; it goes on through pauses and loading.
- **Distances** are the game's units (100 to a metre, roughly: Mario is about 160 tall). **Velocities** (`SmsPlayer.velocity`, `forward_speed`) are units a tick, so their meaning changes with the frame rate; for a speed in units a second, measure the distance moved over `dt` (as `trainer-hud` does) or multiply by `ticks_per_second` with care.
- **Angles**: `SmsPlayer.yaw` is degrees, 0 facing +Z and 90 facing +X; the raw `face_angle` is the game's 16-bit angle (65536 to a turn). `SmsCamera.fovy` is degrees.
- **The world's axes**: +Y is up.

## Multiplayer, save states and Eclipse

- **Multiplayer** (`ext/multiplayer`) runs one game per player, each with its own mods. Mods that only show things are safe. A mod that changes how the game plays (mark it `gameplay = 1`) can make the players' games disagree unless every player runs it with the same settings; the session does not yet compare mod lists (planned). A game mode for everyone is a [server mod](#server-mods): it runs on the host alone, and the players need nothing.
- **Save states** capture the game's memory, not the mods'. After a load, a mod's own state (timers, trails) may not match the game; mods that care should check `game->state` and the player each frame rather than trusting what they kept. (A save/load event is planned.)
- **Super Mario Eclipse** builds run native mods too. `game->state` says `eclipse = 1`; its stages and flags are numbered differently, and its own code patches the game in places, so mods that change gameplay should be tested there.

## Game reference

### Areas

`game->area_name(area)` gives the name in the game's language. The vanilla game's areas (`SmsEvStage.area`; `SMS_WARP=area,episode` takes the same numbers, and `SMS_WARP_LIST=1` prints every stage and episode):

| Area | Stage | Area | Stage |
| --- | --- | --- | --- |
| 0 | Delfino Airstrip | 13 | Pinna Park (the park) |
| 1 | Delfino Plaza | 14 | Sirena Beach |
| 2 | Bianco Hills | 15 | Delfino Plaza (title screen) |
| 3 | Ricco Harbor | 16 | Noki Bay |
| 4 | Gelato Beach | 20–24, 29 | Delfino Plaza secret courses |
| 5 | Pinna Park (beach) | 30 | Ricco Harbor secret course |
| 6, 7 | Sirena Beach | 31 | Noki Bay secret course |
| 8 | Pianta Village | 32, 33 | Gelato Beach secret courses |
| 9 | Noki Bay | 46, 47, 55 | Bianco Hills (secrets, boss) |
| 12 | Delfino Plaza | 48, 59 | Ricco Harbor |
| 40, 51, 56 | Sirena Beach | 41, 50, 58 | Pinna Park |
| 42 | Pianta Village | 44, 57 | Noki Bay |
| 52, 60 | Corona Mountain | | |

Episodes count from 0 (`episode + 1` is the number the game shows).

### Flags

`game->flag_get` / `flag_set` read and write the save file's flags (`TFlagManager`):

| Flag | Is |
| --- | --- |
| `0x10000 + n` | Shine `n` (0–119) collected |
| `0x20001` | lives |
| `0x20005 + area` | the coin record of an area |
| `0x40000` | Shines collected |
| `0x40001` | blue coins collected |
| `0x40002` | coins in the current stage |

Other flags (events seen, nozzles unlocked, the plaza's state) are in the decomp's `src/System/FlagManager.cpp`; setting them can put the story in a state it does not expect.

### Item kinds

`SmsEvItem.kind`: `SMS_ITEM_COIN`, `SMS_ITEM_RED_COIN`, `SMS_ITEM_BLUE_COIN`, `SMS_ITEM_OTHER` (Shines, nozzles and other `TItem`s; `actor_type` tells them apart). 1-Up mushrooms are not `TItem`s and do not come through this event.

### Hit messages

`SmsEvHit.message` (the decomp's `HIT_MESSAGE_*`): `0x0` stomped, `0x1` ground-pounded, `0x2` pushed up, `0x3` super ground pound, `0x4` taken, `0x6` put down, `0x7` thrown, `0x9` electric shock, `0xC` punched, `0xE` touched, `0xF` sprayed with water.

### Nozzles

`SmsPlayer.nozzle` (the second nozzle, the one the nozzle button swaps to), `SmsPlayer.current_nozzle` (the one out now) and `SmsEvPlayerNozzle`: 0 spray, 1 rocket, 2 underwater, 3 Yoshi (his juice: FLUDD's nozzle while Mario rides him), 4 hover, 5 turbo. The diving helmet is not a nozzle but what he wears (`SmsPlayer.helmet`, `player->set_outfit`).

`SMS_EV_PLAYER_NOZZLE` (1.5) comes before FLUDD changes nozzle, once for each change the player sees, with `reason`:

| `reason` (Lua) | when | `to` | cancel |
|---|---|---|---|
| `SMS_NOZZLE_SWITCH` (`"switch"`) | a nozzle picked up (out of its box, or lying in the stage), getting on Yoshi (`to` 3) or off him (`to` the hover, then the spray), the game's own switches, `player->set_nozzle` | changeable | the nozzle stays (a nozzle picked up is gone all the same) |
| `SMS_NOZZLE_SWAP` (`"swap"`) | the nozzle button: to the second nozzle, or back to the spray (`to` 0) | changeable to the second nozzle (it becomes the second one), not on the way back | no swap |
| `SMS_NOZZLE_BOX` (`"box"`) | a nozzle's box is set up as the stage loads (before `SMS_EV_STAGE_LOADED`; `from` -1, `box` the box) | the box's nozzle: 1 rocket, 4 hover, 5 turbo | the box is not in the stage |
| `SMS_NOZZLE_STAGE_START` (`"stage_start"`) | a stage that carries FLUDD's nozzle over gives it the second nozzle the file remembers, with the spray out (Hotel Delfino, for one; not the plaza nor the worlds' own stages, where FLUDD starts with the hover), or Bowser's stage gives the rocket | changeable | FLUDD keeps the nozzles it has |

`to` takes 0 to 5 (3 only to get on Yoshi, and only 3 then); another number is not taken and the nozzle asked for stays. A mod that keeps a nozzle locked refuses it for `SWITCH`, `SWAP` and `STAGE_START` (or changes `to`), and leaves its boxes out with `BOX`: the hover, FLUDD's second nozzle as most stages start, comes through `SWAP`.

### Mario statuses

`SmsPlayer.status` and `SmsEvPlayerStatus` use the game's status numbers (all in the decomp's `include/Player/MarioStatus.hpp`). Some:

| Status | Value | Status | Value |
| --- | --- | --- | --- |
| standing | `0x0C400201` | dive | `0x00000891` |
| running | `0x04000440` | rocket | `0x0000088B` |
| jump | `0x02000880` | landing | `0x0000088C` |
| second jump | `0x02000881` | wall slide | `0x000008A7` |
| triple jump | `0x00000882` | swimming | `0x000022D2` |
| back flip | `0x00000883` | hanging | `0x3800034B` |
| long jump | `0x00000888` | Shine get | `0x00001302` |

Bits: `0x800` airborne, `0x2000` swimming, `0x400` running; `status & 0x1C0` is the kind (`0x000` waiting, `0x040` running, `0x080` jumping, `0x0C0` swimming, `0x100` cutscene, `0x140` special, `0x180` action).

## Debugging

| Setting (`settings.txt`) | Variable | Does |
| --- | --- | --- |
| `native_mods = off` | `SMS_NATIVE_MODS=0` | load no native mods |
| `native_mods_disabled = a;b` | `SMS_NATIVE_MODS_DISABLED` | leave these mods out |
| | `SMS_NATIVE_MODS_DIR=dir;dir` | look for mods in these folders instead of `mods/` |
| | `SMS_NATIVE_MODS_DATA=dir` | keep mods' settings and data here |
| | `SMS_NATIVE_MODS_DEBUG=1` | show `SMS_LOG_DEBUG` lines and the API's own notes |
| | `SMS_NATIVE_MODS_HOT=1` | **hot reload**: load a mod again when its library or script changes (checked twice a second) |
| | `SMS_NATIVE_MODS_MENU_KEY=F2` | the Mods menu's key |

- **The log** (the terminal, or the launcher's log) has a `[mods]` summary, each mod's `[mod:<id>]` lines, why any mod did not load, and every key clash.
- **Hot reload.** With `SMS_NATIVE_MODS_HOT=1` the game loads a copy of each library, so you can rebuild while it runs; when the library changes it unloads the mod (calling `sms_mod_unload`) and loads the new build, keeping its settings. Keep the state you want to survive in settings.
- **A debugger.** The mod is an ordinary shared library in the game's process: `gdb build/linux-32/sms`, `break sms_mod_load`, and its symbols load with it (build it with `-g`).
- **A crash in a mod** takes the game down; the backtrace names the mod's library. Start with `SMS_NATIVE_MODS=0` to rule mods out, then `SMS_NATIVE_MODS_DISABLED` to find the one.

## Compatibility

- The game loads a mod whose `sms_mod_api_version()` has the game's **major** version; another major is refused with a message.
- The current version is **1.5** (1.1 added `core->mod_info` and `core->host_runtime`, for script mods; 1.2 added `api->net`, the `SMS_EV_NET_*` events and `game->in_cube`, for [server mods](#server-mods); 1.3 added players' versions and `client_api`, for [versions](#versions); 1.4 added the stage's objects one at a time, spawning, players, rules and the HUD for server mods, the stage's objects and ground for any mod, and `api->world`, `SmsEvStage.reason` and `SMS_EV_STAGE_START`, for [worlds and episodes](#world); 1.5 added FLUDD's nozzles and Mario's outfit, [events for every kind of mod](#events-for-every-kind-of-mod-15), a mod's own [files](#files), water fights and arenas for server mods, and Mario's facing, aim and hidden parts for first-person cameras).
- **Minor versions only add**: events, functions at the ends of tables, fields at the ends of structs. A mod built for an older minor runs on a newer game unchanged. A mod built for a newer minor runs on an older game if it checks what it uses with `SMS_API_HAS` and `ev->size` (the game logs a note when a mod is newer than it).
- Nothing in the API depends on the game's internal layout, so game updates (and the decomp's progress) do not break mods. `api->native` is the exception, by design.

## Pre-releases

The next minor version comes out first as betas: `v1.6.0-beta.1`, `v1.6.0-beta.2`, ..., then `v1.6.0`. They are on the [releases page](https://github.com/chasem-dev/sms-mod-sdk/releases) marked *Pre-release* (with the lua mod's zip), and in the [changelog](../CHANGELOG.md) under the release they lead to. A beta is for trying what is new while it is made: what a beta added may still change (be renamed, take other arguments, or go) in a later beta, and is settled only in the release. What came out in a release never changes.

To build a mod against a beta:

1. **Name the beta's tag** where the SDK comes from: `GIT_TAG v1.6.0-beta.1` in your `FetchContent_Declare` (the templates have it in `CMakeLists.txt`), or `git checkout v1.6.0-beta.1` in a checkout of the SDK, then configure and build as always (CMake fetches the new tag; delete `build/` if it keeps the old one).
2. **Run it in a game built with that beta.** The PC port's beta builds (SMS Launcher's beta channel, or a source build of the port's `main`) carry the SDK's latest beta once the port moves to it; the game's log says which: `[mods] API 1.6 beta 1: ...`. A release build of the game has the last release (1.5) and does not have what the beta adds; check with `SMS_API_HAS` and `ev->size` as for any newer minor, and the mod still loads there.
3. **Lua script mods** need the lua mod of the same beta (`sms-lua-mod-1.6.0-beta.1.zip` from the beta's release, unzipped into the game's `mods/`), or, in a source build of the port, `python3 tools/mods/get.py lua` once the port pins it.
4. **When the release is out, rebuild** against it (`GIT_TAG v1.6.0`) and fix what changed since your beta (the changelog says). Share a mod built against a beta only with others testing the same beta.

`SMS_MOD_API_PRERELEASE` is the beta's number (0 in a release); `SMS_MOD_DEFINE` exports it as `sms_mod_api_prerelease`, and the game's own is `api->prerelease` (1.5; check `api->size`). The game logs a warning when a mod was built against a beta other than the game's (or against a beta of the game's release): `made for mod API 1.6 beta 1 (what a beta adds may change before its release); this game has 1.6 beta 3: rebuild it`. It still loads it.

## Design notes

### Why a C ABI and events

The port compiles the game from the decomp as native code, so there are no fixed addresses to patch as GameCube code mods do, and the game's classes change layout with every compiler, pointer size and decomp update.
A C function-table API keeps mods independent of all of that: one build of a mod works with every game build of the same major version, mods can be written in any language, and the game knows exactly what each mod registered, which is what lets it order, isolate, unload and hot-reload them.
Hooks are named events at chosen places in the game (`port_ext.h`), each general enough for many mods, rather than detours on arbitrary functions, which two mods cannot share predictably.

### Built-in extensions

The port's [`ext/`](https://github.com/chasem-dev/sms-pc-port/tree/main/ext) holds its built-in extensions (multiplayer, the demo pilot), which hook the game through `port_ext.h`.
The native mod API is one more of them ([`ext/modapi`](https://github.com/chasem-dev/sms-pc-port/tree/main/ext/modapi)): it turns those hooks into mod events and adds the rest (loading, drawing, input, settings).
New hook points go into `port_ext.h` once and serve both built-in extensions and mods.

### What modders will want, and where it stands

| Need | Today | Planned |
| --- | --- | --- |
| HUDs, overlays, information | `draw`, `hud_slot`, `camera->world_to_canvas`, `player`, `game` | the game's own font; textures from the game's archives |
| Markers in the world | `world_line/box/ring/text` (over the scene) | drawing inside the scene, hidden by walls (a GX layer in the game's frame) |
| New moves, movement tweaks | `SMS_EV_TICK` + `player->set_*`, `SMS_EV_PLAYER_STATUS`, `PLAYER_STATUS_CHANGED`, `PLAYER_JUMP`, `PLAYER_LAND`, `PLAYER_SPAWN` | per-status hooks (a status's code each tick), Mario's parameters |
| Rules (lives, damage, coins, exits) | `SMS_EV_PLAYER_DAMAGE` (and its source), `PLAYER_HEAL`, `PLAYER_HIT`, `PLAYER_DEATH`, `GAME_OVER`, `COINS_RESET`, `STAGE_EXIT_REQUEST`, `STAGE_LEAVING`, `FLAG_CHANGED`, `SAVE_PROMPT`; `TIMER`, `PAUSE` | the pause menu's own entries, Shine conditions |
| Camera mods | `SMS_EV_CAMERA` (position, target, field of view) | camera modes, roll |
| Input remapping, scripted play, TAS tools | `SMS_EV_PAD_READ`, `SMS_EV_KEY`, `capture` | mouse, more controllers |
| Objects and enemies | `SMS_EV_OBJECT_HIT`, `OBJECT_MOVE`, `ACTOR_SPAWN`, `ACTOR_DAMAGE`, `ACTOR_DEATH`, `SHINE_APPEAR`, `SHINE_GET`, `MAP_EVENT`; `find_object`, `object_info`, `object_type`, `object_health`, positions | spawning the game's actors for any mod, per-object hooks, enemies' own attacks on each other |
| New content | files in `mods/<name>/files/` (game file mods) | a native mod switching on its own `files/`; new stages |
| Sound | `audio->play_system_sound`; `SMS_EV_SOUND` and `SMS_EV_MUSIC` change or silence the game's | sounds placed in the world for any mod, a mod's own WAV/OGG |
| Messages and text | `SMS_EV_TALK` (another message shown), `SMS_EV_SCRIPT_CALL` | the game's talk boxes with a mod's own text |
| Saving a mod's progress | (1.5) `api->files` beside the card, per save file (saved, copied and erased with it); `config`, `core->data_dir` | save-state events |
| Multiplayer game modes | [server mods](#server-mods): `api->net`'s players and commands, dedicated servers | spawning actors and text in players' worlds; players' buttons; companion mods (`side = both`); the session comparing `gameplay` mods |
| Scripting | Lua script mods (the `lua` mod), `core->host_runtime` for other languages | services for scripts; a sandboxed mode for script mods |
| Anything else | `api->native` + the decomp's headers (same build only) | the code mods' retail-address hooks (`sms_modhook.h`, about 500 places the Eclipse port uses) offered to native mods |

### Scripting

Lua is a mod, not part of the game: the `lua` mod embeds Lua 5.4 and runs script mods through `core->host_runtime`.
The game's part is small and general: it treats a `runtime = <name>` folder as a mod of its own (id, settings, menu row, order, hot reload) and hands its start and stop to the mod hosting that runtime.
Scripts need no compiler and no binary per platform, and their speed is plenty for HUDs, rules and menus; anything heavier, or anything needing a new hook, stays in C.
Lua 5.4 is small (the whole lua mod is about 350 KB stripped), embeds cleanly, sandboxes well, and is familiar from many games' mod scenes.

## Adding to the API

The API is this repository. The game's side of it, the runtime, is [`ext/modapi`](https://github.com/chasem-dev/sms-pc-port/blob/main/ext/modapi/README.md) in [sms-pc-port](https://github.com/chasem-dev/sms-pc-port), which has this repository as its `sdk/` submodule.
A change goes here first, then into the port:

1. **Here:** add the event's id and payload at the end of the lists in `include/sms_mod.h` (a function at the end of its table's struct), document it in this manual ([Events](#events), [The API](#the-api)), give Lua scripts it in `lua/lua_host.c` if they should have it, and note it in `CHANGELOG.md` under the coming release's next beta. The first change after a release starts the next minor's betas (`SMS_MOD_API_MINOR` + 1); tag betas as the work goes and the release when it is done ([RELEASING.md](RELEASING.md)).
2. **In the port:** move `sdk/` to that tag (a beta's, while the game's beta builds try it). If the game needs a new hook point, add it to `PortExtension` and the `port_ext_*` functions (`src/port_include/port_ext.h`, `ext/core/port_ext.cpp`) and call it from a `zzzzz-ext-*` decomp patch, general enough for other extensions.
3. In `ext/modapi/game/modapi_game.cpp`, build the payload in the extension's hook and call `modh_dispatch` (skip it when `modh_wants` says nobody listens). If it is cancellable or consumable, add it to `cancellable()` / `consumable()` in `ext/modapi/host/mod_runtime.cpp`; a function goes in its table's initializer there.

Never reorder or remove anything a release has in `sms_mod.h`: that is a new major version, and every mod built before it stops loading. What only betas have may still change; say so in the changelog.
