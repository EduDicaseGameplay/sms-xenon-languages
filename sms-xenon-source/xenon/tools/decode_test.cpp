// Host check: xenon/gx/xe_gx_decode.h's decodeArgb gives decodeTexture's colours for every format.
//   mkdir -p /tmp/dt/a/b && ln -sfn $PWD/platform /tmp/dt/platform
//   python3 xenon/tools/xcodec.py . /tmp/dt/a/b/codec.cpp
//   g++ -O2 -I/tmp/dt/a/b -Iplatform/gx/include -Iplatform/gx/port_include -Isrc/port_include -Iplatform \
//       xenon/tools/decode_test.cpp -o /tmp/decode_test && /tmp/decode_test
#include "codec.cpp"
#include "../gx/xe_gx_decode.h"
#include <stdio.h>

int main()
{
	static const uint32_t fmts[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14};
	std::vector<uint8_t> src(64 * 64 * 4), tlut(0x8000), rgba(64 * 64 * 4);
	std::vector<uint32_t> out(64 * 64), pal(256);
	uint32_t seed = 1, bad = 0;
	for (size_t i = 0; i < src.size(); i++) src[i] = uint8_t((seed = seed * 1103515245u + 12345u) >> 16);
	for (size_t i = 0; i < tlut.size(); i++) tlut[i] = uint8_t((seed = seed * 1103515245u + 12345u) >> 16);
	for (uint32_t f : fmts)
		for (uint32_t tf = 0; tf < 3; tf++)
			for (uint32_t w = 8; w <= 64; w *= 2)
				for (uint32_t h = 8; h <= 32; h *= 2) {
					gx::decodeTexture(&src[0], f, w, h, &tlut[0], tf, &rgba[0]);
					if (!gx::xe::decodeArgb(&src[0], f, w, h, &tlut[0], tf, &pal[0], &out[0])) {
						printf("fmt %u %ux%u: not decoded\n", f, w, h);
						bad++;
						continue;
					}
					for (uint32_t i = 0; i < w * h; i++) {
						const uint8_t* s = &rgba[i * 4];
						uint32_t want = uint32_t(s[3]) << 24 | s[0] << 16 | s[1] << 8 | s[2];
						if (out[i] != want) {
							printf("fmt %u tlut %u %ux%u texel %u: %08x, want %08x\n", f, tf, w, h, i, out[i], want);
							bad++;
							break;
						}
					}
				}
	puts(bad ? "FAIL" : "ok");
	return bad != 0;
}
