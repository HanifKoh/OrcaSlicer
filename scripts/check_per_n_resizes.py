#!/usr/bin/env python3
"""Flag added C++ lines that size a per-filament or per-extruder config array by hand.

Per-N arrays (one value per filament slot, extruder or variant) are sized by the
cardinality layer (PrintConfigCardinality.cpp) and by PresetBundle's slot functions.
Code that resizes them elsewhere drifts out of step with those counts, so this check
reads the lines a change adds under src/ and reports the ones that look like such a
resize. It is a heuristic for review, not a proof: deliberate exceptions go in
scripts/per_n_resize_allowlist.txt.

Usage:  scripts/check_per_n_resizes.py [BASE] [--head REF] [--warn-only] [--github]

The diff is merge-base(BASE, HEAD)..HEAD. BASE defaults to origin/main, then main.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ALLOWLIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "per_n_resize_allowlist.txt")

CPP_EXT = (".cpp", ".cc", ".cxx", ".hpp", ".h", ".hxx")
SCAN_PREFIX = "src/"

# The arrays PresetBundle::filament_slot_keys() lists.
SLOT_KEYS = [
    "filament_colour", "filament_colour_type", "filament_multi_colour", "filament_map",
    "filament_nozzle_map", "filament_volume_map", "filament_is_mixed",
    "filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_gradient",
    "filament_mixed_gradient_range", "filament_mixed_gradient_curve",
    "filament_mixed_gradient_per_part", "ams_multi_color_filment",
]
# Other per-N arrays that code is likely to touch, matched in identifiers as well.
OTHER_STEMS = [
    "filament_presets", "filament_diameter", "nozzle_diameter", "flush_volumes_matrix",
    "flush_volumes_vector", "printer_extruder_variant",
]
# Option keys written as a string literal that are per-N by family.
KEY_FAMILY = re.compile(
    r'^(filament_\w+|nozzle_\w+|extruder_\w+|printer_extruder_\w+|machine_max_\w+|'
    r'flush_volumes_\w+|flush_multiplier|ams_multi_color_filment)$')

MUTATORS = r"(?:resize|push_back|emplace_back|insert|erase|clear|assign|pop_back|swap)"
CALL_OWNERS = {
    "set_num_filaments": "only PresetBundle's slot functions may change the slot count",
    "set_num_extruders": "grows every per-extruder array; only the preset-load and bundle owners may call it",
    "resize_flush_volumes_matrix": "the flush matrix is resized by the bundle and the CLI helper only",
    "resize_filament_slot_arrays": "private to PresetBundle's slot functions",
}


def stems():
    out = []
    for s in SLOT_KEYS + OTHER_STEMS:
        out.append(s)
        if "colour" in s:
            out.append(s.replace("colour", "color"))
    return out


# An identifier that contains a per-N stem (filament_colors_option, m_filament_presets).
STEM_RE = "|".join(re.escape(s) for s in sorted(set(stems()), key=len, reverse=True))
TOKEN_RE = re.compile(r'\w*(?:' + STEM_RE + r')\w*|"(\w+)"')
MUT_RE = re.compile(r'(?:->|\.)\s*(?:values\s*(?:->|\.)\s*)?' + MUTATORS + r'\s*\(|(?:->|\.)\s*values\s*=[^=]')


def is_per_n_key(key):
    return key in SLOT_KEYS or bool(KEY_FAMILY.match(key))


def per_n_token(text):
    """True when the text names a per-N array by identifier or by option key."""
    return any(m.group(1) is None or is_per_n_key(m.group(1)) for m in TOKEN_RE.finditer(text))


def strip_code(line):
    """Drop a trailing // comment; return '' for comment-only lines."""
    s = line.strip()
    if s.startswith(("//", "*", "/*")):
        return ""
    # Hide '//' inside string literals before cutting the comment.
    hidden = re.sub(r'"(?:[^"\\]|\\.)*"', lambda m: m.group(0).replace("/", "_"), line)
    i = hidden.find("//")
    return line[:i] if i >= 0 else line


def is_call(code, fn):
    """True when fn( appears as a call, not as a declaration or definition."""
    for m in re.finditer(r'\b' + fn + r'\s*\(', code):
        prefix = code[:m.start()]
        if re.fullmatch(r'\s*(?:(?:static|inline|virtual)\s+)*[\w:<>]+[\s&*]+(?:\w+::)?', prefix) \
                and not re.match(r'\s*(?:return|else|throw|co_return)\b', prefix):
            continue
        return True
    return False


def classify(code, tracked_keyed, tracked_base, path=""):
    """Return the reason this line resizes a per-N array, else None."""
    for fn, why in CALL_OWNERS.items():
        if is_call(code, fn):
            return f"call to {fn}: {why}"
    if re.search(r'ConfigOptionVectorBase[^;]*(?:->|\.)\s*resize\s*\(', code):
        return "resizes an option through ConfigOptionVectorBase"
    for name in tracked_base:
        if re.search(r'\b' + re.escape(name) + r'\s*(?:->|\.)\s*resize\s*\(', code):
            return "resizes a ConfigOptionVectorBase pointer; per-N options are sized by enforce_cardinality"
    for m in re.finditer(r'\w*(?:' + STEM_RE + r')\w*|"(\w+)"', code):
        if m.group(1) is not None and not is_per_n_key(m.group(1)):
            continue
        if m.group(0) == "filament_presets" and not os.path.basename(path).startswith("PresetBundle") \
                and not re.search(r'(?:->|\.)\s*$', code[:m.start()]):
            continue  # an unqualified local of that name, not the bundle's list
        rest = code[m.end():].split(";")[0]
        if MUT_RE.match(rest) or MUT_RE.search(rest):
            return "mutates a per-N array outside the cardinality layer and the bundle's slot functions"
    qualifier = "" if os.path.basename(path).startswith("PresetBundle") else r'(?:->|\.)\s*'
    if re.search(qualifier + r'\bfilament_presets\s*(?:=[^=]|\.\s*' + MUTATORS + r'\s*\()', code):
        return "changes filament_presets outside the bundle's slot functions"
    for name in tracked_keyed:
        if re.search(r'\b' + re.escape(name) + r'\s*(?:(?:->|\.)\s*values\s*)?(?:(?:->|\.)\s*' + MUTATORS + r'\s*\(|(?:->|\.)\s*values\s*=[^=])', code):
            return "mutates a variable bound to a per-N array"
    if re.search(r'(?:' + STEM_RE + r')\w*\s*(?:->|\.)\s*values\s*\.\s*size\(\)\s*<=?[^=<]', code):
        return "size check on a per-N array, usually the start of a hand-rolled top-up"
    return None


def load_allowlist(path=ALLOWLIST):
    """Entries: 'path :: file|function NAME|line SUBSTRING :: reason'."""
    entries = []
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            raw = raw.strip()
            if not raw or raw.startswith("#"):
                continue
            parts = [p.strip() for p in raw.split("::")]
            if len(parts) < 3 or not parts[2]:
                raise ValueError(f"allow-list entry needs 'path :: scope :: reason': {raw}")
            scope = parts[1].split(None, 1)
            kind, arg = scope[0], (scope[1] if len(scope) > 1 else "")
            if kind not in ("file", "function", "line") or (kind != "file" and not arg):
                raise ValueError(f"bad allow-list scope: {raw}")
            entries.append((parts[0], kind, arg))
    return entries


def enclosing_function(lines, lineno):
    """Name of the column-0 function definition above 1-based lineno, or ''."""
    for i in range(min(lineno, len(lines)) - 1, -1, -1):
        s = lines[i]
        if not s or s[0] in " \t}#/*{\n" or ";" in s.split("//")[0]:
            continue
        m = re.match(r'[^(]*?(~?\w+)\s*\(', s)
        if m:
            return m.group(1)
    return ""


def allowed(entries, path, text, func):
    for epath, kind, arg in entries:
        if epath == path and (kind == "file" or (kind == "function" and arg == func)
                              or (kind == "line" and arg in text)):
            return True
    return False


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True, text=True).stdout


def default_base():
    for ref in ("origin/main", "main"):
        if subprocess.run(["git", "-C", ROOT, "rev-parse", "--verify", "-q", ref], capture_output=True).returncode == 0:
            return ref
    sys.exit("no base ref: pass one explicitly")


def added_lines(diff):
    """Yield (path, new line number, text) for each added line of a zero-context diff."""
    path, new = None, 0
    for line in diff.splitlines():
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else None
        elif line.startswith("@@"):
            m = re.match(r'@@ -\S+ \+(\d+)', line)
            new = int(m.group(1)) if m else 0
        elif line.startswith("+") and path:
            yield path, new, line[1:]
            new += 1


def scan(diff, read_file, entries):
    findings = []
    state = {}
    cache = {}
    for path, lineno, text in added_lines(diff):
        if not path.startswith(SCAN_PREFIX) or not path.endswith(CPP_EXT):
            continue
        keyed, base = state.setdefault(path, ({}, {}))
        code = strip_code(text)
        if not code.strip():
            continue
        # Names bound to a per-N array or a ConfigOptionVectorBase are remembered for the
        # next 60 lines of the file.
        for d in (keyed, base):
            for k in [k for k, v in d.items() if lineno - v > 60]:
                del d[k]
        reason = classify(code, list(keyed), list(base), path)
        bind = re.match(r'\s*(?:[\w:<>,]+[\s&*]+)*?(\w+)\s*=[^=]', code)
        if bind:
            if "ConfigOptionVectorBase" in code:
                base[bind.group(1)] = lineno
            elif per_n_token(code[bind.end():]):
                keyed[bind.group(1)] = lineno
        if reason is None:
            continue
        if path not in cache:
            cache[path] = read_file(path)
        if allowed(entries, path, text, enclosing_function(cache[path], lineno)):
            continue
        findings.append((path, lineno, text.rstrip(), reason))
    return findings


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("base", nargs="?", help="base ref (default origin/main, then main)")
    ap.add_argument("--head", default="HEAD", help="head ref (default HEAD)")
    ap.add_argument("--warn-only", action="store_true", help="report but exit 0")
    ap.add_argument("--github", action="store_true", help="print GitHub warning annotations")
    args = ap.parse_args(argv)

    base = git("merge-base", args.base or default_base(), args.head).strip()
    diff = git("-c", "core.quotePath=false", "diff", "--no-color", "-U0", "--no-ext-diff",
               base, args.head, "--", SCAN_PREFIX)

    def read_file(path):
        return git("show", f"{args.head}:{path}").splitlines()

    findings = scan(diff, read_file, load_allowlist())
    for path, lineno, text, reason in findings:
        if args.github:
            print(f"::warning file={path},line={lineno},title=Per-N array resize::{reason}. "
                  f"See docs/HLSD/filament-slots.md; list deliberate exceptions in scripts/per_n_resize_allowlist.txt")
        else:
            print(f"{path}:{lineno}: {reason}\n    {text.strip()}")
    if findings:
        print(f"\n{len(findings)} possible hand-rolled per-N resize(s). Per-N arrays are sized by "
              f"enforce_cardinality() and PresetBundle's slot functions (docs/HLSD/filament-slots.md); "
              f"add a reasoned entry to scripts/per_n_resize_allowlist.txt for a deliberate exception.",
              file=sys.stderr)
    return 0 if (args.warn_only or not findings) else 1


if __name__ == "__main__":
    sys.exit(main())
