/*
WIIU_POSIX.C

What the platform layer asks of POSIX that newlib and wut do not have.
*/

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "wiiu_port.h"
#include "wiiu_posix.h"

/* sleep until a CLOCK_MONOTONIC deadline (platform.h's frame pacing), or for
an interval */
int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain)
{
	struct timespec now;
	long long nanoseconds;

	(void)remain;
	if (!request)
		return EINVAL;
	nanoseconds = (long long)request->tv_sec * 1000000000LL + request->tv_nsec;
	if (flags & TIMER_ABSTIME)
	{
		clock_gettime(clock, &now);
		nanoseconds -= (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
	}
	if (nanoseconds > 0)
		OSSleepTicks(OSNanosecondsToTicks(nanoseconds));
	return 0;
}

/* ---------- a code address's symbol (dlfcn.h), from the loader's table */

#include <coreinit/debug.h>
#include "dlfcn.h"

int dladdr(const void *address, Dl_info *information)
{
	static char name[128];
	uint32_t symbol;

	memset(information, 0, sizeof(*information));
	symbol = OSGetSymbolName((uint32_t)address, name, sizeof(name));
	if (!symbol || !name[0])
		return 0;
	information->dli_sname = name;
	information->dli_saddr = (void *)symbol;
	return 1;
}

/* ---------- file times: the SD card's are the system's own */

#include <sys/stat.h>

int utimensat(int directory, const char *path, const struct timespec times[2], int flags)
{
	(void)directory;
	(void)path;
	(void)times;
	(void)flags;
	return 0;
}

/* ---------- the system's numbers (xbox_kernel.c's GlobalMemoryStatus) */

#include <unistd.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>

long sysconf(int name)
{
	switch (name)
	{
	case _SC_PAGESIZE:
		return 0x1000;
	case _SC_PHYS_PAGES:
		/* (an application's share of the console's memory) */
		return 0x40000000L / 0x1000;
	case _SC_AVPHYS_PAGES:
		return (long)(MEMGetAllocatableSizeForExpHeapEx(MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM2), 4) / 0x1000);
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		return 3;
	default:
		errno = EINVAL;
		return -1;
	}
}

/* ---------- reads and writes at an offset (xbox_files.c), as a seek and a
read under one lock (the SD card's files have no positional calls) */

#include <coreinit/mutex.h>

static OSMutex positional_mutex;
static int positional_ready;

static void positional_lock(void)
{
	if (!__atomic_exchange_n(&positional_ready, 1, __ATOMIC_ACQ_REL))
		OSInitMutex(&positional_mutex);
	OSLockMutex(&positional_mutex);
}

ssize_t pread(int descriptor, void *buffer, size_t count, off_t offset)
{
	off_t previous;
	ssize_t result = -1;

	positional_lock();
	previous = lseek(descriptor, 0, SEEK_CUR);
	if (previous != (off_t)-1 && lseek(descriptor, offset, SEEK_SET) != (off_t)-1)
	{
		result = read(descriptor, buffer, count);
		lseek(descriptor, previous, SEEK_SET);
	}
	OSUnlockMutex(&positional_mutex);
	return result;
}

ssize_t pwrite(int descriptor, const void *buffer, size_t count, off_t offset)
{
	off_t previous;
	ssize_t result = -1;

	positional_lock();
	previous = lseek(descriptor, 0, SEEK_CUR);
	if (previous != (off_t)-1 && lseek(descriptor, offset, SEEK_SET) != (off_t)-1)
	{
		result = write(descriptor, buffer, count);
		lseek(descriptor, previous, SEEK_SET);
	}
	OSUnlockMutex(&positional_mutex);
	return result;
}

/* (SleepEx's wait forever: no signals to end it) */
int pause(void)
{
	for (;;)
		OSSleepTicks(OSSecondsToTicks(60));
}

/* ---------- a folder's files by a pattern (glob.h: the platform layer's
SDL_GlobDirectory), the pattern in its last part alone */

#include <dirent.h>
#include <fnmatch.h>
#include <glob.h>
#include <stdlib.h>

static int glob_compare(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags, int (*error_function)(const char *, int), glob_t *found)
{
	const char *slash = strrchr(pattern, '/');
	char folder[1024];
	const char *name_pattern = slash ? slash + 1 : pattern;
	DIR *directory;
	struct dirent *entry;
	int capacity = 0;

	(void)error_function;
	memset(found, 0, sizeof(*found));
	found->gl_flags = flags;
	if (slash)
		snprintf(folder, sizeof(folder), "%.*s", (int)(slash - pattern), pattern);
	else
		snprintf(folder, sizeof(folder), ".");
	directory = opendir(slash && slash == pattern ? "/" : folder);
	if (!directory)
		return GLOB_NOMATCH;
	while ((entry = readdir(directory)))
	{
		char path[1300];

		if (entry->d_name[0] == '.' && name_pattern[0] != '.')
			continue;
		if (fnmatch(name_pattern, entry->d_name, 0) != 0)
			continue;
		if (found->gl_pathc + 1 >= capacity)
		{
			char **larger;

			capacity = capacity ? capacity * 2 : 16;
			larger = realloc(found->gl_pathv, sizeof(char *) * (size_t)capacity);
			if (!larger)
			{
				closedir(directory);
				return GLOB_NOSPACE;
			}
			found->gl_pathv = larger;
		}
		snprintf(path, sizeof(path), "%s/%s", folder, entry->d_name);
		found->gl_pathv[found->gl_pathc] = strdup(path);
		if (!found->gl_pathv[found->gl_pathc])
		{
			closedir(directory);
			return GLOB_NOSPACE;
		}
		found->gl_pathv[++found->gl_pathc] = NULL;
	}
	closedir(directory);
	found->gl_matchc = found->gl_pathc;
	if (!found->gl_pathc)
		return GLOB_NOMATCH;
	if (!(flags & GLOB_NOSORT))
		qsort(found->gl_pathv, (size_t)found->gl_pathc, sizeof(char *), glob_compare);
	return 0;
}

void globfree(glob_t *found)
{
	int index;

	for (index = 0; index < found->gl_pathc; index++)
		free(found->gl_pathv[index]);
	free(found->gl_pathv);
	found->gl_pathv = NULL;
	found->gl_pathc = 0;
}

/* ---------- the compiler's 64-bit atomics (the 750 has no 64-bit atomic
instructions and devkitPPC no libatomic): under one lock */

static volatile int atomic64_lock;

static void atomic64_acquire(void)
{
	while (__atomic_exchange_n(&atomic64_lock, 1, __ATOMIC_ACQUIRE))
		OSYieldThread();
}

static void atomic64_release(void)
{
	__atomic_store_n(&atomic64_lock, 0, __ATOMIC_RELEASE);
}

unsigned long long wiiu_atomic_load_8(const volatile void *pointer, int order) __asm__("__atomic_load_8");
unsigned long long wiiu_atomic_load_8(const volatile void *pointer, int order)
{
	unsigned long long value;

	(void)order;
	atomic64_acquire();
	value = *(const volatile unsigned long long *)pointer;
	atomic64_release();
	return value;
}

void wiiu_atomic_store_8(volatile void *pointer, unsigned long long value, int order) __asm__("__atomic_store_8");
void wiiu_atomic_store_8(volatile void *pointer, unsigned long long value, int order)
{
	(void)order;
	atomic64_acquire();
	*(volatile unsigned long long *)pointer = value;
	atomic64_release();
}

unsigned long long wiiu_atomic_exchange_8(volatile void *pointer, unsigned long long value, int order)
	__asm__("__atomic_exchange_8");
unsigned long long wiiu_atomic_exchange_8(volatile void *pointer, unsigned long long value, int order)
{
	unsigned long long previous;

	(void)order;
	atomic64_acquire();
	previous = *(volatile unsigned long long *)pointer;
	*(volatile unsigned long long *)pointer = value;
	atomic64_release();
	return previous;
}

unsigned long long wiiu_atomic_fetch_add_8(volatile void *pointer, unsigned long long value, int order)
	__asm__("__atomic_fetch_add_8");
unsigned long long wiiu_atomic_fetch_add_8(volatile void *pointer, unsigned long long value, int order)
{
	unsigned long long previous;

	(void)order;
	atomic64_acquire();
	previous = *(volatile unsigned long long *)pointer;
	*(volatile unsigned long long *)pointer = previous + value;
	atomic64_release();
	return previous;
}

int wiiu_atomic_compare_exchange_8(volatile void *pointer, void *expected, unsigned long long desired, int weak,
	int success, int failure) __asm__("__atomic_compare_exchange_8");
int wiiu_atomic_compare_exchange_8(volatile void *pointer, void *expected, unsigned long long desired, int weak,
	int success, int failure)
{
	int result;

	(void)weak;
	(void)success;
	(void)failure;
	atomic64_acquire();
	if (*(volatile unsigned long long *)pointer == *(unsigned long long *)expected)
	{
		*(volatile unsigned long long *)pointer = desired;
		result = 1;
	}
	else
	{
		*(unsigned long long *)expected = *(volatile unsigned long long *)pointer;
		result = 0;
	}
	atomic64_release();
	return result;
}

/* ---------- MSVC's compiler barrier, as a call (scenario.c declares it) */

void _ReadWriteBarrier(void)
{
	__asm__ volatile ("" : : : "memory");
}

/* ---------- libtiff, which a console build leaves out (the game's
tools/console_build.py; --console-debug-tools keeps it): the debug
screenshots' TIFF export (source/bitmaps/tiff_file.c) finds no file to
write */

void *TIFFOpen(const char *name, const char *mode)
{
	(void)name;
	(void)mode;
	return NULL;
}

int TIFFSetField(void *file, unsigned int tag, ...)
{
	(void)file;
	(void)tag;
	return 0;
}

int TIFFWriteScanline(void *file, void *buffer, unsigned int row, unsigned short sample)
{
	(void)file;
	(void)buffer;
	(void)row;
	(void)sample;
	return -1;
}

void TIFFClose(void *file)
{
	(void)file;
}

/* ---------- thread-local variables (HALO_CONSOLE_NO_THREAD_LOCAL: an RPX
has no thread-local storage): a block per thread for each key, as
pthread's thread-specific values */

#include <pthread.h>

static pthread_mutex_t thread_local_lock = PTHREAD_MUTEX_INITIALIZER;

void *platform_thread_local(void **key, unsigned long size)
{
	pthread_key_t *thread_key = (pthread_key_t *)__atomic_load_n(key, __ATOMIC_ACQUIRE);
	void *block;

	if (!thread_key)
	{
		pthread_mutex_lock(&thread_local_lock);
		thread_key = (pthread_key_t *)*key;
		if (!thread_key)
		{
			thread_key = malloc(sizeof(*thread_key));
			if (!thread_key || pthread_key_create(thread_key, free) != 0)
			{
				wiiu_log("thread-local: no key: cannot continue");
				abort();
			}
			__atomic_store_n(key, (void *)thread_key, __ATOMIC_RELEASE);
		}
		pthread_mutex_unlock(&thread_local_lock);
	}
	block = pthread_getspecific(*thread_key);
	if (!block)
	{
		block = calloc(1, size);
		if (!block || pthread_setspecific(*thread_key, block) != 0)
		{
			wiiu_log("thread-local: no memory: cannot continue");
			abort();
		}
	}
	return block;
}
