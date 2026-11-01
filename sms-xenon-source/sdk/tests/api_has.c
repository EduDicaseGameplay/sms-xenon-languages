/* SMS_API_HAS: a function is there only when the game's table is big enough
   to hold it and it is set. */
#include <sms_mod.h>

#include <stdio.h>
#include <string.h>

static void log_fn(SmsMod* m, int l, const char* f, ...) { (void)m; (void)l; (void)f; }
static const char* info_fn(SmsMod* m, const char* k) { (void)m; (void)k; return ""; }

int main(void)
{
	SmsCoreApi core;
	const SmsCoreApi* p = &core;
	int failed = 0;
	memset(&core, 0, sizeof core);
	core.log = log_fn;
	core.mod_info = info_fn;

	core.size = sizeof core;
	if (!SMS_API_HAS(p, log) || !SMS_API_HAS(p, mod_info)) {
		puts("FAIL: a full table lacks a function it has");
		failed = 1;
	}
	if (SMS_API_HAS(p, host_runtime)) {
		puts("FAIL: an unset function is there");
		failed = 1;
	}
	/* a game of API 1.0: its table ends before mod_info */
	core.size = (uint32_t)offsetof(SmsCoreApi, mod_info);
	if (SMS_API_HAS(p, mod_info) || !SMS_API_HAS(p, log)) {
		puts("FAIL: a smaller table's size is not respected");
		failed = 1;
	}
	p = NULL;
	if (SMS_API_HAS(p, log)) {
		puts("FAIL: no table, yet a function");
		failed = 1;
	}
	if (!failed)
		puts("ok");
	return failed;
}
