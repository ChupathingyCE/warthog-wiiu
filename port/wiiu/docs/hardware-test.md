# Testing on the console

A short checklist for a run on a Wii U with Aroma. The SD card's game folder
is `sd:/wiiu/apps/halo/`; the port's log is `port.txt` there, the game's own
is `logs/debug.txt`.

## Before the run

- `halo.wuhb` in `sd:/wiiu/apps/`, `halo.rpx` and `maps/` (NTSC
  01.10.12.2276) in `sd:/wiiu/apps/halo/`.
- No `gl.txt` for a normal run: every switch off, the log small.
- Main menu: no `init.txt`. Blood Gulch: rename `init-bloodgulch.txt` to
  `init.txt` (it holds `map_name levels\test\bloodgulch\bloodgulch`), and
  rename it back for the main menu.

## What to look at

Main menu:

1. The menu drawn, textured, as before.
2. Move through the menus for a minute: does the screen still blink, and
   does a press feel quicker than the last build?
3. HOME button menu, then back: the picture returns.
4. Close from the HOME button menu ends the game.

Blood Gulch:

1. The level loads (the first run compiles the level's shaders, about 50
   the menu did not need: expect hitches for the first minute; a second run
   reads them from `shaders/`).
2. The terrain, sky, bases and the HUD drawn; colors as on the desktop.
3. Walk and look around (GamePad or Pro Controller): the frame rate, any
   blinking, any part of the picture missing.
4. The sound.

Send back `port.txt` (and `logs/debug.txt` if the game stopped).

## What the log says

- `gl: frame N: ... (the longest M ms)` every 2 s for the first 2 minutes,
  every 30 s after, and below it `the longest frame:`, where that frame's
  time went (GPU waits, textures, shaders, the log's own writes, the flip).
  A long frame with none of these large is the game's own work.
- `the GPU did not finish`, `the GPU stopped at`: the GPU hung.
- `a visibility query's result did not come`: a query the GPU never
  answered (counted as nothing seen).

## If something is off: gl.txt

Put words in `sd:/wiiu/apps/halo/gl.txt` (one or more, separated by spaces
or lines), one change a run where possible.

| What you see | Try |
| --- | --- |
| The game stops, black screen | `gpu_check` (the GPU waited for after each step of the first frames and each new program's first draw: names the step that hangs) |
| Wrong colors or garbage on some textures | `white_textures` (all textures white: is it the textures?), then `linear_textures` |
| Missing or broken level geometry | `native_strips`, then `native_shorts`, then `unaligned_vertices` |
| Shading wrong on lit surfaces | `packed_unsigned` |
| Reflections or the sky wrong | `native_cubes` |
| Some surfaces missing | `no_cull`, then `no_depth` |
| Too dark, or fog wrong | `skip_fog` |

`native_strips`, `native_shorts`, `packed_unsigned` and `native_cubes` turn
off a workaround Cemu needed; the console may draw right without it.

To log more (port.txt grows quickly, and every line costs the frame it is
written in): `log_draws` (the first draws in full), `log_frame=N` (every
draw of frame N), `log_textures` (the first textures' surfaces and texels),
`log_shaders` (each shader read or compiled).
