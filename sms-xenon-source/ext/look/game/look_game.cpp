// Look (ext/look/README.md): Customize Mario's colours (the launcher's
// SMS_MARIO_LOOK pack) stay on Mario. The game finds the pack's pictures by
// the texture's hash, and Super Mario Eclipse's Luigi and Il Piantissimo draw
// one of Mario's textures, his mouth (H_ma_mouse1_s3tc), so a player who
// chose either would wear Mario's skin around their mouth. When this game's
// player isn't Mario, every texture of their body model is drawn plain
// (GXPC_SetTexturePlain): their own, never Mario's look. Their cap and hands
// draw the body's texture data, so the body's textures are all of them.
//
// Multiplayer keeps the other players' models out of the look itself
// (mpg_dress_plain), and lets go of every plain texture as a stage is set up
// (mpg_undress_all), as it may our own when looks change: each tick checks
// the marks are still there.
#include "port_ext.h"
#include "../../multiplayer/mp_game_api.h"
#include "../../multiplayer/game/mp_puppet.hpp"

#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <Player/Mario.hpp>
#include <dolphin/gx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" void GXPC_SetTexturePlain(const void* data, size_t bytes) __attribute__((weak));
extern "C" int GXPC_TextureIsPlain(const void* data, size_t bytes) __attribute__((weak));

namespace {

// Where GX reads a texture of a model: the header's offset added in 32 bits,
// as on the console (mp_puppet.cpp's TMpPuppet::dress), and its size.
const void* textureData(ResTIMG* timg, size_t* bytes)
{
	*bytes = (size_t)GXGetTexBufferSize(timg->width, timg->height, timg->format, GX_FALSE, 0);
	return (const void*)(uintptr_t)(u32)((uintptr_t)timg + timg->imageDataOffset);
}

int s_body = MPG_BODY_MARIO; // this stage's player's (Eclipse mounts the archive as a stage starts)

void keepOwnTextures()
{
	TMario* mario = gpMarioOriginal;
	if (s_body == MPG_BODY_MARIO || !mario || !mario->mBodyModelData)
		return;
	J3DTexture* textures = mario->mBodyModelData->getTexture();
	if (!textures || !textures->getNum() || !textures->getResTIMG(0))
		return;
	// one check a tick: the marks go all together
	size_t bytes;
	const void* data = textureData(textures->getResTIMG(0), &bytes);
	if (GXPC_TextureIsPlain(data, bytes))
		return;
	for (u16 i = 0; i < textures->getNum(); i++)
		if (ResTIMG* timg = textures->getResTIMG(i)) {
			data = textureData(timg, &bytes);
			GXPC_SetTexturePlain(data, bytes);
		}
	static bool said = false;
	if (!said) {
		said = true;
		fprintf(stderr, "[look] Customize Mario's colours stay on Mario: this character keeps their own textures\n");
	}
}

void stageSetup()
{
	static const bool look = getenv("SMS_MARIO_LOOK") && *getenv("SMS_MARIO_LOOK");
	s_body = look && GXPC_SetTexturePlain && GXPC_TextureIsPlain ? MpLocalBody() : MPG_BODY_MARIO;
	keepOwnTextures();
}

PortExtension s_extension = {
	"look", 0, keepOwnTextures, stageSetup,
};

struct Register {
	Register() { port_ext_register(&s_extension); }
} s_register;

} // namespace
