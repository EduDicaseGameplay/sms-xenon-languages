// GX textures on Xenos: platform/gx/src/gx_texture.cpp's cache without GL. A texture map's
// image is decoded (decodeArgb, all its mip levels) into a linear A8R8G8B8 texture, kept by
// address, format, size, levels and palette, and checked again after GXInvalidateTexAll as
// upstream does: by hash, unless no write stamp touched the bytes since (gx_fifo.cpp).
// EFB copies made into textures (xe_gx_render.cpp) are found by destination address first.
#include "xe_gx.h"
#include "xe_gx_decode.h"
#include <math.h>
#include <string.h>
#include <unordered_map>
#include <vector>

namespace gx {

uint32_t g_statTexUploads;
uint64_t g_statTexHashBytes, g_statTexInvalidates;

namespace xe {

struct TexKey {
	const void* ptr;
	uint32_t fmt, w, h, levels, tlutAddr, tlutFmt;
};
struct TexKeyHash {
	size_t operator()(const TexKey& k) const { return size_t(hashBytes(&k, sizeof(k))); }
};
struct TexKeyEq {
	bool operator()(const TexKey& a, const TexKey& b) const { return sameWords(&a, &b, sizeof(a)); }
};
struct TexEntry {
	IDirect3DTexture9* tex;
	uint64_t dataHash, tlutHash;
	uint32_t checkedGen, checkedClock, checkedFrame, bytes;
	bool hashAlways;
	bool arbitraryMips;  // its levels are an effect, not reductions (gx_texture.cpp's mipLevelDiff)
	TexEntry()
	    : tex(NULL), dataHash(0), tlutHash(0), checkedGen(0), checkedClock(0), checkedFrame(0), bytes(0), hashAlways(false),
	      arbitraryMips(false) {}
};
static std::unordered_map<TexKey, TexEntry, TexKeyHash, TexKeyEq> s_cache;
static uint32_t s_gen = 1;
static std::vector<uint8_t> s_decode;
static std::vector<uint32_t> s_level, s_prevLevel, s_pal(256);
static IDirect3DTexture9* s_white;

struct CopyTex {
	IDirect3DTexture9* tex;
	int w, h;
	uint32_t bytes;  // written back to RAM: its size there (0: not written back)
};
static std::unordered_map<const void*, CopyTex> s_copies;
static std::vector<IDirect3DTexture9*> s_droppedCopies;  // by copyCpuWrote, any thread
static uint32_t s_copiesGen;  // bumped when a copy's texture may have changed

// Each map's last binding: the same registers, with no invalidation and no copy made since, bind
// the same texture without the cache lookups (most draws keep their textures). Paletted textures
// are looked up each time: a TLUT load changes them behind the registers.
struct BindMemo {
	const uint8_t* ptr;
	uint32_t mode0, mode1, img0, gen, copiesGen, levels;
	IDirect3DBaseTexture9* tex;
	bool arbitraryMips, valid;
};
static BindMemo s_memo[8];

}  // namespace xe

void textureInvalidateAll()
{
	xe::s_gen++;
	g_statTexInvalidates++;
}

namespace xe {

void textureFrameEnd()
{
	for (size_t i = 0; i < s_droppedCopies.size(); i++) releaseLater(s_droppedCopies[i]);
	s_droppedCopies.clear();
	releaseFrameEnd();
}

IDirect3DTexture9* copyTexture(const void* dest, int w, int h)
{
	s_copiesGen++;
	CopyTex& c = s_copies[dest];
	if (c.tex && (c.w != w || c.h != h)) {
		releaseLater(c.tex);
		c.tex = NULL;
	}
	if (!c.tex) {
		// a Resolve target: tiled (the plain format) and 4 KiB aligned, as CreateTexture gives
		if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, 0, &c.tex, NULL))) {
			logmsg("copy texture %dx%d: out of memory", w, h);
			c.tex = NULL;
		}
		c.w = w;
		c.h = h;
	}
	c.bytes = 0;
	return c.tex;
}

bool isCopyTexture(const void* addr) { return s_copies.count(addr) != 0; }

void copySetBytes(const void* dest, uint32_t bytes)
{
	std::unordered_map<const void*, CopyTex>::iterator it = s_copies.find(dest);
	if (it != s_copies.end()) it->second.bytes = bytes;
}

// gx_texture.cpp's textureCpuWrote: RAM is the truth again where the CPU wrote, so copies
// written back there stop shadowing it (their textures go at the next display copy: this
// may run on any game thread).
void copyCpuWrote(const void* p, uint32_t size)
{
	const uint8_t *lo = (const uint8_t*)p, *hi = lo + size;
	for (std::unordered_map<const void*, CopyTex>::iterator it = s_copies.begin(); it != s_copies.end();) {
		const uint8_t* a = (const uint8_t*)it->first;
		if (it->second.bytes && a < hi && a + it->second.bytes > lo) {
			if (it->second.tex) s_droppedCopies.push_back(it->second.tex);
			it = s_copies.erase(it);
			s_copiesGen++;
		} else {
			++it;
		}
	}
}

// A level into 0xAARRGGBB dwords (A, R, G, B in the 360's memory, as A8R8G8B8 has them):
// decodeArgb, or decodeTexture's RGBA bytes for a level that is not whole tiles.
static void decodeLevel(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* tlut, uint32_t tlutFmt,
                        std::vector<uint32_t>& out)
{
	out.resize(size_t(w) * h);
	if (decodeArgb(src, fmt, w, h, tlut, tlutFmt, &s_pal[0], &out[0])) return;
	s_decode.resize(size_t(w) * h * 4);
	decodeTexture(src, fmt, w, h, tlut, tlutFmt, &s_decode[0]);
	const uint8_t* s = &s_decode[0];
	for (size_t i = 0; i < out.size(); i++, s += 4)
		out[i] = uint32_t(s[3]) << 24 | uint32_t(s[0]) << 16 | uint32_t(s[1]) << 8 | s[2];
}

// Dwords into a locked linear level. Whole aligned dwords in order only: a locked texture is
// write-combined memory.
static void storeLevel(IDirect3DTexture9* t, UINT level, const uint32_t* argb, uint32_t w, uint32_t h)
{
	D3DLOCKED_RECT r;
	if (FAILED(t->LockRect(level, &r, NULL, 0))) return;
	for (uint32_t y = 0; y < h; y++, argb += w) {
		uint32_t* d = (uint32_t*)((uint8_t*)r.pBits + y * r.Pitch);
		for (uint32_t x = 0; x < w; x++) d[x] = argb[x];
	}
	t->UnlockRect(level);
}

// Map `map` bound to `tex` with the GX sampler state (gx_texture.cpp's samplerFor). Levels drawn as
// an effect are picked as at the GameCube's resolution: their bias makes up for the internal one.
static void bind(int map, IDirect3DBaseTexture9* tex, uint32_t mode0, uint32_t mode1, uint32_t levels,
                 bool arbitraryMips = false)
{
	static const DWORD wrap[4] = {D3DTADDRESS_CLAMP, D3DTADDRESS_WRAP, D3DTADDRESS_MIRROR, D3DTADDRESS_WRAP};
	DWORD ss[SS_COUNT];
	ss[SS_ADDRESSU] = wrap[mode0 & 3];
	ss[SS_ADDRESSV] = wrap[(mode0 >> 2) & 3];
	ss[SS_MAGFILTER] = (mode0 >> 4) & 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	uint32_t mf = (mode0 >> 5) & 7;
	bool lin = (mf & 4) != 0;
	uint32_t mip = mf & 3;  // 0 none, 1 nearest mip, 2 linear mip
	ss[SS_MINFILTER] = lin ? D3DTEXF_LINEAR : D3DTEXF_POINT;
	ss[SS_MIPFILTER] = levels <= 1 || mip == 0 ? D3DTEXF_NONE : mip == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR;
	float bias = float(int8_t((mode0 >> 9) & 0xFF)) / 32.0f;
	if (arbitraryMips && levels > 1 && mip != 0) bias += log2f(internalScale());
	memcpy(&ss[SS_LODBIAS], &bias, 4);
	DWORD minLevel = (mode1 & 0xFF) / 16;  // GX's min LOD: the most detailed level allowed
	ss[SS_MAXMIPLEVEL] = minLevel < levels ? minLevel : levels - 1;
	emitTexture(map, tex, ss);
}

void bindTexture(int map, float* outW, float* outH)
{
	XeClock clock(g_xeTex);  // checks, decodes and uploads
	uint32_t mode0 = g.bp[bpTexReg(BP_TX_MODE0, map)];
	uint32_t mode1 = g.bp[bpTexReg(BP_TX_MODE1, map)];
	uint32_t img0 = g.bp[bpTexReg(BP_TX_IMAGE0, map)];
	uint32_t tlutReg = g.bp[bpTexReg(BP_TX_TLUT, map)];
	uint32_t w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
	const uint8_t* ptr = g.texImage[map];
	*outW = float(w);
	*outH = float(h);
	BindMemo& memo = s_memo[map];
	if (memo.valid && memo.ptr == ptr && memo.mode0 == mode0 && memo.mode1 == mode1 && memo.img0 == img0 &&
	    memo.gen == s_gen && memo.copiesGen == s_copiesGen && !tracing()) {
		bind(map, memo.tex, mode0, mode1, memo.levels, memo.arbitraryMips);
		return;
	}
	memo.valid = false;
	const BindMemo next = {ptr, mode0, mode1, img0, s_gen, s_copiesGen, 1, NULL, false, false};

	if (tracing())
		logmsg("  tex map %d %p fmt %u %ux%u%s", map, ptr, fmt, w, h, s_copies.count(ptr) ? " (EFB copy)" : "");
	std::unordered_map<const void*, CopyTex>::iterator ct = s_copies.find(ptr);
	if (ct != s_copies.end() && ct->second.tex) {
		bind(map, ct->second.tex, mode0, 0, 1);
		if (mode1 == 0) {  // what bind was given
			memo = next;
			memo.tex = ct->second.tex;
			memo.valid = true;
		}
		return;
	}
	if (!ptr) {
		if (!s_white && SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_LIN_A8R8G8B8, 0, &s_white, NULL))) {
			static const uint32_t px = 0xFFFFFFFFu;
			storeLevel(s_white, 0, &px, 1, 1);
		}
		bind(map, s_white, 0, 0, 1);
		return;
	}

	// mip levels present: GX has no explicit count, derive it from max LOD
	uint32_t levels = 1;
	if (((mode0 >> 5) & 3) != 0) {
		uint32_t maxLod = ((mode1 >> 8) & 0xFF) / 16;
		uint32_t m = w > h ? w : h;
		while (levels <= maxLod && (m >> levels) >= 1) levels++;
		if (levels > 11) levels = 11;
	}
	bool ci = fmt == 8 || fmt == 9 || fmt == 10;
	TexKey key = {ptr, fmt, w, h, levels, ci ? (tlutReg & 0x3FF) : 0, ci ? ((tlutReg >> 10) & 3) : 0};
	const uint8_t* tlut = ci ? g.tlutMem + ((tlutReg & 0x3FF) << 9) : NULL;
	uint32_t tlutBytes = fmt == 8 ? 32 : fmt == 9 ? 512 : 0x8000;
	if (tlut && ((tlutReg & 0x3FF) << 9) + tlutBytes > TMEM_TLUT_SIZE) tlutBytes = TMEM_TLUT_SIZE - ((tlutReg & 0x3FF) << 9);

	TexEntry& e = s_cache[key];
	uint32_t total = 0;
	{
		uint32_t lw = w, lh = h;
		for (uint32_t l = 0; l < levels; l++) {
			total += texLevelBytes(fmt, lw, lh);
			lw = lw > 1 ? lw / 2 : 1;
			lh = lh > 1 ? lh / 2 : 1;
		}
	}
	bool upload = !e.tex;
	bool unwritten = false;
	if (e.checkedGen != s_gen && e.checkedGen != 0 && !upload && trustWriteStamps() && !e.hashAlways &&
	    e.bytes == total && unwrittenSince(ptr, total, e.checkedClock)) {
		unwritten = true;
		if (g_displayFrames - e.checkedFrame < kRecheckFrames && (tlut ? hashBytes(tlut, tlutBytes) : 0) == e.tlutHash)
			e.checkedGen = s_gen;
	}
	if (e.checkedGen != s_gen || upload) {
		g_statTexHashBytes += total;
		e.checkedClock = writeClock();
		// each texture's hash checks on frames of its own (its address's phase of the period):
		// textures checked together would all be hashed again in one frame
		e.checkedFrame = g_displayFrames - (g_displayFrames + (uint32_t(uintptr_t(ptr)) >> 5) * 2654435761u) % kRecheckFrames;
		uint64_t dh = hashBytes(ptr, total);
		uint64_t th = tlut ? hashBytes(tlut, tlutBytes) : 0;
		if (unwritten && dh != e.dataHash) e.hashAlways = true;
		if (upload || dh != e.dataHash || th != e.tlutHash) upload = true;
		e.dataHash = dh;
		e.tlutHash = th;
		e.checkedGen = s_gen;
		e.bytes = total;
	}
	if (upload) {
		// new data in a new texture: the old one may still be read by queued draws
		if (e.tex) releaseLater(e.tex);
		e.tex = NULL;
		HRESULT hr;
		{
			XeClock clock(g_xeTexCreate);
			hr = dev->CreateTexture(w, h, levels, 0, D3DFMT_LIN_A8R8G8B8, 0, &e.tex, NULL);
		}
		if (FAILED(hr)) {
			logmsg("texture %ux%u (%u levels): out of memory", w, h, levels);
			e.tex = NULL;
		} else {
			g_statTexUploads++;
			const uint8_t* src = ptr;
			uint32_t lw = w, lh = h, pw = w, ph = h;
			float mipDiff = 0.0f;
			for (uint32_t l = 0; l < levels; l++) {
				{
					XeClock clock(g_xeTexDecode);
					decodeLevel(src, fmt, lw, lh, tlut, (tlutReg >> 10) & 3, s_level);
				}
				{
					XeClock clock(g_xeTexStore);
					storeLevel(e.tex, l, &s_level[0], lw, lh);
				}
				XeClock clock(g_xeTexMips);
				// a sum over all four channels: their order does not matter
				if (l > 0) mipDiff += mipLevelDiff((const uint8_t*)&s_prevLevel[0], pw, ph, (const uint8_t*)&s_level[0], lw, lh);
				s_prevLevel.swap(s_level);
				pw = lw;
				ph = lh;
				src += texLevelBytes(fmt, lw, lh);
				lw = lw > 1 ? lw / 2 : 1;
				lh = lh > 1 ? lh / 2 : 1;
			}
			e.arbitraryMips = levels > 1 && mipDiff / float(levels - 1) > kArbitraryMipThreshold;
		}
	}
	bind(map, e.tex, mode0, mode1, levels, e.arbitraryMips);
	if (!ci) {
		memo = next;
		memo.tex = e.tex;
		memo.levels = levels;
		memo.arbitraryMips = e.arbitraryMips;
		memo.valid = true;
	}
}

}  // namespace xe
}  // namespace gx
