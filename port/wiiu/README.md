# Wii U

Being brought up: `ninja wiiu` compiles and links the whole game for the
Wii U (`build/wiiu/halo.rpx`). It has no renderer yet. STATUS.md has
what works, what does not, and the next step.

## Requirements

devkitPro's devkitPPC and wut (`/opt/devkitpro`, or `--devkitppc DIR` and `--wut DIR`), with `elf2rpl`; `wuhbtool` for `--wuhb`. For shaders, CafeGLSL's Wii U library (v1.0.0-rc1 or later: its portlib in `/opt/devkitpro/portlibs/wiiu`, or `--cafeglsl DIR`); docs/renderer.md.

## Build

```sh
python3 chupathingyce/configure.py --console-port port/wiiu --devkitppc /opt/devkitpro/devkitPPC --wut /opt/devkitpro/wut
ninja wiiu
python3 tools/wiiu_package.py --maps /path/to/maps --out ~/Downloads/Warthog-wiiu
```

## Testing

Cemu, then Aroma on a console (`sd:/wiiu/apps/halo/`).

## Online

At start-up a self-test logs `net test:` lines to `port.txt` (the byte
order, the network, UDP, DNS, the console game list over plain HTTP); the
list is the Multiplayer menu's ONLINE GAMES, which joins a game through
internet play's tunnel, as on the original Xbox and the Xbox 360. The
checklist: docs/hardware-test.md.

## Plan

Big-endian, like the Xbox 360: the same byte-order and alignment work comes first. `--wuhb` bundles the stub only, never the maps.
