// Other players drawn in this game (mp_puppet.hpp).
#include "mp_puppet.hpp"
#include "mp_mirror.hpp"
#include "mp_pvp.hpp"
#include "../mp_recolour.h"

#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DMaterialAnm.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/Components/J3DGXColorS10.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DAnmLoader.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JAudio/JAInterface/JAIBasic.hpp>
#include <JSystem/JAudio/JAInterface/JAIConst.hpp>
#include <JSystem/JAudio/JAInterface/JAISound.hpp>
#include <JSystem/JKernel/JKRDvdRipper.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <M3DUtil/M3UJoint.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorData.hpp>
#include <M3DUtil/MActorUtil.hpp>
#include <MSound/MSound.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <MarioUtil/LightUtil.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/PacketUtil.hpp>
#include <MarioUtil/TexUtil.hpp>
#include <MoveBG/MapObjBase.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAnimeData.hpp>
#include <Player/MarioDirtyStrings.hpp>
#include <Player/WaterGun.hpp>
#include <Player/Yoshi.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/MarDirector.hpp>
#include <dolphin/dvd.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/vi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const u32 kMarioLoadFlags = J3DMLF_MaterialPEFull | (16 << J3DMLF_TevStageNumShift);
const int kMarioAnims     = 199;
const int kMarioTexPatterns = 0x18;

// A copy of a loaded file, so changes to it (a recoloured texture, another
// texture header) leave the original alone. The game's loaders read model
// files in place, and two loads of one file share its texture headers.
// From `archive` (a body's, below), else from the mounted volumes.
void* copyResource(const char* path, JKRArchive* archive = nullptr)
{
	void* res = archive ? archive->getResource(path) : JKRFileLoader::getGlbResource(path);
	if (!res)
		return nullptr;
	long size = archive ? archive->getResSize(res) : JKRFileLoader::getResSize(res, nullptr);
	if (size <= 0)
		return nullptr;
	void* copy = JKRHeap::alloc((u32)size, 32, nullptr);
	if (!copy)
		return nullptr;
	memcpy(copy, res, (size_t)size);
	return copy;
}

J3DModelData* loadCopy(const char* path, u32 flags, JKRArchive* archive = nullptr)
{
	void* copy = copyResource(path, archive);
	return copy ? J3DModelLoaderDataBase::load(copy, flags) : nullptr;
}

// ---------------------------------------------------------------- bodies
// Super Mario Eclipse's characters: its character select mounts the chosen
// one's archive as "mario" (Eclipse's initCharacterArchives), so "/mario/..."
// is always our own player's. Another player's body comes from their
// character's archive on the disc, read once a stage into the stage's memory
// (it goes with the stage) and kept out of the mounted volumes.
const char* const kBodyArchives[MPG_BODIES] = {"/data/mario.szs", "/data/luigi.szs", "/data/piantissimo.szs",
                                               "/data/shadow_mario.szs"};

// The disc has Eclipse's characters.
bool eclipseBodies()
{
	static int has = -1;
	if (has < 0)
		has = DVDConvertPathToEntrynum((char*)kBodyArchives[MPG_BODY_LUIGI]) >= 0;
	return has != 0;
}

class TMpBodyArchive : public JKRMemArchive {
public:
	explicit TMpBodyArchive(void* data)
	    : JKRMemArchive(data, 0, MBF_0)
	{
		// mounted (the archive reads its files only so), but not a volume:
		// it is named "mario" too
		getVolumeList().remove(&mFileLoaderLink);
	}
};

// A body's archive and its face's texture patterns (TMario::initModel's, for
// its own model's materials and textures), shared by the stage's puppets.
struct BodyFiles {
	JKRArchive* archive;
	J3DAnmTexPattern** patterns;
	J3DTexNoAnm** texNo;
};
BodyFiles s_bodyFiles[MPG_BODIES];
unsigned s_bodyFilesStage;

BodyFiles* bodyFiles(int body)
{
	if (s_bodyFilesStage != MpStageNumber()) {
		memset(s_bodyFiles, 0, sizeof s_bodyFiles);
		s_bodyFilesStage = MpStageNumber();
	}
	BodyFiles& f = s_bodyFiles[body];
	if (!f.archive) {
		void* data = JKRDvdRipper::loadToMainRAM(kBodyArchives[body], nullptr, EXPAND_SWITCH_DECOMPRESS, 0, nullptr,
		                                         JKRDvdRipper::ALLOC_DIRECTION_FORWARD, 0, nullptr);
		if (!data) {
			fprintf(stderr, "[multiplayer] could not read %s\n", kBodyArchives[body]);
			return nullptr;
		}
		f.archive = new TMpBodyArchive(data);
	}
	return &f;
}

} // namespace

int MpLocalBody()
{
	if (!eclipseBodies())
		return MPG_BODY_MARIO;
	// a file only that character's archive has (Shadow Mario's has Il
	// Piantissimo's animation too)
	if (JKRFileLoader::getGlbResource("/mario/custom/ma_mdl1.btk"))
		return MPG_BODY_SHADOW_MARIO;
	if (JKRFileLoader::getGlbResource("/mario/btk/watergun_water.btk"))
		return MPG_BODY_PIANTISSIMO;
	if (JKRFileLoader::getGlbResource("/mario/bck/ma_jump.bck"))
		return MPG_BODY_LUIGI;
	return MPG_BODY_MARIO;
}

namespace {

// A body's file: "/mario/bmd/ma_mdl1.bmd" is "/bmd/ma_mdl1.bmd" in its archive.
J3DModelData* loadBodyFile(const char* path, u32 flags, JKRArchive* archive)
{
	return loadCopy(archive ? path + strlen("/mario") : path, flags, archive);
}

// Mario's face patterns from a body's archive, bound to that body's model.
void loadPatterns(BodyFiles& f, J3DModelData* model)
{
	if (f.patterns)
		return;
	f.patterns = new J3DAnmTexPattern*[kMarioTexPatterns];
	f.texNo    = new J3DTexNoAnm*[kMarioTexPatterns];
	for (int i = 0; i < kMarioTexPatterns; i++) {
		// "/mario/btp/...": the same file in the body's archive
		const char* file = marioAnimeTexPatternFilenames[i] + strlen("/mario");
		void* data       = f.archive->getResource(file);
		f.patterns[i]    = data ? (J3DAnmTexPattern*)J3DAnmLoaderDataBase::load(data) : nullptr;
		const u16 n      = f.patterns[i] ? f.patterns[i]->getUpdateMaterialNum() : 0;
		if (f.patterns[i])
			f.patterns[i]->searchUpdateMaterialID(model);
		f.texNo[i] = new J3DTexNoAnm[n ? n : 1];
		for (u16 j = 0; j < n; j++) {
			f.texNo[i][j].setAnmIndex(j);
			f.texNo[i][j].setAnmTexPattern(f.patterns[i]);
		}
	}
}

// ---------------------------------------------------------------- recolouring
// Mario's clothes are colours in his texture (H_ma_new_main_s3tc, CMPR), not
// material colours: a character is that texture with its colours changed.
// CMPR keeps two RGB565 colours a 4x4 block and mixes the rest from them, so
// changing the two changes the block.

void unpack565(u16 c, int& r, int& g, int& b)
{
	r = (c >> 11) & 31;
	g = (c >> 5) & 63;
	b = c & 31;
	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
}

u16 pack565(int r, int g, int b)
{
	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;
	return (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

using mp_recolour::recolour;

void recolourTexture(ResTIMG* timg, int character)
{
	if (!timg || character == MPG_CHAR_MARIO || timg->format != GX_TF_CMPR)
		return;
	u8* data          = (u8*)timg + timg->imageDataOffset;
	const u32 blocks  = (u32)timg->width * timg->height / 16;
	for (u32 i = 0; i < blocks; i++) {
		u8* blk     = data + i * 8;
		u16 c0      = (u16)(blk[0] << 8 | blk[1]);
		u16 c1      = (u16)(blk[2] << 8 | blk[3]);
		u32 idx     = (u32)blk[4] << 24 | (u32)blk[5] << 16 | (u32)blk[6] << 8 | blk[7];
		bool four   = c0 > c1; // four colours, or three and transparent
		int r, g, b;
		unpack565(c0, r, g, b);
		recolour(character, r, g, b);
		u16 n0 = pack565(r, g, b);
		unpack565(c1, r, g, b);
		recolour(character, r, g, b);
		u16 n1 = pack565(r, g, b);
		if (four ? n0 < n1 : n0 > n1) {
			// keep the block's mode: swap the colours and the indices naming them
			u16 t = n0;
			n0    = n1;
			n1    = t;
			if (four) {
				idx ^= 0x55555555;
			} else {
				u32 out = 0;
				for (int p = 0; p < 16; p++) {
					u32 v = (idx >> (p * 2)) & 3;
					if (v < 2)
						v ^= 1;
					out |= v << (p * 2);
				}
				idx = out;
			}
		}
		if (four && n0 == n1) {
			// equal colours would read as three-and-transparent
			if (n0 == 0xFFFF)
				n1--;
			else
				n0++;
		}
		blk[0] = (u8)(n0 >> 8);
		blk[1] = (u8)n0;
		blk[2] = (u8)(n1 >> 8);
		blk[3] = (u8)n1;
		blk[4] = (u8)(idx >> 24);
		blk[5] = (u8)(idx >> 16);
		blk[6] = (u8)(idx >> 8);
		blk[7] = (u8)idx;
	}
	DCFlushRange(data, blocks * 8);
}

ResTIMG* mainTexture(J3DModelData* data)
{
	if (!data || !data->getTexture() || data->getTexture()->getNum() == 0)
		return nullptr;
	return data->getTexture()->getResTIMG(0);
}

// No goop: the dirt texture's share (the first konst colour's alpha, which
// TMario::addDirty sets for Mario) is nothing.
void clearDirt(J3DModelData* data)
{
	for (u16 i = 0; i < data->getMaterialNum(); i++)
		if (J3DGXColor* k = data->getMaterialNodePointer(i)->getTevBlock()->getTevKColor(0))
			k->color.a = 0;
}

void setShapes(J3DModelData* data, bool hidden)
{
	for (u16 i = 0; i < data->getShapeNum(); i++) {
		if (hidden)
			data->getShapeNodePointer(i)->onFlag(J3DShpFlag_Visible);
		else
			data->getShapeNodePointer(i)->offFlag(J3DShpFlag_Visible);
	}
}

} // namespace

// Rotation matrix <-> quaternion (x, y, z, w), for turning smoothly.
void MpMtxToQuat(const f32 (*m)[4], f32 q[4])
{
	const f32 tr = m[0][0] + m[1][1] + m[2][2];
	if (tr > 0.0f) {
		const f32 s = sqrtf(tr + 1.0f) * 2.0f;
		q[3] = 0.25f * s;
		q[0] = (m[2][1] - m[1][2]) / s;
		q[1] = (m[0][2] - m[2][0]) / s;
		q[2] = (m[1][0] - m[0][1]) / s;
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		const f32 s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
		q[3] = (m[2][1] - m[1][2]) / s;
		q[0] = 0.25f * s;
		q[1] = (m[0][1] + m[1][0]) / s;
		q[2] = (m[0][2] + m[2][0]) / s;
	} else if (m[1][1] > m[2][2]) {
		const f32 s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
		q[3] = (m[0][2] - m[2][0]) / s;
		q[0] = (m[0][1] + m[1][0]) / s;
		q[1] = 0.25f * s;
		q[2] = (m[1][2] + m[2][1]) / s;
	} else {
		const f32 s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
		q[3] = (m[1][0] - m[0][1]) / s;
		q[0] = (m[0][2] + m[2][0]) / s;
		q[1] = (m[1][2] + m[2][1]) / s;
		q[2] = 0.25f * s;
	}
}

namespace {

void toQuat(const f32 (*m)[4], f32 q[4]) { MpMtxToQuat(m, q); }

void fromQuat(const f32 q[4], f32 (*m)[4])
{
	const f32 x = q[0], y = q[1], z = q[2], w = q[3];
	m[0][0] = 1 - 2 * (y * y + z * z), m[0][1] = 2 * (x * y - z * w), m[0][2] = 2 * (x * z + y * w);
	m[1][0] = 2 * (x * y + z * w), m[1][1] = 1 - 2 * (x * x + z * z), m[1][2] = 2 * (y * z - x * w);
	m[2][0] = 2 * (x * z - y * w), m[2][1] = 2 * (y * z + x * w), m[2][2] = 1 - 2 * (x * x + y * y);
}

// Their scale (the matrix's columns' lengths), kept apart from the turn.
f32 columnLength(const f32 (*m)[4], int c) { return sqrtf(m[0][c] * m[0][c] + m[1][c] * m[1][c] + m[2][c] * m[2][c]); }

void blendMatrix(const f32 (*a)[4], const f32 (*b)[4], f32 t, f32 (*out)[4])
{
	f32 sa[3], sb[3], ra[3][4], rb[3][4];
	for (int c = 0; c < 3; c++) {
		sa[c] = columnLength(a, c), sb[c] = columnLength(b, c);
		for (int r = 0; r < 3; r++) {
			ra[r][c] = sa[c] > 1e-6f ? a[r][c] / sa[c] : 0.0f;
			rb[r][c] = sb[c] > 1e-6f ? b[r][c] / sb[c] : 0.0f;
		}
	}
	f32 qa[4], qb[4], q[4];
	toQuat(ra, qa);
	toQuat(rb, qb);
	f32 dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
	if (dot < 0.0f) { // the short way round
		for (int i = 0; i < 4; i++)
			qb[i] = -qb[i];
		dot = -dot;
	}
	f32 wa = 1.0f - t, wb = t;
	if (dot < 0.9995f) {
		const f32 angle = acosf(dot), sine = sinf(angle);
		wa = sinf((1.0f - t) * angle) / sine;
		wb = sinf(t * angle) / sine;
	}
	f32 len = 0.0f;
	for (int i = 0; i < 4; i++) {
		q[i] = qa[i] * wa + qb[i] * wb;
		len += q[i] * q[i];
	}
	len = sqrtf(len);
	for (int i = 0; i < 4; i++)
		q[i] /= len > 1e-6f ? len : 1.0f;
	fromQuat(q, out);
	for (int c = 0; c < 3; c++) {
		const f32 scale = sa[c] + (sb[c] - sa[c]) * t;
		for (int r = 0; r < 3; r++)
			out[r][c] *= scale;
	}
	for (int r = 0; r < 3; r++)
		out[r][3] = a[r][3] + (b[r][3] - a[r][3]) * t;
}

// A frame between two of the same looping animation: forwards, over the
// loop's end when the later one is smaller.
f32 blendFrame(f32 a, f32 b, f32 t, f32 length)
{
	if (b >= a || length <= 0.0f)
		return a + (b - a) * t;
	f32 f = a + (b + length - a) * t;
	return f >= length ? f - length : f;
}

// A 16-bit angle between two, the short way round.
int16_t blendAngle(int16_t a, int16_t b, f32 t) { return (int16_t)(a + (int16_t)(b - a) * t); }

// A joint's turn (MpgPose::headTurn) between two, the short way round.
void blendTurn(const int16_t* a, const int16_t* b, f32 t, int16_t* out)
{
	const f32 sign = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0 ? -1.0f : 1.0f;
	f32 q[4], len = 0.0f;
	for (int i = 0; i < 4; i++) {
		q[i] = a[i] + (b[i] * sign - a[i]) * t;
		len += q[i] * q[i];
	}
	len = sqrtf(len);
	for (int i = 0; i < 4; i++)
		out[i] = len > 1.0f ? (int16_t)(q[i] / len * 32767.0f) : (i == 3 ? 32767 : 0);
}

// The puppet whose models are being posed now (their joint callbacks read its pose).
TMpPuppet* s_posing;

// Turns the joint being calculated by a pose's turn (MpgPose::headTurn).
void applyTurn(const int16_t* turn)
{
	if (turn[0] == 0 && turn[1] == 0 && turn[2] == 0)
		return;
	f32 q[4], len = 0.0f;
	for (int i = 0; i < 4; i++) {
		q[i] = turn[i] / 32767.0f;
		len += q[i] * q[i];
	}
	len = sqrtf(len);
	for (int i = 0; i < 4; i++)
		q[i] /= len > 1e-6f ? len : 1.0f;
	Mtx m;
	MTXIdentity(m);
	fromQuat(q, m);
	MTXConcat(J3DSys::mCurrentMtx, m, J3DSys::mCurrentMtx);
}

// Turns the joint being calculated about its z axis (as TWaterGun's callbacks do).
void rollJoint(f32 x, f32 z)
{
	Mtx m;
	MsMtxSetRotRPH(m, x, 0.0f, z);
	MTXConcat(J3DSys::mCurrentMtx, m, J3DSys::mCurrentMtx);
}

void hookJoint(J3DModelData* data, const char* name, J3DNodeCallBack ctrl)
{
	if (!data)
		return;
	const int joint = data->getJointName()->getIndex(name);
	if (joint >= 0 && joint < data->getJointNum())
		data->getJointNodePointer((u16)joint)->setCallBack(ctrl);
}

} // namespace

// What Mario's joint callbacks do to his models (MarioDraw.cpp, WaterGun.cpp,
// Yoshi.cpp), done to the puppet's from the pose: those read the game's own
// Mario, these the player's pose.
BOOL TMpPuppet::turnHead(J3DNode*, BOOL after)
{
	if (!after && s_posing)
		applyTurn(s_posing->mPose.headTurn);
	return TRUE;
}

BOOL TMpPuppet::turnWaist(J3DNode*, BOOL after)
{
	if (!after && s_posing)
		applyTurn(s_posing->mPose.waistTurn);
	return TRUE;
}

BOOL TMpPuppet::aimNozzle(J3DNode*, BOOL after)
{
	if (!after && s_posing && s_posing->mPose.gunAngle < 0)
		rollJoint(0.0f, s_posing->mPose.gunAngle * (360.0f / 65536.0f));
	return TRUE;
}

BOOL TMpPuppet::tiltHoverL(J3DNode*, BOOL after)
{
	if (!after && s_posing)
		rollJoint(0.0f, (int16_t)-s_posing->mPose.hoverTiltL * (360.0f / 65536.0f));
	return TRUE;
}

BOOL TMpPuppet::tiltHoverR(J3DNode*, BOOL after)
{
	if (!after && s_posing)
		rollJoint(0.0f, (int16_t)-s_posing->mPose.hoverTiltR * (360.0f / 65536.0f));
	return TRUE;
}

BOOL TMpPuppet::turnProp(J3DNode*, BOOL after)
{
	if (!after && s_posing)
		rollJoint(s_posing->mPose.propAngle * (360.0f / 65536.0f), 0.0f);
	return TRUE;
}

BOOL TMpPuppet::aimYoshi(J3DNode*, BOOL after)
{
	if (!after && s_posing)
		rollJoint(0.0f, s_posing->mPose.gunAngle * (360.0f / 65536.0f));
	return TRUE;
}

// Its own models' joints, turned from the pose as Mario's are by the game
// (the body's as each body is made: buildBody).
void TMpPuppet::hookJoints()
{
	// FLUDD's nozzles: only this puppet's own copies (shared ones keep Mario's)
	if (mNozzlesOwn) {
		if (mNozzles[TWaterGun::Spray])
			hookJoint(mNozzles[TWaterGun::Spray]->getModel()->getModelData(), "chn_muzzle_1", &aimNozzle);
		if (mNozzles[TWaterGun::Hover]) {
			hookJoint(mNozzles[TWaterGun::Hover]->getModel()->getModelData(), "jnt_nozzle_L", &tiltHoverL);
			hookJoint(mNozzles[TWaterGun::Hover]->getModel()->getModelData(), "jnt_nozzle_R", &tiltHoverR);
		}
		if (mNozzles[TWaterGun::Turbo]) {
			hookJoint(mNozzles[TWaterGun::Turbo]->getModel()->getModelData(), "chn_back_nozzle_prop", &turnProp);
			hookJoint(mNozzles[TWaterGun::Turbo]->getModel()->getModelData(), "jnt_back_nozzle_neck", &aimNozzle);
		}
	}
	// the puppets' Yoshi (one model data for all of them): his head, as TYoshi's
	if (mYoshi)
		mYoshi->getModel()->getModelData()->getJointNodePointer(21)->setCallBack(&aimYoshi);
}

void TMpPuppet::blendPoses(const MpgPose& a, const MpgPose& b, f32 t, MpgPose* out)
{
	// what is not in between (a nozzle, flags, which animation) is the
	// nearer pose's; the water is always the newer pose's
	*out = t < 0.5f ? a : b;
	out->flags    = (a.flags & ~MPG_POSE_SPRAYING) | (b.flags & MPG_POSE_SPRAYING);
	out->emitters = b.emitters;
	memcpy(out->emitPos, b.emitPos, sizeof out->emitPos);
	memcpy(out->emitDir, b.emitDir, sizeof out->emitDir);
	memcpy(out->emitSpeed, b.emitSpeed, sizeof out->emitSpeed);
	out->pressure = b.pressure;
	if (t <= 0.0f)
		return;
	// a jump between poses (a warp, a respawn) is not crossed smoothly
	const f32 dx = b.mtx[0][3] - a.mtx[0][3], dy = b.mtx[1][3] - a.mtx[1][3], dz = b.mtx[2][3] - a.mtx[2][3];
	if (dx * dx + dy * dy + dz * dz > 800.0f * 800.0f)
		return;
	blendMatrix(a.mtx, b.mtx, t, out->mtx);
	M3UModelMario* pm = mModel;
	if (a.bodyBck == b.bodyBck && a.bodyBck < kMarioAnims && pm->unk4->unk4[a.bodyBck]) {
		out->bodyFrame = blendFrame(a.bodyFrame, b.bodyFrame, t, pm->unk4->unk4[a.bodyBck]->getFrameMax());
		out->bodyBlend = a.bodyBlend + (b.bodyBlend - a.bodyBlend) * t;
	}
	if (a.upperBck == b.upperBck && a.upperBck < kMarioAnims && pm->unk4->unk4[a.upperBck]
	    && (a.flags & b.flags & MPG_POSE_UPPER))
		out->upperFrame = blendFrame(a.upperFrame, b.upperFrame, t, pm->unk4->unk4[a.upperBck]->getFrameMax());
	if (a.btp == b.btp && a.btp < kMarioTexPatterns && pm->unk4->unk8[a.btp])
		out->btpFrame = blendFrame(a.btpFrame, b.btpFrame, t, pm->unk4->unk8[a.btp]->getFrameMax());
	if (a.fluddBck == b.fluddBck && b.fluddFrame >= a.fluddFrame)
		out->fluddFrame = a.fluddFrame + (b.fluddFrame - a.fluddFrame) * t;
	if (a.yoshiBck == b.yoshiBck && b.yoshiFrame >= a.yoshiFrame)
		out->yoshiFrame = a.yoshiFrame + (b.yoshiFrame - a.yoshiFrame) * t;
	blendTurn(a.headTurn, b.headTurn, t, out->headTurn);
	blendTurn(a.waistTurn, b.waistTurn, t, out->waistTurn);
	out->gunAngle   = blendAngle(a.gunAngle, b.gunAngle, t);
	out->hoverTiltL = blendAngle(a.hoverTiltL, b.hoverTiltL, t);
	out->hoverTiltR = blendAngle(a.hoverTiltR, b.hoverTiltR, t);
	out->propAngle  = blendAngle(a.propAngle, b.propAngle, t);
}

TMpPuppet::TMpPuppet(int slot, bool self)
    : THitActor("<MpPuppet>")
    , mSlot(slot)
    , mCharacter(mpg_character(slot))
    , mSelf(self)
    , mActive(false)
    , mHaveSmoothed(false)
    , mLastSession(0)
    , mWaterCarry(0.0f)
    , mSpawnFrames(0)
    , mGoneFrames(1000)
    , mModel(nullptr)
    , mBodyData(nullptr)
    , mGlasses(nullptr)
    , mBody(MPG_BODY_MARIO)
    , mNoBody(0)
    , mHeap(nullptr)
    , mFludd(nullptr)
    , mFluddData(nullptr)
    , mNozzleJoint(0)
    , mNozzlesOwn(false)
    , mYoshi(nullptr)
    , mSurf(nullptr)
    , mCoaster(nullptr)
    , mRide(nullptr)
    , mOpa(nullptr)
    , mXlu(nullptr)
    , mSwapped(false)
    , mDressedVersion(0xFFFFFFFFu)
    , mDrawn(false)
    , mFxShotNext(0)
    , mFxSeq(0)
    , mFxSeen(false)
    , mSoundSeq(0)
    , mSoundSeen(false)
    , mFlushed(false)
{
	memset(mView, 0, sizeof mView);
	memset(mProjection, 0, sizeof mProjection);
	memset(&mPose, 0, sizeof mPose);
	memset(mHands, 0, sizeof mHands);
	memset(mCap, 0, sizeof mCap);
	memset(mBodies, 0, sizeof mBodies);
	memset(mNozzles, 0, sizeof mNozzles);
	memset(mSurfColor, 0, sizeof mSurfColor);
	mSaved[0] = mSaved[1] = nullptr;
	mSmoothed[0] = mSmoothed[1] = mSmoothed[2] = 0.0f;
	mHead[0] = mHead[1] = mHead[2] = 0.0f;
	// seen by nothing: it is a picture of a player whose game does the hitting
	mActorType = 0;
	onHitFlag(HIT_FLAG_NO_COLLISION);
}

// The models of `body` (MPG_BODY_*): ours from our own Mario's files, another
// player's from their character's archive with Eclipse (else Mario's, ours).
bool TMpPuppet::buildBody(int body)
{
	TMario* orig = gpMarioOriginal;
	if (!orig || !orig->getM3UModel())
		return false;
	BodyFiles* files = nullptr;
	if (!mSelf && eclipseBodies()) {
		files = bodyFiles(body);
		if (!files)
			return false;
	}
	JKRArchive* archive = files ? files->archive : nullptr;
	J3DModelData* const bodyData = loadBodyFile("/mario/bmd/ma_mdl1.bmd", kMarioLoadFlags, archive);
	if (!bodyData)
		return false;
	mBodyData   = bodyData;
	mJointChest = mBodyData->getJointName()->getIndex("jnt_chest");
	mJointHandR = mBodyData->getJointName()->getIndex("jnt_hand_R");
	mJointHandL = mBodyData->getJointName()->getIndex("jnt_hand_L");
	mJointHead  = mBodyData->getJointName()->getIndex("jnt_head");
	mJointMHead = mBodyData->getJointName()->getIndex("M_head");

	ResTIMG* pollution = (ResTIMG*)JKRFileLoader::getGlbResource(cDirtyFileName);
	if (pollution)
		SMS_ChangeTextureAll(mBodyData, cDirtyTexName, *pollution);
	// a character's colours are Mario's recoloured; Eclipse's others are their own
	if (body == MPG_BODY_MARIO)
		recolourTexture(mainTexture(mBodyData), mCharacter);
	J3DModel* model = new J3DModel(mBodyData, 0, 1);

	// hands and cap: their own copies, wearing the body's texture
	static const char* const handFiles[2][2] = {
		{"/mario/bmd/ma_hnd2r.bmd", "/mario/bmd/ma_hnd2l.bmd"},
		{"/mario/bmd/ma_hnd3r.bmd", "/mario/bmd/ma_hnd3l.bmd"},
	};
	for (int h = 0; h < 2; h++)
		for (int s = 0; s < 2; s++) {
			J3DModelData* data = loadBodyFile(handFiles[h][s], kMarioLoadFlags, archive);
			if (!data)
				return false;
			data->getTexture()->setResTIMG(0, *mainTexture(mBodyData));
			DCFlushRange(data->getTexture()->getResTIMG(0), sizeof(ResTIMG));
			if (pollution)
				SMS_ChangeTextureAll(data, cDirtyTexName, *pollution);
			clearDirt(data);
			mHands[h][s] = new J3DModel(data, kMarioLoadFlags, 1);
		}
	static const char* const capFiles[2] = {"/mario/bmd/ma_cap1.bmd", "/mario/bmd/ma_cap3.bmd"};
	for (int c = 0; c < 2; c++) {
		J3DModelData* data = loadBodyFile(capFiles[c], kMarioLoadFlags, archive);
		if (!data)
			return false;
		data->getTexture()->setResTIMG(0, *mainTexture(mBodyData));
		DCFlushRange(data->getTexture()->getResTIMG(0), sizeof(ResTIMG));
		if (pollution)
			SMS_ChangeTextureAll(data, cDirtyTexName, *pollution);
		clearDirt(data);
		mCap[c] = new J3DModel(data, 0, 1);
		SMS_MakeDLAndLock(mCap[c]);
	}
	// the sunglasses, as TMarioCap has them (on the cap's joint)
	J3DModelData* glasses = loadBodyFile("/mario/bmd/ma_glass1.bmd", kMarioLoadFlags, archive);
	mGlasses              = glasses ? new J3DModel(glasses, 0, 1) : nullptr;

	// the eyes blink and look by texture pattern, as Mario's do
	u16 eyes[2] = {(u16)mBodyData->getMaterialName()->getIndex("_mat_eye_L"),
	               (u16)mBodyData->getMaterialName()->getIndex("_mat_eye_R")};
	for (int e = 0; e < 2; e++)
		if (eyes[e] < mBodyData->getMaterialNum()) {
			mBodyData->getMaterialNodePointer(eyes[e])->change();
			mBodyData->getMaterialNodePointer(eyes[e])->setMaterialAnm(new J3DMaterialAnm());
		}
	if (files)
		loadPatterns(*files, mBodyData);

	// the animation machinery of Shadow Mario's (TEnemyMario::initModel):
	// Mario's own animations, this body's own blend and frame state, and its
	// face's patterns (our own Mario's, or its archive's for its own model)
	M3UModelCommon* origCommon = orig->getM3UModel()->unk4;
	M3UMtxCalcSIAnmBlendQuat* blend = new M3UMtxCalcSIAnmBlendQuat[2];
	blend[0].mMotionBlendRatio      = 0.0f;
	blend[1].mMotionBlendRatio      = 0.0f;
	J3DFrameCtrl* frames            = new J3DFrameCtrl[3];
	M3UModelCommonMario* common     = new M3UModelCommonMario;
	common->unk4                    = origCommon->unk4;
	common->unk8                    = files ? files->patterns : origCommon->unk8;
	common->unkC                    = files ? files->texNo : origCommon->unkC;
	common->unk18                   = blend;
	M3UModelMario* marioModel       = new M3UModelMario;
	marioModel->unk8                = model;
	marioModel->unk4                = common;
	marioModel->unk20               = common;
	marioModel->unkC                = frames;
	SomeModelMarioStruct* setInfo   = new SomeModelMarioStruct[2];
	SomeModelMarioStruct first      = {0, 2, 0, {0x14, 0x41}, 0};
	SomeModelMarioStruct upper      = {(s16)mJointChest, 2, 1, {0, 0}, 1};
	setInfo[0]                      = first;
	setInfo[1]                      = upper;
	marioModel->unk10               = 2;
	marioModel->unk24               = setInfo;
	M3UModel::Unk1CStruct* texSlot  = new M3UModel::Unk1CStruct;
	texSlot->unk0                   = 0;
	texSlot->unk1                   = 2;
	marioModel->unk1C               = texSlot;
	marioModel->changeMtxCalcSIAnmBQAnmTransform(0, 0, 0x3e);
	marioModel->changeMtxCalcSIAnmBQAnmTransform(1, 0, 0x41);
	frames[0].setRate(0.0f);
	frames[1].setRate(0.0f);
	frames[2].setRate(0.0f);
	setInfo[1].unk3 = 0xff;
	mModel          = marioModel;

	// as Mario's finalDrawInitialize: the hidden extra shape, his bare
	// hands, display lists, and no goop on him (the dirt colour's alpha)
	mBodyData->getShapeNodePointer(4)->onFlag(J3DShpFlag_Visible);
	showHands(0);
	clearDirt(mBodyData);
	SMS_MakeDLAndLock(model);
	for (u16 i = 0; i < mBodyData->getMaterialNum(); i++)
		if (i == eyes[0] || i == eyes[1])
			model->getMatPacketArray()[i].offFlag(0x1);
	for (u16 i = 0; i < mBodyData->getMaterialNum(); i++)
		SMS_InitPacket_OneTevKColorAndFog(model, i, GX_KCOLOR0, nullptr);
	hookJoint(mBodyData, "jnt_head", &turnHead);
	hookJoint(mBodyData, "chn_chest", &turnWaist);
	mBody = body;
	return true;
}

// The models the puppet draws now, kept for when it wears this body again.
void TMpPuppet::keepBody()
{
	Body& b = mBodies[mBody];
	b.model = mModel;
	b.data  = mBodyData;
	memcpy(b.hands, mHands, sizeof b.hands);
	memcpy(b.cap, mCap, sizeof b.cap);
	b.glasses   = mGlasses;
	b.joints[0] = mJointChest, b.joints[1] = mJointHandR, b.joints[2] = mJointHandL;
	b.joints[3] = mJointHead, b.joints[4] = mJointMHead;
}

// Draws the models kept for `body` (keepBody).
void TMpPuppet::takeBody(int body)
{
	const Body& b = mBodies[body];
	mModel        = b.model;
	mBodyData     = b.data;
	memcpy(mHands, b.hands, sizeof mHands);
	memcpy(mCap, b.cap, sizeof mCap);
	mGlasses    = b.glasses;
	mJointChest = b.joints[0], mJointHandR = b.joints[1], mJointHandL = b.joints[2];
	mJointHead = b.joints[3], mJointMHead = b.joints[4];
	mBody = body;
}

// Another body (their character changed): made once a stage, in the stage's
// memory (made while the stage plays too, so from its heap), and painted
// again. False, keeping the one it has, when it cannot be made.
bool TMpPuppet::wearBody(int body)
{
	if (body == mBody || body < 0 || body >= MPG_BODIES || !eclipseBodies() || (mNoBody & (1u << body)))
		return false;
	keepBody();
	const int was = mBody;
	if (mBodies[body].model) {
		takeBody(body);
	} else {
		// a body takes about 450 KB; this guards against a stage without room
		bool made = false;
		if (mHeap && mHeap->getTotalFreeSize() > 1024 * 1024) {
			JKRHeap* current = mHeap->becomeCurrentHeap();
			made             = buildBody(body);
			current->becomeCurrentHeap();
		}
		if (!made) {
			takeBody(was); // buildBody may have set some of it
			mNoBody |= 1u << body; // not tried again in this stage
			fprintf(stderr, "[multiplayer] could not make %s's other character in this stage\n", mpg_character_name(mSlot));
			return false;
		}
	}
	mDressedVersion = ~0u;
	return true;
}

bool TMpPuppet::buildFludd()
{
	// FLUDD as Mario wears it: his loaded animations, our own models (their
	// pose is ours). Each puppet loads its own copy of FLUDD's files, as of
	// Mario's, so it can wear its player's FLUDD colours (dress) without
	// painting anyone else's; its nozzles wear its pack's texture, as
	// TWaterGun::init gives Mario's his. Without room for the copies, it
	// draws the shared model data (and keeps FLUDD's usual colours).
	TWaterGun* gun = gpMarioOriginal ? gpMarioOriginal->mWaterGun : nullptr;
	if (!gun || !gun->mFluddModel)
		return false;
	const u32 flags = J3DMLF_MaterialPEFull | (4 << J3DMLF_TevStageNumShift);
	mFluddData      = loadCopy("/mario/watergun2/body/wg_mdl1.bmd", flags);
	ResTIMG* packTexture = mFluddData && mFluddData->getTexture() && mFluddData->getTexture()->getNum() > 1
	                         ? mFluddData->getTexture()->getResTIMG(1)
	                         : nullptr;
	mFludd = new MActor(gun->mFluddModel->mAnmData);
	mFludd->setModel(new J3DModel(packTexture ? mFluddData : gun->mFluddModel->getModel()->getModelData(), 0, 1), 0);
	mNozzleJoint = gun->unk1CD8;
	J3DModelData* marioFludd = gun->mFluddModel->getModel()->getModelData();
	ResTIMG* marioPack       = marioFludd->getTexture() && marioFludd->getTexture()->getNum() > 1
	                             ? marioFludd->getTexture()->getResTIMG(1)
	                             : nullptr;
	mNozzlesOwn              = true;
	for (int i = 0; i < 6; i++) {
		TNozzleBase* nozzle = gun->mNozzleList[i];
		MActor* actor       = nozzle ? nozzle->getMActor() : nullptr;
		if (!actor)
			continue;
		// its own copy (its joints turned from the pose, hookJoints), in the
		// pack's texture: this puppet's own, else Mario's
		J3DModelData* data = loadCopy(nozzleBmdData.getBmdPath(i), flags);
		ResTIMG* nozzleTexture = packTexture ? packTexture : marioPack;
		if (data && nozzleTexture)
			SMS_ChangeTextureAll(data, "H_watergun_main_dummy", *nozzleTexture);
		else if (data)
			data = nullptr;
		if (!data)
			mNozzlesOwn = false;
		mNozzles[i] = new MActor(actor->mAnmData);
		mNozzles[i]->setModel(new J3DModel(data ? data : actor->getModel()->getModelData(), 0, 1), 0);
	}
	if (!packTexture)
		mFluddData = nullptr;
	return true;
}

bool TMpPuppet::buildYoshi()
{
	// Yoshi's own model data (his colour is a material colour), Mario's
	// Yoshi's animations
	TYoshi* yoshi = gpMarioOriginal ? gpMarioOriginal->mYoshi : nullptr;
	void* res     = JKRFileLoader::getGlbResource("/yoshi/yoshi_model.bmd");
	if (!yoshi || !yoshi->unk30 || !res)
		return false;
	// one model data for every puppet's Yoshi in this stage (each sets his
	// colour just before he is entered)
	static J3DModelData* shared;
	static unsigned sharedFor;
	if (!shared || sharedFor != MpStageNumber()) {
		shared    = J3DModelLoaderDataBase::load(res, J3DMLF_MaterialPEFull | (4 << J3DMLF_TevStageNumShift));
		sharedFor = MpStageNumber();
	}
	mYoshi = new MActor(yoshi->unk30);
	mYoshi->setModel(new J3DModel(shared, 0, 1), 0);
	mYoshi->initNormalMotionBlend();
	mYoshi->offMakeDL();
	return true;
}

// The Blooper a player surfs on: the game draws Mario's from the stage's
// own three (TMapObjManager, one a colour, TMario::getGesso); each puppet
// has its own, coloured as the one its player rides.
bool TMpPuppet::buildSurf()
{
	if (!gpMapObjManager || !gpMapObjManager->mSurfGessoModelData)
		return false;
	mSurf = SMS_MakeMActorFromSDLModelData(gpMapObjManager->mSurfGessoModelData, gpMapObjManager->getMActorAnmData(), 3);
	if (!mSurf)
		return false;
	memcpy(mSurfColor, &gpMapObjManager->unkA8, sizeof mSurfColor);
	TMapObjBase::initPacketMatColor(mSurf->mModel, GX_TEVREG1, (const GXColorS10*)mSurfColor);
	mSurf->setBck("surfgeso_run1");
	if (J3DFrameCtrl* fc = mSurf->getFrameCtrl(ANM_TYPE_BCK))
		fc->setRate(0.5f);
	return true;
}

// The coaster car a player rides through Mecha-Bowser's fight: Mario's own
// (TMario::mTorocco, made in the stages that have one), drawn with him; each
// puppet has its own, under its player.
bool TMpPuppet::buildCoaster()
{
	MActor* car = gpMarioOriginal ? gpMarioOriginal->mTorocco : nullptr;
	if (!car)
		return false;
	mCoaster = new MActor(car->mAnmData);
	mCoaster->setModel(new J3DModel(car->getModel()->getModelData(), 0, 1), 0);
	return true;
}

bool TMpPuppet::init()
{
	JKRHeap* heap = JKRHeap::getCurrentHeap();
	mHeap         = heap;
	s32 free0     = heap->getTotalFreeSize();
	// ours as we are; theirs as they were last seen (it changes with them: wearBody)
	int body = mSelf ? MpLocalBody() : MPG_BODY_MARIO;
	MpgPose last;
	float age;
	if (!mSelf && eclipseBodies() && mpg_pose(mSlot, &last, &age) && last.body < MPG_BODIES)
		body = last.body;
	if (!buildBody(body) && (body == MPG_BODY_MARIO || !buildBody(MPG_BODY_MARIO)))
		return false;
	s32 free1 = heap->getTotalFreeSize();
	buildFludd();
	s32 free2 = heap->getTotalFreeSize();
	buildYoshi();
	buildSurf();
	buildCoaster();
	hookJoints();
	s32 free3 = heap->getTotalFreeSize();
	mOpa = new J3DDrawBuffer(0x20);
	mXlu = new J3DDrawBuffer(0x20);
	fprintf(stderr, "[multiplayer] %s: body %d, FLUDD %d, Yoshi %d bytes\n", mpg_character_name(mSlot),
	        (int)(free0 - free1), (int)(free1 - free2), (int)(free2 - free3));
	return true;
}

// Each texture of theirs this game has a picture for, in their colours: the
// body's (its cap and hands draw the body's texture data too) and FLUDD's
// (its nozzles draw its pack's). The rest are drawn as they are, never in our
// own Mario's look. Eclipse's other characters keep their own textures: theirs
// have Mario's names, but the pictures are Mario's.
void TMpPuppet::dress()
{
	mDressedVersion = mpg_looks_version();
	int dressed = 0, total = 0;
	J3DModelData* const models[2] = {mBodyData, mFluddData};
	for (J3DModelData* model : models) {
		J3DTexture* textures = model ? model->getTexture() : nullptr;
		JUTNameTab* names    = model ? model->getTextureName() : nullptr;
		if (!textures || !names)
			continue;
		const bool own = model == mBodyData && mBody != MPG_BODY_MARIO;
		for (u16 i = 0; i < textures->getNum(); i++, total++) {
			ResTIMG* timg    = textures->getResTIMG(i);
			const char* name = names->getName(i);
			if (!timg || !name)
				continue;
			// where GX reads it: the header's offset added in 32 bits, as on the
			// console (a texture sharing another's data can point back past it)
			const void* data = (const void*)(uintptr_t)(u32)((uintptr_t)timg + timg->imageDataOffset);
			const u32 bytes  = GXGetTexBufferSize(timg->width, timg->height, timg->format, GX_FALSE, 0);
			if (!own && mpg_dress(mSlot, mCharacter, name, data, bytes))
				dressed++;
			else
				mpg_dress_plain(data, bytes);
		}
	}
	static const bool debug = getenv("SMS_MULTIPLAYER_DEBUG") != nullptr;
	if (debug)
		fprintf(stderr, "[multiplayer] %s dressed: %d of %d textures\n", mpg_character_name(mSlot), dressed, total);
}

bool TMpPuppet::nameSpot(const f32* viewer, f32* x, f32* y, f32* alpha) const
{
	// near enough: the full name inside kNear, fading out to nothing at kFar
	const f32 kNear = 900.0f, kFar = 1300.0f;
	if (!mActive || !mDrawn || mProjection[0] != 0.0f) // perspective only
		return false;
	const f32 dx = mPosition.x - viewer[0], dy = mPosition.y - viewer[1], dz = mPosition.z - viewer[2];
	const f32 d = sqrtf(dx * dx + dy * dy + dz * dz);
	if (d >= kFar)
		return false;
	*alpha = d <= kNear ? 1.0f : (kFar - d) / (kFar - kNear);
	// above the cap
	const f32 at[3] = {mHead[0], mHead[1] + 85.0f, mHead[2]};
	f32 v[3];
	for (int r = 0; r < 3; r++)
		v[r] = mView[r][0] * at[0] + mView[r][1] * at[1] + mView[r][2] * at[2] + mView[r][3];
	if (v[2] > -1.0f) // behind the camera
		return false;
	const f32 w = -v[2];
	const f32 nx = (mProjection[1] * v[0] + mProjection[2] * v[2]) / w;
	const f32 ny = (mProjection[3] * v[1] + mProjection[4] * v[2]) / w;
	*x = (nx + 1.0f) * 0.5f;
	*y = (1.0f - ny) * 0.5f;
	return *x > -0.1f && *x < 1.1f && *y > -0.1f && *y < 1.1f;
}

MtxPtr TMpPuppet::jointMtx(const char* name, bool nozzle) const
{
	J3DModel* model = nozzle ? (mPose.nozzle < 6 && mNozzles[mPose.nozzle] ? mNozzles[mPose.nozzle]->getModel() : nullptr)
	                         : (mModel ? mModel->getModel() : nullptr);
	if (!model)
		return nullptr;
	const int joint = model->getModelData()->getJointName()->getIndex(name);
	return joint >= 0 && joint < model->getModelData()->getJointNum() ? model->getAnmMtx((u16)joint) : nullptr;
}

void TMpPuppet::showHands(int hand)
{
	J3DModelData* body = mBodyData;
	// J3DShpFlag_Visible set means hidden
	if (hand == 0) {
		body->getShapeNodePointer(5)->offFlag(J3DShpFlag_Visible);
		body->getShapeNodePointer(6)->offFlag(J3DShpFlag_Visible);
	} else {
		body->getShapeNodePointer(5)->onFlag(J3DShpFlag_Visible);
		body->getShapeNodePointer(6)->onFlag(J3DShpFlag_Visible);
	}
	for (int h = 0; h < 2; h++)
		for (int s = 0; s < 2; s++)
			if (mHands[h][s])
				setShapes(mHands[h][s]->getModelData(), hand != h + 1);
}

void TMpPuppet::applyPose(JDrama::TGraphics* graphics)
{
	M3UModelMario* pm      = mModel;
	SomeModelMarioStruct* s = pm->unk24;
	const MpgPose& p       = mPose;

	s_posing = this;
	u16 body = p.bodyBck < kMarioAnims ? p.bodyBck : 0x3e;
	if (s[0].unk4[0] != body)
		pm->changeMtxCalcSIAnmBQAnmTransform(0, 0, body);
	pm->getFrameCtrl(0).setRate(0.0f);
	pm->getFrameCtrl(0).setFrame(p.bodyFrame);
	pm->unk20->unk18[0].mMotionBlendRatio = p.bodyBlend;
	if ((p.flags & MPG_POSE_UPPER) && p.upperBck < kMarioAnims) {
		s[1].unk3 = 1;
		if (s[1].unk4[0] != p.upperBck)
			pm->changeMtxCalcSIAnmBQAnmTransform(1, 0, p.upperBck);
		pm->getFrameCtrl(1).setFrame(p.upperFrame);
	} else {
		s[1].unk3 = 0xff;
	}
	if (p.btp < kMarioTexPatterns && pm->unk4->unk8[p.btp]) {
		if (pm->unk1C->unk0 != p.btp)
			pm->changeAnmTexPattern(0, p.btp);
		pm->getFrameCtrl(2).setFrame(p.btpFrame);
	}

	// where: the root matrix as blended from their poses
	Mtx base;
	memcpy(base, p.mtx, sizeof base);
	mPosition.x = base[0][3];
	mPosition.y = base[1][3];
	mPosition.z = base[2][3];
	// a player who just arrived grows in, overshooting a little, rather than
	// popping up from nothing
	if (!mSelf && mSpawnFrames < 14) {
		const f32 t     = (f32)(mSpawnFrames + 1) / 14.0f;
		const f32 scale = t < 0.7f ? t / 0.7f * 1.12f : 1.12f - (t - 0.7f) / 0.3f * 0.12f;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				base[r][c] *= scale;
	}
	MTXCopy(base, pm->getModel()->getBaseTRMtx());
	pm->perform(CUE_CALC_ANIM, graphics);

	J3DModel* bodyModel = pm->getModel();
	MtxPtr head         = bodyModel->getAnmMtx(mJointHead);
	mHead[0]            = head[0][3];
	mHead[1]            = head[1][3];
	mHead[2]            = head[2][3];

	showHands(p.hand);
	// the body's shapes as the player's game shows them (the shirt, the
	// hands in the body, the hair under the cap)
	for (u16 i = 0; i < mBodyData->getShapeNum() && i < 32; i++) {
		if (p.hiddenShapes & (1u << i))
			mBodyData->getShapeNodePointer(i)->onFlag(J3DShpFlag_Visible);
		else
			mBodyData->getShapeNodePointer(i)->offFlag(J3DShpFlag_Visible);
	}
	for (int h = 0; h < 2; h++) {
		mHands[h][0]->setBaseTRMtx(bodyModel->getAnmMtx(mJointHandR));
		mHands[h][1]->setBaseTRMtx(bodyModel->getAnmMtx(mJointHandL));
		mHands[h][0]->calc();
		mHands[h][1]->calc();
	}
	J3DModel* cap = mCap[(p.flags & MPG_POSE_CAP) ? 0 : 1];
	cap->setBaseTRMtx(bodyModel->getAnmMtx(mJointMHead));
	cap->calc();
	if (mGlasses && (p.more & MPG_POSE2_SUNGLASSES)) {
		mGlasses->setBaseTRMtx(bodyModel->getAnmMtx(mJointMHead));
		mGlasses->calc();
	}

	if (mFludd && (p.flags & MPG_POSE_FLUDD)) {
		// on the back, tilted as TWaterGun::setBaseTRMtx tilts it
		Mtx chest;
		MTXCopy(bodyModel->getAnmMtx(mJointChest), chest);
		f32 lean     = chest[1][0] < 0.0f ? -chest[1][0] : chest[1][0];
		s16 angle    = (s16)(lean * (0 - -0x1800) + -0x1800);
		Mtx roll, gun;
		MsMtxSetRotRPH(roll, 0.0f, 0.0f, angle * (360.0f / 65536.0f));
		MTXConcat(chest, roll, gun);
		mFludd->getModel()->setBaseTRMtx(gun);
		if (mFludd->getCurAnmIdx(ANM_TYPE_BCK) != p.fluddBck)
			mFludd->setBckFromIndex(p.fluddBck);
		if (J3DFrameCtrl* fc = mFludd->getFrameCtrl(ANM_TYPE_BCK)) {
			fc->setRate(0.0f);
			fc->setFrame(p.fluddFrame);
		}
		mFludd->perform(CUE_CALC_ANIM, graphics);
		MActor* nozzle = p.nozzle < 6 ? mNozzles[p.nozzle] : nullptr;
		if (nozzle) {
			nozzle->getModel()->setBaseTRMtx(mFludd->getModel()->getAnmMtx(mNozzleJoint));
			nozzle->perform(CUE_CALC_ANIM, graphics);
		}
	}

	if (mYoshi && (p.flags & MPG_POSE_YOSHI)) {
		// Mario rides at Yoshi's root (TYoshi::calcAnim: getTakenMtx)
		mYoshi->getModel()->setBaseTRMtx(base);
		if (mYoshi->getCurAnmIdx(ANM_TYPE_BCK) != p.yoshiBck)
			mYoshi->setBckFromIndex(p.yoshiBck);
		if (J3DFrameCtrl* fc = mYoshi->getFrameCtrl(ANM_TYPE_BCK)) {
			fc->setRate(0.0f);
			fc->setFrame(p.yoshiFrame);
		}
		mYoshi->perform(CUE_CALC_ANIM, graphics);
	}

	if (mSurf && (p.more & MPG_POSE2_SURF)) {
		// under them, at their root (TMario::calcAnim), in its rider's colour
		const GXColorS10* colours[3] = {&gpMapObjManager->unkA8, &gpMapObjManager->unkB0, &gpMapObjManager->unkB8};
		const int type               = (p.more & MPG_POSE2_SURF_TYPE) >> 2;
		memcpy(mSurfColor, colours[type < 3 ? type : 0], sizeof mSurfColor);
		mSurf->getModel()->setBaseTRMtx(base);
		mSurf->perform(CUE_CALC_ANIM, graphics);
	}

	if (mCoaster && (p.more & MPG_POSE2_COASTER)) {
		// under them: the game sits Mario on its joint 2, turned by his own
		// angle (TMario::calcBaseMtx), so the car's base is his root without those
		J3DModel* car = mCoaster->getModel();
		Mtx at, seat, turn, unturn, b, root;
		MTXIdentity(at);
		car->setBaseTRMtx(at);
		mCoaster->calcAnm();
		MTXInverse(car->getAnmMtx(2), seat);
		MsMtxSetRotRPH(turn, 0.0f, p.coasterAngle * (360.0f / 65536.0f), 0.0f);
		MTXInverse(turn, unturn);
		// the unscaled pose (base may be scaled): MTXConcat takes no const Mtx
		memcpy(root, p.mtx, sizeof root);
		MTXConcat(root, unturn, b);
		MTXConcat(b, seat, at);
		car->setBaseTRMtx(at);
		mCoaster->calcAnm();
	}

	// their own boat (each player has theirs): under them, as their game has it
	const f32 root[3] = {base[0][3], base[1][3], base[2][3]};
	mRide             = MpMirrorRide(mSlot, root);
	if (mRide)
		mRide->perform(CUE_CALC_ANIM, graphics);
	s_posing = nullptr;
}

// Their effects, as their game asked for them (mp_game.cpp's particleEmit),
// here at them: those going on every frame, and each new one asked for once.
void TMpPuppet::showEffects(const MpgPose& p)
{
	if (!gpMarioParticleManager)
		return;
	for (int i = 0; i < p.fxCount && i < MPG_MAX_FX; i++)
		showEffect(p.fx[i], i);
	if (!mFxSeen) {
		mFxSeen = true;
		mFxSeq  = p.shotSeq;
		return;
	}
	const int fresh = (int)(s8)(u8)(p.shotSeq - mFxSeq);
	if (fresh <= 0)
		return;
	const int n = fresh < p.shotCount ? fresh : p.shotCount;
	for (int i = p.shotCount - n; i < p.shotCount; i++)
		showEffect(p.shots[i], -1);
	mFxSeq = p.shotSeq;
}

// How loud the other players are: a little under the player's own Mario, who
// stays in front; and only near ours, full inside kSoundNear and fading out to
// nothing at kSoundFar (the sound system's own fall-off stops at a floor: Mario's
// sounds are made to be heard at the camera, wherever he is).
const f32 kOthersVolume = 0.7f;
const f32 kSoundNear = 500.0f, kSoundFar = 2500.0f;

// Their sounds, as their game started them (mp_game.cpp's soundHook), here
// at them: each new one once, its place following them while it plays (the
// sound system reads it each frame, so it is kept for the slot, outliving the
// stage), heard only near ours (kSoundFar). Their voice is one line at a
// time, as Mario's is (the sound system cuts the last short, unless it
// matters more).
void TMpPuppet::playSounds(const MpgPose& p)
{
	static Vec s_at[MPG_MAX_PLAYERS];
	static JAISound* s_voice[MPG_MAX_PLAYERS];
	Vec& at = s_at[mSlot];
	at.x    = mPosition.x;
	at.y    = mPosition.y;
	at.z    = mPosition.z;
	if (!mSoundSeen) {
		mSoundSeen = true;
		mSoundSeq  = p.soundSeq;
		return;
	}
	const int fresh = (int)(s8)(u8)(p.soundSeq - mSoundSeq);
	if (fresh <= 0)
		return;
	mSoundSeq = p.soundSeq;
	if (!MSGBasic || (p.more & MPG_POSE2_PAUSED))
		return;
	f32 volume = kOthersVolume, d = 0.0f;
	if (const TMario* m = gpMarioOriginal) {
		const f32 dx = at.x - m->mPosition.x, dy = at.y - m->mPosition.y, dz = at.z - m->mPosition.z;
		d = sqrtf(dx * dx + dy * dy + dz * dz);
		if (d >= kSoundFar)
			return;
		if (d > kSoundNear)
			volume *= (kSoundFar - d) / (kSoundFar - kSoundNear);
	}
	const int n = fresh < p.soundCount ? fresh : p.soundCount;
	for (int i = p.soundCount - n; i < p.soundCount; i++) {
		const u32 id = p.sounds[i];
		if (!(id & 0x800))
			continue; // only those started once (mp_game.cpp's soundHook)
		JAIActor actor(&at, &at, &at, 0);
		JAISound* sound;
		if ((id >> 12 & 0xF) == 7) {
			MSGBasic->startSoundActor(id, &s_voice[mSlot], &actor, 0, 4);
			sound = s_voice[mSlot];
		} else {
			sound = MSGBasic->startSoundActorReturnHandle(id, &actor, 0, 4);
		}
		// (a volume step the game leaves alone: the sound's seven multiply)
		if (sound)
			sound->setVolume(volume, 0, 6);
		MpCountSound(mSlot);
		static const bool trace = getenv("SMS_MULTIPLAYER_TRACE") != nullptr;
		if (trace)
			fprintf(stderr, "[mptrace] %d sound %04x %.0f away, volume %.2f, retrace %u\n", mSlot, (unsigned)id, d, volume,
			        (unsigned)VIGetRetraceCount());
	}
}

void TMpPuppet::showEffect(const MpgFx& f, int ongoing)
{
	// the particle manager's own bounds for its every-frame effects (an
	// effect's slot in TMarioParticleManager::unk10, unk368)
	if (f.group > 3 || f.how > 5 || (f.group == 1 && (f.id < 253 || f.id >= 253 + 214))
	    || (f.group == 3 && (f.id < 486 || f.id >= 486 + 19)))
		return;
	const int k              = ongoing >= 0 ? ongoing : (mFxShotNext++ & 15);
	JGeometry::TVec3<f32>& v = ongoing >= 0 ? mFxPos[k] : mFxShotPos[k];
	MtxPtr m                 = ongoing >= 0 ? mFxMtx[k] : mFxShotMtx[k];
	v.set(mPosition.x + f.off[0], mPosition.y + f.off[1], mPosition.z + f.off[2]);
	TMarioParticleManager* pm = gpMarioParticleManager;
	if (f.how <= 2) {
		MpCountEffect(mSlot, ongoing >= 0);
		if (f.how == 0)
			pm->emit(f.id, &v, f.group, this);
		else if (f.how == 1)
			pm->emitWithRotate(f.id, &v, (s16)(f.turn[0] << 8), (s16)(f.turn[1] << 8), (s16)(f.turn[2] << 8), f.group, this);
		else
			pm->emitAndBindToPosPtr(f.id, f.bind == MPG_FX_BIND_ROOT ? &mPosition : &v, f.group, this);
		return;
	}
	MtxPtr at = nullptr;
	if (f.bind == MPG_FX_BIND_BODY && mModel && f.joint < mModel->getModel()->getModelData()->getJointNum()) {
		at = mModel->getModel()->getAnmMtx(f.joint);
	} else if (f.bind == MPG_FX_BIND_FLUDD && mFludd && (mPose.flags & MPG_POSE_FLUDD)
	           && f.joint < mFludd->getModel()->getModelData()->getJointNum()) {
		at = mFludd->getModel()->getAnmMtx(f.joint);
	} else if (f.bind == MPG_FX_BIND_NONE) {
		f32 q[4]      = {f.turn[0] / 127.0f, f.turn[1] / 127.0f, f.turn[2] / 127.0f, f.turn[3] / 127.0f};
		const f32 len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
		for (int i = 0; i < 4; i++)
			q[i] = len > 1e-3f ? q[i] / len : (i == 3 ? 1.0f : 0.0f);
		fromQuat(q, m);
		m[0][3] = v.x, m[1][3] = v.y, m[2][3] = v.z;
		at = m;
	}
	if (!at)
		return;
	MpCountEffect(mSlot, ongoing >= 0);
	if (f.how == 3)
		pm->emitAndBindToMtxPtr(f.id, at, f.group, this);
	else if (f.how == 4)
		pm->emitAndBindToSRTMtxPtr(f.id, at, f.group, this);
	else
		pm->emitAndBindToMtx(f.id, at, f.group, this);
}

// Debug: how smoothly another player is drawn while they move. Each frame's
// step is held against the steps just before: a stall (barely moving) or a
// jump (far more than lately) is a hitch the eye catches.
void MpMeasureSmoothness(int slot, const MpgPose& pose)
{
	static const bool debug = getenv("SMS_MULTIPLAYER_DEBUG") != nullptr;
	if (!debug || slot < 0 || slot >= MPG_MAX_PLAYERS)
		return;
	struct Track {
		f32 x, z, avg;
		bool have;
		int moving, stalls, jumps, frames;
	};
	static Track tracks[MPG_MAX_PLAYERS];
	Track& k     = tracks[slot];
	const f32 x  = pose.mtx[0][3], z = pose.mtx[2][3];
	if (k.have) {
		const f32 step = sqrtf((x - k.x) * (x - k.x) + (z - k.z) * (z - k.z));
		if (k.avg > 3.0f && step < 400.0f) { // moving (not standing, not a warp)
			k.moving++;
			if (step < k.avg * 0.25f)
				k.stalls++;
			else if (step > k.avg * 2.5f)
				k.jumps++;
		}
		k.avg += (step - k.avg) * 0.2f;
	}
	k.x = x, k.z = z, k.have = true;
	if (++k.frames % 600 == 0 && k.moving) {
		fprintf(stderr, "[multiplayer] drawn smoothness, %s: %d moving frames, %d stalls, %d jumps\n",
		        mpg_character_name(slot), k.moving, k.stalls, k.jumps);
		k.moving = k.stalls = k.jumps = 0;
	}
}

BOOL TMpPuppet::receiveMessage(THitActor* sender, u32 message)
{
	(void)sender;
	return message == HIT_MESSAGE_TRAMPLE && !mSelf && mActive && mpg_collision();
}

void TMpPuppet::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (!mModel)
		return;
	// once a frame, for every player (after Mario, who is first in the group)
	if (cue & CUE_ENTRY)
		MpPvpEntry(graphics);
	if (cue & CUE_UNK20000000)
		MpPvpDraw(graphics);

	if (cue & CUE_CALC_ANIM) {
		float age = 0.0f;
		MpgPose pose;
		if (mSelf) {
			// Mario as he is this frame (his calcAnim ran in the movement pass)
			MpCapturePose(gpMarioOriginal, &pose);
			mHaveSmoothed = false;
			mActive       = gpMarioOriginal && (pose.flags & MPG_POSE_VISIBLE);
		} else {
			// between the two poses either side of a moment just past
			MpgPose a, b;
			f32 t = 0.0f;
			mActive = mpg_pose_pair(mSlot, &a, &b, &t) && (b.flags & MPG_POSE_VISIBLE);
			if (mActive)
				blendPoses(a, b, t, &pose);
			if (mActive)
				MpMeasureSmoothness(mSlot, pose);
			static const bool trace = getenv("SMS_MULTIPLAYER_TRACE") != nullptr;
			MpgPose newest;
			if (trace && mActive && mpg_pose(mSlot, &newest, &age))
				fprintf(stderr, "[mptrace] %d drawn %.1f %.1f newest %.1f %.1f frame %.2f t %.2f\n", mSlot,
				        pose.mtx[0][3], pose.mtx[2][3], newest.mtx[0][3], newest.mtx[2][3], pose.bodyFrame, t);
		}
		if (!mActive && mGoneFrames < 1000)
			mGoneFrames++;
		if (!mActive)
			mDrawn = false;
		// in their character's model (Eclipse's character select), then in
		// their colours, once they are here (not for empty slots)
		if (mActive && !mSelf && pose.body != mBody)
			wearBody(pose.body);
		if (mActive && mDressedVersion != mpg_looks_version())
			dress();
		if (mActive) {
			// gone a second or more, or in a new stage: they appear again
			if (mGoneFrames > 30 || pose.session != mLastSession)
				mSpawnFrames = 0;
			else if (mSpawnFrames < 1000)
				mSpawnFrames++;
			mGoneFrames = 0;
			if (pose.session != mLastSession || mSpawnFrames == 0) {
				mHaveSmoothed = false;
				mFxSeen       = false; // none from before shown now
				mSoundSeen    = false; // nor heard
			}
			mLastSession = pose.session;
			mPose        = pose;
			applyPose(graphics);
			// their water (our own player's is the game's own)
			if (!mSelf && (pose.flags & MPG_POSE_SPRAYING))
				MpEmitWater(mSlot, pose, &mWaterCarry);
			// their effects (our own player's are the game's own)
			if (!mSelf)
				showEffects(pose);
			// their sounds (our own player's are the game's own)
			if (!mSelf)
				playSounds(pose);
		}
	}
	if (!mActive && !mSwapped)
		return;

	if (cue & CUE_CALC_VIEW) {
		MTXCopy(graphics->mViewMtx, j3dSys.mViewMtx);
		MTXCopy(graphics->mViewMtx, mView);
		mModel->getModel()->viewCalc();
		for (int h = 0; h < 2; h++)
			for (int s = 0; s < 2; s++)
				mHands[h][s]->viewCalc();
		mCap[(mPose.flags & MPG_POSE_CAP) ? 0 : 1]->viewCalc();
		if (mGlasses && (mPose.more & MPG_POSE2_SUNGLASSES))
			mGlasses->viewCalc();
		if (mFludd && (mPose.flags & MPG_POSE_FLUDD)) {
			mFludd->perform(CUE_CALC_VIEW, graphics);
			if (mPose.nozzle < 6 && mNozzles[mPose.nozzle])
				mNozzles[mPose.nozzle]->perform(CUE_CALC_VIEW, graphics);
		}
		if (mYoshi && (mPose.flags & MPG_POSE_YOSHI))
			mYoshi->perform(CUE_CALC_VIEW, graphics);
		if (mSurf && (mPose.more & MPG_POSE2_SURF))
			mSurf->perform(CUE_CALC_VIEW, graphics);
		if (mCoaster && (mPose.more & MPG_POSE2_COASTER))
			mCoaster->perform(CUE_CALC_VIEW, graphics);
		if (mRide)
			mRide->perform(CUE_CALC_VIEW, graphics);
	}
	if (cue & CUE_UNK10000000) {
		mOpa->frameInit();
		mXlu->frameInit();
		mSaved[0]             = j3dSys.mDrawBuffer[0];
		mSaved[1]             = j3dSys.mDrawBuffer[1];
		j3dSys.mDrawBuffer[0] = mOpa;
		j3dSys.mDrawBuffer[1] = mXlu;
		mSwapped              = true;
	}
	if ((cue & CUE_ENTRY) && mActive) {
		// splashed: Shadow Mario's flush (mp_pvp.cpp), and off again after
		const bool flushed = MpPvpFlushed(mSlot);
		if (flushed || mFlushed) {
			const f32 at[3] = {mPosition.x, mPosition.y, mPosition.z};
			MpPvpFog(mBodyData, flushed, at, graphics);
			for (int h = 0; h < 2; h++)
				for (int s = 0; s < 2; s++)
					MpPvpFog(mHands[h][s]->getModelData(), flushed, at, graphics);
			for (int c = 0; c < 2; c++)
				MpPvpFog(mCap[c]->getModelData(), flushed, at, graphics);
			if (mGlasses)
				MpPvpFog(mGlasses->getModelData(), flushed, at, graphics);
			mFlushed = flushed;
		}
		mModel->perform(CUE_ENTRY, graphics);
		for (int h = 0; h < 2; h++)
			for (int s = 0; s < 2; s++)
				mHands[h][s]->entry();
		mCap[(mPose.flags & MPG_POSE_CAP) ? 0 : 1]->entry();
		if (mGlasses && (mPose.more & MPG_POSE2_SUNGLASSES))
			mGlasses->entry();
		if (mFludd && (mPose.flags & MPG_POSE_FLUDD)) {
			mFludd->perform(CUE_ENTRY, graphics);
			if (mPose.nozzle < 6 && mNozzles[mPose.nozzle])
				mNozzles[mPose.nozzle]->perform(CUE_ENTRY, graphics);
		}
		if (mYoshi && (mPose.flags & MPG_POSE_YOSHI)) {
			// his colour, as TYoshi::entry sets it
			J3DModelData* data = mYoshi->getModel()->getModelData();
			for (u16 i = 0; i < data->getMaterialNum(); i++) {
				J3DGXColorS10 c;
				c.color.r = mPose.yoshiColor[0];
				c.color.g = mPose.yoshiColor[1];
				c.color.b = mPose.yoshiColor[2];
				c.color.a = 255;
				data->getMaterialNodePointer(i)->getTevBlock()->setTevColor(2, &c);
			}
			mYoshi->perform(CUE_ENTRY, graphics);
		}
		if (mSurf && (mPose.more & MPG_POSE2_SURF)) {
			const JGeometry::TVec3<f32> at(mPosition.x, mPosition.y, mPosition.z);
			mSurf->setLightData(nullptr, at);
			mSurf->perform(CUE_ENTRY, graphics);
		}
		if (mRide) {
			const JGeometry::TVec3<f32> at(mPosition.x, mPosition.y, mPosition.z);
			mRide->setLightData(nullptr, at);
			mRide->perform(CUE_ENTRY, graphics);
		}
		if (mCoaster && (mPose.more & MPG_POSE2_COASTER))
			mCoaster->perform(CUE_ENTRY, graphics);
	}
	if ((cue & CUE_UNK8000000) && mSwapped) {
		j3dSys.mDrawBuffer[0] = mSaved[0];
		j3dSys.mDrawBuffer[1] = mSaved[1];
		mSwapped              = false;
	}
	// drawn under Mario's light, after him (the group draws him first, and
	// his light is set again before the next pass)
	if ((cue & CUE_UNK40000000) && mActive) {
		// the scene's projection, for the name over the head
		GXGetProjectionv(mProjection);
		mDrawn = true;
		if (gpLightManager && gpLightManager->mMarioLight)
			gpLightManager->mMarioLight->setLight(graphics, gpMarioOriginal ? gpMarioOriginal->mLightID : 0);
		j3dSys.setUnk4C(3);
		mOpa->draw();
	}
	if ((cue & CUE_UNK20000000) && mActive) {
		if (gpLightManager && gpLightManager->mMarioLight)
			gpLightManager->mMarioLight->setLight(graphics, gpMarioOriginal ? gpMarioOriginal->mLightID : 0);
		j3dSys.setUnk4C(4);
		mXlu->draw();
	}
}
