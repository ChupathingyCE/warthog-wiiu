/*
WIIU_FLOAT.C

The floating-point unit's mode, the same on every thread as on a PC.

Cafe OS starts a thread with the FPSCR's non-IEEE bit set (NI, bit 29 in
the CPU manual's numbering, 0x4 in the register's value): a result too
small for a normal number is flushed to zero instead of kept as a
denormal. The x87 and SSE units keep denormals, so with NI set the same
simulation step could give a PC and a Wii U different floats, and a game
played together (co-op, multiplayer) drift apart. The bit is cleared on
the thread the system starts the game on (wiiu_float_initialize, from
wiiu_proc.c) and on every thread created after it: the link wraps
OSCreateThread (build.py's --wrap), which writes the new thread's saved
FPSCR before it first runs. That covers the port's own threads, wut's
(pthreads, crash handling) and the game's. The rounding mode is the
game's to set (_control87, the game's port/linux/src/msvc_crt.c), and is
left as it is.
*/

#include <stdint.h>

#include <coreinit/thread.h>

#include "wiiu_port.h"

#define FPSCR_NON_IEEE 0x4u

/* the threads created since the start, and how many of them came with NI
set (the system's default) */
static volatile uint32_t threads_created;
static volatile uint32_t threads_non_ieee;

unsigned int wiiu_float_mode(void)
{
	union { double value; unsigned long long bits; } fpscr;

	__asm__ volatile ("mffs %0" : "=f"(fpscr.value));
	return (unsigned int)fpscr.bits;
}

void wiiu_float_ieee(void)
{
	union { double value; unsigned long long bits; } fpscr;

	/* (read and written whole, as the game's _control87 does) */
	__asm__ volatile ("mffs %0" : "=f"(fpscr.value));
	fpscr.bits &= ~(unsigned long long)FPSCR_NON_IEEE;
	__asm__ volatile ("mtfsf 0xff, %0" : : "f"(fpscr.value));
}

/* the FPSCR the system started the game's thread with */
static unsigned int fpscr_at_start;

/* (a constructor: the thread the system starts the game on, before main
and the game's own initialization; it logs nothing, the log not being up
yet: wiiu_float_report says what it found) */
__attribute__((constructor)) void wiiu_float_initialize(void)
{
	fpscr_at_start = wiiu_float_mode();
	wiiu_float_ieee();
}

void wiiu_float_report(const char *thread)
{
	unsigned int fpscr = wiiu_float_mode();

	wiiu_log("float: FPSCR 0x%08x on %s (%s; 0x%08x at the start); %lu threads created, %lu of them started in "
		"non-IEEE mode and changed", fpscr, thread, (fpscr & FPSCR_NON_IEEE) ? "non-IEEE, denormals flushed" : "IEEE", fpscr_at_start,
		(unsigned long)threads_created,
		(unsigned long)threads_non_ieee);
}

/* the link's names for OSCreateThread (-Wl,--wrap=OSCreateThread): the
system's, and the one every caller gets. Declared here with the argument
types of every wut release (the entry's and argv's types changed in wut
1.9.2): only the thread and its context are read. */
BOOL __real_OSCreateThread(OSThread *thread, void *entry, int32_t argc, void *argv, void *stack, uint32_t stack_size,
	int32_t priority, OSThreadAttributes attributes);
BOOL __wrap_OSCreateThread(OSThread *thread, void *entry, int32_t argc, void *argv, void *stack, uint32_t stack_size,
	int32_t priority, OSThreadAttributes attributes);

BOOL __wrap_OSCreateThread(OSThread *thread, void *entry, int32_t argc, void *argv, void *stack, uint32_t stack_size,
	int32_t priority, OSThreadAttributes attributes)
{
	BOOL created = __real_OSCreateThread(thread, entry, argc, argv, stack, stack_size, priority, attributes);

	/* (a created thread waits for OSResumeThread; its registers are
	loaded from its context when it first runs) */
	if (created)
	{
		threads_created++;
		if (thread->context.fpscr & FPSCR_NON_IEEE)
		{
			threads_non_ieee++;
			thread->context.fpscr &= ~FPSCR_NON_IEEE;
		}
	}
	return created;
}
