// What the PC build's left-out units provide, as the 360 build does without them:
// save states, code mods and the mod API, multiplayer, Discord presence, the
// lockstep trace, the GameCube USB adapter, the HD movie pack, the window, and
// the desktop crash reporter (the 360 gets its own). Each answers "not there".
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <port_savestate.h>
#include <port_presence.h>
#include <sms_hd_movies.h>
#include <sms_mod/modhooks.h>
#include "port_platform.h"
#include "pad/gc_adapter.h"
#include "os/crash.h"
#include "trace/trace.h"
#include "modapi/modapi_bridge.h"

// Save states (platform/savestate)
void port_savestate_hotkey(int) {}
int port_savestate_scan(void) { return 0; }
const PortSaveStateInfo* port_savestate_scanned(int) { return NULL; }
const void* port_savestate_picture(int, int, int*, int*) { return NULL; }
const void* port_savestate_world_art(int, int, int*, int*, int*) { return NULL; }
int port_savestate_take(char*, int) { return 0; }
void port_savestate_set_ready(int) {}
int port_savestate_save(const PortSaveStatePortable*) { return 0; }
int port_savestate_can_load_exact(const char*) { return 0; }
int port_savestate_load_exact(const char*, PortSaveStatePortable*, int*) { return 0; }
void port_savestate_notify(const char*) {}
void port_savestate_request_load(const char*) {}
int port_savestate_delete(const char*) { return 0; }
void port_savestate_hint(PortSaveStateHint* out) { memset(out, 0, sizeof *out); }

// Code mods (platform/mods) and the mod API (ext/modapi)
void* sms_mod_target(uint32_t) { return NULL; }
int sms_mod_word(uint32_t, uint32_t*) { return 0; }
void sms_mod_bind_data(uint32_t, void*, int) {}
void sms_mod_activate(void) {}
void sms_mod_start(void) {}
int sms_mod_active(void) { return 0; }
void modh_frame(uint32_t, int) {}
int modh_dispatch(uint32_t, void*, uint32_t) { return 0; }
int modh_wants(uint32_t) { return 0; }
void modh_pad_read(SmsPadStatus*) {}
const char* modh_save_state_reason(void) { return NULL; }

// Presence, window, HD movies, the USB adapter (the trace's movie playback: xe_movie.cpp)
void port_presence_frame(const PortPresence*) {}
void port_window_icon_init(void) {}
void port_window_title_init(void) {}
const char* port_hd_cutscene_directory(void) { return NULL; }
void gc_adapter_start() {}
bool gc_adapter_read(GcAdapterInput*) { return false; }
void gc_adapter_rumble(bool) {}
void gc_adapter_flush_rumble() {}

// Multiplayer (ext/multiplayer)
extern "C" int mp_player_position(int, float*) { return 0; }
extern "C" int mp_radar_player(int, float*, float*, char*, int) { return 0; }
extern "C" void port_spawn_record(void*, const void*, unsigned) {}

// The desktop crash reporter (platform/os/crash_info.cpp)
void port_crash_emit(PortCrashLine&) {}
bool port_crash_module(uintptr_t, uintptr_t*, const char**) { return false; }
void port_crash_prepare_info() {}
void port_crash_note_file(const char*) {}
void port_crash_set_mem1(uintptr_t, size_t) {}
void port_crash_set_thread_describer(PortCrashDescriber) {}
void port_install_windows_exception_logger() {}
void port_windows_report_current(const char*) {}

// Code-mod data the game's units still name behind SMS_MOD_SITE, which is 0 on the 360: never
// read, but link-time code generation (Master) keeps the references a plain build's /OPT:REF
// drops with the dead code around them. The struct as MarioAnimeData.hpp has it.
extern "C" uintptr_t sms_mod_gpr[32];
uintptr_t sms_mod_gpr[32];
struct unkTMarioAnimeFilesStruct {
	uint32_t unk0;
	const char* unk4;
};
unkTMarioAnimeFilesStruct sPlayerAnimeInfos[1];
size_t sPlayerAnimeInfosSize;

// Upstream's PC features of 2026-10-26/27 the 360 build leaves out: co-op (split view, a second
// adapter port), multiplayer, the mod files menu, texture packs, plain textures (ext/look), the
// overlay panels, Vulkan's pipeline cache, DualSense triggers, save states' disc reads (the pause
// menu's QUIT art: none, so no QUIT item). Each answers "not there".
struct PortSetting;
extern "C" {
int GXPC_ControllerCount(void) { return 1; }
void GXPC_ExtOverlayScreen(uint32_t) {}
void GXPC_ExtOverlaySet(int, const char*) {}
void GXPC_SavePipelineCacheSoon(void) {}
void GXPC_SetSplitView(int, float, float, float, float) {}
void GXPC_SetTexturePlain(const void*, size_t) {}
int GXPC_TextureIsPlain(const void*, size_t) { return 0; }
int GXPC_TexturePackHas(const void*, unsigned, unsigned, unsigned) { return 0; }
unsigned char* GXPC_TexturePackImage(const void*, unsigned, unsigned, unsigned, int*, int*) { return NULL; }
int sms_gx_fludd_trigger(int) { return 0; }
void modh_files_event(int, int, int) {}
const PortSetting* mp_port_settings(int* count) { *count = 0; return NULL; }
void* port_disc_archive_file(const char*, const char*, unsigned* size) { *size = 0; return NULL; }
int port_spawn_record_of(const void*, const void**, unsigned*) { return 0; }
}
int MpLocalBody() { return 0; }
bool gc_adapter_read_second(GcAdapterInput*) { return false; }
