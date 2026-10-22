/*
WIIU_GX2.H

What the screen (wiiu_video.c) and the renderer's OpenGL (wiiu_gl.c) share,
in GX2's own types: the screen's color and depth buffers, which OpenGL's
default framebuffer is, and the context state that GX2's clears leave unset.
*/

#ifndef __WIIU_GX2_H
#define __WIIU_GX2_H

#include <gx2/context.h>
#include <gx2/surface.h>

/* the screen's buffers and GX2's context state (wiiu_video.c); NULL until
GX2 is up */
GX2ColorBuffer *wiiu_video_screen_color(void);
GX2DepthBuffer *wiiu_video_screen_depth(void);
GX2ContextState *wiiu_video_context_state(void);
/* nonzero while the game has the foreground and GX2 can draw */
int wiiu_video_drawing(void);

/* the end of a frame, after its swap (wiiu_gl.c): what the GPU is done with
let go */
void wiiu_gl_frame_end(void);
/* nonzero once the renderer has drawn a frame: the screens are no longer
cleared for it (wiiu_video_idle) */
int wiiu_gl_drawing(void);

#endif
