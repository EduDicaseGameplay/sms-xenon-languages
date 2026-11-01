# ext/outfit: Mario's Shine shirt and sunglasses

Mario wears his Shine shirt, his sunglasses, or both, in every stage from the start.
SMS Launcher turns each on from Customize Mario (Always wear the Shine shirt, Always wear sunglasses), while Use in the game is on.

| Variable | What it does |
| --- | --- |
| `SMS_MARIO_SHIRT=1` | Mario always wears the Shine shirt |
| `SMS_MARIO_SUNGLASSES=1` | Mario always wears his sunglasses |

The two are separate: the shirt doesn't need the sunglasses, as it does in the game.

## How

In the game, Mario gets both at the sunglasses stand (`evChangeSunglass`): the sunglasses, and with them the shirt once he has Shine 0x77.

- The shirt is `MARIO_FLAG_HAS_SHIRT`. `TMario::thinkAloha` shows or hides the body model's shape 10 from it every tick.
- The sunglasses are `TMarioCap`'s `E_CAP_MODEL_SUNGLASSES`, `ma_glass1.bmd` on Mario's `M_head` joint.

Every tick of a stage (`afterMovement`, and `stageSetup` for its first), the extension sets both again on `gpMarioOriginal`.
That way taking them off at the stand, or a stage's new `TMario`, leaves them on.
For the shirt it calls `thinkAloha` again, since the movement pass has already run it this tick.

## Super Mario Eclipse

It works the same in Eclipse, for every character. Luigi, Piantissimo and Shadow Mario each have their own `ma_glass1.bmd` and a shirt as shape 10 of their body model, as Mario does.
Eclipse has a setting of its own, Persistent Shades (on by default). It keeps the sunglasses on across stages once Mario has bought them, and the shirt with them after Shine 119. To remember the purchase it uses save flag `0x3001B`, which this extension leaves alone. Turning the switches off later brings back what Eclipse would do.
Eclipse already keeps the sunglasses from darkening the screen.
Each character wears its own shirt (Luigi's is blue with mushrooms). The launcher's Shine shirt colour repaints only Mario's, as its other colours do.

## Limits

Only Mario wears them. The darker screen of the stand's sunglasses (`TSunGlass`) is still only the stand's.
In multiplayer the other players don't see them yet: a player's pose carries the cap, not the shirt or the sunglasses.
