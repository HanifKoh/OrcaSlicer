#!/usr/bin/env bash
# End-to-end check that the CLI writes one nozzle_volume_type per nozzle into an exported project, as the
# GUI does, so that reopening the export in the CLI keeps the flush matrix it carries instead of
# recomputing it for a changed set of volume types.
#
# usage: test_cli_nozzle_volume_type.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-nozzle-volume-type.XXXXXX")"
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

# A two-nozzle printer with two filaments.
slice base "$WORK/cube.stl" \
    --load-settings "$PROFILES/machine/Bambu Lab H2D 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL H2D.json" \
    --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL H2D.json;$PROFILES/filament/Bambu PLA Basic @BBL H2D.json"

# The export names a volume type per nozzle. Then give the project a flush matrix of its own.
"$PY" - "$WORK/base/out.3mf" "$WORK/authored.3mf" <<'EOF2' || exit $?
import json, sys, zipfile

src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "Metadata/project_settings.config":
            config = json.loads(data)
            nozzles = len(config["nozzle_diameter"])
            if nozzles != 2:
                print("FAIL: the H2D project does not carry two nozzles")
                sys.exit(1)
            if len(config.get("nozzle_volume_type", [])) != nozzles:
                print("FAIL: nozzle_volume_type has %d entries for %d nozzles: %s" % (len(config.get("nozzle_volume_type", [])), nozzles, config.get("nozzle_volume_type")))
                sys.exit(1)
            matrix = config["flush_volumes_matrix"]
            block = len(matrix) // nozzles
            if block * nozzles != len(matrix) or block < 4:
                print("FAIL: flush_volumes_matrix has %d entries for %d nozzles" % (len(matrix), nozzles))
                sys.exit(1)
            # One authored cell per nozzle block: filament 1 -> 2.
            for nozzle in range(nozzles):
                matrix[nozzle * block + 1] = "777"
            config["flush_volumes_matrix"] = matrix
            data = json.dumps(config, indent=4)
        zout.writestr(item, data)
EOF2

slice reopened "$WORK/authored.3mf"

"$PY" - "$WORK/reopened/out.3mf" <<'EOF2' || { tail -n 40 "$WORK/reopened/log"; exit 1; }
import json, sys, zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    config = json.loads(z.read("Metadata/project_settings.config"))
nozzles = len(config["nozzle_diameter"])
errors = []
if len(config.get("nozzle_volume_type", [])) != nozzles:
    errors.append("nozzle_volume_type has %d entries for %d nozzles after the reopen" % (len(config.get("nozzle_volume_type", [])), nozzles))
matrix = config["flush_volumes_matrix"]
block = len(matrix) // nozzles
for nozzle in range(nozzles):
    if float(matrix[nozzle * block + 1]) != 777:
        errors.append("the authored flush volume of nozzle %d was recomputed: %s" % (nozzle + 1, matrix[nozzle * 4 + 1]))
for e in errors:
    print("FAIL: " + e)
sys.exit(1 if errors else 0)
EOF2
echo "PASS"
