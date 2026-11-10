#!/usr/bin/env bash
# End-to-end check that a project with a mixed filament, given more filaments than it has slots, takes
# the extra filament ahead of the mix. A project of two filaments and a mix of them, with the cube on the
# mix, sliced with a fourth filament: the export must hold the extra filament in the third slot and the
# mix in the fourth, the mix still made of the first two, and the cube on the mix.
#
# usage: test_cli_mixed_overload.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-mixed-overload.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

"$PY" - "$WORK/cube.stl" <<'EOF2'
import sys

v = [(x, y, z) for z in (0, 10) for y in (0, 10) for x in (0, 10)]
with open(sys.argv[1], "w") as f:
    f.write("solid cube\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")
EOF2

# slice <tag> <input> [option...]: slice into $WORK/<tag>/out.3mf with a fresh data directory.
slice() {
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" "$@" --slice 0 --outputdir "$out" --export-3mf out.3mf "$input" \
        > "$out/log" 2>&1 || { echo "FAIL: $1: orca-slicer exited $?"; tail -n 40 "$out/log"; exit 1; }
}

BASIC="$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json"
MATTE="$PROFILES/filament/Bambu PLA Matte @BBL P1S 0.4 nozzle.json"
ABS="$PROFILES/filament/Bambu ABS @BBL P1S 0.4 nozzle.json"

# Two filaments and a mix of them in the third slot, the cube on the mix.
slice project "$WORK/cube.stl" \
    --load-settings "$PROFILES/machine/Bambu Lab P1S 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
    --load-filaments "$BASIC;$MATTE;$BASIC" --load-filament-ids 3 \
    --filament-colour "#FF0000;#0000FF;#800080" \
    --filament-is-mixed=0,0,1 --filament-mixed-components ";;1,2" --filament-mixed-sublayer-ratios ";;0.5,0.5"

slice overload "$WORK/project/out.3mf" --load-filaments "$BASIC;$MATTE;$BASIC;$ABS"

"$PY" - "$WORK/project/out.3mf" "$WORK/overload/out.3mf" <<'EOF2' || { tail -n 40 "$WORK/overload/log"; exit 1; }
import json, re, sys, zipfile

def read(path):
    with zipfile.ZipFile(path) as z:
        config = json.loads(z.read("Metadata/project_settings.config"))
        model = z.read("Metadata/model_settings.config").decode()
        gcode = [z.read(n).decode() for n in z.namelist() if re.match(r"Metadata/plate_\d+\.gcode$", n)]
    return config, model, gcode

project, project_model, _ = read(sys.argv[1])
config, model, gcode = read(sys.argv[2])
errors = []
if [int(v) for v in project.get("filament_is_mixed", [])] != [0, 0, 1]:
    errors.append("the base project does not have its mix in the third slot: %r" % project.get("filament_is_mixed"))
if [int(v) for v in config.get("filament_is_mixed", [])] != [0, 0, 0, 1]:
    errors.append("filament_is_mixed is %r, not the mix behind the extra filament" % config.get("filament_is_mixed"))
if config.get("filament_mixed_components", [None] * 4)[3] != "1,2":
    errors.append("the mix is not made of the first two filaments: %r" % config.get("filament_mixed_components"))
types = config.get("filament_type", [])
if types[2:] != ["ABS", "PLA"]:
    errors.append("filament_type is %r, not the extra ABS ahead of the mix's own filament" % types)
for key in ("filament_colour", "filament_settings_id", "filament_diameter"):
    if len(config.get(key, [])) != 4:
        errors.append("%s has %d entries for 4 filaments" % (key, len(config.get(key, []))))
if config.get("filament_colour", [None] * 4)[3] != project.get("filament_colour", [None] * 3)[2]:
    errors.append("the mix lost its colour: %r" % config.get("filament_colour"))
for key in ("inherits_group", "different_settings_to_system"):
    if len(config.get(key, [])) != 6:
        errors.append("%s has %d columns for a print, four filaments and a printer" % (key, len(config.get(key, []))))
if config.get("inherits_group", [""])[-1] != project.get("inherits_group", [""])[-1]:
    errors.append("the printer column of inherits_group changed")
extruders = len(config["nozzle_diameter"])
if len(config.get("flush_volumes_matrix", [])) != 16 * extruders:
    errors.append("flush_volumes_matrix has %d entries for 4 filaments" % len(config.get("flush_volumes_matrix", [])))
# The cube was on the mix, and still is.
if 'key="extruder" value="3"' not in project_model:
    errors.append("the base project's cube is not on the mix")
if 'key="extruder" value="4"' not in model or 'key="extruder" value="3"' in model:
    errors.append("the cube did not follow the mix to the fourth slot")
if not gcode:
    errors.append("no G-code in the exported project")
elif not re.search(r"^M620 S1A", gcode[0], re.M) and not re.search(r"^T1\b", gcode[0], re.M):
    errors.append("the mix does not print with its second filament")
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF2
echo "PASS"
