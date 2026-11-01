// tools/gameptr/test.sh builds this with the pass plugin on arm64 macOS and
// runs it: each check prints "ok" or "FAIL", and the exit status counts the
// failures. The game window is mapped where the port maps it (PORT_GAME_LO,
// PORT_GAME_HI; docs/MACOS-ARM64.md).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>

typedef unsigned int u32;

static int failures;
static void check(const char* what, bool ok) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

template <typename T> struct Ptr32 {  // as the port's PTR32 with the plugin
    u32 raw;
    operator T*() const { return (T*)(unsigned long long)raw; }
};
struct Info {
    Ptr32<char> layers;
};

static int* const kLowGlobal = (int*)0x80000028;  // a constant address (low-memory OS globals)

__attribute__((noinline)) int* fromU32(u32 x) { return (int*)x; }
__attribute__((noinline)) char* fromSum(u32 a, u32 b) { return (char*)((uintptr_t)a + b); }
__attribute__((noinline)) uintptr_t message(void* m) { return (uintptr_t)m; }
__attribute__((noinline)) char* relocate(Info* info) { return (char*)info->layers + (u32)(uintptr_t)info; }
__attribute__((noinline)) int indexFrom(u32 base, int i) { return ((int*)base)[i]; }
__attribute__((noinline)) int* hostIndex(uintptr_t p, int i) { return &((int*)p)[i]; }
__attribute__((noinline)) unsigned char toU8(float f) { return (unsigned char)f; }
__attribute__((noinline)) unsigned short toU16(float f) { return (unsigned short)f; }
__attribute__((noinline)) unsigned toU32(float f) { return (unsigned)f; }
__attribute__((noinline)) int toS32(float f) { return (int)f; }
__attribute__((noinline)) short toS16(float f) { return (short)f; }

static int g_static = 7;

int main() {
    char* mem1 = (char*)mmap((void*)0x10080000000ull, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    char* low = (char*)mmap((void*)0x110000000ull, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (mem1 == MAP_FAILED || low == MAP_FAILED) {
        std::printf("FAIL cannot map the game window\n");
        return 1;
    }
    *(int*)(mem1 + 0x28) = 42;
    *(int*)(low + 0x40) = 5;

    check("a constant MEM1 address is rebased", kLowGlobal == (int*)(mem1 + 0x28) && *kLowGlobal == 42);
    check("a MEM1 address through a u32 comes back", fromU32(0x80000028u) == (int*)(mem1 + 0x28));
    check("a low-half address through a u32 comes back", fromU32(0x10000040u) == (int*)(low + 0x40) && *fromU32(0x10000040u) == 5);
    check("static data through a u32 comes back", fromU32((u32)(uintptr_t)&g_static) == &g_static);
    check("null stays null", fromU32(0) == nullptr);
    check("small integers stay as they are (OSMessage codes)", message((void*)(uintptr_t)fromU32(3)) == 3);
    check("a pointer made from a 32-bit sum is rebased", fromSum(0x80000000u, 0x28) == mem1 + 0x28);
    Info* info = (Info*)(mem1 + 0x100);
    info->layers.raw = 0x20000;  // a file offset above 64 KiB, relocated by the file's address
    check("an offset plus an address relocates in 32 bits", relocate(info) == mem1 + 0x100 + 0x20000);
    ((int*)(mem1 + 0x200))[3] = 77;
    check("indexing from a 32-bit address", indexFrom(0x80000200u, 3) == 77);
    // A host pointer the code turns into an integer and back (a small struct
    // returned in registers, such as unordered_map's pair<iterator, bool>):
    // the malloc heap is far above the game window.
    check("indexing from a host heap address keeps it", hostIndex(0x600001234560ull, 2) == (int*)0x600001234568ull);
    check("indexing from a low-window address stays there", hostIndex(0x110000040ull, 1) == (int*)(low + 0x44));

    volatile float neg = -41.92f, negBig = -3149.98f, big = 3e9f, nan = NAN, wide = 48788.4f;
    check("negative float to u8 wraps as on x86 (0xD7)", toU8(neg) == 0xD7);
    check("negative float to u16 wraps as on x86", toU16(negBig) == 62387);
    check("-1.0 to u32 wraps as on x86", toU32(-1.0f) == 0xFFFFFFFFu);
    check("3e9 to u32 is exact", toU32(big) == 3000000000u);
    check("out of int range gives 0x80000000 as on x86", toS32(big) == int(0x80000000u));
    check("NaN to int gives 0x80000000 as on x86", toS32(nan) == int(0x80000000u));
    check("float to short truncates as on x86", toS16(wide) == -16748);
    check("in-range conversions are unchanged", toS32(-2.5f) == -2 && toU8(200.7f) == 200);

    std::printf("%d failed\n", failures);
    return failures;
}
