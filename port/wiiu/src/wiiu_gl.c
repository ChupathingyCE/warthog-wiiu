/*
WIIU_GL.C

The OpenGL the desktop renderer is written against (port/linux/src/d3d8_gl.c,
the Xbox's Direct3D 8 over OpenGL 4.5, and its texture decoder), on the Wii
U's GX2. The renderer stays the desktop builds' own; this file gives it its
GL entry points (SDL_GL_GetProcAddress, gl_functions.c), as the Xbox 360
port's x360_glsl.c and x360_gl.c give them over Direct3D 9 (warthog-360,
port/xbox360/src/x360_gl.c, whose arrangement this file follows), and
CafeGLSL compiles the renderer's GLSL for Latte as the game runs
(port/wiiu/docs/renderer.md).

How GL's model meets GX2's (the GX2 calls as SDL's Wii U renderer makes them,
devkitPro's SDL, wiiu-sdl2-2.28, src/render/wiiu; GX2's behavior checked in
decaf-emu, src/libdecaf/src/cafe/libraries/gx2):

- Data. The game's vertices, indices and texels: a map's vertices are the
  Xbox's, little-endian, and are fetched with no byte swap (Latte is
  little-endian); what the CPU writes (the game's own vertices, the generic
  attributes' values) is big-endian and fetched with the swap of its
  element's size. Indices are the CPU's 16-bit words (the maps' are made so
  as they load, port/console/game/tag_byte_swap.c): GX2_INDEX_TYPE_U16.
  Texel bytes are stored as they are (GX2's surfaces take no swap: decaf's
  gx2_format.cpp, every format's endian 0): RGBA bytes, DXT blocks as the
  Xbox has them, BGRA bytes swapped to RGBA as they are copied.
- Textures made with their texels (the map's bitmaps), DXT textures, cube
  maps and volumes are in GX2's own tiling (GX2_TILE_MODE_DEFAULT), their
  texels written into linear surfaces and copied into place by
  GX2CopySurface (games' textures are tiled: decaf's gx2_surface.cpp). Textures made empty (render
  targets, atlases written a part at a time) are linear surfaces
  (GX2_TILE_MODE_LINEAR_ALIGNED), as SDL's WIIU_SDL_CreateTexture makes
  them, so that the CPU writes rows at the surface's pitch and a texture
  used as a framebuffer attachment is drawn into where it is sampled from
  (a GX2ColorBuffer over the same memory, as SDL's render targets are). The
  console's GPU stopped at the first draw that sampled linear mipmapped and
  DXT textures; SDL and Butter and Jelly only ever sample linear textures
  of one level (RGBA8, or R8 and R8G8 video planes). Levels are where
  GX2CalcSurfaceSizeAndAlignment puts them: level 0 at image, level 1 at
  mipmaps, the rest at mipmaps plus mipLevelOffset[level - 1].
- Buffers keep their bytes in memory the GPU reads, flushed from the CPU's
  cache after each write. The renderer appends to its stream and index
  buffers and orphans them when full, so data a queued draw reads is never
  written over; orphaned memory is let go two frames on.
- Clip space: the renderer asks for glClipControl(GL_UPPER_LEFT,
  GL_ZERO_TO_ONE), Direct3D's. GX2's viewport puts y = 1 on the first row,
  as Direct3D does (decaf's GX2SetViewport: a y scale of -height / 2), and
  its depth is OpenGL's -1 to 1 (VPORT_ZSCALE (far - near) / 2): each vertex
  shader's depth is made OpenGL's as it ends, z' = 2z - w (what the desktop
  renderer itself does where there is no glClipControl, HALO_GL_NO_CLIP_CONTROL
  in nv2a_vsh.c). The default framebuffer is the screen's color buffer, whose
  rows GL counts from the bottom (the present blit).
- Shaders: each is compiled once by CafeGLSL (register mode, uniforms set
  with GX2SetVertexUniformReg and GX2SetPixelUniformReg at the offsets the
  compiled shader records). A vertex shader's outputs and a fragment
  shader's inputs are given locations by name before they are compiled, the
  same for every shader, so that any vertex shader goes with any fragment
  shader (Latte matches them by semantic, which CafeGLSL makes from the
  location: cafe_compiler.cpp's LatteSemantic).
- Fetch shaders (GX2's attribute fetch) are made for each layout of the
  attributes a draw reads, once, and kept.

Entry points the renderer calls that are not implemented say so in the log
the first time.
*/

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <coreinit/cache.h>
#include <coreinit/memorymap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/enum.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>

#ifdef HALO_WIIU_CAFEGLSL
#include <cafeglsl/CafeGLSLCompiler.h>
#endif

#include <SDL3/SDL_opengl.h>

#include "wiiu_port.h"
#include "wiiu_gx2.h"
#include "wiiu_shader_cache.h"

#ifndef APIENTRY
#define APIENTRY
#endif

/* GX2 functions the console has (gx2.rpl exports them; wut's library has
their imports) that wut's headers do not declare, with decaf-emu's
signatures (gx2_surface.h, gx2_query.h, gx2_sampler.h) */
uint32_t GX2GetSurfaceMipPitch(const GX2Surface *surface, uint32_t level);
uint32_t GX2GetSurfaceMipSliceSize(const GX2Surface *surface, uint32_t level);
typedef struct
{
	uint64_t gpu_data[8];
} wiiu_query_data;
#define WIIU_QUERY_OCCLUSION 0
void GX2QueryBegin(uint32_t type, wiiu_query_data *data);
void GX2QueryEnd(uint32_t type, wiiu_query_data *data);
BOOL GX2QueryGetOcclusionResult(wiiu_query_data *data, uint64_t *result);
void GX2SetPixelSamplerBorderColor(uint32_t unit, float red, float green, float blue, float alpha);
/* (gx2.def's exports; decaf's gx2_state.cpp: in milliseconds, 10 s after
GX2Init) */
uint32_t GX2GetGPUTimeout(void);
void GX2SetGPUTimeout(uint32_t milliseconds);

#define FIRST_USE() first_use(__func__)

/* ---------- the log */

static void first_use(const char *name)
{
	static const char *seen[160];
	static int seen_count;
	int index;

	for (index = 0; index < seen_count; index++)
	{
		if (seen[index] == name)
			return;
	}
	if (seen_count < (int)(sizeof(seen) / sizeof(seen[0])))
		seen[seen_count++] = name;
	wiiu_log("gl: %s (not implemented)", name);
}

/* Bring-up switches (WIIU_APP_FOLDER/gl.txt: one word each), in a build
configured with --wiiu-gl-switches (HALO_WIIU_GL_SWITCHES). A normal build
never reads the file, so every switch stays off. */
static struct
{
	int loaded;
	int log_draws, no_draws, no_cull, swap_winding, no_depth, no_blend, white_textures, log_frame, clear_red, blit_green, screen_magenta, trace, native_strips, no_mips, log_textures, log_big, dst_alpha_one, equal_as_lequal, skip_fog, packed_unsigned, math_cubes, cube_probe, native_cubes, native_shorts, no_gpu_check, unaligned_vertices, copy_vertices, linear_textures, no_aniso, only_tex, gpu_check, log_shaders;
	unsigned int only_tex_format;
	unsigned long log_at;
	GLuint skip_programs[8];
	int skip_count;
	unsigned long log_frame_number;
} debug;

/* frames whose every step the GPU is waited for (gpu_check) */
#define GPU_CHECK_FRAMES 2

/* what Cemu needed (STATUS.md, milestone 3), each on unless its switch is
in gl.txt, in the log once, so that a hardware run says which it ran with */
static void debug_describe(void)
{
	wiiu_log("gl: Cemu's workarounds: strips drawn as lists %s (native_strips), one short fetched as two %s "
		"(native_shorts), packed normals unpacked unsigned %s (packed_unsigned), cube maps read as arrays of faces %s "
		"(native_cubes)", debug.native_strips ? "off" : "on", debug.native_shorts ? "off" : "on",
		debug.packed_unsigned ? "off" : "on", debug.native_cubes ? "off" : "on");
	wiiu_log("gl: the GPU waited for after each step of the first %d frames that draw, and of each program's first "
		"draw: %s (gpu_check)", GPU_CHECK_FRAMES, debug.gpu_check ? "on" : "off");
	wiiu_log("gl: vertex buffers on GX2_VERTEX_BUFFER_ALIGNMENT %s (unaligned_vertices), every attribute copied to the "
		"ring %s (copy_vertices)", debug.unaligned_vertices ? "off" : "on", debug.copy_vertices ? "on" : "off");
	wiiu_log("gl: textures given texels in GX2's tiling, through GX2CopySurface %s (linear_textures)",
		debug.linear_textures ? "off" : "on");
	wiiu_log("gl: other switches to narrow a draw down (gl.txt, a word each): no_draws no_depth no_blend no_cull "
		"white_textures only_tex_format=0xNN no_aniso no_mips skip_program=N; to log more: log_frame=N log_draws "
		"log_textures log_shaders gpu_check");
	if (debug.only_tex)
		wiiu_log("gl: only textures of format 0x%x sampled (only_tex_format), the rest as nothing bound",
			debug.only_tex_format);
}

static void debug_load(void)
{
#ifdef HALO_WIIU_GL_SWITCHES
	FILE *file;
	char word[64];
#endif

	/* (once: it's asked for on every draw and texture, and each line of the
	description is a write to the SD card) */
	if (debug.loaded)
		return;
	debug.loaded = 1;
#ifdef HALO_WIIU_GL_SWITCHES
	file = fopen(WIIU_APP_FOLDER "/gl.txt", "rb");
	if (!file)
	{
		wiiu_log("gl: no gl.txt: every bring-up switch off");
		debug_describe();
		return;
	}
	while (fscanf(file, "%63s", word) == 1)
	{
		if (!strcmp(word, "log_draws")) debug.log_draws = 1;
		if (!strcmp(word, "no_draws")) debug.no_draws = 1;
		if (!strcmp(word, "no_cull")) debug.no_cull = 1;
		if (!strcmp(word, "swap_winding")) debug.swap_winding = 1;
		if (!strcmp(word, "no_depth")) debug.no_depth = 1;
		if (!strcmp(word, "no_blend")) debug.no_blend = 1;
		if (!strcmp(word, "white_textures")) debug.white_textures = 1;
		if (!strcmp(word, "clear_red")) debug.clear_red = 1;
		if (!strcmp(word, "blit_green")) debug.blit_green = 1;
		if (!strcmp(word, "trace")) debug.trace = 1;
		if (!strcmp(word, "native_strips")) debug.native_strips = 1;
		if (!strcmp(word, "no_mips")) debug.no_mips = 1;
		if (!strcmp(word, "log_big")) debug.log_big = 1;
		if (!strcmp(word, "dst_alpha_one")) debug.dst_alpha_one = 1;
		if (!strcmp(word, "lequal")) debug.equal_as_lequal = 1;
		if (!strcmp(word, "skip_fog")) debug.skip_fog = 1;
		if (!strcmp(word, "packed_unsigned")) debug.packed_unsigned = 1;
		if (!strcmp(word, "math_cubes")) debug.math_cubes = 1;
		if (!strcmp(word, "cube_probe")) debug.cube_probe = 1;
		if (!strcmp(word, "native_cubes")) debug.native_cubes = 1;
		if (!strcmp(word, "native_shorts")) debug.native_shorts = 1;
		if (!strcmp(word, "no_gpu_check")) debug.no_gpu_check = 1;
		if (!strcmp(word, "gpu_check")) debug.gpu_check = 1;
		if (!strcmp(word, "log_shaders")) debug.log_shaders = 1;
		if (!strcmp(word, "unaligned_vertices")) debug.unaligned_vertices = 1;
		if (!strcmp(word, "copy_vertices")) debug.copy_vertices = 1;
		if (!strcmp(word, "linear_textures")) debug.linear_textures = 1;
		if (!strcmp(word, "no_aniso")) debug.no_aniso = 1;
		if (sscanf(word, "only_tex_format=%x", &debug.only_tex_format) == 1)
			debug.only_tex = 1;
		sscanf(word, "log_at=%lu", &debug.log_at);
		if (debug.skip_count < 8 && sscanf(word, "skip_program=%u", &debug.skip_programs[debug.skip_count]) == 1)
			debug.skip_count++;
		if (!strcmp(word, "log_textures")) debug.log_textures = 1;
		if (!strcmp(word, "screen_magenta")) debug.screen_magenta = 1;
		if (sscanf(word, "log_frame=%lu", &debug.log_frame_number) == 1)
			debug.log_frame = 1;
	}
	fclose(file);
	wiiu_log("gl: debug switches read (draws %s, culling %s%s)", debug.no_draws ? "off" : "on",
		debug.no_cull ? "off" : "on", debug.swap_winding ? ", winding swapped" : "");
#endif
	debug_describe();
}

/* ---------- limits */

#define MAXIMUM_OBJECTS 16384
#define TEXTURE_UNITS 16
#define ATTRIBUTES 16
#define BINDINGS 16
#define MAXIMUM_LEVELS 14
/* the GPU's draw data of this layer's own (copies of attributes that are not
on a word, the generic attributes' values, blits' quads): two frames' worth */
#define RING_SIZE (8 * 1024 * 1024)
/* (the alignment GX2 asks of a vertex buffer: GX2_VERTEX_BUFFER_ALIGNMENT) */
#define BUFFER_ALIGNMENT 0x40
#define UNIFORM_COMPONENTS 1024

/* ---------- objects */

enum
{
	_object_none,
	_object_texture,
	_object_buffer,
	_object_framebuffer,
	_object_renderbuffer,
	_object_sampler,
	_object_shader,
	_object_program,
	_object_query,
	_object_vertex_array,
};

struct sampler_parameters
{
	GLint min_filter, mag_filter, wrap_s, wrap_t, wrap_r;
	GLfloat border[4], lod_bias, anisotropy, min_lod, max_lod;
};

struct texture
{
	GLenum target;
	GLenum internal_format;
	unsigned long width, height, depth;
	int levels;              /* the surface's */
	int defined_levels;      /* levels given texels (highest + 1) */
	GX2Texture gx2;
	void *memory;            /* the surface's image and mipmaps, one block */
	unsigned long bytes;
	int compressed;          /* 0, or DXT's bytes a block (8 or 16) */
	int tiled;               /* GX2's own tiling: written through a copy */
	int depth_format;
	GLint swizzle[4];
	GLint base_level, max_level;
	struct sampler_parameters sampler;
	/* the GPU drew into it since it was last sampled */
	int written;
	/* a framebuffer attachment: a color buffer for each level drawn into,
	or the depth buffer */
	GX2ColorBuffer *color_buffers[MAXIMUM_LEVELS];
	GX2DepthBuffer *depth_buffer;
	void *depth_memory;
	/* a cube map seen as an array of its six faces (cube_lookup) */
	GX2Texture array_view;
	int array_view_made;
	/* the GPU's time stamp by which its last draw that samples it is done
	(SDL's WIIU_TextureStartRendering): texels the CPU writes in place
	wait for it */
	OSTime used_stamp;
};

struct buffer
{
	unsigned char *data;
	unsigned long size;
	/* memory the buffer is an alias of (console_gl_buffer_alias: the Xbox
	window, whose vertices the GPU reads in place, little-endian) */
	int alias;
};

struct framebuffer
{
	GLuint color;
	GLuint depth;
	GLint color_level;
};

struct renderbuffer
{
	GLenum format;
	unsigned long width, height;
};

struct sampler
{
	struct sampler_parameters parameters;
};

/* a shader, compiled once; a vertex shader's inputs (bit n: location n) */
struct shader
{
	GLenum type;
	char *source;
	int compiled;
	int cached;              /* read from the SD card (wiiu_shader_cache.c) */
	GX2VertexShader *vertex;
	GX2PixelShader *pixel;
	unsigned long inputs;
	/* the texN samplers declared as cube maps and read as arrays of faces
	(bit N) */
	unsigned long cube_arrays;
	uint64_t key;
	char log[256];
	int attachments;
	int deleted;
};

#define MAXIMUM_UNIFORMS 64

/* a uniform: its offsets (32-bit components) in each stage's registers, or
-1, and its type; a sampler's unit and its slot in the pixel shader */
struct uniform
{
	char name[32];
	int vertex_offset, pixel_offset;
	int count;               /* elements */
	int integer;
	int sampler;
	int sampler_location;
	int sampler_slot;        /* the target it samples: 2D, 3D, cube */
	int cube_array;          /* a cube map read as an array of faces */
	GLint unit;
};

struct program
{
	GLuint vertex, fragment;
	int linked;
	struct uniform uniforms[MAXIMUM_UNIFORMS];
	int uniform_count;
	/* the register values, set before a draw (all of them when the program
	is newly in use, else the range written since) */
	uint32_t vertex_values[UNIFORM_COMPONENTS];
	uint32_t pixel_values[UNIFORM_COMPONENTS];
	int vertex_dirty_low, vertex_dirty_high, pixel_dirty_low, pixel_dirty_high;
	int deleted;
	int gpu_checked;         /* its first draw waited for (gpu_check_step) */
};

struct query
{
	wiiu_query_data *data;
	int issued;
	int available;
	GLuint result;
};

struct object
{
	int type;
	void *data;
};

static struct object objects[MAXIMUM_OBJECTS];
static GLuint next_name = 1;

static GLuint object_new(int type, void *data)
{
	GLuint name;

	for (name = next_name; name < MAXIMUM_OBJECTS; name++)
	{
		if (objects[name].type == _object_none)
			break;
	}
	if (name >= MAXIMUM_OBJECTS)
	{
		for (name = 1; name < MAXIMUM_OBJECTS && objects[name].type != _object_none; name++)
			;
		if (name >= MAXIMUM_OBJECTS)
		{
			wiiu_log("gl: out of object names");
			return 0;
		}
	}
	objects[name].type = type;
	objects[name].data = data;
	next_name = name + 1;
	return name;
}

static void *object_get(GLuint name, int type)
{
	if (!name || name >= MAXIMUM_OBJECTS || objects[name].type != type)
		return NULL;
	return objects[name].data;
}

static void generate(GLsizei count, GLuint *names, int type, size_t size)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		void *data = size ? calloc(1, size) : NULL;

		names[index] = object_new(type, data);
	}
}

/* ---------- state */

struct attribute
{
	int enabled;
	GLint size;
	GLenum type;
	int normalized, integer;
	GLuint relative_offset;
	GLuint binding;
	float value[4];
};

struct binding
{
	GLuint buffer;
	GLintptr offset;
	GLsizei stride;
};

static struct
{
	int ready;
	int drawing;             /* a frame drawn (the screens are the renderer's) */
	/* bindings */
	GLuint array_buffer, element_buffer, copy_write_buffer, copy_read_buffer;
	GLenum active_unit;
	GLuint textures[TEXTURE_UNITS][3];   /* 2D, 3D, cube */
	GLuint samplers[TEXTURE_UNITS];
	GLuint draw_framebuffer, read_framebuffer;
	GLuint renderbuffer;
	GLuint program;
	struct attribute attributes[ATTRIBUTES];
	struct binding bindings[BINDINGS];
	/* fixed-function state */
	int blend, depth_test, stencil_test, cull_face, scissor_test, polygon_offset_fill;
	GLenum blend_source, blend_destination, blend_source_alpha, blend_destination_alpha;
	GLenum blend_equation, blend_equation_alpha;
	float blend_color[4];
	GLenum depth_func;
	int depth_mask;
	GLenum stencil_func, stencil_fail, stencil_zfail, stencil_zpass;
	GLint stencil_reference;
	GLuint stencil_mask, stencil_write_mask;
	GLenum cull_mode, front_face;
	int color_mask[4];
	GLint viewport[4], scissor[4];
	float depth_near, depth_far;
	float polygon_offset_factor, polygon_offset_units;
	GLenum polygon_mode;
	float clear_color[4];
	float clear_depth;
	GLint clear_stencil;
	/* what GX2 has bound */
	GX2ColorBuffer *bound_color;
	GX2DepthBuffer *bound_depth;
	unsigned long target_width, target_height;
	int target_has_color, target_has_depth;
	struct texture *target_texture;
	GLuint uploaded_program;
	/* the ring: this layer's own draw data, two frames' worth */
	unsigned char *ring;
	unsigned long ring_offset;
	int frame_half;
	OSTime frame_stamps[2];
	/* textures read by samplers with nothing bound */
	struct texture *empty[3];
	/* the blit's and the clear's shaders */
	struct shader *blit_vertex, *blit_pixel, *clear_pixel;
	int blit_source_location, clear_color_offset;
	unsigned long frame;
	unsigned long draws;
	unsigned long frame_draws;
	OSTime start;
} gl;

static void state_defaults(void)
{
	int index;

	gl.blend_source = gl.blend_source_alpha = GL_ONE;
	gl.blend_destination = gl.blend_destination_alpha = GL_ZERO;
	gl.blend_equation = gl.blend_equation_alpha = GL_FUNC_ADD;
	gl.depth_func = GL_LESS;
	gl.depth_mask = 1;
	gl.stencil_func = GL_ALWAYS;
	gl.stencil_fail = gl.stencil_zfail = gl.stencil_zpass = GL_KEEP;
	gl.stencil_mask = gl.stencil_write_mask = 0xffffffff;
	gl.cull_mode = GL_BACK;
	gl.front_face = GL_CCW;
	gl.color_mask[0] = gl.color_mask[1] = gl.color_mask[2] = gl.color_mask[3] = 1;
	gl.depth_near = 0.0f;
	gl.depth_far = 1.0f;
	gl.polygon_mode = GL_FILL;
	gl.clear_depth = 1.0f;
	for (index = 0; index < ATTRIBUTES; index++)
	{
		gl.attributes[index].value[3] = 1.0f;
		gl.attributes[index].binding = (GLuint)index;
		gl.attributes[index].size = 4;
		gl.attributes[index].type = GL_FLOAT;
	}
}

/* ---------- GPU memory */

/* memory the GPU reads (MEM2, the default heap: SDL's and libwhb's
allocator for everything but the screens' buffers) */
static void *gpu_allocate(unsigned long size, unsigned long alignment)
{
	return memalign(alignment < 4 ? 4 : alignment, size ? size : 4);
}

/* the CPU's writes to [data, data + size) out of its cache, for the GPU to
read as mode says (GX2Invalidate with a CPU mode flushes the range first:
decaf's gx2_mem.cpp) */
static void gpu_written(const void *data, unsigned long size, GX2InvalidateMode mode)
{
	if (data && size)
		GX2Invalidate(mode, (void *)data, size);
}

/* After GX2's own copies and clears (GX2CopySurface, GX2ClearColor,
GX2ClearDepthStencilEx): the context state set again and everything this
layer keeps bound set again at the next draw. On the console these run as
draws of GX2's own, with its own targets and shaders: libwhb sets the
context state again after its clears (gfx.c, WHBGfxClearColor), and Cemu's
GX2CopySurface notes that the real one sets up draw calls where Cemu's is a
command of its own (GX2_Surface_Copy.cpp), which is why Cemu drew right
while a copy in the middle of a frame left the console drawing the rest of
that frame into the texture just copied. */
/* The last draw, for console_gl_draw_again (the game's d3d8_gl.c draws
the lens flares' visibility tests after the first with the first one's
state): valid while GX2 has what that draw gave it, its shaders, state,
targets, textures and attribute buffers, and those read the GL buffers where
they are (no copy made for that draw's vertices alone). GX2's own work, a
target bound, a uniform set, a blit or the frame's end ends it. */
static struct
{
	int valid;
	struct program *program;
} again;

static void gx2_own_work_done(void)
{
	GX2ContextState *context = wiiu_video_context_state();

	again.valid = 0;
	if (context)
		GX2SetContextState(context);
	gl.bound_color = NULL;
	gl.bound_depth = NULL;
	gl.uploaded_program = 0;
}

/* ---------- the waits, timed, and the game's thread watched

Evidence for hardware runs, where nothing else shows what a frame spends its
time on. GX2's waits for the GPU (GX2DrawDone, GX2WaitTimeStamp) give up
after GX2's GPU timeout, 10 s from GX2Init, and return FALSE (decaf-emu's
gx2_state.cpp and gx2_cbpool.cpp): a GPU that has stopped makes each frame
take that long while the game runs on, its input read and its sound starved.
So every wait is timed and a wait that gave up is logged; the first frames
that draw, and each program's first draw, wait for the GPU after each step
(gpu_check), to name the first step it does not finish; the frames' times go to the log every 2 s for the
first 2 minutes; and a thread of its own logs where the game's thread is when
it stays in one place for more than 3 s. */

static struct
{
	/* since the last line in the log */
	unsigned long frames, gpu_waits, gpu_timeouts, queries, query_timeouts, compiles, cache_reads, cache_writes;
	OSTime period_start, frame_start, longest_frame, gpu_wait, query_wait, compile, cache_read, cache_write;
	/* the textures given texels (glTexImage, glTexSubImage, glGenerateMipmap)
	and the time it took, and the in-place writes that waited for the GPU */
	unsigned long textures, texture_waits;
	OSTime texture_time;
	/* where the longest frame's time went: the totals at its start, and
	the longest's share of each (GPU waits, textures, shaders, the log's
	SD card writes, the flip) */
	OSTime base_gpu, base_texture, base_shader, base_log, base_flip;
	OSTime longest_gpu, longest_texture, longest_shader, longest_log, longest_flip, period_log;
	/* gpu_check's */
	int checking, checked_frames, check_failed;
	unsigned long checks;
	uint32_t timeout_saved;
	OSTime longest_check;
	char last_finished[96];
	/* the watch thread's */
	const char *volatile step;
	volatile OSTime step_time;
	volatile int stop;
	OSThread *thread;
	void *stack;
} watch;

static void watch_step(const char *step)
{
	watch.step = step;
	watch.step_time = OSGetSystemTime();
}

void wiiu_gl_step(const char *step)
{
	watch_step(step);
}

/* the GPU waited for, up to stamp; 0 if GX2 gave up */
static int gpu_wait(OSTime stamp, const char *what)
{
	OSTime start = OSGetSystemTime(), took;
	const char *was = watch.step;
	BOOL done;

	watch_step(what);
	done = GX2WaitTimeStamp(stamp);
	took = OSGetSystemTime() - start;
	watch.gpu_wait += took;
	watch.gpu_waits++;
	if (!done)
	{
		static unsigned long timeouts;

		watch.gpu_timeouts++;
		if (timeouts++ < 8)
			wiiu_log("gl: the GPU did not finish (%s): GX2 gave up after %lu ms; frame %lu, draw %lu of the frame, "
				"retired %llu of %llu submitted", what, (unsigned long)OSTicksToMilliseconds(took), gl.frame,
				gl.frame_draws, (unsigned long long)GX2GetRetiredTimeStamp(),
				(unsigned long long)GX2GetLastSubmittedTimeStamp());
	}
	watch.step = was ? was : "the renderer";
	return done;
}

/* (GX2DrawDone's own: decaf's gx2_event.cpp) */
static int gpu_done(const char *what)
{
	GX2Flush();
	return gpu_wait(GX2GetLastSubmittedTimeStamp(), what);
}

/* the frames gpu_check covers begin at their first draw (and a program's
first draw is checked in any frame: gpu_check_step) */
static void gpu_check_arm(void)
{
	if (watch.checking || watch.checked_frames >= GPU_CHECK_FRAMES || !debug.gpu_check || debug.no_gpu_check ||
		!gl.ready)
		return;
	watch.checking = 1;
	watch.checks = 0;
	watch.longest_check = 0;
	strcpy(watch.last_finished, "the frame's start");
	/* (a stopped GPU found in a second, not ten) */
	watch.timeout_saved = GX2GetGPUTimeout();
	GX2SetGPUTimeout(1000);
	wiiu_log("gl: frame %lu: the GPU waited for after each step", gl.frame);
}

/* after a step of a checked frame, or a program's first draw (always): the
GPU let finish it; the first it does not finish is logged, and the next
frame's draws with it (log_frame) */
static void gpu_check_step(const char *step, int always)
{
	OSTime start;
	uint32_t timeout = 0;
	int done;

	if (watch.check_failed || !debug.gpu_check || debug.no_gpu_check || (!watch.checking && !always) || !gl.ready)
		return;
	if (!watch.checking)
	{
		timeout = GX2GetGPUTimeout();
		GX2SetGPUTimeout(1000);
	}
	start = OSGetSystemTime();
	watch.checks++;
	done = gpu_done(step);
	if (timeout)
		GX2SetGPUTimeout(timeout);
	if (!done)
	{
		watch.check_failed = 1;
		wiiu_log("gl: the GPU stopped at frame %lu's %s (draw %lu of the frame, program %u, framebuffer %u %lux%lu, "
			"depth test %d, blend %d); the last step it finished: %s", gl.frame, step, gl.frame_draws, gl.program,
			gl.draw_framebuffer, gl.target_width, gl.target_height, gl.depth_test, gl.blend,
			watch.last_finished[0] ? watch.last_finished : "none checked yet");
		debug.log_frame = 1;
		debug.log_frame_number = gl.frame + 1;
		return;
	}
	if (OSGetSystemTime() - start > watch.longest_check)
		watch.longest_check = OSGetSystemTime() - start;
	snprintf(watch.last_finished, sizeof(watch.last_finished), "%s (frame %lu, draw %lu of the frame, program %u)", step,
		gl.frame, gl.frame_draws, gl.program);
}

static void gpu_check(const char *step)
{
	gpu_check_step(step, 0);
}

static void gpu_check_frame_end(void)
{
	if (!watch.checking)
		return;
	watch.checking = 0;
	watch.checked_frames++;
	GX2SetGPUTimeout(watch.timeout_saved);
	if (!watch.check_failed)
		wiiu_log("gl: frame %lu: the GPU finished each of its %lu steps (%lu draws), the longest in %lu us", gl.frame,
			watch.checks, gl.frame_draws, (unsigned long)OSTicksToMicroseconds(watch.longest_check));
}

/* (every frame's end: a line every 2 s for the first 2 minutes, every
300 frames after) */
static void watch_frame_end(void)
{
	OSTime now = OSGetSystemTime();
	OSTime vsync_wait = 0;
	OSTime shader = watch.compile + watch.cache_read + watch.cache_write;
	OSTime log_time = (OSTime)wiiu_log_write_ticks(), flip = wiiu_video_wait_ticks();
	unsigned long vsync_timeouts = 0, elapsed;
	unsigned long period_ms = OSTicksToMilliseconds(now - gl.start) < 120000 ? 2000 : 30000;

	if (watch.frame_start && now - watch.frame_start > watch.longest_frame)
	{
		watch.longest_frame = now - watch.frame_start;
		watch.longest_gpu = watch.gpu_wait - watch.base_gpu;
		watch.longest_texture = watch.texture_time - watch.base_texture;
		watch.longest_shader = shader - watch.base_shader;
		watch.longest_log = log_time - watch.base_log;
		watch.longest_flip = flip - watch.base_flip;
	}
	watch.frame_start = now;
	watch.base_gpu = watch.gpu_wait;
	watch.base_texture = watch.texture_time;
	watch.base_shader = shader;
	watch.base_log = log_time;
	watch.base_flip = flip;
	watch.frames++;
	if (!watch.period_start)
		watch.period_start = now;
	elapsed = (unsigned long)OSTicksToMilliseconds(now - watch.period_start);
	/* (every 2 s for the first 2 minutes, then every 30 s) */
	if (elapsed < period_ms)
		return;
	wiiu_video_take_waits(&vsync_wait, &vsync_timeouts);
	wiiu_log("gl: frame %lu: %lu frames in %lu ms (the longest %lu ms), %lu draws in the last; waited: GPU %lu ms "
		"(%lu waits, %lu gave up), flips %lu ms (%lu gave up), queries %lu ms (%lu, %lu gave up); shaders: %lu "
		"compiled in %lu ms, %lu read in %lu ms, %lu written in %lu ms", gl.frame, watch.frames, elapsed,
		(unsigned long)OSTicksToMilliseconds(watch.longest_frame), gl.frame_draws,
		(unsigned long)OSTicksToMilliseconds(watch.gpu_wait), watch.gpu_waits, watch.gpu_timeouts,
		(unsigned long)OSTicksToMilliseconds(vsync_wait), vsync_timeouts,
		(unsigned long)OSTicksToMilliseconds(watch.query_wait), watch.queries, watch.query_timeouts, watch.compiles,
		(unsigned long)OSTicksToMilliseconds(watch.compile), watch.cache_reads,
		(unsigned long)OSTicksToMilliseconds(watch.cache_read), watch.cache_writes,
		(unsigned long)OSTicksToMilliseconds(watch.cache_write));
	wiiu_log("gl:   the longest frame: GPU waits %lu ms, textures %lu ms, shaders %lu ms, the log %lu ms, the flip %lu "
		"ms; in all: %lu textures given texels in %lu ms (%lu written in place waited for the GPU), the log's SD card "
		"writes %lu ms",
		(unsigned long)OSTicksToMilliseconds(watch.longest_gpu), (unsigned long)OSTicksToMilliseconds(watch.longest_texture),
		(unsigned long)OSTicksToMilliseconds(watch.longest_shader), (unsigned long)OSTicksToMilliseconds(watch.longest_log),
		(unsigned long)OSTicksToMilliseconds(watch.longest_flip), watch.textures,
		(unsigned long)OSTicksToMilliseconds(watch.texture_time), watch.texture_waits,
		(unsigned long)OSTicksToMilliseconds(log_time - watch.period_log));
	watch.period_log = log_time;
	watch.textures = watch.texture_waits = 0;
	watch.texture_time = 0;
	watch.base_texture = 0;
	watch.frames = watch.gpu_waits = watch.gpu_timeouts = watch.queries = watch.query_timeouts = 0;
	watch.compiles = watch.cache_reads = watch.cache_writes = 0;
	watch.gpu_wait = watch.query_wait = watch.compile = watch.cache_read = watch.cache_write = 0;
	watch.longest_frame = 0;
	watch.base_gpu = 0;
	watch.base_shader = 0;
	watch.period_start = now;
}

/* the watch thread: where the game's thread has stayed for more than 3 s,
while the game has the foreground (the HOME button menu's wait is not
logged), once, then every 10 s */
static int watch_main(int argc, const char **argv)
{
	const char *logged = NULL;
	OSTime logged_at = 0;

	(void)argc;
	(void)argv;
	while (!watch.stop)
	{
		const char *step;
		OSTime since, now;

		OSSleepTicks(OSMillisecondsToTicks(500));
		step = watch.step;
		since = watch.step_time;
		now = OSGetSystemTime();
		if (!step || !wiiu_video_drawing() || now - since < OSSecondsToTicks(3))
			continue;
		if (step == logged && now - logged_at < OSSecondsToTicks(10))
			continue;
		wiiu_log("gl: watch: the game's thread in %s for %lu s (frame %lu, draw %lu of the frame)", step,
			(unsigned long)(OSTicksToMilliseconds(now - since) / 1000), gl.frame, gl.frame_draws);
		logged = step;
		logged_at = now;
	}
	return 0;
}

static void watch_stop(void)
{
	if (!watch.thread)
		return;
	watch.stop = 1;
	OSJoinThread(watch.thread, NULL);
	watch.thread = NULL;
}

static void watch_start(void)
{
	const uint32_t stack_size = 0x4000;

	if (watch.thread)
		return;
	watch.thread = memalign(16, sizeof(OSThread));
	watch.stack = memalign(16, stack_size);
	if (!watch.thread || !watch.stack ||
		!OSCreateThread(watch.thread, watch_main, 0, NULL, (unsigned char *)watch.stack + stack_size, stack_size, 17,
		OS_THREAD_ATTRIB_AFFINITY_ANY))
	{
		free(watch.thread);
		free(watch.stack);
		watch.thread = NULL;
		wiiu_log("gl: the watch thread not started");
		return;
	}
	OSSetThreadName(watch.thread, "gl watch");
	OSResumeThread(watch.thread);
	/* (ended before proc_end, which atexit took first) */
	atexit(watch_stop);
}

/* memory the GPU may still read, let go once the frame that last used it
is done (wiiu_gl_frame_end): two lists, this frame's and the last */
#define RETIRED_MAXIMUM 512

enum
{
	_retired_memory,
	_retired_vertex_shader,
	_retired_pixel_shader,
	_retired_cached_vertex_shader,
	_retired_cached_pixel_shader,
};

static struct
{
	void *items[2][RETIRED_MAXIMUM];
	int kinds[2][RETIRED_MAXIMUM];
	int count[2];
} retired;

static void retired_release(void *item, int kind)
{
	switch (kind)
	{
#ifdef HALO_WIIU_CAFEGLSL
	case _retired_vertex_shader: FreeVertexShader(item); break;
	case _retired_pixel_shader: FreePixelShader(item); break;
#endif
	case _retired_cached_vertex_shader: wiiu_shader_cache_free(1, item); break;
	case _retired_cached_pixel_shader: wiiu_shader_cache_free(0, item); break;
#ifdef HALO_WIIU_CAFEGLSL
#endif
	default: free(item); break;
	}
}

static void retired_free(int half)
{
	int index;

	for (index = 0; index < retired.count[half]; index++)
		retired_release(retired.items[half][index], retired.kinds[half][index]);
	retired.count[half] = 0;
}

static void retire(void *item, int kind)
{
	int half = gl.frame_half;

	if (!item)
		return;
	if (!gl.ready)
	{
		retired_release(item, kind);
		return;
	}
	if (retired.count[half] == RETIRED_MAXIMUM)
	{
		/* (more than a frame holds: the GPU let finish, then all of them
		let go) */
		gpu_done("the GPU let finish to free memory");
		retired_free(half);
		retired_free(half ^ 1);
	}
	retired.items[half][retired.count[half]] = item;
	retired.kinds[half][retired.count[half]++] = kind;
}

/* ---------- the ring */

static void *ring_reserve(unsigned long size)
{
	unsigned long half = RING_SIZE / 2;
	unsigned long start = (unsigned long)gl.frame_half * half;
	unsigned long aligned = (gl.ring_offset + BUFFER_ALIGNMENT - 1) & ~(unsigned long)(BUFFER_ALIGNMENT - 1);

	if (aligned < start)
		aligned = start;
	if (aligned + size > start + half)
	{
		static int warned;

		if (!warned)
		{
			warned = 1;
			wiiu_log("gl: a frame's draw data passed %lu MB; the GPU is waited for", half >> 20);
		}
		gpu_done("the GPU let finish for the draw data ring");
		aligned = start;
		if (size > half)
			return NULL;
	}
	gl.ring_offset = aligned + size;
	return gl.ring + aligned;
}

/* ---------- textures */

static GLuint *bound_texture_slot(GLenum target)
{
	int unit = (int)gl.active_unit;

	if (unit < 0 || unit >= TEXTURE_UNITS)
		unit = 0;
	switch (target)
	{
	case GL_TEXTURE_3D: return &gl.textures[unit][1];
	case GL_TEXTURE_CUBE_MAP:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_X: case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Y: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
	case GL_TEXTURE_CUBE_MAP_POSITIVE_Z: case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
		return &gl.textures[unit][2];
	default: return &gl.textures[unit][0];
	}
}

static struct texture *bound_texture(GLenum target)
{
	return object_get(*bound_texture_slot(target), _object_texture);
}

static void sampler_defaults(struct sampler_parameters *sampler)
{
	sampler->min_filter = GL_NEAREST_MIPMAP_LINEAR;
	sampler->mag_filter = GL_LINEAR;
	sampler->wrap_s = sampler->wrap_t = sampler->wrap_r = GL_REPEAT;
	sampler->anisotropy = 1.0f;
	sampler->min_lod = -1000.0f;
	sampler->max_lod = 1000.0f;
}

static void texture_defaults(struct texture *texture)
{
	sampler_defaults(&texture->sampler);
	texture->max_level = 1000;
	texture->swizzle[0] = GL_RED;
	texture->swizzle[1] = GL_GREEN;
	texture->swizzle[2] = GL_BLUE;
	texture->swizzle[3] = GL_ALPHA;
}

/* the texture's memory and attachments let go (two frames on: a queued
draw may read them) */
static void texture_release(struct texture *texture)
{
	int level;

	retire(texture->memory, _retired_memory);
	texture->memory = NULL;
	for (level = 0; level < MAXIMUM_LEVELS; level++)
	{
		if (texture->color_buffers[level])
		{
			if (gl.bound_color == texture->color_buffers[level])
				gl.bound_color = NULL;
			retire(texture->color_buffers[level], _retired_memory);
			texture->color_buffers[level] = NULL;
		}
	}
	if (texture->depth_buffer)
	{
		if (gl.bound_depth == texture->depth_buffer)
			gl.bound_depth = NULL;
		retire(texture->depth_buffer, _retired_memory);
		retire(texture->depth_memory, _retired_memory);
		texture->depth_buffer = NULL;
		texture->depth_memory = NULL;
	}
	if (gl.target_texture == texture)
		gl.target_texture = NULL;
	texture->defined_levels = 0;
	texture->bytes = 0;
}

static int full_levels(unsigned long width, unsigned long height, unsigned long depth)
{
	int levels = 1;

	while ((width > 1 || height > 1 || depth > 1) && levels < MAXIMUM_LEVELS)
	{
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
		depth = depth > 1 ? depth / 2 : 1;
		levels++;
	}
	return levels;
}

static unsigned long level_size(unsigned long size, int level)
{
	size >>= level;
	return size ? size : 1;
}

/* the GX2 format for a GL internal format; DXT's bytes a block in
*compressed */
static GX2SurfaceFormat surface_format(GLenum internal, int *compressed)
{
	*compressed = 0;
	switch (internal)
	{
	case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
		*compressed = 8;
		return GX2_SURFACE_FORMAT_UNORM_BC1;
	case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
		*compressed = 16;
		return GX2_SURFACE_FORMAT_UNORM_BC2;
	case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
		*compressed = 16;
		return GX2_SURFACE_FORMAT_UNORM_BC3;
	case GL_R8:
		return GX2_SURFACE_FORMAT_UNORM_R8;
	default:
		/* (every other texture the renderer makes is RGBA8, its texels given
		as RGBA or BGRA bytes, kept RGBA) */
		return GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	}
}

static unsigned long texel_bytes(const struct texture *texture)
{
	return texture->internal_format == GL_R8 ? 1 : 4;
}

static uint32_t swizzle_select(GLint swizzle)
{
	switch (swizzle)
	{
	case GL_RED: return GX2_SQ_SEL_R;
	case GL_GREEN: return GX2_SQ_SEL_G;
	case GL_BLUE: return GX2_SQ_SEL_B;
	case GL_ALPHA: return GX2_SQ_SEL_A;
	case GL_ZERO: return GX2_SQ_SEL_0;
	default: return GX2_SQ_SEL_1;
	}
}

/* the texture's channel selects (GL_TEXTURE_SWIZZLE_*: the renderer's Halo
PC meters and multipurpose maps), made into GX2's registers again */
static void texture_component_map(struct texture *texture)
{
	if (!texture->memory)
		return;
	if (texture->internal_format == GL_R8)
		texture->gx2.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
	else
		texture->gx2.compMap = GX2_COMP_MAP(swizzle_select(texture->swizzle[0]), swizzle_select(texture->swizzle[1]),
			swizzle_select(texture->swizzle[2]), swizzle_select(texture->swizzle[3]));
	GX2InitTextureRegs(&texture->gx2);
	texture->array_view_made = 0;
}

/* (re)makes the texture's surface for this size and format; given_texels:
the texture is made with its texels (glTexImage with pixels), not to be
drawn into or written a part at a time */
static int texture_allocate(struct texture *texture, GLenum target, GLenum internal, unsigned long width,
	unsigned long height, unsigned long depth, int given_texels)
{
	int compressed;
	GX2SurfaceFormat format = surface_format(internal, &compressed);
	int levels = full_levels(width, height, target == GL_TEXTURE_3D ? depth : 1);
	int depth_format = internal == GL_DEPTH24_STENCIL8 || internal == GL_DEPTH_COMPONENT24 ||
		internal == GL_DEPTH_COMPONENT || internal == GL_DEPTH_COMPONENT32F;
	GX2Surface *surface = &texture->gx2.surface;
	unsigned long total;

	debug_load();
	if ((texture->memory || texture->depth_format) && texture->width == width && texture->height == height &&
		texture->depth == depth && texture->internal_format == internal && texture->target == target)
	{
		return 1;
	}
	texture_release(texture);
	texture->target = target;
	texture->internal_format = internal;
	texture->width = width;
	texture->height = height;
	texture->depth = depth;
	texture->compressed = compressed;
	texture->levels = levels;
	texture->depth_format = depth_format;
	if (depth_format)
		return 1;
	memset(&texture->gx2, 0, sizeof(texture->gx2));
	surface->dim = target == GL_TEXTURE_3D ? GX2_SURFACE_DIM_TEXTURE_3D : target == GL_TEXTURE_CUBE_MAP ?
		GX2_SURFACE_DIM_TEXTURE_CUBE : GX2_SURFACE_DIM_TEXTURE_2D;
	surface->width = width;
	surface->height = height;
	surface->depth = target == GL_TEXTURE_3D ? depth : target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
	surface->mipLevels = (uint32_t)levels;
	surface->format = format;
	surface->aa = GX2_AA_MODE1X;
	/* (a color buffer too when it is RGBA8: any texture can be drawn into) */
	surface->use = compressed ? GX2_SURFACE_USE_TEXTURE : GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
	/* (cube maps and volumes in GX2's own tiling, as games have them: their
	texels go in through a linear copy, texture_upload; Cemu read the
	linear cube maps' texels as black. So are DXT textures and textures made
	with their texels, the map's bitmaps: the console's GPU stopped at the
	first draw that sampled them linear, mipmapped and DXT, while 1x1 linear
	textures of one level drew fine. Linear textures on the console are as
	SDL's and Butter and Jelly's are, one level of RGBA8, R8 or R8G8; DXT
	and the levels below the first are only ever tiled there. Textures made
	empty, to be drawn into or written a part at a time, stay linear.) */
	texture->tiled = target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_3D ||
		(!debug.linear_textures && (compressed || given_texels));
	surface->tileMode = texture->tiled ? GX2_TILE_MODE_DEFAULT : GX2_TILE_MODE_LINEAR_ALIGNED;
	GX2CalcSurfaceSizeAndAlignment(surface);
	/* (level 1 is mipLevelOffset[0] after the image when the levels follow
	it: GX2CalcSurfaceSizeAndAlignment's offset0) */
	total = surface->mipLevels > 1 ? surface->mipLevelOffset[0] + surface->mipmapSize : surface->imageSize;
	texture->memory = gpu_allocate(total, surface->alignment);
	if (!texture->memory)
	{
		wiiu_log("gl: no memory for a %lux%lux%lu texture (%lu bytes)", width, height, depth, total);
		return 0;
	}
	/* (levels never given texels read as zeros, not the heap's leftovers) */
	memset(texture->memory, 0, total);
	gpu_written(texture->memory, total, GX2_INVALIDATE_MODE_CPU_TEXTURE);
	texture->bytes = total;
	surface->image = texture->memory;
	surface->mipmaps = surface->mipLevels > 1 ? (unsigned char *)texture->memory + surface->mipLevelOffset[0] : NULL;
	texture->gx2.viewFirstMip = 0;
	texture->gx2.viewNumMips = surface->mipLevels;
	texture->gx2.viewFirstSlice = 0;
	texture->gx2.viewNumSlices = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
	texture->levels = (int)surface->mipLevels;
	texture_component_map(texture);
	debug_load();
	{
		/* (the first textures' surfaces, as GX2CalcSurfaceSizeAndAlignment
		made them, in every run) */
		static int described;

		if (debug.log_textures && described++ < 24)
			wiiu_log("gl: texture %lux%lux%lu target 0x%x format 0x%x, tile mode %d (asked %s), use 0x%x, %u levels, "
				"pitch %u, alignment 0x%x, image %u bytes at %p (0x%x past the alignment), mipmaps %u bytes at %p "
				"(0x%x past 0x100), swizzle 0x%x, offsets %u %u %u", width, height, depth, target, (unsigned int)format,
				(int)surface->tileMode, texture->tiled ? "default" : "linear aligned", (unsigned int)surface->use,
				(unsigned int)surface->mipLevels, (unsigned int)surface->pitch, (unsigned int)surface->alignment,
				(unsigned int)surface->imageSize, surface->image,
				(unsigned int)((uintptr_t)surface->image % (surface->alignment ? surface->alignment : 1)),
				(unsigned int)surface->mipmapSize, surface->mipmaps, (unsigned int)((uintptr_t)surface->mipmaps & 0xff),
				(unsigned int)surface->swizzle, (unsigned int)surface->mipLevelOffset[0],
				(unsigned int)surface->mipLevelOffset[1], (unsigned int)surface->mipLevelOffset[2]);
	}
	if (debug.log_textures)
	{
		static int logged;

		if (logged++ < 40)
		{
			int level;
			char text[160];
			int length = 0;

			for (level = 0; level < texture->levels && level < 6; level++)
				length += snprintf(text + length, sizeof(text) - (size_t)length, " %u/%u",
					(unsigned int)GX2GetSurfaceMipPitch(surface, (uint32_t)level),
					(unsigned int)GX2GetSurfaceMipSliceSize(surface, (uint32_t)level));
			wiiu_log("gl: texture %lux%lux%lu format 0x%x target 0x%x, %d levels, pitch %u, image %u, offsets %u %u %u; "
				"pitch/slice by level:%s", width, height, depth, (unsigned int)format, target, texture->levels,
				(unsigned int)surface->pitch, (unsigned int)surface->imageSize, (unsigned int)surface->mipLevelOffset[0],
				(unsigned int)surface->mipLevelOffset[1], (unsigned int)surface->mipLevelOffset[2], text);
		}
	}
	return 1;
}

/* a level's memory, its pitch (bytes a row of texels or of DXT blocks) and
its slices' size */
static unsigned char *texture_level(struct texture *texture, int level, unsigned long *pitch, unsigned long *slice)
{
	GX2Surface *surface = &texture->gx2.surface;
	unsigned long element_bytes = texture->compressed ? (unsigned long)texture->compressed : texel_bytes(texture);
	uint32_t elements = GX2GetSurfaceMipPitch(surface, (uint32_t)level);
	unsigned char *base;

	if (!elements)
	{
		/* (no answer: a linear surface's pitch, 64 elements' alignment) */
		unsigned long width = level_size(texture->width, level);

		if (texture->compressed)
			width = (width + 3) / 4;
		elements = (uint32_t)((width + 63) & ~63UL);
	}
	*pitch = elements * element_bytes;
	*slice = GX2GetSurfaceMipSliceSize(surface, (uint32_t)level);
	if (!*slice)
	{
		unsigned long rows = level_size(texture->height, level);

		if (texture->compressed)
			rows = (rows + 3) / 4;
		*slice = *pitch * rows;
	}
	if (level == 0)
		base = surface->image;
	else if (level == 1)
		base = surface->mipmaps;
	else
		base = (unsigned char *)surface->mipmaps + surface->mipLevelOffset[level - 1];
	return base;
}

/* rows of texels or of DXT blocks copied, BGRA's swapped to RGBA */
static void texture_rows(unsigned char *destination, unsigned long pitch, const unsigned char *source,
	unsigned long rows, unsigned long row_bytes, unsigned long width, int bgra)
{
	unsigned long row;

	for (row = 0; row < rows; row++)
	{
		unsigned char *out = destination + row * pitch;

		if (bgra)
		{
			unsigned long texel;

			for (texel = 0; texel < width; texel++)
			{
				out[texel * 4 + 0] = source[texel * 4 + 2];
				out[texel * 4 + 1] = source[texel * 4 + 1];
				out[texel * 4 + 2] = source[texel * 4 + 0];
				out[texel * 4 + 3] = source[texel * 4 + 3];
			}
		}
		else
			memcpy(out, source, row_bytes);
		source += row_bytes;
	}
}

/* a tiled texture's level: each slice (a cube map's face, a volume's
slice) written into a linear surface of its own and copied into place by
GX2CopySurface, which tiles it (decaf's gx2_surface.cpp and gx2_addrlib.cpp:
the copy goes through the address library's tiling of both surfaces). The
linear surface is let go two frames on, as the GPU may copy later. A whole
level at a time: the renderer writes cube maps and volumes no other way. */
static void texture_upload_tiled(struct texture *texture, int level, unsigned long z, unsigned long width,
	unsigned long height, unsigned long depth, const void *data, GLenum format)
{
	unsigned long block = texture->compressed ? 4 : 1;
	unsigned long element_bytes = texture->compressed ? (unsigned long)texture->compressed : texel_bytes(texture);
	unsigned long rows = (height + block - 1) / block;
	unsigned long row_bytes = (width + block - 1) / block * element_bytes;
	int bgra = !texture->compressed && format == GL_BGRA && element_bytes == 4;
	const unsigned char *source = data;
	unsigned long slice;

	for (slice = 0; slice < (depth ? depth : 1); slice++)
	{
		GX2Surface staging;
		void *memory;

		memset(&staging, 0, sizeof(staging));
		staging.dim = GX2_SURFACE_DIM_TEXTURE_2D;
		staging.width = (uint32_t)width;
		staging.height = (uint32_t)height;
		staging.depth = 1;
		staging.mipLevels = 1;
		staging.format = texture->gx2.surface.format;
		staging.aa = GX2_AA_MODE1X;
		staging.use = GX2_SURFACE_USE_TEXTURE;
		staging.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
		GX2CalcSurfaceSizeAndAlignment(&staging);
		memory = gpu_allocate(staging.imageSize, staging.alignment);
		if (!memory)
			return;
		staging.image = memory;
		texture_rows(memory, staging.pitch * element_bytes, source, rows, row_bytes, width, bgra);
		gpu_written(memory, staging.imageSize, GX2_INVALIDATE_MODE_CPU_TEXTURE);
		GX2CopySurface(&staging, 0, 0, &texture->gx2.surface, (uint32_t)level, (uint32_t)(z + slice));
		gx2_own_work_done();
		gpu_check("a texture's copy into GX2's tiling");
		retire(memory, _retired_memory);
		source += rows * row_bytes;
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, texture->memory, texture->bytes);
	if (level + 1 > texture->defined_levels)
		texture->defined_levels = level + 1;
}

static void texture_flush_drawn(struct texture *texture);

/* (SDL's WIIU_TextureStartRendering: "whichever timestamp will be done
after" the draw, as the draw's own is not submitted yet) */
static void texture_used(struct texture *texture)
{
	texture->used_stamp = (OSTime)GX2GetLastSubmittedTimeStamp() + 1;
}

/* before the CPU writes a texture's texels in place: the draws already
queued that sample it let finish, so that none reads them half written
(SDL's WIIU_SDL_LockTexture and WIIU_TextureWaitDone: GX2Flush, then
GX2WaitTimeStamp) */
static void texture_wait_unused(struct texture *texture)
{
	if (!texture->used_stamp || (OSTime)GX2GetRetiredTimeStamp() >= texture->used_stamp)
		return;
	watch.texture_waits++;
	GX2Flush();
	gpu_wait(texture->used_stamp, "the GPU let finish with a texture written in place");
}

/* A tiled 2D texture's levels changed by the CPU (a part of a level written,
glGenerateMipmap): its levels copied into a linear twin, the same size and
format, by GX2CopySurface (which untiles them), changed there and copied
back. The twin's memory is let go two frames on. */
static int texture_twin_make(struct texture *texture, struct texture *twin, int first, int last)
{
	int level;

	memset(twin, 0, sizeof(*twin));
	texture_defaults(twin);
	if (!texture_allocate(twin, GL_TEXTURE_2D, texture->internal_format, texture->width, texture->height, 1, 0) ||
		twin->tiled)
	{
		texture_release(twin);
		return 0;
	}
	texture_flush_drawn(texture);
	for (level = first; level <= last && level < texture->levels; level++)
		GX2CopySurface(&texture->gx2.surface, (uint32_t)level, 0, &twin->gx2.surface, (uint32_t)level, 0);
	gx2_own_work_done();
	GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, twin->memory, twin->bytes);
	gpu_done("the GPU let finish to untile a texture's levels");
	DCInvalidateRange(twin->memory, twin->bytes);
	twin->defined_levels = texture->defined_levels;
	return 1;
}

static void texture_twin_commit(struct texture *texture, struct texture *twin, int first, int last)
{
	int level;

	gpu_written(twin->memory, twin->bytes, GX2_INVALIDATE_MODE_CPU_TEXTURE);
	for (level = first; level <= last && level < texture->levels; level++)
		GX2CopySurface(&twin->gx2.surface, (uint32_t)level, 0, &texture->gx2.surface, (uint32_t)level, 0);
	gx2_own_work_done();
	GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, texture->memory, texture->bytes);
	gpu_check("a texture's levels copied back into GX2's tiling");
	if (twin->defined_levels > texture->defined_levels)
		texture->defined_levels = twin->defined_levels;
	texture_release(twin);
}

/* copies texels into a level (rows of texels, or of DXT blocks), BGRA's
swapped to the surface's RGBA */
static void texture_upload(struct texture *texture, GLenum target, int level, unsigned long x, unsigned long y,
	unsigned long z, unsigned long width, unsigned long height, unsigned long depth, const void *data, GLenum format)
{
	unsigned long block = texture->compressed ? 4 : 1;
	unsigned long element_bytes = texture->compressed ? (unsigned long)texture->compressed : texel_bytes(texture);
	unsigned long rows = (height + block - 1) / block;
	unsigned long row_bytes = (width + block - 1) / block * element_bytes;
	unsigned long pitch, slice_bytes, slice, row;
	const unsigned char *source = data;
	unsigned char *base, *first = NULL, *last = NULL;
	int bgra = !texture->compressed && format == GL_BGRA && element_bytes == 4;

	if (!texture->memory || !data || level >= texture->levels)
		return;
	if (debug.log_textures && level == 0 && !texture->compressed && element_bytes == 4 && width * height >= 4096)
	{
		/* (the texels' average color and alpha: textures that come out
		dark or clear) */
		static int logged;
		const unsigned char *texel = data;
		unsigned long index, count = width * height * (depth ? depth : 1), sum[4] = { 0, 0, 0, 0 };

		for (index = 0; index < count; index++)
		{
			sum[0] += texel[index * 4 + 0];
			sum[1] += texel[index * 4 + 1];
			sum[2] += texel[index * 4 + 2];
			sum[3] += texel[index * 4 + 3];
		}
		if (logged++ < 80)
			wiiu_log("gl: texture %lux%lu (0x%x data) averages %lu %lu %lu %lu", width, height, format, sum[0] / count,
				sum[1] / count, sum[2] / count, sum[3] / count);
	}
	if (debug.log_textures && level == 0 && texture->width <= 16 && texture->height <= 16 && !texture->compressed)
	{
		static int logged;
		const unsigned char *texel = data;

		if (logged++ < 30)
			wiiu_log("gl: small texture %lux%lu (0x%x data): %02x%02x%02x%02x %02x%02x%02x%02x", texture->width,
				texture->height, format, texel[0], texel[1], texel[2], texel[3], texel[4], texel[5], texel[6], texel[7]);
	}
	if (texture->target == GL_TEXTURE_CUBE_MAP)
		z = (unsigned long)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X);
	if (texture->tiled && texture->target == GL_TEXTURE_2D && (x || y || width != level_size(texture->width, level) ||
		height != level_size(texture->height, level)))
	{
		/* (a part of a tiled level: written in a linear twin) */
		struct texture twin;

		if (texture_twin_make(texture, &twin, level, level))
		{
			twin.defined_levels = 0;
			texture_upload(&twin, target, level, x, y, z, width, height, depth, data, format);
			twin.defined_levels = twin.defined_levels > texture->defined_levels ? twin.defined_levels :
				texture->defined_levels;
			texture_twin_commit(texture, &twin, level, level);
		}
		return;
	}
	if (texture->tiled)
	{
		texture_upload_tiled(texture, level, z, width, height, depth, data, format);
		return;
	}
	texture_wait_unused(texture);
	base = texture_level(texture, level, &pitch, &slice_bytes);
	for (slice = 0; slice < (depth ? depth : 1); slice++)
	{
		unsigned char *destination = base + (z + slice) * slice_bytes + (y / block) * pitch + (x / block) * element_bytes;

		if (!first)
			first = destination;
		for (row = 0; row < rows; row++)
		{
			unsigned char *out = destination + row * pitch;

			if (bgra)
			{
				unsigned long texel;

				for (texel = 0; texel < width; texel++)
				{
					out[texel * 4 + 0] = source[texel * 4 + 2];
					out[texel * 4 + 1] = source[texel * 4 + 1];
					out[texel * 4 + 2] = source[texel * 4 + 0];
					out[texel * 4 + 3] = source[texel * 4 + 3];
				}
			}
			else
				memcpy(out, source, row_bytes);
			source += row_bytes;
			last = out + row_bytes;
		}
	}
	if (first && last > first)
		gpu_written(first, (unsigned long)(last - first), GX2_INVALIDATE_MODE_CPU_TEXTURE);
	if (level + 1 > texture->defined_levels)
		texture->defined_levels = level + 1;
}

static GLenum texture_target_of(GLenum target)
{
	if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z)
		return GL_TEXTURE_CUBE_MAP;
	return target;
}

static void image_given(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
	GLsizei depth, GLenum format, const void *pixels);

/* (timed, for the frame lines: watch) */
static void image(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLsizei depth,
	GLenum format, const void *pixels)
{
	OSTime start = OSGetSystemTime();

	image_given(target, level, internal, width, height, depth, format, pixels);
	watch.texture_time += OSGetSystemTime() - start;
	watch.textures++;
}

static void image_given(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
	GLsizei depth, GLenum format, const void *pixels)
{
	struct texture *texture = bound_texture(target);
	GLenum whole = texture_target_of(target);

	if (!texture)
		return;
	if (whole == GL_TEXTURE_2D_ARRAY)
	{
		FIRST_USE();
		return;
	}
	if (level == 0 || (!texture->memory && !texture->depth_format))
	{
		unsigned long base_width = (unsigned long)width << level, base_height = (unsigned long)height << level;

		if (!texture_allocate(texture, whole, (GLenum)internal, base_width, base_height,
			(unsigned long)(depth ? depth : 1), pixels != NULL))
		{
			return;
		}
	}
	if (pixels)
		texture_upload(texture, target, level, 0, 0, 0, (unsigned long)width, (unsigned long)height,
			whole == GL_TEXTURE_3D ? (unsigned long)depth : 1, pixels, format);
	else if (level + 1 > texture->defined_levels)
		texture->defined_levels = level + 1;
}

/* averages each level into the next, for RGBA8 2D textures (glGenerateMipmap:
the renderer's levels it did not draw) */
static void generate_mipmaps(struct texture *texture)
{
	int level;

	if (!texture->memory || texture->compressed || texture->target != GL_TEXTURE_2D || texel_bytes(texture) != 4)
	{
		FIRST_USE();
		return;
	}
	if (texture->tiled)
	{
		/* (made in a linear twin, from the first level) */
		struct texture twin;

		if (texture->levels > 1 && texture_twin_make(texture, &twin, 0, 0))
		{
			generate_mipmaps(&twin);
			texture_twin_commit(texture, &twin, 1, texture->levels - 1);
		}
		return;
	}
	if (texture->written)
	{
		/* (what the GPU drew is in memory first) */
		gpu_done("the GPU let finish to read a texture drawn");
		DCInvalidateRange(texture->memory, texture->bytes);
	}
	else
		texture_wait_unused(texture);
	for (level = 1; level < texture->levels; level++)
	{
		unsigned long width = level_size(texture->width, level), height = level_size(texture->height, level);
		unsigned long above_width = level_size(texture->width, level - 1);
		unsigned long above_height = level_size(texture->height, level - 1);
		unsigned long above_pitch, below_pitch, slice, x, y;
		const unsigned char *above = texture_level(texture, level - 1, &above_pitch, &slice);
		unsigned char *below = texture_level(texture, level, &below_pitch, &slice);

		for (y = 0; y < height; y++)
		{
			for (x = 0; x < width; x++)
			{
				unsigned long sum[4] = { 0, 0, 0, 0 };
				int dx, dy, channel;
				unsigned char *out = below + y * below_pitch + x * 4;

				for (dy = 0; dy < 2; dy++)
				for (dx = 0; dx < 2; dx++)
				{
					unsigned long sx = x * 2 + dx < above_width ? x * 2 + dx : above_width - 1;
					unsigned long sy = y * 2 + dy < above_height ? y * 2 + dy : above_height - 1;
					const unsigned char *in = above + sy * above_pitch + sx * 4;

					for (channel = 0; channel < 4; channel++)
						sum[channel] += in[channel];
				}
				for (channel = 0; channel < 4; channel++)
					out[channel] = (unsigned char)((sum[channel] + 2) / 4);
			}
		}
	}
	gpu_written(texture->memory, texture->bytes, GX2_INVALIDATE_MODE_CPU_TEXTURE);
	texture->defined_levels = texture->levels;
}

/* ---------- framebuffer attachments */

/* a color buffer over a texture's level, made once (SDL's render targets: a
GX2ColorBuffer over the texture's own surface) */
static GX2ColorBuffer *texture_color_buffer(struct texture *texture, int level)
{
	GX2ColorBuffer *buffer;

	if (!texture->memory || texture->compressed || level < 0 || level >= texture->levels)
		return NULL;
	if (texture->color_buffers[level])
		return texture->color_buffers[level];
	buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;
	buffer->surface = texture->gx2.surface;
	buffer->viewMip = (uint32_t)level;
	buffer->viewFirstSlice = 0;
	buffer->viewNumSlices = 1;
	GX2InitColorBufferRegs(buffer);
	texture->color_buffers[level] = buffer;
	return buffer;
}

/* a depth texture's depth buffer (tiled, as GX2 makes them: libwhb's
GfxInitDepthBuffer), made once */
static GX2DepthBuffer *texture_depth_buffer(struct texture *texture)
{
	GX2DepthBuffer *buffer;

	if (!texture->depth_format)
		return NULL;
	if (texture->depth_buffer)
		return texture->depth_buffer;
	buffer = calloc(1, sizeof(*buffer));
	if (!buffer)
		return NULL;
	buffer->surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
	buffer->surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	buffer->surface.width = texture->width;
	buffer->surface.height = texture->height;
	buffer->surface.depth = 1;
	buffer->surface.mipLevels = 1;
	/* (0x011: as a depth buffer, 24-bit unsigned depth and 8 bits of
	stencil; wut names it UNORM_R24_X8, its use as a texture) */
	buffer->surface.format = GX2_SURFACE_FORMAT_UNORM_R24_X8;
	buffer->surface.aa = GX2_AA_MODE1X;
	buffer->surface.tileMode = GX2_TILE_MODE_DEFAULT;
	buffer->viewNumSlices = 1;
	buffer->depthClear = 1.0f;
	GX2CalcSurfaceSizeAndAlignment(&buffer->surface);
	texture->depth_memory = gpu_allocate(buffer->surface.imageSize, buffer->surface.alignment);
	if (!texture->depth_memory)
	{
		free(buffer);
		wiiu_log("gl: no memory for a %lux%lu depth buffer", texture->width, texture->height);
		return NULL;
	}
	buffer->surface.image = texture->depth_memory;
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, texture->depth_memory, buffer->surface.imageSize);
	GX2InitDepthBufferRegs(buffer);
	texture->depth_buffer = buffer;
	wiiu_log("gl: depth buffer %lux%lu (%u bytes)", texture->width, texture->height,
		(unsigned int)buffer->surface.imageSize);
	return buffer;
}

/* the draw framebuffer's buffers bound in GX2 (the screen's for 0) */
static void bind_targets(void)
{
	struct framebuffer *framebuffer = object_get(gl.draw_framebuffer, _object_framebuffer);
	GX2ColorBuffer *color = NULL;
	GX2DepthBuffer *depth = NULL;
	struct texture *color_texture = NULL;

	if (!framebuffer)
	{
		color = wiiu_video_screen_color();
		depth = wiiu_video_screen_depth();
	}
	else
	{
		struct texture *depth_texture = object_get(framebuffer->depth, _object_texture);

		color_texture = object_get(framebuffer->color, _object_texture);
		if (color_texture)
			color = texture_color_buffer(color_texture, framebuffer->color_level);
		if (depth_texture)
			depth = texture_depth_buffer(depth_texture);
	}
	gl.target_has_color = color != NULL;
	gl.target_has_depth = depth != NULL;
	gl.target_texture = color ? color_texture : NULL;
	if (color)
	{
		gl.target_width = level_size(color->surface.width, (int)color->viewMip);
		gl.target_height = level_size(color->surface.height, (int)color->viewMip);
	}
	else if (depth)
	{
		gl.target_width = depth->surface.width;
		gl.target_height = depth->surface.height;
	}
	/* (a framebuffer without color keeps the last color buffer bound, its
	channels masked: GX2 draws with one) */
	if (color && color != gl.bound_color)
	{
		again.valid = 0;
		GX2SetColorBuffer(color, GX2_RENDER_TARGET_0);
		gl.bound_color = color;
	}
	if (depth && depth != gl.bound_depth)
	{
		again.valid = 0;
		GX2SetDepthBuffer(depth);
		gl.bound_depth = depth;
	}
}

/* the target drawn into is newer than its texels: flushed before it is
next sampled */
static void targets_written(void)
{
	if (gl.target_texture)
		gl.target_texture->written = 1;
}

/* a texture the GPU drew into, out of the color buffer's cache and into the
texture cache (GX2Invalidate's color buffer and texture modes) */
static void texture_flush_drawn(struct texture *texture)
{
	if (!texture->written || !texture->memory)
		return;
	texture->written = 0;
	GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE, texture->memory, texture->bytes);
}

/* ---------- shaders */

/* The varyings' locations, by name, the same for every shader: the first
shader that names one gives it the next free location. */
#define VARYING_LOCATIONS 30

static char varying_names[VARYING_LOCATIONS][32];
static int varying_count;

static int varying_location(const char *name, size_t length)
{
	int index;

	if (length == 0 || length >= sizeof(varying_names[0]))
		return -1;
	for (index = 0; index < varying_count; index++)
	{
		if (strlen(varying_names[index]) == length && !strncmp(varying_names[index], name, length))
			return index;
	}
	if (varying_count == VARYING_LOCATIONS)
		return -1;
	memcpy(varying_names[varying_count], name, length);
	varying_names[varying_count][length] = '\0';
	return varying_count++;
}

/* the text as CafeGLSL is given it: each varying given its location, and a
vertex shader's depth made OpenGL's as it ends (the file's comment) */
/* A cube map's lookup as OpenGL defines it, done in the shader: the face
by the major axis, its coordinates from the other two (the specification's
table of sc, tc and ma, "Cube Map Texture Selection"), read from the faces
as an array. In Cemu, CafeGLSL's cube map lookups read the wrong face and
place (the bring-up switch cube_probe logs them: the face followed the
sign of the first minor axis, whichever the major), which made the
normalization cubes of the maps' bump-mapped lighting give faceted, dark
cliffs; read this way they are right. Filtering does not cross a face's
edge. The bring-up switch native_cubes keeps CafeGLSL's lookups, to compare
on the console. The level of detail is the cube's, worked out here from the
direction's gradients, which do not jump at a face's edge as the face's
coordinates do (textureGrad came out wrong in Cemu: the world drawn as
flat fog). */
static const char cube_lookup_source[] =
	"vec4 wiiu_cube(sampler2DArray faces, vec3 d)\n"
	"{\n"
	"\tvec3 a = abs(d);\n"
	"\tvec3 s_axis, t_axis, m_axis;\n"
	"\tfloat face;\n"
	"\tif (a.x >= a.y && a.x >= a.z)\n"
	"\t{\n"
	"\t\tfloat sign_x = d.x >= 0.0 ? 1.0 : -1.0;\n"
	"\t\tface = d.x >= 0.0 ? 0.0 : 1.0;\n"
	"\t\ts_axis = vec3(0.0, 0.0, -sign_x); t_axis = vec3(0.0, -1.0, 0.0); m_axis = vec3(sign_x, 0.0, 0.0);\n"
	"\t}\n"
	"\telse if (a.y >= a.z)\n"
	"\t{\n"
	"\t\tfloat sign_y = d.y >= 0.0 ? 1.0 : -1.0;\n"
	"\t\tface = d.y >= 0.0 ? 2.0 : 3.0;\n"
	"\t\ts_axis = vec3(1.0, 0.0, 0.0); t_axis = vec3(0.0, 0.0, sign_y); m_axis = vec3(0.0, sign_y, 0.0);\n"
	"\t}\n"
	"\telse\n"
	"\t{\n"
	"\t\tfloat sign_z = d.z >= 0.0 ? 1.0 : -1.0;\n"
	"\t\tface = d.z >= 0.0 ? 4.0 : 5.0;\n"
	"\t\ts_axis = vec3(sign_z, 0.0, 0.0); t_axis = vec3(0.0, -1.0, 0.0); m_axis = vec3(0.0, 0.0, sign_z);\n"
	"\t}\n"
	"\tfloat ma = max(dot(m_axis, d), 1.0e-20);\n"
	"\tvec2 sc = vec2(dot(s_axis, d), dot(t_axis, d));\n"
	"\tvec3 dx = dFdx(d), dy = dFdy(d);\n"
	"\tvec2 gx = 0.5 * (vec2(dot(s_axis, dx), dot(t_axis, dx)) * ma - sc * dot(m_axis, dx)) / (ma * ma);\n"
	"\tvec2 gy = 0.5 * (vec2(dot(s_axis, dy), dot(t_axis, dy)) * ma - sc * dot(m_axis, dy)) / (ma * ma);\n"
	"\tvec2 size = vec2(textureSize(faces, 0).xy);\n"
	"\tfloat rho = max(length(gx * size), length(gy * size));\n"
	"\treturn textureLod(faces, vec3(0.5 * (sc / ma + 1.0), face), log2(max(rho, 1.0e-6)));\n"
	"}\n";

/* a fragment shader's cube maps made arrays of faces read by wiiu_cube;
the texN made so in *cube_arrays (bit N) */
static void shader_cube_arrays(char *text, unsigned long *cube_arrays)
{
	int stage;
	char *after_version;

	for (stage = 0; stage < 8; stage++)
	{
		char declaration[40], call[24], replacement[24];
		char *at;

		snprintf(declaration, sizeof(declaration), "uniform samplerCube tex%d;", stage);
		at = strstr(text, declaration);
		if (!at)
			continue;
		/* (samplerCube to sampler2DArray: the same length but one) */
		memmove(at + 8 + 14, at + 8 + 11, strlen(at + 8 + 11) + 1);
		memcpy(at + 8, "sampler2DArray", 14);
		snprintf(call, sizeof(call), "texture(tex%d,", stage);
		snprintf(replacement, sizeof(replacement), "wiiu_cube(tex%d,", stage);
		while ((at = strstr(text, call)) != NULL)
		{
			memmove(at + strlen(replacement), at + strlen(call), strlen(at + strlen(call)) + 1);
			memcpy(at, replacement, strlen(replacement));
		}
		*cube_arrays |= 1UL << stage;
	}
	if (!*cube_arrays)
		return;
	/* (the lookup after the #version line) */
	after_version = strchr(text, '\n');
	if (after_version)
	{
		size_t length = strlen(cube_lookup_source);

		after_version++;
		memmove(after_version + length, after_version, strlen(after_version) + 1);
		memcpy(after_version, cube_lookup_source, length);
	}
}

static char *shader_text(const char *source, int vertex)
{
	size_t size = strlen(source);
	char *text = malloc(size * 2 + 4096);
	const char *line = source;
	char *out = text;

	if (!text)
		return NULL;
	while (*line)
	{
		const char *end = strchr(line, '\n');
		size_t length = end ? (size_t)(end - line + 1) : strlen(line);
		const char *word = line;
		const char *qualifier = vertex ? "out " : "in ";

		while (*word == ' ' || *word == '\t')
			word++;
		/* (interpolation qualifiers first) */
		if (!strncmp(word, "flat ", 5))
			word += 5;
		else if (!strncmp(word, "noperspective ", 14))
			word += 14;
		else if (!strncmp(word, "smooth ", 7))
			word += 7;
		if (!strncmp(word, qualifier, strlen(qualifier)))
		{
			/* "out vec4 name;": the name is the last word before the ';' */
			const char *semicolon = memchr(word, ';', length - (size_t)(word - line));
			const char *name_end = semicolon, *name_start;

			if (semicolon)
			{
				while (name_end > word && name_end[-1] == ' ')
					name_end--;
				if (name_end > word && name_end[-1] == ']')
				{
					while (name_end > word && name_end[-1] != '[')
						name_end--;
					if (name_end > word)
						name_end--;
				}
				name_start = name_end;
				while (name_start > word && name_start[-1] != ' ')
					name_start--;
				{
					int location = varying_location(name_start, (size_t)(name_end - name_start));

					if (location >= 0)
						out += sprintf(out, "layout(location = %d) ", location);
				}
			}
		}
		memcpy(out, line, length);
		out += length;
		line += length;
	}
	*out = '\0';
	debug_load();
	if (!vertex && debug.math_cubes)
	{
		/* (bring-up: each cube map's lookup made the normalization cube's
		answer, computed: to tell the cube maps' sampling from the rest) */
		int stage;

		for (stage = 0; stage < 4; stage++)
		{
			char find[64], declaration[40], replace[96];
			char *at;

			snprintf(declaration, sizeof(declaration), "samplerCube tex%d;", stage);
			if (!strstr(text, declaration))
				continue;
			snprintf(find, sizeof(find), "texture(tex%d, (xT%d).xyz)", stage, stage);
			snprintf(replace, sizeof(replace), "vec4(normalize((xT%d).xyz) * 0.5 + 0.5, 1.0)", stage);
			at = strstr(text, find);
			if (at)
			{
				memmove(at + strlen(replace), at + strlen(find), strlen(at + strlen(find)) + 1);
				memcpy(at, replace, strlen(replace));
			}
		}
	}
	if (vertex && !debug.packed_unsigned)
	{
		/* (the packed normals' fields taken with unsigned shifts and masks,
		their signs by comparison, not by shifting a signed integer right:
		with the arithmetic shifts nv2a_vsh.c writes, Blood Gulch's
		bump-mapped lighting came out dark and faceted in Cemu, which of
		CafeGLSL's code or Cemu's reading of it is to blame not yet known;
		the same numbers either way. The bring-up switch packed_unsigned
		leaves the shifts as they are, to compare on the console) */
		static const char *const signed_lines[3] =
		{
			"int x = int(p << 21) >> 21;",
			"int y = int(p << 10) >> 21;",
			"int z = int(p) >> 22;",
		};
		static const char *const unsigned_lines[3] =
		{
			"int x = int(p & 0x7FFu) - ((p & 0x400u) != 0u ? 2048 : 0);",
			"int y = int((p >> 11u) & 0x7FFu) - (((p >> 11u) & 0x400u) != 0u ? 2048 : 0);",
			"int z = int(p >> 22u) - ((p & 0x80000000u) != 0u ? 1024 : 0);",
		};
		int line_index;

		for (line_index = 0; line_index < 3; line_index++)
		{
			char *at = strstr(text, signed_lines[line_index]);

			if (at)
			{
				size_t old_length = strlen(signed_lines[line_index]), new_length = strlen(unsigned_lines[line_index]);

				memmove(at + new_length, at + old_length, strlen(at + old_length) + 1);
				memcpy(at, unsigned_lines[line_index], new_length);
			}
		}
	}
	if (vertex)
	{
		/* (main renamed, and called by a main that ends with the depth) */
		char *main_at = strstr(text, "void main(");

		if (main_at)
		{
			size_t rest = strlen(main_at + 5);

			/* "void main(" becomes "void wiiu_main(" */
			memmove(main_at + 10, main_at + 5, rest + 1);
			memcpy(main_at + 5, "wiiu_", 5);
			strcat(text, "\nvoid main()\n{\n\twiiu_main();\n"
				"\tgl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n}\n");
		}
	}
	return text;
}

static void shader_describe(const struct shader *shader);

#ifdef HALO_WIIU_CAFEGLSL
static int compiler_ready;

static void compiler_start(void)
{
	OSTime start;

	if (compiler_ready)
		return;
	start = OSGetSystemTime();
	InitGLSLCompiler();
	compiler_ready = 1;
	wiiu_log("gl: CafeGLSL %s started in %u ms", GetGLSLCompilerVersion(),
		(unsigned int)OSTicksToMilliseconds(OSGetSystemTime() - start));
}
#endif

static struct
{
	unsigned long compiled, failed, cached;
	OSTime time;
} compile_stats;

static int shader_compile(struct shader *shader)
{
#ifdef HALO_WIIU_CAFEGLSL
	int vertex = shader->type == GL_VERTEX_SHADER;
	char *text;
	GX2ShaderMode mode;
	OSTime start;

	if (shader->compiled)
		return shader->vertex || shader->pixel;
	shader->compiled = 1;
	if (!shader->source)
		return 0;
	text = shader_text(shader->source, vertex);
	if (!text)
		return 0;
	debug_load();
	if (!vertex && !debug.native_cubes)
		shader_cube_arrays(text, &shader->cube_arrays);
	start = OSGetSystemTime();
	shader->log[0] = '\0';
	/* (compiled in an earlier run: the text and the compiler the same) */
	{
		uint64_t key = 14695981039346656037ULL;
		const char *version = GetGLSLCompilerVersion();
		const char *byte;

		for (byte = version; *byte; byte++)
			key = (key ^ (unsigned char)*byte) * 1099511628211ULL;
		for (byte = text; *byte; byte++)
			key = (key ^ (unsigned char)*byte) * 1099511628211ULL;
		shader->key = key;
	}
	/* (each step timed: the first 64 shaders' in the log, all of them in
	the frames' lines, watch_frame_end) */
	{
		OSTime read_start = OSGetSystemTime(), read_time, compile_time = 0, write_time = 0;
		int written = -1;

		watch_step("a shader read from the SD card");
		if (vertex)
			shader->vertex = wiiu_shader_cache_load(shader->key, 1);
		else
			shader->pixel = wiiu_shader_cache_load(shader->key, 0);
		read_time = OSGetSystemTime() - read_start;
		watch.cache_read += read_time;
		watch.cache_reads++;
		if (shader->vertex || shader->pixel)
		{
			shader->cached = 1;
			compile_stats.cached++;
			mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
		}
		else
		{
			OSTime compile_start;

			compiler_start();
			watch_step(vertex ? "CafeGLSL compiling a vertex shader" : "CafeGLSL compiling a pixel shader");
			compile_start = OSGetSystemTime();
			if (vertex)
				mode = CompileVertexShader(text, GLSL_COMPILE_REGISTER, &shader->vertex, shader->log,
					sizeof(shader->log), GLSL_COMPILER_FLAG_NONE);
			else
				mode = CompilePixelShader(text, GLSL_COMPILE_REGISTER, &shader->pixel, shader->log,
					sizeof(shader->log), GLSL_COMPILER_FLAG_NONE);
			compile_time = OSGetSystemTime() - compile_start;
			watch.compile += compile_time;
			watch.compiles++;
			if (mode != GLSL_SHADER_MODE_ERROR)
			{
				OSTime write_start = OSGetSystemTime();

				watch_step("a shader written to the SD card");
				written = wiiu_shader_cache_store(shader->key, vertex, vertex ? (const void *)shader->vertex :
					(const void *)shader->pixel);
				write_time = OSGetSystemTime() - write_start;
				watch.cache_write += write_time;
				watch.cache_writes++;
			}
		}
		if (debug.log_shaders && compile_stats.compiled + compile_stats.failed < 64)
			wiiu_log("gl: shader %lu, %s, %lu bytes of GLSL: %s in %lu ms, compiled in %lu ms, written to the SD card %s "
				"in %lu ms", compile_stats.compiled + compile_stats.failed + 1, vertex ? "vertex" : "pixel",
				(unsigned long)strlen(text), shader->cached ? "read from the SD card" : "not on the SD card",
				(unsigned long)OSTicksToMilliseconds(read_time), (unsigned long)OSTicksToMilliseconds(compile_time),
				written < 0 ? "(not written)" : written ? "whole" : "FAILED",
				(unsigned long)OSTicksToMilliseconds(write_time));
		watch_step("the game's own work");
	}
	compile_stats.time += OSGetSystemTime() - start;
	if (mode == GLSL_SHADER_MODE_ERROR)
	{
		compile_stats.failed++;
		if (compile_stats.failed <= 8)
		{
			char path[128];
			FILE *file;

			wiiu_log("gl: a %s shader did not compile: %s", vertex ? "vertex" : "fragment", shader->log);
			snprintf(path, sizeof(path), WIIU_APP_FOLDER "/failed%02lu.%s", compile_stats.failed,
				vertex ? "vert" : "frag");
			file = fopen(path, "wb");
			if (file)
			{
				fputs(text, file);
				fclose(file);
			}
		}
		free(text);
		shader->vertex = NULL;
		shader->pixel = NULL;
		return 0;
	}
	free(text);
	compile_stats.compiled++;
	if (compile_stats.compiled <= 4)
		shader_describe(shader);
	if (compile_stats.compiled == 1 || compile_stats.compiled % 25 == 0)
		wiiu_log("gl: %lu shaders ready (%lu from the SD card's cache, %lu ms in all), %lu failed", compile_stats.compiled,
			compile_stats.cached, (unsigned long)OSTicksToMilliseconds(compile_stats.time), compile_stats.failed);
	if (vertex)
	{
		uint32_t index;

		GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader->vertex->program, shader->vertex->size);
		for (index = 0; index < shader->vertex->attribVarCount; index++)
		{
			uint32_t location = shader->vertex->attribVars[index].location;

			if (location < ATTRIBUTES)
				shader->inputs |= 1UL << location;
		}
	}
	else
		GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, shader->pixel->program, shader->pixel->size);
	return 1;
#else
	(void)shader;
	return 0;
#endif
}

/* a compiled shader's variables, in the log (its uniforms counted: CafeGLSL
lists each element of an array on its own) */
static void shader_describe(const struct shader *shader)
{
	uint32_t index;

	if (!debug.log_shaders)
		return;
	if (shader->vertex)
	{
		const GX2VertexShader *vertex = shader->vertex;

		wiiu_log("gl: vertex shader: %u bytes, mode %d, %u uniforms, %u attributes, %u samplers", (unsigned int)vertex->size,
			(int)vertex->mode, (unsigned int)vertex->uniformVarCount, (unsigned int)vertex->attribVarCount,
			(unsigned int)vertex->samplerVarCount);
		for (index = 0; index < vertex->attribVarCount; index++)
			wiiu_log("gl:   attribute %s: type %d, location %u", vertex->attribVars[index].name,
				(int)vertex->attribVars[index].type, (unsigned int)vertex->attribVars[index].location);
	}
	if (shader->pixel)
	{
		const GX2PixelShader *pixel = shader->pixel;

		wiiu_log("gl: pixel shader: %u bytes, mode %d, %u uniforms, %u samplers", (unsigned int)pixel->size,
			(int)pixel->mode, (unsigned int)pixel->uniformVarCount, (unsigned int)pixel->samplerVarCount);
		for (index = 0; index < pixel->samplerVarCount; index++)
			wiiu_log("gl:   sampler %s: type %d, location %u", pixel->samplerVars[index].name,
				(int)pixel->samplerVars[index].type, (unsigned int)pixel->samplerVars[index].location);
	}
}

static void shader_release(struct shader *shader)
{
	if (shader->vertex)
		retire(shader->vertex, shader->cached ? _retired_cached_vertex_shader : _retired_vertex_shader);
	if (shader->pixel)
		retire(shader->pixel, shader->cached ? _retired_cached_pixel_shader : _retired_pixel_shader);
	free(shader->source);
	free(shader);
}

/* a uniform's name as GL names it: "name" for "name[0]" */
static void uniform_base_name(char *base, size_t size, const char *name)
{
	const char *bracket = strchr(name, '[');
	size_t length = bracket ? (size_t)(bracket - name) : strlen(name);

	if (length >= size)
		length = size - 1;
	memcpy(base, name, length);
	base[length] = '\0';
}

static struct uniform *program_uniform(struct program *program, const char *name)
{
	char base[32];
	int index;

	uniform_base_name(base, sizeof(base), name);
	for (index = 0; index < program->uniform_count; index++)
	{
		if (!strcmp(program->uniforms[index].name, base))
			return &program->uniforms[index];
	}
	if (program->uniform_count == MAXIMUM_UNIFORMS)
		return NULL;
	{
		struct uniform *uniform = &program->uniforms[program->uniform_count++];

		memset(uniform, 0, sizeof(*uniform));
		strcpy(uniform->name, base);
		uniform->vertex_offset = uniform->pixel_offset = -1;
		uniform->sampler_location = -1;
		return uniform;
	}
}

static int shader_variable_integer(GX2ShaderVarType type)
{
	switch (type)
	{
	case GX2_SHADER_VAR_TYPE_INT: case GX2_SHADER_VAR_TYPE_INT2: case GX2_SHADER_VAR_TYPE_INT3:
	case GX2_SHADER_VAR_TYPE_INT4: case GX2_SHADER_VAR_TYPE_UINT: case GX2_SHADER_VAR_TYPE_UINT2:
	case GX2_SHADER_VAR_TYPE_UINT3: case GX2_SHADER_VAR_TYPE_UINT4: case GX2_SHADER_VAR_TYPE_BOOL:
		return 1;
	default:
		return 0;
	}
}

/* an element of an array CafeGLSL also lists on its own ("c[1]", after "c"
and "c[0]"): the array is taken from its first */
static int uniform_later_element(const char *name)
{
	const char *bracket = strchr(name, '[');

	return bracket && atoi(bracket + 1) > 0;
}

/* every uniform of a program's two shaders */
static void program_uniforms(struct program *program, struct shader *vertex, struct shader *fragment)
{
	uint32_t index;

	program->uniform_count = 0;
	for (index = 0; index < vertex->vertex->uniformVarCount; index++)
	{
		const GX2UniformVar *variable = &vertex->vertex->uniformVars[index];
		struct uniform *uniform;

		if (variable->block >= 0 || uniform_later_element(variable->name))
			continue;
		uniform = program_uniform(program, variable->name);
		if (!uniform)
			continue;
		uniform->vertex_offset = (int)variable->offset;
		if ((int)variable->count > uniform->count)
			uniform->count = (int)variable->count;
		uniform->integer = shader_variable_integer(variable->type);
	}
	for (index = 0; index < fragment->pixel->uniformVarCount; index++)
	{
		const GX2UniformVar *variable = &fragment->pixel->uniformVars[index];
		struct uniform *uniform;

		if (variable->block >= 0 || uniform_later_element(variable->name))
			continue;
		uniform = program_uniform(program, variable->name);
		if (!uniform)
			continue;
		uniform->pixel_offset = (int)variable->offset;
		if ((int)variable->count > uniform->count)
			uniform->count = (int)variable->count;
		uniform->integer = shader_variable_integer(variable->type);
	}
	for (index = 0; index < fragment->pixel->samplerVarCount; index++)
	{
		const GX2SamplerVar *variable = &fragment->pixel->samplerVars[index];
		struct uniform *uniform = program_uniform(program, variable->name);

		if (!uniform)
			continue;
		uniform->sampler = 1;
		uniform->sampler_location = (int)variable->location;
		uniform->sampler_slot = variable->type == GX2_SAMPLER_VAR_TYPE_SAMPLER_3D ? 1 :
			variable->type == GX2_SAMPLER_VAR_TYPE_SAMPLER_CUBE ? 2 : 0;
		uniform->cube_array = 0;
		if (!strncmp(variable->name, "tex", 3) && variable->name[3] >= '0' && variable->name[3] <= '7' &&
			!variable->name[4] && ((fragment->cube_arrays >> (variable->name[3] - '0')) & 1))
		{
			uniform->sampler_slot = 2;
			uniform->cube_array = 1;
		}
		uniform->count = 1;
	}
	program->vertex_dirty_low = program->pixel_dirty_low = UNIFORM_COMPONENTS;
	program->vertex_dirty_high = program->pixel_dirty_high = 0;
}

/* a location: the uniform's index and the array element */
#define LOCATION(uniform, element) ((GLint)(((uniform) + 1) << 10 | (element)))
#define LOCATION_UNIFORM(location) (((location) >> 10) - 1)
#define LOCATION_ELEMENT(location) ((location) & 1023)

/* (an element of a register-mode uniform is four components: GX2's
registers are vectors) */
static void uniform_store(struct program *program, struct uniform *uniform, int element, const uint32_t *values,
	int components)
{
	int stage;

	for (stage = 0; stage < 2; stage++)
	{
		int offset = stage ? uniform->pixel_offset : uniform->vertex_offset;
		uint32_t *registers = stage ? program->pixel_values : program->vertex_values;
		int *low = stage ? &program->pixel_dirty_low : &program->vertex_dirty_low;
		int *high = stage ? &program->pixel_dirty_high : &program->vertex_dirty_high;
		int at;

		if (offset < 0)
			continue;
		at = offset + element * 4;
		if (at + 4 > UNIFORM_COMPONENTS)
			continue;
		memcpy(registers + at, values, sizeof(uint32_t) * (size_t)components);
		if (at < *low)
			*low = at;
		if (at + 4 > *high)
			*high = at + 4;
	}
}

static void uniform_set(GLint location, const float *values, int vectors, int components)
{
	struct program *program = object_get(gl.program, _object_program);
	struct uniform *uniform;
	int index, element;

	/* (GX2's uniform registers are the last draw's no more) */
	again.valid = 0;
	if (!program || location < 0)
		return;
	index = LOCATION_UNIFORM(location);
	element = LOCATION_ELEMENT(location);
	if (index < 0 || index >= program->uniform_count)
		return;
	uniform = &program->uniforms[index];
	for (; vectors > 0 && element < uniform->count; vectors--, element++, values += components)
	{
		uint32_t vector[4] = { 0, 0, 0, 0 };
		int component;

		for (component = 0; component < components; component++)
		{
			if (uniform->integer)
				vector[component] = (uint32_t)(int32_t)values[component];
			else
				memcpy(&vector[component], &values[component], 4);
		}
		uniform_store(program, uniform, element, vector, components);
	}
}

/* the program's registers set before a draw: all of its uniforms when it
is newly in use, else those written since */
static void program_upload(struct program *program)
{
	int stage;

	if (gl.uploaded_program != gl.program)
	{
		int index;

		for (index = 0; index < program->uniform_count; index++)
		{
			const struct uniform *uniform = &program->uniforms[index];
			int size = uniform->count * 4;

			if (uniform->sampler)
				continue;
			if (uniform->vertex_offset >= 0 && uniform->vertex_offset + size <= UNIFORM_COMPONENTS)
				GX2SetVertexUniformReg((uint32_t)uniform->vertex_offset, (uint32_t)size,
					program->vertex_values + uniform->vertex_offset);
			if (uniform->pixel_offset >= 0 && uniform->pixel_offset + size <= UNIFORM_COMPONENTS)
				GX2SetPixelUniformReg((uint32_t)uniform->pixel_offset, (uint32_t)size,
					program->pixel_values + uniform->pixel_offset);
		}
		gl.uploaded_program = gl.program;
	}
	else
	{
		for (stage = 0; stage < 2; stage++)
		{
			int low = stage ? program->pixel_dirty_low : program->vertex_dirty_low;
			int high = stage ? program->pixel_dirty_high : program->vertex_dirty_high;

			if (high <= low)
				continue;
			if (stage)
				GX2SetPixelUniformReg((uint32_t)low, (uint32_t)(high - low), program->pixel_values + low);
			else
				GX2SetVertexUniformReg((uint32_t)low, (uint32_t)(high - low), program->vertex_values + low);
		}
	}
	program->vertex_dirty_low = program->pixel_dirty_low = UNIFORM_COMPONENTS;
	program->vertex_dirty_high = program->pixel_dirty_high = 0;
}

/* ---------- fetch shaders */

struct fetch_shader
{
	struct fetch_shader *next;
	uint32_t hash;
	int count;
	GX2AttribStream streams[ATTRIBUTES];
	GX2FetchShader shader;
};

#define FETCH_BUCKETS 256

static struct fetch_shader *fetch_shaders[FETCH_BUCKETS];
static unsigned long fetch_shader_count;

static uint32_t fnv1a(const void *data, size_t size, uint32_t hash)
{
	const unsigned char *bytes = data;
	size_t index;

	for (index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 16777619u;
	return hash;
}

static GX2FetchShader *fetch_shader_get(const GX2AttribStream *streams, int count)
{
	uint32_t hash = fnv1a(streams, sizeof(GX2AttribStream) * (size_t)count, 2166136261u ^ (uint32_t)count);
	struct fetch_shader *entry;
	uint32_t size;
	void *program;

	for (entry = fetch_shaders[hash % FETCH_BUCKETS]; entry; entry = entry->next)
	{
		if (entry->hash == hash && entry->count == count &&
			!memcmp(entry->streams, streams, sizeof(GX2AttribStream) * (size_t)count))
		{
			return &entry->shader;
		}
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	size = GX2CalcFetchShaderSizeEx((uint32_t)count, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
	program = gpu_allocate(size, GX2_SHADER_PROGRAM_ALIGNMENT);
	if (!program)
	{
		free(entry);
		return NULL;
	}
	entry->hash = hash;
	entry->count = count;
	memcpy(entry->streams, streams, sizeof(GX2AttribStream) * (size_t)count);
	GX2InitFetchShaderEx(&entry->shader, program, (uint32_t)count, entry->streams, GX2_FETCH_SHADER_TESSELLATION_NONE,
		GX2_TESSELLATION_MODE_DISCRETE);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, program, size);
	entry->next = fetch_shaders[hash % FETCH_BUCKETS];
	fetch_shaders[hash % FETCH_BUCKETS] = entry;
	if (++fetch_shader_count == 1 || fetch_shader_count % 50 == 0)
		wiiu_log("gl: %lu fetch shaders made", fetch_shader_count);
	return &entry->shader;
}

/* ---------- GL's state as GX2's */

static GX2CompareFunction compare_function(GLenum function)
{
	switch (function)
	{
	case GL_NEVER: return GX2_COMPARE_FUNC_NEVER;
	case GL_LESS: return GX2_COMPARE_FUNC_LESS;
	case GL_EQUAL: return GX2_COMPARE_FUNC_EQUAL;
	case GL_LEQUAL: return GX2_COMPARE_FUNC_LEQUAL;
	case GL_GREATER: return GX2_COMPARE_FUNC_GREATER;
	case GL_NOTEQUAL: return GX2_COMPARE_FUNC_NOT_EQUAL;
	case GL_GEQUAL: return GX2_COMPARE_FUNC_GEQUAL;
	default: return GX2_COMPARE_FUNC_ALWAYS;
	}
}

static GX2BlendMode blend_factor(GLenum factor)
{
	switch (factor)
	{
	case GL_ZERO: return GX2_BLEND_MODE_ZERO;
	case GL_ONE: return GX2_BLEND_MODE_ONE;
	case GL_SRC_COLOR: return GX2_BLEND_MODE_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return GX2_BLEND_MODE_INV_SRC_COLOR;
	case GL_SRC_ALPHA: return GX2_BLEND_MODE_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return GX2_BLEND_MODE_INV_SRC_ALPHA;
	case GL_DST_ALPHA: return debug.dst_alpha_one ? GX2_BLEND_MODE_ONE : GX2_BLEND_MODE_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return GX2_BLEND_MODE_INV_DST_ALPHA;
	case GL_DST_COLOR: return GX2_BLEND_MODE_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return GX2_BLEND_MODE_INV_DST_COLOR;
	case GL_SRC_ALPHA_SATURATE: return GX2_BLEND_MODE_SRC_ALPHA_SAT;
	case GL_CONSTANT_COLOR: return GX2_BLEND_MODE_BLEND_FACTOR;
	case GL_ONE_MINUS_CONSTANT_COLOR: return GX2_BLEND_MODE_INV_BLEND_FACTOR;
	case GL_CONSTANT_ALPHA: return GX2_BLEND_MODE_CONSTANT_ALPHA;
	case GL_ONE_MINUS_CONSTANT_ALPHA: return GX2_BLEND_MODE_INV_CONSTANT_ALPHA;
	default: return GX2_BLEND_MODE_ONE;
	}
}

static GX2BlendCombineMode blend_operation(GLenum equation)
{
	switch (equation)
	{
	case GL_FUNC_SUBTRACT: return GX2_BLEND_COMBINE_MODE_SUB;
	case GL_FUNC_REVERSE_SUBTRACT: return GX2_BLEND_COMBINE_MODE_REV_SUB;
	case GL_MIN: return GX2_BLEND_COMBINE_MODE_MIN;
	case GL_MAX: return GX2_BLEND_COMBINE_MODE_MAX;
	default: return GX2_BLEND_COMBINE_MODE_ADD;
	}
}

static GX2StencilFunction stencil_operation(GLenum operation)
{
	switch (operation)
	{
	case GL_ZERO: return GX2_STENCIL_FUNCTION_ZERO;
	case GL_REPLACE: return GX2_STENCIL_FUNCTION_REPLACE;
	case GL_INCR: return GX2_STENCIL_FUNCTION_INCR_CLAMP;
	case GL_DECR: return GX2_STENCIL_FUNCTION_DECR_CLAMP;
	case GL_INVERT: return GX2_STENCIL_FUNCTION_INV;
	case GL_INCR_WRAP: return GX2_STENCIL_FUNCTION_INCR_WRAP;
	case GL_DECR_WRAP: return GX2_STENCIL_FUNCTION_DECR_WRAP;
	default: return GX2_STENCIL_FUNCTION_KEEP;
	}
}

static GX2TexClampMode clamp_mode(GLint wrap)
{
	switch (wrap)
	{
	case GL_CLAMP_TO_EDGE: return GX2_TEX_CLAMP_MODE_CLAMP;
	case GL_CLAMP_TO_BORDER: return GX2_TEX_CLAMP_MODE_CLAMP_BORDER;
	case GL_MIRRORED_REPEAT: return GX2_TEX_CLAMP_MODE_MIRROR;
	default: return GX2_TEX_CLAMP_MODE_WRAP;
	}
}

/* the scissor, inside the target */
static void apply_scissor(void)
{
	long x = 0, y = 0, width = (long)gl.target_width, height = (long)gl.target_height;

	if (gl.scissor_test)
	{
		long right = gl.scissor[0] + gl.scissor[2], bottom = gl.scissor[1] + gl.scissor[3];

		x = gl.scissor[0] < 0 ? 0 : gl.scissor[0];
		y = gl.scissor[1] < 0 ? 0 : gl.scissor[1];
		if (right > (long)gl.target_width)
			right = (long)gl.target_width;
		if (bottom > (long)gl.target_height)
			bottom = (long)gl.target_height;
		width = right > x ? right - x : 0;
		height = bottom > y ? bottom - y : 0;
	}
	GX2SetScissor((uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height);
}

static void apply_render_state(void)
{
	int depth = gl.depth_test && gl.target_has_depth && !debug.no_depth;
	int stencil = gl.stencil_test && gl.target_has_depth && !debug.no_depth;
	int blend = gl.blend && !debug.no_blend;
	int mask = gl.target_has_color ? (gl.color_mask[0] ? GX2_CHANNEL_MASK_R : 0) |
		(gl.color_mask[1] ? GX2_CHANNEL_MASK_G : 0) | (gl.color_mask[2] ? GX2_CHANNEL_MASK_B : 0) |
		(gl.color_mask[3] ? GX2_CHANNEL_MASK_A : 0) : 0;
	GX2CompareFunction stencil_function = compare_function(gl.stencil_func);
	int cull_front = 0, cull_back = 0;
	GX2FrontFace front = GX2_FRONT_FACE_CCW;

	GX2SetColorControl(GX2_LOGIC_OP_COPY, blend ? 1 : 0, FALSE, mask != 0);
	GX2SetBlendControl(GX2_RENDER_TARGET_0, blend_factor(gl.blend_source), blend_factor(gl.blend_destination),
		blend_operation(gl.blend_equation), TRUE, blend_factor(gl.blend_source_alpha),
		blend_factor(gl.blend_destination_alpha), blend_operation(gl.blend_equation_alpha));
	GX2SetBlendConstantColor(gl.blend_color[0], gl.blend_color[1], gl.blend_color[2], gl.blend_color[3]);
	GX2SetTargetChannelMasks((GX2ChannelMask)mask, 0, 0, 0, 0, 0, 0, 0);
	GX2SetDepthStencilControl(depth, depth && gl.depth_mask,
		compare_function(debug.equal_as_lequal && gl.depth_func == GL_EQUAL ? GL_LEQUAL : gl.depth_func), stencil, FALSE,
		stencil_function, stencil_operation(gl.stencil_zpass), stencil_operation(gl.stencil_zfail),
		stencil_operation(gl.stencil_fail), stencil_function, stencil_operation(gl.stencil_zpass),
		stencil_operation(gl.stencil_zfail), stencil_operation(gl.stencil_fail));
	GX2SetStencilMask((uint8_t)gl.stencil_mask, (uint8_t)gl.stencil_write_mask, (uint8_t)gl.stencil_reference,
		(uint8_t)gl.stencil_mask, (uint8_t)gl.stencil_write_mask, (uint8_t)gl.stencil_reference);
	/* (the winding GL culls, from the face it calls front, in window
	coordinates whose rows run down, glClipControl's GL_UPPER_LEFT, as
	Direct3D's and GX2's do: GX2 is told the other one is the front, and
	culls the back. Strips and fans are drawn as lists: draw) */
	if (gl.cull_face && gl.cull_mode != GL_FRONT_AND_BACK && !debug.no_cull)
	{
		GLenum culled = gl.cull_mode == GL_FRONT ? gl.front_face : gl.front_face == GL_CCW ? GL_CW : GL_CCW;

		if (debug.swap_winding)
			culled = culled == GL_CCW ? GL_CW : GL_CCW;
		front = culled == GL_CCW ? GX2_FRONT_FACE_CW : GX2_FRONT_FACE_CCW;
		cull_back = 1;
	}
	else if (gl.cull_face && gl.cull_mode == GL_FRONT_AND_BACK && !debug.no_cull)
		cull_front = cull_back = 1;
	GX2SetPolygonControl(front, cull_front, cull_back, gl.polygon_mode != GL_FILL,
		gl.polygon_mode == GL_LINE ? GX2_POLYGON_MODE_LINE : gl.polygon_mode == GL_POINT ? GX2_POLYGON_MODE_POINT :
		GX2_POLYGON_MODE_TRIANGLE,
		gl.polygon_mode == GL_LINE ? GX2_POLYGON_MODE_LINE : gl.polygon_mode == GL_POINT ? GX2_POLYGON_MODE_POINT :
		GX2_POLYGON_MODE_TRIANGLE,
		gl.polygon_offset_fill, gl.polygon_offset_fill, FALSE);
	if (gl.polygon_offset_fill)
		GX2SetPolygonOffset(gl.polygon_offset_units, gl.polygon_offset_factor, gl.polygon_offset_units,
			gl.polygon_offset_factor, 0.0f);
	apply_scissor();
	GX2SetViewport((float)gl.viewport[0], (float)gl.viewport[1], (float)gl.viewport[2], (float)gl.viewport[3],
		gl.depth_near, gl.depth_far);
}

/* a GX2 sampler from GL's parameters */
static void sampler_make(GX2Sampler *made, const struct sampler_parameters *sampler, const struct texture *texture)
{
	int mipmapped = sampler->min_filter != GL_NEAREST && sampler->min_filter != GL_LINEAR;
	int top_level = texture->defined_levels > 0 ? texture->defined_levels - 1 : 0;
	GX2TexXYFilterMode minify = (sampler->min_filter == GL_NEAREST || sampler->min_filter == GL_NEAREST_MIPMAP_NEAREST ||
		sampler->min_filter == GL_NEAREST_MIPMAP_LINEAR) ? GX2_TEX_XY_FILTER_MODE_POINT : GX2_TEX_XY_FILTER_MODE_LINEAR;
	GX2TexXYFilterMode magnify = sampler->mag_filter == GL_NEAREST ? GX2_TEX_XY_FILTER_MODE_POINT :
		GX2_TEX_XY_FILTER_MODE_LINEAR;
	GX2TexMipFilterMode mip = !mipmapped ? GX2_TEX_MIP_FILTER_MODE_NONE :
		(sampler->min_filter == GL_NEAREST_MIPMAP_NEAREST || sampler->min_filter == GL_LINEAR_MIPMAP_NEAREST) ?
		GX2_TEX_MIP_FILTER_MODE_POINT : GX2_TEX_MIP_FILTER_MODE_LINEAR;
	GX2TexAnisoRatio aniso = sampler->anisotropy >= 16.0f ? GX2_TEX_ANISO_RATIO_16_TO_1 :
		sampler->anisotropy >= 8.0f ? GX2_TEX_ANISO_RATIO_8_TO_1 : sampler->anisotropy >= 4.0f ?
		GX2_TEX_ANISO_RATIO_4_TO_1 : sampler->anisotropy >= 2.0f ? GX2_TEX_ANISO_RATIO_2_TO_1 : GX2_TEX_ANISO_RATIO_NONE;
	float max_lod = (float)top_level, min_lod = (float)texture->base_level;

	if (debug.no_aniso)
		aniso = GX2_TEX_ANISO_RATIO_NONE;
	if (texture->max_level < top_level)
		max_lod = (float)texture->max_level;
	if (sampler->max_lod < max_lod)
		max_lod = sampler->max_lod;
	if (sampler->min_lod > min_lod)
		min_lod = sampler->min_lod;
	if (!mipmapped || debug.no_mips)
		max_lod = min_lod;
	if (max_lod < 0.0f)
		max_lod = 0.0f;
	if (min_lod < 0.0f)
		min_lod = 0.0f;
	GX2InitSampler(made, clamp_mode(sampler->wrap_s), magnify);
	GX2InitSamplerClamping(made, clamp_mode(sampler->wrap_s), clamp_mode(sampler->wrap_t), clamp_mode(sampler->wrap_r));
	GX2InitSamplerXYFilter(made, magnify, minify, aniso);
	GX2InitSamplerZMFilter(made, GX2_TEX_Z_FILTER_MODE_LINEAR, mip);
	GX2InitSamplerLOD(made, min_lod, max_lod, sampler->lod_bias);
	GX2InitSamplerBorderType(made, GX2_TEX_BORDER_TYPE_VARIABLE);
}

/* the texture a sampler of this type (slot: 2D, 3D, cube) reads on a unit:
that target's, or another's if it has none */
static struct texture *unit_texture(GLint unit, int wanted)
{
	struct texture *texture;
	int slot;

	if (unit < 0 || unit >= TEXTURE_UNITS)
		return NULL;
	texture = object_get(gl.textures[unit][wanted], _object_texture);
	if (texture && texture->memory)
		return texture;
	for (slot = 0; slot < 3; slot++)
	{
		texture = object_get(gl.textures[unit][slot], _object_texture);
		if (texture && texture->memory)
			return texture;
	}
	return NULL;
}

/* a 1x1 texture for samplers with nothing bound (opaque black, as GL reads
an incomplete texture) */
static struct texture *empty_texture(int slot)
{
	static const unsigned char black[4] = { 0, 0, 0, 255 };
	struct texture *texture;
	int face;

	if (gl.empty[slot])
		return gl.empty[slot];
	texture = calloc(1, sizeof(*texture));
	if (!texture)
		return NULL;
	texture_defaults(texture);
	if (!texture_allocate(texture, slot == 1 ? GL_TEXTURE_3D : slot == 2 ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D,
		GL_RGBA8, 1, 1, 1, 0))
	{
		free(texture);
		return NULL;
	}
	for (face = 0; face < (slot == 2 ? 6 : 1); face++)
		texture_upload(texture, slot == 2 ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)face : GL_TEXTURE_2D, 0, 0, 0, 0,
			1, 1, 1, black, GL_RGBA);
	texture->sampler.min_filter = GL_NEAREST;
	gl.empty[slot] = texture;
	return texture;
}

/* ---------- draws */

static struct shader *draw_vertex;

static int draw_logged(void)
{
	return (debug.log_draws && gl.draws < 60) || (debug.log_frame && gl.frame == debug.log_frame_number);
}

/* why draws were not drawn, the first few times */
static void draw_refused(const char *why)
{
	static int refusals;

	if (refusals++ < 8)
		wiiu_log("gl: a draw not drawn (program %u): %s", gl.program, why);
}

/* everything a draw needs but its vertices; 0 if it cannot be drawn */
static int prepare_draw(void)
{
	struct program *program = object_get(gl.program, _object_program);
	struct shader *vertex, *fragment;
	int index;

	for (index = 0; index < debug.skip_count; index++)
	{
		if (debug.skip_programs[index] == gl.program)
			return 0;
	}
	if (!gl.ready || !wiiu_video_drawing() || !program || !program->linked)
	{
		draw_refused(!gl.ready ? "not ready" : !wiiu_video_drawing() ? "no foreground" : !program ? "no program" :
			"program not linked");
		return 0;
	}
	vertex = object_get(program->vertex, _object_shader);
	fragment = object_get(program->fragment, _object_shader);
	if (!vertex || !fragment || !vertex->vertex || !fragment->pixel)
	{
		draw_refused("a shader is missing");
		return 0;
	}
	/* (skip_fog: the passes drawn over the depth already there with one
	input, the atmosphere's) */
	if (debug.skip_fog && vertex->inputs == 1 && gl.depth_func == GL_EQUAL && gl.blend &&
		gl.blend_source == GL_ONE && gl.blend_destination == GL_ONE_MINUS_SRC_ALPHA)
	{
		return 0;
	}
	bind_targets();
	if (!gl.target_has_color && !gl.target_has_depth)
	{
		draw_refused("no target");
		return 0;
	}
	for (index = 0; index < program->uniform_count; index++)
	{
		struct uniform *uniform = &program->uniforms[index];
		struct texture *texture;
		struct sampler *sampler;
		GX2Sampler made;
		const struct sampler_parameters *parameters;
		const GLfloat *border;

		if (!uniform->sampler || uniform->sampler_location < 0)
			continue;
		texture = debug.white_textures ? NULL : unit_texture(uniform->unit, uniform->sampler_slot);
		if (texture && debug.only_tex && (unsigned int)texture->gx2.surface.format != debug.only_tex_format)
			texture = NULL;
		if (texture == gl.target_texture && texture)
			texture = NULL;
		if (!texture)
			texture = empty_texture(uniform->sampler_slot);
		if (!texture)
			continue;
		texture_flush_drawn(texture);
		sampler = uniform->unit >= 0 && uniform->unit < TEXTURE_UNITS ?
			object_get(gl.samplers[uniform->unit], _object_sampler) : NULL;
		parameters = sampler ? &sampler->parameters : &texture->sampler;
		sampler_make(&made, parameters, texture);
		border = parameters->border;
		if (draw_logged())
			wiiu_log("gl:     %s (unit %d, slot %d): %lux%lux%lu format 0x%x, %d of %d levels, filters 0x%x 0x%x, wrap 0x%x 0x%x%s",
				uniform->name, uniform->unit, uniform->sampler_location, texture->width, texture->height, texture->depth,
				(unsigned int)texture->gx2.surface.format, texture->defined_levels, texture->levels, parameters->min_filter,
				parameters->mag_filter, parameters->wrap_s, parameters->wrap_t, texture == gl.empty[uniform->sampler_slot] ?
				" (nothing bound)" : "");
		if (uniform->cube_array && texture->target == GL_TEXTURE_CUBE_MAP)
		{
			/* (the cube's faces as an array: its surface, seen as six
			slices) */
			if (!texture->array_view_made)
			{
				texture->array_view = texture->gx2;
				texture->array_view.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D_ARRAY;
				texture->array_view.viewNumSlices = 6;
				GX2InitTextureRegs(&texture->array_view);
				texture->array_view_made = 1;
			}
			GX2SetPixelTexture(&texture->array_view, (uint32_t)uniform->sampler_location);
		}
		else
			GX2SetPixelTexture(&texture->gx2, (uint32_t)uniform->sampler_location);
		texture_used(texture);
		GX2SetPixelSampler(&made, (uint32_t)uniform->sampler_location);
		if (parameters->wrap_s == GL_CLAMP_TO_BORDER || parameters->wrap_t == GL_CLAMP_TO_BORDER ||
			parameters->wrap_r == GL_CLAMP_TO_BORDER)
		{
			GX2SetPixelSamplerBorderColor((uint32_t)uniform->sampler_location, border[0], border[1], border[2],
				border[3]);
		}
	}
	apply_render_state();
	GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
	GX2SetVertexShader(vertex->vertex);
	GX2SetPixelShader(fragment->pixel);
	program_upload(program);
	if (draw_logged())
		wiiu_log("gl:   program %u, framebuffer %u (%lux%lu, color %d depth %d), viewport %d %d %d %d, depth %d 0x%x %d, "
			"blend %d 0x%x 0x%x, cull %d 0x%x 0x%x, scissor %d", gl.program, gl.draw_framebuffer, gl.target_width,
			gl.target_height, gl.target_has_color, gl.target_has_depth, gl.viewport[0], gl.viewport[1], gl.viewport[2],
			gl.viewport[3], gl.depth_test, gl.depth_func, gl.depth_mask, gl.blend, gl.blend_source,
			gl.blend_destination, gl.cull_face, gl.cull_mode, gl.front_face, gl.scissor_test);
	if (draw_logged())
	{
		/* (each program's shaders once, beside the log: p<program>.vert and
		.frag) */
		static GLuint written[64];
		static int written_count;
		int seen = 0, which;

		for (which = 0; which < written_count; which++)
			seen |= written[which] == gl.program;
		if (!seen && written_count < 64)
		{
			for (which = 0; which < 2; which++)
			{
				char path[128];
				FILE *file;

				snprintf(path, sizeof(path), WIIU_APP_FOLDER "/p%u.%s", gl.program, which ? "frag" : "vert");
				file = fopen(path, "wb");
				if (file)
				{
					fputs(which ? fragment->source : vertex->source, file);
					fclose(file);
				}
			}
			written[written_count++] = gl.program;
		}
		for (which = 0; which < program->uniform_count; which++)
		{
			const struct uniform *uniform = &program->uniforms[which];
			const uint32_t *values = uniform->pixel_offset >= 0 ? program->pixel_values + uniform->pixel_offset :
				uniform->vertex_offset >= 0 ? program->vertex_values + uniform->vertex_offset : NULL;
			float vector[4];

			if (uniform->sampler || !values)
				continue;
			if (!strcmp(uniform->name, "c"))
			{
				int element;

				for (element = 0; element < uniform->count && element < 64; element++)
				{
					memcpy(vector, values + element * 4, sizeof(vector));
					wiiu_log("gl:     c[%d] = %g %g %g %g", element, vector[0], vector[1], vector[2], vector[3]);
				}
				continue;
			}
			memcpy(vector, values, sizeof(vector));
			wiiu_log("gl:     uniform %s (%s %d, %d): %g %g %g %g", uniform->name, uniform->pixel_offset >= 0 ? "pixel" :
				"vertex", uniform->pixel_offset >= 0 ? uniform->pixel_offset : uniform->vertex_offset, uniform->count,
				vector[0], vector[1], vector[2], vector[3]);
		}
	}
	draw_vertex = vertex;
	return 1;
}

/* An attribute's GX2 format, the bytes it takes and the channels it gives
(missing ones 0, 0, 0, 1). GL_BGRA's size is four bytes in the order B, G,
R, A. */
static int attribute_format(const struct attribute *attribute, GX2AttribFormat *format, uint32_t *mask,
	unsigned long *bytes)
{
	int size = attribute->size == GL_BGRA ? 4 : attribute->size;
	uint32_t flags = attribute->integer ? GX2_ATTRIB_FLAG_INTEGER : attribute->normalized ? 0 : GX2_ATTRIB_FLAG_SCALED;
	static const uint32_t identity[4] =
	{
		GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1),
		GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1),
		GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_1),
		GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W),
	};

	if (size < 1 || size > 4)
		return 0;
	*mask = identity[size - 1];
	switch (attribute->type)
	{
	case GL_FLOAT:
	{
		static const GX2AttribFormat floats[4] =
		{
			GX2_ATTRIB_FORMAT_FLOAT_32, GX2_ATTRIB_FORMAT_FLOAT_32_32, GX2_ATTRIB_FORMAT_FLOAT_32_32_32,
			GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32,
		};

		*format = floats[size - 1];
		*bytes = 4 * (unsigned long)size;
		return 1;
	}
	case GL_UNSIGNED_INT:
	case GL_INT:
	{
		static const uint32_t types[4] =
		{
			GX2_ATTRIB_TYPE_32, GX2_ATTRIB_TYPE_32_32, GX2_ATTRIB_TYPE_32_32_32, GX2_ATTRIB_TYPE_32_32_32_32,
		};

		*format = (GX2AttribFormat)(types[size - 1] | flags | (attribute->type == GL_INT ? GX2_ATTRIB_FLAG_SIGNED : 0));
		*bytes = 4 * (unsigned long)size;
		return 1;
	}
	case GL_UNSIGNED_SHORT:
	case GL_SHORT:
		/* (one short is fetched as two, the second masked off: Cemu decodes
		no single 16-bit attribute, _emitAttributeDecodeGLSL's "unsupported
		fmt 05"; the word is the vertex's or a copy's, padded) */
		*format = (GX2AttribFormat)((size == 1 && debug.native_shorts ? GX2_ATTRIB_TYPE_16 :
			size <= 2 ? GX2_ATTRIB_TYPE_16_16 : GX2_ATTRIB_TYPE_16_16_16_16) | flags |
			(attribute->type == GL_SHORT ? GX2_ATTRIB_FLAG_SIGNED : 0));
		*bytes = 2 * (unsigned long)size;
		return 1;
	case GL_UNSIGNED_BYTE:
	case GL_BYTE:
		*format = (GX2AttribFormat)((size == 1 ? GX2_ATTRIB_TYPE_8 : size == 2 ? GX2_ATTRIB_TYPE_8_8 :
			GX2_ATTRIB_TYPE_8_8_8_8) | flags | (attribute->type == GL_BYTE ? GX2_ATTRIB_FLAG_SIGNED : 0));
		*bytes = (unsigned long)size;
		if (attribute->size == GL_BGRA)
			*mask = GX2_SEL_MASK(GX2_SQ_SEL_Z, GX2_SQ_SEL_Y, GX2_SQ_SEL_X, GX2_SQ_SEL_W);
		return 1;
	default:
		return 0;
	}
}

/* the byte swap an attribute's fetch takes: none for the window's (the
map's, little-endian), else the CPU's words' (a D3DCOLOR's four bytes are a
word the CPU wrote) */
static GX2EndianSwapMode attribute_swap(const struct attribute *attribute, int alias)
{
	if (alias)
		return GX2_ENDIAN_SWAP_NONE;
	switch (attribute->type)
	{
	case GL_SHORT: case GL_UNSIGNED_SHORT: return GX2_ENDIAN_SWAP_8_IN_16;
	case GL_BYTE: case GL_UNSIGNED_BYTE:
		return attribute->size == GL_BGRA || attribute->size == 4 ? GX2_ENDIAN_SWAP_8_IN_32 : GX2_ENDIAN_SWAP_NONE;
	default: return GX2_ENDIAN_SWAP_8_IN_32;
	}
}

/* the highest vertex a draw reads, plus one (an attribute copied: its
vertices) */
static unsigned long draw_vertex_end;

/* what the last draw gave GX2, for the log (draw_describe) */
static struct
{
	const void *address[16];
	unsigned long size[16], stride[16], slack[16];
	int buffers;
	GX2AttribStream streams[ATTRIBUTES];
	int stream_count;
} draw_record;

/* GX2SetAttribBuffer, its base put on GX2_VERTEX_BUFFER_ALIGNMENT (the
bytes below are the same memory's; slack is how far the vertices start
past it) */
static void attribute_buffer_set(int index, unsigned long size, unsigned long stride, const void *data)
{
	unsigned long slack = debug.unaligned_vertices ? 0 :
		(unsigned long)((uintptr_t)data & (GX2_VERTEX_BUFFER_ALIGNMENT - 1));

	GX2SetAttribBuffer((uint32_t)index, (uint32_t)(size + slack), (uint32_t)stride, (unsigned char *)data - slack);
	draw_record.address[index] = (const unsigned char *)data - slack;
	draw_record.size[index] = size + slack;
	draw_record.stride[index] = stride;
	draw_record.slack[index] = slack;
	if (index + 1 > draw_record.buffers)
		draw_record.buffers = index + 1;
}

static int setup_vertices(long base_vertex)
{
	GX2AttribStream streams[ATTRIBUTES];
	int count = 0, buffer_count = 0;
	int buffer_of_binding[BINDINGS];
	float *constants = NULL;
	int constant_buffer = -1, constant_count = 0;
	int index;

	for (index = 0; index < BINDINGS; index++)
		buffer_of_binding[index] = -1;
	memset(streams, 0, sizeof(streams));
	draw_record.buffers = 0;
	for (index = 0; index < ATTRIBUTES; index++)
	{
		struct attribute *attribute = &gl.attributes[index];
		GX2AttribStream *stream;
		unsigned long element_bytes;

		if (!((draw_vertex->inputs >> index) & 1))
			continue;
		stream = &streams[count++];
		stream->location = (uint32_t)index;
		stream->type = GX2_ATTRIB_INDEX_PER_VERTEX;
		if (attribute->enabled && attribute->binding < BINDINGS && gl.bindings[attribute->binding].buffer &&
			attribute_format(attribute, &stream->format, &stream->mask, &element_bytes))
		{
			struct binding *binding = &gl.bindings[attribute->binding];
			struct buffer *buffer = object_get(binding->buffer, _object_buffer);
			unsigned long stride = (unsigned long)binding->stride;

			if (!buffer || !buffer->data)
				return 0;
			stream->endianSwap = attribute_swap(attribute, buffer->alias);
			if (debug.copy_vertices || (attribute->relative_offset & 3) || (stride & 3))
			{
				/* (the GPU fetches whole words: an attribute not on a word of its
				vertex, such as a model vertex's node weight at byte 30, is copied
				to a buffer of its own, a word a vertex, as on the Xbox 360) */
				unsigned long slot = (element_bytes + 3) & ~3UL;
				unsigned long vertex_index;
				unsigned char *copy;

				if (!draw_vertex_end || element_bytes > 16 || buffer_count == 16)
					return 0;
				copy = ring_reserve(slot * draw_vertex_end);
				if (!copy)
					return 0;
				memset(copy, 0, slot * draw_vertex_end);
				for (vertex_index = 0; vertex_index < draw_vertex_end; vertex_index++)
				{
					long source = (long)binding->offset + ((long)vertex_index + base_vertex) * (long)stride +
						(long)attribute->relative_offset;

					if (source >= 0 && (unsigned long)source + element_bytes <= buffer->size)
						memcpy(copy + vertex_index * slot, buffer->data + source, element_bytes);
				}
				gpu_written(copy, slot * draw_vertex_end, GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER);
				attribute_buffer_set(buffer_count, slot * draw_vertex_end, slot, copy);
				stream->buffer = (uint32_t)buffer_count++;
				stream->offset = (uint32_t)draw_record.slack[stream->buffer];
			}
			else
			{
				if (buffer_of_binding[attribute->binding] < 0)
				{
					long start = (long)binding->offset + base_vertex * (long)stride;

					if (buffer_count == 16 || (unsigned long)binding->offset >= buffer->size)
						return 0;
					/* (read where it is: buffers are in memory the GPU reads; the
					base vertex is in the address, as on the Xbox 360, where the
					GPU's own took none below zero) */
					/* (on GX2_VERTEX_BUFFER_ALIGNMENT, wut's gx2/enum.h, as
					GX2R places buffers: the bytes below the vertices' start
					are the buffer's, then each attribute's offset is that
					much further on) */
					attribute_buffer_set(buffer_count, (unsigned long)((long)buffer->size - start), stride,
						buffer->data + start);
					buffer_of_binding[attribute->binding] = buffer_count++;
				}
				stream->buffer = (uint32_t)buffer_of_binding[attribute->binding];
				stream->offset = attribute->relative_offset + draw_record.slack[stream->buffer];
			}
			if (draw_logged())
			{
				const unsigned char *at = buffer->data + binding->offset + attribute->relative_offset;

				wiiu_log("gl:     attribute %d: size %d type 0x%x%s%s, offset %u, stride %d%s, format 0x%x swap %d: "
					"%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x", index, attribute->size, attribute->type,
					attribute->normalized ? " normalized" : "", attribute->integer ? " integer" : "",
					attribute->relative_offset, binding->stride, buffer->alias ? " (window)" : "",
					(unsigned int)stream->format, (int)stream->endianSwap, at[0], at[1], at[2], at[3], at[4], at[5],
					at[6], at[7], at[8], at[9], at[10], at[11]);
			}
		}
		else
		{
			/* a generic attribute: one value for every vertex, an instance's
			(GX2's per-instance fetch, the draw's one instance) */
			if (!constants)
			{
				if (buffer_count == 16)
					return 0;
				constants = ring_reserve(ATTRIBUTES * 16);
				if (!constants)
					return 0;
				constant_buffer = buffer_count++;
			}
			memcpy(constants + constant_count * 4, attribute->value, 16);
			stream->buffer = (uint32_t)constant_buffer;
			stream->offset = (uint32_t)(constant_count * 16);
			stream->format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32;
			stream->mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
			stream->type = GX2_ATTRIB_INDEX_PER_INSTANCE;
			stream->aluDivisor = 1;
			stream->endianSwap = GX2_ENDIAN_SWAP_8_IN_32;
			constant_count++;
			if (draw_logged())
				wiiu_log("gl:     attribute %d: constant %g %g %g %g", index, attribute->value[0], attribute->value[1],
					attribute->value[2], attribute->value[3]);
		}
	}
	if (constants)
	{
		gpu_written(constants, (unsigned long)constant_count * 16, GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER);
		attribute_buffer_set(constant_buffer, (unsigned long)constant_count * 16, (unsigned long)constant_count * 16,
			constants);
	}
	{
		GX2FetchShader *fetch = fetch_shader_get(streams, count);

		if (!fetch)
			return 0;
		GX2SetFetchShader(fetch);
	}
	memcpy(draw_record.streams, streams, sizeof(GX2AttribStream) * (size_t)count);
	draw_record.stream_count = count;
	return 1;
}

static GX2PrimitiveMode primitive_mode(GLenum mode, unsigned long *count)
{
	switch (mode)
	{
	case GL_POINTS: return GX2_PRIMITIVE_MODE_POINTS;
	case GL_LINES: *count -= *count % 2; return GX2_PRIMITIVE_MODE_LINES;
	case GL_LINE_STRIP: return GX2_PRIMITIVE_MODE_LINE_STRIP;
	case GL_TRIANGLE_STRIP: return GX2_PRIMITIVE_MODE_TRIANGLE_STRIP;
	case GL_TRIANGLE_FAN: return GX2_PRIMITIVE_MODE_TRIANGLE_FAN;
	default: *count -= *count % 3; return GX2_PRIMITIVE_MODE_TRIANGLES;
	}
}

/* whether a draw has an attribute that is copied (whose vertices the
indices must be read for) */
static int draw_copies_attributes(void)
{
	int index;

	for (index = 0; index < ATTRIBUTES; index++)
	{
		const struct attribute *attribute = &gl.attributes[index];

		if (((draw_vertex->inputs >> index) & 1) && attribute->enabled && attribute->binding < BINDINGS &&
			gl.bindings[attribute->binding].buffer && (debug.copy_vertices ||
			(attribute->relative_offset & 3) || (gl.bindings[attribute->binding].stride & 3)))
		{
			return 1;
		}
	}
	return 0;
}

/* A strip's or a fan's triangles as a list's indices, in the ring (16-bit
words while the vertices fit), in GL's order: in Cemu, the strips' triangles
came out wound the other way from a list's and a fan's (the main menu's
ring, drawn in strips, was culled where its text, drawn in fans, was not,
whichever winding GX2 was told was the front; as lists, both are drawn). A
list's winding is the same everywhere. The bring-up switch native_strips
draws strips as they are, to compare on the console. */
static const void *fan_as_list(unsigned long first, unsigned long count, const unsigned char *indices,
	unsigned long index_bytes, unsigned long *list_count, unsigned long *list_bytes, int strip)
{
	unsigned long triangles = count - 2, triangle, highest = first + count;
	unsigned long out_bytes;
	unsigned char *list;

	if (indices)
	{
		unsigned long index;

		highest = 0;
		for (index = 0; index < count; index++)
		{
			unsigned long value = index_bytes == 4 ? ((const uint32_t *)indices)[index] : ((const uint16_t *)indices)[index];

			if (value + 1 > highest)
				highest = value + 1;
		}
	}
	out_bytes = highest > 0xffff ? 4 : 2;
	list = ring_reserve(triangles * 3 * out_bytes);
	if (!list)
		return NULL;
	for (triangle = 0; triangle < triangles; triangle++)
	{
		unsigned long corners[3], corner;

		if (strip)
		{
			/* (GL's strip: every other triangle's first two corners
			exchanged) */
			corners[0] = triangle + (triangle & 1);
			corners[1] = triangle + 1 - (triangle & 1);
			corners[2] = triangle + 2;
		}
		else
		{
			corners[0] = 0;
			corners[1] = triangle + 1;
			corners[2] = triangle + 2;
		}
		for (corner = 0; corner < 3; corner++)
		{
			unsigned long value = indices ? (index_bytes == 4 ? ((const uint32_t *)indices)[corners[corner]] :
				((const uint16_t *)indices)[corners[corner]]) : first + corners[corner];

			if (out_bytes == 4)
				((uint32_t *)list)[triangle * 3 + corner] = (uint32_t)value;
			else
				((uint16_t *)list)[triangle * 3 + corner] = (uint16_t)value;
		}
	}
	gpu_written(list, triangles * 3 * out_bytes, GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER);
	*list_count = triangles * 3;
	*list_bytes = out_bytes;
	return list;
}

/* everything GX2 is given for a draw, in the log: the first draws, and
each program's first draw until the GPU has stopped (gpu_check_step names
the one it stopped at) */
static void draw_describe(GLenum mode, GX2PrimitiveMode primitive, unsigned long count, const void *indices,
	unsigned long index_bytes)
{
	int index;

	wiiu_log("gl: draw %lu (frame %lu, draw %lu of the frame), program %u: GL mode 0x%x as GX2 primitive %d, %lu %s",
		gl.draws + 1, gl.frame, gl.frame_draws + 1, gl.program, mode, (int)primitive, count,
		indices ? (index_bytes == 4 ? "indices, GX2_INDEX_TYPE_U32" : "indices, GX2_INDEX_TYPE_U16") : "vertices");
	if (indices)
	{
		unsigned long at, low = 0xffffffffUL, high = 0;

		for (at = 0; at < count; at++)
		{
			unsigned long value = index_bytes == 4 ? ((const uint32_t *)indices)[at] : ((const uint16_t *)indices)[at];

			if (value < low)
				low = value;
			if (value > high)
				high = value;
		}
		wiiu_log("gl:   indices at %p (physical %08lx, %lu past 0x20), from %lu to %lu: %lu %lu %lu %lu %lu %lu",
			indices, (unsigned long)OSEffectiveToPhysical((uint32_t)(uintptr_t)indices),
			(unsigned long)((uintptr_t)indices & 0x1f), low, high,
			(unsigned long)(index_bytes == 4 ? ((const uint32_t *)indices)[0] : ((const uint16_t *)indices)[0]),
			count > 1 ? (unsigned long)(index_bytes == 4 ? ((const uint32_t *)indices)[1] : ((const uint16_t *)indices)[1]) : 0UL,
			count > 2 ? (unsigned long)(index_bytes == 4 ? ((const uint32_t *)indices)[2] : ((const uint16_t *)indices)[2]) : 0UL,
			count > 3 ? (unsigned long)(index_bytes == 4 ? ((const uint32_t *)indices)[3] : ((const uint16_t *)indices)[3]) : 0UL,
			count > 4 ? (unsigned long)(index_bytes == 4 ? ((const uint32_t *)indices)[4] : ((const uint16_t *)indices)[4]) : 0UL,
			count > 5 ? (unsigned long)(index_bytes == 4 ? ((const uint32_t *)indices)[5] : ((const uint16_t *)indices)[5]) : 0UL);
	}
	for (index = 0; index < draw_record.buffers; index++)
		wiiu_log("gl:   vertex buffer %d at %p (physical %08lx, %lu past 0x40), %lu bytes (%lu vertices), stride %lu, "
			"the vertices %lu bytes in", index, draw_record.address[index],
			(unsigned long)OSEffectiveToPhysical((uint32_t)(uintptr_t)draw_record.address[index]),
			(unsigned long)((uintptr_t)draw_record.address[index] & 0x3f), draw_record.size[index],
			draw_record.stride[index] ? draw_record.size[index] / draw_record.stride[index] : 0UL,
			draw_record.stride[index], draw_record.slack[index]);
	for (index = 0; index < draw_record.stream_count; index++)
	{
		const GX2AttribStream *stream = &draw_record.streams[index];

		wiiu_log("gl:   fetch: location %u from buffer %u at %u, format 0x%x, %s, swap %d, mask %08x",
			(unsigned int)stream->location, (unsigned int)stream->buffer, (unsigned int)stream->offset,
			(unsigned int)stream->format, stream->type == GX2_ATTRIB_INDEX_PER_INSTANCE ? "per instance" : "per vertex",
			(int)stream->endianSwap, (unsigned int)stream->mask);
	}
	if (gl.bound_color)
		wiiu_log("gl:   color buffer at %p (physical %08lx), %ux%u, format 0x%x, tile mode %d, pitch %u, %u bytes, "
			"aligned 0x%x", gl.bound_color->surface.image,
			(unsigned long)OSEffectiveToPhysical((uint32_t)(uintptr_t)gl.bound_color->surface.image),
			(unsigned int)gl.bound_color->surface.width, (unsigned int)gl.bound_color->surface.height,
			(unsigned int)gl.bound_color->surface.format, (int)gl.bound_color->surface.tileMode,
			(unsigned int)gl.bound_color->surface.pitch, (unsigned int)gl.bound_color->surface.imageSize,
			(unsigned int)gl.bound_color->surface.alignment);
	if (gl.bound_depth)
		wiiu_log("gl:   depth buffer at %p (physical %08lx), %ux%u, format 0x%x, tile mode %d, %u bytes, aligned 0x%x, "
			"hi-z %p", gl.bound_depth->surface.image,
			(unsigned long)OSEffectiveToPhysical((uint32_t)(uintptr_t)gl.bound_depth->surface.image),
			(unsigned int)gl.bound_depth->surface.width, (unsigned int)gl.bound_depth->surface.height,
			(unsigned int)gl.bound_depth->surface.format, (int)gl.bound_depth->surface.tileMode,
			(unsigned int)gl.bound_depth->surface.imageSize, (unsigned int)gl.bound_depth->surface.alignment,
			gl.bound_depth->hiZPtr);
}

static void draw_steps(GLenum mode, unsigned long first, unsigned long count, GLenum index_type, const void *indices,
	long base_vertex)
{
	GX2PrimitiveMode primitive;
	const unsigned char *source = NULL;
	unsigned long index_bytes = index_type == GL_UNSIGNED_INT ? 4 : 2;
	int repeatable;

	again.valid = 0;
	debug_load();
	if (debug.no_draws || !count || !prepare_draw())
		return;
	primitive = primitive_mode(mode, &count);
	if (!count)
		return;
	if (index_type)
	{
		struct buffer *buffer = object_get(gl.element_buffer, _object_buffer);
		unsigned long offset = (unsigned long)(uintptr_t)indices;

		if (!buffer || !buffer->data || offset + count * index_bytes > buffer->size)
			return;
		source = buffer->data + offset;
		draw_vertex_end = 0;
		if (draw_copies_attributes())
		{
			unsigned long index;

			for (index = 0; index < count; index++)
			{
				unsigned long value = index_bytes == 4 ? ((const uint32_t *)source)[index] :
					((const uint16_t *)source)[index];

				if (value + 1 > draw_vertex_end)
					draw_vertex_end = value + 1;
			}
		}
	}
	else
	{
		draw_vertex_end = first + count;
		base_vertex = 0;
	}
	if (!setup_vertices(base_vertex))
		return;
	/* (its attribute buffers are the GL buffers themselves) */
	repeatable = !index_type && !draw_copies_attributes() && !debug.log_draws && !debug.gpu_check;
	if (primitive == GX2_PRIMITIVE_MODE_TRIANGLE_FAN ||
		(!debug.native_strips && primitive == GX2_PRIMITIVE_MODE_TRIANGLE_STRIP))
	{
		unsigned long list_count, list_bytes;
		const void *list;

		if (count < 3)
			return;
		list = fan_as_list(first, count, source, index_bytes, &list_count, &list_bytes,
			primitive == GX2_PRIMITIVE_MODE_TRIANGLE_STRIP);
		if (!list)
			return;
		source = list;
		count = list_count;
		index_bytes = list_bytes;
		primitive = GX2_PRIMITIVE_MODE_TRIANGLES;
	}
	if (draw_logged())
	{
		wiiu_log("gl: draw %lu: %s %lu from %lu, base %ld, mode 0x%x", gl.draws, source ? "indexed" : "arrays", count,
			first, base_vertex, mode);
		if (source && index_bytes == 2 && count >= 8)
		{
			struct buffer *buffer = object_get(gl.element_buffer, _object_buffer);

			wiiu_log("gl:     indices%s: %u %u %u %u %u %u %u %u", buffer && buffer->alias ? " (window)" : "",
				((const uint16_t *)source)[0], ((const uint16_t *)source)[1], ((const uint16_t *)source)[2],
				((const uint16_t *)source)[3], ((const uint16_t *)source)[4], ((const uint16_t *)source)[5],
				((const uint16_t *)source)[6], ((const uint16_t *)source)[7]);
		}
	}
	gpu_check_arm();
	{
		struct program *program = object_get(gl.program, _object_program);

		if ((debug.log_draws && gl.draws < 3) ||
			(debug.gpu_check && program && !program->gpu_checked && !watch.check_failed && !debug.no_gpu_check))
			draw_describe(mode, primitive, count, source, index_bytes);
	}
	if (source)
	{
		/* (the indices are the CPU's words, where they are) */
		GX2DrawIndexedEx(primitive, (uint32_t)count, index_bytes == 4 ? GX2_INDEX_TYPE_U32 : GX2_INDEX_TYPE_U16, source,
			0, 1);
	}
	else
		GX2DrawEx(primitive, (uint32_t)count, (uint32_t)first, 1);
	gl.draws++;
	gl.frame_draws++;
	targets_written();
	again.valid = repeatable;
	again.program = object_get(gl.program, _object_program);
	{
		struct program *program = object_get(gl.program, _object_program);
		int first = program && !program->gpu_checked;

		if (program)
			program->gpu_checked = 1;
		gpu_check_step(first ? (source ? "first indexed draw of a program" : "first draw of a program") :
			(source ? "indexed draw" : "draw"), first);
	}
}

static void draw(GLenum mode, unsigned long first, unsigned long count, GLenum index_type, const void *indices,
	long base_vertex)
{
	watch_step("a draw");
	draw_steps(mode, first, count, index_type, indices, base_vertex);
	watch_step("the game's own work");
}

/* The last draw again, its vertices from `first` (d3d8_gl.c's visibility
test runs, by name through SDL_GL_GetProcAddress): the caller has changed
nothing since but the bound buffers' bytes and the queries, so GX2 already
has the shaders, state and attribute buffers; only the draw is made (a fan's
or a strip's triangles as a list, as draw_steps makes them). A draw that
cannot be repeated so is made in full. */
static void console_gl_draw_again(GLenum mode, GLint first, GLsizei count)
{
	GX2PrimitiveMode primitive;
	unsigned long vertices = (unsigned long)count;

	if (!again.valid || first < 0 || object_get(gl.program, _object_program) != again.program ||
		!wiiu_video_drawing())
	{
		draw(mode, (unsigned long)first, (unsigned long)count, 0, NULL, 0);
		return;
	}
	watch_step("a draw");
	primitive = primitive_mode(mode, &vertices);
	if (primitive == GX2_PRIMITIVE_MODE_TRIANGLE_FAN ||
		(!debug.native_strips && primitive == GX2_PRIMITIVE_MODE_TRIANGLE_STRIP))
	{
		unsigned long list_count, list_bytes;
		const void *list = vertices >= 3 ? fan_as_list((unsigned long)first, vertices, NULL, 2, &list_count,
			&list_bytes, primitive == GX2_PRIMITIVE_MODE_TRIANGLE_STRIP) : NULL;

		if (list)
			GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, (uint32_t)list_count,
				list_bytes == 4 ? GX2_INDEX_TYPE_U32 : GX2_INDEX_TYPE_U16, list, 0, 1);
	}
	else if (vertices)
		GX2DrawEx(primitive, (uint32_t)vertices, (uint32_t)first, 1);
	gl.draws++;
	gl.frame_draws++;
	targets_written();
	watch_step("the game's own work");
}

/* ---------- quads of this layer's own: blits and clears */

static const char blit_vertex_source[] =
	"#version 450 core\n"
	"layout(location = 0) in vec4 corner;\n"
	"out vec2 blit_uv;\n"
	"void main()\n"
	"{\n"
	"\tblit_uv = corner.zw;\n"
	"\tgl_Position = vec4(corner.xy, 0.5, 1.0);\n"
	"}\n";

static const char blit_pixel_source[] =
	"#version 450 core\n"
	"uniform sampler2D blit_source;\n"
	"in vec2 blit_uv;\n"
	"layout(location = 0) out vec4 blit_color;\n"
	"void main()\n"
	"{\n"
	"\tblit_color = texture(blit_source, blit_uv);\n"
	"}\n";

static const char clear_pixel_source[] =
	"#version 450 core\n"
	"uniform vec4 clear_color;\n"
	"layout(location = 0) out vec4 blit_color;\n"
	"void main()\n"
	"{\n"
	"\tblit_color = clear_color;\n"
	"}\n";

static struct shader *internal_shader(GLenum type, const char *source)
{
	struct shader *shader = calloc(1, sizeof(*shader));

	if (!shader)
		return NULL;
	shader->type = type;
	shader->source = strdup(source);
	if (!shader_compile(shader))
	{
		wiiu_log("gl: the layer's own %s shader did not compile", type == GL_VERTEX_SHADER ? "vertex" : "fragment");
		return NULL;
	}
	shader_describe(shader);
	return shader;
}

/* a quad drawn with the layer's own shaders, into what is bound: corners
in clip space (x0, y0)-(x1, y1), texture coordinates (u0, v0)-(u1, v1) */
static void quad_draw(struct shader *pixel, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
	float v1)
{
	float *corners = ring_reserve(4 * 4 * sizeof(float));
	GX2AttribStream stream;
	GX2FetchShader *fetch;
	int corner;

	again.valid = 0;
	if (!corners || !gl.blit_vertex || !pixel)
		return;
	for (corner = 0; corner < 4; corner++)
	{
		corners[corner * 4 + 0] = (corner & 1) ? x1 : x0;
		corners[corner * 4 + 1] = (corner & 2) ? y1 : y0;
		corners[corner * 4 + 2] = (corner & 1) ? u1 : u0;
		corners[corner * 4 + 3] = (corner & 2) ? v1 : v0;
	}
	gpu_written(corners, 4 * 4 * sizeof(float), GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER);
	memset(&stream, 0, sizeof(stream));
	stream.location = 0;
	stream.format = GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32;
	stream.type = GX2_ATTRIB_INDEX_PER_VERTEX;
	stream.mask = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
	stream.endianSwap = GX2_ENDIAN_SWAP_8_IN_32;
	fetch = fetch_shader_get(&stream, 1);
	if (!fetch)
		return;
	GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
	GX2SetFetchShader(fetch);
	GX2SetVertexShader(gl.blit_vertex->vertex);
	GX2SetPixelShader(pixel->pixel);
	GX2SetAttribBuffer(0, 4 * 4 * sizeof(float), 4 * sizeof(float), corners);
	GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
	/* (the registers are the quad's now) */
	gl.uploaded_program = 0;
}

/* the state for a quad: no depth, stencil, blend or culling */
static void quad_state(unsigned long width, unsigned long height, int channels)
{
	GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
	GX2SetTargetChannelMasks((GX2ChannelMask)channels, 0, 0, 0, 0, 0, 0, 0);
	GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
		GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_COMPARE_FUNC_ALWAYS,
		GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
	GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE,
		FALSE, FALSE, FALSE);
	GX2SetViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
	GX2SetScissor(0, 0, (uint32_t)width, (uint32_t)height);
}

/* ---------- the device */

static int gl_ready(void)
{
	if (gl.ready)
		return 1;
	if (!wiiu_video_screen_color())
		return 0;
	state_defaults();
	gl.start = OSGetSystemTime();
	gl.ring = gpu_allocate(RING_SIZE, 0x100);
	if (!gl.ring)
	{
		wiiu_log("gl: no memory for the draw data ring");
		return 0;
	}
	gl.ready = 1;
	gl.blit_vertex = internal_shader(GL_VERTEX_SHADER, blit_vertex_source);
	gl.blit_pixel = internal_shader(GL_FRAGMENT_SHADER, blit_pixel_source);
	gl.clear_pixel = internal_shader(GL_FRAGMENT_SHADER, clear_pixel_source);
	gl.blit_source_location = gl.blit_pixel && gl.blit_pixel->pixel->samplerVarCount ?
		(int)gl.blit_pixel->pixel->samplerVars[0].location : 0;
	gl.clear_color_offset = gl.clear_pixel && gl.clear_pixel->pixel->uniformVarCount ?
		(int)gl.clear_pixel->pixel->uniformVars[0].offset : 0;
	/* (OpenGL's clipping: depth -1 to 1, which the vertex shaders' end
	makes it) */
	GX2SetRasterizerClipControl(TRUE, TRUE);
	wiiu_log("gl: ready on GX2 (ring %u MB, GX2's GPU timeout %u ms)", RING_SIZE >> 20, (unsigned int)GX2GetGPUTimeout());
	/* (the switches in the log from the start) */
	debug_load();
	watch_step("the game's own work");
	watch_start();
	return 1;
}

/* ---------- the entry points: state */

static void APIENTRY wiiu_glEnable(GLenum cap)
{
	switch (cap)
	{
	case GL_BLEND: gl.blend = 1; break;
	case GL_DEPTH_TEST: gl.depth_test = 1; break;
	case GL_STENCIL_TEST: gl.stencil_test = 1; break;
	case GL_CULL_FACE: gl.cull_face = 1; break;
	case GL_SCISSOR_TEST: gl.scissor_test = 1; break;
	case GL_POLYGON_OFFSET_FILL: gl.polygon_offset_fill = 1; break;
	default: break;
	}
}

static void APIENTRY wiiu_glDisable(GLenum cap)
{
	switch (cap)
	{
	case GL_BLEND: gl.blend = 0; break;
	case GL_DEPTH_TEST: gl.depth_test = 0; break;
	case GL_STENCIL_TEST: gl.stencil_test = 0; break;
	case GL_CULL_FACE: gl.cull_face = 0; break;
	case GL_SCISSOR_TEST: gl.scissor_test = 0; break;
	case GL_POLYGON_OFFSET_FILL: gl.polygon_offset_fill = 0; break;
	default: break;
	}
}

static GLboolean APIENTRY wiiu_glIsEnabled(GLenum cap)
{
	switch (cap)
	{
	case GL_BLEND: return (GLboolean)gl.blend;
	case GL_DEPTH_TEST: return (GLboolean)gl.depth_test;
	case GL_STENCIL_TEST: return (GLboolean)gl.stencil_test;
	case GL_CULL_FACE: return (GLboolean)gl.cull_face;
	case GL_SCISSOR_TEST: return (GLboolean)gl.scissor_test;
	case GL_POLYGON_OFFSET_FILL: return (GLboolean)gl.polygon_offset_fill;
	default: return GL_FALSE;
	}
}

static void APIENTRY wiiu_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
	gl.blend_color[0] = r; gl.blend_color[1] = g; gl.blend_color[2] = b; gl.blend_color[3] = a;
}
static void APIENTRY wiiu_glBlendEquation(GLenum mode) { gl.blend_equation = gl.blend_equation_alpha = mode; }
static void APIENTRY wiiu_glBlendFunc(GLenum source, GLenum destination)
{
	gl.blend_source = gl.blend_source_alpha = source;
	gl.blend_destination = gl.blend_destination_alpha = destination;
}
static void APIENTRY wiiu_glBlendFuncSeparate(GLenum source, GLenum destination, GLenum source_alpha,
	GLenum destination_alpha)
{
	gl.blend_source = source;
	gl.blend_destination = destination;
	gl.blend_source_alpha = source_alpha;
	gl.blend_destination_alpha = destination_alpha;
}
static void APIENTRY wiiu_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a)
{
	gl.clear_color[0] = r; gl.clear_color[1] = g; gl.clear_color[2] = b; gl.clear_color[3] = a;
}
static void APIENTRY wiiu_glClearDepth(GLclampd depth) { gl.clear_depth = (float)depth; }
static void APIENTRY wiiu_glClearStencil(GLint s) { gl.clear_stencil = s; }
static void APIENTRY wiiu_glClipControl(GLenum origin, GLenum depth) { (void)origin; (void)depth; }
static void APIENTRY wiiu_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	gl.color_mask[0] = r; gl.color_mask[1] = g; gl.color_mask[2] = b; gl.color_mask[3] = a;
}
static void APIENTRY wiiu_glCullFace(GLenum mode) { gl.cull_mode = mode; }
static void APIENTRY wiiu_glFrontFace(GLenum mode) { gl.front_face = mode; }
static void APIENTRY wiiu_glDepthFunc(GLenum func) { gl.depth_func = func; }
static void APIENTRY wiiu_glDepthMask(GLboolean flag) { gl.depth_mask = flag; }
static void APIENTRY wiiu_glDepthRange(GLclampd near_value, GLclampd far_value)
{
	gl.depth_near = (float)near_value;
	gl.depth_far = (float)far_value;
}
static void APIENTRY wiiu_glStencilFunc(GLenum func, GLint reference, GLuint mask)
{
	gl.stencil_func = func;
	gl.stencil_reference = reference;
	gl.stencil_mask = mask;
}
static void APIENTRY wiiu_glStencilMask(GLuint mask) { gl.stencil_write_mask = mask; }
static void APIENTRY wiiu_glStencilOp(GLenum fail, GLenum zfail, GLenum zpass)
{
	gl.stencil_fail = fail;
	gl.stencil_zfail = zfail;
	gl.stencil_zpass = zpass;
}
static void APIENTRY wiiu_glLineWidth(GLfloat width) { if (gl.ready) GX2SetLineWidth(width); }
static void APIENTRY wiiu_glPolygonMode(GLenum face, GLenum mode) { (void)face; gl.polygon_mode = mode; }
static void APIENTRY wiiu_glPolygonOffset(GLfloat factor, GLfloat units)
{
	gl.polygon_offset_factor = factor;
	gl.polygon_offset_units = units;
}
static void APIENTRY wiiu_glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	gl.scissor[0] = x; gl.scissor[1] = y; gl.scissor[2] = width; gl.scissor[3] = height;
}
static void APIENTRY wiiu_glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	gl.viewport[0] = x; gl.viewport[1] = y; gl.viewport[2] = width; gl.viewport[3] = height;
}
static void APIENTRY wiiu_glPixelStorei(GLenum name, GLint value) { (void)name; (void)value; }
static void APIENTRY wiiu_glFinish(void) { if (gl.ready) gpu_done("glFinish"); }
static void APIENTRY wiiu_glFlush(void) { if (gl.ready) GX2Flush(); }
static void APIENTRY wiiu_glMemoryBarrier(GLbitfield barriers) { (void)barriers; }
static GLenum APIENTRY wiiu_glGetError(void) { return GL_NO_ERROR; }

static void APIENTRY wiiu_glGetIntegerv(GLenum name, GLint *value)
{
	switch (name)
	{
	case GL_MAJOR_VERSION: *value = 4; break;
	case GL_MINOR_VERSION: *value = 5; break;
	case GL_MAX_SAMPLES: *value = 1; break;
	case GL_MAX_TEXTURE_SIZE: *value = 8192; break;
	case GL_MAX_RENDERBUFFER_SIZE: *value = 8192; break;
	case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: *value = 16; break;
	case GL_VIEWPORT: memcpy(value, gl.viewport, sizeof(gl.viewport)); break;
	case GL_SCISSOR_BOX: memcpy(value, gl.scissor, sizeof(gl.scissor)); break;
	case GL_DRAW_FRAMEBUFFER_BINDING: *value = (GLint)gl.draw_framebuffer; break;
	case GL_READ_FRAMEBUFFER_BINDING: *value = (GLint)gl.read_framebuffer; break;
	case GL_CURRENT_PROGRAM: *value = (GLint)gl.program; break;
	case GL_ACTIVE_TEXTURE: *value = (GLint)(GL_TEXTURE0 + gl.active_unit); break;
	case GL_TEXTURE_BINDING_2D: *value = (GLint)*bound_texture_slot(GL_TEXTURE_2D); break;
	case GL_ARRAY_BUFFER_BINDING: *value = (GLint)gl.array_buffer; break;
	case GL_ELEMENT_ARRAY_BUFFER_BINDING: *value = (GLint)gl.element_buffer; break;
	default: *value = 0; break;
	}
}

static const GLubyte *APIENTRY wiiu_glGetString(GLenum name)
{
	switch (name)
	{
	case GL_VENDOR: return (const GLubyte *)"ChupathingyCE";
	case GL_RENDERER: return (const GLubyte *)"Wii U Latte (GX2)";
	case GL_VERSION: return (const GLubyte *)"4.5 (on GX2)";
	case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"4.50 (CafeGLSL)";
	default: return (const GLubyte *)"";
	}
}

/* ---------- the entry points: textures and samplers */

static void APIENTRY wiiu_glGenTextures(GLsizei n, GLuint *names)
{
	GLsizei index;

	generate(n, names, _object_texture, sizeof(struct texture));
	for (index = 0; index < n; index++)
	{
		struct texture *texture = object_get(names[index], _object_texture);

		if (texture)
			texture_defaults(texture);
	}
}

static void APIENTRY wiiu_glDeleteTextures(GLsizei n, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < n; index++)
	{
		struct texture *texture = object_get(names[index], _object_texture);
		int unit, slot;

		if (!texture)
			continue;
		texture_release(texture);
		free(texture);
		objects[names[index]].type = _object_none;
		objects[names[index]].data = NULL;
		for (unit = 0; unit < TEXTURE_UNITS; unit++)
		{
			for (slot = 0; slot < 3; slot++)
			{
				if (gl.textures[unit][slot] == names[index])
					gl.textures[unit][slot] = 0;
			}
		}
	}
}

static void APIENTRY wiiu_glActiveTexture(GLenum unit) { gl.active_unit = unit - GL_TEXTURE0; }
static void APIENTRY wiiu_glBindTexture(GLenum target, GLuint name) { *bound_texture_slot(target) = name; }

/* (GL 4.4's multi-bind: each unit's texture bound to the target it was
made as; none unbinds all of the unit's targets) */
static void APIENTRY wiiu_glBindTextures(GLuint first, GLsizei count, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < count; index++)
	{
		GLuint unit = first + (GLuint)index;
		GLuint name = names ? names[index] : 0;
		struct texture *texture = object_get(name, _object_texture);

		if (unit >= TEXTURE_UNITS)
			break;
		if (!name)
		{
			gl.textures[unit][0] = gl.textures[unit][1] = gl.textures[unit][2] = 0;
			continue;
		}
		if (!texture)
			continue;
		gl.textures[unit][texture->target == GL_TEXTURE_3D ? 1 : texture->target == GL_TEXTURE_CUBE_MAP ? 2 : 0] = name;
	}
}

static void APIENTRY wiiu_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
	GLint border, GLenum format, GLenum type, const GLvoid *pixels)
{
	(void)border;
	(void)type;
	image(target, level, internal, width, height, 1, format, pixels);
}

static void APIENTRY wiiu_glTexImage3D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height,
	GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
	(void)border;
	(void)type;
	image(target, level, internal, width, height, depth, format, pixels);
}

static void APIENTRY wiiu_glCompressedTexImage2D(GLenum target, GLint level, GLenum format, GLsizei width,
	GLsizei height, GLint border, GLsizei size, const void *data)
{
	(void)border;
	(void)size;
	image(target, level, (GLint)format, width, height, 1, 0, data);
}

static void APIENTRY wiiu_glCompressedTexImage3D(GLenum target, GLint level, GLenum format, GLsizei width,
	GLsizei height, GLsizei depth, GLint border, GLsizei size, const void *data)
{
	(void)border;
	(void)size;
	image(target, level, (GLint)format, width, height, depth, 0, data);
}

static void APIENTRY wiiu_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
	GLsizei height, GLenum format, GLenum type, const GLvoid *pixels)
{
	struct texture *texture = bound_texture(target);
	OSTime start = OSGetSystemTime();

	(void)type;
	if (texture)
		texture_upload(texture, target, level, (unsigned long)x, (unsigned long)y, 0, (unsigned long)width,
			(unsigned long)height, 1, pixels, format);
	watch.texture_time += OSGetSystemTime() - start;
	watch.textures++;
}

static void texture_parameter(struct texture *texture, struct sampler_parameters *sampler, GLenum name,
	const GLint *integer, const GLfloat *real)
{
	GLint value = integer ? *integer : (GLint)*real;
	GLfloat value_float = real ? *real : (GLfloat)*integer;

	switch (name)
	{
	case GL_TEXTURE_MIN_FILTER: sampler->min_filter = value; break;
	case GL_TEXTURE_MAG_FILTER: sampler->mag_filter = value; break;
	case GL_TEXTURE_WRAP_S: sampler->wrap_s = value; break;
	case GL_TEXTURE_WRAP_T: sampler->wrap_t = value; break;
	case GL_TEXTURE_WRAP_R: sampler->wrap_r = value; break;
	case GL_TEXTURE_LOD_BIAS: sampler->lod_bias = value_float; break;
	case GL_TEXTURE_MIN_LOD: sampler->min_lod = value_float; break;
	case GL_TEXTURE_MAX_LOD: sampler->max_lod = value_float; break;
	case GL_TEXTURE_MAX_ANISOTROPY_EXT: sampler->anisotropy = value_float; break;
	case GL_TEXTURE_BORDER_COLOR:
		if (real)
			memcpy(sampler->border, real, sizeof(sampler->border));
		break;
	case GL_TEXTURE_BASE_LEVEL: if (texture) texture->base_level = value; break;
	case GL_TEXTURE_MAX_LEVEL: if (texture) texture->max_level = value; break;
	case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_G: case GL_TEXTURE_SWIZZLE_B: case GL_TEXTURE_SWIZZLE_A:
		if (texture)
		{
			texture->swizzle[name - GL_TEXTURE_SWIZZLE_R] = value;
			texture_component_map(texture);
		}
		break;
	default: break;
	}
}

static void APIENTRY wiiu_glTexParameteri(GLenum target, GLenum name, GLint value)
{
	struct texture *texture = bound_texture(target);

	if (texture)
		texture_parameter(texture, &texture->sampler, name, &value, NULL);
}
static void APIENTRY wiiu_glTexParameteriv(GLenum target, GLenum name, const GLint *value)
{
	struct texture *texture = bound_texture(target);

	if (texture)
		texture_parameter(texture, &texture->sampler, name, value, NULL);
}
static void APIENTRY wiiu_glTexParameterf(GLenum target, GLenum name, GLfloat value)
{
	struct texture *texture = bound_texture(target);

	if (texture)
		texture_parameter(texture, &texture->sampler, name, NULL, &value);
}
static void APIENTRY wiiu_glTexParameterfv(GLenum target, GLenum name, const GLfloat *value)
{
	struct texture *texture = bound_texture(target);

	if (texture)
		texture_parameter(texture, &texture->sampler, name, NULL, value);
}

static void APIENTRY wiiu_glGenSamplers(GLsizei n, GLuint *names)
{
	GLsizei index;

	generate(n, names, _object_sampler, sizeof(struct sampler));
	for (index = 0; index < n; index++)
	{
		struct sampler *sampler = object_get(names[index], _object_sampler);

		if (sampler)
			sampler_defaults(&sampler->parameters);
	}
}
static void APIENTRY wiiu_glBindSampler(GLuint unit, GLuint sampler)
{
	if (unit < TEXTURE_UNITS)
		gl.samplers[unit] = sampler;
}
static void APIENTRY wiiu_glSamplerParameteri(GLuint name, GLenum parameter, GLint value)
{
	struct sampler *sampler = object_get(name, _object_sampler);

	if (sampler)
		texture_parameter(NULL, &sampler->parameters, parameter, &value, NULL);
}
static void APIENTRY wiiu_glSamplerParameterf(GLuint name, GLenum parameter, GLfloat value)
{
	struct sampler *sampler = object_get(name, _object_sampler);

	if (sampler)
		texture_parameter(NULL, &sampler->parameters, parameter, NULL, &value);
}
static void APIENTRY wiiu_glSamplerParameterfv(GLuint name, GLenum parameter, const GLfloat *value)
{
	struct sampler *sampler = object_get(name, _object_sampler);

	if (sampler)
		texture_parameter(NULL, &sampler->parameters, parameter, NULL, value);
}

static void APIENTRY wiiu_glGenerateMipmap(GLenum target)
{
	struct texture *texture = bound_texture(target);
	OSTime start = OSGetSystemTime();

	if (texture)
		generate_mipmaps(texture);
	watch.texture_time += OSGetSystemTime() - start;
}

static void APIENTRY wiiu_glGetTexImage(GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels)
{
	FIRST_USE();
	(void)target; (void)level; (void)format; (void)type; (void)pixels;
}

/* ---------- the entry points: framebuffers */

static void APIENTRY wiiu_glGenFramebuffers(GLsizei n, GLuint *names)
{
	generate(n, names, _object_framebuffer, sizeof(struct framebuffer));
}

static void APIENTRY wiiu_glDeleteFramebuffers(GLsizei n, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < n; index++)
	{
		void *framebuffer = object_get(names[index], _object_framebuffer);

		if (!framebuffer)
			continue;
		free(framebuffer);
		objects[names[index]].type = _object_none;
		if (gl.draw_framebuffer == names[index])
			gl.draw_framebuffer = 0;
		if (gl.read_framebuffer == names[index])
			gl.read_framebuffer = 0;
	}
}

static void APIENTRY wiiu_glBindFramebuffer(GLenum target, GLuint framebuffer)
{
	if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
		gl.draw_framebuffer = framebuffer;
	if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
		gl.read_framebuffer = framebuffer;
}

static void APIENTRY wiiu_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum texture_target,
	GLuint texture, GLint level)
{
	struct framebuffer *framebuffer = object_get(target == GL_READ_FRAMEBUFFER ? gl.read_framebuffer :
		gl.draw_framebuffer, _object_framebuffer);

	(void)texture_target;
	if (!framebuffer)
		return;
	if (attachment == GL_COLOR_ATTACHMENT0)
	{
		framebuffer->color = texture;
		framebuffer->color_level = level;
	}
	else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT || attachment == GL_DEPTH_ATTACHMENT)
		framebuffer->depth = texture;
}

static void APIENTRY wiiu_glDrawBuffers(GLsizei n, const GLenum *buffers) { (void)n; (void)buffers; }
static GLenum APIENTRY wiiu_glCheckFramebufferStatus(GLenum target) { (void)target; return GL_FRAMEBUFFER_COMPLETE; }

static void APIENTRY wiiu_glGenRenderbuffers(GLsizei n, GLuint *names)
{
	generate(n, names, _object_renderbuffer, sizeof(struct renderbuffer));
}
static void APIENTRY wiiu_glBindRenderbuffer(GLenum target, GLuint renderbuffer)
{
	(void)target;
	gl.renderbuffer = renderbuffer;
}
static void APIENTRY wiiu_glRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum format,
	GLsizei width, GLsizei height)
{
	FIRST_USE();
	(void)target; (void)samples; (void)format; (void)width; (void)height;
}
static void APIENTRY wiiu_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffer_target,
	GLuint renderbuffer)
{
	FIRST_USE();
	(void)target; (void)attachment; (void)renderbuffer_target; (void)renderbuffer;
}

/* the whole target, or a scissored part: a quad then (GX2's clears take
the whole buffer) */
static void APIENTRY wiiu_glClear(GLbitfield mask)
{
	int color, depth, whole, channels;

	if (!gl_ready() || !wiiu_video_drawing())
		return;
	bind_targets();
	color = (mask & GL_COLOR_BUFFER_BIT) && gl.target_has_color && gl.bound_color;
	depth = (mask & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)) && gl.target_has_depth && gl.bound_depth;
	if (!color && !depth)
		return;
	whole = !gl.scissor_test || (gl.scissor[0] <= 0 && gl.scissor[1] <= 0 &&
		gl.scissor[0] + gl.scissor[2] >= (GLint)gl.target_width && gl.scissor[1] + gl.scissor[3] >= (GLint)gl.target_height);
	channels = (gl.color_mask[0] ? GX2_CHANNEL_MASK_R : 0) | (gl.color_mask[1] ? GX2_CHANNEL_MASK_G : 0) |
		(gl.color_mask[2] ? GX2_CHANNEL_MASK_B : 0) | (gl.color_mask[3] ? GX2_CHANNEL_MASK_A : 0);
	if ((debug.log_draws && gl.draws < 60) || (debug.trace && gl.frame < 12))
		wiiu_log("gl: clear 0x%x (color %.2f %.2f %.2f %.2f channels 0x%x, depth %.2f) %s, framebuffer %u (%lux%lu)",
			(unsigned int)mask, gl.clear_color[0], gl.clear_color[1], gl.clear_color[2], gl.clear_color[3], channels,
			gl.clear_depth, whole ? "whole" : "scissored", gl.draw_framebuffer, gl.target_width, gl.target_height);
	if (color && !channels)
		color = 0;
	debug_load();
	if (color && whole && channels == GX2_CHANNEL_MASK_RGBA)
	{
		if (debug.clear_red && gl.draw_framebuffer)
			GX2ClearColor(gl.bound_color, 1.0f, 0.0f, 0.0f, 1.0f);
		else
			GX2ClearColor(gl.bound_color, gl.clear_color[0], gl.clear_color[1], gl.clear_color[2], gl.clear_color[3]);
		/* (GX2's clears leave the context state unset: libwhb's
		WHBGfxClearColor) */
		gx2_own_work_done();
		bind_targets();
	}
	else if (color && gl.clear_pixel)
	{
		/* (some channels, or through the scissor: a quad of the clear
		color, GX2's clear taking every channel of the whole buffer) */
		uint32_t values[4];

		quad_state(gl.target_width, gl.target_height, channels);
		apply_scissor();
		memcpy(values, gl.clear_color, sizeof(values));
		GX2SetPixelUniformReg((uint32_t)gl.clear_color_offset, 4, values);
		quad_draw(gl.clear_pixel, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
	}
	if (depth)
	{
		GX2ClearFlags flags = (GX2ClearFlags)(((mask & GL_DEPTH_BUFFER_BIT) ? GX2_CLEAR_FLAGS_DEPTH : 0) |
			((mask & GL_STENCIL_BUFFER_BIT) ? GX2_CLEAR_FLAGS_STENCIL : 0));

		if (!whole)
			FIRST_USE();
		GX2ClearDepthStencilEx(gl.bound_depth, gl.clear_depth, (uint8_t)gl.clear_stencil, flags);
		gx2_own_work_done();
		bind_targets();
	}
	if (color)
		targets_written();
	/* (in the frames gpu_check covers, from the second frame that draws on;
	and the first clear of each framebuffer in any frame) */
	if (gl.draws)
		gpu_check_arm();
	{
		static GLuint cleared[32];
		static int cleared_count;
		int seen = 0, at;

		for (at = 0; at < cleared_count; at++)
			seen |= cleared[at] == gl.draw_framebuffer;
		if (!seen && cleared_count < 32)
		{
			cleared[cleared_count++] = gl.draw_framebuffer;
			if (debug.gpu_check && !watch.check_failed && !debug.no_gpu_check)
				wiiu_log("gl: framebuffer %u's first clear (%s%s%s), color %p tile mode %d, depth %p tile mode %d",
					gl.draw_framebuffer, color ? "color" : "", color && depth ? " and " : "", depth ? "depth" : "",
					color ? gl.bound_color->surface.image : NULL, color ? (int)gl.bound_color->surface.tileMode : -1,
					depth ? gl.bound_depth->surface.image : NULL, depth ? (int)gl.bound_depth->surface.tileMode : -1);
		}
		gpu_check_step(color && depth ? "clear of color and depth" : color ? "clear of color" : "clear of depth",
			!seen);
	}
}


/* Bring-up (cube_probe): which texel of which face a cube map's lookup
reads. A 4x4 cube whose texels say their face (red: 40 a face, from 40)
and place (green: column, blue: row, 30 + 60 each), sampled in 24
directions (each face's major axis, the other two at -0.5 and 0.5) into a
row of pixels that are read back and logged, to compare with OpenGL's
rule (the specification's table of the major axis' sc and tc). */
static const char probe_pixel_source[] =
	"#version 450 core\n"
	"uniform samplerCube tex0;\n"
	"uniform vec4 probe_direction;\n"
	"layout(location = 0) out vec4 probe_color;\n"
	"void main()\n"
	"{\n"
	"\tprobe_color = texture(tex0, probe_direction.xyz);\n"
	"\tif (probe_direction.w > 0.5) probe_color = vec4(probe_direction.xyz * 0.25 + 0.5, 1.0);\n"
	"}\n";

static void cube_probe_run(void)
{
	struct texture *cube = calloc(1, sizeof(*cube)), *row = calloc(1, sizeof(*row));
	struct shader *pixel;
	GX2ColorBuffer *buffer;
	GX2Sampler sampler;
	unsigned char texels[4 * 4 * 4];
	int face, probe, sampler_location = 0, direction_offset = 0;
	unsigned long pitch, slice;
	const unsigned char *read;

	if (!cube || !row)
		return;
	texture_defaults(cube);
	texture_defaults(row);
	pixel = internal_shader(GL_FRAGMENT_SHADER, probe_pixel_source);
	if (!pixel || !texture_allocate(cube, GL_TEXTURE_CUBE_MAP, GL_RGBA8, 4, 4, 1, 0) ||
		!texture_allocate(row, GL_TEXTURE_2D, GL_RGBA8, 48, 1, 1, 0))
	{
		wiiu_log("gl: cube probe: not made");
		return;
	}
	if (pixel->pixel->samplerVarCount)
		sampler_location = (int)pixel->pixel->samplerVars[0].location;
	if (pixel->pixel->uniformVarCount)
		direction_offset = (int)pixel->pixel->uniformVars[0].offset;
	for (face = 0; face < 6; face++)
	{
		int x, y;

		for (y = 0; y < 4; y++)
		{
			for (x = 0; x < 4; x++)
			{
				unsigned char *texel = texels + (y * 4 + x) * 4;

				texel[0] = (unsigned char)(40 * (face + 1));
				texel[1] = (unsigned char)(30 + 60 * x);
				texel[2] = (unsigned char)(30 + 60 * y);
				texel[3] = 255;
			}
		}
		texture_upload(cube, GL_TEXTURE_CUBE_MAP_POSITIVE_X + (GLenum)face, 0, 0, 0, 0, 4, 4, 1, texels, GL_RGBA);
	}
	buffer = texture_color_buffer(row, 0);
	if (!buffer)
		return;
	GX2SetColorBuffer(buffer, GX2_RENDER_TARGET_0);
	gl.bound_color = buffer;
	GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
	GX2InitSamplerLOD(&sampler, 0.0f, 0.0f, 0.0f);
	if (pixel->cube_arrays)
	{
		cube->array_view = cube->gx2;
		cube->array_view.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D_ARRAY;
		cube->array_view.viewNumSlices = 6;
		GX2InitTextureRegs(&cube->array_view);
		GX2SetPixelTexture(&cube->array_view, (uint32_t)sampler_location);
	}
	else
		GX2SetPixelTexture(&cube->gx2, (uint32_t)sampler_location);
	GX2SetPixelSampler(&sampler, (uint32_t)sampler_location);
	for (probe = 0; probe < 48; probe++)
	{
		float direction[4] = { 0.0f, 0.0f, 0.0f, probe >= 24 ? 1.0f : 0.0f };
		int major = ((probe % 24) / 4) / 2, sign = ((probe % 24) / 4) & 1 ? -1 : 1;
		float u = (probe & 1) ? 0.5f : -0.5f, v = (probe & 2) ? 0.5f : -0.5f;
		int minor0 = major == 0 ? 1 : 0, minor1 = major == 2 ? 1 : 2;

		direction[major] = (float)sign;
		direction[minor0] = u;
		direction[minor1] = v;
		quad_state(48, 1, GX2_CHANNEL_MASK_RGBA);
		GX2SetViewport((float)probe, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f);
		GX2SetScissor((uint32_t)probe, 0, 1, 1);
		GX2SetPixelUniformReg((uint32_t)direction_offset, 4, (uint32_t *)direction);
		quad_draw(pixel, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f);
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, row->memory, row->bytes);
	GX2DrawDone();
	DCInvalidateRange(row->memory, row->bytes);
	read = texture_level(row, 0, &pitch, &slice);
	for (probe = 0; probe < 24; probe++)
	{
		const unsigned char *texel = read + probe * 4;
		int major = (probe / 4) / 2, sign = (probe / 4) & 1 ? -1 : 1;
		float u = (probe & 1) ? 0.5f : -0.5f, v = (probe & 2) ? 0.5f : -0.5f;

		const unsigned char *check = read + (probe + 24) * 4;

		wiiu_log("gl: cube probe %c%c, minor %+.1f %+.1f: face %d, column %d, row %d (direction seen %02x %02x %02x)",
			sign > 0 ? '+' : '-', "XYZ"[major], u, v, texel[0] / 40 - 1, (texel[1] - 30) / 60, (texel[2] - 30) / 60,
			check[0], check[1], check[2]);
	}
	gl.bound_color = NULL;
	gl.uploaded_program = 0;
}

/* the read framebuffer's color into the draw framebuffer, as a textured
quad (to the screen for 0, whose rows GL counts from the bottom) */
static void APIENTRY wiiu_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
	GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
	struct framebuffer *read = object_get(gl.read_framebuffer, _object_framebuffer);
	struct texture *source = read ? object_get(read->color, _object_texture) : NULL;
	unsigned long width, height, source_width, source_height;
	GX2Sampler sampler;
	float x0, y0, x1, y1;

	if (!(mask & GL_COLOR_BUFFER_BIT) || !source || !source->memory || !gl_ready() || !wiiu_video_drawing())
	{
		if (mask & GL_COLOR_BUFFER_BIT)
			FIRST_USE();
		return;
	}
	bind_targets();
	if (debug.trace && gl.frame < 12)
		wiiu_log("gl: frame %lu: blit %d,%d-%d,%d of %lux%lu to %d,%d-%d,%d of framebuffer %u (%lux%lu)", gl.frame, sx0, sy0,
			sx1, sy1, source->width, source->height, dx0, dy0, dx1, dy1, gl.draw_framebuffer, gl.target_width,
			gl.target_height);
	if (!gl.target_has_color || gl.target_texture == source)
		return;
	width = gl.target_width;
	height = gl.target_height;
	if (!gl.draw_framebuffer && !gl.drawing)
	{
		/* (the screens are the renderer's from now on: wiiu_video_idle) */
		gl.drawing = 1;
		debug_load();
		if (debug.cube_probe)
		{
			cube_probe_run();
			bind_targets();
		}
		wiiu_log("gl: the game's first picture presented (frame %lu, %lu draws)", gl.frame, gl.draws);
	}
	if (!gl.draw_framebuffer)
	{
		dy0 = (GLint)height - dy0;
		dy1 = (GLint)height - dy1;
	}
	texture_flush_drawn(source);
	source_width = level_size(source->width, read->color_level);
	source_height = level_size(source->height, read->color_level);
	/* (window rows to clip space: Direct3D's, the first row at y = 1) */
	x0 = (float)dx0 / (float)width * 2.0f - 1.0f;
	x1 = (float)dx1 / (float)width * 2.0f - 1.0f;
	y0 = 1.0f - (float)dy0 / (float)height * 2.0f;
	y1 = 1.0f - (float)dy1 / (float)height * 2.0f;
	if (debug.screen_magenta && !gl.draw_framebuffer)
	{
		GX2ClearColor(gl.bound_color, 1.0f, 0.0f, 1.0f, 1.0f);
		GX2SetContextState(wiiu_video_context_state());
		gl.bound_color = NULL;
		bind_targets();
	}
	if (debug.screen_magenta && !gl.draw_framebuffer && !debug.blit_green)
		return;
	quad_state(width, height, GX2_CHANNEL_MASK_RGBA);
	if (debug.blit_green)
	{
		static const float green[4] = { 0.0f, 1.0f, 0.0f, 1.0f };

		GX2SetPixelUniformReg((uint32_t)gl.clear_color_offset, 4, (uint32_t *)green);
		quad_draw(gl.clear_pixel, x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, 1.0f);
		return;
	}
	GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, filter == GL_LINEAR ? GX2_TEX_XY_FILTER_MODE_LINEAR :
		GX2_TEX_XY_FILTER_MODE_POINT);
	GX2InitSamplerLOD(&sampler, (float)read->color_level, (float)read->color_level, 0.0f);
	GX2SetPixelTexture(&source->gx2, (uint32_t)gl.blit_source_location);
	texture_used(source);
	GX2SetPixelSampler(&sampler, (uint32_t)gl.blit_source_location);
	quad_draw(gl.blit_pixel, x0, y0, x1, y1, (float)sx0 / (float)source_width, (float)sy0 / (float)source_height,
		(float)sx1 / (float)source_width, (float)sy1 / (float)source_height);
	targets_written();
	gpu_check(gl.draw_framebuffer ? "blit" : "blit to the screen");
}

static void APIENTRY wiiu_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
	GLvoid *pixels)
{
	FIRST_USE();
	(void)x; (void)y; (void)format; (void)type;
	if (pixels && width > 0 && height > 0)
		memset(pixels, 0, (size_t)width * (size_t)height * 4);
}

/* ---------- the entry points: buffers */

static struct buffer *bound_buffer(GLenum target)
{
	GLuint name = target == GL_ELEMENT_ARRAY_BUFFER ? gl.element_buffer : target == GL_ARRAY_BUFFER ?
		gl.array_buffer : target == GL_COPY_WRITE_BUFFER ? gl.copy_write_buffer : target == GL_COPY_READ_BUFFER ?
		gl.copy_read_buffer : 0;

	return object_get(name, _object_buffer);
}

static void APIENTRY wiiu_glGenBuffers(GLsizei n, GLuint *names)
{
	generate(n, names, _object_buffer, sizeof(struct buffer));
}

static void APIENTRY wiiu_glDeleteBuffers(GLsizei n, const GLuint *names)
{
	GLsizei index;

	for (index = 0; index < n; index++)
	{
		struct buffer *buffer = object_get(names[index], _object_buffer);

		if (!buffer)
			continue;
		if (!buffer->alias)
			retire(buffer->data, _retired_memory);
		free(buffer);
		objects[names[index]].type = _object_none;
	}
}

static void APIENTRY wiiu_glBindBuffer(GLenum target, GLuint buffer)
{
	if (target == GL_ARRAY_BUFFER)
		gl.array_buffer = buffer;
	else if (target == GL_ELEMENT_ARRAY_BUFFER)
		gl.element_buffer = buffer;
	else if (target == GL_COPY_WRITE_BUFFER)
		gl.copy_write_buffer = buffer;
	else if (target == GL_COPY_READ_BUFFER)
		gl.copy_read_buffer = buffer;
}

static void APIENTRY wiiu_glBindBufferBase(GLenum target, GLuint index, GLuint buffer)
{
	FIRST_USE();
	(void)target; (void)index; (void)buffer;
}

/* new storage each time (what the GPU may still read of the old is kept
until it is done: a stream buffer orphaned when full) */
static void APIENTRY wiiu_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
	struct buffer *buffer = bound_buffer(target);
	unsigned char *storage;

	(void)usage;
	if (!buffer)
		return;
	storage = gpu_allocate((unsigned long)size, 0x100);
	if (!storage)
	{
		wiiu_log("gl: out of memory for a %lu byte buffer", (unsigned long)size);
		return;
	}
	if (!buffer->alias)
		retire(buffer->data, _retired_memory);
	buffer->data = storage;
	buffer->size = (unsigned long)size;
	buffer->alias = 0;
	if (data)
	{
		memcpy(buffer->data, data, (size_t)size);
		gpu_written(buffer->data, (unsigned long)size, GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER);
	}
}

static void APIENTRY wiiu_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
{
	struct buffer *buffer = bound_buffer(target);

	if (!buffer || !buffer->data || (unsigned long)offset + (unsigned long)size > buffer->size)
		return;
	/* (an alias given its own bytes: they are there already) */
	if (data != buffer->data + offset)
		memcpy(buffer->data + offset, data, (size_t)size);
	gpu_written(buffer->data + offset, (unsigned long)size, GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER);
}

static void APIENTRY wiiu_glCopyBufferSubData(GLenum read_target, GLenum write_target, GLintptr read_offset,
	GLintptr write_offset, GLsizeiptr size)
{
	struct buffer *source = bound_buffer(read_target), *destination = bound_buffer(write_target);

	if (!source || !destination || !source->data || !destination->data ||
		(unsigned long)read_offset + (unsigned long)size > source->size ||
		(unsigned long)write_offset + (unsigned long)size > destination->size)
	{
		return;
	}
	memmove(destination->data + write_offset, source->data + read_offset, (size_t)size);
	gpu_written(destination->data + write_offset, (unsigned long)size, GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER);
}

static struct
{
	struct buffer *buffer;
	unsigned long offset, length;
} mapped;

static void *APIENTRY wiiu_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access)
{
	struct buffer *buffer = bound_buffer(target);

	(void)access;
	if (!buffer || !buffer->data)
		return NULL;
	mapped.buffer = buffer;
	mapped.offset = (unsigned long)offset;
	mapped.length = (unsigned long)length;
	return buffer->data + offset;
}

static GLboolean APIENTRY wiiu_glUnmapBuffer(GLenum target)
{
	(void)target;
	if (mapped.buffer && mapped.buffer->data)
		gpu_written(mapped.buffer->data + mapped.offset, mapped.length, GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER);
	mapped.buffer = NULL;
	return GL_TRUE;
}

/* the buffer bound to target made an alias of memory the GPU reads in place
(the Xbox window's: d3d8_gl.c's mirror, HALO_CONSOLE_GL_LAYER) */
void console_gl_buffer_alias(GLenum target, void *memory, unsigned long size)
{
	struct buffer *buffer = bound_buffer(target);

	if (!buffer)
		return;
	if (!buffer->alias)
		retire(buffer->data, _retired_memory);
	buffer->data = memory;
	buffer->size = size;
	buffer->alias = 1;
}

/* ---------- the entry points: vertex attributes */

/* a vertex array object keeps the attribute formats, their bindings and the
index buffer; binding one swaps them in for the current ones (the generic
attribute values are not part of it) */
struct vertex_array
{
	int made;
	struct attribute attributes[ATTRIBUTES];
	struct binding bindings[BINDINGS];
	GLuint element_buffer;
};

static struct vertex_array default_vertex_array;
static GLuint current_vertex_array_name;

static void vertex_array_save(struct vertex_array *array)
{
	memcpy(array->attributes, gl.attributes, sizeof(array->attributes));
	memcpy(array->bindings, gl.bindings, sizeof(array->bindings));
	array->element_buffer = gl.element_buffer;
	array->made = 1;
}

static void vertex_array_load(const struct vertex_array *array)
{
	int index;

	for (index = 0; index < ATTRIBUTES; index++)
	{
		float value[4];

		/* (the generic values are the context's) */
		memcpy(value, gl.attributes[index].value, sizeof(value));
		gl.attributes[index] = array->attributes[index];
		memcpy(gl.attributes[index].value, value, sizeof(value));
	}
	memcpy(gl.bindings, array->bindings, sizeof(array->bindings));
	gl.element_buffer = array->element_buffer;
}

static void APIENTRY wiiu_glGenVertexArrays(GLsizei n, GLuint *names)
{
	generate(n, names, _object_vertex_array, sizeof(struct vertex_array));
}
static void APIENTRY wiiu_glBindVertexArray(GLuint name)
{
	struct vertex_array *current, *next;
	int index;

	if (name == current_vertex_array_name)
		return;
	current = current_vertex_array_name ? object_get(current_vertex_array_name, _object_vertex_array) :
		&default_vertex_array;
	next = name ? object_get(name, _object_vertex_array) : &default_vertex_array;
	if (!next)
		return;
	if (current)
		vertex_array_save(current);
	if (!next->made)
	{
		/* (a new one: nothing enabled, each attribute on its own binding) */
		for (index = 0; index < ATTRIBUTES; index++)
		{
			next->attributes[index].size = 4;
			next->attributes[index].type = GL_FLOAT;
			next->attributes[index].binding = (GLuint)index;
		}
		next->made = 1;
	}
	vertex_array_load(next);
	current_vertex_array_name = name;
}
static void APIENTRY wiiu_glEnableVertexAttribArray(GLuint index)
{
	if (index < ATTRIBUTES)
		gl.attributes[index].enabled = 1;
}
static void APIENTRY wiiu_glDisableVertexAttribArray(GLuint index)
{
	if (index < ATTRIBUTES)
		gl.attributes[index].enabled = 0;
}

static void attribute_set_format(GLuint index, GLint size, GLenum type, GLboolean normalized, int integer,
	GLuint offset)
{
	struct attribute *attribute;

	if (index >= ATTRIBUTES)
		return;
	attribute = &gl.attributes[index];
	attribute->size = size;
	attribute->type = type;
	attribute->normalized = normalized;
	attribute->integer = integer;
	attribute->relative_offset = offset;
}

static void APIENTRY wiiu_glVertexAttribFormat(GLuint index, GLint size, GLenum type, GLboolean normalized,
	GLuint offset)
{
	attribute_set_format(index, size, type, normalized, 0, offset);
}
static void APIENTRY wiiu_glVertexAttribIFormat(GLuint index, GLint size, GLenum type, GLuint offset)
{
	attribute_set_format(index, size, type, GL_FALSE, 1, offset);
}
static void APIENTRY wiiu_glVertexAttribBinding(GLuint index, GLuint binding)
{
	if (index < ATTRIBUTES)
		gl.attributes[index].binding = binding;
}
static void APIENTRY wiiu_glBindVertexBuffer(GLuint binding, GLuint buffer, GLintptr offset, GLsizei stride)
{
	if (binding >= BINDINGS)
		return;
	gl.bindings[binding].buffer = buffer;
	gl.bindings[binding].offset = offset;
	gl.bindings[binding].stride = stride;
}

/* (the older form: a binding of the attribute's own) */
static void attribute_pointer(GLuint index, GLint size, GLenum type, GLboolean normalized, int integer,
	GLsizei stride, const void *pointer)
{
	GX2AttribFormat format;
	uint32_t mask;
	unsigned long bytes = 16;

	if (index >= ATTRIBUTES)
		return;
	attribute_set_format(index, size, type, normalized, integer, 0);
	gl.attributes[index].binding = index;
	attribute_format(&gl.attributes[index], &format, &mask, &bytes);
	wiiu_glBindVertexBuffer(index, gl.array_buffer, (GLintptr)pointer, stride ? stride : (GLsizei)bytes);
}

static void APIENTRY wiiu_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
	GLsizei stride, const void *pointer)
{
	attribute_pointer(index, size, type, normalized, 0, stride, pointer);
}
static void APIENTRY wiiu_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
	const void *pointer)
{
	attribute_pointer(index, size, type, GL_FALSE, 1, stride, pointer);
}
static void APIENTRY wiiu_glVertexAttrib4fv(GLuint index, const GLfloat *v)
{
	if (index < ATTRIBUTES)
		memcpy(gl.attributes[index].value, v, sizeof(gl.attributes[index].value));
}
static void APIENTRY wiiu_glVertexAttribI4ui(GLuint index, GLuint x, GLuint y, GLuint z, GLuint w)
{
	if (index >= ATTRIBUTES)
		return;
	/* (an integer input reads the words as they are) */
	memcpy(&gl.attributes[index].value[0], &x, 4);
	memcpy(&gl.attributes[index].value[1], &y, 4);
	memcpy(&gl.attributes[index].value[2], &z, 4);
	memcpy(&gl.attributes[index].value[3], &w, 4);
}

/* ---------- the entry points: draws */

static void APIENTRY wiiu_glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
	draw(mode, (unsigned long)first, (unsigned long)count, 0, NULL, 0);
}
static void APIENTRY wiiu_glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid *indices)
{
	draw(mode, 0, (unsigned long)count, type, indices, 0);
}
static void APIENTRY wiiu_glDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices,
	GLint base)
{
	draw(mode, 0, (unsigned long)count, type, indices, base);
}

/* ---------- the entry points: shaders and programs */

static GLuint APIENTRY wiiu_glCreateShader(GLenum type)
{
	struct shader *shader = calloc(1, sizeof(*shader));
	GLuint name;

	if (!shader)
		return 0;
	shader->type = type;
	name = object_new(_object_shader, shader);
	if (!name)
		free(shader);
	return name;
}

static void APIENTRY wiiu_glShaderSource(GLuint name, GLsizei count, const GLchar *const *strings,
	const GLint *lengths)
{
	struct shader *shader = object_get(name, _object_shader);
	size_t total = 0;
	GLsizei index;

	if (!shader)
		return;
	for (index = 0; index < count; index++)
		total += lengths && lengths[index] >= 0 ? (size_t)lengths[index] : strlen(strings[index]);
	free(shader->source);
	shader->source = malloc(total + 1);
	if (!shader->source)
		return;
	total = 0;
	for (index = 0; index < count; index++)
	{
		size_t length = lengths && lengths[index] >= 0 ? (size_t)lengths[index] : strlen(strings[index]);

		memcpy(shader->source + total, strings[index], length);
		total += length;
	}
	shader->source[total] = '\0';
	shader->compiled = 0;
}

static void APIENTRY wiiu_glCompileShader(GLuint name)
{
	struct shader *shader = object_get(name, _object_shader);

	if (shader)
		shader_compile(shader);
}

static void APIENTRY wiiu_glGetShaderiv(GLuint name, GLenum parameter, GLint *value)
{
	struct shader *shader = object_get(name, _object_shader);

	switch (parameter)
	{
	case GL_COMPILE_STATUS: *value = shader && (shader->vertex || shader->pixel) ? GL_TRUE : GL_FALSE; break;
	case GL_INFO_LOG_LENGTH: *value = shader ? (GLint)strlen(shader->log) + 1 : 1; break;
	default: *value = 0; break;
	}
}

static void APIENTRY wiiu_glGetShaderInfoLog(GLuint name, GLsizei size, GLsizei *length, GLchar *log)
{
	struct shader *shader = object_get(name, _object_shader);
	const char *text = shader ? shader->log : "";

	if (size > 0)
	{
		strncpy(log, text, (size_t)size - 1);
		log[size - 1] = '\0';
	}
	if (length)
		*length = size > 0 ? (GLsizei)strlen(log) : 0;
}

/* a deleted shader goes when no program has it (a program made from it
uses its compiled code) */
static void shader_free_if_unused(GLuint name)
{
	struct shader *shader = object_get(name, _object_shader);

	if (!shader || !shader->deleted || shader->attachments > 0)
		return;
	objects[name].type = _object_none;
	objects[name].data = NULL;
	shader_release(shader);
}

static void shader_detach(GLuint name)
{
	struct shader *shader = object_get(name, _object_shader);

	if (!shader)
		return;
	shader->attachments--;
	shader_free_if_unused(name);
}

static void APIENTRY wiiu_glDeleteShader(GLuint name)
{
	struct shader *shader = object_get(name, _object_shader);

	if (!shader)
		return;
	shader->deleted = 1;
	shader_free_if_unused(name);
}

static GLuint APIENTRY wiiu_glCreateProgram(void)
{
	struct program *program = calloc(1, sizeof(*program));
	GLuint name;

	if (!program)
		return 0;
	name = object_new(_object_program, program);
	if (!name)
		free(program);
	return name;
}

static void APIENTRY wiiu_glAttachShader(GLuint program_name, GLuint shader_name)
{
	struct program *program = object_get(program_name, _object_program);
	struct shader *shader = object_get(shader_name, _object_shader);

	if (!program || !shader)
		return;
	if (shader->type == GL_VERTEX_SHADER)
	{
		if (program->vertex == shader_name)
			return;
		shader->attachments++;
		shader_detach(program->vertex);
		program->vertex = shader_name;
	}
	else
	{
		if (program->fragment == shader_name)
			return;
		shader->attachments++;
		shader_detach(program->fragment);
		program->fragment = shader_name;
	}
}

/* (its shaders detached: those deleted go with it) */
static void program_free(GLuint name)
{
	struct program *program = object_get(name, _object_program);

	if (!program)
		return;
	objects[name].type = _object_none;
	objects[name].data = NULL;
	shader_detach(program->vertex);
	shader_detach(program->fragment);
	free(program);
	if (gl.uploaded_program == name)
		gl.uploaded_program = 0;
}

static void APIENTRY wiiu_glDeleteProgram(GLuint name)
{
	struct program *program = object_get(name, _object_program);

	if (!program)
		return;
	/* (the one in use stays until another is: GL's rule) */
	if (name == gl.program)
		program->deleted = 1;
	else
		program_free(name);
}

static void APIENTRY wiiu_glBindAttribLocation(GLuint program, GLuint index, const GLchar *name)
{
	(void)program; (void)index; (void)name;
}
static void APIENTRY wiiu_glBindFragDataLocation(GLuint program, GLuint color, const GLchar *name)
{
	(void)program; (void)color; (void)name;
}

static void APIENTRY wiiu_glLinkProgram(GLuint name)
{
	struct program *program = object_get(name, _object_program);
	struct shader *vertex, *fragment;

	if (!program)
		return;
	vertex = object_get(program->vertex, _object_shader);
	fragment = object_get(program->fragment, _object_shader);
	program->linked = vertex && fragment && shader_compile(vertex) && shader_compile(fragment) && vertex->vertex &&
		fragment->pixel;
	if (program->linked)
		program_uniforms(program, vertex, fragment);
	if (gl.uploaded_program == name)
		gl.uploaded_program = 0;
}

static void APIENTRY wiiu_glGetProgramiv(GLuint name, GLenum parameter, GLint *value)
{
	struct program *program = object_get(name, _object_program);

	switch (parameter)
	{
	case GL_LINK_STATUS: *value = program && program->linked ? GL_TRUE : GL_FALSE; break;
	case GL_INFO_LOG_LENGTH: *value = 1; break;
	default: *value = 0; break;
	}
}

static void APIENTRY wiiu_glGetProgramInfoLog(GLuint program, GLsizei size, GLsizei *length, GLchar *log)
{
	(void)program;
	if (size > 0)
		log[0] = '\0';
	if (length)
		*length = 0;
}

static void APIENTRY wiiu_glUseProgram(GLuint name)
{
	struct program *previous = object_get(gl.program, _object_program);

	if (previous && previous->deleted && gl.program != name)
		program_free(gl.program);
	gl.program = name;
}

static GLint APIENTRY wiiu_glGetUniformLocation(GLuint name, const GLchar *uniform_name)
{
	struct program *program = object_get(name, _object_program);
	char base[32];
	const char *bracket = strchr(uniform_name, '[');
	int element = bracket ? atoi(bracket + 1) : 0;
	int index;

	if (!program)
		return -1;
	uniform_base_name(base, sizeof(base), uniform_name);
	for (index = 0; index < program->uniform_count; index++)
	{
		struct uniform *uniform = &program->uniforms[index];

		if (strcmp(uniform->name, base))
			continue;
		if (!uniform->sampler && element >= uniform->count)
			return -1;
		return LOCATION(index, element);
	}
	return -1;
}

static void APIENTRY wiiu_glUniform1i(GLint location, GLint v0)
{
	struct program *program = object_get(gl.program, _object_program);
	int index = location >= 0 ? LOCATION_UNIFORM(location) : -1;

	if (!program || index < 0 || index >= program->uniform_count)
		return;
	if (program->uniforms[index].sampler)
		program->uniforms[index].unit = v0;
	else
	{
		float value = (float)v0;

		uniform_set(location, &value, 1, 1);
	}
}
static void APIENTRY wiiu_glUniform1iv(GLint location, GLsizei count, const GLint *values)
{
	GLsizei index;

	for (index = 0; index < count; index++)
		wiiu_glUniform1i(location + index, values[index]);
}
static void APIENTRY wiiu_glUniform1f(GLint location, GLfloat v0) { uniform_set(location, &v0, 1, 1); }
static void APIENTRY wiiu_glUniform2f(GLint location, GLfloat v0, GLfloat v1)
{
	float values[2] = { v0, v1 };

	uniform_set(location, values, 1, 2);
}
static void APIENTRY wiiu_glUniform4fv(GLint location, GLsizei count, const GLfloat *values)
{
	uniform_set(location, values, (int)count, 4);
}

/* ---------- the entry points: queries (occlusion: GX2's, decaf's
gx2_query.cpp) */

static void APIENTRY wiiu_glGenQueries(GLsizei n, GLuint *names)
{
	generate(n, names, _object_query, sizeof(struct query));
}

static GLuint active_query;

static void APIENTRY wiiu_glBeginQuery(GLenum target, GLuint name)
{
	struct query *query = object_get(name, _object_query);

	(void)target;
	if (!query || !gl.ready || !wiiu_video_drawing())
		return;
	if (!query->data)
		query->data = gpu_allocate(sizeof(wiiu_query_data), 0x40);
	if (!query->data)
		return;
	GX2QueryBegin(WIIU_QUERY_OCCLUSION, query->data);
	gpu_check("visibility query's begin");
	query->issued = 1;
	query->available = 0;
	active_query = name;
}

static void APIENTRY wiiu_glEndQuery(GLenum target)
{
	struct query *query = object_get(active_query, _object_query);

	(void)target;
	if (query && query->data && query->issued)
	{
		GX2QueryEnd(WIIU_QUERY_OCCLUSION, query->data);
		gpu_check("visibility query's end");
	}
	active_query = 0;
}

static void APIENTRY wiiu_glGetQueryObjectuiv(GLuint name, GLenum parameter, GLuint *value)
{
	struct query *query = object_get(name, _object_query);

	*value = 0;
	if (!query || !query->data || !query->issued)
	{
		if (parameter == GL_QUERY_RESULT_AVAILABLE)
			*value = GL_TRUE;
		return;
	}
	if (!query->available)
	{
		uint64_t result = 0;

		if (GX2QueryGetOcclusionResult(query->data, &result))
		{
			query->available = 1;
			query->result = (GLuint)result;
		}
		else if (parameter == GL_QUERY_RESULT)
		{
			/* (the GPU given what is queued, then waited for, 100 ms at
			most: the result is the GPU's write to memory, read past the
			CPU's cache after the first look; one that never comes counts
			no samples) */
			OSTime start = OSGetSystemTime(), waited;
			int done;

			watch_step("waiting for a visibility query");
			GX2Flush();
			for (;;)
			{
				done = GX2QueryGetOcclusionResult(query->data, &result);
				waited = OSGetSystemTime() - start;
				if (done || waited > OSMillisecondsToTicks(100))
					break;
				OSSleepTicks(OSMicrosecondsToTicks(50));
				DCInvalidateRange(query->data, sizeof(wiiu_query_data));
			}
			watch.query_wait += waited;
			watch.queries++;
			if (!done)
			{
				static unsigned long timeouts;

				watch.query_timeouts++;
				result = 0;
				if (timeouts++ < 8)
					wiiu_log("gl: a visibility query's result did not come in 100 ms (frame %lu); taken as 0",
						gl.frame);
			}
			watch_step("the game's own work");
			query->available = 1;
			query->result = (GLuint)result;
		}
	}
	if (parameter == GL_QUERY_RESULT_AVAILABLE)
		*value = query->available ? GL_TRUE : GL_FALSE;
	else
		*value = query->result;
}

static void APIENTRY wiiu_glGetQueryBufferObjectuiv(GLuint id, GLuint buffer, GLenum name, GLintptr offset)
{
	FIRST_USE();
	(void)id; (void)buffer; (void)name; (void)offset;
}

/* ---------- the frame */

/* (after the frame's swap, wiiu_video_swap): what the GPU is done with let
go, two frames on */
void wiiu_gl_frame_end(void)
{
	again.valid = 0;
	if (!gl.ready)
		return;
	if (debug.trace && gl.frame < 12)
		wiiu_log("gl: frame %lu ends: %lu draws", gl.frame, gl.frame_draws);
	gpu_check("copy to the screens and swap");
	gpu_check_frame_end();
	watch_step("the frame's end");
	GX2Flush();
	gl.frame_stamps[gl.frame_half] = GX2GetLastSubmittedTimeStamp();
	gl.frame_half ^= 1;
	if (gl.frame_stamps[gl.frame_half])
		gpu_wait(gl.frame_stamps[gl.frame_half], "the GPU let finish the frame before last");
	retired_free(gl.frame_half);
	gl.ring_offset = (unsigned long)gl.frame_half * (RING_SIZE / 2);
	/* (the next frame binds its own) */
	gl.bound_color = NULL;
	gl.bound_depth = NULL;
	gl.uploaded_program = 0;
	gl.frame++;
	/* (log_at=N: the first frame after N seconds, once) */
	if (debug.log_at && OSTicksToMilliseconds(OSGetSystemTime() - gl.start) / 1000 >= (OSTime)debug.log_at)
	{
		debug.log_at = 0;
		debug.log_frame = 1;
		debug.log_frame_number = gl.frame;
		wiiu_log("gl: frame %lu logged (the last had %lu draws)", gl.frame, gl.frame_draws);
	}
	/* (log_big: the frame after the first of more than 300 draws, once) */
	if (debug.log_big == 1 && gl.frame_draws > 300)
	{
		debug.log_big = 2;
		debug.log_frame = 1;
		debug.log_frame_number = gl.frame;
	}
	watch_frame_end();
	watch_step("the game's own work");
	gl.frame_draws = 0;
}

int wiiu_gl_drawing(void)
{
	return gl.drawing;
}

/* ---------- the entry points by name (SDL_GL_GetProcAddress) */

#define ENTRY(name) { #name, (void *)wiiu_##name },

static const struct
{
	const char *name;
	void *function;
} entries[] =
{
	ENTRY(glActiveTexture) ENTRY(glAttachShader) ENTRY(glBeginQuery) ENTRY(glBindAttribLocation) ENTRY(glBindBuffer)
	ENTRY(glBindBufferBase) ENTRY(glBindFragDataLocation) ENTRY(glBindFramebuffer) ENTRY(glBindRenderbuffer)
	ENTRY(glBindSampler) ENTRY(glBindTexture) ENTRY(glBindTextures) ENTRY(glBindVertexArray) ENTRY(glBindVertexBuffer)
	ENTRY(glBlendColor) ENTRY(glBlendEquation) ENTRY(glBlendFunc) ENTRY(glBlendFuncSeparate) ENTRY(glBlitFramebuffer)
	ENTRY(glBufferData) ENTRY(glBufferSubData) ENTRY(glCheckFramebufferStatus) ENTRY(glClear) ENTRY(glClearColor)
	ENTRY(glClearDepth) ENTRY(glClearStencil) ENTRY(glClipControl) ENTRY(glColorMask) ENTRY(glCompileShader)
	ENTRY(glCompressedTexImage2D) ENTRY(glCompressedTexImage3D) ENTRY(glCopyBufferSubData) ENTRY(glCreateProgram)
	ENTRY(glCreateShader) ENTRY(glCullFace) ENTRY(glDeleteBuffers) ENTRY(glDeleteProgram)
	ENTRY(glDeleteFramebuffers) ENTRY(glDeleteShader) ENTRY(glDeleteTextures) ENTRY(glDepthFunc) ENTRY(glDepthMask)
	ENTRY(glDepthRange) ENTRY(glDisable) ENTRY(glDisableVertexAttribArray) ENTRY(glDrawArrays) ENTRY(glDrawBuffers)
	ENTRY(glDrawElements) ENTRY(glDrawElementsBaseVertex) ENTRY(glEnable) ENTRY(glEnableVertexAttribArray)
	ENTRY(glEndQuery) ENTRY(glFinish) ENTRY(glFlush) ENTRY(glFramebufferRenderbuffer) ENTRY(glFramebufferTexture2D)
	ENTRY(glFrontFace) ENTRY(glGenBuffers) ENTRY(glGenFramebuffers) ENTRY(glGenQueries) ENTRY(glGenRenderbuffers)
	ENTRY(glGenSamplers) ENTRY(glGenTextures) ENTRY(glGenVertexArrays) ENTRY(glGenerateMipmap) ENTRY(glGetError)
	ENTRY(glGetIntegerv) ENTRY(glGetProgramInfoLog) ENTRY(glGetProgramiv) ENTRY(glGetQueryBufferObjectuiv)
	ENTRY(glGetQueryObjectuiv) ENTRY(glGetShaderInfoLog) ENTRY(glGetShaderiv) ENTRY(glGetString) ENTRY(glGetTexImage)
	ENTRY(glGetUniformLocation) ENTRY(glIsEnabled) ENTRY(glLineWidth) ENTRY(glLinkProgram) ENTRY(glMapBufferRange)
	ENTRY(glMemoryBarrier) ENTRY(glPixelStorei) ENTRY(glPolygonMode) ENTRY(glPolygonOffset) ENTRY(glReadPixels)
	ENTRY(glRenderbufferStorageMultisample) ENTRY(glSamplerParameterf) ENTRY(glSamplerParameterfv)
	ENTRY(glSamplerParameteri) ENTRY(glScissor) ENTRY(glShaderSource) ENTRY(glStencilFunc) ENTRY(glStencilMask)
	ENTRY(glStencilOp) ENTRY(glTexImage2D) ENTRY(glTexImage3D) ENTRY(glTexParameterf) ENTRY(glTexParameterfv)
	ENTRY(glTexParameteri) ENTRY(glTexParameteriv) ENTRY(glTexSubImage2D) ENTRY(glUniform1f) ENTRY(glUniform1i)
	ENTRY(glUniform1iv) ENTRY(glUniform2f) ENTRY(glUniform4fv) ENTRY(glUnmapBuffer) ENTRY(glUseProgram)
	ENTRY(glVertexAttrib4fv) ENTRY(glVertexAttribBinding) ENTRY(glVertexAttribFormat) ENTRY(glVertexAttribI4ui)
	ENTRY(glVertexAttribIFormat) ENTRY(glVertexAttribIPointer) ENTRY(glVertexAttribPointer) ENTRY(glViewport)
	/* (the optional ones the renderer manages without: glBufferStorage,
	glCopyImageSubData, glDebugMessageCallback) */
	/* (not GL's: the renderer's visibility test runs, d3d8_gl.c) */
	{ "console_gl_draw_again", (void *)console_gl_draw_again },
};

void *wiiu_gl_proc_address(const char *name)
{
#ifdef HALO_WIIU_CAFEGLSL
	size_t index;

	if (!gl_ready())
		return NULL;
	for (index = 0; index < sizeof(entries) / sizeof(entries[0]); index++)
	{
		if (!strcmp(entries[index].name, name))
			return entries[index].function;
	}
#else
	/* (no shader compiler, no renderer) */
	(void)name;
	(void)entries;
#endif
	return NULL;
}
