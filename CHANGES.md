# What was created or changed

**English** | [Português (Brasil)](ALTERACOES.md)

Starting point: the original source code of the Xbox 360 port, `sms-xenon-source-f233c15`, imported
with **no changes** in the first commit. Everything that changed after it is listed here.

To see the differences line by line:
```bash
git diff 26a14ce HEAD -- sms-xenon-source
```

## Source files (`sms-xenon-source/`)

| File | Type | What changed |
|---|---|---|
| [`xenon/dist/sms_language.py`](sms-xenon-source/xenon/dist/sms_language.py) | **new** | Builds the language packs (`mods/languages/<code>`) from the North American disc plus a PAL disc (the European one or the Brazilian Portuguese translation). |
| [`xenon/dist/sms_extract.py`](sms-xenon-source/xenon/dist/sms_extract.py) | changed (+4 lines) | Given a European disc, explains that it is used for language packs instead of only refusing it. |
| [`xenon/platform/xe_language.cpp`](sms-xenon-source/xenon/platform/xe_language.cpp) | **new** | The language menu shown before the game starts, in the console's own message box (`XShowMessageBoxUI`): English and every installed pack, two per page with "More". It waits until the controller's buttons are released, opens the box again if the system closes it right away, and writes what happened to `sms.log`. |
| [`platform/misc/language.cpp`](sms-xenon-source/platform/misc/language.cpp) | changed (+11 lines, Xbox only) | With no `SMS_LANGUAGE` (or `SMS_LANGUAGE=ask`), asks the menu above for the language. The PC build is unchanged. |
| [`xenon/dist/README.txt`](sms-xenon-source/xenon/dist/README.txt) | changed (+12 lines) | New "Languages" section, with the menu. |
| [`xenon/dist/sms_env.txt`](sms-xenon-source/xenon/dist/sms_env.txt) | changed (+5 lines) | A commented-out `SMS_LANGUAGE=pt` line, and how the menu works. |
| [`xenon/tools/xpackage`](sms-xenon-source/xenon/tools/xpackage) | changed | Ships `sms_language.py` in the package. |
| [`xenon/tools/xstubs.py`](sms-xenon-source/xenon/tools/xstubs.py) | changed (fix) | Works with the `compile_commands.json` that Ubuntu 22.04's CMake writes (no `output` field), and says clearly when the Linux build is missing. Without it, `xstage` stopped halfway and Visual Studio got unconverted code (C2059 errors in `J3DJoint.hpp`). |
| [`decomp/.gitattributes`](sms-xenon-source/decomp/.gitattributes) | changed (1 line) | `* text=auto` → `* text=auto eol=lf`. Without it, cloning on Windows turned the decompilation's sources into CRLF and the first decomp patch failed (`gesso.cpp ... hunk FAILED`). Now cloning in Ubuntu, cloning on Windows or downloading the ZIP all give LF files. |

The language packs themselves use the pack system the port already had (`platform/misc/language.cpp`,
`platform/dvd/dvd.cpp`); the only C/C++ changes are the menu (`xe_language.cpp`) and the call to it.

## New files at the repository root

| File | Purpose |
|---|---|
| `README.md`, `README.pt-BR.md` | Step-by-step guide and credits, in English and Portuguese. |
| `CHANGES.md`, `ALTERACOES.md` | This list, in English and Portuguese. |
| `LICENSE` | License of my additions (MIT), pointing to the original licenses. |
| `.gitignore` | Keeps disc images, game data and compiled files out of the repository. |
| `.gitattributes` | Stores files byte for byte, so the Linux scripts do not break. |

## Technical note on the first commit

The original code was imported exactly as it was in the `.7z`, with one difference that does not
change any file's contents: the 168 scripts that start with `#!` (such as `build.sh` and
`xenon/tools/xstage`) are marked **executable** in Git. Extracting the `.7z` on Windows loses that
permission, and without it the build fails on Linux with "Permission denied".

Check: of the 2729 files, 2718 are stored byte for byte identical to the `.7z`. The other 11
(`build.cmd`, `clean.cmd`, `run.cmd` and 8 `JSG*` files in `decomp/libs/JSystem`) have the same
contents, but Git stores them with LF line endings because of the original project's own
`.gitattributes` (`*.cmd text eol=crlf`, and `* text=auto` in `decomp/`). When cloned, the `.cmd`
files get CRLF again.

## How `sms_language.py` works

The European disc keeps each language in separate files (`game_6_sp.szs`, `message_sp.bmg`, ...), and
the Brazilian Portuguese translation puts Portuguese in place of Spanish (`_sp`). The script:

1. Copies each stage's dialogue to `scene/<stage>.szs/map/message.bmg`.
2. Copies the system messages (`cmn2d_<language>.szs`) into `common.szs/2d/`.
3. Copies the disc's font (with á, ã, ç, ê...) into `nintendo.szs`.
4. Rebuilds `game_6`, `guide`, `select`, `endsave`, `subtitle` and `option.szs` by merging the North
   American archive with the language's. The port's pack system can only replace files inside a
   `.szs`, not add new ones, and the PAL versions have extra files (such as the letters of "¡BIEN!").
5. Detects on its own whether the Spanish slot holds Portuguese (the Brazilian disc) and writes `pt`
   instead of `es`.

The RARC archive rebuilder was checked by rebuilding, byte for byte, all 122 `.szs` archives of the
North American disc and all 147 of the European one.
