#!/usr/bin/env python3
"""Flag added C++ lines that size a per-filament or per-extruder config array by hand.

Per-N arrays (one value per filament slot, extruder or variant) are sized by the
cardinality layer (PrintConfigCardinality.cpp) and by PresetBundle's slot functions.
Code that resizes them elsewhere drifts out of step with those counts, so this check
reads the lines a change adds under src/ and reports the ones that look like such a
resize. It is a textual heuristic for review, not a proof: deliberate exceptions go in
scripts/per_n_resize_allowlist.txt.

Usage:  scripts/check_per_n_resizes.py [BASE] [--head REF] [--warn-only] [--github]
                                        [--check-allowlist]

The diff is merge-base(BASE, HEAD)..HEAD. BASE defaults to origin/main, then main.
"""

import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ALLOWLIST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "per_n_resize_allowlist.txt")
BUNDLE_SOURCE = os.path.join(ROOT, "src", "libslic3r", "PresetBundle.cpp")

CPP_EXT = (".cpp", ".cc", ".cxx", ".mm", ".hpp", ".h", ".hxx")
SCAN_PREFIX = "src/"
LOOKBACK = 60

# Per-N arrays that are not in the bundle's slot list but are commonly touched.
OTHER_STEMS = [
    "filament_presets", "filament_diameter", "nozzle_diameter", "flush_volumes_matrix",
    "flush_volumes_vector", "printer_extruder_variant", "default_filament_colour",
]
# Option keys written as a string literal that are per-N by family.
KEY_FAMILY = re.compile(
    r'^(filament_\w+|nozzle_\w+|extruder_\w+|printer_extruder_\w+|machine_max_\w+|'
    r'flush_volumes_\w+|flush_multiplier|ams_multi_color_filment|default_filament_colour)$')

MUTATORS = r"(?:resize|push_back|emplace_back|insert|erase|clear|assign|pop_back|swap|set_at|set|append)"
OPT_SRC = re.compile(r'\boption(?:_throw)?\s*(?:<[^>]*>)?\s*\(|\bopt(?:ptr)?\s*(?:<[^>]*>)?\s*\(|'
                     r'\bget_option\b|dynamic_cast\s*<\s*(?:const\s+)?ConfigOption')
CALL_OWNERS = {
    "set_num_filaments": "new call site of set_num_filaments: check the slot count is not already set by the "
                         "top-up and that the model follows the slots",
    "set_num_extruders": "new call site of set_num_extruders: enforce_cardinality already sizes per-extruder "
                         "options at load and apply",
    "resize_flush_volumes_matrix": "new call site of resize_flush_volumes_matrix: check the bundle or the CLI "
                                   "has not already rebuilt the matrix",
    "resize_filament_slot_arrays": "resize_filament_slot_arrays is private to the bundle's slot-count functions",
}

SLOT_KEYS = []
STEM_RE = ""
TOKEN_RE = re.compile(r"(?!)")


def parse_slot_keys(text):
    """Keys listed in PresetBundle::filament_slot_keys() in the given source text."""
    m = re.search(r'PresetBundle::filament_slot_keys\(\)\s*\{(.*?)\n\}', text, re.S)
    return re.findall(r'"(\w+)"', m.group(1)) if m else []


def configure(slot_keys):
    """Set the slot-array keys and build the patterns derived from them."""
    global SLOT_KEYS, STEM_RE, TOKEN_RE
    SLOT_KEYS = list(slot_keys)
    stems = set()
    for s in SLOT_KEYS + OTHER_STEMS:
        stems.add(s)
        if "colour" in s:
            stems.add(s.replace("colour", "color"))
    STEM_RE = "|".join(re.escape(s) for s in sorted(stems, key=len, reverse=True))
    TOKEN_RE = re.compile(r'\w*(?:' + STEM_RE + r')\w*')


def load_slot_keys():
    try:
        with open(BUNDLE_SOURCE, encoding="utf-8") as fh:
            keys = parse_slot_keys(fh.read())
    except OSError:
        keys = []
    if not keys:
        print("warning: could not read filament_slot_keys() from PresetBundle.cpp; "
              "slot arrays are matched by their option families only", file=sys.stderr)
    return keys


def is_per_n_key(key):
    return key in SLOT_KEYS or bool(KEY_FAMILY.match(key))


def strip_code(line):
    """Drop comments; return '' for comment-only lines."""
    s = line.strip()
    if s.startswith(("//", "*/")) or s == "*" or s.startswith("* ") \
            or (s.startswith("/*") and not s.partition("*/")[2].strip()):
        return ""
    hidden = re.sub(r'"(?:[^"\\]|\\.)*"', lambda m: m.group(0).replace("/", "_"), line)
    hidden = re.sub(r'/\*.*?\*/', lambda m: " " * len(m.group(0)), hidden)
    i = hidden.find("//")
    out = line[:i] if i >= 0 else line
    return re.sub(r'/\*.*?\*/', " ", out)


def mask_strings(code):
    """Keep string literals that are exactly a per-N key; blank out all other text in strings."""
    def sub(m):
        return m.group(0) if is_per_n_key(m.group(0)[1:-1]) else '""'
    return re.sub(r'"(?:[^"\\]|\\.)*"', sub, code)


def is_call(code, fn):
    """True when fn( appears as a call, not as a declaration or definition."""
    for m in re.finditer(r'\b' + fn + r'\s*\(', code):
        prefix = code[:m.start()]
        if re.fullmatch(r'\s*(?:(?:static|inline|virtual)\s+)*[\w:<>]+[\s&*]+(?:\w+::)?', prefix) \
                and not re.match(r'\s*(?:return|else|throw|co_return)\b', prefix):
            continue
        return True
    return False


def bind_info(stmt):
    """(name, kind) when the statement binds a name to a per-N option or its values.

    kind is 'base' for a ConfigOptionVectorBase, 'opt' for an option pointer looked up by a per-N
    key, 'ref' for a reference to the values of a per-N array."""
    code = mask_strings(stmt)
    m = re.search(r'(?:^|[\s(,;*&])(\w+)\s*(?:=(?!=)|\{)\s*(.*)', code, re.S) \
        or re.search(r'[*&]\s*(\w+)\s*\((.*)', code, re.S)
    if not m or m.group(1) in ("if", "while", "for", "return", "switch"):
        return None
    name, rhs = m.group(1), m.group(2)
    if "ConfigOptionVectorBase" in code:
        return name, "base"
    has_key = any(is_per_n_key(k) for k in re.findall(r'"(\w+)"', rhs))
    if has_key and OPT_SRC.search(rhs):
        return name, "opt"
    if re.search(r'(?:(?:->|\.)\s*values|filament_presets|ams_multi_color_filment)\s*[;)}]*\s*$', rhs.strip()) \
            and (has_key or TOKEN_RE.search(rhs)):
        return name, "ref"
    return None


def statement_at(lines, i, limit=6):
    """Code of the statement starting at 0-based line i, joined across continuation lines."""
    parts = []
    for j in range(i, min(i + limit, len(lines))):
        c = strip_code(lines[j])
        parts.append(c.strip())
        if re.search(r'[;{}]\s*$', c):
            break
    return " ".join(parts)


def lookup_binding(lines, lineno, name):
    """Kind of the per-N binding of name in the LOOKBACK lines above 1-based lineno, or None."""
    for i in range(min(lineno - 2, len(lines) - 1), max(lineno - 2 - LOOKBACK, -1), -1):
        if name not in lines[i]:
            continue
        info = bind_info(statement_at(lines, i))
        if info and info[0] == name:
            return info[1]
    return None


def classify(stmt, lines, lineno, path=""):
    """Return the reason this statement resizes a per-N array, else None."""
    code = mask_strings(stmt)
    for fn, why in CALL_OWNERS.items():
        if is_call(code, fn):
            return why
    if re.search(r'ConfigOptionVectorBase[^;]*(?:->|\.)\s*' + MUTATORS + r'\s*\(', code):
        return "resizes an option through ConfigOptionVectorBase; enforce_cardinality sizes per-N options"

    bundle_file = os.path.basename(path).startswith("PresetBundle")
    after_key = r'\s*(?:,\s*[\w:]+\s*)?\)\s*'
    chain = r'(?:->|\.)\s*(?:values\s*(?:->|\.)\s*)?' + MUTATORS + r'\s*\('
    for m in re.finditer(r'"(\w+)"', code):
        if not is_per_n_key(m.group(1)):
            continue
        rest, before = code[m.end():], code[:m.start()]
        if re.match(after_key + chain, rest) or re.match(after_key + r'->\s*values\s*=[^=]', rest):
            return "mutates a per-N array outside the cardinality layer and the bundle's slot functions"
        if re.search(r'\b(?:set_key_value|set)\s*\(\s*$', before) and "placeholder_parser" not in code:
            return "replaces a whole per-N array, which keeps its own length rather than the count that indexes it"
        if re.match(r'\s*\*', before) and re.match(after_key + r'=[^=]', rest):
            return "assigns a whole per-N array, which keeps its own length rather than the count that indexes it"
    for m in TOKEN_RE.finditer(code):
        ident, rest = m.group(0), code[m.end():]
        if ident in ("filament_presets", "ams_multi_color_filment") or ident.endswith("_filament_presets"):
            if (bundle_file or re.search(r'(?:->|\.)\s*$', code[:m.start()])) \
                    and re.match(r'\s*(?:=[^=]|\.\s*' + MUTATORS + r'\s*\()', rest):
                return f"changes {ident} outside the bundle's slot functions"
            continue
        if re.match(r'\s*(?:->|\.)\s*values\s*(?:->|\.)\s*' + MUTATORS + r'\s*\(|\s*(?:->|\.)\s*values\s*=[^=]', rest) \
                or (re.search(r'(?:opt|option|ptr)$', ident) and re.match(r'\s*->\s*' + MUTATORS + r'\s*\(', rest)):
            return "mutates a per-N array outside the cardinality layer and the bundle's slot functions"
    # A name bound to a per-N option or its values, mutated here.
    names = {m.group(1) for m in re.finditer(r'\b(\w+)\s*(?:->|\.)\s*(?:values\s*(?:->|\.)\s*)?' + MUTATORS + r'\s*\(', code)}
    names |= {m.group(1) for m in re.finditer(r'\b(\w+)\s*->\s*values\s*=[^=]', code)}
    names |= {m.group(1) for m in re.finditer(r'(?:^|[;{(]\s*)\*\s*(\w+)\s*=[^=]', code)}
    for name in sorted(names - {"this"}):
        kind = lookup_binding(lines, lineno, name)
        if kind == "base":
            return "resizes a ConfigOptionVectorBase pointer; enforce_cardinality sizes per-N options"
        if kind in ("opt", "ref"):
            return "mutates a variable bound to a per-N array outside the cardinality layer and the slot functions"
    if not re.search(r'\b(?:return|continue|break|throw)\b', code):
        size_lt = r'(?:->|\.)\s*values\s*\.\s*size\(\)\s*<(?![=<])'
        if re.search(r'\w*(?:' + STEM_RE + r')\w*\s*' + size_lt, code) \
                or re.search(r'"(\w+)"[^;]*?\)\s*->\s*values\s*\.\s*size\(\)\s*<(?![=<])', code) \
                or any(lookup_binding(lines, lineno, n) in ("opt", "ref")
                       for n in re.findall(r'\b(\w+)\s*' + size_lt, code)):
            return "size check on a per-N array that may start a hand-rolled top-up; sizing belongs to the owners"
    return None


def load_allowlist(path=ALLOWLIST):
    """Entries: 'path :: file|function NAME|line TEXT :: reason'."""
    entries = []
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as fh:
        for raw in fh:
            raw = raw.strip()
            if not raw or raw.startswith("#"):
                continue
            parts = [p.strip() for p in raw.split(" :: ", 2)]
            if len(parts) != 3 or not parts[0] or not parts[1] or not parts[2]:
                raise ValueError(f"allow-list entry needs 'path :: scope :: reason': {raw}")
            scope = parts[1].split(None, 1)
            kind, arg = scope[0], (scope[1] if len(scope) > 1 else "")
            if kind == "file" and arg:
                raise ValueError(f"a file scope takes no argument: {raw}")
            if kind == "function" and not re.fullmatch(r'~?\w+', arg):
                raise ValueError(f"a function scope takes one function name: {raw}")
            if kind == "line" and len(arg) < 8:
                raise ValueError(f"a line scope needs at least 8 characters of text: {raw}")
            if kind not in ("file", "function", "line"):
                raise ValueError(f"bad allow-list scope: {raw}")
            entries.append((parts[0], kind, arg))
    return entries


def stale_entries(entries, read_file):
    """Entries whose path, function or text no longer exists in the head tree."""
    stale = []
    for epath, kind, arg in entries:
        try:
            lines = read_file(epath)
        except (subprocess.CalledProcessError, OSError):
            stale.append((epath, kind, arg))
            continue
        if kind == "function" and not any(re.match(r'[^\s}#/*{][^;]*\b' + re.escape(arg) + r'\s*\(', l) for l in lines):
            stale.append((epath, kind, arg))
        elif kind == "line" and not any(arg in l for l in lines):
            stale.append((epath, kind, arg))
    return stale


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
    return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True, text=True,
                          errors="replace").stdout


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


def statements(diff):
    """Yield (path, first line number, joined code) for runs of added lines ending at ; { or }."""
    cur = None
    for path, lineno, text in added_lines(diff):
        if not path.startswith(SCAN_PREFIX) or not path.endswith(CPP_EXT):
            continue
        if cur and (cur[0] != path or cur[1] + len(cur[2]) != lineno):
            yield cur[0], cur[1], " ".join(cur[2])
            cur = None
        code = strip_code(text)
        if cur is None:
            cur = (path, lineno, [])
        cur[2].append(code.strip())
        if not code.strip() or re.search(r'[;{}]\s*$', code) or len(cur[2]) >= 8:
            yield cur[0], cur[1], " ".join(cur[2])
            cur = None
    if cur:
        yield cur[0], cur[1], " ".join(cur[2])


def scan(diff, read_file, entries):
    findings = []
    cache = {}
    for path, lineno, stmt in statements(diff):
        if not stmt.strip():
            continue
        if path not in cache:
            cache[path] = read_file(path)
        lines = cache[path]
        reason = classify(stmt, lines, lineno, path)
        if reason is None or allowed(entries, path, stmt, enclosing_function(lines, lineno)):
            continue
        findings.append((path, lineno, stmt.strip(), reason))
    return findings


def esc_data(s):
    return s.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


def esc_prop(s):
    return esc_data(s).replace(":", "%3A").replace(",", "%2C")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("base", nargs="?", help="base ref (default origin/main, then main)")
    ap.add_argument("--head", default="HEAD", help="head ref (default HEAD)")
    ap.add_argument("--warn-only", action="store_true", help="report but exit 0")
    ap.add_argument("--github", action="store_true", help="print GitHub warning annotations")
    ap.add_argument("--check-allowlist", action="store_true", help="report allow-list entries that match nothing in HEAD")
    args = ap.parse_args(argv)

    configure(load_slot_keys())
    entries = load_allowlist()

    def read_file(path):
        return git("show", f"{args.head}:{path}").splitlines()

    if args.check_allowlist:
        stale = stale_entries(entries, read_file)
        for e in stale:
            print(f"stale allow-list entry: {e[0]} :: {e[1]} {e[2]}".rstrip())
        return 0 if (args.warn_only or not stale) else 1

    base = git("merge-base", args.base or default_base(), args.head).strip()
    diff = git("-c", "core.quotePath=false", "diff", "--no-color", "-U0", "--no-ext-diff",
               base, args.head, "--", SCAN_PREFIX)
    findings = scan(diff, read_file, entries)
    for path, lineno, text, reason in findings:
        if args.github:
            print(f"::warning file={esc_prop(path)},line={lineno},title=Per-N array resize::"
                  + esc_data(f"{reason}. See docs/HLSD/filament-slots.md; list a deliberate exception "
                             f"in scripts/per_n_resize_allowlist.txt"))
        else:
            print(f"{path}:{lineno}: {reason}\n    {text}")
    if findings:
        print(f"\n{len(findings)} possible hand-rolled per-N resize(s). Per-N arrays are sized by "
              f"enforce_cardinality() and PresetBundle's slot functions (docs/HLSD/filament-slots.md); "
              f"add a reasoned entry to scripts/per_n_resize_allowlist.txt for a deliberate exception.",
              file=sys.stderr)
    return 0 if (args.warn_only or not findings) else 1


configure([])

if __name__ == "__main__":
    sys.exit(main())
