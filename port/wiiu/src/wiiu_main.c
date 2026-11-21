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

/* (the watch thread's, every half second) how deep the game's thread has
been: the lowest word of its stack that is not the pattern any more */
void wiiu_main_stack_watch(void)
{
	OSTime now = OSGetSystemTime();
	const uint32_t *word, *end;
	unsigned long used;

	if (!game_started || !game_stack || (stack_checked && now - stack_checked < OSSecondsToTicks(STACK_CHECK_SECONDS)))
		return;
	stack_checked = now;
	/* (the bottom words are the system's: OSCreateThread's mark) */
	word = game_stack + 16;
	end = game_stack + GAME_STACK / sizeof(uint32_t);
	while (word < end && *word == STACK_PATTERN)
		word++;
	used = (unsigned long)((const uint8_t *)end - (const uint8_t *)word);
	if (stack_logged_at && used < stack_logged + STACK_LOG_GROWTH &&
		now - stack_logged_at < OSSecondsToTicks(STACK_LOG_SECONDS))
		return;
	wiiu_log("stack: the game's thread has used %lu KB of its %lu KB at most", used / 1024,
		(unsigned long)GAME_STACK / 1024);
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
