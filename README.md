<h1 align="center">Warthog for Wii U</h1>

<p align="center"><b>Chupathingy on the Wii U (homebrew, Aroma).</b></p>

<p align="center">
<a href="https://github.com/ChupathingyCE/chupathingyce">The game (Chupathingy)</a> ·
<a href="https://discord.gg/chupathingy">Discord</a>
</p>

## Status

**Public test build.** Chupathingy plays on a Wii U: the menus, offline
multiplayer maps, and online games through Online Games, with the [C] Warthog
servers and community servers. It is early and has had little time on real
consoles. Get it from the
[test-20261022 release](https://github.com/ChupathingyCE/warthog-wiiu/releases/tag/test-20261022).

## Install

1. You need a Wii U running [Aroma](https://aroma.foryour.cafe/).
2. Download `chupathingy-wiiu-test-20261022.zip` from the release, unpack it,
   and copy its `wiiu` folder to the root of your SD card, so you end up with
   `wiiu/apps/halo.wuhb` and `wiiu/apps/halo/halo.rpx`.
3. Copy the maps from your own copy of Halo (the NTSC maps, 01.10.12.2276)
   into `wiiu/apps/halo/maps` on the SD card. The build has no game files, and
   we never share them.
4. Start Chupathingy from the Wii U Menu. The GamePad and the Pro Controller
   work: A accepts, B goes back.

## Known issues

- Online matches run at about 15 fps (frame pacing); this is being worked on.
- The campaign has not been tested yet.
- No movies: they are skipped.
- Games on older OpenCE versions show "not accepting players".
- The picture is slightly inset from the edges of the TV.
- On some capture cards the TV picture may not fit until you press HOME once;
  regular TVs are fine.

## Reporting problems

Tell us in #wii-u on the [Discord](https://discord.gg/chupathingy). Say what
you were doing, and attach `wiiu/apps/halo/logs/debug.txt` and `port.txt`.

## What is here

- `chupathingyce/`: the game, a submodule of
  [ChupathingyCE/chupathingyce](https://github.com/ChupathingyCE/chupathingyce).
- `port/wiiu/`: the Wii U port: `build.py` (the build rules), `include/` (the
  prefix header), `src/` (the console layer), and a README with its requirements.
- `tools/wiiu_package.py`: lays out the build for an SD card (Aroma, at
  `sd:/wiiu/apps/halo/`) or for Cemu.

The Wii U is big-endian, like the Xbox 360.

## Building

Clone with the submodule:

```sh
git clone --recursive https://github.com/ChupathingyCE/warthog-wiiu.git
cd warthog-wiiu
```

You need devkitPro's devkitPPC and wut (`/opt/devkitpro`, or pass
`--devkitppc DIR` and `--wut DIR`). Then:

```sh
python3 chupathingyce/configure.py --console-port port/wiiu --console-no-debug-asserts
ninja wiiu
```

`--console-no-debug-asserts` is the build testers get, faster on the
console: the debug decomp's asserts are compiled out, as the retail game had
them, except the memory checks. Leave it out for a build that stops at the
first failed assert.

This is the game's console-port build; see "Console ports" in the game's
[docs/building.md](https://github.com/ChupathingyCE/chupathingyce/blob/main/docs/building.md).
If the toolchain is missing, configure says so and stops.

The shader compiler, [CafeGLSL](https://github.com/Exzap/CafeGLSL) (MIT, v1.0.0-rc1
or later), is linked statically: unpack the release's `wiiu-ppc` archive and
pass `--cafeglsl DIR`, or put it in `/opt/devkitpro/portlibs/wiiu`.

## CI and test builds

`.github/workflows/build.yml` builds in devkitPro's `devkitpro/devkitppc` image
for every pull request, every push to main and by hand, and keeps the SD card's
folder (`wiiu/apps/halo.wuhb`, `wiiu/apps/halo/halo.rpx`) as the run's
artifact. It downloads CafeGLSL from its release and checks the archive's
SHA-256; nothing else needs setting up, and no maps are ever packaged.

To put up a test build, push a tag on the commit to build:
`git tag test-20261022 && git push origin test-20261022` (or `v<number>`).
`release.yml` builds it and publishes a pre-release, "Wii U test build", with
the zip of the card's folder, its SHA-256, and the notes in
`.github/release-notes.md` (edit that file to change what they say).

## Your own Halo files

Bring your own Halo files. This repository never contains, and we never
share, the game's maps or other data. Packaging copies the maps from a
folder you point it at, onto your own SD card.

## Links

- The game: https://github.com/ChupathingyCE/chupathingyce
- Discord: https://discord.gg/chupathingy

Licensed GPL-3.0; see `LICENSE`.
