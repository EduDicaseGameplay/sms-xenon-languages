// Pipeline cache saves (ext/pipelinecache/README.md): the Vulkan renderer's
// pipeline cache is saved at moments when the few milliseconds it takes go
// unseen, not in play: when the pause menu, the map or a save screen opens,
// and when a stage starts loading (sms_gx's GXPC_SavePipelineCacheSoon, which
// saves only what is new since the last save). It is saved at shutdown too.
#include "port_ext.h"

#include <System/Application.hpp>
#include <System/MarDirector.hpp>

extern "C" void GXPC_SavePipelineCacheSoon(void) __attribute__((weak));

namespace {

bool s_stageReady; // gpMarDirector is the stage's, set up
bool s_paused;

void save()
{
	if (GXPC_SavePipelineCacheSoon)
		GXPC_SavePipelineCacheSoon();
}

// The pause menu, the map and the save screens (as the mod API's SMS_EV_PAUSE)
bool paused()
{
	if (!s_stageReady || !gpMarDirector)
		return false;
	switch (gpMarDirector->mState) {
	case TMarDirector::STATE_PAUSE_MENU:
	case TMarDirector::STATE_CARD_SAVE:
	case TMarDirector::STATE_UNK12:
	case TMarDirector::STATE_GUIDE:
		return true;
	default:
		return false;
	}
}

void frame()
{
	if (gpApplication.mAppState != TApplication::APP_STATE_GAMEPLAY)
		s_stageReady = false;
	const bool now = paused();
	if (now && !s_paused)
		save();
	s_paused = now;
}

void stageSetup()
{
	s_stageReady = true;
	s_paused     = false;
}

void moveStage() { s_stageReady = false; }

void stageLoading(int, int) { save(); }

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name         = "pipelinecache";
		s_extension.frame        = frame;
		s_extension.stageSetup   = stageSetup;
		s_extension.moveStage    = moveStage;
		s_extension.stageLoading = stageLoading;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
