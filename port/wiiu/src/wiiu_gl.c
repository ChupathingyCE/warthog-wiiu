/*
WIIU_GL.C

The OpenGL the desktop renderer is written against (port/linux/src/d3d8_gl.c),
on the Wii U's GX2. Not yet (port/wiiu/STATUS.md, milestone 3): no entry
point is given, so the renderer is not started (platform_video_initialize
says there is no picture first).
*/

#include <stddef.h>

#include "wiiu_port.h"

void *wiiu_gl_proc_address(const char *name)
{
	(void)name;
	return NULL;
}
