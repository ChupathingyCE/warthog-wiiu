/*
WIIU_CONSOLE_LINK.C

Link Profile on the Wii U (the game's port/console/link/console_link.h):
the hooks it asks of the console. Its requests go to the console list's
host over plain HTTP (wiiu_online_request, src/wiiu_online.c), on a thread
of the platform layer's (thread_stop.h: it stops at the game's end). The
device key and the linked profile are kept on the SD card beside the game:
wiiu/apps/halo/delta_device.key and delta_link.txt. Delta Peer's PROFILE
carries console_profile_id() (HALO_CONSOLE_PROFILE_ID).
*/

#include <string.h>

#include <coreinit/mutex.h>
#include <coreinit/time.h>

#include "posix.h"
#include "thread_stop.h"
#include "console_link.h"
#include "wiiu_port.h"

int wiiu_online_request(const char *path, const char *body, int *status, char *reply, int reply_size);

static OSMutex link_lock;
static int link_started;

struct link_thread
{
	void (*function)(void *context);
	void *context;
};

static struct link_thread link_thread_start;

static void *link_thread(void *parameter)
{
	struct link_thread *start = parameter;

	start->function(start->context);
	return NULL;
}

static int link_start_thread(void (*function)(void *context), void *context)
{
	/* (console_link.c runs one at a time, so one start's place is enough) */
	link_thread_start.function = function;
	link_thread_start.context = context;
	return thread_stop_create("Delta link", link_thread, &link_thread_start) == 0;
}

static void link_enter(void)
{
	OSLockMutex(&link_lock);
}

static void link_leave(void)
{
	OSUnlockMutex(&link_lock);
}

static void link_random(void *buffer, int size)
{
	/* (wiiu_net.c's pool: the clock's jitter and the SD card's seed) */
	posix_random_bytes(buffer, (posix_ulong)size);
}

static unsigned long link_milliseconds(void)
{
	return (unsigned long)OSTicksToMilliseconds(OSGetSystemTime());
}

static void link_log(const char *text)
{
	wiiu_log("%s", text);
}

static const struct console_link_port wiiu_link_port =
{
	"wiiu",
	"Warthog Wii U",
	"fs:/vol/external01/wiiu/apps/halo/delta_device.key",
	"fs:/vol/external01/wiiu/apps/halo/delta_link.txt",
	wiiu_online_request,
	/* (no TLS on the console yet) */
	NULL,
	link_start_thread,
	link_enter,
	link_leave,
	link_random,
	link_milliseconds,
	link_log,
};

/* the main thread's, before the panel or Delta Peer asks */
void wiiu_console_link_start(void)
{
	if (link_started)
		return;
	link_started = 1;
	OSInitMutex(&link_lock);
	console_link_initialize(&wiiu_link_port);
}

int console_profile_id(unsigned char *id)
{
	wiiu_console_link_start();
	return console_link_player_id(id);
}
