#!/bin/sh
# The Wii U port's checks on the host, under AddressSanitizer and
# UndefinedBehaviorSanitizer where the compiler has them:
#
#   port/wiiu/tests/run.sh    which hosts the console joins (a Delta host
#                                by its wire and range, whatever OpenCE
#                                number it announces), and that the build
#                                and the game take the Delta path, and
#                                that the GL layer has every function the
#                                game's renderer loads, and that a vertex
#                                shader's nv2a_finite is rewritten or fails
#
# CC picks the compiler (default: cc). The binaries go to build/wiiu-tests.
# Run from this repository, with the game in chupathingyce/ (git submodule
# update --init; GAME names another checkout of it).
set -e
cd "$(dirname "$0")/../../.."
G=${GAME:-chupathingyce}
if [ ! -f "$G/port/linux/src/delta_compat.c" ]; then
	echo "$0: the game is not in $G (git submodule update --init, or GAME=DIR)" >&2
	exit 2
fi
CC=${CC:-cc}
OUT=build/wiiu-tests
mkdir -p "$OUT"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
echo 'int main(void) { return 0; }' > "$OUT/probe.c"
$CC $SAN "$OUT/probe.c" -o "$OUT/probe" 2>/dev/null || SAN=
# the console's defines (build.py, include/halo_wiiu_prefix.h)
CONSOLE="-DHALO_WIIU_CONSOLE -DHALO_CONSOLE -DHALO_BIG_ENDIAN -DHALO_CONSOLE_PLAYERS=16 -DHALO_CONSOLE_DELTA"
# which hosts the console joins (delta_compat.c as the console builds it)
$CC -std=c89 -pedantic -Wall -Wextra -Werror $SAN $CONSOLE \
	-I$G/port/linux/src -idirafter $G/port/linux/include port/wiiu/tests/delta_join_test.c \
	$G/port/linux/src/delta_compat.c -o "$OUT/delta_join_test"
"$OUT/delta_join_test"
# a vertex shader's nv2a_finite is given back as it is, from the game's own
# text of it (xgpu.h's NV2A_FINITE), and any other spelling fails the shader
# (wiiu_shader_finite.c; a game from before #985 has none to check)
if grep -q '^#define NV2A_FINITE \\$' "$G/port/linux/src/xgpu.h"; then
	awk '/^#define NV2A_FINITE \\$/ { on = 1 } on { print } on && !/\\$/ { exit }' \
		"$G/port/linux/src/xgpu.h" > "$OUT/nv2a_finite_text.h"
	$CC -std=c89 -pedantic -Wall -Wextra -Werror $SAN -Iport/wiiu/src -I"$OUT" \
		port/wiiu/tests/shader_finite_test.c port/wiiu/src/wiiu_shader_finite.c -o "$OUT/shader_finite_test"
	"$OUT/shader_finite_test"
else
	echo "$0: the game has no NV2A_FINITE in xgpu.h: the vertex shader rewrite is not checked"
fi
# every GL function the game's renderer loads is in the GL layer's table, or
# the game stops at gl_functions_load with a black screen (the game's
# tools/console_gl_check.py; a game from before it has none to run)
if [ -f "$G/tools/console_gl_check.py" ]; then
	python3 "$G/tools/console_gl_check.py" port/wiiu/src/wiiu_gl.c
else
	echo "$0: the game has no tools/console_gl_check.py yet: the GL table is not checked"
fi
# the build defines HALO_CONSOLE_DELTA, and the game's join check takes the
# Delta wire path on a console
grep -q '"-DHALO_CONSOLE_DELTA"' port/wiiu/build.py || {
	echo "$0: build.py does not build Delta Peer's client (HALO_CONSOLE_DELTA)" >&2
	exit 1
}
grep -q '^#if defined(HALO_GAME_BROWSER) || defined(HALO_CONSOLE_DELTA) || defined(HALO_CONSOLE_ONLINE_GAMES)$' \
	$G/source/networking/network_client_manager.c || {
	echo "$0: the game's join check does not take the Delta wire path on the console" >&2
	exit 1
}
# a build of 32 players (configure.py --console-players 32, the tester
# build) asks the console list for the games of up to 32
path=$(printf '#include "wiiu_game_list.h"\nGAME_LIST_PATH\n' |
	$CC -E -P -DHALO_CONSOLE_PLAYERS=32 -Iport/wiiu/include - | tail -1)
[ "$path" = '"/v1/console/games?coop=1&packs=1&players=" "32"' ] || {
	echo "$0: a 32-player build asks the list for $path" >&2
	exit 1
}
echo "$0: ok"
