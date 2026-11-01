# ext/autosave: auto save

Saves by itself where the game would ask the player to save: after a Shine, a blue coin, a nozzle's box opened for good, and Yoshi hatched.
The stage goes on without the prompt. While it saves, a Shine turns at the bottom left of the game's picture. Then it stops beside "Saved", the save sound from the game's own prompt plays, and the panel fades out.
It's off unless `SMS_AUTO_SAVE=1` (`auto_save = on` in settings.txt). SMS Launcher turns it on by default (Settings → Gameplay → Auto Save).

| Variable | What it does |
| --- | --- |
| `SMS_AUTO_SAVE=1` | save without asking (off by default: the game asks, as it always has) |

## How it saves

It saves the way the prompt's Save does (`TCardSave`). First it reads the card's slots. Then it writes the file's slot (`TFlagManager::save`), marks the flags as saved (`saveSuccess`), and writes the options block.
It saves only over the file's own last save or an empty slot.
Anything the prompt has to ask the player about is left to the prompt, which opens as usual: no card, no save file on the card yet, a slot another game has saved over since, or a damaged file.

The card's commands run on its own thread, which has a higher priority than the game's (`TCardManager`, priority 14), so each command is done by the time the call that asks for it returns. A save takes a moment of one frame, and its outcome is known before the game decides whether to open the prompt.
If the card is ever still busy, the save carries on over the next frames. If it then fails, the panel says the game couldn't save, and auto save stays off until the game restarts, so the next prompt asks.

A game over's prompt (Continue or quit) still asks, and so does the pause menu's Save.

## Hooks

- `savePrompt` (patch `zzzzz-ext-09-save-prompt`): the stage is about to open the prompt. Returning 1 leaves it out. For a nozzle the game still plays the jingle the prompt opens with and shows the help balloon it shows on closing.
- `frame`: finishes a save the card was still busy with, and shows "Saved" with the sound once the Shine has turned for 0.9 s.

The indicator is sms_gx's `GXPC_AutoSaveIndicator` ([gx_autosave_overlay.cpp](../../platform/gx/src/gx_autosave_overlay.cpp)). Only a window shows it, not `SMS_SHOTS` captures.

## Super Mario Eclipse

Eclipse's engine, BetterSunshineEngine, has its own auto save: its Save Prompts setting (ALL, NO BLUE, NONE, AUTO SAVE; AUTO SAVE by default), with its own spinning icon.
With BetterSunshineEngine loaded (its `checkBootOut` patch registered), this extension leaves every prompt to it.
In the 64-bit port that setting used to read the prompt's reason from the wrong byte, so Eclipse asked anyway. See [ECLIPSE.md](../../docs/ECLIPSE.md) (Save prompts).
