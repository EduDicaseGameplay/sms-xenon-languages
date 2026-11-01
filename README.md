# Super Mario Sunshine on Xbox 360, in Portuguese and other languages

**English** | [Português (Brasil)](README.pt-BR.md)

This repository is the **Super Mario Sunshine port for the Xbox 360** (made by other people, see
[credits](#credits)) with one addition of mine: playing in **Brazilian Portuguese, Spanish, French,
German and Italian**, using your own disc images.

> **I did not make the port.** The decompilation, the PC port and the Xbox 360 port are the work of the
> authors listed in the [credits](#credits). What this repository adds is the ability to build the
> language packs from the European disc and from the Brazilian Portuguese translation, a build fix,
> and this step-by-step guide. The exact list of files I created or changed is in [CHANGES.md](CHANGES.md).

> **No game data is included.** Super Mario Sunshine belongs to Nintendo. You need your own copies of
> the game. Disc images, extracted files and generated packs must **never** be committed to the
> repository (the [.gitignore](.gitignore) already blocks them).

---

## How it works

The port only runs on the **North American disc** (GMSE01), which is the game's base. The other
languages come from other discs:

| Disc image | Used for |
|---|---|
| **Super Mario Sunshine (USA)** — GMSE01 | The game's base (required) |
| **Super Mario Sunshine (Europe)** — GMSP01 | Builds the `es`, `fr`, `de` and `it` packs |
| **Brazilian Portuguese translation** (built on the European disc, also GMSP01) | Builds the `pt` pack |

[`sms_language.py`](sms-xenon-source/xenon/dist/sms_language.py) reads the North American disc and the
European (or Brazilian) one and writes `mods/languages/<code>`. The port already knows how to load that
folder. When the game starts, a **language menu** (the console's own message box) lists English and every
installed pack, so you pick the language with the controller; no file to edit.

---

## Folder layout used in this guide

```
Ubuntu (WSL):
~/sms-xenon-languages/            <- this repository (a copy INSIDE Ubuntu)
├── sms-xenon-source/             <- the port's source code
└── build-xenon/stage/            <- created by the build

Windows:
C:\SMS\isos\                      <- put your disc images here
C:\SMS\jogo-xbox\                 <- created in step 14: the folder that goes to the Xbox
C:\work\sms-xenon\stage\          <- created in step 9: where Visual Studio compiles
```

> The build always runs from a copy **inside Ubuntu** (`~/...`), never from `/mnt/c`. You can get the
> code by cloning it in Ubuntu, or by downloading the ZIP / cloning on Windows and copying the folder
> into Ubuntu (step 6). Both ways were tested and give the same files.

## Requirements

- Windows 10/11 with **Visual Studio 2010** and the **Xbox 360 SDK (XDK)** installed (not provided here).
- A modded Xbox 360 that runs homebrew, with a hard drive or USB storage.
- Your disc images: the **North American** one (required), plus the **European** and/or **Brazilian Portuguese** one.

> **Just want to play, without building?** The `default.xex` from the port's official package
> (sms-xenon-f233c15) also reads language packs, but it has **no language menu**: with it, put
> `SMS_LANGUAGE=pt` (or `es`, `fr`, `de`, `it`) in `sms_env.txt`. Skip parts 1 to 3 and go to [part 4](#part-4-build-the-game-folder-with-the-languages-in-ubuntu).

---

## Part 1: Install Ubuntu on Windows (once)

**1.** Open **PowerShell as Administrator** (Start → "PowerShell" → right-click → *Run as administrator*):
```powershell
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
```

**2.** Restart the computer.

**3.** In PowerShell as Administrator:
```powershell
wsl --install -d Ubuntu-22.04
```
> If you get a **virtualization** error: enable *Intel VT-x* (Intel) or *SVM* (AMD) in the BIOS and try
> again, **or** run `wsl --set-default-version 1` and try again (WSL1, no BIOS change needed).

**4.** Open **Ubuntu** from the Start menu and create a user name and password.

---

## Part 2: Prepare the code (in Ubuntu)

**5.** Install the tools (once). **Both** `apt install` lines are required:
```bash
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install -y git cmake make python3 patch rsync binutils gcc-multilib g++-multilib libsdl2-dev libegl-dev libgl-dev
sudo apt install -y libsdl2-2.0-0:i386 libgl1:i386 libegl1:i386 libgl1-mesa-dri:i386 libegl-mesa0:i386
```

**6.** Get the code into Ubuntu. Pick **one** way:

- **Clone in Ubuntu:**
  ```bash
  git clone https://github.com/EduDicaseGameplay/sms-xenon-languages.git ~/sms-xenon-languages
  ```
- **Or download it on Windows** (*Code → Download ZIP* on GitHub, then extract it, for example to
  `C:\SMS\sms-xenon-languages-main`), and copy it into Ubuntu:
  ```bash
  cp -r /mnt/c/SMS/sms-xenon-languages-main ~/sms-xenon-languages
  ```

Then:
```bash
cd ~/sms-xenon-languages/sms-xenon-source
```

**7.** Apply the patches and build the Linux version. The Linux build is **required**: the Xbox build reads its object files.
```bash
cmake -S . -B build/linux-32 -DSMS_ARCH=32 -DSMS_GX_BUILD_TESTS=OFF -DSMS_BUNDLE_DISC=
cmake --build build/linux-32 --target sms --parallel $(nproc)
```
The first command must end with `-- Generating done`, the second with `[100%] Built target sms`.

**8.** Stage the Xbox build and write the Visual Studio project:
```bash
rm -rf ../build-xenon/stage
xenon/tools/xstage --no-sync
python3 xenon/tools/xproj.py . ../build-xenon/stage
```
`xstage` must finish without a `Traceback`; one of its last lines is `xstubs: N stubs dropped`.

**9.** Copy the stage to Windows:
```bash
mkdir -p /mnt/c/work/sms-xenon
rsync -rlt --delete --exclude=/xenon/out/ ~/sms-xenon-languages/build-xenon/stage/ /mnt/c/work/sms-xenon/stage/
```

---

## Part 3: Compile (in Windows PowerShell)

**10.** Open a **normal PowerShell** (not Ubuntu; the prompt starts with `PS C:\`):
```powershell
& C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe C:\work\sms-xenon\stage\xenon\sms.vcxproj /m /t:Rebuild /p:Configuration=Master "/p:Platform=Xbox 360"
```
It must end with **Build succeeded** (`C4355` warnings are normal). The result is:
`C:\work\sms-xenon\stage\xenon\out\Master\default.xex`

---

## Part 4: Build the game folder with the languages (in Ubuntu)

Put your disc images in `C:\SMS\isos\` (in Ubuntu that folder is `/mnt/c/SMS/isos/`).

**11.** Go to the source folder:
```bash
cd ~/sms-xenon-languages/sms-xenon-source
mkdir -p /mnt/c/SMS/jogo-xbox
```

**12.** The game's base, from the North American disc. Pick **one** option:

- **Option A (simplest):** copy the North American disc image as `GMSE01.iso`:
  ```bash
  cp "/mnt/c/SMS/isos/Super Mario Sunshine (USA).iso" /mnt/c/SMS/jogo-xbox/GMSE01.iso
  ```
- **Option B:** extract its files (creates `files/` and `sys/`):
  ```bash
  python3 xenon/dist/sms_extract.py "/mnt/c/SMS/isos/Super Mario Sunshine (USA).iso" /mnt/c/SMS/jogo-xbox
  ```

> Never rename the European or the Brazilian disc image to `GMSE01.iso`: the port's code is the North American version's.

**13.** Build the language packs:

Portuguese (from the Brazilian disc), about 1 minute:
```bash
python3 xenon/dist/sms_language.py "/mnt/c/SMS/isos/Super Mario Sunshine (USA).iso" "/mnt/c/SMS/isos/Mario Sunshine (BR).iso" /mnt/c/SMS/jogo-xbox
```
Spanish, French, German and Italian (from the European disc), about 3 minutes:
```bash
python3 xenon/dist/sms_language.py "/mnt/c/SMS/isos/Super Mario Sunshine (USA).iso" "/mnt/c/SMS/isos/Super Mario Sunshine (Europe).iso" /mnt/c/SMS/jogo-xbox
```
(Replace the file names with your own.)

**14.** Copy the executable and the options file:
```bash
cp /mnt/c/work/sms-xenon/stage/xenon/out/Master/default.xex /mnt/c/SMS/jogo-xbox/
cp xenon/dist/sms_env.txt /mnt/c/SMS/jogo-xbox/
```
Leave the `SMS_LANGUAGE` line in `sms_env.txt` commented out (`# SMS_LANGUAGE=pt`): the game then shows
the language menu every time it starts. If you always want the same language, remove the `#` and set the
code (`pt`, `es`, `fr`, `de` or `it`): the game starts in it without asking.

The folder ends up like this:
```
C:\SMS\jogo-xbox\
├── default.xex
├── sms_env.txt
├── GMSE01.iso           (option A)  or  files\ + sys\  (option B)
└── mods\languages\      (pt  es  fr  de  it)
```

---

## Part 5: Play on the Xbox

**15.** Copy **everything** in `C:\SMS\jogo-xbox` to the console (FTP with Aurora/FSD, or a USB drive),
for example to `Hdd1:\Games\Sunshine\`.

**16.** Start `default.xex` from Aurora. After about two seconds of black screen, the language menu appears:

```
Super Mario Sunshine
Escolha o idioma / Choose your language / Elige el idioma

  [ English ]  [ Português (Brasil) ]  [ Mais / More / Más > ]
```

The console's message box holds three buttons at most, so the languages come two per page, with
**More** for the next ones. **B** goes back to the first page; on the first page it closes the menu and
the game starts in English.

If the game starts in English when it should not, open `sms.log` in the game's folder:
- `language menu: ...` lines → what the menu did (the button chosen, or why it closed);
- `no pack in mods/languages/pt` → the `mods` folder is not next to `default.xex`;
- `language pt: ... replaced` → the pack was loaded.

---

## Known limitations

- **Some screens or texts may stay in English.** The packs cover dialogue, menus, the pause guide, the
  shine select, the save screen, movie subtitles and the title screen, but not everything the European
  release changes has been mapped yet.
- **The North American disc is always required**: the European and Brazilian discs only provide the text.
- Movie subtitles come from the PAL disc and may be slightly out of sync.

Found a text that was not translated? Open an issue saying which screen it is on.

---

## Credits

| Who | What | License |
|---|---|---|
| **Nintendo** | Super Mario Sunshine (the game and all the data on its disc) | — (not included) |
| **18woldemar** | The Xbox 360 port: renderer for the 360's GPU, platform layer, audio, input and build tools (`xenon/`) | MIT |
| **SoftwareAura LLC / [chasem-dev](https://github.com/chasem-dev)** | The [Super Mario Sunshine PC port](https://github.com/chasem-dev/sms-pc-port) the Xbox port is built on, and the [mod SDK](https://github.com/chasem-dev/sms-mod-sdk) | MIT |
| **[chasem-dev/sms-english](https://github.com/chasem-dev/sms-english) and contributors** | The Super Mario Sunshine decompilation (`decomp/`) | CC0 1.0 |
| **The Brazilian Portuguese translation's authors** | The Portuguese translation the `pt` pack is built from (not included) | — |
| **EduDicaseGameplay** (this repository) | `sms_language.py`, the `xstubs.py` fix, the language documentation and this guide; see [CHANGES.md](CHANGES.md) | MIT |

The original code comes from the `sms-xenon-source-f233c15` package (sms-pc-port, branch `xenon`,
commit `f233c150b8407a56c88de794887c3d9726f9ccef`). This repository's first commit is that code with
**no changes**; my changes are in the commits after it, so you can see exactly what changed with:
```bash
git diff 26a14ce HEAD -- sms-xenon-source
```
The original license notices are kept in [sms-xenon-source/LICENSE](sms-xenon-source/LICENSE),
[sms-xenon-source/xenon/dist/LICENSE.txt](sms-xenon-source/xenon/dist/LICENSE.txt),
[sms-xenon-source/decomp/LICENSE](sms-xenon-source/decomp/LICENSE) and
[sms-xenon-source/sdk/LICENSE](sms-xenon-source/sdk/LICENSE).
