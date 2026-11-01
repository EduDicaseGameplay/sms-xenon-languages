// HLSL for the current XF (texgen, lighting) and BP (TEV, indirect, alpha compare, fog) state:
// platform/gx/src/gx_shader.cpp's GLSL generator, kept line for line where it can be, for Shader
// Model 3 on Xenos. SM3 has no integers, so the TEV's integer math runs on floats that hold
// whole numbers (exact up to 2^24): `x & 255` is x - 256 * floor(x / 256), `x >> n` is
// floor(x / 2^n), as two's complement integers give. Programs are cached by the state that
// shapes the code and the batch's vertex format; everything numeric is a constant register
// (xe_gx.h). The vertex shader reads only the attributes the format has.
#include "xe_gx.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace gx {
namespace xe {

struct Key {
	uint32_t fmt;  // the batch's VtxFmtBits
	uint32_t numTexGens, numChans, dualTex;
	uint32_t texgen[8], post[8];
	uint32_t chan[4];
	uint32_t numStages, numInd;
	uint32_t colorEnv[16], alphaEnv[16], order[16], ksel[16], ind[16];
	uint32_t swap[4];
	uint32_t iref, alphaFunc, fogType;
	uint32_t indScale[2];
};

static void buildKey(Key& k, uint32_t fmt)
{
	memset(&k, 0, sizeof(k));
	k.fmt = fmt;
	k.numTexGens = g.xfReg[XFR_NUMTEXGENS] & 15;
	if (k.numTexGens > 8) k.numTexGens = 8;
	k.numChans = g.xfReg[XFR_NUMCHANS] & 3;
	k.dualTex = g.xfReg[XFR_DUALTEX] & 1;
	for (uint32_t i = 0; i < k.numTexGens; i++) {
		k.texgen[i] = g.xfReg[XFR_TEXGEN + i];
		if (k.dualTex) k.post[i] = g.xfReg[XFR_POSTTEX + i] & 0x13F;
	}
	for (uint32_t i = 0; i < 2; i++) {
		k.chan[i] = g.xfReg[XFR_COLOR0CTRL + i] & 0x7FFF;
		k.chan[2 + i] = g.xfReg[XFR_ALPHA0CTRL + i] & 0x7FFF;
	}
	uint32_t gen = g.bp[BP_GENMODE];
	k.numStages = ((gen >> 10) & 15) + 1;
	k.numInd = (gen >> 16) & 7;
	if (k.numInd > 4) k.numInd = 4;
	for (uint32_t s = 0; s < k.numStages; s++) {
		k.colorEnv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s] & 0xFFFFFF;
		k.alphaEnv[s] = g.bp[BP_TEV_COLOR_ENV + 2 * s + 1] & 0xFFFFFF;
		k.order[s] = (g.bp[BP_TREF + (s >> 1)] >> ((s & 1) * 12)) & 0x3FF;
		uint32_t ks = g.bp[BP_TEV_KSEL + (s >> 1)];
		k.ksel[s] = (s & 1) ? ((ks >> 14) & 0x3FF) : ((ks >> 4) & 0x3FF);
		k.ind[s] = k.numInd ? (g.bp[BP_IND_CMD + s] & 0x1FFFFF) : 0;
	}
	for (int t = 0; t < 4; t++)
		k.swap[t] = (g.bp[BP_TEV_KSEL + 2 * t] & 15) | (g.bp[BP_TEV_KSEL + 2 * t + 1] & 15) << 4;
	k.iref = k.numInd ? g.bp[BP_RAS1_IREF] & 0xFFFFFF : 0;
	k.alphaFunc = (g.bp[BP_ALPHACOMPARE] >> 16) & 0xFF;
	k.fogType = (g.bp[BP_FOG3] >> 21) & 7;
	if (k.numInd) {
		k.indScale[0] = g.bp[BP_RAS1_SS0] & 0xFFFF;
		k.indScale[1] = g.bp[BP_RAS1_SS1] & 0xFFFF;
	}
}

static void add(std::string& s, const char* fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	_vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	buf[sizeof buf - 1] = 0;
	s += buf;
}

// ------------------------------------------------------------------ vertex shader
static void emitLight(std::string& s, uint32_t ctrl, const char* comp, int l)
{
	uint32_t diff = (ctrl >> 7) & 3;
	bool attn = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
	const char* dfn = diff == 0 ? "1.0" : diff == 1 ? "dot(N, L)" : "max(dot(N, L), 0.0)";
	int b = VC_LIGHT + 5 * l;  // colour, A, K, position, direction
	if (!attn) {
		add(s, "  { float3 L = normalize(xf[%d].xyz - vpos); lsum%s += (%s) * xf[%d]%s; }\n", b + 3, comp, dfn, b, comp);
	} else if (spot) {
		add(s,
		    "  { float3 L = xf[%d].xyz - vpos; float d2 = dot(L, L); float d = sqrt(d2); L /= d;\n"
		    "    float cs = max(dot(L, xf[%d].xyz), 0.0); float3 a = xf[%d].xyz; float3 k = xf[%d].xyz;\n"
		    "    float at = max(a.x + a.y * cs + a.z * cs * cs, 0.0) / max(k.x + k.y * d + k.z * d2, 1e-30);\n"
		    "    lsum%s += at * (%s) * xf[%d]%s; }\n",
		    b + 3, b + 4, b + 1, b + 2, comp, dfn, b, comp);
	} else {
		add(s,
		    "  { float3 L = normalize(xf[%d].xyz); float nl = dot(N, L);\n"
		    "    float t = nl > 0.0 ? max(dot(N, xf[%d].xyz), 0.0) : 0.0; float3 a = xf[%d].xyz; float3 k = xf[%d].xyz;\n"
		    "    float at = max(a.x + a.y * t + a.z * t * t, 0.0) / max(k.x + k.y * t + k.z * t * t, 1e-30);\n"
		    "    lsum%s += at * (%s) * xf[%d]%s; }\n",
		    b + 3, b + 4, b + 1, b + 2, comp, dfn, b, comp);
	}
}

static std::string genVS(const Key& k)
{
	const uint32_t f = k.fmt;
	std::string s;
	add(s, "float4 xf[%d] : register(c0);\n", VC_PROJ);
	add(s, "float4 u_proj[2] : register(c%d);\nfloat4 u_vp[2] : register(c%d);\nfloat4 u_efb : register(c%d);\n"
	       "float4 u_chan[4] : register(c%d);\nfloat4 u_defmtx[3] : register(c%d);\n",
	    VC_PROJ, VC_VP, VC_EFB, VC_CHAN, VC_DEFMTX);
	s += "struct VIn {\n  float3 pos : POSITION;\n";
	if (f & VF_NRM) s += "  float3 nrm : NORMAL;\n";
	if ((f & VF_NRM) && (f & VF_NBT)) s += "  float3 bin : BINORMAL;\n  float3 tan : TANGENT;\n";
	for (int c = 0; c < 2; c++)
		if (f & (VF_CLR0 << c)) add(s, "  float4 clr%d : COLOR%d;\n", c, c);
	for (int t = 0; t < 8; t++)
		if (f & (VF_TEX0 << t)) add(s, "  float2 tex%d : TEXCOORD%d;\n", t, t);
	if (f & VF_MTX) s += "  float4 mtx0 : BLENDINDICES0;\n  float4 mtx1 : BLENDINDICES1;\n  float4 mtx2 : BLENDINDICES2;\n";
	s += "};\n"
	     "struct VOut {\n  float4 pos : POSITION;\n  float4 c0 : COLOR0;\n  float4 c1 : COLOR1;\n"
	     "  float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3;\n"
	     "  float4 tc4 : TEXCOORD4; float4 tc5 : TEXCOORD5; float4 tc6 : TEXCOORD6; float4 tc7 : TEXCOORD7;\n};\n"
	     "VOut main(VIn i) {\n  VOut o;\n";
	// what the absent attributes read in GL (the current value 0, 0, 0, 1; matrix indices from the batch)
	add(s, "  float3 a_nrm = %s;\n", (f & VF_NRM) ? "i.nrm" : "float3(0.0, 0.0, 0.0)");
	add(s, "  float3 a_bin = %s;\n  float3 a_tan = %s;\n", ((f & VF_NRM) && (f & VF_NBT)) ? "i.bin" : "float3(0.0, 0.0, 0.0)",
	    ((f & VF_NRM) && (f & VF_NBT)) ? "i.tan" : "float3(0.0, 0.0, 0.0)");
	for (int c = 0; c < 2; c++)
		add(s, "  float4 a_clr%d = %s;\n", c, (f & (VF_CLR0 << c)) ? (c ? "i.clr1" : "i.clr0") : "float4(0.0, 0.0, 0.0, 1.0)");
	for (int t = 0; t < 8; t++) {
		if (f & (VF_TEX0 << t)) add(s, "  float2 a_tex%d = i.tex%d;\n", t, t);
		else add(s, "  float2 a_tex%d = float2(0.0, 0.0);\n", t);
	}
	if (f & VF_MTX) s += "  float4 m0 = i.mtx0, m1 = i.mtx1;\n";
	else s += "  float4 m0 = u_defmtx[0], m1 = u_defmtx[1];\n";
	s += "  int pm = (int)m0.x;\n"
	     "  float4 p = float4(i.pos, 1.0);\n"
	     "  float3 vpos = float3(dot(xf[pm], p), dot(xf[pm + 1], p), dot(xf[pm + 2], p));\n";
	add(s, "  int nm = %d + pm;\n", VC_NRM);
	s += "  float3 N = float3(dot(xf[nm].xyz, a_nrm), dot(xf[nm + 1].xyz, a_nrm), dot(xf[nm + 2].xyz, a_nrm));\n"
	     "  N = dot(N, N) > 0.0 ? normalize(N) : float3(0.0, 0.0, 1.0);\n"
	     "  float4 clip;\n"
	     "  if (u_proj[1].z == 0.0) {\n"
	     "    clip = float4(u_proj[0].x * vpos.x + u_proj[0].y * vpos.z, u_proj[0].z * vpos.y + u_proj[0].w * vpos.z,\n"
	     "                  u_proj[1].x * vpos.z + u_proj[1].y, -vpos.z);\n"
	     "  } else {\n"
	     "    clip = float4(u_proj[0].x * vpos.x + u_proj[0].y, u_proj[0].z * vpos.y + u_proj[0].w,\n"
	     "                  u_proj[1].x * vpos.z + u_proj[1].y, 1.0);\n"
	     "  }\n"
	     // the viewport in EFB pixels, then D3D's clip space over the whole EFB (row 0 at the top, z 0..w)
	     "  o.pos.x = ((u_vp[1].x - 342.0) * clip.w + u_vp[0].x * clip.x) * (2.0 / u_efb.x) - clip.w;\n"
	     "  o.pos.y = clip.w - ((u_vp[1].y - 342.0) * clip.w + u_vp[0].y * clip.y) * (2.0 / u_efb.y);\n"
	     "  float zw = u_vp[1].z * clip.w + u_vp[0].z * clip.z;\n"
	     "  o.pos.z = zw * (1.0 / 16777215.0);\n"
	     "  o.pos.w = clip.w;\n";

	const char* vtx[2] = {"a_clr0", "a_clr1"};
	for (int c = 0; c < 2; c++) {
		uint32_t cc = k.chan[c], ac = k.chan[2 + c];
		add(s, "  float4 col%d;\n", c);
		if (uint32_t(c) >= k.numChans) {
			add(s, "  col%d = float4(0.0, 0.0, 0.0, 0.0);\n", c);
			continue;
		}
		for (int part = 0; part < 2; part++) {
			uint32_t ctrl = part ? ac : cc;
			const char* comp = part ? ".a" : ".rgb";
			add(s, "  { float4 mat = %s; ", (ctrl & 1) ? vtx[c] : (c ? "u_chan[3]" : "u_chan[2]"));
			if (ctrl & 2) {
				add(s, "float4 amb = %s; float4 lsum = amb;\n", (ctrl & 64) ? vtx[c] : (c ? "u_chan[1]" : "u_chan[0]"));
				uint32_t mask = ((ctrl >> 2) & 15) | ((ctrl >> 11) & 15) << 4;
				for (int l = 0; l < 8; l++)
					if (mask & (1u << l)) emitLight(s, ctrl, comp, l);
				add(s, "    col%d%s = (mat * saturate(lsum))%s; }\n", c, comp, comp);
			} else {
				add(s, "col%d%s = mat%s; }\n", c, comp, comp);
			}
		}
	}
	s += "  o.c0 = col0;\n  o.c1 = col1;\n";

	// texgen; the texture matrix of coordinate t is byte t + 1 of the indices
	static const char* mi[8] = {"m0.y", "m0.z", "m0.w", "m1.x", "m1.y", "m1.z", "m1.w", "m2.x"};
	if (k.numTexGens > 7) s += (f & VF_MTX) ? "  float4 m2 = i.mtx2;\n" : "  float4 m2 = u_defmtx[2];\n";
	s += "  float3 tcs[8];\n";
	for (uint32_t i = 0; i < 8; i++) {
		if (i >= k.numTexGens) {
			add(s, "  tcs[%u] = float3(a_tex%u, 1.0);\n", i, i);
			continue;
		}
		uint32_t tg = k.texgen[i];
		uint32_t proj = (tg >> 1) & 1, form = (tg >> 2) & 1, type = (tg >> 4) & 7, row = (tg >> 7) & 31;
		uint32_t embSrc = (tg >> 12) & 7;
		char src[96];
		if (row == 0) strcpy(src, "float4(i.pos, 1.0)");
		else if (row == 1) strcpy(src, "float4(a_nrm, 1.0)");
		else if (row == 2) strcpy(src, "a_clr0");
		else if (row == 3) strcpy(src, "float4(a_bin, 1.0)");
		else if (row == 4) strcpy(src, "float4(a_tan, 1.0)");
		else _snprintf(src, sizeof src, "float4(a_tex%u, 1.0, 1.0)", (row - 5) & 7);
		std::string sv = src;
		if (form == 0 && row < 5) sv = "float4((" + sv + ").xy, 1.0, 1.0)";
		if (type == 0) {
			add(s, "  { float4 sv = %s; int tm = (int)%s; float3 t = float3(dot(xf[tm], sv), dot(xf[tm + 1], sv), %s);\n",
			    sv.c_str(), mi[i], proj ? "dot(xf[tm + 2], sv)" : "1.0");
		} else if (type == 1) {
			add(s, "  { float3 t = tcs[%u];\n", embSrc < i ? embSrc : 0);
		} else {
			add(s, "  { float3 t = float3(%s.rg, 1.0);\n", type == 3 ? "col1" : "col0");
		}
		if (k.dualTex && type == 0) {
			uint32_t pt = k.post[i] & 63;
			bool norm = (k.post[i] >> 8) & 1;
			add(s,
			    "    %s int pw = %d; float4 pv = float4(t, 1.0);\n"
			    "    t = float3(dot(xf[pw], pv), dot(xf[pw + 1], pv), dot(xf[pw + 2], pv));\n",
			    norm ? "t = normalize(t);" : "", VC_POST + int(pt));
		}
		add(s, "    tcs[%u] = t; }\n", i);
	}
	// the fragment's depth goes in tc0.w / tc1.w (z and w, interpolated: their ratio is screen-linear)
	s += "  o.tc0 = float4(tcs[0], o.pos.z); o.tc1 = float4(tcs[1], o.pos.w);\n"
	     "  o.tc2 = float4(tcs[2], 0.0); o.tc3 = float4(tcs[3], 0.0); o.tc4 = float4(tcs[4], 0.0);\n"
	     "  o.tc5 = float4(tcs[5], 0.0); o.tc6 = float4(tcs[6], 0.0); o.tc7 = float4(tcs[7], 0.0);\n"
	     "  return o;\n}\n";
	return s;
}

// ------------------------------------------------------------------ pixel shader
static const char* kPsHeader =
    "sampler2D s0 : register(s0); sampler2D s1 : register(s1); sampler2D s2 : register(s2); sampler2D s3 : register(s3);\n"
    "sampler2D s4 : register(s4); sampler2D s5 : register(s5); sampler2D s6 : register(s6); sampler2D s7 : register(s7);\n"
    "float4 u_tevreg[4] : register(c0);\n"
    "float4 u_konst[4] : register(c4);\n"
    "float4 u_texscale[8] : register(c8);\n"
    "float4 u_texsize[8] : register(c16);\n"
    "float4 u_alpharef : register(c24);\n"
    "float4 u_fog : register(c25);\n"
    "float4 u_fogcolor : register(c26);\n"
    "float4 u_indmtx[6] : register(c27);\n"
    "struct PIn {\n  float4 c0 : COLOR0;\n  float4 c1 : COLOR1;\n"
    "  float4 tc0 : TEXCOORD0; float4 tc1 : TEXCOORD1; float4 tc2 : TEXCOORD2; float4 tc3 : TEXCOORD3;\n"
    "  float4 tc4 : TEXCOORD4; float4 tc5 : TEXCOORD5; float4 tc6 : TEXCOORD6; float4 tc7 : TEXCOORD7;\n};\n"
    // whole numbers in floats: GLSL's ivec math of gx_shader.cpp
    "float4 texi(float4 c) { return floor(saturate(c) * 255.0 + 0.5); }\n"
    "float2 proj(float3 t) { return t.xy / (t.z == 0.0 ? 1.0 : t.z); }\n"
    "float3 lo8(float3 v) { return v - 256.0 * floor(v / 256.0); }\n"
    "float lo8(float v) { return v - 256.0 * floor(v / 256.0); }\n"
    "float3 lerp3(float3 a, float3 b, float3 c) { float3 c2 = c + floor(c / 128.0); return floor((a * (256.0 - c2) + b * c2 + 128.0) / 256.0); }\n"
    "float lerp1(float a, float b, float c) { float c2 = c + floor(c / 128.0); return floor((a * (256.0 - c2) + b * c2 + 128.0) / 256.0); }\n"
    "float2 wrapmod(float2 x, float n) { return x - n * floor(x / n); }\n";

static const char* kColorArg[16] = {"cprev.rgb", "cprev.aaa", "creg0.rgb", "creg0.aaa", "creg1.rgb", "creg1.aaa",
                                    "creg2.rgb", "creg2.aaa", "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa",
                                    "float3(255.0, 255.0, 255.0)", "float3(128.0, 128.0, 128.0)", "konst.rgb",
                                    "float3(0.0, 0.0, 0.0)"};
static const char* kAlphaArg[8] = {"cprev.a", "creg0.a", "creg1.a", "creg2.a", "tex.a", "ras.a", "konst.a", "0.0"};
static const char* kRegName[4] = {"cprev", "creg0", "creg1", "creg2"};

static std::string konstExpr(uint32_t sel, bool alpha)
{
	static const int k8[8] = {255, 223, 191, 159, 127, 95, 63, 31};
	char buf[64];
	if (sel < 8) {
		_snprintf(buf, sizeof buf, alpha ? "%d.0" : "float3(%d.0, %d.0, %d.0)", k8[sel], k8[sel], k8[sel]);
		return buf;
	}
	if (!alpha && sel >= 12 && sel < 16) {
		_snprintf(buf, sizeof buf, "u_konst[%u].rgb", sel - 12);
		return buf;
	}
	if (sel >= 16) {
		const char comp = "rgba"[(sel - 16) >> 2];
		_snprintf(buf, sizeof buf, alpha ? "u_konst[%u].%c" : "u_konst[%u].%c%c%c", sel & 3, comp, comp, comp);
		return buf;
	}
	return alpha ? "255.0" : "float3(255.0, 255.0, 255.0)";
}

static std::string swizzle(uint32_t swap)
{
	std::string s;
	const char* c = "rgba";
	s += c[swap & 3];
	s += c[(swap >> 2) & 3];
	s += c[(swap >> 4) & 3];
	s += c[(swap >> 6) & 3];
	return s;
}

static std::string genPS(const Key& k)
{
	std::string s = kPsHeader;
	s += "float4 main(PIn i) : COLOR0 {\n"
	     "  float3 vtc[8] = { i.tc0.xyz, i.tc1.xyz, i.tc2.xyz, i.tc3.xyz, i.tc4.xyz, i.tc5.xyz, i.tc6.xyz, i.tc7.xyz };\n"
	     "  float4 cprev = u_tevreg[0], creg0 = u_tevreg[1], creg1 = u_tevreg[2], creg2 = u_tevreg[3];\n"
	     "  float4 ras0 = texi(i.c0), ras1 = texi(i.c1);\n"
	     "  float4 tex = float4(255.0, 255.0, 255.0, 255.0), ras = float4(0.0, 0.0, 0.0, 0.0), konst = ras;\n"
	     "  float bumpA = 0.0; float2 prevTc = float2(0.0, 0.0);\n";
	for (uint32_t i = 0; i < k.numInd; i++) {
		uint32_t map = (k.iref >> (6 * i)) & 7, coord = (k.iref >> (6 * i + 3)) & 7;
		uint32_t ss = (k.indScale[i < 2 ? 0 : 1] >> ((i & 1) * 8)) & 0xFF;
		add(s, "  float3 ind%u = texi(tex2D(s%u, proj(vtc[%u]) / float2(%d.0, %d.0))).abg;\n", i, map, coord,
		    1 << (ss & 15), 1 << ((ss >> 4) & 15));
	}
	uint32_t lastDest = 0, lastA = 0;
	for (uint32_t st = 0; st < k.numStages; st++) {
		uint32_t ord = k.order[st];
		uint32_t map = ord & 7, coord = (ord >> 3) & 7, en = (ord >> 6) & 1, chan = (ord >> 7) & 7;
		uint32_t ce = k.colorEnv[st], ae = k.alphaEnv[st];
		uint32_t rasSwap = k.swap[ae & 3], texSwap = k.swap[(ae >> 2) & 3];
		add(s, "  // stage %u\n  {\n", st);
		uint32_t ind = k.ind[st];
		if (en) {
			if (ind) {
				uint32_t bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3,
				         mid = (ind >> 9) & 15, sw = (ind >> 13) & 7, tw = (ind >> 16) & 7, fb = (ind >> 20) & 1;
				static const int fmtMod[4] = {256, 32, 16, 8};
				uint32_t src = bt < k.numInd ? bt : 0;
				add(s, "    float3 iv = ind%u - %d.0 * floor(ind%u / %d.0);\n", src, fmtMod[fmt], src, fmtMod[fmt]);
				if (bs) add(s, "    bumpA = floor(ind%u.%c / 8.0) * 8.0;\n", src, "xxyz"[bs]);
				int bv = fmt == 0 ? -128 : 1;
				add(s, "    iv += float3(%d.0, %d.0, %d.0);\n", (bias & 1) ? bv : 0, (bias & 2) ? bv : 0, (bias & 4) ? bv : 0);
				if (mid >= 1 && mid <= 3) {
					// the hardware's integer offset (gx_shader.cpp): s.10 entries, the product >> 3 in 1/128
					// texels, shifted by the matrix's exponent, kept to the coordinate's 24 bits
					uint32_t m = mid - 1;
					add(s,
					    "    float3 mr0 = u_indmtx[%u].xyz * 1024.0, mr1 = u_indmtx[%u].xyz * 1024.0;\n"
					    "    float2 it = floor(float2(dot(mr0, iv), dot(mr1, iv)) / 8.0);\n"
					    "    float sc = u_indmtx[%u].w;\n"
					    "    it = sc >= 32.0 ? float2(0.0, 0.0) : floor(it * exp2(sc));\n"
					    "    it = it - 16777216.0 * floor((it + 8388608.0) / 16777216.0);\n"
					    "    float2 off = it / 128.0;\n",
					    m * 2, m * 2 + 1, m * 2);
				} else {
					s += "    float2 off = float2(0.0, 0.0);\n";
				}
				add(s, "    float2 tcx = proj(vtc[%u]) * u_texscale[%u].xy * u_texsize[%u].xy;\n", coord, coord, map);
				static const int wrapSize[8] = {0, 256, 128, 64, 32, 16, 0, 0};
				if (sw == 6) s += "    tcx.x = 0.0;\n";
				else if (sw) add(s, "    tcx.x = wrapmod(tcx, %d.0).x;\n", wrapSize[sw]);
				if (tw == 6) s += "    tcx.y = 0.0;\n";
				else if (tw) add(s, "    tcx.y = wrapmod(tcx, %d.0).y;\n", wrapSize[tw]);
				add(s, "    tcx += off%s; prevTc = tcx;\n", fb ? " + prevTc" : "");
				add(s, "    tex = texi(tex2D(s%u, tcx / u_texsize[%u].xy)).%s;\n", map, map, swizzle(texSwap).c_str());
			} else {
				add(s, "    tex = texi(tex2D(s%u, proj(vtc[%u]) * u_texscale[%u].xy)).%s;\n", map, coord, coord,
				    swizzle(texSwap).c_str());
			}
		}
		if (chan == 0) add(s, "    ras = ras0.%s;\n", swizzle(rasSwap).c_str());
		else if (chan == 1) add(s, "    ras = ras1.%s;\n", swizzle(rasSwap).c_str());
		else if (chan == 5) s += "    ras = bumpA.xxxx;\n";
		else if (chan == 6) s += "    ras = (bumpA + floor(bumpA / 32.0)).xxxx;\n";  // bumpA | bumpA >> 5: no common bits
		else s += "    ras = float4(0.0, 0.0, 0.0, 0.0);\n";
		uint32_t kc = k.ksel[st] & 31, ka = (k.ksel[st] >> 5) & 31;
		add(s, "    konst = float4(%s, %s);\n", konstExpr(kc, false).c_str(), konstExpr(ka, true).c_str());

		{  // colour
			uint32_t d = ce & 15, c = (ce >> 4) & 15, b = (ce >> 8) & 15, a = (ce >> 12) & 15;
			uint32_t bias = (ce >> 16) & 3, op = (ce >> 18) & 1, clampv = (ce >> 19) & 1, scale = (ce >> 20) & 3,
			         dest = (ce >> 22) & 3;
			add(s, "    float3 A = lo8(%s), B = lo8(%s), Cc = lo8(%s), D = %s;\n", kColorArg[a], kColorArg[b],
			    kColorArg[c], kColorArg[d]);
			if (bias != 3) {
				add(s, "    float3 r = D %c lerp3(A, B, Cc)%s;\n", op ? '-' : '+',
				    bias == 1 ? " + 128.0" : bias == 2 ? " - 128.0" : "");
				if (scale == 1) s += "    r *= 2.0;\n";
				else if (scale == 2) s += "    r *= 4.0;\n";
				else if (scale == 3) s += "    r = floor(r / 2.0);\n";
			} else {
				uint32_t mode = (scale << 1) | op;
				const char* cond;
				switch (mode) {
				case 0: cond = "(A.r > B.r).xxx"; break;
				case 1: cond = "(A.r == B.r).xxx"; break;
				case 2: cond = "((A.g * 256.0 + A.r) > (B.g * 256.0 + B.r)).xxx"; break;
				case 3: cond = "((A.g * 256.0 + A.r) == (B.g * 256.0 + B.r)).xxx"; break;
				case 4: cond = "((A.b * 65536.0 + A.g * 256.0 + A.r) > (B.b * 65536.0 + B.g * 256.0 + B.r)).xxx"; break;
				case 5: cond = "((A.b * 65536.0 + A.g * 256.0 + A.r) == (B.b * 65536.0 + B.g * 256.0 + B.r)).xxx"; break;
				case 6: cond = "(A > B)"; break;
				default: cond = "(A == B)"; break;
				}
				add(s, "    float3 r = D + (float3)(%s) * Cc;\n", cond);
			}
			add(s, "    %s.rgb = %s;\n", kRegName[dest], clampv ? "clamp(r, 0.0, 255.0)" : "clamp(r, -1024.0, 1023.0)");
			lastDest = dest;
		}
		{  // alpha
			uint32_t d = (ae >> 4) & 7, c = (ae >> 7) & 7, b = (ae >> 10) & 7, a = (ae >> 13) & 7;
			uint32_t bias = (ae >> 16) & 3, op = (ae >> 18) & 1, clampv = (ae >> 19) & 1, scale = (ae >> 20) & 3,
			         dest = (ae >> 22) & 3;
			add(s, "    float Aa = lo8(%s), Ba = lo8(%s), Ca = lo8(%s), Da = %s;\n", kAlphaArg[a], kAlphaArg[b],
			    kAlphaArg[c], kAlphaArg[d]);
			if (bias != 3) {
				add(s, "    float ra = Da %c lerp1(Aa, Ba, Ca)%s;\n", op ? '-' : '+',
				    bias == 1 ? " + 128.0" : bias == 2 ? " - 128.0" : "");
				if (scale == 1) s += "    ra *= 2.0;\n";
				else if (scale == 2) s += "    ra *= 4.0;\n";
				else if (scale == 3) s += "    ra = floor(ra / 2.0);\n";
			} else {
				// as gx_shader.cpp: the colour arguments' A/B of this stage for the wider compares
				uint32_t mode = (scale << 1) | op;
				const char* cond;
				switch (mode) {
				case 0: cond = "A.r > B.r"; break;
				case 1: cond = "A.r == B.r"; break;
				case 2: cond = "(A.g * 256.0 + A.r) > (B.g * 256.0 + B.r)"; break;
				case 3: cond = "(A.g * 256.0 + A.r) == (B.g * 256.0 + B.r)"; break;
				case 4: cond = "(A.b * 65536.0 + A.g * 256.0 + A.r) > (B.b * 65536.0 + B.g * 256.0 + B.r)"; break;
				case 5: cond = "(A.b * 65536.0 + A.g * 256.0 + A.r) == (B.b * 65536.0 + B.g * 256.0 + B.r)"; break;
				case 6: cond = "Aa > Ba"; break;
				default: cond = "Aa == Ba"; break;
				}
				add(s, "    float ra = Da + ((%s) ? Ca : 0.0);\n", cond);
			}
			add(s, "    %s.a = %s;\n", kRegName[dest], clampv ? "clamp(ra, 0.0, 255.0)" : "clamp(ra, -1024.0, 1023.0)");
			lastA = dest;
		}
		s += "  }\n";
	}
	// the last stage's output keeps its low 8 bits, as on the console (gx_shader.cpp: Bianco's river)
	add(s, "  float4 outc = float4(lo8(%s.rgb), lo8(%s.a));\n", kRegName[lastDest], kRegName[lastA]);
	{
		uint32_t c0 = k.alphaFunc & 7, c1 = (k.alphaFunc >> 3) & 7, op = (k.alphaFunc >> 6) & 3;
		static const char* ops[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
		std::string a, b;
		char buf[64];
		if (c0 == 0 || c0 == 7) a = c0 ? "true" : "false";
		else { _snprintf(buf, sizeof buf, "(outc.a %s u_alpharef.x)", ops[c0]); a = buf; }
		if (c1 == 0 || c1 == 7) b = c1 ? "true" : "false";
		else { _snprintf(buf, sizeof buf, "(outc.a %s u_alpharef.y)", ops[c1]); b = buf; }
		std::string e;
		switch (op) {
		case 0: e = a + " && " + b; break;
		case 1: e = a + " || " + b; break;
		case 2: e = a + " != " + b; break;
		default: e = a + " == " + b; break;
		}
		// the Xenos microcode compiler has no `discard`: clip() is its texkill
		if (!(c0 == 7 && c1 == 7 && op <= 1)) s += "  clip((" + e + ") ? 1.0 : -1.0);\n";
	}
	s += "  float4 color = outc / 255.0;\n";
	if (k.fogType) {
		// u_fog: x = A, y = B, z = C, w = 1 for orthographic; the depth is the pixel's own z / w
		s += "  float fz = i.tc0.w / i.tc1.w;\n"
		     "  float ze = u_fog.w != 0.0 ? u_fog.x * fz : u_fog.x / (u_fog.y - fz);\n"
		     "  float f = saturate(ze - u_fog.z);\n";
		switch (k.fogType) {
		case 4: s += "  f = 1.0 - exp2(-8.0 * f);\n"; break;
		case 5: s += "  f = 1.0 - exp2(-8.0 * f * f);\n"; break;
		case 6: s += "  f = exp2(-8.0 * (1.0 - f));\n"; break;
		case 7: s += "  f = exp2(-8.0 * (1.0 - f) * (1.0 - f));\n"; break;
		default: break;
		}
		s += "  color.rgb = lerp(color.rgb, u_fogcolor.rgb, f);\n";
	}
	// SMS_XE_DEBUG 64: the rasterised colour 0 instead; 128: TEV stage 0's texture as sampled
	static int dbg = -1;
	if (dbg < 0) dbg = getenv("SMS_XE_DEBUG") ? atoi(getenv("SMS_XE_DEBUG")) : 0;
	if (dbg & 64) s += "  color = float4(saturate(i.c0.rgb), 1.0);\n";
	if (dbg & 2048) s += "  color = float4(ras0.rgb / 255.0, 1.0);\n";
	if (dbg & 4096) s += "  color = float4(saturate(cprev.rgb / 255.0), 1.0);\n";
	if (dbg & 8192) s += "  color = float4(outc.aaa / 255.0, 1.0);\n";
	if (dbg & 512) s += "  color = float4(u_konst[0].rgb / 255.0, 1.0);\n";
	if (dbg & 1024) s += "  color = float4(u_tevreg[1].aaa / 255.0, 1.0);\n";
	if ((dbg & 128) && k.numStages) add(s, "  color = float4(tex2D(s%u, proj(vtc[%u])).rgb, 1.0);\n", k.order[0] & 7, (k.order[0] >> 3) & 7);
	s += "  return color;\n}\n";
	return s;
}

// ------------------------------------------------------------------ compile and cache
// Compiled shaders are kept on disk by their source's hash (game:\\data\\shaders, SMS_XE_SHADER_CACHE=0
// off): D3DXCompileShader on the console's CPU takes long enough to hitch a frame per new shader.
uint32_t g_xeCompiles, g_xeCacheHits;
double g_xeCompileSeconds;

static bool shaderCacheOn()
{
	static int on = -1;
	if (on < 0) {
		on = !(getenv("SMS_XE_SHADER_CACHE") && getenv("SMS_XE_SHADER_CACHE")[0] == '0');
		if (on) {
			CreateDirectoryA("game:\\data", NULL);
			CreateDirectoryA("game:\\data\\shaders", NULL);
		}
	}
	return on != 0;
}

// The cache is one file, game:\\data\\shaders\\pack.bin, read whole at the first shader: a record per
// shader (its source's hash, the profile's letter, the size, the code), appended as shaders compile.
// Opening a file per shader (as the cache first kept them) cost 20-70 ms frames on the console's
// disk whenever new objects appeared; the per-shader files a cache of that time left are read into
// the pack once.
static std::unordered_map<uint64_t, std::vector<uint8_t> > s_pack;
static const char kPack[] = "game:\\data\\shaders\\pack.bin";

static uint64_t packKey(uint64_t hash, char profile) { return hash ^ (uint64_t(uint8_t(profile)) << 56); }

static void packAppend(FILE* f, uint64_t hash, char profile, const void* code, uint32_t n)
{
	fwrite(&hash, 8, 1, f);
	fwrite(&profile, 1, 1, f);
	fwrite(&n, 4, 1, f);
	fwrite(code, 1, n, f);
}

static void packLoad()
{
	static bool loaded;
	if (loaded) return;
	loaded = true;
	if (FILE* f = fopen(kPack, "rb")) {
		fseek(f, 0, SEEK_END);
		const long size = ftell(f);
		fseek(f, 0, SEEK_SET);
		std::vector<uint8_t> all(size_t(size > 0 ? size : 0));
		const size_t got = all.empty() ? 0 : fread(&all[0], 1, all.size(), f);
		fclose(f);
		size_t at = 0;
		while (at + 13 <= got) {
			uint64_t hash;
			uint32_t n;
			memcpy(&hash, &all[at], 8);
			const char profile = char(all[at + 8]);
			memcpy(&n, &all[at + 9], 4);
			if (at + 13 + n > got) break;  // a record cut short: the rest compiles again
			s_pack[packKey(hash, profile)].assign(all.begin() + (at + 13), all.begin() + (at + 13 + n));
			at += 13 + n;
		}
		if (at < got)  // cut off there, or later records would be appended after the break, unread
			if (FILE* w = fopen(kPack, "wb")) {
				if (at) fwrite(&all[0], 1, at, w);
				fclose(w);
			}
		logmsg("shader cache: %u shaders", unsigned(s_pack.size()));
		return;
	}
	// the files of a cache from before the pack, into a new pack
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA("game:\\data\\shaders\\*.*", &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	FILE* out = fopen(kPack, "wb");
	do {
		unsigned long long hash;
		char profile;
		if (sscanf(fd.cFileName, "%16llx.%c", &hash, &profile) != 2) continue;
		char path[64];
		_snprintf(path, sizeof path, "game:\\data\\shaders\\%s", fd.cFileName);
		path[sizeof path - 1] = 0;
		if (FILE* f = fopen(path, "rb")) {
			std::vector<uint8_t>& code = s_pack[packKey(hash, profile)];
			code.resize(fd.nFileSizeLow);
			if (code.empty() || fread(&code[0], 1, code.size(), f) != code.size()) s_pack.erase(packKey(hash, profile));
			else if (out) packAppend(out, hash, profile, &code[0], uint32_t(code.size()));
			fclose(f);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	if (out) fclose(out);
	logmsg("shader cache: %u shaders moved into the pack", unsigned(s_pack.size()));
}

static ID3DXBuffer* compile(const std::string& src, const char* profile)
{
	const uint64_t hash = hashBytes(src.data(), src.size());
	ID3DXBuffer* code = NULL;
	if (shaderCacheOn()) {
		packLoad();
		// each source is compiled once (makeProgram keeps its shader): its code is not needed again
		std::unordered_map<uint64_t, std::vector<uint8_t> >::iterator it = s_pack.find(packKey(hash, profile[0]));
		if (it != s_pack.end() && SUCCEEDED(D3DXCreateBuffer(DWORD(it->second.size()), &code))) {
			memcpy(code->GetBufferPointer(), &it->second[0], it->second.size());
			s_pack.erase(it);
			g_xeCacheHits++;
			return code;
		}
	}
	LARGE_INTEGER t0, t1, freq;
	QueryPerformanceCounter(&t0);
	ID3DXBuffer* errors = NULL;
	HRESULT hr = D3DXCompileShader(src.c_str(), UINT(src.size()), NULL, NULL, "main", profile, 0, &code, &errors, NULL);
	QueryPerformanceCounter(&t1);
	QueryPerformanceFrequency(&freq);
	g_xeCompiles++;
	g_xeCompileSeconds += double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
	if (FAILED(hr) || !code) {
		logmsg("%s compile failed (%08x): %s\n--- source ---\n%s", profile, unsigned(hr),
		       errors ? (const char*)errors->GetBufferPointer() : "", src.c_str());
		if (errors) errors->Release();
		if (code) code->Release();
		return NULL;
	}
	if (errors) errors->Release();
	if (shaderCacheOn()) {
		const uint8_t* p = (const uint8_t*)code->GetBufferPointer();
		if (FILE* f = fopen(kPack, "ab")) {
			packAppend(f, hash, profile[0], p, uint32_t(code->GetBufferSize()));
			fclose(f);
		}
	}
	return code;
}

IDirect3DVertexShader9* compileVS(const char* src)
{
	ID3DXBuffer* code = compile(src, "vs_3_0");
	if (!code) return NULL;
	IDirect3DVertexShader9* vs = NULL;
	dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &vs);
	code->Release();
	return vs;
}

IDirect3DPixelShader9* compilePS(const char* src)
{
	ID3DXBuffer* code = compile(src, "ps_3_0");
	if (!code) return NULL;
	IDirect3DPixelShader9* ps = NULL;
	dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ps);
	code->Release();
	return ps;
}

struct KeyHash {
	size_t operator()(const Key& k) const { return size_t(hashBytes(&k, sizeof(k))); }
};
struct KeyEq {
	bool operator()(const Key& a, const Key& b) const { return sameWords(&a, &b, sizeof(a)); }
};
static std::unordered_map<Key, Program, KeyHash, KeyEq> s_programs;
// shaders by source: many state keys make the same vertex or pixel code
static std::unordered_map<uint64_t, IDirect3DVertexShader9*> s_vs;
static std::unordered_map<uint64_t, IDirect3DPixelShader9*> s_ps;
static Key s_lastKey;
static const Program* s_last = NULL;

// A program for key k, made now (its shaders from the cache, or compiled).
static std::unordered_map<Key, Program, KeyHash, KeyEq>::iterator makeProgram(const Key& k)
{
	Program p;
	std::string vsrc = genVS(k), psrc = genPS(k);
	uint64_t vh = hashBytes(vsrc.data(), vsrc.size()), ph = hashBytes(psrc.data(), psrc.size());
	std::unordered_map<uint64_t, IDirect3DVertexShader9*>::iterator v = s_vs.find(vh);
	p.vs = v != s_vs.end() ? v->second : (s_vs[vh] = compileVS(vsrc.c_str()));
	std::unordered_map<uint64_t, IDirect3DPixelShader9*>::iterator q = s_ps.find(ph);
	p.ps = q != s_ps.end() ? q->second : (s_ps[ph] = compilePS(psrc.c_str()));
	p.id = int(s_programs.size());
	{  // SMS_XE_DUMP_PROG=n: program n's sources in the log
		static int dump = -2;
		if (dump == -2) dump = getenv("SMS_XE_DUMP_PROG") ? atoi(getenv("SMS_XE_DUMP_PROG")) : -1;
		if (dump == p.id) logmsg("program %d\n--- vs ---\n%s\n--- ps ---\n%s", p.id, vsrc.c_str(), psrc.c_str());
	}
	return s_programs.insert(std::make_pair(k, p)).first;
}

// The keys of every program made, in game:\\data\\shaders\\programs.bin (after a header: its
// format, the key's size), are made again at the start (programWarmup): a new program costs its
// sources and shaders, ~20 ms in a frame where several appear at once.
static const char kPrograms[] = "game:\\data\\shaders\\programs.bin";
enum { kProgramsMagic = 0x58505231 };  // XPR1

// The file with this format's header, open for reading, or NULL. A key cut short (power off
// mid-write) would shift every later one: the file is cut back to its whole keys first.
static FILE* openPrograms()
{
	FILE* f = fopen(kPrograms, "rb");
	if (!f) return NULL;
	uint32_t head[2] = {0, 0};
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size < 8 || fread(head, 4, 2, f) != 2 || head[0] != kProgramsMagic || head[1] != sizeof(Key)) {
		fclose(f);
		return NULL;
	}
	const long whole = 8 + (size - 8) / long(sizeof(Key)) * long(sizeof(Key));
	if (whole == size) return f;
	std::vector<uint8_t> keep(size_t(whole - 8));
	const bool got = keep.empty() || fread(&keep[0], 1, keep.size(), f) == keep.size();
	fclose(f);
	if (!got) return NULL;
	if (FILE* w = fopen(kPrograms, "wb")) {
		fwrite(head, 4, 2, w);
		if (!keep.empty()) fwrite(&keep[0], 1, keep.size(), w);
		fclose(w);
	}
	f = fopen(kPrograms, "rb");
	if (f) fseek(f, 8, SEEK_SET);
	return f;
}

void programWarmup()
{
	if (!shaderCacheOn()) return;
	FILE* f = openPrograms();
	if (!f) return;
	const double t0 = monoSeconds();
	Key k;
	while (fread(&k, sizeof k, 1, f) == 1)
		if (s_programs.find(k) == s_programs.end()) makeProgram(k);
	fclose(f);
	logmsg("shader warm-up: %u programs in %.2f s", unsigned(s_programs.size()), monoSeconds() - t0);
}

static void recordProgram(const Key& k)
{
	if (!shaderCacheOn()) return;
	static bool checked;
	if (!checked) {  // a file of another format, or cut short, starts over
		checked = true;
		if (FILE* f = openPrograms()) {
			fclose(f);
		} else if (FILE* w = fopen(kPrograms, "wb")) {
			const uint32_t h[2] = {kProgramsMagic, uint32_t(sizeof(Key))};
			fwrite(h, 4, 2, w);
			fclose(w);
		}
	}
	if (FILE* f = fopen(kPrograms, "ab")) {
		fwrite(&k, sizeof k, 1, f);
		fclose(f);
	}
}

const Program* programFor(uint32_t fmt)
{
	Key k;
	buildKey(k, fmt);
	if (s_last && sameWords(&k, &s_lastKey, sizeof(k))) return s_last;
	std::unordered_map<Key, Program, KeyHash, KeyEq>::iterator it = s_programs.find(k);
	if (it == s_programs.end()) {
		it = makeProgram(k);
		recordProgram(k);
	}
	s_lastKey = k;
	s_last = it->second.vs && it->second.ps ? &it->second : NULL;
	return s_last;
}

}  // namespace xe
}  // namespace gx
