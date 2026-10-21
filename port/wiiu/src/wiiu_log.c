/*
WIIU_LOG.C

Where the game's and the platform layer's output goes on the Wii U: their
stdout and stderr (the platform layer's log, platform_log) and the port's
own lines (wiiu_log) go to the system's log (OSReport: Cemu's log.txt, and
on a console Aroma's logging, which udplogserver reads on the network), and
to sd:/wiiu/apps/halo/port.txt, which the owner of a console can send back.
The game's own debug.txt is the game's (its logs folder, port_config.c).

The file is opened once the SD card is there (wut mounts it as
fs:/vol/external01 at start-up) and flushed at most twice a second, and at
once for a line that says something failed.
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

#include <coreinit/debug.h>
#include <coreinit/mutex.h>
#include <coreinit/time.h>

#include "wiiu_port.h"

#define LOG_FOLDER "fs:/vol/external01/wiiu/apps/halo"
#define LOG_PATH LOG_FOLDER "/port.txt"

static OSMutex log_mutex;
static int log_ready;
static FILE *log_file;
static int log_file_failed;
static OSTime log_flushed_at;
/* a line being put together from writes without its newline yet */
static char pending[512];
static size_t pending_length;

static void log_initialize(void)
{
	if (!log_ready)
	{
		OSInitMutex(&log_mutex);
		log_ready = 1;
	}
}

/* (the mutex held) */
static void log_line(const char *line)
{
	OSTime now;

	OSReport("%s\n", line);
	if (!log_file && !log_file_failed)
	{
		mkdir("fs:/vol/external01/wiiu", 0777);
		mkdir("fs:/vol/external01/wiiu/apps", 0777);
		mkdir(LOG_FOLDER, 0777);
		log_file = fopen(LOG_PATH, "w");
		if (!log_file)
			log_file_failed = 1;
	}
	if (!log_file)
		return;
	fputs(line, log_file);
	fputc('\n', log_file);
	now = OSGetTime();
	if (now - log_flushed_at > OSMillisecondsToTicks(500) || strstr(line, "FAIL") || strstr(line, "fail") ||
		strstr(line, "assert") || strstr(line, "exception") || strstr(line, "crash"))
	{
		fflush(log_file);
		log_flushed_at = now;
	}
}

/* (the mutex held) a write to stdout or stderr: whole lines out */
static void log_write(const char *text, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++)
	{
		char character = text[index];

		if (character == '\n' || pending_length == sizeof(pending) - 1)
		{
			pending[pending_length] = 0;
			log_line(pending);
			pending_length = 0;
			if (character == '\n')
				continue;
		}
		if (character != '\r')
			pending[pending_length++] = character;
	}
}

static ssize_t log_devoptab_write(struct _reent *reent, void *file, const char *text, size_t length)
{
	(void)reent;
	(void)file;
	log_initialize();
	OSLockMutex(&log_mutex);
	log_write(text, length);
	OSUnlockMutex(&log_mutex);
	return (ssize_t)length;
}

static const devoptab_t log_devoptab =
{
	.name = "halo_log",
	.write_r = log_devoptab_write,
};

void wiiu_log_v(const char *format, va_list arguments)
{
	char line[1024];
	int length = vsnprintf(line, sizeof(line), format, arguments);

	if (length < 0)
		return;
	log_initialize();
	OSLockMutex(&log_mutex);
	log_line(line);
	OSUnlockMutex(&log_mutex);
}

void wiiu_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	wiiu_log_v(format, arguments);
	va_end(arguments);
}

void wiiu_log_flush(void)
{
	if (!log_ready)
		return;
	OSLockMutex(&log_mutex);
	if (pending_length)
	{
		pending[pending_length] = 0;
		log_line(pending);
		pending_length = 0;
	}
	if (log_file)
		fflush(log_file);
	OSUnlockMutex(&log_mutex);
}

/* stdout and stderr into the log (before the game's constructors, whose
lines are the first of it) */
__attribute__((constructor(101))) static void log_attach(void)
{
	log_initialize();
	devoptab_list[STD_OUT] = &log_devoptab;
	devoptab_list[STD_ERR] = &log_devoptab;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	wiiu_log("Chupathingy for Wii U %s starting", HALO_VERSION);
	wiiu_memory_report();
}
