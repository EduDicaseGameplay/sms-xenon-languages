/* No dynamic loading on the 360: every dlopen fails, so the port's optional
 * libraries (SDL audio output, Vulkan) are simply absent, as when not installed. */
#ifndef XE_DLFCN_H
#define XE_DLFCN_H
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void*)0)
static __inline void* dlopen(const char* name, int mode) { (void)name; (void)mode; return 0; }
static __inline void* dlsym(void* h, const char* name) { (void)h; (void)name; return 0; }
static __inline int dlclose(void* h) { (void)h; return 0; }
static __inline const char* dlerror(void) { return "no dynamic loading on the Xbox 360"; }
#endif
