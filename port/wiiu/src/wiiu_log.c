/*
WIIU_LOG.C

Where the game's and the platform layer's output goes on the Wii U: their
stdout and stderr (the platform layer's log, platform_log) and the port's
own lines (wiiu_log) go to the system's log (OSReport: Cemu's log.txt, and
on a console Aroma's logging, which udplogserver reads on the network), and
to sd:/wiiu/apps/halo/port.txt, which the owner of a console can send back.
The game's own debug.txt is the game's (its logs folder, port_config.c).

The file is on the SD card (wut mounts it as fs:/vol/external01 at
start-up), begun again at each start. Each write opens, appends and closes
it, so a stop loses none of what was written. Once ProcUI is up, the lines
are written by a thread of the log's own (wiiu_log_start_writer): a line
is queued in memory and the thread that logged it goes on, where before it
waited for the SD card (10 to 30 ms a line on a console, the frame lines'
own writes among them). wiiu_log_flush still writes everything queued
before it returns, and the end (proc_end) stops the thread first and logs
as before from then on. A crash that stops the console at once can lose
the lines of the last few milliseconds.
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <malloc.h>

#include <coreinit/condition.h>
#include <coreinit/debug.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
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
/* the lines kept in memory while the HOME button menu has the foreground
(the SD card may not be written then): written out on return */
#define HELD_LINES 64
static int log_held;
static char held[HELD_LINES][256];
static int held_count;
/* the time the SD card's writes took (wiiu_log_write_ticks), and the share
of it the thread that logged waited for (wiiu_log_wait_ticks) */
static OSTime log_write_time, log_wait_time;

/* the lines waiting for the writer thread: a ring of text, whole lines with
their newlines. The file's own mutex keeps the writes in order: the writer
takes it before it lets go of the log's, and a flush takes it to write what
is left after the writer's chunk */
#define QUEUE_SIZE (64 * 1024)
#define WRITER_CHUNK 8192
static char queue[QUEUE_SIZE];
static size_t queue_start, queue_length;
static unsigned long queue_dropped;
static OSMutex file_mutex;
static OSCondition queue_ready;
static OSThread *writer_thread;
static void *writer_stack;
/* the writer takes the lines (set and read with the log's mutex held) */
static int writer_running, writer_stop;

static void log_initialize(void)
{
	if (!log_ready)
	{
		OSInitMutex(&log_mutex);
		OSInitMutex(&file_mutex);
		OSInitCond(&queue_ready);
		log_ready = 1;
	}
}

/* (the file's mutex held) text appended to the file on the SD card */
static void file_write(const char *text, size_t length)
{
	OSTime start;

	if (log_file_failed || !length)
		return;
	start = OSGetSystemTime();
	/* (opened for each write: a crash or a stop loses none, on a console or
	in an emulator that writes a file out only when it is closed) */
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
	fwrite(text, 1, length, log_file);
	fclose(log_file);
	log_file = NULL;
	log_write_time += OSGetSystemTime() - start;
}

/* (the log's mutex held) the queue written out by the calling thread */
static void queue_drain(void)
{
	OSTime start = OSGetSystemTime();

	OSLockMutex(&file_mutex);
	while (queue_length)
	{
		size_t length = QUEUE_SIZE - queue_start < queue_length ? QUEUE_SIZE - queue_start : queue_length;

		file_write(queue + queue_start, length);
		queue_start = (queue_start + length) % QUEUE_SIZE;
		queue_length -= length;
	}
	queue_start = 0;
	OSUnlockMutex(&file_mutex);
	log_wait_time += OSGetSystemTime() - start;
}

/* (the log's mutex held) a line queued for the writer: 0 if it is full */
static int queue_line(const char *line)
{
	size_t length = strlen(line), index, at;

	if (queue_length + length + 1 > QUEUE_SIZE)
		return 0;
	at = (queue_start + queue_length) % QUEUE_SIZE;
	for (index = 0; index < length; index++)
		queue[(at + index) % QUEUE_SIZE] = line[index];
	queue[(at + length) % QUEUE_SIZE] = '\n';
	queue_length += length + 1;
	return 1;
}

/* (the mutex held) */
static void log_line(const char *line)
{
	OSReport("%s\n", line);
	wiiu_screen_line(line);
	if (writer_running)
	{
		if (!queue_line(line))
		{
			/* (full: written out here, unless the SD card may not be written
			now, the HOME button menu's; then the line is left out, counted) */
			if (log_held)
			{
				queue_dropped++;
				return;
			}
			queue_drain();
			queue_line(line);
		}
		OSSignalCond(&queue_ready);
		return;
	}
	if (log_held)
	{
		if (held_count < HELD_LINES)
		{
			strncpy(held[held_count], line, sizeof(held[0]) - 1);
			held[held_count][sizeof(held[0]) - 1] = 0;
			held_count++;
		}
		return;
	}
	{
		OSTime start = OSGetSystemTime();

		OSLockMutex(&file_mutex);
		file_write(line, strlen(line));
		file_write("\n", 1);
		OSUnlockMutex(&file_mutex);
		log_wait_time += OSGetSystemTime() - start;
	}
}

/* the writer thread: the queue's lines out to the SD card, a chunk at a
time, while the foreground is the game's */
static int writer_main(int argc, const char **argv)
{
	static char chunk[WRITER_CHUNK];

	(void)argc;
	(void)argv;
	OSLockMutex(&log_mutex);
	while (!writer_stop)
	{
		size_t length = 0;

		if (!queue_length || log_held)
		{
			OSWaitCond(&queue_ready, &log_mutex);
			continue;
		}
		if (queue_dropped)
		{
			char note[96];

			snprintf(note, sizeof(note), "log: %lu lines left out while the HOME button menu was open (the queue full)",
				queue_dropped);
			queue_dropped = 0;
			if (!queue_line(note))
				queue_dropped = 1;
		}
		while (length < WRITER_CHUNK && queue_length)
		{
			chunk[length++] = queue[queue_start];
			queue_start = (queue_start + 1) % QUEUE_SIZE;
			queue_length--;
		}
		/* (the file's mutex before the log's is let go: a flush that comes
		now writes after this chunk) */
		OSLockMutex(&file_mutex);
		OSUnlockMutex(&log_mutex);
		file_write(chunk, length);
		OSUnlockMutex(&file_mutex);
		OSLockMutex(&log_mutex);
	}
	OSUnlockMutex(&log_mutex);
	return 0;
}

void wiiu_log_start_writer(void)
{
	const uint32_t stack_size = 0x4000;

	log_initialize();
	OSLockMutex(&log_mutex);
	if (writer_thread || writer_stop || log_file_failed)
	{
		OSUnlockMutex(&log_mutex);
		return;
	}
	writer_thread = memalign(16, sizeof(OSThread));
	writer_stack = memalign(16, stack_size);
	/* (a step above the caller's priority, as the audio's filling thread:
	it sleeps until there are lines, and a thread below the game's, which
	never sleeps, is never run in Cemu) */
	if (!writer_thread || !writer_stack ||
		!OSCreateThread(writer_thread, writer_main, 0, NULL, (unsigned char *)writer_stack + stack_size, stack_size,
		OSGetThreadPriority(OSGetCurrentThread()) - 1, OS_THREAD_ATTRIB_AFFINITY_ANY))
	{
		free(writer_thread);
		free(writer_stack);
		writer_thread = NULL;
		writer_stack = NULL;
		OSUnlockMutex(&log_mutex);
		wiiu_log("log: the writer thread not started; each line is written as it comes");
		return;
	}
	OSSetThreadName(writer_thread, "Chupathingy log");
	writer_running = 1;
	OSResumeThread(writer_thread);
	OSUnlockMutex(&log_mutex);
	wiiu_log("log: lines written to the SD card by a thread of their own");
}

/* (the end's first step: proc_end) the writer stopped, what it had left
written, and every line from here on written as it comes */
void wiiu_log_stop_writer(void)
{
	OSThread *thread;

	if (!log_ready)
		return;
	OSLockMutex(&log_mutex);
	thread = writer_thread;
	writer_stop = 1;
	if (writer_running)
	{
		writer_running = 0;
		OSSignalCond(&queue_ready);
		if (!log_held)
			queue_drain();
	}
	OSUnlockMutex(&log_mutex);
	if (thread)
	{
		OSJoinThread(thread, NULL);
		writer_thread = NULL;
		/* (what it had not taken while held: written now) */
		OSLockMutex(&log_mutex);
		if (!log_held)
			queue_drain();
		OSUnlockMutex(&log_mutex);
	}
}

long long wiiu_log_write_ticks(void)
{
	return (long long)log_write_time;
}

long long wiiu_log_wait_ticks(void)
{
	return (long long)log_wait_time;
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
	/* (the writer's queue too: everything logged is on the card when this
	returns, as before the writer) */
	if (writer_running && !log_held)
		queue_drain();
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

/* lines kept in memory (held) or written out again (not held) */
void wiiu_log_hold(int on)
{
	int index, count;
	static char out[HELD_LINES][256];

	log_initialize();
	OSLockMutex(&log_mutex);
	if (on)
	{
		log_held = 1;
		OSUnlockMutex(&log_mutex);
		return;
	}
	log_held = 0;
	if (writer_running)
		OSSignalCond(&queue_ready);
	else if (queue_length)
		queue_drain();
	count = held_count;
	memcpy(out, held, sizeof(held));
	held_count = 0;
	for (index = 0; index < count; index++)
		log_line(out[index]);
	OSUnlockMutex(&log_mutex);
}
