# Wii U bring-up status

The log of the Wii U port's bring-up, milestone by milestone: what works, what
does not, and the next step. The newest entry is at the bottom.

## Milestone 1: the whole game compiles and links (2026-10-21)

**Works.** `ninja wiiu` compiles the game's sources (port/linux/port.json),
the port's game units (port/linux/game), the desktop platform layer
(port/linux/src) less what the console does its own way, the third-party
libraries the platform layer uses, and port/wiiu, with devkitPPC's GCC 16.1
and newlib for the Espresso (`-mcpu=750`, big-endian), and links
`build/wiiu/halo.rpx` (11 MB, with 1.8 MB of code).

How the pieces fit:

- `include/halo_wiiu_prefix.h`: the MSVC and Xbox SDK environment the
  sources assume, for GCC and newlib (MSVC keywords, `__inline` as GNU
  inline functions with the link keeping one copy of each, as MSVC's COMDATs,
  newlib's headers read first with a 16-bit `wint_t`).
- The Xbox's memory window is moved (`HALO_WINDOW_REBASE`): taken from MEM2
  before the C library takes the heap (`src/wiiu_memory.c`, `__preinit_user`),
  256 MB at a multiple of its size, else 128 MB.
- No thread-local storage in an RPX (elf2rpl takes no TLS relocations): the
  platform layer's per-thread variables go through `platform_thread_local`
  (game PR #403, `HALO_CONSOLE_NO_THREAD_LOCAL`).
- `src/` talks to the console through wut: the log (`port.txt` on the SD
  card and OSReport), ProcUI, the controllers (VPAD GamePad, KPAD Pro and
  Classic Controllers), sockets (nsysnet), random numbers, the POSIX newlib
  leaves out (`include/posix`: `sys/mman.h`, `ifaddrs.h`, `dlfcn.h`).
- `platform/` is the port's side of the platform layer in its own terms
  (the Xbox SDK declarations): the screen and the system's events in place
  of `sdl_platform.c`, XInput in place of `xinput_sdl.c`, the renderer's
  write tracking without page protection.
- The game needs game PR ChupathingyCE/chupathingyce#403 (the submodule
  points at its branch, `wiiu-shared`, until it is merged).

**Not yet.** No renderer: `platform_video_initialize` says so and the game
runs without a picture, as the dedicated server does. No sound device (the
mixer is in, AX is not). No byte-swapped maps: the Xbox 360's shared
big-endian fixes (tag swap tables, scripts' narrow compares) arrive with the
game's x360-shared PR, and the Wii U takes them from there.

**Next.** Milestone 2: boot in Cemu, log, reach the main loop.

## Milestone 2: the maps load (2026-10-21)

**Works, in Cemu.** The game boots, copies ui.map into its cache file,
reads and byte-swaps its tags (980 of 983; the two `metr` tags have no
layout yet and are left as they are), adds the menus' tags, loads the main
menu's structure bsp and reaches the main menu, its music started. With
`d:\init.txt` holding `map_name levels\test\bloodgulch\bloodgulch` it loads
Blood Gulch instead (1803 of 1806 tags, its bsp's 380 blocks) and the game
ticks. Still no picture, no sound.

What it took:

- The big-endian map reading is the shared code's (game PR #402, in through
  #403): `build.py` compiles `port/console/game`, the byte swap of the
  maps' headers, tags and bsps, which `HALO_BIG_ENDIAN` turns on.
- The log on the screens: Cemu's OSScreen draws the GamePad's text past the
  buffer size it reports, over the heap's next blocks (the cache file
  thread's event among them, so the first read of the tags never woke the
  thread). Each screen buffer now has the TV's size and a megabyte more.
- The SD card's files (wut's device): an `open` that maps CreateFileA's
  flags onto the ones the device takes (write-only without create, as on
  OPEN_EXISTING, was refused, so no profile or playlist could be written),
  a seek past a file's end made by writing zeros first (the console's
  file system will not place a position past the end: the hardware run
  stopped at "setup for new cache file failed (#87)"), and the device's
  data set for the device calls the port makes itself.
- Game PR #403: the data arrays' "is it whole" checks compared the
  elements' pointer with their Xbox address, which differ when the window
  is moved, so the lights were let go every tick.

**Cemu's recompiler.** In Cemu's default (recompiler) mode the game halts in
the menus' tags (`csmemcpy`, "destination && source"): a `mfcr`/`mtcrf`
pair that saves a compare across a call reads back the wrong result, and
code behind an `if (count)` runs with a count of 0. With
`--force-interpreter` the same build runs on. Real hardware is the
reference; Cemu runs are interpreter runs for now.

**First hardware run** (the owner's console, before these fixes): booted,
took the 256 MB window, wrote its settings, and stopped at the cache file
setup (the seek past the end above), then at the renderer's missing device.

**Next.** The same on the console; then milestone 3, the renderer.

## Milestone 3: the renderer draws (2026-10-22)

**Works, in Cemu.** The game's own renderer (the game's
`port/linux/src/d3d8_gl.c`, unchanged) draws through an OpenGL layer over
GX2, `src/wiiu_gl.c`, arranged as the Xbox 360's `x360_gl.c` over Direct3D
9. The main menu draws whole (the ring, the planet, the stars, the menus),
and Blood Gulch loads and draws: the level, the sky, the first-person
weapon and the HUD. Screenshots: `showcase/wiiu-night/` (owner's work
folder), `*-menu4.png` and `*-bg1.png`.

How it is put together (the file's comment has the details):

- Shaders: the GLSL `d3d8_gl.c` makes, compiled one by one by CafeGLSL in
  register mode, as the game asks for them. The varyings are given
  locations by name before they are compiled, so any vertex shader goes
  with any fragment shader (Latte matches them by semantic, which CafeGLSL
  makes from the location). Each vertex shader's depth is made OpenGL's as
  it ends (z' = 2z - w): GX2's viewport puts y = 1 on the first row, as
  Direct3D does, but its depth is OpenGL's (decaf's GX2SetViewport).
- Uniforms: CafeGLSL lists an array's elements on their own ("c[1]" after
  "c" and "c[0]"); an array is taken from its first. Values are set with
  GX2SetVertexUniformReg and GX2SetPixelUniformReg, all of a program's when
  it is newly in use, else the range written since.
- Textures are linear surfaces, drawn into where they are sampled (SDL's
  Wii U render targets), BGRA texels swapped to RGBA as they are copied,
  DXT blocks as they are (GX2's surfaces take no byte swap). Depth
  attachments are GX2 depth buffers of their own.
- Vertices: the map's (the Xbox window, which `d3d8_gl.c` aliases with
  `console_gl_buffer_alias` under `HALO_CONSOLE_GL_LAYER`) are fetched
  without a byte swap, the CPU's with the swap of their element's size.
  Attributes not on a word are copied, as on the 360. Fetch shaders are
  made per layout and kept.
- Culling: in Cemu a strip's triangles came out wound the other way from a
  list's or a fan's, so strips and fans are drawn as lists in GL's order.
  To compare on the console: the bring-up switch `native_strips`.
- Cemu decodes no single 16-bit attribute: one short is fetched as two.

Bring-up switches (`--wiiu-gl-switches` at configure, then words in
`sd:/wiiu/apps/halo/gl.txt`): `log_draws`, `log_frame=N`, `no_draws`,
`no_cull`, `no_depth`, `no_blend`, `swap_winding`, `native_strips`,
`white_textures`, `clear_red`, `trace`.

**Not yet.** Blood Gulch's ground is too dark near the camera (next to
look at). Shaders are compiled every run (no cache on the SD card yet).
Scissored depth clears, MSAA, glReadPixels.
