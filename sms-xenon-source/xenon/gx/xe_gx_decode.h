// GameCube texture levels straight into 0xAARRGGBB dwords: the Xenos twin of gx_texture.cpp's
// decodeTexture (RGBA bytes, a pixel at a time), same colours bit for bit, built for an
// in-order core: whole tiles, dword stores, palettes expanded once. Levels that are not a whole
// number of tiles return false; decodeTexture takes them. No D3D here, so a host test
// (xenon/tools/decode_test.cpp) compares the two.
#ifndef XE_GX_DECODE_H
#define XE_GX_DECODE_H
#include <stdint.h>

namespace gx {
namespace xe {

static inline uint32_t argb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return a << 24 | r << 16 | g << 8 | b; }
static inline uint32_t be16At(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
static inline uint32_t fromRgb565(uint32_t v)
{
	return argb(255, (v >> 11) << 3 | (v >> 13), ((v >> 5) & 63) << 2 | ((v >> 9) & 3), (v & 31) << 3 | ((v >> 2) & 7));
}
static inline uint32_t fromRgb5a3(uint32_t v)
{
	if (v & 0x8000)
		return argb(255, ((v >> 10) & 31) << 3 | ((v >> 12) & 7), ((v >> 5) & 31) << 3 | ((v >> 7) & 7),
		            (v & 31) << 3 | ((v >> 2) & 7));
	return argb(((v >> 12) & 7) * 255 / 7, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17);
}
static inline uint32_t fromIa8(uint32_t v) { return (v >> 8) << 24 | (v & 0xFF) * 0x010101u; }
static inline uint32_t fromTlut(uint32_t v, uint32_t tlutFmt)
{
	return tlutFmt == 0 ? fromIa8(v) : tlutFmt == 1 ? fromRgb565(v) : fromRgb5a3(v);
}

// pal: room for 256 dwords
static inline bool decodeArgb(const uint8_t* p, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* tlut,
                              uint32_t tlutFmt, uint32_t* pal, uint32_t* out)
{
	static const uint8_t tileW[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
	static const uint8_t tileH[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
	fmt &= 15;
	uint32_t tw = tileW[fmt], th = tileH[fmt];
	if (!tw || w % tw || h % th) return false;
	if (fmt == 8 || fmt == 9) {  // C14X2 looks up its palette per texel: 32 KiB may run past TLUT memory
		uint32_t n = fmt == 8 ? 16 : 256;
		for (uint32_t i = 0; i < n; i++) pal[i] = fromTlut(be16At(tlut + 2 * i), tlutFmt);
	}
	for (uint32_t y0 = 0; y0 < h; y0 += th) {
		for (uint32_t x0 = 0; x0 < w; x0 += tw) {
			uint32_t* o = out + y0 * w + x0;
			switch (fmt) {
			case 0:  // I4
				for (uint32_t r = 0; r < 8; r++, p += 4, o += w)
					for (uint32_t i = 0; i < 4; i++) {
						o[2 * i] = (p[i] >> 4) * 0x11111111u;
						o[2 * i + 1] = (p[i] & 15) * 0x11111111u;
					}
				break;
			case 1:  // I8
				for (uint32_t r = 0; r < 4; r++, p += 8, o += w)
					for (uint32_t i = 0; i < 8; i++) o[i] = p[i] * 0x01010101u;
				break;
			case 2:  // IA4
				for (uint32_t r = 0; r < 4; r++, p += 8, o += w)
					for (uint32_t i = 0; i < 8; i++) o[i] = (p[i] >> 4) * 0x11000000u | (p[i] & 15) * 0x111111u;
				break;
			case 3:  // IA8
				for (uint32_t r = 0; r < 4; r++, p += 8, o += w)
					for (uint32_t i = 0; i < 4; i++) o[i] = fromIa8(be16At(p + 2 * i));
				break;
			case 4:  // RGB565
				for (uint32_t r = 0; r < 4; r++, p += 8, o += w)
					for (uint32_t i = 0; i < 4; i++) o[i] = fromRgb565(be16At(p + 2 * i));
				break;
			case 5:  // RGB5A3
				for (uint32_t r = 0; r < 4; r++, p += 8, o += w)
					for (uint32_t i = 0; i < 4; i++) o[i] = fromRgb5a3(be16At(p + 2 * i));
				break;
			case 6:  // RGBA8: AR plane then GB plane
				for (uint32_t r = 0; r < 4; r++, p += 8, o += w)
					for (uint32_t i = 0; i < 4; i++) o[i] = be16At(p + 2 * i) << 16 | be16At(p + 32 + 2 * i);
				p += 32;
				break;
			case 8:  // C4
				for (uint32_t r = 0; r < 8; r++, p += 4, o += w)
					for (uint32_t i = 0; i < 4; i++) {
						o[2 * i] = pal[p[i] >> 4];
						o[2 * i + 1] = pal[p[i] & 15];
					}
				break;
			case 9:  // C8
				for (uint32_t r = 0; r < 4; r++, p += 8, o += w)
					for (uint32_t i = 0; i < 8; i++) o[i] = pal[p[i]];
				break;
			case 10:  // C14X2
				for (uint32_t r = 0; r < 4; r++, p += 8, o += w)
					for (uint32_t i = 0; i < 4; i++) o[i] = fromTlut(be16At(tlut + 2 * (be16At(p + 2 * i) & 0x3FFF)), tlutFmt);
				break;
			case 14:  // CMPR: 2x2 DXT1 sub-blocks
				for (uint32_t sb = 0; sb < 4; sb++, p += 8) {
					uint32_t c0 = be16At(p), c1 = be16At(p + 2);
					uint32_t a = fromRgb565(c0), b = fromRgb565(c1);
					uint32_t ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
					uint32_t br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
					uint32_t c[4] = {a, b};
					if (c0 > c1) {
						c[2] = argb(255, (2 * ar + br) / 3, (2 * ag + bg) / 3, (2 * ab + bb) / 3);
						c[3] = argb(255, (ar + 2 * br) / 3, (ag + 2 * bg) / 3, (ab + 2 * bb) / 3);
					} else {
						// index 3: transparent with index 2's colour, as decodeTexture has it
						c[2] = argb(255, (ar + br) / 2, (ag + bg) / 2, (ab + bb) / 2);
						c[3] = c[2] & 0xFFFFFFu;
					}
					uint32_t* s = o + (sb >> 1) * 4 * w + (sb & 1) * 4;
					for (uint32_t r = 0; r < 4; r++, s += w) {
						uint32_t bits = p[4 + r];
						s[0] = c[bits >> 6];
						s[1] = c[(bits >> 4) & 3];
						s[2] = c[(bits >> 2) & 3];
						s[3] = c[bits & 3];
					}
				}
				break;
			}
		}
	}
	return true;
}

}  // namespace xe
}  // namespace gx
#endif
