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

## Online: the network test plan

The build tests the network at start-up and fills ONLINE GAMES from the
console game list, as the original Xbox and the Xbox 360 do.

1. Boot to the main menu: no `init.txt` (rename it away, to
   `init-bloodgulch.txt`, if it is there).
2. Read the `net test:` lines in `port.txt` (they come within the first
   minute; below: what each should say).
3. MULTIPLAYER, then ONLINE GAMES (pick the profile if it asks). The list
   comes up: check that the Warthog servers are in it (`[C] Warthog 1`,
   `[C] Warthog 3`, ...; page with left and right, X asks for the list
   again).
4. Pick `[C] Warthog 1` and press A. It joins through internet play's
   tunnel: a few seconds, then the lobby, then the map loads.
   Warthog 1's map changes with its rotation (it is Blood Gulch only part
   of the time): the Wii U needs that map in `maps/`, or it leaves the game
   right after joining. Any `[C]` server on a map the card has will do.
5. Play two minutes: walk, shoot, get killed, respawn. Look at the
   scoreboard (the players' names and scores), the other players if any
   join, and the vehicles.
6. HOME, then Close.

Send back `port.txt` and `logs/debug.txt`.

What the log should say, in order (`port.txt`):

- `net test: start`
- `net test: byte order ok: all 27 samples as the PC's bytes, both ways`:
  the game's messages turned into the PC's byte order and back, against
  the bytes a PC build gives the same samples. `FAILED` names the message
  kinds that differ (a number each): send that line.
- `network: up (already connected), a private address (N ms)`: nn::ac
  connected (Aroma usually has the link up already; otherwise
  `network: not connected yet; connecting ...` comes first). Never an
  address, only its kind.
- `net test: UDP to this console's own port came back, by its own address`
  (or by the loopback: datagrams work, but not out to the network and back).
- `net test: UDP to the loopback came back`: the way internet play hands
  the host's datagrams to the game.
- Three `net test: UDP out to the internet (STUN stun.cloudflare.com, ...)`
  lines, each `answered (N ms)` or `FAILED: <why>`: a plain socket; a
  socket made on one thread and used from another (as internet play's
  tunnel is); a socket given 1 MB buffers (as the tunnel and the game asked
  for before; the line gives whether the console took or refused each
  size, with errno and nsysnet's code). Which of them answers says why the
  tunnel could not reach the host: send all three lines.
- `net test: DNS warthog.milenko.org resolved (N ms)`
- `net test: HTTP GET /v1/console/games: status 200, N bytes, N games
  (N named Warthog)`: the list as ONLINE GAMES shows it.
- `game list: N games (format 1)`, then `net test: done`.

The `net:` lines are every socket's life and every socket call that
failed: `net: socket N made (UDP)`, `bound to ..., port N`, the options it
was given, its first datagram out to and in from each kind of address
(never the address), and `net: <call> on socket N, <what was asked> failed:
errno N (<name>), nsysnet N, Winsock N`. Internet play's tunnel is the
first UDP socket bound to any address with `SO_SNDBUF 1048576 asked for`
after it; its `first datagram out to a public address` is its first STUN
request or probe, `first datagram in from a public address` the first
answer. Then `Internet play: connected to host ...`, `tunnel: the first
packet from a peer opened`, and `tunnel: N datagram(s) from the host's port
5150 for the game's port N: sent to the game`: the handover to the game.

Never copy `pad-test.txt` to the console's card: it lets a file press the
GamePad's buttons, for unattended runs in Cemu (src/wiiu_pad.c).

In `logs/debug.txt`, when joining: `online games: joining [C] Warthog 1`,
`joining a host of network version 32`, `the host's game was advertised
through the tunnel`, `successfully joined a net game`, `received updated
game settings from the server; there are N players on N machines`, then
the map's `tag byte swap` lines and `local machine is loaded & ready to
play`.

If something fails, the lines to look at:

| What you see | Where to look |
| --- | --- |
| ONLINE GAMES says it could not reach the game list | `port.txt`: `network:` (no configuration, could not connect), `net test: DNS ... FAILED`, `net test: HTTP GET ... FAILED: <why>`, `game list: try N of 3` |
| The list is there, but no Warthog servers | `net test: HTTP GET ... (N named Warthog)`; the servers may be down: check the site's list |
| "That game is not accepting players." | the game is full or in progress without room; pick another |
| "That game can't be joined from this version." | `debug.txt`: `online games: the host's game is not one this version joins` (another network version) |
| "Internet play is off" | `config.toml`: `network.online` must be true |
| "Connecting..." then "The host did not answer." or "Could not connect to the host." | `debug.txt`: `online games: joining`, then `online games: the host's game was not advertised within N seconds` or `the connection to the host's game failed` (internet play's tunnel to the host) |
| Back at the menu right after joining | `debug.txt`: a map missing (`maps/` has not the server's map), `network_game_client_game_settings_updated() failed`, `invalid message_server_game_settings_update` (the settings record's byte order) |
| In the game, but players jump, scores stay 0, or the game drops you | `debug.txt`: `the host:` lines (the host's notices), `connection has been silent`; `port.txt`: the `net test: byte order` line |

In Cemu (interpreter) this build passed every step: the byte order test,
the list (15 games), and joining `[C] Warthog 1` on Chill Out, loaded and
playing.

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
