// The quit key's "are you sure" prompt, drawn with the debug overlay's
// renderer: the quit key (Escape) opens it, and only its Quit button ends the
// game, so a stray press no longer loses unsaved progress.
#ifndef SMS_GX_QUIT_PROMPT_H
#define SMS_GX_QUIT_PROMPT_H

union SDL_Event;

namespace gx {

#ifdef SMS_GX_HAVE_SDL2
bool quitPromptVisible();
void quitPromptOpen();
// True when the event was the prompt's and must not reach the game.
bool quitPromptHandleEvent(const SDL_Event& ev);
// True once Quit was picked: the caller ends the game.
bool quitPromptConfirmed();
// On the GL thread, between presenting the frame and the swap.
void quitPromptRender(int winW, int winH);
#else
inline bool quitPromptVisible() { return false; }
inline void quitPromptOpen() {}
inline bool quitPromptHandleEvent(const SDL_Event&) { return false; }
inline bool quitPromptConfirmed() { return false; }
inline void quitPromptRender(int, int) {}
#endif

}  // namespace gx

#endif
