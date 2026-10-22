# The Wii U renderer

How the game's renderer reaches the Wii U's GPU (Latte, through GX2), and
how its shaders are made. Milestone 3 in STATUS.md.

## The plan

The desktop renderer (the game's `port/linux/src/d3d8_gl.c`) stays as it
is, over a subset of OpenGL that the port gives on GX2
(`port/wiiu/src/wiiu_gl.c`), as the Xbox 360 port gives one over Direct3D 9.
`d3d8_gl.c` makes its shaders at run time, as GLSL text, from the game's
render state, so the question is how GLSL becomes Latte code.

## GX2 (done)

`src/wiiu_video.c` starts GX2 the way SDL's Wii U video driver does
(devkitPro's SDL, branch `wiiu-sdl2-2.28`, `src/video/wiiu/SDL_wiiuvideo.c`
and `SDL_wiiu_gfx_heap.c`) and wut's libwhb does (`libraries/libwhb/src/gfx.c`
and `gfx_heap.c`):

| Step | Reference |
| --- | --- |
| `GX2Init` with the port's own command buffer pool (4 MB) | `WIIU_VideoInit`; libwhb's `WHB_GFX_COMMAND_BUFFER_POOL_SIZE` |
| TV render mode from `GX2GetSystemTVScanMode`, GamePad mode from `GX2GetSystemDRCScanMode` | `WIIU_VideoInit` |
| Scan buffers in the foreground heap, color and depth buffers in MEM1, both heaps made on acquire and destroyed on release | `WIIU_ForegroundAcquired`, `WIIU_ForegroundReleased`, `WIIU_GfxHeap_*`; libwhb's `GfxProcCallbackAcquired`, `GfxProcCallbackReleased` |
| GX2R allocator: color, depth and scan buffers in MEM1, the rest in MEM2 | `WIIU_GfxHeap_GX2RAlloc` |
| ProcUI acquire and release callbacks (priority 100), added to the port's own ProcUI (`src/wiiu_proc.c`) | `WIIU_VideoInit` |
| One color buffer copied to both scan buffers, swap, flush, screens enabled after the first frame | `WIIU_SDL_RenderPresent` |
| A frame waits for the previous swap (at most 10 vsyncs) | `WHBGfxBeginRender` |
| `GX2Shutdown` after ProcUI says to exit | `WIIU_VideoQuit` |

The log's OSScreen text (`src/wiiu_screen.c`) is shut down when GX2
starts. Until the renderer draws, both screens are cleared to one color
every frame.

## How many shaders the game makes

Measured on the macOS build (game main at 5c5bebf), with
`debug.gpu_dump_shaders`, an empty save root, the NTSC maps, no audio:

| Run | Pixel shader keys | Vertex shader variants |
| --- | --- | --- |
| Main menu, 60 s (with the intro movie) | 11 | 4 |
| Blood Gulch, system link host, scripted bot input, 300 s | 111 | 21 |

Across both runs there are 47 distinct pixel shader texts (keys that
differ only in values that are uniforms give the same text) and 21 distinct
vertex shader texts. A text is 2.4 to 7.8 KB of GLSL (`#version 410 core`).

What bounds the two sets:

- Vertex shaders come from the game's own table of NV2A vertex programs
  (`source/rasterizer/xbox/rasterizer_xbox_vertex_shaders.h`,
  `NUMBER_OF_VERTEX_SHADERS = 67`), each in up to four variants
  (`d3d8_gl.c` `vertex_shader_get`: stream or immediate, lit or not). A
  closed set of at most 268.
- Pixel shaders are keyed by the register combiner state
  (`port/linux/src/xgpu.h`, `struct nv2a_pixel_shader_key`) plus alpha
  test, fog and sampler variants. The transparent shaders' combiner
  programs are built at run time from the shader tags' data
  (`source/rasterizer/xbox/shader_transparent_generic_preprocessor.c`,
  `shader_transparent_chicago_preprocessor.c`), so the set depends on the
  maps loaded. Custom Edition and HaloMD maps can make shaders that no list
  made from the stock maps has.

## Option A: GLSL compiled on the console (CafeGLSL)

[CafeGLSL](https://github.com/Exzap/CafeGLSL) is a GLSL compiler for Latte
built on Mesa (its GLSL front end and the r600 back end).

- License: the fork's additions are MIT; Mesa's core is MIT. The parts it
  links (`cafecompiler/meson.build`: `libglsl_standalone`, `libr600`,
  `libgallium`) carry MIT SPDX tags; none carries a GPL tag. Its GFD writer
  is decaf-emu's libgfd, used with its author's permission. All of this can
  be linked into the GPL-3.0 game.
- Distribution: v0.2.0 (2024-11-17) shipped `glslcompiler.rpl` (3.8 MB),
  a module loaded at run time from the SD card. v1.0.0-rc1 (2026-09-19) is a
  static library instead (`cafeglsl-1.0.0-rc1-wiiu-ppc.tar.xz`, laid out as a
  devkitPro portlib, `libcafeglsl.a` 21.7 MB, linked with `-lstdc++`), and
  desktop builds of the same compiler (`glslcompiler`, and a library) for
  Windows, macOS and Linux.
- API: `InitGLSLCompiler`, then `CompileShaderPair` (or
  `CompileVertexShader` and `CompilePixelShader`), which return
  `GX2VertexShader` and `GX2PixelShader` and the shader mode for
  `GX2SetShaderMode` (uniform registers or uniform blocks).

Tests:

- The desktop compiler (v1.0.0-rc1, macOS arm64) compiled all 147 dumped
  GLSL files of the two runs above with no failures, every one in uniform
  register mode. The compiled `.gsh` files total 584 KB. Each compile,
  process start included, took about 6 ms on an Apple M-series Mac.
- Linked into `halo.rpx` (`configure.py --cafeglsl DIR`), the RPX grows
  from 11.2 MB to 14.0 MB. Five symbols are defined both by the library and
  by the game or the port: `debug_malloc`, `debug_free` and `debug_realloc`
  (Mesa's `u_debug_memory.c`; nothing in the library calls them and that
  unit is not linked) and `__atomic_load_8` and `__atomic_store_8` (the
  port's own, `src/wiiu_posix.c`, are the ones linked).
- With CafeGLSL in the build, `src/wiiu_video.c` compiles a one-triangle
  shader pair at start-up and draws it over the clear color, and logs the
  compiler's start-up and compile times. Its hardware result goes here.

## Option B: precompiled shaders (as Butter and Jelly does)

Butter and Jelly (GPL-2.0-or-later) ships its one video shader pair
precompiled: Latte assembly (`src/shaders/video.vsh`, `nv12.psh`) assembled
offline by decaf-emu's `latte-assembler` (`scripts/build-shaders.sh`) into a
committed `.gsh`, loaded with wut's `GFD*` functions
(`src/platform/wiiu/gx2_video.cpp`).

An offline GLSL-to-Latte path exists: CafeGLSL's desktop `glslcompiler`
writes `.gsh` files from the same GLSL the game makes (tested above).
`latte-assembler` takes hand-written assembly only, which suits a few fixed
shaders, not generated ones.

A precompiled set covers the closed vertex shader table and the pixel
shaders of the maps it was made from. It cannot cover pixel shaders that
other maps' tags make.

## Recommendation

Option A: CafeGLSL linked into the game, compiling `d3d8_gl.c`'s GLSL on the
console, with the compiled shaders kept on the SD card by the hash of their
text (as `xgpu_shader_cache.c` keeps GL programs on desktop), so that a
shader is compiled once per install. A set built offline from the stock
maps with the same compiler can seed that cache. Option B alone cannot draw
custom maps' transparent shaders.

Open:

- The compiler's time and memory on the Espresso (the triangle build logs
  the time).
- Uniforms: in register mode, `d3d8_gl.c`'s uniforms go through
  `GX2SetVertexUniformReg` and `GX2SetPixelUniformReg` at the offsets the
  compiled shader records.
- Clip space: the macOS path converts Direct3D's clip space in the vertex
  shader (`HALO_GL_NO_CLIP_CONTROL`); the Wii U takes the same path or sets
  GX2's clip control.
