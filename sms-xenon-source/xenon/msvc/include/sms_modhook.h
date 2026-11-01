/* The Xbox 360 build loads no code mods: every hook of
 * src/port_include/sms_modhook.h is its original. The upstream header still
 * declares the registry and the member-to-free-function types the patched
 * code names; the macros that are GCC statement expressions are replaced, and
 * the patches' inline ones become their original branch in
 * xenon/tools/xrewrite.py. sms_mod_word() answers "not modded" (xenon/). */
#ifndef XE_SMS_MODHOOK_H
#define XE_SMS_MODHOOK_H
#include "../../../src/port_include/sms_modhook.h"

#undef SMS_MOD_SITE
#undef SMS_MOD_GPR
#undef SMS_MOD_CALLF
#undef SMS_MOD_CALLF_R
#undef SMS_MOD_CALLM
#undef SMS_MOD_CALLM_R
#undef SMS_MOD_CALL
#undef SMS_MOD_CALL_R
#undef SMS_MOD_WORD
#undef SMS_MOD_HILO
#define SMS_MOD_SITE(addr) ((void*)0)
#define SMS_MOD_GPR(reg, value) ((void)0)
#define SMS_MOD_CALLF(addr, fn, original, ...) do { original; } while (0)
#define SMS_MOD_CALLF_R(addr, fn, original, ...) (original)
#define SMS_MOD_CALLM(addr, Class, method, original, obj, ...) do { original; } while (0)
#define SMS_MOD_CALLM_R(addr, Class, method, original, obj, ...) (original)
#define SMS_MOD_CALL(addr, fnType, original, ...) do { original; } while (0)
#define SMS_MOD_CALL_R(addr, fnType, original, ...) (original)
#define SMS_MOD_WORD(addr, original) ((unsigned int)(original))
#define SMS_MOD_MASK(addr, original) ((unsigned int)(original))
#define SMS_MOD_ORI(addr, original) ((unsigned int)(original))
#define SMS_MOD_HILO(hiAddr, loAddr, original) (&(original)[0])
#endif
