#!/usr/bin/env bash
#
# Mutation testing of the colour organ: change ONE character of the shipped
# code and require that a check fails. A check that still passes against a
# mutant was not looking at the code it claims to cover -- and a harness that
# compiled its own copy of the shaders or of Organ.cpp would pass every mutant
# here.
#
# One copy of the tree in a temporary directory (the FFGL SDK and the earshot
# snapshot symlinked, not copied), configured once, arm64 only. Before each
# mutant every object of filament_core and fitest is deleted: make compares
# mtimes to the second, and a file mutated and restored inside one second
# would otherwise keep its mutant's object. Only the named check runs, at
# CI's raster, under a time limit (a mutant can hang: honeydew's lesson; a
# timeout counts as caught).
#
#     tools/mutate.sh            every mutant
#     tools/mutate.sh 4 9        only those (1-based)
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain
# one (conway's trap): no || anywhere below.
MUTANTS=(
	"source/Organ.cpp|f.b1 = ( 1.0 - c ) / a0;|f.b1 = ( 1.0 + c ) / a0;|--crossover|C++: the low-pass's b1 with the high-pass's sign, - -> +"
	"source/Organ.cpp|f.a2 = ( 1.0 - alpha ) / a0;|f.a2 = ( 1.0 + alpha ) / a0;|--crossover|C++: the biquad's a2 with alpha added, - -> +"
	"source/Organ.h|constexpr double kQ = 0.5;|constexpr double kQ = 0.6;|--crossover|C++: Linkwitz-Riley's Q 0.6, 5 -> 6"
	"source/Organ.cpp|leakKeep[ c ]         = std::exp( -dt / ( release * C ) );|leakKeep[ c ]         = std::exp( +dt / ( release * C ) );|--detector|C++: the detector's release growing, - -> +"
	"source/Organ.h|{ 1.0e-6, 0.47e-6, 0.47e-6 }|{ 1.0e-6, 0.47e-6, 0.57e-6 }|--detector|C++: the treble detector's C10 0.57 uF, 4 -> 5"
	"source/Organ.cpp|kPerturbPowerGain ) != 0 ? 10.0 : 20.0 )|kPerturbPowerGain ) != 0 ? 10.0 : 21.0 )|--detector|C++: the gain law 10^( dB / 21 ), 0 -> 1"
	"source/Organ.cpp|	if( !listed )|	if(  listed )|--organdevices|C++: the devices never listed on the first frame on, ! -> space"
	"source/Organ.cpp|if( engine && applied != 0.0f )|if( engine && applied == 0.0f )|--organdevices|C++: organ off leaves the device open, ! -> ="
	"source/Filament.cpp|organ::kSwitchCount ) == 1 && ensureOrgan()|organ::kSwitchCount ) != 1 && ensureOrgan()|--organdevices|C++: the organ runs with its switch Off, = -> !"
	"source/Filament.cpp|const int64_t firstHalf = 2 * static_cast|const int64_t firstHalf = 3 * static_cast|--organframes|C++: the rows numbered from the wrong half-cycle, 2 -> 3"
	"source/Shaders.cpp|return d - cosPi( lo + hi ) * sinPi( d ) / kPi;|return d + cosPi( lo + hi ) * sinPi( d ) / kPi;|--firing|GLSL: the conduction energy's sine term added, - -> +"
	"source/Shaders.cpp|	float z = y + 0.5;|	float z = y + 0.6;|--firing|GLSL: cosPi a tenth of a half-cycle off, 5 -> 6"
	"source/Shaders.cpp|u[ c ]     = ( float( j ) + t ) / 16.0;|u[ c ]     = ( float( j ) + t ) / 15.0;|--firing|GLSL: the firing point on a grid of 15, 6 -> 5"
	"source/Shaders.cpp|vec3 carried = ramp.a == Tag0 ? ramp.rgb : vec3( 0.0 );|vec3 carried = ramp.a != Tag0 ? ramp.rgb : vec3( 0.0 );|--organframes|GLSL: a ramp carried across the wrong frames, = -> !"
	"source/Shaders.cpp|float end0 = r1 > row ? 1.0 : w1 - h0;|float end0 = r1 < row ? 1.0 : w1 - h0;|--firing|GLSL: a substep's first part ending at the wrong place, > -> <"
	"source/Shaders.cpp|T[ c ]   = clamp( t + StepSeconds / 6.0 *|T[ c ]   = clamp( t + StepSeconds / 5.0 *|--organstep|GLSL: the organ's RK4 weights over 5, 6 -> 5"
	"source/Shaders.cpp|mod( Tag0 + float( rEnd ), 4194304.0 )|mod( Tag0 + float( rEnd ), 4194305.0 )|--organclock|GLSL: the ramp's tag wrapped at 2^22 + 1, 4 -> 5"
	"source/Shaders.cpp|return ( a.rgb + ( b.rgb - a.rgb ) * t )|return ( a.rgb + ( b.rgb + a.rgb ) * t )|--organcolour|GLSL: the gel row's interpolation, - -> +"
	"source/Shaders.cpp|gelEmission( T.g, gel.g )|gelEmission( T.g, gel.b )|--organcolour|GLSL: the mid filament behind the treble gel, g -> b"
	"source/Shaders.cpp|float r = length( gl_FragCoord.xy - LampCentre );|float r = length( gl_FragCoord.xy + LampCentre );|--organdevices|GLSL: the status lamp drawn off its corner, - -> +"
)

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

tree="$WORK/tree"
mkdir -p "$tree/external"
cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
ln -s "$REPO/external/ffgl" "$tree/external/ffgl"
ln -s "$REPO/external/earshot" "$tree/external/earshot"
cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
cmake --build "$tree/build" --target fitest -j8 >/dev/null 2>&1

selected=( "$@" )
caught=0
ran=0
index=0
for entry in "${MUTANTS[@]}"; do
	index=$(( index + 1 ))
	if [[ ${#selected[@]} -gt 0 ]] && [[ ! " ${selected[*]} " == *" $index "* ]]; then
		continue
	fi
	IFS='|' read -r file original mutant check what <<<"$entry"
	cp "$REPO/$file" "$tree/$file"
	python3 - "$tree/$file" "$original" "$mutant" <<'PY'
import sys, pathlib
path, original, mutant = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = path.read_text()
if text.count(original) != 1:
    sys.exit(f"mutation target found {text.count(original)} times in {path}: '{original}'")
if sum(a != b for a, b in zip(original, mutant)) != 1 or len(original) != len(mutant):
    sys.exit("a mutant must differ by exactly one character")
path.write_text(text.replace(original, mutant))
PY

	printf '\n== mutant %d: %s\n' "$index" "$what"
	find "$tree/build/CMakeFiles/filament_core.dir" "$tree/build/CMakeFiles/fitest.dir" -name '*.o' -delete
	rm -f "$tree/build/fitest"
	if ! cmake --build "$tree/build" --target fitest -j8 >"$WORK/build.log" 2>&1; then
		printf '   FAIL  the mutant does not compile; it tests nothing\n'
		tail -5 "$WORK/build.log" | sed 's/^/        /'
		cp "$REPO/$file" "$tree/$file"
		ran=$(( ran + 1 ))
		continue
	fi
	status=0
	extra=()
	[[ "$check" == "--crossover" || "$check" == "--detector" ]] || extra=( --size 320x180 )
	start=$( date +%s )
	limit 600 "$tree/build/fitest" "$check" "${extra[@]}" >"$WORK/log" 2>&1 || status=$?
	took=$(( $( date +%s ) - start ))
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES (%d s) -- the check does not cover this code\n' "$check" "$took"
	else
		why="exit $status"
		[[ "$status" -eq 142 ]] && why="timed out at 600 s, counted as caught"
		printf '   ok    %s fails against the mutant (%s, %d s):\n' "$check" "$why" "$took"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^   FAIL' "$WORK/log" || true; } | head -2 | cut -c1-170 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
	ran=$(( ran + 1 ))
	cp "$REPO/$file" "$tree/$file"
done

printf '\nmutants: %d, caught: %d\n' "$ran" "$caught"
[[ "$caught" -eq "$ran" ]]
