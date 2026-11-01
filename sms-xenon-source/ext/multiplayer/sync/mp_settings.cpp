// The multiplayer host's options in the game's settings menu (port_settings.h,
// on the Gameplay page while hosting): Player Collision, the host's word on
// whether players collide (mpg_collision; a server mod may override it).
// Started from SMS Launcher, the menu's change is saved as `mpCollision`
// for the launcher's next host (SMS_MULTIPLAYER_COLLISION).
#include "port_settings.h"

#include "../mp_game_api.h"

namespace {

int hosting() { return mpg_role() == MPG_ROLE_HOST; }
int offOnCount() { return 2; }
const char* offOnName(int i) { return i ? "On" : "Off"; }
int collisionGet() { return mpg_collision_setting() ? 1 : 0; }
int collisionSet(int v) { return mpg_set_collision_setting(v != 0); }

const PortSetting kSettings[] = {
	{"mpCollision", PORT_SETTINGS_PAGE_GAMEPLAY, 60, "Player Collision",
	 "Hosting: players bump into each other and bounce off heads, in every game.", 0, hosting, offOnCount,
	 offOnName, collisionGet, collisionSet},
};

} // namespace

extern "C" const PortSetting* mp_port_settings(int* count)
{
	*count = (int)(sizeof kSettings / sizeof kSettings[0]);
	return kSettings;
}
