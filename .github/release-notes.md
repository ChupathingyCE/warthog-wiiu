Wii U test build 6. This is a test build: it runs in the Cemu emulator on our
side, but it hasn't had time on a real Wii U yet. Please play it, report bugs
(crashes, freezes, visual or sound bugs), and send screenshots of what you see.
Once the bugs are ironed out, it becomes the next Wii U release.

## What's new since test build 5 (November 13)

- Caught up with a week of Chupathingy: the new game lobby, the button layout
  row in the settings, the loading screen's colors on the Wii U, and a display
  gamma setting.
- Speed work in the game's busiest parts on consoles (objects, tags, lights).
- BirchWood's speedups: the map read the game is waiting for goes first, and
  fewer walks over every object when objects are deleted or decals are made.
- If the game stops, the halt screen now says what stopped it.
- The missing maps screen shows the SD card as `sd:/`.

## Install

1. Download `@ZIP@` and unpack it.
2. You need a Wii U running Aroma. Copy the `wiiu` folder to the root of your
   SD card, so you end up with `wiiu/apps/halo.wuhb` and `wiiu/apps/halo/halo.rpx`.
   This replaces the last test build; keep your maps and your `config.toml`.
3. Start Chupathingy from the Wii U Menu.

## Bring your own maps

This build has no game files, and we never share them. Copy the maps from
your own copy of Halo (the NTSC maps, 01.10.12.2276) into
`wiiu/apps/halo/maps` on the SD card.

## What to try

1. Boot to the main menu. Is the HALO logo there, with all the menu text?
   Send a screenshot.
2. Blood Gulch for two minutes. Are the ground and the cliffs textured? How is
   the frame rate? Send a screenshot.
3. Settings: can you change the button layout and the controls?
4. Campaign: The Pillar of Autumn from the start. Does it load, and how long
   does it take?
5. Online: Online Games, a [C] server, two minutes.
6. HOME in and out of a game, then Close from the HOME Menu. Does it exit
   cleanly?

## Known issues

- Online matches can run slow; this is being worked on.
- The campaign is still being tested.
- No movies: they are skipped.

## Report problems

Send `wiiu/apps/halo/logs/debug.txt` and `wiiu/apps/halo/port.txt` at
https://dump.chupathingy.org (platform: Wii U). Then tell us in #wii-u on the
Discord (https://discord.gg/chupathingy) what you were doing, give the
reference code, and add your screenshots. Ideas are welcome in the ideas
channel, but for this build bugs come first.

## Checking the download

The SHA-256 of the zip is in `@ZIP@.sha256`:

```
@SHA256@  @ZIP@
```

Built from @COMMIT_URL@.
