/*
WIIU_CRASH.C

A crash's report: where a thread took a CPU exception (a bad address read
or written, an instruction fetched from one, a misaligned access, an
illegal instruction), in port.txt and crash.txt and on the screen, instead
of the system's bare "An error has occurred" screen.

The callbacks are coreinit's, for every core (OSSetExceptionCallbackEx with
OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, wut's coreinit/exception.h). A callback
runs in the exception's context, where nothing may wait or write a file:
it keeps the registers in a static report, then points the context's srr0
at crash_report (and r1 below the thread's own frames, or at a stack of its
own if the thread's looks bad) and returns TRUE, so the thread goes on in
crash_report as an ordinary function, as other Wii U homebrew's exception
handlers do (RetroArch's wiiu/system/exception_handler.c). crash_report
follows the stack's back chain, writes the report, and ends the game with
OSFatal, whose screen shows the address too. A second exception while the
first is being reported is left to the system.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <coreinit/context.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/thread.h>

#include "wiiu_port.h"

#define CRASH_FRAMES 32
#define CRASH_STACK (32 * 1024)

static struct
{
	volatile int taken;
	int type;
	uint32_t srr0, srr1, lr, ctr, dar, dsisr, r1, r3, r4, r5;
	OSThread *thread;
	uint32_t stack_low, stack_high;
	const char *thread_name;
	uint32_t frames[CRASH_FRAMES];
	int frame_count;
} crash;

static uint8_t crash_stack[CRASH_STACK] __attribute__((aligned(16)));
static char crash_text[4096];

static const char *crash_type_name(int type)
{
	switch (type)
	{
	case OS_EXCEPTION_TYPE_DSI: return "DSI (a data address)";
	case OS_EXCEPTION_TYPE_ISI: return "ISI (an instruction address)";
	case OS_EXCEPTION_TYPE_ALIGNMENT: return "alignment";
	case OS_EXCEPTION_TYPE_PROGRAM: return "program (an illegal instruction or a trap)";
	default: return "unknown";
	}
}

static int crash_in_stack(uint32_t address)
{
	return address >= crash.stack_low && address + 8 <= crash.stack_high && !(address & 3);
}

/* (on the crashed thread, as an ordinary function) the report, then the
end */
static void crash_report(void)
{
	FILE *file;
	size_t used = 0;
	uint32_t frame;
	int index;

	/* the back chain from the faulting frame: each frame's first word is
	its caller's stack pointer, and the word after the caller's is the
	return address its callee saved there */
	crash.frame_count = 0;
	frame = crash.r1;
	for (index = 0; index < CRASH_FRAMES && crash_in_stack(frame); index++)
	{
		uint32_t caller = *(uint32_t *)frame;

		if (!crash_in_stack(caller) || caller <= frame)
			break;
		crash.frames[crash.frame_count++] = *(uint32_t *)(caller + 4);
		frame = caller;
	}

	used += snprintf(crash_text + used, sizeof(crash_text) - used,
		"crash: %s exception on thread %p \"%s\"\n"
		"crash: srr0 (pc) %08lx lr %08lx ctr %08lx srr1 %08lx\n"
		"crash: dar %08lx dsisr %08lx r1 %08lx (stack %08lx to %08lx) r3 %08lx r4 %08lx r5 %08lx\n"
		"crash: calls (return addresses, innermost first):",
		crash_type_name(crash.type), (void *)crash.thread, crash.thread_name ? crash.thread_name : "",
		(unsigned long)crash.srr0, (unsigned long)crash.lr, (unsigned long)crash.ctr, (unsigned long)crash.srr1,
		(unsigned long)crash.dar, (unsigned long)crash.dsisr, (unsigned long)crash.r1, (unsigned long)crash.stack_low,
		(unsigned long)crash.stack_high, (unsigned long)crash.r3, (unsigned long)crash.r4, (unsigned long)crash.r5);
	for (index = 0; index < crash.frame_count && used < sizeof(crash_text) - 16; index++)
		used += snprintf(crash_text + used, sizeof(crash_text) - used, " %08lx", (unsigned long)crash.frames[index]);
	if (used < sizeof(crash_text) - 2)
		used += snprintf(crash_text + used, sizeof(crash_text) - used, "\n");

	OSReport("%s", crash_text);
	/* (a file of its own first: the log's lock may be the crashed
	thread's) */
	file = fopen(WIIU_APP_FOLDER "/crash.txt", "w");
	if (file)
	{
		fputs(crash_text, file);
		fclose(file);
	}
	wiiu_log("%s", crash_text);
	wiiu_log_flush();

	snprintf(crash_text, sizeof(crash_text),
		"Chupathingy crashed: %s exception\nat %08lx, called from %08lx (address %08lx)\n"
		"The details are in sd:/wiiu/apps/halo/crash.txt",
		crash_type_name(crash.type), (unsigned long)crash.srr0, (unsigned long)crash.lr, (unsigned long)crash.dar);
	OSFatal(crash_text);
}

/* (in the exception's context: no waits, no files) */
static BOOL crash_callback(int type, OSContext *context)
{
	OSThread *thread;
	uint32_t r1, resume;

	if (__atomic_exchange_n(&crash.taken, 1, __ATOMIC_ACQ_REL))
		return FALSE;
	thread = OSGetCurrentThread();
	crash.type = type;
	crash.srr0 = context->srr0;
	crash.srr1 = context->srr1;
	crash.lr = context->lr;
	crash.ctr = context->ctr;
	crash.dar = context->dar;
	crash.dsisr = context->dsisr;
	crash.r1 = r1 = context->gpr[1];
	crash.r3 = context->gpr[3];
	crash.r4 = context->gpr[4];
	crash.r5 = context->gpr[5];
	crash.thread = thread;
	crash.thread_name = thread ? thread->name : NULL;
	crash.stack_low = thread ? (uint32_t)thread->stackEnd : 0;
	crash.stack_high = thread ? (uint32_t)thread->stackStart : 0;
	/* the report below the thread's own frames when there is room for it
	in its stack, else on a stack of its own (an overrun stack's crash) */
	if (thread && r1 > crash.stack_low + 16 * 1024 + 1024 && r1 <= crash.stack_high)
		resume = (r1 - 1024) & ~15u;
	else
		resume = ((uint32_t)crash_stack + CRASH_STACK - 64) & ~15u;
	*(uint32_t *)resume = 0;
	context->gpr[1] = resume;
	context->srr0 = (uint32_t)crash_report;
	return TRUE;
}

static BOOL crash_dsi(OSContext *context) { return crash_callback(OS_EXCEPTION_TYPE_DSI, context); }
static BOOL crash_isi(OSContext *context) { return crash_callback(OS_EXCEPTION_TYPE_ISI, context); }
static BOOL crash_alignment(OSContext *context) { return crash_callback(OS_EXCEPTION_TYPE_ALIGNMENT, context); }
static BOOL crash_program(OSContext *context) { return crash_callback(OS_EXCEPTION_TYPE_PROGRAM, context); }

/* (before the game's start, after the log's) */
__attribute__((constructor(103))) static void crash_attach(void)
{
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, crash_dsi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, crash_isi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ALIGNMENT, crash_alignment);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, crash_program);
	wiiu_log("crash: exception reports on (DSI, ISI, alignment, program; all cores)");
}
