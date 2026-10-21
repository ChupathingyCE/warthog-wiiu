#!/usr/bin/env python3
"""Lay out the Nintendo Wii U build as an SD card's files, for a console
running Aroma or for Cemu.

    python3 tools/wiiu_package.py --out ~/Downloads/chupathingy-wiiu
    python3 tools/wiiu_package.py --out ~/Downloads/chupathingy-wiiu --maps /path/to/your/maps

writes, under OUT (the SD card's root):

    wiiu/apps/halo.wuhb         the game as the Wii U Menu lists it (Aroma)
    wiiu/apps/halo/halo.rpx     the same, for Cemu or the Homebrew Launcher
    wiiu/apps/halo/maps/        your own Xbox maps, with --maps only

Copy OUT's wiiu folder onto the SD card's root. The game's folder
(sd:/wiiu/apps/halo) is where it looks for maps/ and writes its settings,
logs (logs/debug.txt), the port's log (port.txt) and its cache.

The maps are the player's own (NTSC 01.10.12.2276, from their own disc)
and never part of a package that is passed on: --maps is for your own SD
card, and the .wuhb never holds them.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_FOLDER = Path("wiiu/apps/halo")
NAME = "Chupathingy"
AUTHOR = "ChupathingyCE"


def place(source: Path, destination: Path) -> None:
    if destination.exists():
        destination.unlink()
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def wuhbtool() -> str:
    found = shutil.which("wuhbtool")
    if found:
        return found
    devkitpro = os.environ.get("DEVKITPRO", "/opt/devkitpro")
    return str(Path(devkitpro) / "tools" / "bin" / "wuhbtool")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rpx", type=Path, default=Path("build/wiiu/halo.rpx"))
    parser.add_argument("--maps", type=Path, help="your own maps folder, copied for your own SD card")
    parser.add_argument("--out", type=Path, required=True, help="the SD card's root (or a folder laid out as one)")
    parser.add_argument("--no-wuhb", action="store_true", help="the RPX alone")
    args = parser.parse_args()

    if not args.rpx.is_file():
        print(f"no {args.rpx}: run ninja wiiu first", file=sys.stderr)
        return 1
    app = args.out.expanduser() / APP_FOLDER
    app.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.rpx, app / "halo.rpx")

    if not args.no_wuhb:
        tool = wuhbtool()
        if not os.path.isfile(tool):
            print(f"no wuhbtool ({tool}): the RPX alone", file=sys.stderr)
        else:
            command = [tool, str(args.rpx), str(app.parent / "halo.wuhb"), f"--name={NAME}", f"--short-name={NAME}",
                       f"--author={AUTHOR}"]
            icon = ROOT / "port" / "wiiu" / "assets" / "icon.png"
            if icon.is_file():
                command.append(f"--icon={icon}")
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL)

    count = 0
    if args.maps:
        maps = args.maps.expanduser()
        files = sorted(maps.glob("*.map")) + [p for p in [maps / "loading.tga"] if p.is_file()]
        if not any(p.name == "ui.map" for p in files):
            print(f"no ui.map in {maps}", file=sys.stderr)
            return 1
        (app / "maps").mkdir(exist_ok=True)
        for path in files:
            place(path, app / "maps" / path.name)
        count = len(files)

    print(f"{app}: halo.rpx{'' if args.no_wuhb else ', halo.wuhb'}" + (f" and {count} map files" if count else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
