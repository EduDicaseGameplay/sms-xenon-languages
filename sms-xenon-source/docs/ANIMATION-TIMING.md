# High frame rate animation audit

Custom animation rates expressed as frames per native 30 Hz display frame need a `30 / active_fps` conversion when the controller updates once per displayed frame.
The default MActor rates already use `SMSGetAnmFrameRate()` and need no second conversion.
Controllers advanced by the director's 120 Hz movement ticks also need no display-rate conversion.

`framerate-40-native-animation-rates.patch` corrects these overrides:

| Path | Correction |
| --- | --- |
| Petey's `TBossPakkun::changeBck` | Scale the configured goo wind-up rate; animation-driven mouth opening and completion retain native timing. |
| Mecha Bowser's `TLimitKoopa::changeBck` | Scale the supplied rates for idle, turning, fire, damage and recovery animations. |
| Hinokuri's animation selection | Scale both its configured level-zero walk rate and the fixed rate used for other animations. |
| Blooper's beak-damage sequence | Scale the explicitly assigned BTK rate. Frozen BTP controllers stay frozen. |
| Wiggler's walking animation | Scale the minimum rate as well as the already-scaled requested rate, so the clamp cannot restore a native per-frame speed at higher FPS. |
| Script `linSetAnmRate` | Scale explicit BCK and BTP rates supplied by actor scripts. |

The audit reviewed the game's explicit `setRate`/`setFrameRate` writes and their update paths.
Mario and Yoshi's animation updates are called from `TMario::perform`'s movement pass, so their fixed rates retain the 120 Hz simulation clock.
Resume rates copied from existing controllers, zero rates, particle emission rates and rates already derived from `SMSGetAnmFrameRate()` are preserved.
Petey's motion blending also advances on the movement pass and is preserved.
This review covers explicit animation-rate overrides; it is not a claim that every gameplay mechanic has been tested at every frame rate.

## Verification

`tools/framerate/check.sh` checks equal one-second animation advancement at 30, 60 and 120 FPS for normal, frozen and reverse custom rates in both word sizes.

The gameplay fixture selects the normal vomit nerve on the full Petey in Bianco Hills episode 5, reads the configured rate from his loaded parameters and measures actual MActor frame advancement over one simulated second.
It does not inject a playback rate.
With this disc's configured rate of 1.6, the old executable retains 1.6 at 60 and 120 FPS, where the expected rates are 0.8 and 0.4.
The corrected executable advances 47.999989, 47.999973 and 48.000038 animation frames per second at 30, 60 and 120 FPS respectively.

```sh
python3 tools/regress/animation_timing.py --disc /path/to/GMSE01/files
python3 tools/regress/animation_timing.py --fixture duck --fps 60,120 --disc /path/to/GMSE01/files
```

The duck fixture selects the state used after a stomp on a stunned duck and requires its ordinary dizzy emitter to reach the renderer with visible particles at finite positions.
It passed at 60 and 120 FPS on the current code.
The reported missing duck effect was not reproduced; the existing `framerate-36` particle stepping correction remains in place.
The fixture tests the effect path, rather than a full player-input stomp sequence.

## Per-frame passes

In stages, `TMarDirector::direct` runs four, two or one 1/120 s movement ticks per displayed frame at 30, 60 and 120 fps.
Everything driven by those ticks (`CUE_MOVE`, the HUD and menus' moves, `changeState`, nerves and timers) keeps its timing without change.
What runs once per displayed frame is the calc-anim pass (MActor frame updates, model calc with its joint callbacks, `calcRootMatrix`), the pre-entry list (`unk34`), the draw lists, and code on the frame's first or last tick (`graphics->unk0`, `stageLoop`, `RumbleMgr::update`).
A fixed step there runs two or four times as fast at 60 and 120.
An audit of those paths found and fixed:

Before is what each did at 60 and 120 fps; after is what it does at every rate, which is its 30 fps (retail) timing unless noted.
Before values follow from the code (a step taken once per displayed frame, two or four times a second as often); the coin counter and the episode select were also measured.

| Patch | What | Before (30 / 60 / 120 fps) | After (all rates) |
| --- | --- | --- | --- |
| `framerate-46` | Episode select frame rate | 30 / 30 / 30 fps | 30 / 60 / 120 fps (16.7 and 8.3 ms a frame measured) |
| `framerate-46` | Episode select timing, had it simply run faster | scroll between episodes 0.33 / 0.17 / 0.08 s; background colour phase 4.3 / 2.1 / 1.1 s; shine spin 180 / 180 / 120 °/s, stopping turned away at 120; music fade after choosing 1 / 2 / 4 s | 0.33 s, 4.3 s, 180 °/s stopping facing front, 1 s (its other fades, pulses, slides and the close delay keep their 30 fps length too) |
| `framerate-47` | HUD coin counter roll (and the jet-balloon counter) | 30 coins in about 1.6 / 0.8 / 0.4 s, a sparkle per coin as fast | about 1.6 s (measured: reaches 30 at the same game field at all rates) |
| `framerate-47` | Water tank flash at full pressure | 25-step pulse every 0.83 / 0.42 / 0.21 s | 0.83 s |
| `framerate-48` | Regrowing goop | stamped back 1x / 2x / 4x as often | retail rate |
| `framerate-49` | MareFalls and seagull calls | a call every 3.4-12.8 / 1.7-6.4 / 0.85-3.2 s | 3.4-12.8 s |
| `framerate-50` | Mario's jiggle after a hard landing or shock (and his cap, Shadow Mario, Bianco root plants) | buzz 7.5 / 15 / 30 Hz, shock lasting up to 20 / 10 / 5 s and dying out as much faster | 7.5 Hz, up to 20 s |
| `framerate-51` | FLUDD splash sprites | live 0.53 / 0.27 / 0.13 s; rise about 4 / 22 / 60+ units (at 120 still rising when gone) | 0.53 s, about 4-6 units |
| `framerate-52` | Piantas' and Peach's hip sway | spring stepped 30 / 60 / 120 times a second: stiffer, quicker wobble | 30 steps a second |
| `framerate-53` | Gatekeeper head turn toward Mario | up to 90 / 180 / 360 °/s | 90 °/s |
| `framerate-53` | Petey's head turn | up to 30 / 60 / 120 °/s | 30 °/s |
| `framerate-53` | Phantamanta's wobble after a hit | 1.67 / 0.83 / 0.42 s | 1.67 s |
| `framerate-53` | Hinokuri 2's animation blend | 0.67 / 0.33 / 0.17 s | 0.67 s |
| `framerate-53` | King Boo's spicy-hit sparks; slot spin sound | 30 / 60 / 120 emitters a second; spin sound 15 / 30 / 60 times a second | 30 a second; 15 a second |
| `framerate-54` | Distance music cross-fade ramp as a stage starts (260 frames, 340 on stage 4, 90 in Pianta Village) | 8.7 / 4.3 / 2.2 s (default) | 8.7 s |
| `framerate-55` | Rumble motor rest after continuous rumble; hold | after 30 / 15 / 7.5 s; hold 2.7 / 1.3 / 0.7 s | 30 s; 2.7 s |
| `framerate-56` | 1-Up's blink before it vanishes | 10 Hz (2 frames on, 1 off) / 20 Hz / 20 Hz half on | 10 Hz, 2 on 1 off |

Port code (not retail, so the target is the look it was made at, 60 fps, or the stated time):

| File | What | Before (30 / 60 / 120 fps) | After (all rates) |
| --- | --- | --- | --- |
| `src/settings_menu.cpp`, `src/savestate_menu.cpp` | Selected row pulse; list scroll (stepped per tick, but scaled as if per frame, so slower at higher rates) | pulse 3.8 / 1.9 / 0.95 Hz; scroll ease 2x faster / as made / 2x slower | 1.9 Hz; as made at 60 |
| `src/radar.cpp` | Full map fade; radar fade; zoom label | 0.22 / 0.11 / 0.06 s; 0.27 / 0.13 / 0.07 s; 2.5 / 1.25 / 0.63 s | 0.11 s; 0.13 s; 1.25 s |
| `ext/multiplayer/game/mp_server.cpp` | Race "Go!" against the server's start | 1 s late / on time / 0.5 s early | on time ("Go!" is always 1 s after "Ready") |
| `ext/multiplayer/game/mp_game.cpp` | Notice line; player status | 5 / 2.5 / 1.25 s; 10 / 5 / 2.5 s | 5 s; 10 s |
| `ext/modapi/host/mod_menu.cpp` | Mod menu stick repeat | first after 0.7 / 0.35 / 0.18 s, then 6 / 12 / 24 a second | 0.7 s, 6 a second |

At 30 fps the retail patches change nothing. The port menus, radar and multiplayer timings do change at 30, toward their 60 fps look.

Checked and left as they are: MActor and J3D playback (rates from `SMSGetAnmFrameRate`), NPC motion blends (`CLBPalFrame`), Mario and Yoshi (movement ticks), stateless joint callbacks and draws, keep-alive particle requests, one-shot emits gated by animation events, the title, file select and options (stage 15, all on movement ticks).
Measured: with 30 coins added in Delfino Plaza the HUD counter reaches 30 at the same game field at 30, 60 and 120 fps.

## GO! letters

As the stage-start GO! letters fly apart (`TConsoleStr::processGo`), each movement tick resizes a letter to its *global* bounds plus two.
The global bounds only change when a frame is drawn: once in four ticks at 30 fps, every tick at 120.
So at 60 and 120 fps the letters grew two or four times as fast, and kept the excess after the shrink at the end of the effect.
Every later GO! was then drawn too big, such as the READY/GO of Eclipse's races (measured: 316 pixels wide at 120 fps, 78 at 30).
`zzzz-rate-01` grows them once per native frame (`port_native_frame_step`). It applies after the `zzzz-lang` patches that rename those panes.

