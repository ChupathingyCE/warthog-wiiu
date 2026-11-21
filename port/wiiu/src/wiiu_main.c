/*
WIIU_MAIN.C

The game's own thread. An application's main thread (the RPX's default
thread) has the 64 KB of stack elf2rpl gives every RPX, and nothing here
can ask for more. The game needs far more: some of the campaign's AI keeps
80 to 160 KB of arrays on the stack in one function, as the retail game did
on the Xbox's 512 KB, and a campaign on the console froze once its frames
ran past the end of the 64 KB (#34). Multiplayer has no AI, which is why it
played.

So the game runs on a thread of its own with GAME_STACK of stack (the Xbox
360's main thread has the same, port/xbox360's XEX_STACK), on the main
thread's core and at its priority. The main thread stays the one that
started ProcUI and takes its messages, as wut's whb (libwhb/src/proc.c:
ProcUIProcessMessages on the main core) and SDL's Wii U port
(src/video/wiiu/SDL_wiiuvideo.c: WIIU_PumpEvents) have it, but only when
the game's thread asks (wiiu_main_call): at the same points of the game's
frame as before (wiiu_proc.c), with the game's thread waiting, so the
foreground is never let go in the middle of a frame's drawing.

The game's end comes the same way: exit() on the game's thread is run on
the main thread (the atexit handlers, proc_end with ProcUI's last
messages, then wut's own end), as it was when the game ran there.

The linker's --wrap=main and --wrap=exit (build.py) put __wrap_main and
__wrap_exit in the place of main and exit: wut's start (crt0_rpx.s) calls
__wrap_main, which starts the game's thread on __real_main, the game's.

The game's stack is filled with a pattern before the thread starts, and
the watch thread (wiiu_gl.c) logs how deep the game has been
(wiiu_main_stack_watch): at the first check, each time it goes 16 KB
deeper, and every 5 minutes.
*/

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <coreinit/core.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "wiiu_port.h"

#define GAME_STACK (8 * 1024 * 1024)
/* the words of the stack the game has not reached */
#define STACK_PATTERN 0xFEFEFEFEu
#define STACK_CHECK_SECONDS 10
#define STACK_LOG_GROWTH (16 * 1024)
#define STACK_LOG_SECONDS 300

int __real_main(int argc, char **argv);
void __real_exit(int status) __attribute__((noreturn));

static OSThread game_thread __attribute__((aligned(16)));
static uint32_t *game_stack;
static volatile int game_started;
static int game_argc;
static char **game_argv;
static OSMessageQueue requests, replies;
static OSMessage request_messages[1], reply_messages[1];
static unsigned long stack_logged;
static OSTime stack_checked, stack_logged_at;
/* the thread the game runs on, and the part of its stack that holds the
pattern (wiiu_main_stack_watch): from the bottom, past the system's mark,
to the top */
static OSThread *watched_thread;
static uint32_t *stack_low, *stack_high;
static uint32_t stack_mark;
static int stack_mark_logged;
/* the game's thread's breadcrumbs (wiiu_main_crumb): written by it alone,
only read by the watch thread, which never stops it */
static const char *volatile crumb_step;
static volatile uint32_t crumb_r1, crumb_deepest_r1;
static volatile OSTime crumb_time;
static const char *volatile crumb_deepest_step;

/* function(argument) on the main thread, for the game's thread (which
waits for it); on any other thread, there and then */
int wiiu_main_call(int (*function)(int), int argument)
{
	OSMessage message;

	if (!game_started || OSGetCurrentThread() != &game_thread)
		return function(argument);
	memset(&message, 0, sizeof(message));
	message.message = (void *)function;
	message.args[0] = (uint32_t)argument;
	OSSendMessage(&requests, &message, OS_MESSAGE_FLAGS_BLOCKING);
	OSReceiveMessage(&replies, &message, OS_MESSAGE_FLAGS_BLOCKING);
	return (int)message.args[0];
}

/* the game's own thread, or NULL while the game runs on the main thread */
OSThread *wiiu_main_game_thread(void)
{
	return game_started ? &game_thread : NULL;
}

/* the thread the game runs on, its own or the main thread (the watch
thread's) */
OSThread *wiiu_main_watched_thread(void)
{
	return watched_thread;
}

static int exit_on_main_thread(int status)
{
	__real_exit(status);
}

void __wrap_exit(int status)
{
	if (game_started && OSGetCurrentThread() == &game_thread)
	{
		wiiu_main_call(exit_on_main_thread, status);
		/* (never back: the process ends on the main thread) */
		for (;;)
			OSSuspendThread(&game_thread);
	}
	__real_exit(status);
}

static int game_main(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	exit(__real_main(game_argc, game_argv));
	return 0;
}

/* (the game's thread's, at the places it passes often: each frame, each
GL step, the files' reads, the waits) where it is, and its own stack
pointer, for the watch thread to read; nothing on other threads */
void wiiu_main_crumb(const char *step)
{
	uint32_t r1;

	if (!watched_thread || OSGetCurrentThread() != watched_thread)
		return;
	__asm__ volatile("mr %0, 1" : "=r"(r1));
	crumb_step = step;
	crumb_r1 = r1;
	crumb_time = OSGetSystemTime();
	if (!crumb_deepest_r1 || r1 < crumb_deepest_r1)
	{
		crumb_deepest_r1 = r1;
		crumb_deepest_step = step;
	}
}

/* read(), with a breadcrumb first (--wrap=read): the reads the game's
files make that pread's poll does not see */
ssize_t __real_read(int descriptor, void *buffer, size_t count);
ssize_t __wrap_read(int descriptor, void *buffer, size_t count)
{
	wiiu_main_crumb("a file read");
	return __real_read(descriptor, buffer, count);
}

/* (the watch thread's) the last breadcrumb, and the deepest; the depths in
bytes from the top of the game's stack (more than its size: below its
end) */
int wiiu_main_crumb_read(const char **step, unsigned long *age_ms, unsigned long *depth, unsigned long *deepest,
	const char **deepest_step, unsigned long *stack_size)
{
	uint32_t top, r1, low;

	if (!watched_thread || !crumb_step)
		return 0;
	top = (uint32_t)watched_thread->stackStart;
	r1 = crumb_r1;
	low = crumb_deepest_r1;
	*step = crumb_step;
	*age_ms = (unsigned long)OSTicksToMilliseconds(OSGetSystemTime() - crumb_time);
	*depth = top > r1 ? top - r1 : 0;
	*deepest = top > low ? top - low : 0;
	*deepest_step = crumb_deepest_step;
	*stack_size = (unsigned long)((uint8_t *)watched_thread->stackStart - (uint8_t *)watched_thread->stackEnd);
	return 1;
}

/* (the watch thread's, every half second) how deep the game's thread has
been: the lowest word of its stack that is not the pattern any more */
void wiiu_main_stack_watch(void)
{
	OSTime now = OSGetSystemTime();
	const uint32_t *word, *end;
	unsigned long used;

	unsigned long size;

	if (!watched_thread || !stack_low || (stack_checked && now - stack_checked < OSSecondsToTicks(STACK_CHECK_SECONDS)))
		return;
	stack_checked = now;
	/* (the system's mark at the bottom of the stack, as it was at the
	start: a stack run past its end writes over it) */
	if (!stack_mark_logged && *(volatile uint32_t *)watched_thread->stackEnd != stack_mark)
	{
		wiiu_log("stack: the game's thread ran past the end of its stack: the mark at %p is %08lx, was %08lx",
			watched_thread->stackEnd, (unsigned long)*(volatile uint32_t *)watched_thread->stackEnd,
			(unsigned long)stack_mark);
		stack_mark_logged = 1;
	}
	word = stack_low;
	end = (const uint32_t *)watched_thread->stackStart;
	size = (unsigned long)((const uint8_t *)end - (const uint8_t *)watched_thread->stackEnd);
	while (word < stack_high && *word == STACK_PATTERN)
		word++;
	used = (unsigned long)((const uint8_t *)end - (const uint8_t *)word);
	if (stack_logged_at && used < stack_logged + STACK_LOG_GROWTH &&
		now - stack_logged_at < OSSecondsToTicks(STACK_LOG_SECONDS))
		return;
	wiiu_log("stack: the game's thread has used %lu KB of its %lu KB at most%s", used / 1024, size / 1024,
		word == stack_low ? " (all of it)" : "");
	stack_logged = used;
	stack_logged_at = now;
}

int __wrap_main(int argc, char **argv)
{
	OSThread *main_thread = OSGetCurrentThread();
	uint32_t index;

	wiiu_log("main: the main thread's stack: %p to %p (%lu KB)", main_thread->stackEnd, main_thread->stackStart,
		(unsigned long)((uint8_t *)main_thread->stackStart - (uint8_t *)main_thread->stackEnd) / 1024);
	game_argc = argc;
	game_argv = argv;
#ifdef WIIU_GAME_ON_MAIN_THREAD
	/* (a test build's: the game on the main thread, as before, its stack
	watched the same way: the pattern from just below here to the bottom) */
	{
		uint32_t *here = (uint32_t *)__builtin_frame_address(0) - 64;

		watched_thread = main_thread;
		stack_mark = *(uint32_t *)main_thread->stackEnd;
		stack_low = (uint32_t *)main_thread->stackEnd + 16;
		stack_high = here;
		for (uint32_t *word = stack_low; word < here; word++)
			*word = STACK_PATTERN;
		wiiu_log("main: the game runs on the main thread (a test build); the stack's mark %08lx",
			(unsigned long)stack_mark);
		return __real_main(argc, argv);
	}
#endif
	game_stack = memalign(16, GAME_STACK);
	if (!game_stack)
	{
		wiiu_log("main: no %d MB for the game's stack: the game runs on the main thread", GAME_STACK / (1024 * 1024));
		return __real_main(argc, argv);
	}
	for (index = 0; index < GAME_STACK / sizeof(uint32_t); index++)
		game_stack[index] = STACK_PATTERN;
	OSInitMessageQueue(&requests, request_messages, 1);
	OSInitMessageQueue(&replies, reply_messages, 1);
	/* (on this core, which GX2 and AX are started on, at this thread's
	priority) */
	if (!OSCreateThread(&game_thread, WIIU_THREAD_ENTRY(game_main), 0, NULL, (uint8_t *)game_stack + GAME_STACK,
			GAME_STACK, OSGetThreadPriority(main_thread), (OSThreadAttributes)(1u << OSGetCoreId())))
	{
		wiiu_log("main: the game's thread not made: the game runs on the main thread");
		free(game_stack);
		game_stack = NULL;
		return __real_main(argc, argv);
	}
	OSSetThreadName(&game_thread, "Halo");
	watched_thread = &game_thread;
	stack_mark = *(uint32_t *)game_thread.stackEnd;
	stack_low = game_stack + 16;
	stack_high = game_stack + GAME_STACK / sizeof(uint32_t);
	wiiu_log("main: the game's thread: stack %p to %p (%lu KB), core %lu, priority %ld", game_thread.stackEnd,
		game_thread.stackStart, (unsigned long)GAME_STACK / 1024, (unsigned long)OSGetCoreId(),
		(long)OSGetThreadPriority(&game_thread));
	game_started = 1;
	OSResumeThread(&game_thread);

	/* the main thread from here: what the game's thread asks of it, until
	the end (exit_on_main_thread) */
	for (;;)
	{
		OSMessage message;
		int (*function)(int);

		OSReceiveMessage(&requests, &message, OS_MESSAGE_FLAGS_BLOCKING);
		function = (int (*)(int))message.message;
		message.args[0] = (uint32_t)function((int)message.args[0]);
		OSSendMessage(&replies, &message, OS_MESSAGE_FLAGS_BLOCKING);
	}
}
