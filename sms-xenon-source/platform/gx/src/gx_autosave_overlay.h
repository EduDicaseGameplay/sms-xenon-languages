// Auto save's indicator (gx_autosave_overlay.cpp): a Shine that turns at the
// bottom left of the game's picture while the game saves by itself, then says
// it saved, set from any thread with GXPC_AutoSaveIndicator (sms_gx/gx_pc.h).
#ifndef SMS_GX_AUTOSAVE_OVERLAY_H
#define SMS_GX_AUTOSAVE_OVERLAY_H

namespace gx {

#ifdef SMS_GX_HAVE_SDL2
// On the GL thread, between presenting the frame and the swap.
void autosaveOverlayRender(int winW, int winH);
#else
inline void autosaveOverlayRender(int, int) {}
#endif

}  // namespace gx

#endif
