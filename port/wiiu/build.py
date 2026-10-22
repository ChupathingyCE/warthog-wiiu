"""Ninja rules for the Nintendo Wii U build (``ninja wiiu``): the port's
build.py, which the game's configure.py loads (the game's
tools/console_build.py). From this repository's root, with the game in
chupathingyce/ (a submodule):

    python3 chupathingyce/configure.py --console-port port/wiiu
    ninja wiiu

Target: the Wii U's Espresso (PowerPC 750 family), 32-bit big-endian, with
devkitPPC's GCC and newlib and wut, producing build/wiiu/halo.rpx.

It is the Linux build's arrangement (the game's tools/linux_build.py) on the
console: the game's sources (port/linux/port.json), the port's game units
(port/linux/game), the desktop platform layer (port/linux/src) less what the
console does its own way, the third-party libraries the platform layer uses,
and port/wiiu/src. The game and the platform layer see the Xbox SDK
declarations of the game's port/include/xdk, and SDL's headers (downloaded
once, as the dedicated server's build does: its OpenGL declarations and
types; the functions the platform layer calls are the port's,
port/wiiu/src/wiiu_sdl.c). port/wiiu/src/wiiu_*.c talk to the console
through wut.
"""

import os
import shlex
from pathlib import Path
from typing import Any, List, Optional

from tools.console_build import ConsolePort, console_defines
from tools.ninja_syntax import Writer
from tools.version import version

# the game's own folders, under the game's directory
XDK_INCLUDE = Path("port/include/xdk")
LINUX_INCLUDE = Path("port/linux/include")
LINUX_SRC = Path("port/linux/src")
LINUX_GAME = Path("port/linux/game")
CONSOLE_INCLUDE = Path("port/console/include")
CONSOLE_GAME = Path("port/console/game")
TOML_DIR = Path("port/third_party/tomlc17")
EXPAT_DIR = Path("port/third_party/expat")
EXPAT_SOURCES = ("xmlparse.c", "xmlrole.c", "xmltok.c")
KCP_DIR = Path("port/third_party/kcp")
QRCODEGEN_DIR = Path("port/third_party/qrcodegen")
MONOCYPHER_DIR = Path("port/third_party/monocypher")
MUSL_MATH_DIR = Path("port/third_party/musl-math")
STB_DIR = Path("port/third_party/stb")
ZLIB_DIR = Path("port/third_party/zlib")
ZLIB_SOURCES = ("adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "uncompr.c", "zutil.c")
ZLIB_DEFINES = ("-DZ_PREFIX", "-Dz_errmsg=z_port_errmsg")

# the CPU (wut's own rules: -DESPRESSO -mcpu=750 -meabi -mhard-float) and
# the sections the linker drops when unused
TARGET_FLAGS = [
    "-DESPRESSO",
    "-mcpu=750",
    "-meabi",
    "-mhard-float",
    "-ffunction-sections",
    "-fdata-sections",
    "-D__WIIU__",
    "-D__WUT__",
    "-DHALO_CONSOLE",
    "-DHALO_WIIU_CONSOLE",
    "-DHALO_BIG_ENDIAN",
    "-O2",
    "-g",
    # (no temporary assembly files: the generated HUD source is large)
    "-pipe",
]

# the game's and the desktop platform layer's ABI, as the other ports give
# it (the game's tools/linux_build.py): 16-bit wchar_t (UTF-16 strings in tag
# data and saved games), anonymous structures, tentative definitions shared
# between units, no optimisations that assume the absence of
# MSVC-tolerated UB, the same floating point results on every port (no fused
# multiply-adds, which PowerPC would otherwise emit everywhere), and char
# signed as MSVC has it (PowerPC's is unsigned: a char holding NONE would
# read 255). Small structures come back in registers on this ABI already.
ABI_FLAGS = [
    "-fshort-wchar",
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-ffp-contract=off",
    "-fsigned-char",
    # newlib's wide string functions assume a 32-bit wchar_t
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

# the game's C89, as MSVC took it
GAME_FLAGS = [
    "-std=gnu89",
    # newlib restricted to ISO C, so that POSIX names (random, strnlen) cannot
    # collide with the game's own declarations (as the Linux build has glibc)
    "-D__STRICT_ANSI__",
    "-w",
    "-fpermissive",
]

PLATFORM_FLAGS = [
    "-std=gnu11",
    "-D_GNU_SOURCE",
    "-DHALO_LINUX_PLATFORM_LAYER",
    "-Wall",
    "-Wno-unused-function",
    "-Wno-unknown-pragmas",
    "-Wno-missing-braces",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-pointer-sign",
    "-Wno-format-truncation",
    "-Werror=incompatible-pointer-types",
    "-Werror=int-conversion",
    "-Werror=implicit-function-declaration",
]

# port/wiiu/src/wiiu_*.c: the console through wut, warnings on (on this
# big-endian ABI a pointer or integer mismatch is a real bug)
CONSOLE_FLAGS = [
    "-std=gnu11",
    "-Wall",
    "-Wno-unused-function",
    "-Werror=incompatible-pointer-types",
    "-Werror=int-conversion",
    "-Werror=implicit-function-declaration",
]

# the desktop platform layer's units the console does its own way
# (port/wiiu/src), or has no use for
REPLACED_PLATFORM_SOURCES = {
    # the window, the input and the sound device (wiiu_platform.c,
    # wiiu_input.c; dsound_sdl.c's mixer stays, over wiiu_audio.c)
    "sdl_platform.c",
    "xinput_sdl.c",
    "touch_input.c",
    "touch_menu.c",
    # page protection and its fault handler: Cafe OS gives an application
    # neither (HALO_CONSOLE_NO_WRITE_WATCH: platform/wiiu_memory_watch.c)
    "memory_watch.c",
    # the sockets, the network's address and random numbers (wiiu_net.c)
    "posix_net.c",
    # the desktop crash reports: the console's go to the log
    "posix_crash.c",
    # the self-updater: a console is updated with its files
    "posix_update.c",
    "updater.c",
    "update_signature.c",
    # UPnP and the desktop game browser's HTTPS (the console's Online Games
    # come later)
    "posix_upnp.c",
    "posix_browser.c",
}


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return shlex.quote(text)


def _devkitppc_dir(sln: Any) -> Optional[Path]:
    candidates = [
        getattr(sln.args, "devkitppc", None),
        os.environ.get("DEVKITPPC"),
        Path(os.environ["DEVKITPRO"]) / "devkitPPC" if "DEVKITPRO" in os.environ else None,
        Path("/opt/devkitpro/devkitPPC"),
    ]
    for c in candidates:
        if c:
            p = Path(c).expanduser().resolve()
            if (p / "bin" / "powerpc-eabi-gcc").is_file():
                return p
    return None


def _wut_dir(sln: Any) -> Optional[Path]:
    candidates = [
        getattr(sln.args, "wut", None),
        Path(os.environ["DEVKITPRO"]) / "wut" if "DEVKITPRO" in os.environ else None,
        Path("/opt/devkitpro/wut"),
    ]
    for c in candidates:
        if c:
            p = Path(c).expanduser().resolve()
            if (p / "include").is_dir() and (p / "share" / "wut.specs").is_file():
                return p
    return None


def _devkitpro_tool(devkitpro: Path, name: str) -> Optional[Path]:
    for candidate in (devkitpro / "tools" / "bin" / name, Path("/opt/devkitpro/tools/bin") / name):
        if candidate.is_file():
            return candidate
    return None


def add_arguments(parser: Any) -> None:
    """the port's own configure.py options"""
    parser.add_argument(
        "--devkitppc",
        metavar="DIR",
        type=Path,
        help="devkitPPC directory for `ninja wiiu` (default: DEVKITPPC or /opt/devkitpro/devkitPPC)",
    )
    parser.add_argument(
        "--wut",
        metavar="DIR",
        type=Path,
        help="wut directory for `ninja wiiu` (default: /opt/devkitpro/wut)",
    )


def configure_inputs(port: ConsolePort) -> List[Path]:
    """Files whose change must re-run configure.py."""
    return [port.path("src"), port.path("platform"), port.path("include"), port.path("build.py"),
            port.game_path(LINUX_SRC), port.game_path(LINUX_GAME), port.game_path(CONSOLE_GAME), port.game_path("port", "linux", "port.json")]


def _missing(n: Writer, text: str) -> None:
    """`ninja wiiu` says what is missing rather than not existing"""
    n.comment("Nintendo Wii U build (ninja wiiu): not available")
    n.rule(name="wiiu_missing", command=f"echo {shlex.quote('ninja wiiu needs: ' + text)} && exit 1",
           description="WIIU not configured")
    n.build(outputs="wiiu", rule="wiiu_missing")
    n.newline()


def _sdl_include(port: ConsolePort) -> Optional[Path]:
    """SDL's headers, as the dedicated server's build fetches them (once, into
    the game's build/server/third_party), as build.ninja names them"""
    from tools import console_build, server_build
    cwd = Path.cwd()
    os.chdir(console_build.GAME_ROOT)
    try:
        if not server_build.fetch_sdl_headers():
            return None
        include = (console_build.GAME_ROOT / server_build.SDL_INCLUDE).resolve()
    finally:
        os.chdir(cwd)
    return Path(os.path.relpath(include, cwd))


def generate(n: Writer, sln: Any, port: ConsolePort) -> None:
    devkitppc = _devkitppc_dir(sln)
    wut = _wut_dir(sln)
    if not devkitppc or not wut:
        _missing(n, "devkitPPC and wut (--devkitppc DIR, --wut DIR; port/wiiu/README.md)")
        return
    devkitpro = devkitppc.parent
    elf2rpl = _devkitpro_tool(devkitpro, "elf2rpl")
    if not elf2rpl:
        _missing(n, "devkitPro's wut-tools (elf2rpl)")
        return
    sdl_include = _sdl_include(port)
    if not sdl_include:
        _missing(n, "SDL's headers (downloaded once into the game's build/server/third_party)")
        return

    game = port.game
    port_dir = port.root
    gcc = devkitppc / "bin" / "powerpc-eabi-gcc"
    build_dir: Path = sln.build_dir / "wiiu"
    obj_dir = build_dir / "obj"
    gen_dir = build_dir / "gen"
    elf = build_dir / "halo.elf"
    rpx = build_dir / "halo.rpx"
    prefix_header = port_dir / "include" / "halo_wiiu_prefix.h"
    semantics_header = gen_dir / "halo_msvc_semantics.h"

    n.comment("Nintendo Wii U build (ninja wiiu)")
    env_prefix = f"DEVKITPRO={_quote(devkitpro)} DEVKITPPC={_quote(devkitppc)} "
    n.variable("wiiu_cc", _quote(gcc))
    n.rule(
        name="wiiu_cc",
        command=f"{env_prefix}$wiiu_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="WIIU CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="wiiu_elf",
        command=f"{env_prefix}$wiiu_cc $ldflags -o $out @$out.rsp $libs",
        description="WIIU LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.rule(
        name="wiiu_rpx",
        command=f"{_quote(elf2rpl)} $in $out",
        description="WIIU RPX $out",
    )
    n.rule(
        name="wiiu_msvc_semantics",
        command=f"$python {_quote(game / 'tools' / 'linux_msvc_semantics.py')} --output $out $scan",
        description="WIIU MSVC SEMANTICS $out",
        restat=True,
    )
    n.rule(
        name="wiiu_embed_assets",
        command=f"$python {_quote(game / 'tools' / 'embed_assets.py')} $out",
        description="WIIU EMBED $out",
    )

    # The game's file-scope struct tags (the generated header's first half;
    # its second, the inline functions' `#pragma weak`, is for clang's static
    # inlines: here the link keeps one copy of each, halo_wiiu_prefix.h)
    game_headers = port.game_headers()
    n.build(
        outputs=semantics_header,
        rule="wiiu_msvc_semantics",
        implicit=[game / "tools" / "linux_msvc_semantics.py", *game_headers],
        variables={"scan": f"--tags {_quote(game / 'source')}"},
    )
    # the high-res HUD's textures (the game's port/assets/hud)
    hud_assets = gen_dir / "hud_hires_assets.c"
    n.build(outputs=hud_assets, rule="wiiu_embed_assets", implicit=[game / "tools" / "embed_assets.py"])

    objects: List[Path] = []
    # the game's file names as the game spells them ("source/game/game.c"),
    # as the other console builds give them: the log's and the asserts' names
    # stay the desktop builds'
    file_names = f"-fmacro-prefix-map={_quote(game.as_posix() + '/')}=" if game != Path(".") else ""

    # (the SDK declarations are read as system headers, which the depfiles
    # leave out: every unit waits for them)
    xdk_headers = sorted((game / XDK_INCLUDE).glob("*.h"))

    def add_object(source: Path, cflags: str, implicit: List[Path] = ()) -> None:
        obj = (obj_dir / source.relative_to(build_dir) if build_dir in source.parents
               else port.output_path(obj_dir, source)).with_suffix(".o")
        objects.append(obj)
        n.build(outputs=obj, rule="wiiu_cc", inputs=source,
                implicit=[prefix_header, *xdk_headers, *implicit],
                variables={"cflags": f"{cflags} {file_names}".rstrip()})

    defines = console_defines(sln)
    target = " ".join(TARGET_FLAGS)
    abi = " ".join([target, *ABI_FLAGS, *defines])
    config = port.load_port_config()
    sdk_flags = f"-idirafter {_quote(game / XDK_INCLUDE)}"
    version_define = f"'-DHALO_VERSION=\"{version()}\"'"

    game_cflags = " ".join([
        abi,
        " ".join(GAME_FLAGS),
        f"-include {_quote(prefix_header)}",
        f"-include {_quote(semantics_header)}",
        f"-iquote {_quote(game)}",
        f"-I{_quote(port_dir / 'include')}",
        f"-I{_quote(game / LINUX_INCLUDE)}",
        f"-I{_quote(game / CONSOLE_INCLUDE)}",
        # the headers of the port's own game units, for the game sources that
        # call them
        f"-iquote {_quote(game / LINUX_GAME)}",
        port.game_defines_and_includes(config, _quote),
        sdk_flags,
    ])
    for source, flags in port.game_units(sln, config):
        add_object(source, " ".join([game_cflags, *flags]), [semantics_header])
    for source in sorted((game / LINUX_GAME).glob("*.c")):
        add_object(source, game_cflags, [semantics_header])
    # a big-endian console's byte swap of the maps' headers and tags
    # (port/console/game, which source/cache/cache_files.c calls with
    # HALO_BIG_ENDIAN)
    for source in sorted((game / CONSOLE_GAME).glob("*.c")):
        add_object(source, f"{game_cflags} -I{_quote(game / CONSOLE_INCLUDE)}", [semantics_header])

    platform_cflags = " ".join([
        abi,
        " ".join(PLATFORM_FLAGS),
        version_define,
        f"-include {_quote(prefix_header)}",
        f"-iquote {_quote(game)}",
        f"-I{_quote(game / LINUX_SRC)}",
        f"-I{_quote(port_dir / 'include')}",
        f"-I{_quote(game / LINUX_INCLUDE)}",
        f"-I{_quote(game / TOML_DIR)}",
        f"-I{_quote(game / EXPAT_DIR)}",
        f"-I{_quote(game / KCP_DIR)}",
        f"-I{_quote(game / QRCODEGEN_DIR)}",
        f"-I{_quote(game / MONOCYPHER_DIR)}",
        f"-I{_quote(game / ZLIB_DIR)}",
        f"-I{_quote(game / STB_DIR)}",
        f"-I{_quote(game / 'source')} -I{_quote(game / 'source' / 'cseries')}",
        sdk_flags,
        f"-isystem {_quote(sdl_include)}",
        f"-idirafter {_quote(wut / 'include')}",
        # (the POSIX the C library leaves out: sys/mman.h, ifaddrs.h)
        f"-idirafter {_quote(port_dir / 'include' / 'posix')}",
    ])
    # (posix_*.c talk to the C library alone: the host's ABI, without the
    # game's headers or the SDK declarations, as on Linux)
    posix_cflags = " ".join([
        target,
        "-fsigned-char",
        "-ffp-contract=off",
        *defines,
        "-std=gnu11",
        "-D_GNU_SOURCE",
        "-Wall",
        "-Wno-unused-function",
        version_define,
        f"-I{_quote(game / LINUX_SRC)}",
        f"-idirafter {_quote(port_dir / 'include' / 'posix')}",
        f"-include {_quote(port_dir / 'include' / 'posix' / 'wiiu_posix.h')}",
        f"-I{_quote(game / STB_DIR)}",
        f"-I{_quote(wut / 'include')}",
    ])
    for source in sorted((game / LINUX_SRC).glob("*.c")):
        if source.name in REPLACED_PLATFORM_SOURCES:
            continue
        add_object(source, posix_cflags if source.name.startswith("posix_") else platform_cflags)
    add_object(hud_assets, platform_cflags)
    # the port's units in the platform layer's terms (the Xbox SDK's
    # declarations): its window, input and the rest of sdl_platform.h
    for source in sorted((port_dir / "platform").glob("*.c")):
        add_object(source, f"{platform_cflags} -I{_quote(port_dir / 'src')}")

    # the port's own units: the console, through wut
    console_cflags = " ".join([
        target,
        *defines,
        " ".join(CONSOLE_FLAGS),
        version_define,
        f"-iquote {_quote(game)}",
        f"-I{_quote(port_dir / 'include')}",
        f"-I{_quote(game / LINUX_SRC)}",
        f"-I{_quote(game / CONSOLE_INCLUDE)}",
        f"-I{_quote(game / MONOCYPHER_DIR)}",
        f"-I{_quote(port_dir / 'include' / 'posix')}",
        f"-isystem {_quote(sdl_include)}",
        f"-I{_quote(wut / 'include')}",
    ])
    for source in sorted((port_dir / "src").glob("*.c")):
        add_object(source, console_cflags)

    # the third-party libraries, with the platform layer's ABI
    third_party = " ".join([abi, "-std=gnu11", "-w"])
    add_object(game / TOML_DIR / "tomlc17.c", third_party)
    for name in EXPAT_SOURCES:
        add_object(game / EXPAT_DIR / name, f"{third_party} -I{_quote(game / EXPAT_DIR)}")
    add_object(game / KCP_DIR / "ikcp.c", third_party)
    add_object(game / QRCODEGEN_DIR / "qrcodegen.c", third_party)
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        add_object(game / MONOCYPHER_DIR / name, third_party)
    for name in ZLIB_SOURCES:
        add_object(game / ZLIB_DIR / name, " ".join([third_party, *ZLIB_DEFINES]))
    # the game's sin, pow and the rest, the same on every port
    musl_math = " ".join([third_party, f"-I{_quote(game / MUSL_MATH_DIR / 'include')}",
                          f"-include {_quote(game / MUSL_MATH_DIR / 'include' / 'libm.h')}"])
    for source in sorted((game / MUSL_MATH_DIR / "src").glob("*.c")):
        add_object(source, musl_math)

    ldflags = " ".join([*TARGET_FLAGS[:4], f"-specs={_quote(wut / 'share' / 'wut.specs')}", f"-L{_quote(wut / 'lib')}",
                        f"-Wl,-Map,{_quote(build_dir / 'halo.map')}",
                        # one copy of each inline function every unit emits
                        # (MSVC's COMDATs: halo_wiiu_prefix.h)
                        "-Wl,--allow-multiple-definition"])
    n.build(
        outputs=elf,
        rule="wiiu_elf",
        inputs=objects,
        implicit_outputs=[build_dir / "halo.map"],
        variables={"ldflags": ldflags, "libs": "-lwut -lm"},
    )
    n.build(outputs=rpx, rule="wiiu_rpx", inputs=elf)
    n.build(outputs="wiiu", rule="phony", inputs=rpx)
    n.newline()
