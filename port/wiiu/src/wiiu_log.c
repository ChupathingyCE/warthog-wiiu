/*
WIIU_LOG.C

Where the game's and the platform layer's output goes on the Wii U: their
stdout and stderr (the platform layer's log, platform_log) and the port's
own lines (wiiu_log) go to the system's log (OSReport: Cemu's log.txt, and
on a console Aroma's logging, which udplogserver reads on the network), and
to sd:/wiiu/apps/halo/port.txt, which the owner of a console can send back.
The game's own debug.txt is the game's (its logs folder, port_config.c).

The file is on the SD card (wut mounts it as fs:/vol/external01 at
start-up), begun again at each start.
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <coreinit/debug.h>
#include <coreinit/mutex.h>
#include <coreinit/time.h>

#include "wiiu_port.h"

#define LOG_FOLDER WIIU_APP_FOLDER
#define LOG_PATH LOG_FOLDER "/port.txt"

static OSMutex log_mutex;
static int log_ready;
static FILE *log_file;
static int log_file_failed;
static int log_started;
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
	OSReport("%s\n", line);
	wiiu_screen_line(line);
	if (log_file_failed)
		return;
	/* (opened for each line, while the port is brought up: a crash or a
	stop loses none, on a console or in an emulator that writes a file out
	only when it is closed) */
	if (!log_started)
	{
		mkdir("fs:/vol/external01/wiiu", 0777);
		mkdir("fs:/vol/external01/wiiu/apps", 0777);
		mkdir(LOG_FOLDER, 0777);
	}
	log_file = fopen(LOG_PATH, log_started ? "a" : "w");
	if (!log_file)
	{
		log_file_failed = 1;
		return;
	}
	log_started = 1;
	fputs(line, log_file);
	fputc('\n', log_file);
	fclose(log_file);
	log_file = NULL;
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
	wiiu_proc_poll();
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
	wiiu_proc_poll();
}

void wiiu_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	wiiu_log_v(format, arguments);
	va_end(arguments);
}

void wiiu_log_lock(void)
{
	log_initialize();
	OSLockMutex(&log_mutex);
}

void wiiu_log_unlock(void)
{
	OSUnlockMutex(&log_mutex);
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
	/* the game's folder is its working folder: maps/, the settings and the
	logs are found from there, as beside a desktop build's executable */
	if (chdir(WIIU_APP_FOLDER) != 0)
		wiiu_log("cannot use %s as the game's folder", WIIU_APP_FOLDER);
}
