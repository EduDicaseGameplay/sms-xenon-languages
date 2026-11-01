/* MSVC passes __VA_ARGS__ on to another macro as a single argument, so the
 * decomp's VERSION_SELECT joined nothing. Re-scanning the call fixes it. */
#ifndef XE_VERSION_H
#define XE_VERSION_H
#include "../../../game/include/version.h"
#define XE_EXPAND(x) x
#undef VERSION_SELECT
#define VERSION_SELECT(...) (XE_EXPAND(VERSION_SELECT_JOIN(__VA_ARGS__, , , , , , , )))
#endif
