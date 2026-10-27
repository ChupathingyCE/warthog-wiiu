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
  servers
- HOME and Close from the HOME Menu
- Sound
- The GamePad and the Pro Controller: A to accept, B to go back
- Servers of up to 32 players in Online Games
- The Halo CE+ map pack: copy its maps to `wiiu/apps/halo/maps_cex` to join
  CE+ games

## Known issues

- Online matches run at about 15 fps (frame pacing); this is being worked on.
- Some effects, like grenade smoke, have a pink or purple tint.
- If Chupathingy stays on a dark screen at start, turn the Wii U fully off
  and on again. This can happen after closing it from the HOME Menu.
- The campaign has not been tested yet.
- Games on older OpenCE versions show "not accepting players".
- No movies: they are skipped.
- The picture is slightly inset from the edges of the TV.
- On some capture cards the TV picture may not fit until you press HOME once;
  regular TVs are fine.

## Report problems

Tell us in #wii-u on the Discord (https://discord.gg/chupathingy). Say what you
were doing, and attach `wiiu/apps/halo/logs/debug.txt` and `port.txt`.

## Checking the download

The SHA-256 of the zip is in `@ZIP@.sha256`:

```
@SHA256@  @ZIP@
```

Built from @COMMIT_URL@.
