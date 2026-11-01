// Direct warp (ext/warp/README.md): with SMS_WARP=area,episode[,shines] and
// SMS_WARP_DIRECT=1, the game starts in that area's episode with no logos,
// opening movie, title or file select, as a new game, behind a black screen
// until the stage fades in. SMS Launcher's World editor plays a level this way.
//
// SMS_WARP alone still sends a file chosen at file select there instead
// (decomp-patches/port-02-warp-after-file-select.patch). Here the title is
// left as a multiplayer guest leaves it for the host's stage (ext/multiplayer,
// skipTitle): once the title stage has set itself up, setNextStage. The flags
// are a new game's, as TFlagManager's constructor leaves them (firstStart).
#include "port_ext.h"

#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" int port_skip_intro;
extern "C" void GXPC_ExtOverlayScreen(uint32_t rgba) __attribute__((weak));

namespace {

enum { TITLE = 15 };

int s_on = -1;              // SMS_WARP_DIRECT with a usable SMS_WARP
int s_area, s_episode, s_shines = -1;
bool s_sent, s_done;        // setNextStage called; the stage is in (or the warp was given up)
int s_titleFrames;

bool read()
{
	if (s_on >= 0)
		return s_on;
	s_on = 0;
	const char* direct = getenv("SMS_WARP_DIRECT");
	const char* warp   = getenv("SMS_WARP");
	if (!direct || !*direct || !strcmp(direct, "0") || !warp)
		return false;
	if (sscanf(warp, "%d,%d,%d", &s_area, &s_episode, &s_shines) < 1 || s_area < 0 || s_area > 254
	    || s_episode < 0 || s_episode > 254 || s_area == TITLE) {
		fprintf(stderr, "[warp] SMS_WARP_DIRECT: SMS_WARP=%s isn't area,episode[,shines]\n", warp);
		return false;
	}
	s_on = 1;
	// no logos or opening movie (SMS_SKIP_INTRO), from the first frame, before the logos'
	port_skip_intro = 1;
	fprintf(stderr, "[warp] starting in area %d, episode %d\n", s_area, s_episode);
	return true;
}

void cover(bool black)
{
	if (GXPC_ExtOverlayScreen)
		GXPC_ExtOverlayScreen(black ? 0x000000FFu : 0);
}

void frame()
{
	if (!read() || s_done)
		return;
	const int state = gpApplication.mAppState;
	const bool gameplay = state == TApplication::APP_STATE_GAMEPLAY;
	if (gameplay && s_sent && gpApplication.mCurrArea.getStage() != TITLE) {
		// in the stage: its own fade-in from black takes over
		s_done = true;
		cover(false);
		return;
	}
	cover(true); // the boot, logos, opening movie (skipped) and title
	TMarDirector* dir = gameplay ? gpMarDirector : nullptr;
	if (s_sent || !dir || gpApplication.mCurrArea.getStage() != TITLE || dir->mState != TMarDirector::STATE_UNK4
	    || dir->checkFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING)) {
		s_titleFrames = 0;
		return;
	}
	// the title's own setup first (about a second)
	if (++s_titleFrames < 30)
		return;
	TFlagManager* flags = TFlagManager::getInstance();
	if (s_shines >= 0)
		flags->setFlag(0x40000, s_shines);
	// the episode as a world's episode select records it
	flags->setFlag(0x40003, s_episode);
	// the story movies of a first visit as seen (TApplication::checkAdditionalMovie):
	// the airstrip's (1), the plaza's (3, which then asks whether to hear the
	// explanation again; 4) and FLUDD taken at a secret course (5)
	flags->setBool(true, 0x30009);
	flags->setBool(true, 0x3000B);
	flags->setBool(true, 0x3000C);
	flags->setBool(true, 0x3000D);
	dir->setNextStage((u16)(((s_area + 1) << 8) | s_episode), nullptr);
	s_sent = true;
	fprintf(stderr, "[warp] left the title for area %d, episode %d\n", s_area, s_episode);
}

PortExtension s_extension = {
	"warp", frame,
};

struct Register {
	Register() { port_ext_register(&s_extension); }
} s_register;

} // namespace
