// Extensions' text in the window (gx_ext_overlay.cpp): a notice at the top and
// a small status panel in a corner, set from any thread with GXPC_ExtOverlaySet
// (sms_gx/gx_pc.h).
#ifndef SMS_GX_EXT_OVERLAY_H
#define SMS_GX_EXT_OVERLAY_H

#include <string>

namespace gx {

// Hides `panel` if its text is still `text` (a notice of sms_gx's own that
// something else may have replaced since). Any thread.
void extOverlayClearIf(int panel, const std::string& text);

#ifdef SMS_GX_HAVE_SDL2
// On the GL thread, between presenting the frame and the swap.
void extOverlayRender(int winW, int winH);
#else
inline void extOverlayRender(int, int) {}
#endif

}  // namespace gx

#endif
