// Split-screen co-op (SMS_COOP=1, ext/coop/README.md): a second player in the
// same game, on the same screen. Player 2 is a second TMario, made from the
// stage's own Mario record while the stage loads, green as Luigi, reading
// controller 2 (platform/pad: the controller that joined with Start). A
// second CPolarSubCamera follows them. While player 2 runs (their move, their
// camera's, and the drawing of their view), the game's player globals
// (gpMarioOriginal, gpMarioPos and the rest, gpCamera and its helpers) are
// theirs, so the game's own Mario code plays them as it plays Mario. Each
// view is drawn into its half of the frame (GXPC_SetSplitView), the HUD once
// over both, with each player's health and FLUDD in their own view (player 2's
// from a second HUD of their own). Sounds are heard from the nearer player's
// camera. Objects near player 2 move as if player 2 were Mario (objectMove),
// so enemies go after whoever is nearest.
// Game code: C++03.
#include "port_ext.h"
#include "port_framerate.h"
#include "../../multiplayer/mp_recolour.h"

#include <Camera/Camera.hpp>
#include <Camera/CameraMarioData.hpp>
#include <Camera/CameraOption.hpp>
#include <Camera/CameraShake.hpp>
#include <Camera/CubeManagerBase.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JDrama/JDRViewObjPtrList.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JSupport/JSUMemoryInputStream.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <Map/MapData.hpp>
#include <MoveBG/ItemManager.hpp>
#include <NPC/NpcBase.hpp>
#include <MoveBG/Item.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/MarioCap.hpp>
#include <Player/WaterGun.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/Strategy.hpp>
#include <System/Application.hpp>
#include <GC2D/BoundPane.hpp>
#include <GC2D/ExPane.hpp>
#include <GC2D/GCConsole2.hpp>
#include <JSystem/J2D/J2DPicture.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <System/PerformList.hpp>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
// platform/pad (pad.cpp): the second player's controller
int port_coop_enabled(void);
int port_coop_player2(void);
void port_coop_set_joinable(int on);
void port_coop_leave(void);
unsigned port_coop_take_events(void);
int port_coop_can_join(void);
void port_coop_join_virtual(void);
extern int port_coop_in_p2;
// platform/pad: the normal camera's mouse (and C-stick orbit) state, which
// zzz-camera-02 applies to whichever camera is gpCamera
extern f32 port_camera_mouse_pitch, port_camera_zoom;
extern int port_camera_mouse_busy;
// platform/gx
void GXPC_SetSplitView(int on, float x, float y, float w, float h) __attribute__((weak));
void GXPC_ExtOverlaySet(int panel, const char* text) __attribute__((weak));
// ext/multiplayer/game/mp_spawn.cpp: the stage's scene.bin records
int port_spawn_record_of(const void* object, const void** record, unsigned* size);
// decomp-patches/zzzzzz-coop-01: ViewFrustumClipCheck's other view
Mtx* port_coop_other_view = nullptr;
// ...and TPollutionManager::stampModel's: the second view's entry pass is on
int port_coop_second_entry = 0;
// zzzzzz-coop-03: both players' cameras while player 2 plays, for clipping
Mtx* port_coop_views[2] = { nullptr, nullptr };
// zzzzzz-coop-08: camera 2's place while player 2 plays, the sound's second ear
const Vec* port_coop_ear_pos = nullptr;
}

namespace {

const int kNoticeFrames = 30 * 5;
const int kOverlayNotice = 0; // GXPC_EXT_OVERLAY_NOTICE

// ------------------------------------------------------------ the players' globals

// The globals the game reads "the player" and "the camera" through.
struct Ctx {
	TMario* mario;
	CPolarSubCamera* cam;
	TCameraShake* shake;
	TCameraMarioData* camMario;
	TCameraOption* camOption;
	// the cube the player is in, of the area cubes and fast cubes A, B and C
	// (TMario::thinkCube sets them for whoever is gpMarioOriginal)
	int cubes[4];
	// the camera's PC look (platform/pad): its tilt (the mouse's or the
	// C-stick orbit's), its wheel zoom, and whether it follows mouse look
	// closely. One set of globals served both cameras, so player 1's mouse
	// tilted, zoomed and tightened player 2's camera.
	f32 camPitch, camZoom;
	int camMouseBusy;
};

Ctx s_p1, s_p2;
bool s_inP2;          // player 2's globals are in force
bool s_made;          // this stage has player 2 and their camera
bool s_active;        // player 2 is playing
bool s_split;         // the stage is drawn in two views this frame
bool s_wanted;        // SMS_COOP: co-op is on
TGCConsole2* s_console2; // player 2's HUD, for their health and FLUDD
Mtx* s_earMtx;        // camera 2's view while player 2 plays: the sound's second ear
int s_layout;         // 0 side by side, 1 one above the other
f32 s_baseAspect;     // the game camera's own aspect
TMarDirector* s_dir;  // the director whose stage has them
int s_respawnFrames;  // player 2 is down: the frame they are back on
// Lives are shared, as in multiplayer: player 2's lost life is one of the
// game's (TFlagManager 0x20001, the HUD's count). Lost on the last one,
// player 2 is out until the next life comes (a 1-Up), which brings them back.
const u32 kLivesFlag = 0x20001;

// A life taken from the shared count. The HUD redraws its lives only when
// they go up (losing one reloads the stage), so its digits are set here, as
// TGCConsole2 sets them, and its lives panel shown as for a 1-Up.
void showLives()
{
	TGCConsole2* c = gpMarDirector ? gpMarDirector->getConsole() : nullptr;
	if (!c)
		return;
	int lives = TFlagManager::getInstance()->getFlag(kLivesFlag);
	lives     = lives < 0 ? 0 : lives > 99 ? 99 : lives;
	c->unk3AC = (u8)lives;
	((J2DPicture*)c->unk39C[0]->getPane())->changeTexture(c->unkE0[lives / 10]->getTexInfo(), 0);
	((J2DPicture*)c->unk39C[1]->getPane())->changeTexture(c->unkE0[lives % 10]->getTexInfo(), 0);
	c->startAppearMario(false);
}

void takeLife()
{
	TFlagManager::getInstance()->decFlag(kLivesFlag, 1);
	showLives();
}
bool s_out;           // player 2 is out of lives
bool s_lastLife;      // ...and their death is playing: out once it has
s32 s_livesWhenOut;   // the count when they went out: a life more brings them back
int s_noticeFrames;
int s_hintFrames;     // "press Start to join" shown at a stage's start
bool s_hintShown;
bool s_connectHintShown; // "connect a second controller", shown once a game
bool s_testJoined;
int s_testJoinFrame;
int s_frame;
int s_swapCount;      // object moves led by player 2 lately (SMS_COOP_DEBUG)

void capture(Ctx& c)
{
	c.mario     = gpMarioOriginal;
	c.cam       = gpCamera;
	c.shake     = gpCameraShake;
	c.camMario  = gpCameraMario;
	c.camOption = gpCameraOption;
	TCubeManagerMarioIn* m[4] = { gpCubeArea, gpCubeFastA, gpCubeFastB, gpCubeFastC };
	for (int i = 0; i < 4; i++)
		c.cubes[i] = m[i] ? m[i]->unk1C : -1;
	c.camPitch     = port_camera_mouse_pitch;
	c.camZoom      = port_camera_zoom;
	c.camMouseBusy = port_camera_mouse_busy;
}

void apply(const Ctx& c)
{
	gpMarioOriginal = c.mario;
	gpMarioAddress  = c.mario;
	if (c.mario)
		SMS_SetMarioAccessParams();
	gpCamera       = c.cam;
	gpCameraShake  = c.shake;
	gpCameraMario  = c.camMario;
	gpCameraOption = c.camOption;
	TCubeManagerMarioIn* m[4] = { gpCubeArea, gpCubeFastA, gpCubeFastB, gpCubeFastC };
	for (int i = 0; i < 4; i++)
		if (m[i])
			m[i]->unk1C = c.cubes[i];
	port_camera_mouse_pitch = c.camPitch;
	port_camera_zoom        = c.camZoom;
	port_camera_mouse_busy  = c.camMouseBusy;
}

// Player 2's globals for as long as it lives (nested: the outermost restores).
bool enterP2(Ctx& saved)
{
	if (s_inP2 || !s_p2.mario || !s_p2.cam)
		return false;
	capture(saved);
	memcpy(s_p1.cubes, saved.cubes, sizeof s_p1.cubes); // player 1's, the other cubes while player 2 runs
	apply(s_p2);
	s_inP2          = true;
	port_coop_in_p2 = 1;
	return true;
}
void leaveP2(const Ctx& saved)
{
	capture(s_p2); // what player 2's code changed (a camera's helpers) stays theirs
	s_p2.mario = (TMario*)gpMarioOriginal;
	// the mouse is player 1's (port_camera_take_mouse gives camera 2 nothing):
	// camera 2 keeps the game's distance and its own pace
	s_p2.camZoom      = 1.0f;
	s_p2.camMouseBusy = 0;
	apply(saved);
	s_inP2          = false;
	port_coop_in_p2 = 0;
}
struct InP2 {
	Ctx saved;
	bool entered;
	InP2()
	    : entered(enterP2(saved))
	{
	}
	~InP2()
	{
		if (entered)
			leaveP2(saved);
	}
};

// ------------------------------------------------------------ Luigi's colours

// The bytes a texture's image takes, its mipmaps included.
u32 imageBytes(const ResTIMG& t)
{
	const u8 levels = t.mipmapEnabled && t.mipmapCount > 1 ? t.mipmapCount : 1;
	return GXGetTexBufferSize(t.width, t.height, t.format, levels > 1 ? GX_TRUE : GX_FALSE, levels);
}

// A texture table of a model's own: its headers and then every image and
// palette, copied into one block so each header's offsets stay positive.
// Texture 0 is `first` when given. A header's offsets are 32-bit and may point
// back before it (Eclipse sets some of Mario's textures to images loaded
// earlier): the game's memory is under 4 GB, so the game's own 32-bit image
// addresses wrap to the right place; read here, they are signed.
J3DTexture* ownTextures(J3DTexture* old, const ResTIMG* first)
{
	if (!old || !old->getNum())
		return old;
	const u16 n = old->getNum();
	u32 total   = sizeof(ResTIMG) * n;
	for (u16 i = 0; i < n; i++) {
		const ResTIMG& t = i == 0 && first ? *first : *old->getResTIMG(i);
		total = (total + 31) & ~31u;
		total += imageBytes(t);
		if (t.isIndexTexture && t.numColors)
			total = ((total + 31) & ~31u) + t.numColors * 2;
	}
	u8* block = (u8*)JKRHeap::alloc(total, 32, nullptr);
	if (!block)
		return old;
	ResTIMG* heads = (ResTIMG*)block;
	u32 at         = sizeof(ResTIMG) * n;
	for (u16 i = 0; i < n; i++) {
		const ResTIMG& t = i == 0 && first ? *first : *old->getResTIMG(i);
		heads[i]         = t;
		at               = (at + 31) & ~31u;
		const u32 bytes  = imageBytes(t);
		if (getenv("SMS_COOP_DEBUG"))
			fprintf(stderr, "[coop] texture %d/%d at %p: format %d %dx%d mips %d/%d image +0x%x (%u bytes) palette %d +0x%x (%d)\n",
			        (int)i, (int)n, (const void*)&t, (int)t.format, (int)t.width, (int)t.height, (int)t.mipmapEnabled,
			        (int)t.mipmapCount, (unsigned)t.imageDataOffset, (unsigned)bytes, (int)t.isIndexTexture,
			        (unsigned)t.paletteOffset, (int)t.numColors);
		memcpy(block + at, (const u8*)&t + (s32)t.imageDataOffset, bytes);
		heads[i].imageDataOffset = (u32)(at - sizeof(ResTIMG) * i);
		at += bytes;
		if (t.isIndexTexture && t.numColors) {
			at = (at + 31) & ~31u;
			memcpy(block + at, (const u8*)&t + (s32)t.paletteOffset, t.numColors * 2);
			heads[i].paletteOffset = (u32)(at - sizeof(ResTIMG) * i);
			at += t.numColors * 2;
		}
	}
	DCFlushRange(block, total);
	return new J3DTexture(n, heads);
}

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

// Mario's red made Luigi's green in a CMPR texture (each 4x4 block's two
// colours, as multiplayer's puppets are), every mipmap.
void luigiColours(ResTIMG* t)
{
	if (!t || t->format != GX_TF_CMPR)
		return;
	u8* data         = (u8*)t + t->imageDataOffset;
	const u32 blocks = imageBytes(*t) / 8;
	for (u32 i = 0; i < blocks; i++) {
		u8* blk     = data + i * 8;
		u16 c0      = (u16)(blk[0] << 8 | blk[1]);
		u16 c1      = (u16)(blk[2] << 8 | blk[3]);
		const bool four = c0 > c1;
		int r, g, b;
		unpack565(c0, r, g, b);
		mp_recolour::recolour(MPG_CHAR_LUIGI, r, g, b);
		u16 n0 = pack565(r, g, b);
		unpack565(c1, r, g, b);
		mp_recolour::recolour(MPG_CHAR_LUIGI, r, g, b);
		u16 n1 = pack565(r, g, b);
		if (four ? n0 <= n1 : n0 > n1) {
			if (n0 == n1) {
				// equal colours would turn a four-colour block into three and transparent
				if (four && n1 > 0)
					n1--;
				else if (four)
					n0++;
			} else {
				u16 tmp = n0;
				n0      = n1;
				n1      = tmp;
				u32 idx = (u32)blk[4] << 24 | (u32)blk[5] << 16 | (u32)blk[6] << 8 | blk[7];
				if (four) {
					idx ^= 0x55555555; // 0<->1, 2<->3
				} else {
					// three colours: 0<->1, 2 (the mix) and 3 (transparent) stay
					u32 out = 0;
					for (int k = 0; k < 16; k++) {
						u32 v = (idx >> (k * 2)) & 3;
						if (v < 2)
							v ^= 1;
						out |= v << (k * 2);
					}
					idx = out;
				}
				blk[4] = (u8)(idx >> 24), blk[5] = (u8)(idx >> 16), blk[6] = (u8)(idx >> 8), blk[7] = (u8)idx;
			}
		}
		blk[0] = (u8)(n0 >> 8), blk[1] = (u8)n0, blk[2] = (u8)(n1 >> 8), blk[3] = (u8)n1;
	}
	DCFlushRange(data, imageBytes(*t));
}

// Player 2 in Luigi's colours: their body, hands and cap get textures of
// their own (Mario's models share theirs with Mario), the body's main
// texture recoloured. Before their loadAfter, which makes the body's display
// lists with the textures it then has.
void dressAsLuigi(TMario* m)
{
	J3DModelData* body = m->mBodyModelData;
	if (!body)
		return;
	J3DTexture* own = ownTextures(body->getTexture(), nullptr);
	if (own == body->getTexture())
		return;
	body->setTexture(own);
	luigiColours(own->getResTIMG(0));
	const ResTIMG* main = own->getResTIMG(0);
	for (int h = 0; h < 2; h++)
		for (int s = 0; s < 2; s++)
			if (m->mHandModels[h][s]) {
				J3DModelData* d = m->mHandModels[h][s]->getModelData();
				d->setTexture(ownTextures(d->getTexture(), main));
			}
	if (m->mRHand4ndModel) {
		J3DModelData* d = m->mRHand4ndModel->getModelData();
		d->setTexture(ownTextures(d->getTexture(), main));
	}
	if (m->mCap)
		for (int c = 0; c < 2; c++)
			if (J3DModel* cap = m->mCap->unk10[c]) {
				J3DModelData* d = cap->getModelData();
				d->setTexture(ownTextures(d->getTexture(), main));
				cap->unlock();
				SMS_MakeDLAndLock(cap); // its display lists again, with the new texture
			}
}

// ------------------------------------------------------------ player 2 and camera 2

TMario* p1() { return s_p1.mario; }
TMario* p2() { return s_made ? s_p2.mario : nullptr; }

class TCoopMario : public TMario {
public:
	virtual void load(JSUMemoryInputStream& stream)
	{
		InP2 in;
		TMario::load(stream);
	}
	virtual void loadAfter()
	{
		InP2 in;
		TMario::loadAfter();
		// the sound's player is Mario, not this one (TMario::loadAfter set it while this was Mario)
		if (s_p1.mario)
			SMSGetMSound()->setPlayerInfo(&s_p1.mario->mPosition, &s_p1.mario->mPrevPosition,
			                              s_p1.mario->getM3UModel()->getModel()->getAnmMtx(1), true);
	}
	virtual void perform(u32 cue, JDrama::TGraphics* graphics)
	{
		if (!s_active || !s_made)
			return;
		const bool hellDead = gpMarDirector && (gpMarDirector->unk4E & TMarDirector::DEMO_FLAG_HELL_DEAD);
		{
			InP2 in;
			TMario::perform(cue, graphics);
			// their camera follows them, after they moved (as Mario's does after him)
			if (cue & (CUE_MOVE | CUE_CALC_ANIM))
				gpCamera->testPerform(cue & (CUE_MOVE | CUE_CALC_ANIM | CUE_MOVEMENT_GATE_A | CUE_MOVEMENT_GATE_B),
				                      graphics);
		}
		// a fall into a pit is theirs: the stage carries on
		if (gpMarDirector && !hellDead)
			gpMarDirector->unk4E &= ~TMarDirector::DEMO_FLAG_HELL_DEAD;
	}
};

class TCoopCamera : public CPolarSubCamera {
public:
	TCoopCamera(const char* name)
	    : CPolarSubCamera(name)
	{
	}
	virtual void loadAfter()
	{
		InP2 in;
		CPolarSubCamera::loadAfter();
	}
	virtual void perform(u32 cue, JDrama::TGraphics* graphics)
	{
		InP2 in;
		CPolarSubCamera::perform(cue, graphics);
	}
};

// The scene.bin record `object` was made from, past its type name
// (TNameRef::getType): what its load reads.
bool recordBody(const void* object, const u8** body, u32* size)
{
	const void* record = nullptr;
	unsigned bytes     = 0;
	if (!port_spawn_record_of(object, &record, &bytes) || bytes < 8)
		return false;
	const u8* r = (const u8*)record;
	u32 at      = 4 + 2;
	at += 2 + ((u32)r[at] << 8 | r[at + 1]);
	if (at >= bytes)
		return false;
	*body = r + at;
	*size = bytes - at;
	return true;
}

// The stage's scene is in: player 2 and their camera are made from Mario's
// record and with the stage's parameters, and put where the scene's loadAfter
// finds them (the player group, the cameras).
void sceneLoaded()
{
	s_made     = false;
	s_split    = false;
	s_active   = false; // a new stage's player 2 joins it once it plays (frame)
	s_console2 = nullptr;
	if (!s_wanted || !gpMarioOriginal || !gpCamera || !gpStrategy || gpApplication.mCurrArea.getStage() == 15)
		return;
	TIdxGroupObj* players = gpStrategy->unk10[6]; // "プレーヤーグループ"
	JDrama::TViewObjPtrListT<JDrama::TViewObj>* cameras
	    = (JDrama::TViewObjPtrListT<JDrama::TViewObj>*)JDrama::TNameRefGen::search2("Cameras");
	const u8* record = nullptr;
	u32 size         = 0;
	if (!players || !cameras || !recordBody(gpMarioOriginal, &record, &size)) {
		fprintf(stderr, "[coop] this stage has no Mario of its own to copy\n");
		return;
	}
	capture(s_p1);
	s_baseAspect = gpCamera->mAspect;

	// camera 2: its constructor makes the camera's helpers anew (gpCameraShake...)
	TCoopCamera* cam = new TCoopCamera("camera 2");
	capture(s_p2);
	s_p2.cam          = cam;
	s_p2.camPitch     = 0.0f; // level, at the game's distance
	s_p2.camZoom      = 1.0f;
	s_p2.camMouseBusy = 0;
	apply(s_p1);
	cam->mAspect = s_baseAspect;
	cam->setGamePad(gpMarDirector->getGamePad(1));
	cameras->getChildren().push_back(cam);

	// player 2: Mario's record
	TCoopMario* m = new TCoopMario;
	s_p2.mario    = m;
	JSUMemoryInputStream body((void*)record, size);
	m->load(body); // as player 2 (InP2): its parameters, models and FLUDD
	m->setGamePad(gpMarDirector->getGamePad(1));
	dressAsLuigi(m);
	m->onHitFlag(HIT_FLAG_NO_COLLISION); // until they join
	THitActor* actor = m;
	players->add(actor);
	s_made = true;
	s_dir  = gpMarDirector;
	fprintf(stderr, "[coop] player 2 and camera 2 ready (%d bytes free in the stage's heap)\n",
	        (int)JKRHeap::getCurrentHeap()->getTotalFreeSize());
}

// ------------------------------------------------------------ drawing two views

// Links of the stage's perform lists that perform camera 1 (drawn with
// camera 2 for player 2's view), and the GX Post list's parts: the stage's
// 3D before the 2D (TMarDirector::initECDisp), its own 3D overlays (lens
// flare, talk cursor), and the 2D drawn once over both views.
const int kMostCamLinks = 64;
TPerformLink* s_camLinks[kMostCamLinks];
int s_camLinkCount;
const int kMostPost = 512;
TPerformLink* s_post[kMostPost];
int s_postCount, s_post3DEnd, s_postOverlayFrom, s_postOverlayTo, s_composite;

void findCamLinks(TPerformList* list, JDrama::TViewObj* cam)
{
	if (!list)
		return;
	for (JGadget::TSingleLinkList<TPerformLink, 0>::iterator it = list->getChildren().begin();
	     it != list->getChildren().end(); ++it)
		if (it->mPerformer == cam && s_camLinkCount < kMostCamLinks)
			s_camLinks[s_camLinkCount++] = &*it;
}

void swapCamLinks(JDrama::TViewObj* to)
{
	for (int i = 0; i < s_camLinkCount; i++)
		s_camLinks[i]->mPerformer = to;
}

// Player 2's HUD: a second TGCConsole2, made from the stage's own (its
// record, as the stage loaded it) once that one is set up, its loadAfter with
// player 2's globals, so it shows their health and FLUDD as the game's shows
// Mario's. It runs and is drawn with the game's (port_coop_hud); only its
// health and FLUDD meters are ever drawn.
void makeConsole2()
{
	s_console2       = nullptr;
	TGCConsole2* own = gpMarDirector->getConsole();
	const u8* record = nullptr;
	u32 size         = 0;
	if (!own || !recordBody(own, &record, &size)) {
		fprintf(stderr, "[coop] this stage has no HUD of its own to copy: player 2's health and FLUDD go unshown\n");
		return;
	}
	TGCConsole2* c = new TGCConsole2("console 2");
	JSUMemoryInputStream body((void*)record, size);
	c->load(body);
	{
		InP2 in;
		c->loadAfter();
	}
	// its counters are the game's HUD's: their sparkles are never drawn
	JPABaseEmitter* sparkles[3] = { c->unk124, c->unk144, c->unk164 };
	for (int i = 0; i < 3; i++)
		if (sparkles[i])
			sparkles[i]->stopDrawParticle();
	s_console2 = c;
}

void stageSetup()
{
	s_camLinkCount = s_postCount = 0;
	s_post3DEnd = s_postOverlayFrom = s_postOverlayTo = s_composite = -1;
	s_respawnFrames = 0;
	s_hintShown     = false;
	if (s_lastLife) { // the stage changed while their last life was being lost
		s_lastLife     = false;
		s_out          = true;
		s_livesWhenOut = TFlagManager::getInstance()->getFlag(kLivesFlag);
	}
	if (!s_made || !gpMarDirector)
		return;
	// camera 2's list of the actors the close-up view Y opens can turn to, as
	// the director makes camera 1's (TMarDirector::setupObjects, once the
	// conductor has its managers): without it, Y read an unmade list
	s_p2.cam->setNoticeInfo();
	TMarDirector* d = gpMarDirector;
	findCamLinks(d->unk34, s_p1.cam);
	findCamLinks(d->mPerformListGX, s_p1.cam);
	findCamLinks(d->mPerformListSilhouette, s_p1.cam);
	findCamLinks(d->mPerformListGXPost, s_p1.cam);
	JDrama::TViewObj* group2D2  = (JDrama::TViewObj*)JDrama::TNameRefGen::search2("Group 2D 2");
	for (JGadget::TSingleLinkList<TPerformLink, 0>::iterator it = d->mPerformListGXPost->getChildren().begin();
	     it != d->mPerformListGXPost->getChildren().end() && s_postCount < kMostPost; ++it) {
		TPerformLink* l = &*it;
		const int i     = s_postCount;
		s_post[s_postCount++] = l;
		const char* name = l->mPerformer ? l->mPerformer->getName() : nullptr;
		if (s_post3DEnd < 0 && name && !strcmp(name, "stageDisp"))
			s_post3DEnd = i;
		if (s_postOverlayTo < 0 && group2D2 && l->mPerformer == group2D2)
			s_postOverlayTo = i - 1; // the projection before Group 2D 2
	}
	// the composite (合成3) follows the 2D part's viewport and projection
	// (TMarDirector::initECDisp); found by place, as a literal in Japanese
	// would not match the scene's names where the compiler cannot write them
	// in Shift-JIS (the CP932 mirror covers the game's sources, not ext/)
	if (s_post3DEnd >= 0 && s_post3DEnd + 3 < s_postCount && s_post[s_post3DEnd + 1]->mPerformer
	    && s_post[s_post3DEnd + 1]->mPerformer->getName() && !strcmp(s_post[s_post3DEnd + 1]->mPerformer->getName(), "Screen 2D"))
		s_composite = s_post3DEnd + 3;
	s_postOverlayFrom = s_composite + 1;
	if (s_post3DEnd < 0 || s_composite < s_post3DEnd || s_postOverlayTo < s_postOverlayFrom) {
		fprintf(stderr, "[coop] this stage's GX Post list is not as expected: one view only\n");
		s_postCount = 0;
	}
	if (getenv("SMS_COOP_DEBUG"))
		fprintf(stderr, "[coop] %d camera links; GX Post: %d links, 3D to %d, composite %d, overlays %d-%d\n",
		        s_camLinkCount, s_postCount, s_post3DEnd, s_composite, s_postOverlayFrom, s_postOverlayTo);
	makeConsole2();
}

void performPost(int from, int to, JDrama::TGraphics* g)
{
	for (int i = from; i < to && i < s_postCount; i++)
		if (i != s_composite)
			s_post[i]->perform(CUE_ALL, g);
}

// The two views' parts of the frame: player 1 left or top.
void viewRect(int view, float* r)
{
	if (s_layout == 1) {
		r[0] = 0.0f, r[1] = view ? 0.5f : 0.0f, r[2] = 1.0f, r[3] = 0.5f;
	} else {
		r[0] = view ? 0.5f : 0.0f, r[1] = 0.0f, r[2] = 0.5f, r[3] = 1.0f;
	}
}

// One view: its camera's models' view matrices and draw buffers (the entry
// pass the game makes after each tick, made again for a view after the
// first), then the stage's 3D, into its part.
void drawView(TMarDirector* d, JDrama::TGraphics* g, int view, bool entry)
{
	float r[4];
	viewRect(view, r);
	port_coop_other_view = (Mtx*)(view ? s_p1.cam : s_p2.cam)->getUnk1EC();
	GXPC_SetSplitView(1, r[0], r[1], r[2], r[3]);
	g->unk0 = 0;
	if (entry) {
		port_coop_second_entry = 1;
		d->unk34->perform(CUE_ALL, g);
		port_coop_second_entry = 0;
	}
	d->unk40->perform(CUE_ALL, g);
	d->mPerformListGX->perform(CUE_ALL, g);
	if (gpSilhouetteManager->unk48 > 0.0f || gpCamera->getUnk2C8() != -1)
		d->mPerformListSilhouette->perform(CUE_ALL, g);
	performPost(0, s_post3DEnd, g);
	// the 2D part's 3D: its viewport and projection, then the lens flare and talk cursor
	performPost(s_post3DEnd + 1, s_composite, g);
	performPost(s_postOverlayFrom, s_postOverlayTo, g);
}

// A dark line between the two views, under the HUD.
void drawDivider()
{
	Mtx44 ortho;
	C_MTXOrtho(ortho, 0.0f, 448.0f, 0.0f, 640.0f, 0.0f, 1.0f);
	GXSetProjection(ortho, GX_ORTHOGRAPHIC);
	Mtx id;
	PSMTXIdentity(id);
	GXLoadPosMtxImm(id, GX_PNMTX0);
	GXSetCurrentMtx(GX_PNMTX0);
	GXSetViewport(0.0f, 0.0f, 640.0f, 448.0f, 0.0f, 1.0f);
	GXSetScissor(0, 0, 640, 448);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GXSetNumChans(1);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	const GXColor dark = { 16, 16, 20, 255 };
	GXSetChanMatColor(GX_COLOR0A0, dark);
	GXSetNumTexGens(0);
	GXSetNumIndStages(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GXSetCullMode(GX_CULL_NONE);
	GXSetColorUpdate(GX_TRUE);
	f32 x0, y0, x1, y1;
	if (s_layout == 1)
		x0 = 0.0f, x1 = 640.0f, y0 = 223.0f, y1 = 225.0f;
	else
		x0 = 319.0f, x1 = 321.0f, y0 = 0.0f, y1 = 448.0f;
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	GXPosition2f32(x0, y0);
	GXPosition2f32(x1, y0);
	GXPosition2f32(x1, y1);
	GXPosition2f32(x0, y1);
	GXEnd();
}

int drawStage(void* director, void* graphics)
{
	if (!s_split || !GXPC_SetSplitView || !s_postCount || director != s_dir)
		return 0;
	TMarDirector* d     = (TMarDirector*)director;
	JDrama::TGraphics* g = (JDrama::TGraphics*)graphics;
	// once: the frame's setup and the goop's textures (drawn at the frame's corner, then copied)
	d->unk40->perform(CUE_ALL, g);
	d->unk38->perform(CUE_ALL, g);
	d->unk3C->perform(CUE_ALL, g);
	// player 1's view first, from the entry pass the game made after the
	// tick; then player 2's, entered anew (the buffers drawn are cleared as
	// they are drawn, the rest as the entry pass begins)
	drawView(d, g, 0, false);
	{
		InP2 in;
		swapCamLinks(s_p2.cam);
		drawView(d, g, 1, true);
		swapCamLinks(s_p1.cam);
	}
	GXPC_SetSplitView(0, 0, 0, 1, 1);
	port_coop_other_view = nullptr;
	drawDivider();
	// the 2D, once over both: the frame's copy set up, the screen's effects, the HUD, the copy
	for (int i = s_post3DEnd; i <= s_composite; i++)
		s_post[i]->perform(CUE_ALL, g);
	for (int i = s_postOverlayTo; i < s_postCount; i++)
		s_post[i]->perform(CUE_ALL, g);

	return 1;
}

// ------------------------------------------------------------ each player's HUD

bool s_hudBusy;          // port_coop_hud is running the HUDs (their performs come back to it)
bool s_console2Updating; // player 2's HUD's update: its sounds (soundHook)

// One draw of a HUD: `parts` of it (port_hud_set_drawn), moved and scaled into
// x, y, w, h of the frame (GXPC_SetSplitView; 0, 0, 1, 1 where it is).
void hudPass(TGCConsole2* c, JDrama::TGraphics* g, unsigned parts, float x, float y, float w, float h)
{
	const bool moved = x != 0.0f || y != 0.0f || w != 1.0f || h != 1.0f;
	port_hud_set_drawn(parts);
	if (moved && GXPC_SetSplitView)
		GXPC_SetSplitView(1, x, y, w, h);
	c->perform(8, g);
	if (moved && GXPC_SetSplitView)
		GXPC_SetSplitView(0, 0, 0, 1, 1);
	port_hud_set_drawn(PORT_HUD_EVERYTHING);
}

// The HUD while the screen is split: what the players share (the counters,
// the lives, the messages) where the game has it, and each player's health
// and FLUDD in their own view. Side by side, a view is as tall as the frame
// and half as wide: player 1's meters move left by half the frame, to their
// view's right edge, and player 2's stay at the frame's; both health meters
// come down below the top row, where player 1's would cover the lives (at the
// frame's top middle). One above the other, each player's HUD is drawn at
// half size into the right half of their view, so the health meter sits at
// its top right and FLUDD at its bottom right.
const float kHealthDown = 80.0f / 448.0f; // side by side: under the lives' row

void drawHuds(TGCConsole2* own, JDrama::TGraphics* g)
{
	const unsigned meters = PORT_HUD_HEALTH | PORT_HUD_WATER;
	hudPass(own, g, PORT_HUD_EVERYTHING & ~meters, 0.0f, 0.0f, 1.0f, 1.0f);
	if (s_layout == 1) {
		hudPass(own, g, meters, 0.5f, 0.0f, 0.5f, 0.5f);
	} else {
		hudPass(own, g, PORT_HUD_HEALTH, -0.5f, kHealthDown, 1.0f, 1.0f);
		hudPass(own, g, PORT_HUD_WATER, -0.5f, 0.0f, 1.0f, 1.0f);
	}
	InP2 in;
	if (s_layout == 1) {
		hudPass(s_console2, g, meters, 0.5f, 0.5f, 0.5f, 0.5f);
	} else {
		hudPass(s_console2, g, PORT_HUD_HEALTH, 0.0f, kHealthDown, 1.0f, 1.0f);
		hudPass(s_console2, g, PORT_HUD_WATER, 0.0f, 0.0f, 1.0f, 1.0f);
	}
}

// Sounds player 2's HUD makes as it runs: those of their health and FLUDD
// (hurt, healed, low, the tank full); the rest are the game's HUD's already
// (the counters, the messages, the HUD sliding in and out).
int soundHook(unsigned* id, const float* pos, int system)
{
	(void)pos;
	(void)system;
	if (!s_console2Updating)
		return 0;
	switch (*id) {
	case MSD_SE_SY_HP_EMPTY:
	case MSD_SE_SY_HP_RECOVER:
	case MSD_SE_SY_HP_DOWN:
	case MSD_SE_SY_DAMAGE:
	case MSD_SE_SY_PO_WATER_FULL:
		return 0;
	default:
		return 1;
	}
}

// ------------------------------------------------------------ joining, leaving, play

TMarDirector* director()
{
	return gpApplication.mAppState == TApplication::APP_STATE_GAMEPLAY ? gpMarDirector : nullptr;
}

bool playing()
{
	TMarDirector* d = director();
	return s_made && d && d == s_dir && d->mState == TMarDirector::STATE_UNK4 && gpMarioOriginal == s_p1.mario
	       && !d->checkFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
}

void notice(const char* text, int frames)
{
	if (GXPC_ExtOverlaySet)
		GXPC_ExtOverlaySet(kOverlayNotice, text);
	s_noticeFrames = frames;
	fprintf(stderr, "[coop] %s\n", text);
}

// Beside player 1, facing as they do, a little behind and to their right.
int s_movedP2At = -100; // the frame co-op itself last put player 2 somewhere
void placeBeside(TMario* m, TMario* a)
{
	const f32 s = sinf(a->mFaceAngle.y * (3.14159265f / 32768.0f));
	const f32 c = cosf(a->mFaceAngle.y * (3.14159265f / 32768.0f));
	m->mPosition.x = a->mPosition.x + c * 90.0f - s * 40.0f;
	m->mPosition.y = a->mPosition.y + 20.0f;
	m->mPosition.z = a->mPosition.z - s * 90.0f - c * 40.0f;
	m->mFaceAngle.y = a->mFaceAngle.y;
	m->mModelFaceAngle = a->mFaceAngle.y;
	m->mVel.x = m->mVel.y = m->mVel.z = 0.0f;
	m->mForwardVel = 0.0f;
}
void placeBesideP1(TMario* m)
{
	placeBeside(m, p1());
	s_movedP2At = s_frame;
}

// Player 2's FLUDD gauge as a stage starts it (TGCConsole2::loadAfter: down,
// to come up as they have FLUDD), after it went down with them. While their
// death plays, their HUD takes their camera's death close-up for a cutscene
// and hides the gauge every frame, as the game's does for Mario's (whose death
// then reloads the stage), while it goes on bringing it up: it would be left
// up with nothing shown but its back.
void resetTank(TGCConsole2* c)
{
	if (!c || c->unk2F8->getPane()->isVisible())
		return;
	c->unk45 = c->unk46 = 0;
	c->unk7C = 0;
	c->unk26C->getPane()->show();
	c->unk270->getPane()->hide();
	c->unk274->getPane()->hide();
	c->unk29C->getPane()->hide();
	c->unk288->hide();
}

void revive(TMario* m)
{
	resetTank(s_console2);
	m->offFlag(MARIO_FLAG_GAME_OVER);
	m->mHealth = m->mDeParams.mHPMax.get();
	placeBesideP1(m);
	{
		InP2 in;
		// as a new life starts: the death's status ends (changePlayerStatus refuses to leave it)
		m->mPrevStatus  = MARIO_STATUS_WAIT;
		m->mStatus      = MARIO_STATUS_WAIT;
		m->mStatusArg   = 0;
		m->mStatusState = 0;
		m->mStatusTimer = 0;
		// their camera out of its death close-up, as player 1's is after a life
		CPolarSubCamera* cam = gpCamera;
		cam->unk64 &= ~(CPolarSubCamera::CAMERA_FLAG_DEAD_DEMO | CPolarSubCamera::CAMERA_FLAG_HELL_DEAD_DEMO);
		cam->mDeadDemoCountdown          = 0;
		cam->mDeadDemoCountdownToFovZoom = 0;
		cam->mPosFreezeFrames            = 0;
		cam->mFovy                       = s_p1.cam->mFovy;
		cam->warpPosAndAt(cam->mCurrentTarget.unk28, *gpMarioAngleY + 0x8000);
	}
}

f32 dist2(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b);

// Player 2's FLUDD as the stage set up Mario's (TMarDirector::setMario: none
// in a secret course, the saved nozzle, the Rocket where it is the stage's).
TMarDirector* s_fluddSyncedFor;
void syncFludd(TMario* m)
{
	TMario* a = p1();
	if (a->checkFlag(MARIO_FLAG_HAS_FLUDD))
		m->onFlag(MARIO_FLAG_HAS_FLUDD);
	else
		m->offFlag(MARIO_FLAG_HAS_FLUDD);
	TWaterGun* g1 = a->getFludd();
	TWaterGun* g2 = m->getFludd();
	if (!g1 || !g2 || g1->mCurrentNozzle == TWaterGun::Yoshi)
		return;
	InP2 in;
	if (g2->mSecondNozzle != g1->mSecondNozzle && g1->mSecondNozzle != TWaterGun::Spray)
		g2->changeNozzle((TWaterGun::TNozzleType)g1->mSecondNozzle, true);
	if (g2->mCurrentNozzle != g1->mCurrentNozzle)
		g2->changeNozzle((TWaterGun::TNozzleType)g1->mCurrentNozzle, true);
}

// Player 2 beside Mario and their camera behind them, as after Mario was
// warped (a script's warp, a pipe within the stage, a rising floor).
void followP1(TMario* m)
{
	placeBesideP1(m);
	InP2 in;
	gpCamera->warpPosAndAt(gpCamera->mCurrentTarget.unk28, *gpMarioAngleY + 0x8000);
}

void activate(bool on)
{
	TMario* m = p2();
	if (!m || s_active == on)
		return;
	s_active = on;
	if (getenv("SMS_COOP_DEBUG"))
		fprintf(stderr, "[coop] frame %d (gx frame %u): player 2 %s\n", s_frame, (unsigned)GXPC_FrameCount(),
		        on ? "in play" : "out of play");
	if (on) {
		m->offHitFlag(HIT_FLAG_NO_COLLISION);
		revive(m);
		s_fluddSyncedFor = nullptr; // as the stage set up Mario's
	} else {
		m->onHitFlag(HIT_FLAG_NO_COLLISION);
		m->mPosition.y = -100000.0f; // out of everyone's way
	}
}

// A view's camera as wide as its part of the frame.
void setAspects(bool split)
{
	f32 aspect = s_baseAspect;
	if (split)
		aspect *= s_layout == 1 ? 2.0f : 0.5f;
	if (s_p1.cam)
		s_p1.cam->mAspect = aspect;
	if (s_p2.cam)
		s_p2.cam->mAspect = aspect;
}

void frame()
{
	s_frame++;
	if (!s_wanted)
		return;
	if (gpApplication.mCurrArea.getStage() == 15)
		s_out = s_lastLife = false; // the title: a file's lives start again
	TMarDirector* d = director();
	const bool here = s_made && d && d == s_dir;
	// both cameras clip the stage's actors while both play (zzzzzz-coop-03)
	port_coop_views[0] = here && s_active && s_p1.cam ? (Mtx*)s_p1.cam->getUnk1EC() : nullptr;
	port_coop_views[1] = here && s_active && s_p2.cam ? (Mtx*)s_p2.cam->getUnk1EC() : nullptr;
	// ...and sounds are heard from the nearer camera (zzzzzz-coop-08)
	s_earMtx          = port_coop_views[1];
	port_coop_ear_pos = s_earMtx ? &s_p2.cam->getUnk124Vec() : nullptr;
	if (!here) {
		port_coop_set_joinable(0);
		s_split = false;
		return;
	}
	port_coop_set_joinable(playing());
	// controller 2 reads as controller 1 does in each of the stage's modes
	// (the director sets controller 1's: play, talk, menus)
	// (flag 4, B talks, is each controller's own: the director sets it for the
	// player with someone to talk to, port_coop_find_talk_npc)
	if (TMarioGamePad* pad2 = d->getGamePad(1))
		pad2->mFlags = (d->getGamePad(0)->mFlags & ~4u) | (pad2->mFlags & 4u);
	// a test's player 2 (SMS_COOP_TEST_JOIN=frames into the stage)
	if (const char* t = getenv("SMS_COOP_TEST_JOIN"))
		if (!s_testJoined && playing() && ++s_hintFrames > atoi(t)) {
			s_testJoined    = true;
			s_testJoinFrame = s_frame;
			port_coop_join_virtual();
			if (const char* l = getenv("SMS_COOP_TEST_LIVES")) // the shared lives as the test joins
				TFlagManager::getInstance()->setFlag(kLivesFlag, atoi(l)), showLives();
		}
	if (const char* t = getenv("SMS_COOP_TEST_ONE_UP")) // a 1-Up, frames after the test's join
		if (s_testJoined && s_frame == s_testJoinFrame + atoi(t))
			TFlagManager::getInstance()->incMario(1);
	if (const char* t = getenv("SMS_COOP_TEST_LEAVE")) // a test's player 2 leaving, frames after joining
		if (s_testJoined && s_frame == s_testJoinFrame + atoi(t))
			port_coop_leave();
	const unsigned events = port_coop_take_events();
	if ((events & 1) && s_out)
		notice("Luigi is out of lives. The next 1-Up brings him back.", kNoticeFrames);
	else if (events & 1)
		notice("Luigi joined! Hold START on their controller to leave.", kNoticeFrames);
	if (events & 2)
		notice("Luigi left. Press START on another controller to play again.", kNoticeFrames);
	// out of lives: back with the next life that comes (it is theirs)
	if (s_out && !s_active && playing()) {
		TFlagManager* flags = TFlagManager::getInstance();
		const s32 lives     = flags->getFlag(kLivesFlag);
		if (lives > s_livesWhenOut) {
			takeLife();
			s_out = false;
			notice("A 1-Up for Luigi: he's back!", kNoticeFrames / 2);
		} else if (lives < s_livesWhenOut) {
			s_livesWhenOut = lives; // Mario lost one meanwhile
		}
	}
	const bool want = port_coop_player2() != 0 && !s_out;
	if (want != s_active && (!want || playing()))
		activate(want);
	if (s_active && playing() && s_fluddSyncedFor != s_dir && p2()) {
		s_fluddSyncedFor = s_dir;
		syncFludd(p2());
	}
	if (!s_active && !s_hintShown && playing() && port_coop_can_join()) {
		s_hintShown = true;
		notice("Co-op: press START on another controller to join as Luigi.", kNoticeFrames);
	} else if (!s_active && !s_hintShown && !s_connectHintShown && playing() && port_coop_player2() == 0) {
		// no controller but the first player's (once a game: they may mean to play alone for now)
		s_connectHintShown = true;
		notice("Co-op: connect a second controller, then press its START to join as Luigi.", kNoticeFrames);
	}
	if (s_noticeFrames > 0 && --s_noticeFrames == 0 && GXPC_ExtOverlaySet)
		GXPC_ExtOverlaySet(kOverlayNotice, "");
	// two views while both play the stage; one (player 1's camera) for its
	// cutscenes, Shine Gets and the like, which the game shows on camera 1
	const bool split = s_active && d->mState >= TMarDirector::STATE_UNK4 && !d->isDemoModeNow()
	                   && !d->checkDemoFlag(TMarDirector::DEMO_FLAG_SHINE_GET_STOP_THE_WORLD)
	                   && !s_p1.cam->isDemoCamera() && s_postCount > 0;
	if (split != s_split)
		setAspects(split);
	s_split = split;
}

void afterMovement()
{
	TMario* m = p2();
	static JGeometry::TVec3<f32> p1Last;
	static TMarDirector* p1LastFor;
	if (!s_active || !m || director() != s_dir) {
		p1LastFor = nullptr;
		return;
	}
	// Mario warped (moved further than any run, jump or rocket takes him in a
	// frame): player 2 goes with him, as everyone does through a pipe
	const JGeometry::TVec3<f32> p1Now = p1()->mPosition;
	if (p1LastFor == s_dir && playing() && !m->checkFlag(MARIO_FLAG_GAME_OVER)
	    && dist2(p1Now, p1Last) > 1500.0f * 1500.0f && dist2(m->mPosition, p1Now) > 1500.0f * 1500.0f) {
		if (getenv("SMS_COOP_DEBUG"))
			fprintf(stderr, "[coop] Mario warped %.0f units: player 2 goes with him\n", sqrtf(dist2(p1Now, p1Last)));
		followP1(m);
	}
	// ...and Mario goes with player 2 when they are warped (a warp floor of theirs)
	static JGeometry::TVec3<f32> p2Last;
	const JGeometry::TVec3<f32> p2Now = m->mPosition;
	if (p1LastFor == s_dir && playing() && !m->checkFlag(MARIO_FLAG_GAME_OVER) && s_frame - s_movedP2At > 2
	    && dist2(p2Now, p2Last) > 1500.0f * 1500.0f && dist2(p2Now, p1()->mPosition) > 1500.0f * 1500.0f) {
		if (getenv("SMS_COOP_DEBUG"))
			fprintf(stderr, "[coop] player 2 warped %.0f units: Mario goes with them\n", sqrtf(dist2(p2Now, p2Last)));
		placeBeside(p1(), m);
		gpCamera->warpPosAndAt(gpCamera->mCurrentTarget.unk28, *gpMarioAngleY + 0x8000);
	}
	p2Last    = m->mPosition;
	p1Last    = p1()->mPosition;
	p1LastFor = s_dir;
	// test aids (frames since the game started): player 2 loses a life; the
	// stage changes (SMS_COOP_TEST_WARP=frame:area,episode)
	static bool killed, warped;
	if (const char* k = getenv("SMS_COOP_TEST_KILL"))
		if (!killed && s_frame >= atoi(k)) {
			killed = true;
			InP2 in;
			m->loserExec();
		}
	if (const char* w = getenv("SMS_COOP_TEST_WARP")) {
		int at = 0, area = 0, episode = 0;
		if (!warped && sscanf(w, "%d:%d,%d", &at, &area, &episode) == 3 && s_frame >= at) {
			warped = true;
			s_dir->setNextStage((u16)(((area + 1) << 8) | episode), nullptr);
		}
	}
	// player 2 put at x,y,z facing angle (SMS_COOP_TEST_AT), 10 frames after joining
	if (const char* t = getenv("SMS_COOP_TEST_AT")) {
		static bool placed;
		float x, y, z;
		int ang;
		if (!placed && s_testJoined && s_frame >= s_testJoinFrame + 10 && sscanf(t, "%f,%f,%f,%d", &x, &y, &z, &ang) == 4) {
			placed = true;
			s_movedP2At = s_frame;
			m->mPosition.x = x, m->mPosition.y = y, m->mPosition.z = z;
			m->mFaceAngle.y = (s16)ang;
			m->mModelFaceAngle = (s16)ang;
			InP2 in;
			gpCamera->warpPosAndAt(gpCamera->mCurrentTarget.unk28, *gpMarioAngleY + 0x8000);
		}
	}
	// FLUDD put on player 2's back mid-stage, frames after joining (as a stage
	// that starts without it hands it out)
	static bool gaveFludd;
	if (const char* t = getenv("SMS_COOP_TEST_FLUDD"))
		if (!gaveFludd && s_testJoined && s_frame >= s_testJoinFrame + atoi(t)) {
			gaveFludd = true;
			m->onFlag(MARIO_FLAG_HAS_FLUDD);
			fprintf(stderr, "[coop] test: player 2 has FLUDD now\n");
		}
	// both players hurt, frames after joining: Mario by 2, player 2 by 3, and
	// player 2's FLUDD a third full (their meters)
	static bool hurt;
	if (const char* t = getenv("SMS_COOP_TEST_HURT"))
		if (!hurt && s_testJoined && s_frame >= s_testJoinFrame + atoi(t)) {
			hurt = true;
			p1()->decHP(2);
			InP2 in;
			m->decHP(3);
			if (TWaterGun* gun = m->getFludd())
				gun->mCurrentWater = gun->getMaxWater() / 3;
			fprintf(stderr, "[coop] test: Mario's health %d, player 2's %d\n", (int)p1()->mHealth, (int)m->mHealth);
		}
	// coins brought out at player 2 and taken by them, frames after joining
	static int coinsDone;
	if (const char* t = getenv("SMS_COOP_TEST_COINS")) {
		int at = 0, count = 0;
		if (sscanf(t, "%d:%d", &at, &count) == 2 && s_testJoined && coinsDone < count && gpItemManager
		    && s_frame >= s_testJoinFrame + at + coinsDone * 10 && playing()) {
			if (TCoin* coin = gpItemManager->newAndRegisterCoinReal()) {
				coin->mPosition = m->mPosition;
				coin->appear(); // the item group's hit check takes it, as it does Mario's
			}
			coinsDone++;
			fprintf(stderr, "[coop] test: coin %d brought out at player 2; the stage's coins: %d\n", coinsDone,
			        (int)TFlagManager::getInstance()->getFlag(0x40002));
		}
	}
	// the stage's 100-coin Shine brought out, frames after joining, and player 2
	// put on it once it has come out
	static TShine* shine;
	static int shineAt;
	if (const char* t = getenv("SMS_COOP_TEST_SHINE")) {
		if (!shineAt && s_testJoined && s_frame >= s_testJoinFrame + atoi(t) && gpItemManager) {
			shineAt = s_frame;
			shine   = gpItemManager->makeShineAppearWithDemo("シャイン（１００枚コイン用）", "シャイン（１００枚コイン用）カメラ",
			                                                 m->mPosition.x, m->mPosition.y + 50.0f, m->mPosition.z);
		}
		if (shine && shineAt && s_frame >= shineAt + 360 && shineAt > 0 && playing()) {
			shineAt      = -1;
			m->mPosition = shine->mPosition;
			m->mPosition.y -= 60.0f;
			fprintf(stderr, "[coop] test: player 2 put on the Shine at %.0f %.0f %.0f (state %d)\n", shine->mPosition.x,
			        shine->mPosition.y, shine->mPosition.z, (int)shine->mState);
			// the item group's hit check gives it to them, as it does to Mario
		}
	}
	if (getenv("SMS_COOP_DEBUG") && s_frame % 30 == 0) {
		TMarioGamePad* pad = m->getGamePad();
		fprintf(stderr, "[coop] frame %d: player 2 at %.0f %.0f %.0f status %x stick %.2f,%.2f; player 1 at %.0f %.0f %.0f\n",
		        s_frame, m->mPosition.x, m->mPosition.y, m->mPosition.z, (unsigned)m->mStatus,
		        pad ? pad->getMainStickX() : 0.0f, pad ? pad->getMainStickY() : 0.0f, p1()->mPosition.x, p1()->mPosition.y,
		        p1()->mPosition.z);
		fprintf(stderr, "[coop] %d object moves led by player 2 lately; player 2 health %d, water %d\n", s_swapCount,
		        (int)m->mHealth, m->getFludd() ? (int)m->getFludd()->mCurrentWater : -1);
		s_swapCount = 0;
		if (s_p1.cam && s_p2.cam) {
			const Vec& c1 = s_p1.cam->getUnk124Vec();
			const Vec& c2 = s_p2.cam->getUnk124Vec();
			fprintf(stderr, "[coop] camera 1 at %.0f %.0f %.0f tilt %.0f zoom %.2f; camera 2 at %.0f %.0f %.0f tilt %.0f\n", c1.x,
			        c1.y, c1.z, port_camera_mouse_pitch, port_camera_zoom, c2.x, c2.y, c2.z, s_p2.camPitch);
		}
	}
	// player 2 down (a lost life, a pit): a life from the shared count, as
	// Mario's is; back beside player 1 a moment later, or out on the last one
	if (m->checkFlag(MARIO_FLAG_GAME_OVER)) {
		const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
		TFlagManager* flags = TFlagManager::getInstance();
		if (s_respawnFrames == 0) {
			s_respawnFrames = s_frame + fps * 3;
			if (flags->getFlag(kLivesFlag) > 0) {
				takeLife();
				notice("Luigi lost a life.", kNoticeFrames / 2);
			} else {
				s_lastLife = true; // the death plays out, then they leave the screen
			}
		} else if (s_frame >= s_respawnFrames) {
			s_respawnFrames = 0;
			if (s_lastLife) {
				s_lastLife     = false;
				s_out          = true;
				s_livesWhenOut = flags->getFlag(kLivesFlag);
				activate(false);
				notice("Luigi is out of lives. The next 1-Up brings him back.", kNoticeFrames);
				return;
			}
			revive(m);
		}
	}
	// fell out of the world, or left far behind (player 1 took a pipe, a cannon)
	const f32 dx = m->mPosition.x - p1()->mPosition.x, dz = m->mPosition.z - p1()->mPosition.z;
	if (!m->checkFlag(MARIO_FLAG_GAME_OVER) && (m->mPosition.y < -20000.0f || dx * dx + dz * dz > 30000.0f * 30000.0f))
		revive(m);
}

// Objects near player 2 move as if player 2 were Mario: an enemy goes after
// whoever is nearest, an NPC talks to whoever is beside it.
struct Swap {
	void* object;
	Ctx saved;
};
Swap s_swaps[16];
int s_swapDepth;

f32 dist2(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b)
{
	const f32 x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
	return x * x + y * y + z * z;
}

// Which player an object plays with this move: the one touching it (its hits
// this frame, as the hit check found them) when only one is, else the one
// standing on it, else the nearer.
TMario* playerFor(THitActor* a)
{
	bool touch1 = false, touch2 = false;
	for (int i = 0; i < a->getColNum(); i++) {
		THitActor* c = a->getCollision(i);
		touch1      = touch1 || c == (THitActor*)s_p1.mario;
		touch2      = touch2 || c == (THitActor*)s_p2.mario;
	}
	if (touch1 != touch2)
		return touch2 ? s_p2.mario : s_p1.mario;
	// ...else the one standing on it (a platform asking whether Mario is on it)
	const bool on1 = s_p1.mario->mGroundPlane && (const void*)s_p1.mario->mGroundPlane->mActor == (const void*)a;
	const bool on2 = s_p2.mario->mGroundPlane && (const void*)s_p2.mario->mGroundPlane->mActor == (const void*)a;
	if (on1 != on2)
		return on2 ? s_p2.mario : s_p1.mario;
	return dist2(a->mPosition, s_p2.mario->mPosition) < dist2(a->mPosition, s_p1.mario->mPosition) ? s_p2.mario
	                                                                                                  : s_p1.mario;
}

void objectMove(void* object, int after)
{
	if (after) {
		if (s_swapDepth > 0 && s_swaps[s_swapDepth - 1].object == object) {
			Swap& w = s_swaps[--s_swapDepth];
			gpMarioOriginal = w.saved.mario;
			gpMarioAddress  = w.saved.mario;
			SMS_SetMarioAccessParams();
			gpCamera = w.saved.cam;
		}
		return;
	}
	if (!s_active || s_inP2 || s_swapDepth >= 16 || !s_p1.mario || gpMarioOriginal != s_p1.mario)
		return;
	// any object the hit check knows (a TLiveActor, a gate's TTakeActor...), not the players
	THitActor* a = dynamic_cast<THitActor*>(static_cast<JDrama::TViewObj*>(object));
	if (!a || dynamic_cast<TMario*>(a) || s_p2.mario->checkFlag(MARIO_FLAG_GAME_OVER) || playerFor(a) != s_p2.mario)
		return;
	s_swapCount++;
	Swap& w        = s_swaps[s_swapDepth++];
	w.object       = object;
	w.saved.mario  = gpMarioOriginal;
	w.saved.cam    = gpCamera;
	gpMarioOriginal = s_p2.mario;
	gpMarioAddress  = s_p2.mario;
	SMS_SetMarioAccessParams();
	gpCamera = s_p2.cam; // what is in sight of the player it plays with (an NPC awake beside player 2)
}

// A test's player 2 input (SMS_COOP_TEST_PAD=stick x:y@frame+frames,...;
// "a@frame+frames" presses A; frames since they joined): what controller 2 reads.
struct TestPress {
	int x, y;
	u16 buttons;
	int at, until;
};
TestPress s_test[32];
int s_testCount = -1;

void padRead(void* statusv)
{
	if (!s_wanted || !port_coop_player2())
		return;
	PADStatus* status = (PADStatus*)statusv;
	if (s_testCount < 0) {
		s_testCount = 0;
		const char* e = getenv("SMS_COOP_TEST_PAD");
		char buf[512];
		snprintf(buf, sizeof buf, "%s", e ? e : "");
		for (char* tok = strtok(buf, ","); tok && s_testCount < 32; tok = strtok(nullptr, ",")) {
			TestPress p = { 0, 0, 0, 0, 0 };
			int len     = 8;
			if (strchr("abrxyzl", tok[0]) && tok[1] == '@') {
				p.buttons = tok[0] == 'a' ? PAD_BUTTON_A : tok[0] == 'b' ? PAD_BUTTON_B : tok[0] == 'x' ? PAD_BUTTON_X
				          : tok[0] == 'y' ? PAD_BUTTON_Y : tok[0] == 'z' ? PAD_TRIGGER_Z : tok[0] == 'l' ? PAD_TRIGGER_L
				                                                                         : PAD_TRIGGER_R;
				if (sscanf(tok + 1, "@%d+%d", &p.at, &len) < 1)
					continue;
			} else if (sscanf(tok, "%d:%d@%d+%d", &p.x, &p.y, &p.at, &len) < 3) {
				continue;
			}
			p.until             = p.at + len;
			s_test[s_testCount++] = p;
		}
	}
	PADStatus& s = status[1];
	if (s_testCount) {
		s.err = PAD_ERR_NONE;
		for (int i = 0; i < s_testCount; i++)
			if (s_frame - s_testJoinFrame >= s_test[i].at && s_frame - s_testJoinFrame < s_test[i].until) {
				if (s_test[i].buttons)
					s.button |= s_test[i].buttons;
				else
					s.stickX = (s8)s_test[i].x, s.stickY = (s8)s_test[i].y;
				if (s_test[i].buttons & PAD_TRIGGER_R)
					s.triggerRight = 255;
				if (s_test[i].buttons & PAD_TRIGGER_L)
					s.triggerLeft = 255;
			}
	}
	// a talk (either player's) reads controller 1: player 2's A and B move it on too
	TMarDirector* d = director();
	if (s_active && d && d == s_dir && d->isTalkModeNow()) {
		status[0].button |= s.button & (PAD_BUTTON_A | PAD_BUTTON_B);
		if (s.analogA > status[0].analogA)
			status[0].analogA = s.analogA;
		if (s.analogB > status[0].analogB)
			status[0].analogB = s.analogB;
	}
}

int allowSaveStates() { return !s_active; }
const char* saveStatesOffReason() { return "Save states are off while two play"; }

PortExtension s_extension;

} // namespace

// decomp-patches/zzzzzz-coop-09: the game's HUD's perform (`console`, its
// `flags`: 1 its update, 2 its second pass, 8 its draw). Player 2's HUD
// updates with it, with their globals; while the screen is split, both are
// drawn a part at a time, each player's meters in their view (drawHuds). 0
// leaves the game's perform to itself.
extern "C" int port_coop_hud(void* console, unsigned flags, void* graphics)
{
	TGCConsole2* own = (TGCConsole2*)console;
	if (s_hudBusy || !s_console2 || !s_made || !gpMarDirector || gpMarDirector != s_dir
	    || own != gpMarDirector->getConsole())
		return 0;
	JDrama::TGraphics* g = (JDrama::TGraphics*)graphics;
	s_hudBusy = true;
	// its update and its second pass (the balloon's, the counters' and the
	// gauge's Yoshi juice look)
	if (const unsigned update = flags & 3) {
		own->perform(update, g);
		if (s_active) {
			InP2 in;
			// the counters' bursts are not made for it (port_hud_hidden)
			port_hud_set_drawn(PORT_HUD_HEALTH | PORT_HUD_WATER);
			s_console2Updating = true;
			s_console2->perform(update, g);
			s_console2Updating = false;
			port_hud_set_drawn(PORT_HUD_EVERYTHING);
		}
	}
	if (flags & 8) {
		if (s_split && s_active)
			drawHuds(own, g);
		else
			own->perform(8, g);
	}
	s_hudBusy = false;
	return 1;
}

// decomp-patches/zzzzzz-coop-10: the director's check for a change of state,
// as BetterSunshineEngine makes it (checkBootOut: before it, `after` 0; after
// it, 1). After a Shine Get that keeps the players in the stage (Super Mario
// Eclipse's no-boot Shines), the mod puts gpMarioAddress back in play once the
// camera or the save prompt is done, if it is in its Shine Get: player 1 was,
// and player 2, who took the Shine, stayed in their Shine Get pose for good.
// While player 2 is in the Shine Get of the Shine the director has, they are
// "the player" for it (not camera 2: the Shine Get plays on camera 1).
static TMario* s_shineGetSaved;
extern "C" void port_coop_shine_get_player(void* director, int after)
{
	if (after) {
		if (s_shineGetSaved) {
			gpMarioOriginal = s_shineGetSaved;
			gpMarioAddress  = s_shineGetSaved;
			SMS_SetMarioAccessParams();
			s_shineGetSaved = nullptr;
		}
		return;
	}
	TMarDirector* d = (TMarDirector*)director;
	TMario* m       = s_p2.mario;
	if (!s_active || s_inP2 || !m || !d || !d->unk25C || m->unk384 != (THitActor*)d->unk25C
	    || m->mStatus != MARIO_STATUS_WIN_DEMO || gpMarioOriginal != s_p1.mario)
		return;
	s_shineGetSaved = gpMarioOriginal;
	gpMarioOriginal = m;
	gpMarioAddress  = m;
	SMS_SetMarioAccessParams();
}

// decomp-patches/zzzzzz-coop-08: where a sound is, as the nearer of the two
// cameras hears it (`camSpace` as camera 1's view has it, made camera 2's when
// that is nearer; `prev`, last frame's, made this frame's as the sound goes
// from one camera to the other, so its Doppler has no jump). Volume, pan,
// surround, priority and the distance at which sounds are dropped all follow.
extern "C" void port_coop_hear(Vec* camSpace, Vec* prev, const Vec* pos)
{
	if (!s_earMtx || !pos)
		return;
	Vec at = *pos, other; // (the SDK's PSMTXMultVec takes a non-const source)
	MTXMultVec(*s_earMtx, &at, &other);
	const f32 d1 = camSpace->x * camSpace->x + camSpace->y * camSpace->y + camSpace->z * camSpace->z;
	const f32 d2 = other.x * other.x + other.y * other.y + other.z * other.z;
	const Vec chosen = d2 < d1 ? other : *camSpace;
	const Vec other2 = d2 < d1 ? *camSpace : other;
	// last frame's from the other camera (nearer to where that one has it)
	const f32 toChosen = (prev->x - chosen.x) * (prev->x - chosen.x) + (prev->y - chosen.y) * (prev->y - chosen.y)
	                     + (prev->z - chosen.z) * (prev->z - chosen.z);
	const f32 toOther = (prev->x - other2.x) * (prev->x - other2.x) + (prev->y - other2.y) * (prev->y - other2.y)
	                    + (prev->z - other2.z) * (prev->z - other2.z);
	if (toOther < toChosen)
		*prev = chosen;
	*camSpace = chosen;
}

// decomp-patches/zzzzzz-coop-08: the stage is going (TMarDirector's
// destructor): player 2, camera 2 and their HUD go with its heap.
extern "C" void port_coop_stage_exit(void)
{
	s_made             = false;
	s_split            = false;
	s_console2         = nullptr;
	s_earMtx           = nullptr;
	port_coop_ear_pos  = nullptr;
	port_coop_views[0] = port_coop_views[1] = nullptr;
}

// decomp-patches/zzzzzz-coop-03: the cube the other player is in (player 2's
// while player 1's globals are in force, player 1's while player 2's are), of
// the area cubes or one of the fast cubes; -1 without a second player.
extern "C" int port_coop_other_cube(const void* manager)
{
	if (!s_active || !s_made || !gpMarDirector || gpMarDirector != s_dir)
		return -1;
	const Ctx& other = s_inP2 ? s_p1 : s_p2;
	const void* m[4] = { gpCubeArea, gpCubeFastA, gpCubeFastB, gpCubeFastC };
	for (int i = 0; i < 4; i++)
		if (manager == m[i])
			return other.cubes[i];
	return -1;
}

// decomp-patches/zzzzzz-coop-04: player 2 while they play this stage (not
// down), for the item group's hit check and the water's reach; null otherwise.
static TMario* playingP2()
{
	TMario* m = p2();
	if (!s_active || !m || !gpMarDirector || gpMarDirector != s_dir || m->checkFlag(MARIO_FLAG_GAME_OVER))
		return nullptr;
	return m;
}
extern "C" THitActor* port_coop_player2_actor(void) { return playingP2(); }
// decomp-patches/zzzzzz-coop-07: player 2 for the wire manager
extern "C" TTakeActor* port_coop_wire_player2(void) { return playingP2(); }

// decomp-patches/zzzzzz-coop-06: game code run again as player 2 (a pass of
// TMapWarp's), and where goop's enemies come up (each player's place in turn).
static Ctx s_gameP2Saved;
static bool s_gameP2Entered;
extern "C" int port_coop_enter_player2(void)
{
	if (s_gameP2Entered || !playingP2() || !enterP2(s_gameP2Saved))
		return 0;
	s_gameP2Entered = true;
	return 1;
}
extern "C" void port_coop_leave_player2(void)
{
	if (!s_gameP2Entered)
		return;
	s_gameP2Entered = false;
	leaveP2(s_gameP2Saved);
}
extern "C" const JGeometry::TVec3<f32>* port_coop_spawn_center(const JGeometry::TVec3<f32>* mario)
{
	static unsigned turn;
	TMario* m = playingP2();
	return m && (turn++ & 1) ? &m->mPosition : mario;
}
extern "C" const Vec* port_coop_player2_pos(void)
{
	TMario* m = playingP2();
	return m ? (const Vec*)&m->mPosition : nullptr;
}

// decomp-patches/zzzzzz-coop-05: the person player 2 can talk to, found as the
// director finds Mario's, with player 2 as Mario.
TBaseNPC* port_coop_find_talk_npc(TMarDirector* director)
{
	if (!playingP2() || director != s_dir)
		return nullptr;
	InP2 in;
	return director->findNearestTalkNPC();
}

// decomp-patches/zzzzzz-coop-02: the camera a controller's player sees
// through, for the controller's meanings (TMarioGamePad::updateMeaning reads
// whether it is in the close-up view Y opens). Controller 2 is player 2's,
// with camera 2, while they play this stage; everything else gpCamera.
CPolarSubCamera* port_coop_pad_camera(const TMarioGamePad* pad)
{
	if (s_made && s_active && s_p2.cam && gpApplication.mAppState == TApplication::APP_STATE_GAMEPLAY
	    && gpMarDirector == s_dir && pad == gpMarDirector->getGamePad(1))
		return s_p2.cam;
	if (s_made && s_inP2 && s_p1.cam && pad == gpMarDirector->getGamePad(0))
		return s_p1.cam; // controller 1 updated while player 2's globals are in force
	return gpCamera;
}

namespace {

struct Register {
	Register()
	{
		const char* e = getenv("SMS_COOP");
		s_wanted      = e && *e && strcmp(e, "0") != 0 && !getenv("SMS_MULTIPLAYER");
		if (!s_wanted)
			return;
		const char* layout = getenv("SMS_COOP_SPLIT");
		s_layout           = layout && (!strcmp(layout, "stacked") || !strcmp(layout, "horizontal")) ? 1 : 0;
		memset(&s_extension, 0, sizeof s_extension);
		s_extension.name                = "coop";
		s_extension.frame               = frame;
		s_extension.afterMovement       = afterMovement;
		s_extension.stageSetup          = stageSetup;
		s_extension.padRead             = padRead;
		s_extension.objectMove          = objectMove;
		s_extension.allowSaveStates     = allowSaveStates;
		s_extension.saveStatesOffReason = saveStatesOffReason;
		s_extension.sceneLoaded         = sceneLoaded;
		s_extension.drawStage           = drawStage;
		s_extension.sound               = soundHook;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
