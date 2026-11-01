// Set by the F9 cheat menu (platform/gx), read by the decomp (decomp-patches/port-03 to port-08, port-12 and port-13).
// Movement speed, jump height and coin multiplier are factors, low gravity a divisor: 1 is off.
extern "C" {
int port_cheat_god_mode = 0;
int port_cheat_movement_speed = 1;
int port_cheat_jump_height = 1;
int port_cheat_infinite_lives = 0;
int port_cheat_infinite_fludd = 0;
int port_cheat_infinite_air = 0;
int port_cheat_low_gravity = 1;
int port_cheat_coin_multiplier = 1;
// Counts warps (the warp menu) and save state loads: play after one doesn't earn achievements (ext/achievements).
int port_play_unchecked = 0;
}
