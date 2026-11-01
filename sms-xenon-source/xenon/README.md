# sms-xenon

Super Mario Sunshine for the Xbox 360, built on chasem-dev/sms-pc-port (this tree's `main` is upstream; our work
is on `xenon`). The decomp (`decomp/`) and the mod SDK (`sdk/`) are private mirrors on GitVerse, branch `xenon`.

## Reference frames (`xenon/tools/`)

GameCube frames come from Dolphin 2512 (Software Renderer) built with two local patches: `dolphin-emu-nogui`
plays `-m movie.dtm`, and logs its poll count. The same `.dtm` drives both Dolphin and the port.

| Tool | Use |
| --- | --- |
| `make_dtm.py OUT.dtm LAST_FIELD 'a@500+6,stick_left@1700,...'` | input movie (SMS_AUTOPRESS syntax, fields in retail numbering); `--test` self-checks |
| `portrun.sh WORK MOVIE.dtm FIELDS [SECONDS]` | deterministic headless port run with that movie and a fresh card; PNG captures in `WORK/shots` |
| `refmatch.py PORT_DIR DOLPHIN_FRAMES OUT` | pairs each port capture with the closest Dolphin frame dump, prints the difference |
| `rarc_at.py FILE.szs HEXOFFSET [OUT]` | which file of a Yaz0 RARC holds an offset (find what a MEM1 address was loaded from) |
| `tex1_list.py FILE.bmd` | the TEX1 textures of a J3D file: format, size, mips, data offset |

Game data never goes into this repository.

## Console bench (`xenon/tools/bench/`)

`sms_env.bench.txt` (as the console's `sms_env.txt`) and `bench.txt` beside `default.xex`, with `plaza.dtm` and
`tour.dtm` from the xregress references: one launch plays every run of `bench.txt` (the xregress movies, at 30 fps,
deterministic clock), relaunching itself between runs, and appends each run's frame times (mean, p50, p95, p99,
max: frame, game, GX core, render, GPU waits; GPU reads and write-backs per frame) to `bench_results.txt`. The runs
use their own card (`bench\card-a`), never the player's. See `xenon/platform/xe_bench.cpp`.
