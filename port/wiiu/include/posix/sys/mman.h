/*
SYS/MMAN.H

The memory mapping the desktop platform layer asks for (xbox_memory.c,
xbox_heap.c), on the Wii U (port/wiiu/src/wiiu_memory.c). Cafe OS gives an
application neither page protection nor anonymous mappings: the window of
Xbox memory is set aside before the C library takes the heap, and a mapping
asked for inside it is that part of it; any other is heap memory. Protection
always succeeds and changes nothing (HALO_CONSOLE_NO_WRITE_WATCH).
*/

#ifndef __WIIU_SYS_MMAN_H
#define __WIIU_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)

#define MADV_NORMAL 0
#define MADV_DONTNEED 4

void *mmap(void *address, size_t length, int protection, int flags, int descriptor, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);
int madvise(void *address, size_t length, int advice);

#endif
