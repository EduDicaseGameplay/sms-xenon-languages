// Host test for painting players in their own colours (sync/mp_look.cpp):
// a paint folder of one 2x1 texture of Mario's and one of FLUDD's, painted
// for a player with colours and for one without (their character's, but
// FLUDD's own).
#include "../../mp_game_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

static std::string g_look;
static std::vector<unsigned char> g_shown;
static const void* g_shownFor;
extern "C" int mpg_look(int, char* out, int size)
{
	snprintf(out, (size_t)size, "%s", g_look.c_str());
	return !g_look.empty();
}
extern "C" void GXPC_SetTextureOverride(const void* data, size_t, const unsigned char* rgba, int w, int h)
{
	g_shownFor = data;
	g_shown.assign(rgba, rgba + (size_t)w * h * 4);
}
static const void* g_plainFor;
extern "C" void GXPC_SetTexturePlain(const void* data, size_t) { g_plainFor = data; }
extern "C" void GXPC_ClearTextureOverrides(void) {}

static int failures;
#define CHECK(cond)                                                                    \
	do {                                                                               \
		if (!(cond)) {                                                                 \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
			failures++;                                                                \
		}                                                                              \
	} while (0)

static void put32(FILE* f, unsigned v)
{
	unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
	fwrite(b, 1, 4, f);
}

int main()
{
	char dir[] = "/tmp/mp_look_test_XXXXXX";
	if (!mkdtemp(dir))
		return 1;
	FILE* f = fopen((std::string(dir) + "/paint.txt").c_str(), "w");
	fprintf(f, "sms-paint 1\nregion 0 hat d6161c 46\nregion 1 shirt d6161c 46\nregion 2 fluddBody ffe239 90\n"
	           "texture H_ma_new_main_s3tc 0.paint\ntexture H_watergun_main_s3tc 1.paint\n");
	fclose(f);
	// texel 0: the cap's red, at the cap's typical lightness (117 of 255 = 46%); texel 1: left as it is
	f = fopen((std::string(dir) + "/0.paint").c_str(), "wb");
	fwrite("SMSPAINT", 1, 8, f);
	put32(f, 1), put32(f, 2), put32(f, 1);
	const unsigned char rgba[8]  = {214, 22, 28, 255, 30, 60, 200, 255};
	const unsigned char maps[8]  = {0, 255, 255, 255, 0, 0, 117, 90}; // mask, second, mix, lightness (2 texels each)
	fwrite(rgba, 1, 8, f);
	fwrite(maps, 1, 8, f);
	fclose(f);
	// FLUDD's: texel 0 its yellow body (lightness 229 of 255 = 90%), texel 1 Mario's red, left as it is
	f = fopen((std::string(dir) + "/1.paint").c_str(), "wb");
	fwrite("SMSPAINT", 1, 8, f);
	put32(f, 1), put32(f, 2), put32(f, 1);
	const unsigned char fluddRgba[8] = {255, 226, 57, 255, 214, 22, 28, 255};
	const unsigned char fluddMaps[8] = {2, 255, 255, 255, 0, 0, 229, 117};
	fwrite(fluddRgba, 1, 8, f);
	fwrite(fluddMaps, 1, 8, f);
	fclose(f);
	setenv("SMS_MARIO_PAINT", dir, 1);
	static unsigned char data[32];

	CHECK(!mpg_dress(1, MPG_CHAR_LUIGI, "H_ma_other_texture", data, sizeof data));

	g_look = "hat=2fa846;bogus=123456;shirt=nothex";
	CHECK(mpg_dress(1, MPG_CHAR_LUIGI, "H_ma_new_main_s3tc", data, sizeof data));
	CHECK(g_shownFor == data && g_shown.size() == 8);
	// the chosen green at the cap's typical lightness; the other texel and alpha untouched
	CHECK(abs(g_shown[0] - 0x2f) <= 3 && abs(g_shown[1] - 0xa8) <= 3 && abs(g_shown[2] - 0x46) <= 3);
	CHECK(g_shown[3] == 255);
	CHECK(!memcmp(&g_shown[4], &rgba[4], 4));

	// no colours of their own: Luigi's (Mario's red to green), the blue left as it is
	g_look.clear();
	CHECK(mpg_dress(1, MPG_CHAR_LUIGI, "H_ma_new_main_s3tc", data, sizeof data));
	CHECK(g_shown[1] > g_shown[0] && g_shown[1] > g_shown[2]);
	CHECK(!memcmp(&g_shown[4], &rgba[4], 4));
	// and Mario's own: the picture as it is
	CHECK(mpg_dress(0, MPG_CHAR_MARIO, "H_ma_new_main_s3tc", data, sizeof data));
	CHECK(!memcmp(g_shown.data(), rgba, 8));

	// FLUDD: a player's FLUDD colour paints its body
	g_look = "hat=2fa846;fluddBody=2a52c8";
	CHECK(mpg_dress(1, MPG_CHAR_LUIGI, "H_watergun_main_s3tc", data, sizeof data));
	CHECK(g_shown.size() == 8 && g_shown[2] > g_shown[0] && g_shown[2] > g_shown[1]);
	CHECK(!memcmp(&g_shown[4], &fluddRgba[4], 4));
	// without FLUDD colours, FLUDD's own, even for Luigi (his green is for Mario's clothes)
	g_look = "hat=2fa846";
	CHECK(mpg_dress(1, MPG_CHAR_LUIGI, "H_watergun_main_s3tc", data, sizeof data));
	CHECK(!memcmp(g_shown.data(), fluddRgba, 8));
	g_look.clear();
	CHECK(mpg_dress(1, MPG_CHAR_LUIGI, "H_watergun_main_s3tc", data, sizeof data));
	CHECK(!memcmp(g_shown.data(), fluddRgba, 8));

	// a texture with no picture (none of this game's, or Eclipse's own
	// characters' textures): kept plain, so this game's own look stays off it
	CHECK(!mpg_dress(1, MPG_CHAR_MARIO, "H_ma_other_texture", data, sizeof data));
	mpg_dress_plain(data, sizeof data);
	CHECK(g_plainFor == data);

	remove((std::string(dir) + "/1.paint").c_str());
	remove((std::string(dir) + "/paint.txt").c_str());
	remove((std::string(dir) + "/0.paint").c_str());
	rmdir(dir);
	if (failures)
		fprintf(stderr, "%d checks failed\n", failures);
	else
		printf("mp_look_test: all checks passed\n");
	return failures ? 1 : 0;
}
