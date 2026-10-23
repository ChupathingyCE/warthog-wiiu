/*
HALO_WIIU_PREFIX.H

Force-included ahead of every game and platform unit of the Wii U build
(gcc -include), as halo_linux_prefix.h is in the Linux build. It gives
devkitPPC's GCC and newlib the MSVC and Xbox SDK environment the game's
sources assume, so the sources compile as the desktop builds compile them.

Target: the Wii U's Espresso (PowerPC 750 family), 32-bit, big-endian, with
devkitPPC's GCC and newlib and wut. Pointers are 32 bits, as the Xbox's; the
Xbox's memory window is wherever the port puts it (HALO_WINDOW_REBASE,
source/cseries/xbox_address.h; port/wiiu/src/wiiu_memory.c).
*/

#ifndef __HALO_WIIU_PREFIX_H
#define __HALO_WIIU_PREFIX_H

#if !defined(__PPC__) || defined(__LP64__)
#error the Wii U build targets 32-bit PowerPC
#endif

#ifndef __WIIU__
#define __WIIU__ 1
#endif
#ifndef __WUT__
#define __WUT__ 1
#endif
#ifndef HALO_WIIU_CONSOLE
#define HALO_WIIU_CONSOLE 1
#endif
#ifndef HALO_BIG_ENDIAN
#define HALO_BIG_ENDIAN 1
#endif

/* ---------- the C library, read first

newlib's headers use __inline for helpers of their own, which the MSVC
inline semantics below redefine; they are read here first, and their include
guards keep later includes from seeing the redefinition. wchar_t is 16 bits
(-fshort-wchar) and wint_t with it, typed before newlib types its own.
<limits.h> is left out: cseries.h defines LONG_MAX and friends as
enumerators. */

#ifndef __cplusplus
#define _WINT_T 1
#define __wint_t_defined 1
typedef unsigned short wint_t;
#endif
#include <stdint.h>
#include <stddef.h>
/* the Xbox SDK's Winsock declares u_long itself */
#define u_long halo_host_u_long
#include <sys/types.h>
#undef u_long
#include <stdarg.h>
#include <float.h>
#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <assert.h>
#ifdef HALO_LINUX_PLATFORM_LAYER
/* what the platform layer uses of POSIX that newlib leaves out */
#include "posix/wiiu_posix.h"
#endif
/* the classification functions as functions: the game declares them itself
(hs.c), which newlib's macros would expand */
#undef isalnum
#undef isalpha
#undef iscntrl
#undef isdigit
#undef isgraph
#undef islower
#undef isprint
#undef ispunct
#undef isspace
#undef isupper
#undef isxdigit
#undef isblank
#undef isascii
#undef toascii
#undef tolower
#undef toupper

/* ---------- pointers inside Xbox data: the window is wherever the port
puts it */

#ifndef HALO_WINDOW_REBASE
#define HALO_WINDOW_REBASE 1
#endif
#include "source/cseries/xbox_address.h"

/* ---------- XDK architecture selection

The Xbox SDK declarations (port/include/xdk) are written for the Xbox's x86;
they need its architecture's names, which select only their declarations
here. _M_IX86 stays undefined: the game's x86 assembly is behind it. */

#define _X86_ 1
#define _STDCALL_SUPPORTED 1
#define _INTEGRAL_MAX_BITS 64
#define _WCHAR_T_DEFINED
#define _USE_MATH_DEFINES
/* the XDK's COM headers decorate methods with __export when _WIN32 is unset */
#define __export

/* ---------- MSVC keywords GCC does not have on this target */

/* (newlib's own spellings of these, if any, give way) */
#undef __stdcall
#undef __cdecl
#undef __fastcall
#define __stdcall
#define __cdecl
#define __fastcall
#define __int64 long long
#define __int32 int
#define __int16 short
#define __int8 char
#define __w64
#define __unaligned
#define __ptr32
#define __declspec(specifier) __halo_declspec_##specifier
#define __halo_declspec_align(bytes) __attribute__((aligned(bytes)))
#define __halo_declspec_selectany __attribute__((weak))
#define __halo_declspec_noreturn __attribute__((noreturn))
#define __halo_declspec_noinline __attribute__((noinline))
#define __halo_declspec_naked __attribute__((naked))
#define __halo_declspec_dllimport
#define __halo_declspec_dllexport
#define __halo_declspec_novtable
#define __halo_declspec_thread __thread

/* ---------- MSVC inline semantics

MSVC gives C `__inline` functions COMDAT (pick-any) linkage. The desktop
builds make them static (clang only warns about the `static static` of the
sources' own `static __inline`), which GCC rejects; here they are GNU inline
functions instead, whose every unit emits its definition, and the link keeps
the first of each (-Wl,--allow-multiple-definition: port/wiiu/build.py), as
MSVC keeps any one of a COMDAT's copies. */

#define __inline __inline__ __attribute__((gnu_inline))
#define _inline __inline__ __attribute__((gnu_inline))
#define __forceinline __inline__ __attribute__((gnu_inline, always_inline))

/* ---------- __declspec(selectany) data (XDK D3DCONST tables) */

#define DECLSPEC_SELECTANY __attribute__((weak))

/* ---------- MSVC intrinsics: the platform layer's (as on Linux) */

#define _InterlockedCompareExchange halo_linux_InterlockedCompareExchange
#define _InterlockedDecrement halo_linux_InterlockedDecrement
#define _InterlockedExchange halo_linux_InterlockedExchange
#define _InterlockedExchangeAdd halo_linux_InterlockedExchangeAdd
#define _InterlockedIncrement halo_linux_InterlockedIncrement

/* ---------- structured exception handling: the guarded block always runs */

#define __try if (1)
#define __except(filter) else if (0)
#define __finally
#define __leave

/* ---------- multiplayer session limits */

#include "halo_port_limits.h"

/* the Xbox Winsock headers' fd_set in game units */
#ifndef HALO_LINUX_PLATFORM_LAYER
#undef FD_SETSIZE
#define FD_SETSIZE HALO_PORT_FD_SETSIZE
#endif

/* ---------- Winsock under private names, and the port's functions the
game's sources call */

#ifndef HALO_LINUX_PLATFORM_LAYER
#include "halo_linux_winsock_names.h"
#include "halo_linux_source_fixups.h"
#endif

#endif /* __HALO_WIIU_PREFIX_H */
