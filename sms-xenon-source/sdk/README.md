# sms-mod-sdk

The modding SDK for the [Super Mario Sunshine PC port](https://github.com/chasem-dev/sms-pc-port).

A mod is a folder in the game's `mods/` with a `mod.ini`: a native library (C, C++, Rust, Zig, anything that makes a C shared library) or a Lua script.
The game loads every mod at start and gives each the same API, so any number run together: their handlers go in order, their panels stack instead of overlapping, they are warned about keys two of them want, and each gets a row and its settings in the game's **Mods menu** (F1).
Mods can hear of what happens in the game (a stage loads, Mario jumps, lands or is hurt, an enemy is defeated, a coin is taken, a flag or a sound changes, a key is pressed), read and change it (Mario, the save file's flags, the camera, the controllers), draw over it (panels, text, images, markers in the world) and talk to each other.

**Server mods** run a multiplayer game mode from the host alone, a dedicated server as a rule, while players join with the plain game: the server decides when and where, and each player's game shows it with its own models and HUD ([Server mods](docs/MODDING.md#server-mods)).

**The manual is [docs/MODDING.md](docs/MODDING.md)**: every event and function, the `mod.ini`, Lua, drawing, input, settings, game numbers (stages, flags, items), debugging and the design.

## Start here: templates and examples

Templates and examples are in **[sms-mod-templates](https://github.com/chasem-dev/sms-mod-templates)**.

| | |
| --- | --- |
| [`templates/cpp`](https://github.com/chasem-dev/sms-mod-templates/tree/main/templates/cpp) | A C++ mod to copy as a repository of your own: builds with CMake against this SDK, and its GitHub workflow builds every platform and zips a release. |
| [`templates/c`](https://github.com/chasem-dev/sms-mod-templates/tree/main/templates/c) | The same in C. |
| [`templates/lua`](https://github.com/chasem-dev/sms-mod-templates/tree/main/templates/lua) | A Lua script mod: a `mod.ini` and a `main.lua`, nothing to build. |
| [`examples/hello`](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/hello) | C++, the smallest useful mod: names the stage, counts coins, a key and a setting. |
| [`examples/trainer-hud`](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/trainer-hud) | C, most of the API: a speed panel and graph, a trail and landing ring in the world, coin pop-ups, a field of view setting, invincibility, moon jump, images, options, keys, another mod's events. |
| [`examples/stage-timer`](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/stage-timer) | C, made to sit beside others: a panel that stacks under the trainer HUD's, damage watched at `MONITOR` priority, best times in its settings, a custom event other mods can hear. |
| [`examples/lua-challenges`](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/lua-challenges) | Lua: a challenge against the clock each time a stage loads, with a panel, world markers, sounds, settings, a key, a module of its own, and the C stage-timer's events. |
| [`examples/squid-race`](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/squid-race) | C, **a server mod**: a race round Ricco Harbor's Blooper course for everyone who joins a multiplayer game, run by the host alone; players join with the plain game. |
| [`examples/lua-red-light`](https://github.com/chasem-dev/sms-mod-templates/tree/main/examples/lua-red-light) | Lua, **a server mod**: Red Light, Green Light for everyone in a multiplayer game. |

## What is here

| | |
| --- | --- |
| [`include/sms_mod.h`](include/sms_mod.h) | The API: one C header, every function and payload documented where it is declared. A native mod needs nothing else. |
| [`include/sms_mod.hpp`](include/sms_mod.hpp) | A header-only C++11 layer on it: lambdas for handlers, payloads by type, a class for the mod. |
| [`cmake/SmsMod.cmake`](cmake/SmsMod.cmake) | `sms_add_mod(<folder> <sources>)`: builds a mod and lays it out as the game loads it (`<id>/mod.ini`, `<id>/bin/<platform>/<library>`); `sms_add_script_mod(<folder>)` for a script mod. |
| [`docs/MODDING.md`](docs/MODDING.md) | The manual. |
| [`lua/`](lua) | The **lua** mod, which runs Lua script mods. Optional: players install it once (below). |
| [`tests/`](tests) | Checks that the headers build warning-free as C99 and C++11 on every compiler, and that a built mod exports what the game looks for. |
| [`CHANGELOG.md`](CHANGELOG.md) | What each API version added. |

## Using the SDK

**With CMake** (what the templates do):

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_mod C)

include(FetchContent)
FetchContent_Declare(sms_mod_sdk
  GIT_REPOSITORY https://github.com/chasem-dev/sms-mod-sdk.git
  GIT_TAG v1.5.0)
FetchContent_MakeAvailable(sms_mod_sdk)

sms_add_mod(mod src/my_mod.c)   # mod/ holds mod.ini and whatever ships with it
```

```sh
cmake -S . -B build -DSMS_MODS_OUT=/path/to/the/game/mods   # add -DSMS_MOD_ARCH=32 for the 32-bit Linux game
cmake --build build
```

`-DFETCHCONTENT_SOURCE_DIR_SMS_MOD_SDK=<path>` builds against a checkout of the SDK instead of the tag.
To try the next version while it is made, name a pre-release's tag (`GIT_TAG v1.6.0-beta.1`, once there is one) and run the mod in a beta build of the game: [MODDING.md, "Pre-releases"](docs/MODDING.md#pre-releases).
`sms::mod_sdk` is the headers alone, for a library built some other way.

**Without CMake:** put `include/` on the include path. A mod exports `sms_mod_api_version` (`SMS_MOD_DEFINE()`) and `sms_mod_load`, and its library goes in `mods/<id>/bin/<platform>/` beside the `mod.ini`. The platforms: `linux-x86`, `linux-x86_64`, `linux-arm64`, `windows-x86`, `windows-x86_64`, `macos-x86_64`, `macos-arm64`.

**In Lua:** nothing from here is needed but the manual's [Lua section](docs/MODDING.md#lua-scripts); start from [`templates/lua`](https://github.com/chasem-dev/sms-mod-templates/tree/main/templates/lua).

## The lua mod

Lua script mods (`runtime = lua` in their `mod.ini`) run in the **lua** mod, a native mod that embeds Lua 5.4.
It is optional: the game does not include it, so players who want script mods install it once.

- From the [releases](https://github.com/chasem-dev/sms-mod-sdk/releases): `sms-lua-mod-<version>.zip` holds `lua/` with a library for every platform; unzip it into the game's `mods/` folder.
- From a source checkout of the port: `python3 tools/mods/get.py lua`.
- Or build it: `cmake -S . -B build -DSMS_MODS_OUT=/path/to/the/game/mods` here (Lua's sources are downloaded from lua.org and checked against the release's hash; `-DSMS_LUA_SOURCE_DIR=<lua-5.4.x>` uses a copy).

## Versions

A release's tag is the API version it describes: `v1.1.0` is API 1.1 (patch releases change documentation, tools or the lua mod, never the API).
Within a major version the API only grows: a mod built against any 1.x loads in every game with API 1.x, and checks with `SMS_API_HAS` for what a game older than its SDK may lack.
The game logs the API version it implements, and refuses mods of another major version.
See [Compatibility](docs/MODDING.md#compatibility).

The game's side of the API is the runtime in the port, [`ext/modapi`](https://github.com/chasem-dev/sms-pc-port/tree/main/ext/modapi), which has this repository as its `sdk/` submodule.
Changes to the API are made here first, then implemented there ([Adding to the API](docs/MODDING.md#adding-to-the-api)).

## License

[MIT](LICENSE). Mods built with the SDK may use any license, closed source included.
The lua mod's release also includes Lua (MIT, [`lua/LICENSES.txt`](lua/LICENSES.txt)).
