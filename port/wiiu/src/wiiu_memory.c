/*
WIIU_MEMORY.C

The Xbox's memory window and the memory mapping the platform layer asks for
(include/posix/sys/mman.h), on the Wii U.

The game's data is linked for the Xbox's window at 0x80000000, which an
application cannot have on the Wii U (Cafe OS and Aroma's own mappings are
there). The window is moved (HALO_WINDOW_REBASE, source/cseries/
xbox_address.h): it is taken from the application's heap (MEM2) before
the C library takes the rest of it, at a multiple of its size as the
platform layer asks (port/linux/src/platform.h), and the platform layer is
told where it is (platform_contiguous_base and platform_contiguous_size)
before any constructor runs. 256 MB if the heap has it, else 128 MB, a
development kit's.

Cafe OS gives an application neither page protection nor anonymous
mappings: a mapping asked for inside the window is that part of it (the
platform layer reserves the whole window at start-up, xbox_memory.c), any
other is heap memory; protection always succeeds and changes nothing.
*/

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/memexpheap.h>
#include <coreinit/memheap.h>
#include <coreinit/memdefaultheap.h>

#include "wiiu_port.h"
#include "sys/mman.h"

/* the platform layer's (port/linux/src/xbox_memory.c) */
extern unsigned long platform_contiguous_base;
extern unsigned long platform_contiguous_size;

/* wut's own start-up, which __preinit_user replaces (wut_preinit.c) */
void __init_wut_sbrk_heap(MEMHeapHandle heap);
void __init_wut_thread(void);
void __init_wut_defaultheap(void);

static void *window;
static unsigned long window_size;
static unsigned long heap_free_before, heap_free_after;
static int window_cleared;

/* Before the C library's start: the window first, then wut's own steps,
which give the C library's heap the rest of MEM2. */
void __preinit_user(MEMHeapHandle *mem1, MEMHeapHandle *foreground, MEMHeapHandle *mem2)
{
	static const unsigned long sizes[] = { 0x10000000UL, 0x08000000UL };
	unsigned int index;

	(void)mem1;
	(void)foreground;
	heap_free_before = MEMGetAllocatableSizeForExpHeapEx(*mem2, 4);
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]) && !window; index++)
	{
		window = MEMAllocFromExpHeapEx(*mem2, sizes[index], (int)sizes[index]);
		if (window)
			window_size = sizes[index];
	}
	if (window)
	{
		platform_contiguous_base = (unsigned long)(uintptr_t)window;
		platform_contiguous_size = window_size;
	}
	heap_free_after = MEMGetAllocatableSizeForExpHeapEx(*mem2, 4);
	__init_wut_sbrk_heap(*mem2);
	__init_wut_thread();
	__init_wut_defaultheap();
}

void wiiu_memory_report(void)
{
	if (window)
	{
		wiiu_log("memory: window of %lu MB at %08lx; MEM2 heap %lu MB free before it, %lu MB after", window_size >> 20,
			(unsigned long)(uintptr_t)window, heap_free_before >> 20, heap_free_after >> 20);
	}
	else
	{
		wiiu_log("memory: no room for the window in MEM2 (%lu MB free)", heap_free_before >> 20);
	}
}

static int inside_window(const void *address, size_t length)
{
	uintptr_t start = (uintptr_t)address;

	return window && start >= (uintptr_t)window && length <= window_size &&
		start - (uintptr_t)window <= window_size - length;
}

void *mmap(void *address, size_t length, int protection, int flags, int descriptor, off_t offset)
{
	void *result;

	(void)protection;
	(void)offset;
	if (descriptor != -1 || !(flags & MAP_ANONYMOUS) || !length)
	{
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (address && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)))
	{
		if (!inside_window(address, length))
		{
			errno = ENOMEM;
			return MAP_FAILED;
		}
		/* (anonymous memory is zeros: the window the first time it is
		reserved whole, a part of it when it is mapped again) */
		if (!window_cleared || address != window || length != window_size)
			memset(address, 0, length);
		window_cleared = 1;
		return address;
	}
	result = memalign(0x1000, (length + 0xfff) & ~(size_t)0xfff);
	if (!result)
	{
		errno = ENOMEM;
		return MAP_FAILED;
	}
	memset(result, 0, length);
	return result;
}

int munmap(void *address, size_t length)
{
	if (!inside_window(address, length))
		free(address);
	return 0;
}

int mprotect(void *address, size_t length, int protection)
{
	(void)address;
	(void)length;
	(void)protection;
	return 0;
}

int madvise(void *address, size_t length, int advice)
{
	(void)address;
	(void)length;
	(void)advice;
	return 0;
}
