/*
WIIU_SCREEN.C

The log on the TV and the GamePad while there is no renderer (OSScreen,
the system's text screens): the newest lines of port.txt, so that a
console's owner sees how far the game got without a computer. The renderer
takes the screens over once it starts (wiiu_screen_stop).
*/

#include <stdlib.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/screen.h>

#include "wiiu_port.h"

#define SCREEN_LINES 24
/* (the GamePad's width in the system's font; a longer line wraps onto the
next) */
#define SCREEN_COLUMNS 66

static int screen_state; /* 0 not started, 1 showing, -1 stopped */
static int screen_released; /* the HOME button menu has the screens */
static void *buffers[2];
static unsigned int buffer_sizes[2];
static char lines[SCREEN_LINES][SCREEN_COLUMNS + 1];
static int line_count;

/* the room given each screen's buffers: Cemu's OSScreen draws the GamePad's
text past the size it gives for its buffers (it wrote over the heap's next
blocks, the cache file thread's event among them), so each has the TV's
size and a megabyte more */
#define SCREEN_BUFFER_ROOM(size) ((size) > 0x708000u ? (size) + 0x100000u : 0x808000u)

static void screen_start(void)
{
	OSScreenInit();
	buffer_sizes[0] = OSScreenGetBufferSizeEx(SCREEN_TV);
	buffer_sizes[1] = OSScreenGetBufferSizeEx(SCREEN_DRC);
	buffers[0] = MEMAllocFromDefaultHeapEx(SCREEN_BUFFER_ROOM(buffer_sizes[0]), 0x100);
	buffers[1] = MEMAllocFromDefaultHeapEx(SCREEN_BUFFER_ROOM(buffer_sizes[1]), 0x100);
	if (!buffers[0] || !buffers[1])
	{
		screen_state = -1;
		return;
	}
	OSScreenSetBufferEx(SCREEN_TV, buffers[0]);
	OSScreenSetBufferEx(SCREEN_DRC, buffers[1]);
	OSScreenEnableEx(SCREEN_TV, TRUE);
	OSScreenEnableEx(SCREEN_DRC, TRUE);
	screen_state = 1;
}

static volatile int lines_changed;

/* (the log's lock held: wiiu_log.c) a line kept for the next redraw */
void wiiu_screen_line(const char *text)
{
	if (screen_state < 0)
		return;
	if (line_count == SCREEN_LINES)
	{
		memmove(lines[0], lines[1], sizeof(lines[0]) * (SCREEN_LINES - 1));
		line_count--;
	}
	/* (without the platform layer's prefix, which every line has) */
	if (!strncmp(text, "chupathingyce: ", 15))
		text += 15;
	strncpy(lines[line_count], text, SCREEN_COLUMNS);
	lines[line_count][SCREEN_COLUMNS] = 0;
	line_count++;
	lines_changed = 1;
}

/* the screens drawn again with the newest lines, if there are new ones (the
main thread's, from the system's events each frame: drawing waits for the
screens, which the log's writers must not) */
void wiiu_screen_update(void)
{
	char shown[SCREEN_LINES][SCREEN_COLUMNS + 1];
	int screen, line, count;

	if (screen_state < 0 || screen_released || !lines_changed)
		return;
	if (!screen_state)
		screen_start();
	if (screen_state < 0)
		return;
	wiiu_log_lock();
	lines_changed = 0;
	count = line_count;
	memcpy(shown, lines, sizeof(shown));
	wiiu_log_unlock();
	for (screen = 0; screen < 2; screen++)
	{
		OSScreenID id = screen ? SCREEN_DRC : SCREEN_TV;
		int half;

		/* (both of the screen's buffers cleared and drawn the same: the
		system's own clear is not to be counted on in every emulator) */
		memset(buffers[screen], 0, buffer_sizes[screen]);
		for (half = 0; half < 2; half++)
		{
			OSScreenPutFontEx(id, 0, 0, "Chupathingy for Wii U: bring-up build");
			for (line = 0; line < count; line++)
				OSScreenPutFontEx(id, 0, line + 2, shown[line]);
			DCFlushRange(buffers[screen], buffer_sizes[screen]);
			OSScreenFlipBuffersEx(id);
		}
	}
}

void wiiu_screen_stop(void)
{
	if (screen_state == 1)
	{
		OSScreenShutdown();
		MEMFreeToDefaultHeap(buffers[0]);
		MEMFreeToDefaultHeap(buffers[1]);
	}
	screen_state = -1;
}

/* The foreground given up (the HOME button menu): nothing is drawn until it
comes back. WHBLogConsole does the same and frees only its buffers, which
it keeps in MEM1 (the system takes MEM1 back); ours are in MEM2 and stay,
and OSScreen is never shut down, as it is not there. */
void wiiu_screen_release(void)
{
	screen_released = 1;
}

/* (the foreground back: the buffers given to OSScreen again, as
WHBLogConsole gives them, and the lines drawn at the next frame) */
void wiiu_screen_acquire(void)
{
	if (screen_state == 1)
	{
		OSScreenSetBufferEx(SCREEN_TV, buffers[0]);
		OSScreenSetBufferEx(SCREEN_DRC, buffers[1]);
	}
	screen_released = 0;
	lines_changed = 1;
}
