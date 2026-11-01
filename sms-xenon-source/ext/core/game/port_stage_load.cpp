// A stage about to load (port_ext.h's chooseStageLoad and stageLoading): game
// code, with the game's flags and headers.
//
// TApplication::proc's APP_STATE_GAMEPLAY is where every stage loads, with
// its episode chosen: by a world's episode select (its own app state, before),
// by TMarDirector::moveStage for the plaza, Hotel Delfino, Pinna Park and
// Casino Delfino, by a lost life's reload. Extensions hear of it there
// (zzzzz-ext-24-stage-loading), before the movie some stages open with
// (checkAdditionalMovie: the Airstrip's, a secret course's), so the movie is
// that of the stage that loads. After the movie the loop comes back to
// APP_STATE_GAMEPLAY for the same load, with mPrevArea set to the stage
// itself (the loop's mPrevArea = mCurrArea): the extensions are not asked
// again, and a mPrevArea one of them chose is put back.
#include "port_ext.h"

#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/ScenarioArchiveName.hpp>
#include <System/StageUtil.hpp>

namespace {

// The load the extensions were asked about, for the pass after its movie.
bool s_asked;
u8 s_area, s_episode;
bool s_fromChosen; // an extension changed mPrevArea
u8 s_fromArea, s_fromEpisode;

// A world's way in (TMarDirector::moveStage: with no episode, its select).
bool isWorldEntry(int area)
{
	return area == 2 || area == 3 || area == 4 || area == 5 || area == 6 || area == 8 || area == 9;
}

} // namespace

extern "C" int port_ext_stage_exists(int area, int episode)
{
	if (area < 0 || area > 254 || episode < 0 || episode > 254 || !gpApplication.unk30)
		return 0;
	TNameRefPtrAryT<TNameRefAryT<TScenarioArchiveName> >& stages = *gpApplication.unk30;
	if ((u32)area >= stages.getChildren().size())
		return 0;
	TNameRefAryT<TScenarioArchiveName>* episodes = stages.getChildren()[area];
	return episodes && (u32)episode < episodes->size();
}

extern "C" int port_ext_load_instead(int area, int episode)
{
	TGameSequence& cur = gpApplication.mCurrArea;
	if (area == cur.unk0 && episode == cur.unk1)
		return 1;
	if (area == 15 || cur.unk0 == 15 || !port_ext_stage_exists(area, episode))
		return 0;
	const u8 wasArea = cur.unk0, wasEpisode = cur.unk1;
	TGameSequence& next = gpApplication.mNextArea;
	if (next.unk0 == wasArea && next.unk1 == wasEpisode) {
		next.unk0 = (u8)area;
		next.unk1 = (u8)episode;
	}
	cur.unk0 = (u8)area;
	cur.unk1 = (u8)episode;
	// As the game would have come here: a world's episode recorded as its
	// episode select records it (TSelectDir), the plaza's as moveStage does;
	// a new visit's coins afresh (moveStage resets them going to another
	// world, the coins of another episode of a world are another visit's).
	TFlagManager* flags = TFlagManager::getInstance();
	if (isWorldEntry(area))
		flags->setFlag(0x40003, episode);
	else if (area == 1 && wasArea != 1)
		flags->setFlag(0x40003, 0);
	if (SMS_getShineStage((u8)area) != SMS_getShineStage(wasArea) || (isWorldEntry(area) && episode != wasEpisode))
		flags->setFlag(0x40002, 0);
	return 1;
}

extern "C" void port_ext_stage_loading(int afterMovie)
{
	TGameSequence& cur  = gpApplication.mCurrArea;
	TGameSequence& prev = gpApplication.mPrevArea;
	// (the loop's mPrevArea = mCurrArea after the movie: else the movie was
	// left another way, a reset, and this is another load)
	if (afterMovie && s_asked && cur.unk0 == s_area && cur.unk1 == s_episode && prev.unk0 == cur.unk0
	    && prev.unk1 == cur.unk1) {
		if (s_fromChosen) {
			prev.unk0 = s_fromArea;
			prev.unk1 = s_fromEpisode;
		}
		return;
	}
	const u8 fromArea = prev.unk0, fromEpisode = prev.unk1;
	port_ext_choose_stage_load(cur.unk0, cur.unk1);
	s_asked       = true;
	s_area        = cur.unk0;
	s_episode     = cur.unk1;
	s_fromChosen  = prev.unk0 != fromArea || prev.unk1 != fromEpisode;
	s_fromArea    = prev.unk0;
	s_fromEpisode = prev.unk1;
}
