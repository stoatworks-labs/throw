#!/usr/bin/env bash
#
# Mutation testing: change ONE character of the shipped code and require that a
# check fails. A check that still passes against a mutant was not looking at
# the code it claims to cover -- and a harness that compiled its own copy of
# the shaders, or built its references from the plugin's own functions, would
# pass every mutant here.
#
# Each mutant is a copy of the tree in a temporary directory (the FFGL SDK and
# stb are symlinked, not copied), built arm64-only, with only the named check
# run at CI's raster, under a time limit (a mutant can hang: honeydew's lesson;
# a timeout counts as caught).
#
#     tools/mutate.sh
#
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# file | the exact text | its one-character mutant | the check that must fail | what it is
# The fields are split on '|', so neither a target nor a mutant may contain one
# (conway's trap).
MUTANTS=(
	"source/Shaders.cpp|cosA * cosA * cosA;|cosA * cosA + cosA;|--lambert|GLSL: the beam's spread off axis a sum, * -> +"
	"source/Shaders.cpp|float g      = inside ? Design * Design * lit / ( reach2 * spread )|float g      = inside ? Design * Design * lit * ( reach2 * spread )|--flat|GLSL: the light times the square of its distance, / -> *"
	"source/Shaders.cpp|S.z / ( Lens.z - S.z ) ) ) / Frame;|S.z / ( Lens.z + S.z ) ) ) / Frame;|--parallax|GLSL: the projector's ray to a recess by the wrong similar triangle, - -> +"
	"source/Shaders.cpp|vec2 toward  = ( p - eye.xy ) / eye.z;|vec2 toward  = ( p + eye.xy ) / eye.z;|--parallax|GLSL: the viewer's ray leaning the wrong way, - -> +"
	"source/Shaders.cpp|vec2 run   = ( Lens.xy - S.xy )|vec2 run   = ( Lens.xy + S.xy )|--shadow|GLSL: the shadow marched towards the wrong place, - -> +"
	"source/Shaders.cpp|float bias = 1e-3 * Relief;|float bias = 1e+3 * Relief;|--shadow|GLSL: a shadow bias a thousand reliefs high, - -> +"
	"source/Shaders.cpp|return normalize( vec3( -slope, 1.0 ) );|return normalize( vec3( -slope, 2.0 ) );|--lambert|GLSL: every slope halved in the normal, 1 -> 2"
	"source/Shaders.cpp|1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055|1.055 * pow( l, vec3( 1.0 / 2.5 ) ) - 0.055|--flat|GLSL: sRGB encoded at 1/2.5, 4 -> 5"
	"source/Shaders.cpp|float dd    = nh * nh * ( a2 - 1.0 ) + 1.0;|float dd    = nh * nh * ( a2 + 1.0 ) + 1.0;|--hotspot|GLSL: GGX's denominator from a2 + 1, - -> +"
	"source/Shaders.cpp|float smith = 0.5 / (|float smith = 0.6 / (|--hotspot|GLSL: the visibility term a fifth strong, 5 -> 6"
	"source/Shaders.cpp|( m * m * m * m * m )|( m * m * m * m + m )|--hotspot|GLSL: Schlick's fifth power a sum, * -> +"
	"source/Shaders.cpp|quv    = 0.5 + Design|quv    = 0.6 + Design|--keystone|GLSL: a pointed projector's picture a tenth off centre, 5 -> 6"
	"source/Shaders.cpp|sum += 1.0 / ( 1.0 + rise * rise );|sum += 2.0 / ( 1.0 + rise * rise );|--ambient|GLSL: occlusion letting in twice the room, 1 -> 2"
	"source/Shaders.cpp|float course = bh + jt;|float course = bh - jt;|--surfaces|GLSL: a course the brick less its joint, + -> -"
	"source/Shaders.cpp|h = -Relief * ( 0.5 - 0.5 * cosTurn( x ) );|h = -Relief * ( 0.5 - 0.6 * cosTurn( x ) );|--surfaces|GLSL: the corrugation's sine a fifth too deep, 5 -> 6"
	"source/Shaders.cpp|a2 * ( -0.5 + a2|a2 * ( -0.6 + a2|--surfaces|GLSL: cosTurn's second Taylor term off, 5 -> 6"
	"source/Room.cpp|AmbientFromParam( p[ PT_AMBIENT ] ) * room.W * room.H|AmbientFromParam( p[ PT_AMBIENT ] ) * room.W + room.H|--ambient|C++: the picture's area a sum, * -> +"
	"source/Room.cpp|room.right          = unit( cross( room.axis, Vec3 { 0.0, 1.0, 0.0 } ) );|room.right          = unit( cross( room.axis, Vec3 { 0.0, 1.0, 1.0 } ) );|--keystone|C++: a pointed projector's world up leaning at the wall, 0 -> 1"
	"source/Room.cpp|return std::clamp( 4.0 * crumple|return std::clamp( 5.0 * crumple|--units|C++: the paper's generations arriving a fifth early, 4 -> 5"
	"source/Room.h|kSineDepthPerPitch = 18.0 / 76.0;|kSineDepthPerPitch = 18.0 / 66.0;|--surfaces|C++: 3-inch corrugated 15% too deep, 7 -> 6"
	"source/Controls.cpp|kStopsMost * ( 2.0 * clamp01( value ) - 1.0 )|kStopsMost * ( 2.0 * clamp01( value ) - 2.0 )|--units|C++: Exposure's middle four stops down, 1 -> 2"
	"source/Throw.cpp|f( room.eye.x ), f( room.eye.y )|f( room.eye.x ), f( room.eye.x )|--parallax|C++: the viewer's height handed over as its offset, y -> x"
	"source/Throw.cpp|f( room.f0[ 0 ] ), f( room.f0[ 1 ] )|f( room.f0[ 0 ] ), f( room.f0[ 0 ] )|--hotspot|C++: green's reflectance handed over as red's, 1 -> 0"
)

# Checks that need no GL context run at no raster.
offline() { [[ "$1" == "--units" || "$1" == "--cues" || "$1" == "--names" ]]; }

limit() {
	# macOS has no timeout(1): perl's alarm is enough.
	perl -e 'alarm shift; exec @ARGV' "$@"
}

caught=0
for entry in "${MUTANTS[@]}"; do
	IFS='|' read -r file original mutant check what <<<"$entry"
	tree="$WORK/tree"
	rm -rf "$tree"
	mkdir -p "$tree/external"
	cp -R "$REPO/source" "$REPO/tools" "$REPO/cmake" "$REPO/CMakeLists.txt" "$tree/"
	ln -s "$REPO/external/ffgl" "$tree/external/ffgl"
	ln -s "$REPO/external/stb" "$tree/external/stb"

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

	printf '\n== mutant: %s\n' "$what"
	cmake -S "$tree" -B "$tree/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 >/dev/null
	cmake --build "$tree/build" --target twtest -j"$(sysctl -n hw.ncpu)" >/dev/null 2>&1
	status=0
	extra=()
	offline "$check" || extra=( --size 320x180 )
	limit 300 "$tree/build/twtest" "$check" "${extra[@]}" >"$WORK/log" 2>&1 || status=$?
	if [[ "$status" -eq 0 ]]; then
		printf '   FAIL  %s still PASSES -- the check does not cover this code\n' "$check"
	else
		printf '   ok    %s fails against the mutant (exit %d):\n' "$check" "$status"
		# A mutant that timed out printed no FAIL: grep finding none must not end the run.
		{ grep -E '^  FAIL' "$WORK/log" || true; } | head -2 | cut -c1-160 | sed 's/^/        /'
		caught=$(( caught + 1 ))
	fi
done

printf '\nmutants: %d, caught: %d\n' "${#MUTANTS[@]}" "$caught"
[[ "$caught" -eq "${#MUTANTS[@]}" ]]
