/* A built mod exports what the game looks for: sms_mod_api_version, of this
   SDK's major version, and sms_mod_load. */
#include <sms_mod.h>

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define OPEN(p) ((void*)LoadLibraryA(p))
#define SYM(h, n) ((void*)GetProcAddress((HMODULE)(h), n))
#else
#include <dlfcn.h>
#define OPEN(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define SYM(h, n) dlsym(h, n)
#endif

int main(int argc, char** argv)
{
	void* lib;
	SmsModVersionFn version, prerelease;
	if (argc < 2) {
		puts("usage: sms_mod_exports <mod library>");
		return 2;
	}
	lib = OPEN(argv[1]);
	if (!lib) {
		printf("FAIL: cannot load %s\n", argv[1]);
		return 1;
	}
	*(void**)&version = SYM(lib, "sms_mod_api_version");
	if (!version || !SYM(lib, "sms_mod_load")) {
		printf("FAIL: %s does not export sms_mod_api_version and sms_mod_load\n", argv[1]);
		return 1;
	}
	if (version() >> 16 != SMS_MOD_API_MAJOR) {
		printf("FAIL: API %u.%u, not major %d\n", version() >> 16, version() & 0xFFFF, SMS_MOD_API_MAJOR);
		return 1;
	}
	*(void**)&prerelease = SYM(lib, "sms_mod_api_prerelease");
	if (!prerelease || prerelease() != SMS_MOD_API_PRERELEASE) {
		printf("FAIL: %s does not export sms_mod_api_prerelease %d\n", argv[1], SMS_MOD_API_PRERELEASE);
		return 1;
	}
	printf("ok: %s, API %u.%u, pre-release %u\n", argv[1], version() >> 16, version() & 0xFFFF, prerelease());
	return 0;
}
