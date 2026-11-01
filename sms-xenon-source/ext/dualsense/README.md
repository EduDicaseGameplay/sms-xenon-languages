# ext/dualsense: the DualSense FLUDD trigger

On a DualSense (PlayStation 5) controller, R2 gets harder to pull along its travel while it is FLUDD's trigger, with the most resistance at the end of the pull, so a light pull for spraying on the move feels apart from a full one.
Off by default; SMS Launcher sets it from Settings → Controls, and the game's settings menu has it under Controls (DualSense FLUDD Trigger).

| Variable | What it does |
| --- | --- |
| `SMS_FLUDD_TRIGGER=n` | the resistance at the end of R2's pull, in percent of the controller's strongest (25, 50, 75, 100); 0 or unset is off |

Only the force on the trigger changes: R2 reads the same, so the game plays the same, and other controllers are left alone.

## How

- This extension sets `port_fludd_trigger_ready` (platform/pad) while Mario has FLUDD (`MARIO_FLAG_HAS_FLUDD` on `gpMarioOriginal`) and his stage is running, from its `afterMovement` and `frame` hooks: a stage tick moved with the director playing (state 4). Movies, the title and file select run no movement ticks, and the pause menu (state 5) isn't playing, so they let the trigger go.
- `PADRead` asks for the resistance while that is so, the window has focus, no menu has the input, and R is bound to R2 (`PAD_RT`, the default); otherwise it asks for none.
- The GX layer (`sms_gx_fludd_trigger`) sends the DualSense's adaptive trigger effect through `SDL_GameControllerSendEffect` to the controller in use, only when the level or the controller changes, and none at exit or to a controller put down. The effect is "feedback" (0x21): a resistance for each tenth of the travel, rising to the chosen strength at the end.

A crash leaves the trigger as it was until the controller is turned off or plugged in again.
