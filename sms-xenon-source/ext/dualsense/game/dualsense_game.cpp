// DualSense FLUDD trigger (ext/dualsense/README.md): tells platform/pad when
// R2 is FLUDD's trigger, so that it can make a DualSense's R2 harder to pull
// then (SMS_FLUDD_TRIGGER), and let it go otherwise.
//
// That is while Mario has FLUDD and his stage is being played: a tick of it
// moved (afterMovement), which a movie, the title and file select don't, with
// the director playing it (state 4) rather than in the pause menu (5), under
// which ticks still run. A few frames without one let the trigger go, so a
// frame drawn between two ticks doesn't.
#include "port_ext.h"

#include <Player/Mario.hpp>
#include <Player/MarioFlags.hpp>
#include <System/MarDirector.hpp>

extern "C" int port_fludd_trigger_ready;

namespace {

const int kHoldFrames = 10;
int s_framesSinceTick = kHoldFrames;
bool s_hasFludd;

void tick()
{
	TMario* mario     = gpMarioOriginal;
	s_hasFludd        = mario && mario->checkFlag(MARIO_FLAG_HAS_FLUDD) && gpMarDirector
	             && gpMarDirector->mState == TMarDirector::STATE_UNK4;
	s_framesSinceTick = 0;
}

void frame()
{
	port_fludd_trigger_ready = s_hasFludd && s_framesSinceTick < kHoldFrames;
	if (s_framesSinceTick < kHoldFrames)
		s_framesSinceTick++;
}

PortExtension s_extension = {
	"dualsense", frame, tick,
};

struct Register {
	Register() { port_ext_register(&s_extension); }
} s_register;

} // namespace
