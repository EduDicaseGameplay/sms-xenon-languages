Super Mario Sunshine for the Xbox 360
=====================================

A native Xbox 360 build of the Super Mario Sunshine PC port (decompiled game code, its own
renderer for the 360's GPU). It contains no Nintendo data: the game's files come from your own
disc, through sms_extract.py.

You need
- an Xbox 360 that runs unsigned code (homebrew), with a hard drive or USB storage;
- a dump of your Super Mario Sunshine disc, USA version (GMSE01), as .iso, .gcm or .ciso
  (Dolphin converts .rvz, .gcz and .wia: right-click the game, Convert File);
- Python 3 on a computer.

Install
1. python3 sms_extract.py <your disc image> <a folder>
   This writes files\ and sys\ into the folder (about 1.2 GB) and checks that the image is
   GMSE01.
2. Make a folder on the console's storage, for example Hdd1:\Games\Sunshine, and copy into it
   default.xex, sms_env.txt, the data folder, and the files and sys folders.
   (A GMSE01.iso in place of files and sys works too.)
3. Start default.xex from your dashboard or file manager.

Controls
The Xbox 360 controller in the GameCube's layout: A, B, X, Y as on the GameCube; RB is Z;
LT and RT are L and R (a full press clicks); the left stick moves, the right stick turns
the camera; Back opens the map.

Saves
The memory card is data\sms-port\card-a in the game's folder. Keep that folder when you copy a
new version over the old one. data\shaders holds the shaders made ready for the game.

Options
sms_env.txt lists them: frame rate, widescreen, internal resolution, volume, rumble, camera,
intro. The picture is 720p and 16:9 by default; SMS_WIDESCREEN=0 and SMS_XE_SCALE=1 give the
GameCube's own 640x448 at 4:3.

Problems
The game writes sms.log in its folder. Attach it to a report.
