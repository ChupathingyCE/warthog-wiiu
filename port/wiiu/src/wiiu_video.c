/*
WIIU_VIDEO.C

The TV's and the GamePad's picture, through GX2. The steps are SDL's Wii U
video driver's (devkitPro's SDL, branch wiiu-sdl2-2.28,
src/video/wiiu/SDL_wiiuvideo.c and SDL_wiiu_gfx_heap.c) and wut's libwhb
(libraries/libwhb/src/gfx.c and gfx_heap.c), both zlib-licensed:

- GX2Init with a command buffer pool of our own (SDL's WIIU_VideoInit);
- the TV's render mode from the system's scan mode, the GamePad's the
system's (WIIU_VideoInit);
- the scan buffers in the foreground heap, the color and depth buffers the
GPU draws into in MEM1, both heaps made when the foreground is acquired and
destroyed when it is released, with GX2R's allocator sending color, depth
and scan buffers to MEM1 (WIIU_ForegroundAcquired, WIIU_ForegroundReleased,
WIIU_GfxHeap_*; libwhb's GfxProcCallbackAcquired and
GfxProcCallbackReleased);
- one color buffer, copied to both screens' scan buffers at the end of a
frame, then the swap and the flush, the screens enabled after the first
frame (SDL's WIIU_SDL_RenderPresent);
- a frame begun only once the previous swap is shown (libwhb's
WHBGfxBeginRender).

The renderer itself (d3d8_gl.c over wiiu_gl.c) is not up yet
(port/wiiu/STATUS.md, milestone 3): until it is, the screens are cleared to
one color every frame (wiiu_video_idle), so that GX2 is seen to work, and
the game runs without a picture, as the dedicated server does. The log's
text screens (wiiu_screen.c) are let go when GX2 starts.
*/

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/memdefaultheap.h>
#include <coreinit/memexpheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/swap.h>
#include <gx2r/mem.h>
#include <proc_ui/procui.h>

#include "wiiu_port.h"

/* (libwhb's WHB_GFX_COMMAND_BUFFER_POOL_SIZE) */
#define VIDEO_COMMAND_BUFFER_POOL_SIZE 0x400000
/* (the GamePad's screen: SDL's DRC_SCREEN_WIDTH and DRC_SCREEN_HEIGHT) */
#define VIDEO_DRC_WIDTH 854
#define VIDEO_DRC_HEIGHT 480
/* (MEMRecordStateForFrmHeap's tag for MEM1, as SDL's 'SDL2') */
#define VIDEO_MEM1_STATE_TAG 0x48414c4f /* 'HALO' */

/* the color the screens are cleared to until the renderer draws */
#define VIDEO_IDLE_RED 0.06f
#define VIDEO_IDLE_GREEN 0.18f
#define VIDEO_IDLE_BLUE 0.30f

static int video_state; /* 0 not started, 1 up, -1 failed or shut down */
static int video_foreground;
static int video_screens_enabled;
static void *command_buffer_pool;
static GX2TVRenderMode tv_render_mode;
static GX2DrcRenderMode drc_render_mode;
static uint32_t draw_width, draw_height;
static uint32_t tv_scan_buffer_size, drc_scan_buffer_size;
static void *tv_scan_buffer, *drc_scan_buffer;
static GX2ColorBuffer color_buffer;
static GX2DepthBuffer depth_buffer;
static GX2ContextState *context_state;
static MEMHeapHandle heap_mem1, heap_foreground;

/* ---------- the heaps (SDL_wiiu_gfx_heap.c, libwhb's gfx_heap.c) */

/* MEM1, all of what is free, as an expanded heap inside the frame heap,
whose state is recorded so that the release gives back exactly this */
static int heap_mem1_create(void)
{
	MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
	uint32_t size;
	void *base;

	if (!MEMRecordStateForFrmHeap(heap, VIDEO_MEM1_STATE_TAG))
	{
		wiiu_log("video: MEMRecordStateForFrmHeap failed (MEM1)");
		return 0;
	}
	size = MEMGetAllocatableSizeForFrmHeapEx(heap, 4);
	base = size ? MEMAllocFromFrmHeapEx(heap, size, 4) : NULL;
	heap_mem1 = base ? MEMCreateExpHeapEx(base, size, 0) : NULL;
	if (!heap_mem1)
	{
		wiiu_log("video: no MEM1 heap (0x%x bytes free)", (unsigned int)size);
		return 0;
	}
	return 1;
}

static void heap_mem1_destroy(void)
{
	if (heap_mem1)
	{
		MEMDestroyExpHeap(heap_mem1);
		heap_mem1 = NULL;
	}
	MEMFreeByStateToFrmHeap(MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1), VIDEO_MEM1_STATE_TAG);
}

/* the foreground heap, all of it (it is the foreground's alone) */
static int heap_foreground_create(void)
{
	MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG);
	uint32_t size = MEMGetAllocatableSizeForFrmHeapEx(heap, 4);
	void *base = size ? MEMAllocFromFrmHeapEx(heap, size, 4) : NULL;

	heap_foreground = base ? MEMCreateExpHeapEx(base, size, 0) : NULL;
	if (!heap_foreground)
	{
		wiiu_log("video: no foreground heap (0x%x bytes free)", (unsigned int)size);
		return 0;
	}
	return 1;
}

static void heap_foreground_destroy(void)
{
	if (heap_foreground)
	{
		MEMDestroyExpHeap(heap_foreground);
		heap_foreground = NULL;
	}
	MEMFreeToFrmHeap(MEMGetBaseHeapHandle(MEM_BASE_HEAP_FG), MEM_FRM_HEAP_FREE_ALL);
}

static void *heap_allocate(MEMHeapHandle heap, uint32_t size, uint32_t alignment)
{
	if (!heap)
		return NULL;
	return MEMAllocFromExpHeapEx(heap, size, alignment < 4 ? 4 : alignment);
}

static void heap_free(MEMHeapHandle heap, void *block)
{
	if (heap && block)
		MEMFreeToExpHeap(heap, block);
}

/* GX2R's buffers: color, depth and scan buffers in MEM1, the rest in MEM2
(SDL's WIIU_GfxHeap_GX2RAlloc, libwhb's GfxGX2RAlloc) */
static int gx2r_in_mem1(GX2RResourceFlags flags)
{
	return (flags & (GX2R_RESOURCE_BIND_COLOR_BUFFER | GX2R_RESOURCE_BIND_DEPTH_BUFFER |
		GX2R_RESOURCE_BIND_SCAN_BUFFER | GX2R_RESOURCE_USAGE_FORCE_MEM1)) &&
		!(flags & GX2R_RESOURCE_USAGE_FORCE_MEM2);
}

static void *gx2r_allocate(GX2RResourceFlags flags, uint32_t size, uint32_t alignment)
{
	if (gx2r_in_mem1(flags))
		return heap_allocate(heap_mem1, size, alignment);
	return MEMAllocFromDefaultHeapEx(size, alignment < 4 ? 4 : alignment);
}

static void gx2r_free(GX2RResourceFlags flags, void *block)
{
	if (gx2r_in_mem1(flags))
		heap_free(heap_mem1, block);
	else
		MEMFreeToDefaultHeap(block);
}

/* ---------- the foreground (ProcUI's callbacks, on the main thread) */

/* the heaps made again and the screens' buffers placed in them (SDL's
WIIU_ForegroundAcquired, with libwhb's color and depth buffers in MEM1) */
static uint32_t video_acquire(void *context)
{
	(void)context;
	if (video_state != 1)
		return 0;
	video_foreground = 1;
	if (!heap_foreground_create() || !heap_mem1_create())
		return (uint32_t)-1;

	tv_scan_buffer = heap_allocate(heap_foreground, tv_scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
	drc_scan_buffer = heap_allocate(heap_foreground, drc_scan_buffer_size, GX2_SCAN_BUFFER_ALIGNMENT);
	color_buffer.surface.image = heap_allocate(heap_mem1, color_buffer.surface.imageSize, color_buffer.surface.alignment);
	depth_buffer.surface.image = heap_allocate(heap_mem1, depth_buffer.surface.imageSize, depth_buffer.surface.alignment);
	if (!tv_scan_buffer || !drc_scan_buffer || !color_buffer.surface.image || !depth_buffer.surface.image)
	{
		wiiu_log("video: the screens' buffers did not fit");
		return (uint32_t)-1;
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, tv_scan_buffer, tv_scan_buffer_size);
	GX2SetTVBuffer(tv_scan_buffer, tv_scan_buffer_size, tv_render_mode,
		GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, drc_scan_buffer, drc_scan_buffer_size);
	GX2SetDRCBuffer(drc_scan_buffer, drc_scan_buffer_size, drc_render_mode,
		GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, color_buffer.surface.image, color_buffer.surface.imageSize);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, depth_buffer.surface.image, depth_buffer.surface.imageSize);

	/* (SDL's: "Workaround for leaving foreground with a swap interval 0") */
	if (GX2GetSwapInterval() == 0)
		GX2SetSwapInterval(0);
	wiiu_log("video: foreground acquired, the screens' buffers made");
	return 0;
}

/* the GPU let finish, the buffers and the heaps let go (SDL's
WIIU_ForegroundReleased) */
static uint32_t video_release(void *context)
{
	(void)context;
	if (video_state != 1 || !video_foreground)
		return 0;
	GX2DrawDone();
	heap_free(heap_foreground, tv_scan_buffer);
	heap_free(heap_foreground, drc_scan_buffer);
	heap_free(heap_mem1, color_buffer.surface.image);
	heap_free(heap_mem1, depth_buffer.surface.image);
	tv_scan_buffer = drc_scan_buffer = NULL;
	color_buffer.surface.image = NULL;
	depth_buffer.surface.image = NULL;
	heap_mem1_destroy();
	heap_foreground_destroy();
	/* (SDL's: "necessary to avoid a black frame on leaving foreground") */
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(TRUE);
	video_foreground = 0;
	wiiu_log("video: foreground released, the screens' buffers let go");
	return 0;
}

/* ---------- the start and the end */

static void video_buffers_describe(void)
{
	/* (libwhb's GfxInitTvColourBuffer) */
	memset(&color_buffer, 0, sizeof(color_buffer));
	color_buffer.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
	color_buffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	color_buffer.surface.width = draw_width;
	color_buffer.surface.height = draw_height;
	color_buffer.surface.depth = 1;
	color_buffer.surface.mipLevels = 1;
	color_buffer.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	color_buffer.surface.aa = GX2_AA_MODE1X;
	color_buffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
	color_buffer.viewNumSlices = 1;
	GX2CalcSurfaceSizeAndAlignment(&color_buffer.surface);
	GX2InitColorBufferRegs(&color_buffer);

	/* (libwhb's GfxInitDepthBuffer, with a stencil, which the game uses) */
	memset(&depth_buffer, 0, sizeof(depth_buffer));
	depth_buffer.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
	depth_buffer.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	depth_buffer.surface.width = draw_width;
	depth_buffer.surface.height = draw_height;
	depth_buffer.surface.depth = 1;
	depth_buffer.surface.mipLevels = 1;
	depth_buffer.surface.format = GX2_SURFACE_FORMAT_FLOAT_D24_S8;
	depth_buffer.surface.aa = GX2_AA_MODE1X;
	depth_buffer.surface.tileMode = GX2_TILE_MODE_DEFAULT;
	depth_buffer.viewNumSlices = 1;
	depth_buffer.depthClear = 1.0f;
	GX2CalcSurfaceSizeAndAlignment(&depth_buffer.surface);
	GX2InitDepthBufferRegs(&depth_buffer);
}

/* GX2 up (SDL's WIIU_VideoInit, with ProcUI already the port's: wiiu_proc.c
started it, so only the foreground's callbacks are added); 1 if the game
can draw, 0 if it runs without a picture */
int wiiu_video_initialize(void)
{
	uint32_t init_attributes[9];
	uint32_t unused;

	if (video_state)
		return 0;
	video_state = -1;

	/* (the log's text screens let go first: GX2 takes the screens over) */
	wiiu_screen_stop();

	command_buffer_pool = memalign(GX2_COMMAND_BUFFER_ALIGNMENT, VIDEO_COMMAND_BUFFER_POOL_SIZE);
	context_state = memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof(GX2ContextState));
	if (!command_buffer_pool || !context_state)
	{
		wiiu_log("video: no memory for GX2's command buffers");
		return 0;
	}
	init_attributes[0] = GX2_INIT_CMD_BUF_BASE;
	init_attributes[1] = (uint32_t)(uintptr_t)command_buffer_pool;
	init_attributes[2] = GX2_INIT_CMD_BUF_POOL_SIZE;
	init_attributes[3] = VIDEO_COMMAND_BUFFER_POOL_SIZE;
	init_attributes[4] = GX2_INIT_ARGC;
	init_attributes[5] = 0;
	init_attributes[6] = GX2_INIT_ARGV;
	init_attributes[7] = 0;
	init_attributes[8] = GX2_INIT_END;
	GX2Init(init_attributes);

	/* (SDL's modes: the TV's from its scan mode, wide; the GamePad's the
	system's) */
	switch (GX2GetSystemTVScanMode())
	{
	case GX2_TV_SCAN_MODE_480I:
	case GX2_TV_SCAN_MODE_480P:
		tv_render_mode = GX2_TV_RENDER_MODE_WIDE_480P;
		break;
	case GX2_TV_SCAN_MODE_1080I:
	case GX2_TV_SCAN_MODE_1080P:
		tv_render_mode = GX2_TV_RENDER_MODE_WIDE_1080P;
		break;
	case GX2_TV_SCAN_MODE_720P:
	default:
		tv_render_mode = GX2_TV_RENDER_MODE_WIDE_720P;
		break;
	}
	drc_render_mode = GX2GetSystemDRCScanMode();
	GX2CalcTVSize(tv_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE,
		&tv_scan_buffer_size, &unused);
	GX2CalcDRCSize(drc_render_mode, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_BUFFERING_MODE_DOUBLE,
		&drc_scan_buffer_size, &unused);

	/* (the game draws at 1280x720 whatever the TV's mode: the copy to the
	scan buffers scales, as SDL's one window texture is copied to both) */
	draw_width = WIIU_SCREEN_WIDTH;
	draw_height = WIIU_SCREEN_HEIGHT;
	video_buffers_describe();

	GX2RSetAllocator(gx2r_allocate, gx2r_free);
	ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, video_acquire, NULL, 100);
	ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, video_release, NULL, 100);

	video_state = 1;
	/* (in the foreground now: the callback called, as SDL calls it) */
	if (video_acquire(NULL) != 0)
	{
		wiiu_log("video: GX2 started, but the screens' buffers could not be made");
		video_release(NULL);
		video_state = -1;
		return 0;
	}

	GX2SetupContextStateEx(context_state, TRUE);
	GX2SetContextState(context_state);
	GX2SetColorBuffer(&color_buffer, GX2_RENDER_TARGET_0);
	GX2SetDepthBuffer(&depth_buffer);
	GX2SetViewport(0.0f, 0.0f, (float)draw_width, (float)draw_height, 0.0f, 1.0f);
	GX2SetScissor(0, 0, draw_width, draw_height);
	GX2SetTVScale(draw_width, draw_height);
	GX2SetDRCScale(VIDEO_DRC_WIDTH, VIDEO_DRC_HEIGHT);
	/* (60 frames a second, with vsync: libwhb's) */
	GX2SetSwapInterval(1);

	wiiu_log("video: GX2 started, TV scan mode %d, GamePad mode %d, drawing %ux%u",
		(int)GX2GetSystemTVScanMode(), (int)drc_render_mode,
		(unsigned int)draw_width, (unsigned int)draw_height);
	wiiu_log("video: no renderer yet: the screens are cleared, the game runs without a picture");
	return 0;
}

/* (proc_end, after ProcUI says to exit: SDL's WIIU_VideoQuit) */
void wiiu_video_shutdown(void)
{
	if (video_state != 1)
		return;
	if (video_foreground)
		video_release(NULL);
	GX2RSetAllocator(NULL, NULL);
	GX2Shutdown();
	free(context_state);
	free(command_buffer_pool);
	context_state = NULL;
	command_buffer_pool = NULL;
	video_state = -1;
	wiiu_log("video: GX2 shut down");
}

/* ---------- the frame */

/* (libwhb's WHBGfxBeginRender: the previous swap shown first, at most ten
vsyncs' wait) */
static void video_frame_wait(void)
{
	uint32_t swap_count, flip_count;
	OSTime last_flip, last_vsync;
	int waits;

	for (waits = 0; waits < 10; waits++)
	{
		GX2GetSwapStatus(&swap_count, &flip_count, &last_flip, &last_vsync);
		if (flip_count >= swap_count)
			return;
		GX2WaitForVsync();
	}
}

/* the frame's color buffer on both screens (SDL's WIIU_SDL_RenderPresent) */
void wiiu_video_swap(void)
{
	if (video_state != 1 || !video_foreground)
		return;
	video_frame_wait();
	GX2CopyColorBufferToScanBuffer(&color_buffer, GX2_SCAN_TARGET_TV);
	GX2CopyColorBufferToScanBuffer(&color_buffer, GX2_SCAN_TARGET_DRC);
	GX2SwapScanBuffers();
	GX2Flush();
	GX2SetContextState(context_state);
	/* (the screens enabled once the first frame is there) */
	if (!video_screens_enabled)
	{
		GX2SetTVEnable(TRUE);
		GX2SetDRCEnable(TRUE);
		video_screens_enabled = 1;
		wiiu_log("video: the first frame shown");
	}
}

/* (every frame, from the system's events, while the game draws nothing:
the screens cleared to one color) */
void wiiu_video_idle(void)
{
	if (video_state != 1 || !video_foreground)
		return;
	GX2ClearColor(&color_buffer, VIDEO_IDLE_RED, VIDEO_IDLE_GREEN, VIDEO_IDLE_BLUE, 1.0f);
	GX2ClearDepthStencilEx(&depth_buffer, depth_buffer.depthClear, depth_buffer.stencilClear,
		GX2_CLEAR_FLAGS_DEPTH | GX2_CLEAR_FLAGS_STENCIL);
	/* (GX2's clears leave the context state unset: libwhb's WHBGfxClearColor) */
	GX2SetContextState(context_state);
	wiiu_video_swap();
}
