/*
DLFCN.H

A code address's symbol (posix_files.c's posix_describe_address), from the
loader's own table (port/wiiu/src/wiiu_posix.c: OSGetSymbolName).
*/

#ifndef __WIIU_DLFCN_H
#define __WIIU_DLFCN_H

typedef struct
{
	const char *dli_fname;
	void *dli_fbase;
	const char *dli_sname;
	void *dli_saddr;
} Dl_info;

int dladdr(const void *address, Dl_info *information);

#endif
