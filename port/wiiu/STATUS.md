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

**First hardware run** (a test console, before these fixes): booted,
took the 256 MB window, wrote its settings, and stopped at the cache file
setup (the seek past the end above), then at the renderer's missing device.

**Next.** The same on the console; then milestone 3, the renderer.

## Milestone 3: the renderer draws (2026-10-22)

**Works, in Cemu.** The game's own renderer (the game's
`port/linux/src/d3d8_gl.c`, unchanged) draws through an OpenGL layer over
GX2, `src/wiiu_gl.c`, arranged as the Xbox 360's `x360_gl.c` over Direct3D
9. The main menu draws whole (the ring, the planet, the stars, the menus),
and Blood Gulch loads and draws as on the desktop and the Xbox 360: the
level lit by its lightmaps and bump maps, the sky, the first-person weapon
with its reflections, the HUD. Not yet seen on a console.

How it is put together (the file's comment has the details):

- Shaders: the GLSL `d3d8_gl.c` makes, compiled one by one by CafeGLSL in
  register mode, as the game asks for them, and kept on the SD card
  (`src/wiiu_shader_cache.c`, `sd:/wiiu/apps/halo/shaders`, by a hash of the
  text and the compiler's version): the main menu's 50 shaders took 6.4 s
  to compile in Cemu and 37 ms to read back the next run. The varyings are
  given locations by name before they are compiled, so any vertex shader
  goes with any fragment shader (Latte matches them by semantic, which
  CafeGLSL makes from the location). Each vertex shader's depth is made
  OpenGL's as it ends (z' = 2z - w): GX2's viewport puts y = 1 on the first
  row, as Direct3D does, but its depth is OpenGL's (decaf's GX2SetViewport).
- Uniforms: CafeGLSL lists an array's elements on their own ("c[1]" after
  "c" and "c[0]"); an array is taken from its first. Values are set with
  GX2SetVertexUniformReg and GX2SetPixelUniformReg, all of a program's when
  it is newly in use, else the range written since.
- Textures made with their texels (the map's bitmaps), DXT textures, cube
  maps and volumes are in GX2's tiling, written through linear surfaces and
  GX2CopySurface (Cemu read linear cube maps as black; the console stopped
  at linear mipmapped and DXT textures). Textures made empty (render
  targets, atlases) are linear, drawn into where they are sampled (SDL's
  Wii U render targets). BGRA texels are swapped to RGBA as they are
  copied, DXT blocks kept as they are (GX2's surfaces take no byte swap).
  Depth attachments are GX2 depth buffers of their own.
- Vertices: the map's (the Xbox window, which `d3d8_gl.c` aliases with
  `console_gl_buffer_alias` under `HALO_CONSOLE_GL_LAYER`) are fetched
  without a byte swap, the CPU's with the swap of their element's size.
  Attributes not on a word are copied, as on the 360. Fetch shaders are
  made per layout and kept.

What Cemu needed, each with a bring-up switch to compare on the console
(none of these is known yet to be needed on hardware):

- Strips and fans are drawn as lists in GL's order: a strip's triangles
  came out wound the other way from a list's or a fan's (`native_strips`).
- One short attribute is fetched as two: Cemu decodes no single 16-bit
  attribute.
- The packed normals (`unpack_normpacked3`, nv2a_vsh.c) are unpacked with
  unsigned shifts and masks: with the arithmetic shifts the bump-mapped
  lighting's light vectors were wrong and the ground was black near the
  camera (`packed_unsigned` keeps the original).
- Cube maps are read as arrays of their faces by a lookup in the shader
  (OpenGL's sc, tc, ma table, its level of detail from the direction's
  gradients): CafeGLSL's cube lookups read the wrong face and place in Cemu
  (`cube_probe` samples a labeled cube in 24 directions, reads the pixels
  back and logs them; `native_cubes` keeps CafeGLSL's lookups).
  textureGrad came out wrong in Cemu, textureLod right.

Bring-up switches (`--wiiu-gl-switches` at configure, then words in
`sd:/wiiu/apps/halo/gl.txt`): `log_draws`, `log_frame=N`, `log_at=SECONDS`,
`log_textures`, `trace`, `no_draws`, `no_cull`, `no_depth`, `no_blend`,
`no_mips`, `lequal`, `dst_alpha_one`, `skip_fog`, `skip_program=N`,
`swap_winding`, `white_textures`, `clear_red`, `screen_magenta`,
`blit_green`, `math_cubes`, and the four above. A logged frame also writes
each program's GLSL beside the log (`p<program>.vert`, `.frag`).

Screenshots: captures from the first hardware run (`*-mainmenu-cached-shaders`,
`*-bloodgulch-correct`, and `*-reference-macos-bloodgulch` from the macOS
build at the same spawn).

**Not yet.** Scissored depth clears (never seen used), MSAA, glReadPixels.
Hardware: everything above.

## Milestone 4: the controllers and the sound (2026-10-22)

**Input, in Cemu.** `debug.test_input` (menu:, bot:, look:) works on the
Wii U as on the desktop (`platform/wiiu_input.c`), over the first
controller's state, so the menus can be driven without a hand on a
controller: in Cemu, `menu:` presses moved the main menu to MULTIPLAYER and
A on CAMPAIGN opened the new profile's name screen; in Blood Gulch,
`bot:1` walked, turned, fired and threw grenades (the explosions and the
HUD's messages drawn). Presses are held for
600 ms of their second (a frame takes about 300 ms in Cemu's interpreter).
The GamePad and Pro Controller themselves (`src/wiiu_pad.c`) are not tested:
Cemu's emulated controllers take the host's input, which an overnight run
does not give.

**Sound, in Cemu.** `src/wiiu_audio.c` plays the mixer's float stereo on
two AX voices the way SDL's Wii U audio does: 16-bit big-endian samples,
deinterleaved into two buffers each voice loops over, AX's frame callback
moving the loop to the buffer filled next, a thread of the port's filling
them, the voices made on AX's core and again when AX restarts after the
HOME button menu. The log every 30 s counts AX frames, buffers filled and
the loudest sample: the main menu's music reaches the voices. In Cemu's
interpreter, mixing a 21 ms buffer takes about 20 ms, so the filling thread
keeps up with only half the buffers and each plays twice; the console's CPU
is the measure (not chased).

## Performance in Cemu (interpreter, not chased)

- Main menu: about 200 ms a frame (5 frames a second); Blood Gulch about
  400 ms a frame, 123 draws.
- Shaders: about 130 ms each to compile in Cemu, 0.7 ms to read back from
  the SD card.
- Cemu's recompiler still stops in the menus' tags (milestone 2), so every
  run is an interpreter run.

**Next.** On the console: the switches above, one at a time, to learn
which of Cemu's needs hardware shares; the frame time; the GamePad and Pro
Controller; the sound.

## First hardware run of the renderer (2026-10-22)

**Seen on the console.** GX2 and CafeGLSL start; the start-up shaders and
the main menu's first 11 shaders compile and are written to the SD card
(their files' times: 4 s for all 11, so compiling is not the stall). Then
the screen stays black and the music is choppy, while the game's thread
still runs: it reads the controllers every frame (`platform_pump_events`),
which is where HOME and Close ended it. No frame line came (one every 300
frames), so frames took seconds each.

What fits all of it: a GPU that stopped. GX2's waits for the GPU give up
after GX2's GPU timeout and return FALSE (decaf-emu's gx2_state.cpp and
gx2_cbpool.cpp; decaf says 10 s, the console logged 1000 ms); the frame's
end waits for the frame before last, so each frame takes that long and no
flip comes. Not certain
yet, so the next run logs it:

- every GPU wait timed, and any that gives up logged with the timestamps
  retired and submitted; the flip wait's 10-vsync give-ups logged;
- the first two frames that draw, and each program's first draw in any
  frame, wait for the GPU after each step (draws, clears, blits, texture
  copies, visibility queries, the swap) with a 1 s timeout: "the GPU
  stopped at" names the first step it did not finish, and the next frame's
  draws are logged in full (`no_gpu_check` turns it off);
- a line every 2 s for the first 2 minutes: frames, the longest frame,
  time in GPU waits, flip waits, visibility queries, shader compiles and
  the SD card's reads and writes;
- a thread ("gl watch") logs where the game's thread is when it stays in
  one place for more than 3 s;
- each of the first 64 shaders: read or compiled, with its times and
  whether its file was written whole (uniforms now a count, not a line
  each);
- Cemu's four workarounds and their switches in the log at the start
  (`native_shorts` is new: one 16-bit attribute fetched as one).

The visibility queries' wait (the game reads each result) was a loop with
no end; it now gives up after 100 ms, counting no samples.

**The music.** AX is fed on time: 1407 buffers of 1024 samples in 10000 AX
frames (30 s) is real time. The mixer's streams are refilled by the game's
thread (`DirectSoundDoWork`, every frame: dsound_sdl.c), so a frame of
seconds starves them. It should follow the frame rate.

**Second run: the step named.** Frames 1 to 18 (no draws) were presented.
Frame 19's first draw stopped the GPU for good: "the GPU stopped at frame
19's first indexed draw of a program" (program 4117, a strip of 1227
indices drawn as a list into framebuffer 4126, 1280x720, depth test off,
additive blend), with nothing checked before it in that frame (its clears
were not checked). The run had no gl.txt, so all four of Cemu's
workarounds were on. Every frame after took about 1.15 s (the GPU and flip
waits giving up).

Not yet known which part of that draw: its clears, the vertices read in
place from the Xbox window, its textures (linear, DXT) or its targets (a
linear color texture with a tiled depth buffer in MEM2). The next run:

- vertex buffers are given to GX2 on GX2_VERTEX_BUFFER_ALIGNMENT (0x40,
  wut's gx2/enum.h; GX2R's buffers are), the window's vertices read from
  the 0x40 below their start with each attribute's offset moved on by as
  much (`unaligned_vertices` gives GX2 the start as before);
- each framebuffer's first clear is checked, and logged with its buffers'
  addresses and tile modes, so a clear that stops the GPU is named;
- the first draws, and each program's first draw until the GPU stops, log
  everything GX2 is given: the primitive, index type, index address
  (physical, and its alignment), the indices' range, each vertex buffer's
  address, alignment, size and stride, each fetch, and the color and depth
  buffers;
- `copy_vertices` copies every attribute to the ring (aligned, as many
  vertices as the indices reach), to rule the window in or out; the other
  switches that narrow a draw down are listed in the log at the start.

**Third run: the textures.** With every texture replaced by the 1x1 one
(`white_textures`) the console drew 118 draws a frame at 60 fps, flipped,
and showed the menu: shaders, vertices, targets and presentation work. With
the real textures the first textured draw (program 4117: a 64x64 DXT1 of 7
levels, a 256x256 and a 128x128 RGBA8 of 9 and 8 levels, all linear) did
not finish, and the GPU retired about one submission a second after it.
The only linear textures known to work on the console are of one level:
SDL's (`SDL_rtexture_wiiu.c`, `GX2_TILE_MODE_LINEAR_ALIGNED` with
`mipLevels = 1`), Butter and Jelly's video planes (R8 and R8G8, one level)
and our 1x1. Games' textures are tiled (decaf's
`GX2CalcSurfaceSizeAndAlignment`, gx2_surface.cpp: a default 2D texture is
`Tiled2DThin1`).

So textures made with their texels and every DXT texture are now tiled,
written through a linear surface and `GX2CopySurface`, as cube maps
already were; a part of a tiled level written later, and glGenerateMipmap
on one, go through a linear twin (copied out, changed, copied back).
Render targets and atlases made empty stay linear. Every texture's memory
starts zeroed, so levels never given texels read as black. Cemu draws Blood
Gulch as before. New in the log: each of the first 24 textures' surfaces
(tile mode, use, levels, pitch, alignment, image and mipmap addresses and
their alignment, swizzle, offsets). New switches: `linear_textures` (the
old linear textures), `only_tex_format=0xNN` (only that GX2 format sampled,
the rest as nothing bound), `no_aniso`.

## The textured main menu on the console (2026-10-22)

**Seen on the console.** With the tiled textures the main menu is drawn,
textured, at 60 fps, and the GamePad moves through it. A tester noted a slight
delay, and an occasional blink when moving through the menus. The log: 120
frames every 2 s, but frames of 90 to 540 ms while moving through the menus
(no GPU wait or shader behind them), and 1 to 1.5 s of every 2 s spent
waiting for flips.

**The blink.** Three things a frame did on the console that it did not in
Cemu:

- `GX2CopySurface` in the middle of a frame (a texture given texels, now
  tiled): on the console GX2's copies and clears are draws of GX2's own
  (Cemu's `GX2_Surface_Copy.cpp` notes the real one sets up draw calls,
  where Cemu's is a command of its own; libwhb sets its context state again
  after its clears, `WHBGfxClearColor`). The context state was set again
  after clears but not after copies, and the layer kept the copy's target
  as its own: the rest of that frame could be drawn into the texture just
  copied, not the screen. Now every copy and clear is followed by the
  context state and the layer's bindings set again (`gx2_own_work_done`).
- Textures written in place by the CPU (linear ones: glTexSubImage, the
  mipmaps made on the CPU) while draws already queued still sampled them.
  Now, as SDL does (`WIIU_SDL_LockTexture`, `WIIU_TextureWaitDone`), each
  texture keeps the time stamp of its last draw, and a write in place waits
  for it.
- The long frames: each program's first draw waited for the whole GPU and
  wrote ten lines to port.txt, each line opening, appending and closing the
  file on the SD card (bring-up's `gpu_check`). Of the frames over 80 ms after the
  first 10 s, three (344, 370 and 89 ms) fell in periods that logged such
  draws, the 537 ms one in the period that wrote the new profile's files,
  the 170 ms one with only the game's "event handler function failed"
  logged; frames of about 90 ms in the first 10 s have no cause in the log. The next run's
  frame lines say where the longest frame's time went.

**The delay.** The swap waited for the previous frame's flip before copying
this one (libwhb's `WHBGfxBeginRender` wait, but done one frame late): a
frame drawn from input read one flip earlier showed a flip later. The wait
is now for this frame's own flip, right after its swap, so the game's next
frame, which reads its input first, starts with nothing queued: a frame's
input to its flip is at most one frame. Swap interval 1 (SDL's
`WIIU_SDL_SetVSync` with vsync, libwhb's).

**The log.** Off unless gl.txt asks: the first draws, each program's first
draw and the GPU waits after it (`gpu_check`, new), the first textures'
surfaces (`log_textures`), each shader read or compiled (`log_shaders`,
new). The frame line comes every 2 s for 2 minutes, then every 30 s, with
a second line: where the longest frame's time went (GPU waits, textures,
shaders, the log's own SD card writes, the flip), the textures given texels
and in what time, and the writes in place that waited.

Cemu (interpreter) draws Blood Gulch as before with the same build. The
console's checklist: docs/hardware-test.md.

## Online: the game list and joining (2026-10-22)

**Works, in Cemu (interpreter).** At start-up a thread (`src/wiiu_online.c`,
the Xbox 360's `x360_online.c` on the Wii U's threads and clock) logs
`net test:` lines to port.txt: the byte order's self-test, the network,
a UDP datagram to the console's own port, `warthog.milenko.org` looked up,
and `GET /v1/console/games` (plain HTTP/1.0) fetched and parsed. That list
is the Multiplayer menu's ONLINE GAMES (`game/wiiu_browser_screen.c`,
`HALO_CONSOLE_ONLINE_GAMES`): the original Xbox's screen, parser and words,
as the 360 has them. In Cemu the list came (15 games, the Warthog servers
among them), and A on `[C] Warthog 1` joined it through internet play's
tunnel: the lobby, the settings record, Chill Out loaded, the player
spawned and played.

How it is put together:

- The network comes up as Butter and Jelly brings it up on the Wii U
  (`wiiu_network_start`, wiiu_net.c; its http_curl.cpp:306-338 and
  platform_wiiu.cpp:184): wut's start-up has already started the socket
  library and asked nn::ac to connect (ACConnectAsync, wut_socket_common's
  __init_wut_socket); then ACInitialize, ACIsApplicationConnected, and only
  if it is not connected, ACGetStartupId and ACConnectWithConfigId; the
  address by its kind only. A failure is tried again after 10 s.
- The cross-play byte order is the game's (ChupathingyCE/chupathingyce,
  branch console-network-wire, on the Xbox 360's console-network): the
  distributed netcode's messages, the game types' states, the game
  settings record and the advertisement, little-endian on the wire as the
  PCs and the original Xbox send them. Its self-test
  (`network_wire_self_test`) compares 27 samples with the bytes the 32-bit
  Linux build gives them, both ways: it passed in Cemu.
- The list asks without `packs=1`: this base has no Xbox map packs yet.
- The submodule is the game's `wiiu-network` branch: `wiiu-night` with the
  shared byte-order commits picked onto it (this base is older than the
  game's main; on it the settings record has no Bomb rules to turn).

**Not yet.** Hardware. A game whose map the SD card has not is left right
after joining (the map's check, as on the desktop). Hosting from the Wii U
is untested.

The console's checklist: docs/hardware-test.md, "Online".

## Online on the console: the first run (2026-10-22)

**Works, on the console.** The byte order (27 of 27), UDP to its own
address, DNS, the console game list over HTTP (21 games) and ONLINE GAMES.

**Does not.** Joining `[C] Quick Match` and `[C] Warthog 1`: internet play
reached the host through the brokers (TCP), but the tunnel never heard
from it, nor from the STUN servers (no `this machine's public address`
line); the join gave up ("could not connect") or waited. And one
`winsock error #10022: WSAEINVAL` when the game's network started.

What Cemu shows with this build's socket log: there the 10022 is the bind
of the game's port 5151 (nsysnet 99999, Cemu's own; the game then takes
another port). On the console the bind did not fail (no "the client port
is in use" line), so its 10022 came from the options the game gives each
socket before binding: SO_BROADCAST with -1, or the 1 MB SO_SNDBUF and
SO_RCVBUF it and the tunnel ask for (more than the most an application can
give the console's network for all its sockets' buffers: wut's
nn/nets2/somemopt.h, 128 KB to 3 MB). nsysnet's errors that wut has no
errno for all become EINVAL (wut_socket_common.c), so 10022 alone does not
say which.

This build:

- every socket keeps the system's buffer sizes (a size asked for is taken
  without an error, as Linux takes one above its limit), and SO_BROADCAST
  is given 1 (as Butter and Jelly's discovery.cpp does);
- every failed socket call is logged with its option or destination kind,
  errno, nsysnet's own code and the Winsock error; each socket's life and
  its first datagram out to and in from each kind of address; the
  tunnel's packets and the datagrams it hands the game (P2P_TRACE_DATAGRAMS,
  as the original Xbox logs them);
- the network test also sends UDP out to the internet: a STUN request from
  a plain socket, from a socket used on another thread than the one that
  made it (as the tunnel), and from one given 1 MB buffers through nsysnet
  itself; and UDP through the loopback (the tunnel's way to the game).

In Cemu, with this build, `[C] Warthog 1` was joined through the tunnel
again (connected, advertised, joined; Cemu's card did not have the map
being played, so it left after the settings record, as it should).

**Next.** The console's run: the three STUN lines and the tunnel socket's
`net:` lines say which way of making a UDP socket reaches the internet
from the Wii U, and the next build makes the tunnel that way.

## Online on the console: the second run (2026-10-22)

The socket log found both. The 10022 is the game's 1 MB SO_SNDBUF and
SO_RCVBUF (refused, nsysnet 11; the console's own sizes are 1492 and 4096
bytes). And UDP reaches the internet from every kind of socket (all three
STUN tests answered), but every receive on internet play's tunnel failed
with EMSGSIZE (errno 122, nsysnet 12): the tunnel reads into 2048 bytes,
and the console refuses a datagram socket's receive that long whatever the
datagram's size (the game's 1200 and the test's 512 were taken). Every
answer from the host and the STUN servers was lost.

This build: a receive refused with EMSGSIZE is asked again at once with
shorter lengths (1472 down to 512), the longest taken kept as the limit
and logged; buffer sizes step down from the one asked for to the largest
the console takes (64 KB at most, then 32, 16, 8), logged; and the network
test tries receive lengths from 4096 down and says which are refused and
whether a refused receive keeps the datagram. In Cemu it joined
`[C] Warthog 1` through the tunnel again.

## On the game's main: the first public test build (2026-10-22)

**Works, in Cemu (interpreter).** The game's main (Grifball and Bomb, the
audio set, the byte order merged as #429 and #431) boots to the main menu,
skips the movies (no FFmpeg on the console: bink_null.c, as a missing movie
file) and loads Blood Gulch.

- The submodule is the game's `wiiu-test-build` branch: main with
  `wiiu-night` (game PRs #411, #418, #421: the console's waits, the thread
  stop and the GL layer) merged, until those are in main.
- The map list (`HALO_MAP_LIST`) is built, as on the Xbox and Xbox 360: the
  game browser and the lobby name their maps by it. The console list still
  asks without `packs=1`.
- `wiiu_sdl.c` answers the mixer's newer questions: the output's format
  (stereo), the stream's device and `SDL_GetTicks`.
- Cemu's recompiler stops it while the menus are built (a call's saved
  values read back as zero); run Cemu with `--force-interpreter`.

## Map packs, big games and the menu stall (2026-10-27)

**Works, on the console.** Joined the Halo CE+ server on hardware and played
Underground (ratrace@cex) at about 15 fps.

- The console list asks `packs=1&players=32`: map pack games and servers of
  up to 32 players show, as on the other consoles.
- The GL layer's description is logged once, not before every draw: release
  builds wrote it 2,449 times and the menus stalled on the SD card.
- The game's tag byte swap swaps a model's shared index ranges once (game
  #465): Halo CE+ maps share them between parts, and a range swapped twice
  drew as stray triangles over the level.
- Map pack maps past the 47 MB multiplayer cache files take a campaign map's,
  as on the Xbox (game #466): Underground (67 MB) stopped with a dirty disc
  error.
- Known: a pink or purple tint on some effects (grenade smoke); a dark screen
  at start after Close from the HOME Menu left the last exit unfinished
  (power off fully to recover). The end now stops the sound's thread and
  the exit trace's before the system's exit, as SDL's Wii U port does
  (docs/exit.md); not yet tested on the console.

## Halo CE+ at 15 fps: what the logs show (2026-10-28)

The 10-27 hardware logs (Underground, ratrace@cex, about 15 fps) have three
things in them, none of them the map itself:

- **The log.** "settings: no such setting display.particle_limit" (and the
  light limits) was written about three times a frame: that build's
  settings had no such rows on the Wii U. Each line opens, appends and
  closes port.txt on the SD card, so 500 to 1500 ms of every 2 s went to
  the log. The game's main has had the rows on every build and logs an
  unknown name once since game 9f8ed7d1, which this repo's submodule has.
- **The limits.** With no rows, the Wii U ran with no dynamic light limit,
  the particles moved every frame and the 8192-particle cap. Fixed by the
  same commit.
- **The flip.** The rest of the frame was mostly the wait for the flip
  (about 30 ms a frame), which is the GPU's work plus the vsync. The logs
  cannot tell the GPU's share from the vsync's.

This build:

- port.txt is written by a thread of the log's own once ProcUI is up: the
  thread that logs queues the line and goes on. `wiiu_log_flush` still
  writes everything before it returns, and the end stops the thread first
  (proc_end). The frame line's "waiting for the log" is now the game's
  thread's wait only.
- Render targets (textures made empty with one level, the renderer's,
  d3d8_gl.c) are made again in GX2's tiling when first drawn into, as the
  screen's color buffer is; before, the whole scene was drawn into a linear
  1280x720 surface in MEM2. `linear_targets` in gl.txt keeps them linear.
  Cemu (interpreter) draws the main menu and Blood Gulch as before.
- `debug.gpu_stats = true` (the game's setting, off by default) adds two
  lines to each frame line: the game's thread's working time against its
  waits, this layer's share by part (state, vertices copied, strips made
  lists, index reads), the GPU's time from a frame's first command to its
  last (GX2SampleTopGPUCycle and GX2SampleBottomGPUCycle; Cemu gives 0),
  draws, draws into linear targets, uniforms and ring bytes.

In Cemu, Blood Gulch: 123 draws, this layer 13 to 15 ms of a 75 ms frame
(interpreter), 36,000 indices a frame made lists from strips.

**Next, on the console:** a CE+ map with `debug.gpu_stats = true`. If the
GPU's time is most of the frame: the main render target and its depth in
MEM1 (they are in MEM2; MEM1 is given back at each HOME button menu, so
they need making again there), HiZ on the depth buffers, and the cube maps'
lookup in the shader (`native_cubes`). If the game's thread is: the strips
drawn natively (`native_strips`, a Cemu workaround that may not be needed
on hardware), and a frame in flight while the next is made.
