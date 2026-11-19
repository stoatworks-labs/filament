#!/usr/bin/env bash
#
# Build demo/filament-organ.{js,wasm}: the colour organ's CPU half, the
# plugin's own C++, compiled to WebAssembly for the browser demo. (flyback's
# script, which is woofer's and mirrorball's, cut down to the files the organ
# needs: the rest of filament's page is the plugin's shaders in WebGL2 and a
# JavaScript port of the rest of its CPU half.)
#
#     demo/tools/build-wasm.sh          (needs emscripten: em++ on PATH, or EMXX=...)
#
# The output is COMMITTED, because the Cloudflare deploy has no build step:
# what is in demo/ is what is served. So is demo/wasm/inputs.sha256, the hash
# of every file that went in (the compiler's own dependency list, so every
# header the sources include, and nothing they do not) and of both outputs;
# demo/tools/check_shaders.py (run by tools/verify.sh) fails when an input has
# changed since this last ran, which is the only way a stale .wasm can be
# noticed without emscripten. Rerun it after source/Organ.*, source/Diag.*,
# source/GelTable.h, source/Lamp.h, demo/wasm/glue.cpp or anything under
# external/earshot/ changes (a new snapshot included).
#
# What goes in, and none of it is edited for the browser:
#
#   source/   Organ.cpp -- organ::Organ: the FIFO onto the host's clock, the
#             DC block, the LR2 crossover, ETI 428's detectors, the 1/16
#             half-cycle grid and the control rows, the lamp's state, the
#             menu, and the control laws (CrossoverLowHertz and the rest) --
#             and Diag.cpp, the log it writes to (never opened here: nothing
#             calls diag::init, so every line is dropped). Its headers:
#             Organ.h, GelTable.h (the gel menu's names), Diag.h, and Lamp.h
#             (only for lamp::kPi, which the harness's track uses). Nothing
#             else from source/: Filament.cpp, the controls and the passes are
#             the page's port and the page's WebGL2.
#   external/earshot   the snapshot at 8124191, unpatched, built as its CMake
#             builds it with EARSHOT_CAPTURE=OFF: src/NoCapture.cpp in place of
#             miniaudio (Miniaudio.cpp and MiniaudioImpl.cpp are NOT compiled,
#             and miniaudio.h is never included), so this module has no device
#             backend at all. listDevices() is empty and selectDevice() of a
#             name reports Failed -- and the organ never calls either, under
#             FILAMENT_NO_DEVICES=1 (glue.cpp's fo_prepare). Audio reaches it
#             only through earshot's feed(), as fitest's does.
#   demo/wasm glue.cpp: the plugin's calls (Activate, Deactivate, Advance with
#             its Settings) and the harness's feeder (tools/fitest/Organ.inc's
#             track and Audio, verbatim) as C functions.
#
# Flags follow CMakeLists.txt's Release build: -O3 -DNDEBUG, C++17. No
# pthreads: earshot's device worker would be a thread, and it is only ever
# started by selectDevice() with a name, which nothing here calls. WebAssembly
# has no fused multiply-add, so where clang fuses one on arm64 the browser
# rounds twice, and its sin, cos, exp, log and pow are musl's rather than
# Apple's libm: the organ's double arithmetic need not agree with the Mac's bit
# for bit (AGENTS.md, "The browser demo", says what was compared).
# SOURCE_DATE_EPOCH=0 pins __DATE__/__TIME__ (Diag.cpp names its build), so
# the same inputs build the same bytes.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO"

EMXX="${EMXX:-em++}"
command -v "$EMXX" >/dev/null 2>&1 || { echo "em++ not found (install emscripten, or set EMXX=)" >&2; exit 1; }
[[ -f external/earshot/CMakeLists.txt ]] || { echo "external/earshot missing" >&2; exit 1; }

ES=external/earshot
PLUGIN_SOURCES="Organ Diag"
# earshot's CMakeLists.txt's own list, with EARSHOT_CAPTURE=OFF's NoCapture.cpp.
EARSHOT_SOURCES="Capture Engine Fft HostFft Onsets Partials Pitch Stereo Tap Tempo ThirdOctave NoCapture"

# Every earshot source but the two miniaudio files, so a new one in a new
# snapshot cannot be left out quietly.
for file in "$ES"/src/*.cpp; do
	name="$( basename "$file" .cpp )"
	[[ "$name" == Miniaudio || "$name" == MiniaudioImpl || " $EARSHOT_SOURCES " == *" $name "* ]] \
		|| { echo "$ES/src/$name.cpp is not in EARSHOT_SOURCES -- add it, or say why it is left out" >&2; exit 1; }
done

OBJ="$( mktemp -d )"
trap 'rm -rf "$OBJ"' EXIT

export SOURCE_DATE_EPOCH=0
BASE=( -std=c++17 -O3 -DNDEBUG -Isource -I"$ES/include" )

objects=()
compile() {
	local src="$1"; shift
	local stem="$OBJ/$( echo "$src" | tr '/' '_' )"
	"$EMXX" "${BASE[@]}" "$@" -MMD -MF "$stem.d" -c "$src" -o "$stem.o"
	objects+=( "$stem.o" )
}

for name in $PLUGIN_SOURCES; do
	compile "source/$name.cpp"
done
for name in $EARSHOT_SOURCES; do
	compile "$ES/src/$name.cpp" -I"$ES/src"
done
compile demo/wasm/glue.cpp

LINK=(
	-O3
	-sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createFilamentOrgan
	-sENVIRONMENT=web
	-sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
	-sDYNAMIC_EXECUTION=0
	-sFILESYSTEM=0
	-sEXPORTED_RUNTIME_METHODS=HEAPF32,HEAPF64,HEAP32
	-sEXPORTED_FUNCTIONS=_malloc,_free
)
"$EMXX" "${objects[@]}" "${LINK[@]}" -o demo/filament-organ.js

# Served as static files and committed: no executable bit (pinhole's trap;
# the linker writes the .wasm 755).
chmod 644 demo/filament-organ.js demo/filament-organ.wasm

#---------------------------------------------------------------------------
# The manifest check_shaders.py reads. Inputs first -- every file the compiler
# says it read, from its own dependency files -- then outputs.
#---------------------------------------------------------------------------
inputs="$(
	sed -e 's/^[^:]*://' -e 's/\\$//' "$OBJ"/*.d | tr ' ' '\n' | sed '/^$/d' | sort -u
)"
{
	echo "# demo/tools/build-wasm.sh, $( "$EMXX" --version | sed -n 1p )"
	while IFS= read -r path; do
		[[ "$path" == /* ]] && { echo "an input outside the repository: $path" >&2; exit 1; }
		shasum -a 256 "$path"
	done <<< "$inputs"
	shasum -a 256 demo/tools/build-wasm.sh
	shasum -a 256 demo/filament-organ.js demo/filament-organ.wasm
} > demo/wasm/inputs.sha256

ls -l demo/filament-organ.js demo/filament-organ.wasm | awk '{ print "wrote " $NF ", " $5 " bytes" }'
echo "$( grep -vc '^#' demo/wasm/inputs.sha256 ) files hashed in demo/wasm/inputs.sha256"
