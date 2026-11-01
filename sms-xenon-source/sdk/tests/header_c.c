/* sms_mod.h builds as strict C99 with every warning an error. */
#include <sms_mod.h>

#include <string.h>

SMS_MOD_DEFINE()

static int on_frame(SmsMod* mod, SmsEvent* ev, void* user)
{
	(void)mod; (void)ev; (void)user;
	return SMS_CONTINUE;
}

SMS_MOD_EXPORT int sms_mod_load(SmsMod* mod, const SmsApi* api)
{
	if (!SMS_API_HAS(api->core, subscribe))
		return 1;
	api->core->subscribe(mod, SMS_EV_FRAME, on_frame, NULL, SMS_PRIORITY_NORMAL);
	if (SMS_API_HAS(api->core, mod_info))
		api->core->log(mod, SMS_LOG_INFO, "%s", api->core->mod_info(mod, "name"));
	if (SMS_API_HAS(api, net) && api->net->role() == SMS_NET_HOST) {
		SmsCmd c;
		SmsNetPlayer p;
		memset(&c, 0, sizeof c);
		c.size = sizeof c;
		c.type = SMS_CMD_BANNER;
		c.i[0] = (int32_t)SMS_RGB(255, 208, 32);
		c.text = "Hello, players";
		api->net->send(SMS_NET_ALL, &c);
		memset(&p, 0, sizeof p);
		p.size = sizeof p;
		if (api->net->player(1, &p) && (p.flags & SMS_NETP_SURFING))
			api->core->log(mod, SMS_LOG_INFO, "%s surfs", p.name);
	}
	return 0;
}
