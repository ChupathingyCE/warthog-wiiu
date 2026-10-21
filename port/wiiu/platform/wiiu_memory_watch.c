/*
WIIU_MEMORY_WATCH.C

The renderer's write tracking for guest memory (port/linux/src/platform.h),
in place of memory_watch.c, on a console that cannot write-protect memory
(HALO_CONSOLE_NO_WRITE_WATCH). A watched page is never made read-only: the
writes the platform layer knows of (resource locks, file reads into the
window) say so themselves (memory_watch_prepare_write), and those make the
pages' cached copies stale, as a fault would have on the desktop.
*/

#include "platform.h"

#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)
#define WATCH_PAGE_COUNT_MAXIMUM (PLATFORM_CONTIGUOUS_MAXIMUM / WATCH_PAGE_SIZE)

/* each page: 0 not watched, 1 watched, 2 written since it was watched */
static unsigned char page_watched[WATCH_PAGE_COUNT_MAXIMUM];
static unsigned long page_generation[WATCH_PAGE_COUNT_MAXIMUM];
static volatile unsigned long current_generation = 1;

/* the pages [first, last] of a range in the window; FALSE if it has none */
static BOOL page_range(unsigned long address, unsigned long size, unsigned long *first, unsigned long *last)
{
	unsigned long start = address, end = address + size;
	unsigned long window_end = PLATFORM_CONTIGUOUS_BASE + PLATFORM_CONTIGUOUS_SIZE;

	if (!size || end <= PLATFORM_CONTIGUOUS_BASE || start >= window_end)
		return FALSE;
	if (start < PLATFORM_CONTIGUOUS_BASE)
		start = PLATFORM_CONTIGUOUS_BASE;
	if (end > window_end)
		end = window_end;
	*first = (start - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
	*last = (end - 1 - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
	return TRUE;
}

void memory_watch_initialize(void)
{
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	unsigned long first, last, page;

	if (!page_range(address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
		page_watched[page] = 1;
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, newest = 0;

	if (!page_range(address, size, &first, &last))
		return 0;
	for (page = first; page <= last; page++)
	{
		if (page_generation[page] > newest)
			newest = page_generation[page];
	}
	return newest;
}

unsigned long memory_watch_serial(void)
{
	return current_generation;
}

void memory_watch_prepare_write(void *address, unsigned long size)
{
	unsigned long first, last, page;

	if (!page_range((unsigned long)address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		if (page_watched[page] == 1)
		{
			page_watched[page] = 2;
			page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
		}
	}
}

void memory_watch_forget(void *address, unsigned long size)
{
	unsigned long first, last, page;

	if (!page_range((unsigned long)address, size, &first, &last))
		return;
	for (page = first; page <= last; page++)
	{
		page_watched[page] = 0;
		page_generation[page] = __sync_add_and_fetch(&current_generation, 1);
	}
}
