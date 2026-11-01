// Auto save (ext/autosave/README.md): with SMS_AUTO_SAVE=1 (auto_save = on)
// the game saves by itself where it would ask the player to save: after a
// Shine, a blue coin, a nozzle's box opened for good and Yoshi hatched. A
// Shine turns at the bottom left of the window while it saves (sms_gx's
// GXPC_AutoSaveIndicator), then stops and says Saved, to the save sound of
// the game's own prompt.
//
// It saves as the prompt's Save does (TCardSave): it reads the card's slots
// first and saves only over this file's own last save or an empty slot.
// Whatever the prompt would have to ask about (no card, no save file on it
// yet, a slot another game has saved over since, a damaged file) is left to
// the prompt, which then opens as ever.
//
// A game over's prompt (Continue or quit) still asks. Super Mario Eclipse has
// auto saving of its own (BetterSunshineEngine's Save Prompts setting), so
// with BetterSunshineEngine loaded this leaves every prompt to it.
#include "port_ext.h"

#include <JSystem/JSupport/JSUMemoryOutputStream.hpp>
#include <MSound/MSound.hpp>
#include <MSound/SoundEffects.hpp>
#include <System/Application.hpp>
#include <System/CardManager.hpp>
#include <System/FlagManager.hpp>
#include <dolphin/card.h>
#include <dolphin/os.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void GXPC_AutoSaveIndicator(int state) __attribute__((weak));
extern "C" void* sms_mod_target(unsigned int addr);

namespace {

// sms_gx/gx_pc.h's GXPC_AUTOSAVE_*
enum { INDICATOR_OFF, INDICATOR_SAVING, INDICATOR_SAVED, INDICATOR_FAILED };

// The card's commands run on its own thread (TCardManager), above the game's
// in priority, so each is done by the time the call that asks for it returns
// and a save takes a moment of one frame. Should one still be busy, the save
// goes on in the frames after (the steps below).
enum Step {
	STEP_IDLE,
	STEP_SCAN,   // reading the slots
	STEP_WRITE,  // writing the file's slot
	STEP_OPTION, // writing the options block, as the prompt does after
};

Step s_step;
bool s_again; // asked to save again before the last save was done
int s_on = -1;
TCardBookmarkInfo s_marks[3];
int s_slot;

// The Shine turns for at least this long, so a save that takes a moment of
// one frame still shows; then it says Saved, to the save sound.
const u32 kTurnMs = 900;
OSTime s_turnStart;
bool s_savedShown = true;

bool enabled()
{
	if (s_on < 0) {
		const char* v = getenv("SMS_AUTO_SAVE");
		s_on = v && *v && strcmp(v, "0") != 0 && strcmp(v, "off") != 0;
		if (s_on)
			fprintf(stderr, "[autosave] on: the game saves by itself after Shines and blue coins\n");
	}
	return s_on > 0;
}

void indicate(int state)
{
	if (GXPC_AutoSaveIndicator)
		GXPC_AutoSaveIndicator(state);
}

// As the prompt's Save (TCardSave, PROGRESS_UNK2A/2B): a file it may save
// over without asking.
bool maySave()
{
	if (s_slot < 0 || s_slot > 2)
		return false;
	const TCardBookmarkInfo& mark = s_marks[s_slot];
	if (mark.unk0 == 1)
		return false; // damaged: the prompt asks whether to save over it
	return mark.unk18 == 0 || TFlagManager::getInstance()->getLastSaveTime() == (s64)mark.unk8;
}

void startScan()
{
	s_slot = SMSGetApplication()->mSaveFile;
	gpCardManager->getBookmarkInfos(s_marks);
	s_step = STEP_SCAN;
}

// Moves the save on as far as the card lets it now. Returns 0 when it stops
// without saving because the prompt has to ask (the slots read, nothing
// written), else 1.
int advance()
{
	for (;;) {
		const s32 status = gpCardManager->getLastStatus();
		if (s_step != STEP_IDLE && status == CARD_RESULT_BUSY)
			return 1;
		switch (s_step) {
		case STEP_IDLE:
			return 1;

		case STEP_SCAN:
			if (status != CARD_RESULT_READY || !maySave()) {
				fprintf(stderr, "[autosave] not saved (card %d, slot %d): the prompt asks\n", (int)status, s_slot);
				gpCardManager->unmount();
				s_step = STEP_IDLE;
				return 0;
			}
			{
				JSUMemoryOutputStream stream;
				gpCardManager->getWriteStream(&stream);
				TFlagManager::getInstance()->save(stream);
				gpCardManager->writeBlock(s_slot);
			}
			s_step = STEP_WRITE;
			break;

		case STEP_WRITE:
			if (status != CARD_RESULT_READY) {
				fprintf(stderr, "[autosave] the save failed (card %d): the prompt asks\n", (int)status);
				TFlagManager::getInstance()->saveFail();
				gpCardManager->unmount();
				s_step = STEP_IDLE;
				return 0;
			}
			TFlagManager::getInstance()->saveSuccess();
			port_ext_game_saved(s_slot); // other extensions hear of it
			{
				JSUMemoryOutputStream stream;
				gpCardManager->getOptionWriteStream(&stream);
				TFlagManager::getInstance()->saveOption(stream);
				gpCardManager->writeOptionBlock();
			}
			s_step = STEP_OPTION;
			break;

		case STEP_OPTION:
			gpCardManager->unmount();
			fprintf(stderr, "[autosave] saved to file %c\n", 'A' + s_slot);
			s_step       = STEP_IDLE;
			s_savedShown = false;
			if (s_again) {
				s_again = false;
				startScan();
				break;
			}
			return 1;
		}
	}
}

int savePrompt(int kind)
{
	if (kind != 1 && (kind < 3 || kind > 6))
		return 0; // a game over's: Continue or quit
	if (!enabled() || !gpCardManager)
		return 0;
	if (sms_mod_target(0x802995A8))
		return 0; // BetterSunshineEngine decides (its checkBootOut)
	if (s_step != STEP_IDLE) {
		s_again = true; // saved once the save under way is done
		return 1;
	}
	startScan();
	if (!advance())
		return 0;
	s_turnStart  = OSGetTime();
	s_savedShown = false;
	indicate(INDICATOR_SAVING);
	return 1;
}

void frame()
{
	if (s_step != STEP_IDLE && !advance()) {
		// it found what the prompt must ask about only after the stage went
		// on, so the prompt cannot open now: say so, and leave saving to the
		// prompt until the game restarts
		s_on         = 0;
		s_savedShown = true;
		indicate(INDICATOR_FAILED);
		return;
	}
	if (s_step == STEP_IDLE && !s_savedShown
	    && OSTicksToMilliseconds(OSGetTime() - s_turnStart) >= (OSTime)kTurnMs) {
		s_savedShown = true;
		indicate(INDICATOR_SAVED);
		// the prompt's sound for a save done
		if (gpMSound)
			gpMSound->startSoundSystemSE(MSD_SE_SY_DECIDE_COMMON, 0, nullptr, 0);
	}
}

PortExtension s_extension;

struct Register {
	Register()
	{
		s_extension.name       = "autosave";
		s_extension.frame      = frame;
		s_extension.savePrompt = savePrompt;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace
