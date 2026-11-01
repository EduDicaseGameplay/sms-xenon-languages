// Game symbols that only code mods or dead GCC paths define on the PC (weak
// imports there); MSVC keeps their references, so they need a definition.
#include <Player/MarioAnimeData.hpp>
#include <dolphin/types.h>

// BetterSunshineEngine's animation table (MarioUpper.cpp reads it only when a mod is active)
TMarioAnimeData sPlayerAnimeDatas[1];
// retail's DSP microcode image (dsptask.c); the port's DSP is high-level emulation
u8 lbl_80004600[1];
