/* Port override of decomp/libs/dolphin/include/dolphin/types.h (same include guard, so it
 * wins whichever copy is reached first; port_compat.h includes this one).
 * MWCC's u32/s32 are (unsigned) long; on LP64 hosts that is 64 bits, so the
 * port spells them int on all 64-bit hosts (including Windows LLP64), so its platform signatures agree. On ILP32 hosts long is 32 bits and the original
 * spelling (and C++ mangling) is kept. */
#ifndef _DOLPHIN_TYPES_H_
#define _DOLPHIN_TYPES_H_

typedef signed char s8;
typedef unsigned char u8;
typedef signed short int s16;
typedef unsigned short int u16;
#if defined(__LP64__) || defined(_LP64) || defined(_WIN64)
typedef signed int s32;
typedef unsigned int u32;
#else
typedef signed long s32;
typedef unsigned long u32;
#endif
#if defined(__i386__)
/* The GameCube aligns 8-byte members to 8 inside structs; the i386 psABI
 * aligns them to 4, which would lay out every class holding an OSTime or an
 * f64 after a 4-byte boundary (TMarDirector, JUTGamePad, OSThread...)
 * differently from retail. Code mods address those classes with retail
 * offsets, so the 32-bit port keeps retail's alignment for these types.
 * Argument passing is unaffected. */
typedef signed long long int s64 __attribute__((aligned(8)));
typedef unsigned long long int u64 __attribute__((aligned(8)));
#else
typedef signed long long int s64;
typedef unsigned long long int u64;
#endif

typedef float f32;
#if defined(__i386__)
typedef double f64 __attribute__((aligned(8)));
#else
typedef double f64;
#endif

typedef char* Ptr;

typedef int BOOL;

#define FALSE 0
#define TRUE  1

/* Hardware-mapped objects (low-memory OS globals, hw_regs.h register blocks)
 * are *defined* in SDK headers. On the host they become ordinary weak
 * globals, so the per-TU definitions merge at link time; the platform layer
 * initialises the ones that matter (bus clock, memory size, TV mode). */
#ifdef _WIN32
/* COFF has no ELF-style weak data. selectany puts header definitions in
 * COMDAT sections so the linker keeps one copy shared by all game units. */
#define AT_ADDRESS(addr) __attribute__((selectany))
#else
#define AT_ADDRESS(addr) __attribute__((weak))
#endif

#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void*)0)
#endif
#endif

/* Native arm64 macOS reserves the low 4 GiB (PAGEZERO) and puts its shared
 * library cache at 0x180000000-0x300000000, so there a game address x lives
 * at PORT_GAME_LO + x below 0x80000000 (the executable, stacks,
 * port_low_alloc) and at PORT_GAME_HI + x from 0x80000000 (MEM1, at 1 TiB:
 * the system takes addresses above the shared cache too, and on macOS 27
 * something already held 0x780000000 when PORT_GAME_HI was 28 GiB). A pointer
 * truncated to a u32 is the game address, and tools/gameptr's pass plugin
 * rebases every integer the game and platform code turn into a pointer
 * (docs/MACOS-ARM64.md; it defines PORT_GAMEPTR_PLUGIN); code built
 * without it (platform/gx) uses port_game_ptr. Both bases are 0 on every
 * other host. */
#if defined(__APPLE__) && defined(__aarch64__)
#define PORT_GAME_LO 0x100000000ull
#define PORT_GAME_HI 0x10000000000ull
#else
#define PORT_GAME_LO 0ull
#define PORT_GAME_HI 0ull
#endif
/* Values below PORT_GAME_MIN are small integers passed as pointers (null,
 * OSMessage codes), never game addresses: they are not rebased. */
#define PORT_GAME_MIN 0x10000u
#define PORT_GAME_REBASE(a) ((unsigned long long)(a) | ((a) & 0x80000000u ? PORT_GAME_HI : PORT_GAME_LO))
#define port_game_ptr(a) ((u32)(a) >= PORT_GAME_MIN ? (void*)PORT_GAME_REBASE((u32)(a)) : (void*)(unsigned long long)(u32)(a))

/* PTR32(T): a pointer field of a struct laid over file data (see the decomp's
 * types.h). With 4-byte pointers it is T*. With 8-byte pointers it stays a
 * 4-byte slot holding the address, which works because everything the game
 * can point at has a 32-bit game address (below 4 GiB, or rebased as above:
 * MEM1, static data, low thread stacks); any other address is a port bug,
 * reported by port_ptr32_trap. */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#ifdef __cplusplus
extern "C" void port_ptr32_trap(const void* p);
template <typename T> struct port_ptr32 {
	u32 raw;
#ifdef PORT_GAMEPTR_PLUGIN
	/* a plain cast, which the plugin rebases: so (uintptr_t)(void*)field
	 * folds back to the offset a relocation adds, as on every other host */
	operator T*() const { return (T*)(unsigned long long)raw; }
#else
	operator T*() const { return (T*)port_game_ptr(raw); }
#endif
	T* operator->() const { return (T*)*this; }
	port_ptr32& operator=(T* p)
	{
		unsigned long long a = (unsigned long long)p;
		/* below 4 GiB: a game address (where the optimiser folded the
		 * plugin's cast away) or a small integer */
		if ((a >> 32) && PORT_GAME_REBASE((u32)a) != a)
			port_ptr32_trap(p);
		raw = (u32)a;
		return *this;
	}
	/* pointer arithmetic, as on T* (in-place relocation adds a base), on the
	 * 32-bit address: the slot may still hold a file offset, which must not
	 * pick up PORT_GAME_LO before the base is added */
	template <typename I> port_ptr32& operator+=(I n) { return *this = (T*)port_game_ptr(raw + (u32)(n * sizeof(T))); }
	template <typename I> port_ptr32& operator-=(I n) { return *this = (T*)port_game_ptr(raw - (u32)(n * sizeof(T))); }
};
#define PTR32(T) port_ptr32<T>
#else
#define PTR32(T) u32
#endif
#else
#define PTR32(T) T*
#endif

#endif
