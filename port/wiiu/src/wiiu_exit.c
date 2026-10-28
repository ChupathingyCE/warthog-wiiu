/*
WIIU_EXIT.C

What happens after the game's end (proc_end, wiiu_proc.c) and where it
stops. The exit path is wut's (libraries/wutcrt/crt0_rpx.s, wut_crt.c,
libraries/wutnewlib/wut_newlib.c): main's return, or exit(), runs the
atexit handlers and destructors, then newlib's _exit calls
__syscall_exit, which runs __fini_wut (sockets, the SD card's device,
the C library's heap) and then coreinit's exit (__wut_exit). Each of those
is hooked here and noted before it runs (port/wiiu/docs/exit.md).

The notes go to port.txt through the port's log while wut's devoptab is
there, and through FSA itself once it is taken down (fopen has nothing to
write with then, and the heap is gone after the last step), and to the
system's log (OSReport). A thread of its own writes a beat to
exit_beat.txt every quarter second, so a hang shows what the thread that
kept running last saw. FSA is given what wut's own writes give it
(devoptab_fsa_write.cpp): a cache-aligned buffer, a cache line at most at
a time; the first notes, written from the stack, were lost. Neither path
uses the C library once the devoptab is down. The threads alive are listed
(read only: nothing is suspended or ended).
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
/* FSA's buffers: cache-aligned, a cache line long (devoptab_fsa_write.cpp) */
#define FSA_LINE 0x40

struct exit_file
{
	FSAClientHandle client;
	FSAFileHandle file;
	int added;
	int open;
	uint8_t buffer[FSA_LINE] __attribute__((aligned(FSA_LINE)));
};

static struct exit_file notes __attribute__((aligned(FSA_LINE)));
static struct exit_file beats __attribute__((aligned(FSA_LINE)));
static OSThread beat_thread;
static int beat_started;
static uint8_t beat_stack[BEAT_STACK] __attribute__((aligned(16)));
static volatile const char *volatile phase = "before the end";
static volatile int beat_stop;
/* wut's devoptab is gone: the notes go to FSA, not the port's log */
static volatile int devoptab_down;
static void (*volatile coreinit_exit)(int);

/* wut's own (wut_newlib.c, wut_crt.c) */
extern void (*__wut_exit)(int);
void __fini_wut_newlib(void);
void __fini_wut_devoptab(void);
void __fini_wut_socket(void) __attribute__((weak));

static void file_open(struct exit_file *target, const char *path, const char *mode)
{
	FSAInit();
	target->client = FSAAddClient(NULL);
	if (target->client <= 0)
		return;
	target->added = 1;
	if (FSAOpenFileEx(target->client, path, mode, 0x660, FS_OPEN_FLAG_NONE, 0, &target->file) == FS_ERROR_OK)
		target->open = 1;
}

/* the file closed and its client deleted, as wut's own FSA device is let
go (devoptab_fsa.cpp's __fini_wut_devoptab: FSADelClient) */
static void file_close(struct exit_file *target)
{
	if (target->open)
		FSACloseFile(target->client, target->file);
	target->open = 0;
	if (target->added)
		FSADelClient(target->client);
	target->added = 0;
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

/* (one writer for each file: the notes' is the thread at the end, the
beats' the beat thread) */
static void file_line(struct exit_file *target, const char *line, size_t length)
{
	size_t at = 0;

	if (!target->open)
		return;
	while (at < length)
	{
		size_t part = length - at < FSA_LINE ? length - at : FSA_LINE;
		size_t index;

		for (index = 0; index < part; index++)
			target->buffer[index] = (uint8_t)line[at + index];
		if ((int32_t)FSAWriteFile(target->client, target->buffer, 1, (uint32_t)part, target->file, 0) != (int32_t)part)
			return;
		at += part;
	}
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
	line[at] = 0;
	if (!devoptab_down)
	{
		/* (the port's log: OSReport, port.txt) */
		wiiu_log("%s", line);
		return;
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

/* The trace's own thread and files let go before coreinit's exit: none of
the references (SDL's Wii U port, libwhb, Butter and Jelly) reach the
system's exit with a thread of their own still running or an FSA client
still open, and the beat thread was left writing to the SD card through
it. Stopped as Butter and Jelly stops its threads: asked, then joined
(docs/exit.md), here with a second's limit. */
static void trace_stop(void)
{
	int waited = 0;

	if (beat_started)
	{
		beat_stop = 1;
		while (!OSIsThreadTerminated(&beat_thread) && waited < 1000)
		{
			OSSleepTicks(OSMillisecondsToTicks(10));
			waited += 10;
		}
		if (OSIsThreadTerminated(&beat_thread))
		{
			OSJoinThread(&beat_thread, NULL);
			beat_started = 0;
			note("the beat thread stopped", 0, 0, 0);
		}
		else
			note("the beat thread still running, ms waited:", 1, (uint32_t)waited, 0);
	}
	note("the trace's files closed next", 0, 0, 0);
	/* (the beats' file only once its writer has stopped) */
	if (!beat_started)
		file_close(&beats);
	file_close(&notes);
}

static void coreinit_exit_traced(int code)
{
	wiiu_exit_phase("coreinit's exit next");
	trace_stop();
	/* (the files closed: OSReport only from here) */
	if (coreinit_exit)
		coreinit_exit(code);
	OSReport("exit: coreinit's exit returned\n");
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
	file_open(&beats, EXIT_FOLDER "exit_beat.txt", "w");
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
		beat_started = 1;
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
	/* (port.txt, taken on through FSA from here, at its end) */
	wiiu_log_flush();
	file_open(&notes, EXIT_FOLDER "port.txt", "a");
	devoptab_down = 1;
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
