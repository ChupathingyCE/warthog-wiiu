<h1 align="center">Warthog for Wii U</h1>

<p align="center"><b>Chupathingy on the Wii U (homebrew, Aroma).</b></p>

<p align="center">
<a href="https://github.com/ChupathingyCE/chupathingyce">The game (Chupathingy)</a> ·
<a href="https://discord.gg/chupathingy">Discord</a>
</p>

## Status

Scaffolding only. This is not playable, and it has not run on hardware.
`ninja wiiu` builds a stub that logs a line and exits; none of the game is
compiled for this console yet. We are looking for a porter: if you know the
Wii U homebrew toolchain and want to help, say so on the Discord.

## What is here

- `chupathingyce/`: the game, a submodule of
  [ChupathingyCE/chupathingyce](https://github.com/ChupathingyCE/chupathingyce).
- `port/wiiu/`: the Wii U port: `build.py` (the build rules), `include/` (the
  prefix header), `src/` (the stub), and a README with its requirements.
- `tools/wiiu_package.py`: lays out the build for an SD card (Aroma, at
  `sd:/wiiu/apps/halo/`) or for Cemu.

The Wii U is big-endian, like the Xbox 360, so byte-order and alignment work
comes first.

## Building

Clone with the submodule:

```sh
git clone --recursive https://github.com/ChupathingyCE/warthog-wiiu.git
cd warthog-wiiu
```

You need devkitPro's devkitPPC and wut (`/opt/devkitpro`, or pass
`--devkitppc DIR` and `--wut DIR`). Then:

```sh
python3 chupathingyce/configure.py --console-port port/wiiu
ninja wiiu
```

This is the game's console-port build; see "Console ports" in the game's
[docs/building.md](https://github.com/ChupathingyCE/chupathingyce/blob/main/docs/building.md).
If the toolchain is missing, configure says so and stops.

## Your own Halo files

Bring your own Halo files. This repository never contains, and we never
share, the game's maps or other data. Packaging copies the maps from a
folder you point it at, onto your own SD card.

## Links

- The game: https://github.com/ChupathingyCE/chupathingyce
- Discord: https://discord.gg/chupathingy

Licensed GPL-3.0; see `LICENSE`.
