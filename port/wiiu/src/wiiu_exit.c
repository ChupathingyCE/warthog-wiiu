/*
WIIU_EXIT.C

What happens after the game's end (proc_end, wiiu_proc.c) and where it
stops. The exit path is wut's (libraries/wutcrt/crt0_rpx.s, wut_crt.c,
libraries/wutnewlib/wut_newlib.c): main's return, or exit(), runs the
atexit handlers and destructors, then newlib's _exit calls
__syscall_exit, which runs __fini_wut (sockets, the SD card's device,
the C library's heap) and then coreinit's exit (__wut_exit). Each of those
is hooked here and noted before it runs.

The notes go to the system's log (OSReport) and to a file through FSA
itself (exit.txt beside port.txt), not the C library: once wut's
devoptab has been taken down, fopen has nothing to write with, and the
heap is gone after the last step. A thread of its own writes a beat to
exit_beat.txt every quarter second, so a hang shows what the thread
that kept running last saw. It uses no C library call. The threads alive
are listed (read only: nothing is suspended or ended).
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/debug.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "wiiu_port.h"

#define EXIT_FOLDER "/vol/external01/wiiu/apps/halo/"
#define BEAT_STACK 0x4000

struct exit_file
{
	FSAClientHandle client;
	FSAFileHandle file;
	int open;
};

static struct exit_file notes;
static struct exit_file beats;
static OSThread beat_thread;
static uint8_t beat_stack[BEAT_STACK] __attribute__((aligned(16)));
static volatile const char *volatile phase = "before the end";
static volatile int beat_stop;
static void (*volatile coreinit_exit)(int);

/* wut's own (wut_newlib.c, wut_crt.c) */
extern void (*__wut_exit)(int);
void __fini_wut_newlib(void);
void __fini_wut_devoptab(void);
void __fini_wut_socket(void) __attribute__((weak));

static void file_open(struct exit_file *target, const char *path)
{
	FSAInit();
	target->client = FSAAddClient(NULL);
	if (target->client < 0)
		return;
	if (FSAOpenFileEx(target->client, path, "w", 0x660, FS_OPEN_FLAG_NONE, 0, &target->file) == FS_ERROR_OK)
		target->open = 1;
}

/* (no C library: the line is put together by hand, safe on any thread
and after the heap has gone) */
static size_t text_put(char *out, size_t at, size_t size, const char *text)
{
	while (*text && at < size - 1)
		out[at++] = *text++;
	return at;
}

static size_t number_put(char *out, size_t at, size_t size, uint32_t value, int hex)
{
	char digits[12];
	int count = 0;
	uint32_t base = hex ? 16 : 10;

	do
	{
		uint32_t digit = value % base;

		digits[count++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
		value /= base;
	} while (value && count < 12);
	if (hex)
		at = text_put(out, at, size, "0x");
	while (count && at < size - 1)
		out[at++] = digits[--count];
	return at;
}

static void file_line(struct exit_file *target, const char *line, size_t length)
{
	if (!target->open)
		return;
	FSAWriteFile(target->client, (void *)line, 1, (uint32_t)length, target->file, 0);
	FSAFlushFile(target->client, target->file);
}

/* a line out, noted before the step it announces. text, then (when
with_value) a number */
static void note(const char *text, int with_value, uint32_t value, int hex)
{
	char line[160];
	size_t at = text_put(line, 0, sizeof(line) - 2, "exit: ");

	at = text_put(line, at, sizeof(line) - 2, text);
	if (with_value)
	{
		at = text_put(line, at, sizeof(line) - 2, " ");
		at = number_put(line, at, sizeof(line) - 2, value, hex);
	}
	line[at++] = '\n';
	line[at] = 0;
	OSReport("%s", line);
	file_line(&notes, line, at);
}

static int beat_main(int argc, const char **argv)
{
	uint32_t count = 0;

	(void)argc;
	(void)argv;
	while (!beat_stop)
	{
		char line[120];
		size_t at = text_put(line, 0, sizeof(line) - 2, "beat ");

		at = number_put(line, at, sizeof(line) - 2, count++, 0);
		at = text_put(line, at, sizeof(line) - 2, " in: ");
		at = text_put(line, at, sizeof(line) - 2, (const char *)phase);
		line[at++] = '\n';
		line[at] = 0;
		OSReport("exit: %s", line);
		file_line(&beats, line, at);
		OSSleepTicks(OSMillisecondsToTicks(250));
	}
	return 0;
}

/* the threads alive, read only (OSGetActiveThreadLink walks the active
queue, as OSGetThreadName and the entry point read from it) */
static void threads_list(const char *when)
{
	OSThread *thread = OSGetCurrentThread();
	OSThreadLink link;
	int guard = 0;
	int count = 0;

	/* (back to the first, then forward) */
	for (;;)
	{
		OSGetActiveThreadLink(thread, &link);
		if (!link.prev || guard++ > 64)
			break;
		thread = link.prev;
	}
	note(when, 0, 0, 0);
	for (guard = 0; thread && guard < 64; guard++)
	{
		const char *name = OSGetThreadName(thread);

		note(name && *name ? name : "(unnamed)", 1, (uint32_t)(uintptr_t)thread->entryPoint, 1);
		note("  state", 1, thread->state, 1);
		note("  priority", 1, (uint32_t)OSGetThreadPriority(thread), 0);
		note("  affinity", 1, OSGetThreadAffinity(thread), 1);
		note(thread == OSGetCurrentThread() ? "  (this thread)" : "  (another)", 0, 0, 0);
		count++;
		OSGetActiveThreadLink(thread, &link);
		thread = link.next;
	}
	note("  threads alive", 1, (uint32_t)count, 0);
}

void wiiu_exit_phase(const char *text)
{
	phase = text;
	note(text, 0, 0, 0);
}

static void coreinit_exit_traced(int code)
{
	wiiu_exit_phase("coreinit's exit next");
	if (coreinit_exit)
		coreinit_exit(code);
	wiiu_exit_phase("coreinit's exit returned");
	for (;;)
		OSSleepTicks(OSMillisecondsToTicks(1000));
}

/* the first of the end: the files, the beat, the threads listed, wut's
exit hooked */
void wiiu_exit_begin(void)
{
	static int begun;

	if (begun)
		return;
	begun = 1;
	file_open(&notes, EXIT_FOLDER "exit.txt");
	file_open(&beats, EXIT_FOLDER "exit_beat.txt");
	note("begun", 0, 0, 0);
	threads_list("threads at the game's end:");
	/* (coreinit's exit, which wut's __syscall_exit calls last) */
	if (__wut_exit != coreinit_exit_traced)
	{
		coreinit_exit = __wut_exit;
		__wut_exit = coreinit_exit_traced;
	}
	if (OSCreateThread(&beat_thread, beat_main, 0, NULL, beat_stack + BEAT_STACK, BEAT_STACK, 17,
		OS_THREAD_ATTRIB_AFFINITY_ANY))
	{
		OSSetThreadName(&beat_thread, "exit beat");
		OSResumeThread(&beat_thread);
	}
}

void wiiu_exit_threads(const char *when)
{
	threads_list(when);
}

/* wut_crt.c's __fini_wut (weak there), step for step and in its order,
each noted before it runs */
void __fini_wut(void)
{
	wiiu_exit_phase("__fini_wut: sockets next");
	if (&__fini_wut_socket)
		__fini_wut_socket();
	wiiu_exit_phase("__fini_wut: devoptab next");
	__fini_wut_devoptab();
	wiiu_exit_phase("__fini_wut: newlib next");
	__fini_wut_newlib();
	wiiu_exit_phase("__fini_wut: done");
}

/* the last of what exit() runs: the last destructor, and the last atexit
handler (the first registered runs last; proc_end is registered at 102) */
static void exit_last_atexit(void)
{
	wiiu_exit_phase("the last atexit handler");
}

__attribute__((destructor(101))) static void exit_last_destructor(void)
{
	wiiu_exit_phase("the last destructor");
}

__attribute__((constructor(101))) static void exit_attach(void)
{
	atexit(exit_last_atexit);
}
