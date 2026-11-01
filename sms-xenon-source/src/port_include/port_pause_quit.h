#ifndef PORT_PAUSE_QUIT_H
#define PORT_PAUSE_QUIT_H

// The pause menu's QUIT GAME item (src/pause_quit.cpp), after its own items
// (after EXIT AREA, or after SAVE where there is no EXIT AREA). The pause menu
// lays it out and drives it (patch zzzzz-quit-01).

struct ResTIMG;

// The item's picture: QUIT GAME in the menu's lettering, made from the
// game's own letters (a texture pack's, at its resolution, when it has
// them); with a language pack, that language's QUIT from the file select,
// restyled the same way. *layoutWidth is its width in the disc's pixels,
// which the menu's pictures are laid out in. Each kind is made once and kept
// for the session; null when the disc has neither (and the menu then has no
// such item).
const ResTIMG* PauseQuitArt(int* layoutWidth);
// The item picked: the quit prompt opens over the paused game, as the quit
// key's does, and only its Quit ends the game. Headless, the game ends now.
void PauseQuitRequest();

#endif
