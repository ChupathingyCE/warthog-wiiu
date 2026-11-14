This is a test build of the Wii U port of Chupathingy. It is early, it changes
often, and it has had little time on real consoles.

## Install

1. Download `@ZIP@` and unpack it.
2. You need a Wii U running Aroma. Copy the `wiiu` folder to the root of your
   SD card, so you end up with `wiiu/apps/halo.wuhb` and `wiiu/apps/halo/halo.rpx`.
3. Start Chupathingy from the Wii U Menu.

## Bring your own maps

This build has no game files, and we never share them. Copy the maps from
your own copy of Halo (the NTSC maps, 01.10.12.2276) into
`wiiu/apps/halo/maps` on the SD card.

## What works

- Boot and the menus, at 60 fps
- Blood Gulch and the multiplayer maps offline, at 30 fps
- Online play through Online Games: the [C] Warthog servers and community
  servers, OpenCE's current one (network version 44) and older ones
- HOME and Close from the HOME Menu
- Sound
- The GamePad and the Pro Controller: A to accept, B to go back
- Up to 32 players: Online Games lists only the servers that fit
- The Halo CE+ map pack: copy its maps to `wiiu/apps/halo/maps_cex` to join
  CE+ games
- Linking your Delta profile: on Online Games, press + (Start), then A for a
  code to enter at stats.chupathingy.org/connect

## Known issues

- Online matches can run slow (frame pacing); this is being worked on.
- The campaign is still being tested.
- A game that grows past 32 players is left, as a full one is.
- No movies: they are skipped.
- The picture is slightly inset from the edges of the TV.

## Report problems

Send `wiiu/apps/halo/logs/debug.txt` and `wiiu/apps/halo/port.txt` at
https://dump.chupathingy.org (platform: Wii U). Then tell us in #wii-u on the
Discord (https://discord.gg/chupathingy) what you were doing, and give the
reference code.

## Checking the download

The SHA-256 of the zip is in `@ZIP@.sha256`:

```
@SHA256@  @ZIP@
```

Built from @COMMIT_URL@.
