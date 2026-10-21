# Wii U

Being brought up: `ninja wiiu` compiles and links the whole game for the
Wii U (`build/wiiu/halo.rpx`). It has no renderer yet. STATUS.md has
what works, what does not, and the next step.

## Requirements

devkitPro's devkitPPC and wut (`/opt/devkitpro`, or `--devkitppc DIR` and `--wut DIR`), with `elf2rpl`; `wuhbtool` for `--wuhb`.

## Build

```sh
python3 chupathingyce/configure.py --console-port port/wiiu --devkitppc /opt/devkitpro/devkitPPC --wut /opt/devkitpro/wut
ninja wiiu
python3 tools/wiiu_package.py --maps /path/to/maps --out ~/Downloads/Warthog-wiiu
```

## Testing

Cemu, then Aroma on a console (`sd:/wiiu/apps/halo/`).

## Plan

Big-endian, like the Xbox 360: the same byte-order and alignment work comes first. `--wuhb` bundles the stub only, never the maps.
