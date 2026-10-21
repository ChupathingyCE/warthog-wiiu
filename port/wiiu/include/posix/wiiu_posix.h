/*
WIIU_POSIX.H

POSIX the platform layer uses that newlib and wut leave out, declared for
every unit of the Wii U build (port/wiiu/src/wiiu_posix.c defines them).
*/

#ifndef __WIIU_POSIX_H
#define __WIIU_POSIX_H

#include <time.h>

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

/* utimensat's (newlib defines them for Cygwin and RTEMS only) */
#ifndef UTIME_OMIT
#define UTIME_NOW -2L
#define UTIME_OMIT -1L
#endif

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remain);

#endif
