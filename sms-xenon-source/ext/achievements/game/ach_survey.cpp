// Achievements (ext/achievements/README.md): the survey, a test aid for
// writing achievements. With SMS_ACHIEVEMENTS_DEBUG=1 and
// SMS_ACHIEVEMENTS_TEST=survey the tour warps through the stops and, at each,
// lists what achievements are made of: the 1-Ups (with the slots
// ach_oneups.cpp names them by), the Shines (with their flags) and any other
// actor with "star" in its name. Each line starts "survey:", for a script.
#include "achievements.h"

#include <Enemy/Conductor.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBase.hpp>
#include <MoveBG/MapObjItem2.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/FlagManager.hpp>
#include <ctype.h>
#include <string.h>

namespace {

const unsigned kShineType = 0x20000013; // MapObjInit's shine

// An actor's name (TNameRef keeps it protected).
struct Named : JDrama::TNameRef {
	static const char* of(const JDrama::TNameRef* r) { return static_cast<const Named*>(r)->mName; }
};

bool starName(const char* name)
{
	if (!name)
		return false;
	for (const char* p = name; p[0] && p[1] && p[2] && p[3]; ++p)
		if (tolower(p[0]) == 's' && tolower(p[1]) == 't' && tolower(p[2]) == 'a' && tolower(p[3]) == 'r')
			return true;
	return false;
}

} // namespace

namespace ach {

void survey()
{
	const int a = area(), e = episode();
	int oneUps = 0;
	if (gpItemManager)
		for (int i = 0; i < gpItemManager->getObjNum(); ++i)
			if (TMushroom1up* m = dynamic_cast<TMushroom1up*>(gpItemManager->getObj(i))) {
				++oneUps;
				log("survey: %d %d 1up slot %#x at %.0f %.0f %.0f", a, e, 0xAC + i * 4, m->mPosition.x, m->mPosition.y,
				    m->mPosition.z);
			}
	int shines = 0, stars = 0;
	if (gpConductor)
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it) {
			TLiveManager* m = *it;
			if (!m)
				continue;
			for (int i = 0; i < m->getObjNum(); ++i) {
				TLiveActor* actor = m->getObj(i);
				if (!actor)
					continue;
				if (actor->getActorType() == kShineType) {
					const u32 id = static_cast<TMapObjBase*>(actor)->getEventId();
					++shines;
					log("survey: %d %d shine %u (%s) got %d dead %d at %.0f %.0f %.0f", a, e, id, Named::of(actor),
					    id < 0x100 ? (int)TFlagManager::getInstance()->getShineFlag(id) : -1,
					    actor->checkLiveFlag(LIVE_FLAG_DEAD) ? 1 : 0, actor->mPosition.x, actor->mPosition.y,
					    actor->mPosition.z);
				} else if (starName(Named::of(actor))) {
					++stars;
					log("survey: %d %d star-named %s type %#x at %.0f %.0f %.0f", a, e, Named::of(actor),
					    actor->getActorType(), actor->mPosition.x, actor->mPosition.y, actor->mPosition.z);
				}
			}
		}
	log("survey: %d %d done: %d 1-Ups, %d Shines, %d star-named", a, e, oneUps, shines, stars);
}

} // namespace ach
