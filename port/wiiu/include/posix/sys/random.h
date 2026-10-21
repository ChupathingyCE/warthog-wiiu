/*
SYS/RANDOM.H

Random numbers (port/wiiu/src/wiiu_net.c's: Cafe OS has no source of its
own for an application).
*/

#ifndef __WIIU_SYS_RANDOM_H
#define __WIIU_SYS_RANDOM_H

#include <sys/types.h>

ssize_t getrandom(void *buffer, size_t size, unsigned int flags);

#endif
