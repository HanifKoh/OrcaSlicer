#!/usr/bin/env bash
# End-to-end check that the CLI sizes a project's per-filament arrays to its filament count before it
# uses them. A two-filament project whose filament_colour holds a single entry and whose flush matrix
# was written for three filaments, with the second filament used for the outer walls, must slice; the
# exported project must carry one colour per filament and the flush pairs the project had.
#
# usage: test_cli_short_palette.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-short-palette.XXXXXX")"
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

slice base "$WORK/cube.stl" \
    --load-settings "$PROFILES/machine/Bambu Lab P1S 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
    --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json;$PROFILES/filament/Bambu ABS @BBL P1S 0.4 nozzle.json"

# Keep the two filaments but leave a single colour, as a project written before the second slot existed.
"$PY" - "$WORK/base/out.3mf" "$WORK/short.3mf" <<'EOF2' || exit $?
import json, sys, zipfile

src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "Metadata/project_settings.config":
            config = json.loads(data)
            if len(config["filament_diameter"]) != 2:
                print("FAIL: the base project does not carry two filaments")
                sys.exit(1)
            config["filament_colour"] = ["#FF0000"]
            config["filament_colour_type"] = ["1"]
            config["filament_multi_colour"] = ["#FF0000"]
            config["outer_wall_filament"] = "2"
            # A three-filament matrix on a two-filament project: the pairs it has must survive the resize.
            config["flush_volumes_matrix"] = ["0", "123", "999", "456", "0", "999", "999", "999", "0"]
            config["flush_multiplier"] = ["1"]
            data = json.dumps(config, indent=4)
        zout.writestr(item, data)
EOF2

slice short "$WORK/short.3mf"

"$PY" - "$WORK/short/out.3mf" <<'EOF2' || { tail -n 40 "$WORK/short/log"; exit 1; }
import json, sys, zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    config = json.loads(z.read("Metadata/project_settings.config"))
    names = z.namelist()
errors = []
filaments = len(config["filament_diameter"])
for key in ("filament_colour", "filament_colour_type", "filament_multi_colour"):
    if len(config.get(key, [])) != filaments:
        errors.append("%s has %d entries for %d filaments" % (key, len(config.get(key, [])), filaments))
if config["filament_colour"][0] != "#FF0000":
    errors.append("the first colour was not kept: %r" % config["filament_colour"])
extruders = len(config["nozzle_diameter"])
if len(config["flush_volumes_matrix"]) != filaments * filaments * extruders:
    errors.append("flush_volumes_matrix has %d entries for %d filaments" % (len(config["flush_volumes_matrix"]), filaments))
elif [float(v) for v in config["flush_volumes_matrix"][:4]] != [0, 123, 456, 0]:
    errors.append("flush_volumes_matrix lost the authored pairs: %r" % config["flush_volumes_matrix"][:4])
if not any(n.startswith("Metadata/plate_1.gcode") for n in names):
    errors.append("no G-code in the exported project")
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF2
# A one-filament project given two filaments: the loaded count wins and the matrix follows it.
slice one "$WORK/cube.stl" \
    --load-settings "$PROFILES/machine/Bambu Lab P1S 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
    --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json"
slice two "$WORK/one/out.3mf" \
    --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json;$PROFILES/filament/Bambu PLA Matte @BBL P1S 0.4 nozzle.json"

"$PY" - "$WORK/two/out.3mf" <<'EOF2' || { tail -n 40 "$WORK/two/log"; exit 1; }
import json, sys, zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    config = json.loads(z.read("Metadata/project_settings.config"))
errors = []
if len(config["filament_diameter"]) != 2:
    errors.append("filament_diameter has %d entries for two loaded filaments" % len(config["filament_diameter"]))
# The CLI only carries the two colour side arrays when the project had them.
for key in ("filament_colour", "filament_colour_type", "filament_multi_colour"):
    if key in config and len(config[key]) != 2:
        errors.append("%s has %d entries for 2 filaments" % (key, len(config[key])))
if len(config["filament_colour"]) != 2:
    errors.append("filament_colour has %d entries for 2 filaments" % len(config["filament_colour"]))
extruders = len(config["nozzle_diameter"])
if len(config["flush_volumes_matrix"]) != 4 * extruders:
    errors.append("flush_volumes_matrix has %d entries for 2 filaments" % len(config["flush_volumes_matrix"]))
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF2
echo "PASS"
