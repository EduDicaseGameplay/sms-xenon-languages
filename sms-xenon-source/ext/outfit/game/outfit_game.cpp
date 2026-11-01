// Outfit (ext/outfit/README.md): Mario wears his Shine shirt
// (SMS_MARIO_SHIRT=1) and his sunglasses (SMS_MARIO_SUNGLASSES=1) in every
// stage, each on its own, as the launcher's Customize Mario chooses.
//
// The game gives him both only at the sunglasses stand (evChangeSunglass):
// the sunglasses, and the shirt with them once he has Shine 0x77. The shirt
// is MARIO_FLAG_HAS_SHIRT, which TMario::thinkAloha turns into the body
// model's shape 10; the sunglasses are TMarioCap's E_CAP_MODEL_SUNGLASSES
// (ma_glass1.bmd on Mario's M_head). Both are put back every tick, so taking
// them off at the stand or a new stage's fresh TMario leaves them on. Only
// Mario wears them: the stand's darker screen (TSunGlass) stays the stand's.
//
// Super Mario Eclipse sets the same two from its Persistent Shades setting,
// for every character; each has its own ma_glass1.bmd and shirt shape.
#include "port_ext.h"

#include <Player/Mario.hpp>
#include <Player/MarioCap.hpp>
#include <Player/MarioFlags.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

int s_shirt = -1, s_sunglasses = -1;

bool on(const char* name)
{
	const char* v = getenv(name);
	return v && *v && strcmp(v, "0") != 0 && strcmp(v, "off") != 0;
}

void read()
{
	if (s_shirt >= 0)
		return;
	s_shirt      = on("SMS_MARIO_SHIRT");
	s_sunglasses = on("SMS_MARIO_SUNGLASSES");
	if (s_shirt || s_sunglasses)
		fprintf(stderr, "[outfit] Mario always wears%s%s%s\n", s_shirt ? " his Shine shirt" : "",
		        s_shirt && s_sunglasses ? " and" : "", s_sunglasses ? " his sunglasses" : "");
}

void dress()
{
	read();
	TMario* mario = gpMarioOriginal;
	if (!mario || (!s_shirt && !s_sunglasses))
		return;
	// thinkAloha has shown or hidden the shirt in this tick's movement already:
	// show it again now, before Mario is posed and drawn.
	if (s_shirt && !mario->checkFlag(MARIO_FLAG_HAS_SHIRT)) {
		mario->onFlag(MARIO_FLAG_HAS_SHIRT);
		if (mario->mModel)
			mario->thinkAloha();
	}
	// TMarioCap draws the sunglasses from its calc-anim cue on, which comes after this.
	if (s_sunglasses && mario->mCap)
		mario->mCap->setModelActive(TMarioCap::E_CAP_MODEL_SUNGLASSES);
}

PortExtension s_extension = {
	"outfit", 0, dress, dress,
};

struct Register {
	Register() { port_ext_register(&s_extension); }
} s_register;

} // namespace
