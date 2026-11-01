/* The XDK's Win32 subset for the shims in this directory, without the macros
 * that break standard C++ (min/max). port_compat.h, forced into the game's
 * units, erases __declspec (an MWCC spelling): the XDK headers need theirs, or
 * their selectany tables become strong definitions in every unit. */
#ifndef XE_WIN_H
#define XE_WIN_H
#ifndef NOMINMAX
#define NOMINMAX
#endif
#pragma push_macro("__declspec")
#pragma push_macro("asm")
#undef __declspec
#undef asm
#include <xtl.h>
#pragma pop_macro("asm")
#pragma pop_macro("__declspec")
#endif
