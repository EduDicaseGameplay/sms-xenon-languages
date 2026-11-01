// sms_mod.hpp (and so sms_mod.h) builds as strict C++11 with every warning an error.
#include <sms_mod.hpp>

struct Check : sms::Mod {
	using sms::Mod::Mod;
	int frames = 0;

	int load() override
	{
		optionBool("show", "Show", true);
		on<SmsEvFrame>(SMS_EV_FRAME, [this](SmsEvFrame&) { frames++; });
		on<SmsEvKey>(SMS_EV_KEY, [](SmsEvKey& k) { return k.down ? SMS_CANCEL : SMS_CONTINUE; });
		return SMS_API_HAS(&api(), core) ? 0 : 1;
	}
};

SMS_MOD_CLASS(Check)
