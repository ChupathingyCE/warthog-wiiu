#!/bin/bash
# A smoke test of a Wii U build in Cemu: boot to the main menu, then boot
# again straight into a map, run it for a while, and keep the logs.
#
#   tools/cemu_smoke.sh --cemu ~/emulators/cemu-2.6/Cemu.app --package package --out smoke
#
# --cemu      Cemu 2.6 or later from https://github.com/cemu-project/Cemu/releases:
#             the macOS Cemu.app, the Linux AppImage or the Cemu binary. The
#             test runs it in portable mode: its settings and SD card are a
#             "portable" folder next to it, made and emptied by this script
#             (and only one this script made), never the Cemu you play with.
# --package   the card's folder (tools/wiiu_package.py --out DIR), holding
#             wiiu/apps/halo.wuhb (and wiiu/apps/halo/halo.rpx, copied
#             too when it is there; Cemu starts the .wuhb)
# --out       where the logs go (one folder a phase), and the scratch data
# --maps-root the script that makes a data root of links to your own maps
#             (default ~/halo-maps/new-data-root.sh); the maps are linked,
#             never copied, and never written
# --map       the map the second boot loads (default
#             levels\test\bloodgulch\bloodgulch)
# --seconds   how long the map runs once it is loaded (default 60)
# --timeout   the longest a phase may take to reach its mark (default 900)
# --renderer  Cemu's graphics API: vulkan (the default) or opengl
# --display   an X display to run on (Linux); without DISPLAY, the test
#             starts its own Xvfb
# --shots-every a screenshot of the display every N seconds while a phase
#             runs, and one more at its end, in the phase's shots folder
#             (default 15; 0 for none). Needs an X display and ImageMagick's
#             import, so none on macOS
#
# The first boot passes when the game starts the main menu's music and draws
# on for 10 s; the second when the map has loaded (its scripts read) and the
# game has drawn it for --seconds, from its first frame after its shaders,
# without a halt. Either fails on the game's EXCEPTION halt, on Cemu
# leaving, on the time running out, on no frames drawn, or on a shader that
# did not compile ("gl: a ... shader did not compile"; the game keeps its
# source as failedNN.vert or .frag, kept with the phase's logs). Each
# phase's result.txt has the reasons, and summary.txt in --out has both
# phases on a line each. Cemu runs with its interpreter
# (--force-interpreter): its recompiler misreads a compare across a call
# in the menus' tags (port/wiiu/STATUS.md, milestone 2).
#
# The second boot loads its map with the game's own d:\init.txt in the
# game's folder of the test's SD card. It is the test's: the player's card
# is never touched. No game files are in this repository or in the logs.
#
# A pass here is a pass in Cemu, not on a Wii U.
set -euo pipefail

cemu=
package=
out=
maps_root="$HOME/halo-maps/new-data-root.sh"
map='levels\test\bloodgulch\bloodgulch'
seconds=60
timeout=900
renderer=vulkan
display=${DISPLAY:-}
shots_every=15

while [ $# -gt 0 ]; do
	case "$1" in
		--cemu) cemu=$2; shift 2 ;;
		--package) package=$2; shift 2 ;;
		--out) out=$2; shift 2 ;;
		--maps-root) maps_root=$2; shift 2 ;;
		--map) map=$2; shift 2 ;;
		--seconds) seconds=$2; shift 2 ;;
		--timeout) timeout=$2; shift 2 ;;
		--renderer) renderer=$2; shift 2 ;;
		--display) display=$2; shift 2 ;;
		--shots-every) shots_every=$2; shift 2 ;;
		*) echo "$0: unknown argument $1" >&2; exit 2 ;;
	esac
done
if [ -z "$cemu" ] || [ -z "$package" ] || [ -z "$out" ]; then
	echo "usage: $0 --cemu PATH --package DIR --out DIR [--maps-root SCRIPT] [--map NAME] [--seconds N] [--timeout N]" >&2
	exit 2
fi

case "$renderer" in
	vulkan) api=1 ;;
	opengl) api=0 ;;
	*) echo "$0: --renderer is vulkan or opengl" >&2; exit 2 ;;
esac

absolute() { (cd "$(dirname "$1")" && printf '%s/%s\n' "$(pwd -P)" "$(basename "$1")"); }
cemu=$(absolute "$cemu")
package=$(absolute "$package")
mkdir -p "$out"
out=$(absolute "$out")

# Cemu's binary and its portable folder (Cemu's CemuApp::DeterminePaths: next
# to the .app on macOS, next to the AppImage or the binary on Linux)
case "$cemu" in
	*.app) binary="$cemu/Contents/MacOS/Cemu"; portable="$(dirname "$cemu")/portable" ;;
	*) binary="$cemu"; portable="$(dirname "$cemu")/portable" ;;
esac
if [ ! -x "$binary" ]; then
	echo "$0: no Cemu at $binary" >&2
	exit 2
fi
if [ ! -f "$package/wiiu/apps/halo.wuhb" ]; then
	echo "$0: $package has no wiiu/apps/halo.wuhb (tools/wiiu_package.py --out DIR)" >&2
	exit 2
fi
if [ ! -x "$maps_root" ]; then
	echo "$0: no data root script at $maps_root" >&2
	exit 2
fi
if [ -e "$portable" ] && [ ! -f "$portable/.cemu-smoke" ]; then
	echo "$0: $portable is not this test's (no .cemu-smoke in it); move it or use another Cemu" >&2
	exit 2
fi

# a fresh portable folder: settings without the first start's dialogs
rm -rf "$portable"
mkdir -p "$portable"
touch "$portable/.cemu-smoke"
cat > "$portable/settings.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<content>
    <logflag>2305843009214087172</logflag>
    <use_discord_presence>false</use_discord_presence>
    <check_update>false</check_update>
    <vk_warning>true</vk_warning>
    <gp_download>false</gp_download>
    <macos_disclaimer>true</macos_disclaimer>
    <play_boot_sound>false</play_boot_sound>
    <window_size><x>640</x><y>400</y></window_size>
    <Graphic>
        <api>$api</api>
        <VSync>0</VSync>
    </Graphic>
    <Audio>
        <TVVolume>0</TVVolume>
    </Audio>
</content>
EOF

# the test's SD card: the package, and the maps as links. Each map file is
# linked on its own, because Cemu 2.6 stops (an uncaught filesystem_error)
# when a folder the game lists holds a link that leads nowhere, as a shared
# maps folder can.
card="$portable/sdcard"
game="$card/wiiu/apps/halo"
mkdir -p "$game/maps"
cp "$package/wiiu/apps/halo.wuhb" "$card/wiiu/apps/halo.wuhb"
if [ -f "$package/wiiu/apps/halo/halo.rpx" ]; then
	cp "$package/wiiu/apps/halo/halo.rpx" "$game/halo.rpx"
fi
data="$out/data"
"$maps_root" "$data" > /dev/null
linked=0
for file in "$data"/maps/*.map; do
	if [ -f "$file" ]; then
		ln -s "$(cd "$(dirname "$file")" && pwd -P)/$(basename "$file")" "$game/maps/$(basename "$file")"
		linked=$((linked + 1))
	fi
done
if [ ! -f "$game/maps/ui.map" ]; then
	echo "$0: no ui.map in $data/maps" >&2
	exit 2
fi
echo "maps: $linked linked from $data/maps"

# an X display for Cemu on Linux: the one given, or an Xvfb of the test's
xvfb_pid=
if [ "$(uname)" != Darwin ] && [ -z "$display" ]; then
	command -v Xvfb > /dev/null || { echo "$0: no DISPLAY and no Xvfb" >&2; exit 2; }
	number=290
	while [ -e "/tmp/.X$number-lock" ]; do number=$((number + 1)); done
	display=":$number"
	Xvfb "$display" -screen 0 1280x720x24 -nolisten tcp > /dev/null 2>&1 &
	xvfb_pid=$!
	sleep 2
fi
if [ -n "$display" ]; then
	export DISPLAY=$display
fi
if [ "$shots_every" != 0 ] && { [ "$(uname)" = Darwin ] || ! command -v import > /dev/null; }; then
	echo "$0: no screenshots here (they need an X display and ImageMagick's import)" >&2
	shots_every=0
fi

# a screenshot of the display (a phase's shots folder, the picture's name)
screenshot() {
	[ "$shots_every" != 0 ] || return 0
	mkdir -p "$1"
	import -display "$display" -window root "$1/$2.png" 2> /dev/null || true
}

cemu_pid=
stop_cemu() {
	if [ -n "$cemu_pid" ] && kill -0 "$cemu_pid" 2> /dev/null; then
		kill "$cemu_pid" 2> /dev/null || true
		for _ in 1 2 3 4 5 6 7 8 9 10; do
			kill -0 "$cemu_pid" 2> /dev/null || break
			sleep 1
		done
		kill -9 "$cemu_pid" 2> /dev/null || true
	fi
	cemu_pid=
}
finish() {
	stop_cemu
	if [ -n "$xvfb_pid" ]; then
		kill "$xvfb_pid" 2> /dev/null || true
	fi
}
trap finish EXIT

# the port's newest frame number in Cemu's log (its "gl: frame N" reports), 0
# for none yet
frame_now() {
	grep -h -o "gl: frame [0-9]*" "$portable/log.txt" 2> /dev/null | tail -1 | sed 's/[^0-9]//g' | grep . || echo 0
}

# the draws in the frame of that newest report (a loading screen has a few,
# a map's view a hundred or more)
draws_now() {
	grep -h "gl: frame [0-9]*" "$portable/log.txt" 2> /dev/null | tail -1 | grep -o "[0-9]* draws in the last" |
		sed 's/[^0-9]//g' | grep . || echo 0
}

# the mark in the logs, after the line that must come first when there is one
marked_in() {
	local after=$1 mark=$2
	shift 2
	if [ -z "$after" ]; then
		grep -q -h -F -- "$mark" "$@"
	else
		awk -v after="$after" -v mark="$mark" \
			'index($0, after) { seen = 1 } seen && index($0, mark) { found = 1 } END { exit !found }' "$@"
	fi
}

# a phase: boot, wait for its mark in debug.txt, then hold, and keep the logs
# (name, init.txt's text or empty, the line that must come first or empty,
# the mark, the seconds to hold after it)
phase() {
	local name=$1 init=$2 after=$3 mark=$4 hold=$5
	local logs="$out/$name" started now result=fail reason="" marked=0 drawing=0 logs_found mark_frame=0 first_frame=0 last_frame=0
	local shot=0 last_shot shader_failures
	mkdir -p "$logs"
	rm -rf "$game/logs" "$game/z" "$logs/shots"
	rm -f "$game"/failed[0-9][0-9].vert "$game"/failed[0-9][0-9].frag
	rm -f "$game/port.txt" "$game/init.txt" "$portable/log.txt"
	if [ -n "$init" ]; then
		printf '%s\r\n' "$init" > "$game/init.txt"
	fi
	echo "$name: starting Cemu"
	"$binary" --force-interpreter -g "$card/wiiu/apps/halo.wuhb" > "$logs/cemu.out" 2>&1 &
	cemu_pid=$!
	started=$(date +%s)
	last_shot=$started
	while :; do
		sleep 5
		now=$(date +%s)
		if ! kill -0 "$cemu_pid" 2> /dev/null; then
			reason="Cemu left"
			break
		fi
		if [ "$shots_every" != 0 ] && [ $((now - last_shot)) -ge "$shots_every" ]; then
			shot=$((shot + 1))
			screenshot "$logs/shots" "$(printf '%03d' "$shot")-$((now - started))s"
			last_shot=$now
		fi
		# (the game's debug.txt on the card, and Cemu's log, which has the
		# port's console lines, debug.txt's among them, as they are written)
		logs_found=$(ls "$game/logs/debug.txt" "$portable/log.txt" 2> /dev/null || true)
		if [ -n "$logs_found" ] && grep -q -h "EXCEPTION halt" $logs_found; then
			reason="the game halted: $(grep -h -m1 "EXCEPTION halt" $logs_found | head -1 | cut -c1-300)"
			break
		fi
		if [ "$marked" = 0 ]; then
			if [ -f "$portable/log.txt" ] && marked_in "$after" "$mark" "$portable/log.txt"; then
				marked=$now
				mark_frame=$(frame_now)
				echo "$name: reached \"$mark\" after $((now - started)) s (frame $mark_frame)"
			elif [ $((now - started)) -ge "$timeout" ]; then
				reason="no \"$mark\" in $timeout s"
				break
			fi
		elif [ "$drawing" = 0 ]; then
			# (the first frames of a map wait for its shaders, compiled once
			# and kept on the card: the time held starts with the first frame
			# of the view itself, past the loading screen's few draws)
			if [ "$(frame_now)" -gt "$mark_frame" ] && [ "$(draws_now)" -ge 20 ]; then
				drawing=$now
				first_frame=$(frame_now)
				echo "$name: drawing (frame $first_frame, $(draws_now) draws) after $((now - started)) s; running $hold s more"
			elif [ $((now - started)) -ge "$timeout" ]; then
				reason="no frame drawn after \"$mark\" in $timeout s (frame $mark_frame)"
				break
			fi
		elif [ $((now - drawing)) -ge "$hold" ]; then
			# (and the game kept drawing, not only stood there)
			last_frame=$(frame_now)
			if [ "$last_frame" -gt "$first_frame" ]; then
				result=pass
			else
				reason="no frame drawn in the $hold s after frame $first_frame"
			fi
			break
		fi
	done
	if kill -0 "$cemu_pid" 2> /dev/null; then
		screenshot "$logs/shots" last
	fi
	stop_cemu
	cp "$game/logs/debug.txt" "$logs/" 2> /dev/null || true
	cp "$game/port.txt" "$game/config.toml" "$logs/" 2> /dev/null || true
	cp "$portable/log.txt" "$logs/cemu-log.txt" 2> /dev/null || true
	# (the port's last frame report, to show the game was drawing)
	grep -h "gl: frame" "$logs/cemu-log.txt" 2> /dev/null | tail -1 | sed 's/.*gl: /  last frame report: /' | cut -c1-200 > "$logs/frames.txt" || true
	# (shaders that did not compile: the game's line, and its copy of the
	# source in the game's folder)
	mv "$game"/failed[0-9][0-9].vert "$game"/failed[0-9][0-9].frag "$logs/" 2> /dev/null || true
	shader_failures=$(grep -c "gl: a [a-z]* shader did not compile" "$logs/cemu-log.txt" 2> /dev/null || true)
	shader_failures=${shader_failures:-0}
	if [ "$result" = pass ] && [ "$shader_failures" -gt 0 ]; then
		result=fail
		reason="shader failure: $shader_failures shaders did not compile (the first: $(grep -m1 \
			"gl: a [a-z]* shader did not compile" "$logs/cemu-log.txt" | sed 's/.*gl: /gl: /' | cut -c1-200))"
	fi
	if [ "$result" = pass ]; then
		echo "$name: PASS ($(( $(date +%s) - started )) s; frames $first_frame to $last_frame in the $hold s)" | tee "$logs/result.txt"
		cat "$logs/frames.txt" | tee -a "$logs/result.txt"
	else
		echo "$name: FAIL: $reason" | tee "$logs/result.txt"
	fi
	[ "$result" = pass ]
}

status=0
rm -f "$out/summary.txt"
phase menu "" "" "starting main menu music" 10 || status=1
head -1 "$out/menu/result.txt" >> "$out/summary.txt"
# (the map: the game's init.txt; it is in once its scripts are read, which
# follows its tags and its structure bsp)
phase map "map_name $map" "starting precaching of map '${map##*\\}'" "scenario scripts:" "$seconds" || status=1
head -1 "$out/map/result.txt" >> "$out/summary.txt"
rm -rf "$data"
if [ "$status" = 0 ]; then
	echo "cemu smoke: PASS (Cemu only; not a Wii U)"
else
	echo "cemu smoke: FAIL (logs in $out)"
fi
exit $status
