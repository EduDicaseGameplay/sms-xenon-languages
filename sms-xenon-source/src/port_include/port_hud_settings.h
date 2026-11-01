#ifndef PORT_HUD_SETTINGS_H
#define PORT_HUD_SETTINGS_H

// The settings menu's HUD page (platform/misc/port_hud_settings.cpp): the game's own HUD hidden
// without a mod, kept in hud.txt in the game's data folder. Plain C, for the game, the pad
// layer and the settings menu alike.

#ifdef __cplusplus
extern "C" {
#endif

// The parts (PORT_HUD_*, port_ext.h) the page hides now: 0 while Hide HUD is off. The game's
// HUD (ext/core/game/port_hud.cpp) asks it every draw.
unsigned port_hud_settings_mask(void);
// The HUD_TOGGLE key (H by default, platform/pad/pad.cpp): flips Hide HUD, and keeps it.
void port_hud_settings_toggle(void);

#ifdef __cplusplus
}
#endif

#endif
