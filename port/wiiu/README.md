# Wii U

`ninja wiiu` compiles and links the whole game for the Wii U
(`build/wiiu/halo.rpx`). It plays on hardware and joins servers online; the
repository's README has the public test build and its known issues, and
STATUS.md the bring-up log.

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

The console is a Delta Peer client (`HALO_CONSOLE_DELTA`), as the original
Xbox and the Xbox 360 are. It joins a Delta host by its wire and range,
whatever OpenCE number the host announces, and reads the host's game
settings in the layout of that number. It hears text chat and says the
controller's quick lines (no keyboard), and never hosts on Delta.
`sh port/wiiu/tests/run.sh` checks the join decision on the host.

`--wuhb` bundles the game only, never the maps.
