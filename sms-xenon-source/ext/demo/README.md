# ext/demo: the demo pilot

Plays Mario from a script, the way a player holds the controller, for trailers and other footage.
It steers with the analog stick toward points in the stage, looking every frame at where Mario is and where the camera faces, so runs curve, ease off and settle as a player's do; it presses buttons for jumps, dives and FLUDD.
Nothing changes unless `SMS_DEMO_PILOT` names a script.

| Variable | What it does |
| --- | --- |
| `SMS_DEMO_PILOT=<file>` | the script to play |
| `SMS_DEMO_LOG=1` | print where Mario is every half second (to find places for a script) |
| `SMS_DEMO_PACE=<fps>` | with `SMS_VI_DETERMINISTIC=1`: while the script plays in a stage, run no faster than this many frames a second of wall time, so several games captured together (multiplayer) keep the same pace |

The pilot takes over only while a stage is being played, so title screens and menus need `SMS_AUTOPRESS` or a player.
It changes the input through the extensions' `padRead` hook, after `PADRead`.

## Scripts

One command a line; `#` starts a comment. Times are seconds of play in the current stage; positions are the stage's X and Z.
Steering goes on in the background while the steps after it run, so a script can steer and jump at once.

| Command | What it does |
| --- | --- |
| `area A` | wait for stage A; its clock starts at 0. When the stage changes under the script (a host took everyone along), it carries on from that stage's `area` |
| `at T`, `wait S` | wait until the stage clock reads T; wait S seconds |
| `go X Z [mag]`, `via X Z [mag]` | run to a point and stop there; run toward a point and keep running (mag: how far the stick is pushed, 0 to 1) |
| `goplayer K [dist] [mag]` | run to multiplayer player K (0 to 3), stopping `dist` short |
| `stop`, `near R`, `arrive` | let go of the stick; wait until the point is within R; wait until a `go` has stopped |
| `face X Z`, `faceplayer K` | turn on the spot toward a point or a player |
| `a [S]`, `b [S]`, `x`, `y` | hold A or B (0.12 s unless given); tap X or Y |
| `r S [level]`, `ron [level]`, `roff` | hold R for S seconds; hold it until `roff` (spray, hover) |
| `land`, `air` | wait until Mario is on the ground, or in the air |
| `spin` | turn the stick in a circle, then jump: a spin jump |
| `cam DX DY S` | hold the C-stick (-1 to 1) for S seconds |
| `players N`, `waitnear K R`, `waitfar K R`, `waitat K X Z R` | multiplayer: wait until N players are in the stage; until player K is within R, or further than R; until player K is within R of a point |
| `stage A E` | leave for area A, episode E |
| `log`, `end` | print where Mario is; stop playing |

A triple jump is `a`, `land`, `a`, `land`, `a` while running.
