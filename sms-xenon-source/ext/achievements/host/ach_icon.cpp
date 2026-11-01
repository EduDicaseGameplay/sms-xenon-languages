// Achievements (ext/achievements/README.md): the banner's icons. The launcher
// installs them in the game folder's assets/achievements/ and names that folder
// in SMS_ACHIEVEMENT_ICONS; each is <id>.png, 512 pixels square. The banner
// draws one at a time, so one texture is kept: the last icon asked for, made
// smaller and laid out as a GX RGBA8 texture in memory the game can address.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../../../platform/gx/src/third_party/stb_image.h"

extern "C" void* port_low_alloc(unsigned long size);

namespace {

const int kSize = 128; // the banner draws the icon about 83 game pixels wide; more for big windows

int s_id = 0; // the icon in s_tex, or 0 for none (and -id when it couldn't be read)
unsigned char* s_tex;

// RGBA rows to GX RGBA8 tiles: 4x4 blocks, each an AR half then a GB half.
void toGxRgba8(const unsigned char* rgba, int w, int h, unsigned char* out)
{
	size_t o = 0;
	for (int by = 0; by < h; by += 4)
		for (int bx = 0; bx < w; bx += 4)
			for (int half = 0; half < 2; half++)
				for (int y = 0; y < 4; y++)
					for (int x = 0; x < 4; x++) {
						const unsigned char* p = rgba + ((size_t)(by + y) * w + bx + x) * 4;
						out[o++]               = half ? p[1] : p[3];
						out[o++]               = half ? p[2] : p[0];
					}
}

// The icon at kSize x kSize, each pixel the average of the ones it covers,
// colour weighted by alpha so the medal's soft edge doesn't darken.
void shrink(const unsigned char* src, int w, int h, unsigned char* out)
{
	for (int y = 0; y < kSize; y++)
		for (int x = 0; x < kSize; x++) {
			const int x0 = x * w / kSize, x1 = (x + 1) * w / kSize, y0 = y * h / kSize, y1 = (y + 1) * h / kSize;
			double sum[4] = { 0, 0, 0, 0 };
			int n         = 0;
			for (int sy = y0; sy < y1; sy++)
				for (int sx = x0; sx < x1; sx++, n++) {
					const unsigned char* p = src + ((size_t)sy * w + sx) * 4;
					for (int k = 0; k < 3; k++)
						sum[k] += p[k] * (p[3] / 255.0);
					sum[3] += p[3];
				}
			unsigned char* o = out + ((size_t)y * kSize + x) * 4;
			const double a   = n ? sum[3] / n : 0;
			for (int k = 0; k < 3; k++) {
				const double v = a > 0 ? sum[k] / n / (a / 255.0) + 0.5 : 0;
				o[k]           = (unsigned char)(v > 255 ? 255 : v);
			}
			o[3] = (unsigned char)(a + 0.5);
		}
}

} // namespace

// Achievement `id`'s icon as a GX RGBA8 texture, size x size, or null when
// there is none (no SMS_ACHIEVEMENT_ICONS, or no such file).
extern "C" const void* sms_ach_icon(int id, int* size)
{
	if (id == s_id || id == -s_id) {
		*size = kSize;
		return s_id > 0 ? s_tex : nullptr;
	}
	s_id            = -id;
	const char* dir = getenv("SMS_ACHIEVEMENT_ICONS");
	if (!dir || !*dir)
		return nullptr;
	char path[1024];
	snprintf(path, sizeof path, "%s/%d.png", dir, id);
	int w = 0, h = 0, n = 0;
	unsigned char* rgba = stbi_load(path, &w, &h, &n, 4);
	if (!rgba)
		return nullptr;
	if (!s_tex)
		s_tex = (unsigned char*)port_low_alloc((unsigned long)kSize * kSize * 4);
	if (s_tex && w >= kSize && h >= kSize) {
		unsigned char* small = (unsigned char*)malloc((size_t)kSize * kSize * 4);
		if (small) {
			shrink(rgba, w, h, small);
			toGxRgba8(small, kSize, kSize, s_tex);
			free(small);
			s_id = id;
		}
	}
	stbi_image_free(rgba);
	*size = kSize;
	return s_id > 0 ? s_tex : nullptr;
}
