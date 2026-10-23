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

- The menus
- Blood Gulch
- The online game list

## Known issues

- This is a test build: expect crashes, missing graphics and slow frame rates.
- Other maps and modes are untested.
- Online play is new, and joining some games may fail.

## Report problems

Tell us in #wii-u on the Discord (https://discord.gg/chupathingy). Say what you
were doing, and attach `wiiu/apps/halo/logs/debug.txt` and `port.txt`.

## Checking the download

The SHA-256 of the zip is in `@ZIP@.sha256`:

```
@SHA256@  @ZIP@
```

Built from @COMMIT_URL@.
