#!/usr/bin/env python3
"""Tests for scripts/check_per_n_resizes.py (stdlib unittest, no external deps).

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import check_per_n_resizes as lint  # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def diff_of(path, *added):
    body = "".join(f"+{line}\n" for line in added)
    return (f"diff --git a/{path} b/{path}\n--- a/{path}\n+++ b/{path}\n"
            f"@@ -10,0 +11,{len(added)} @@\n{body}")


def run(path, *added, entries=(), source=("void Foo::bar()", "{", "}")):
    return lint.scan(diff_of(path, *added), lambda _p: list(source), list(entries))


class FlaggedLines(unittest.TestCase):
    def test_slot_count_call(self):
        self.assertEqual(len(run("src/libslic3r/Foo.cpp", "    bundle.set_num_filaments(n);")), 1)

    def test_extruder_and_flush_calls(self):
        self.assertEqual(len(run("src/libslic3r/Foo.cpp", "    cfg.set_num_extruders(4);")), 1)
        self.assertEqual(len(run("src/libslic3r/Foo.cpp", "    resize_flush_volumes_matrix(cfg, 2, 1);")), 1)

    def test_values_resize_by_key(self):
        line = '    cfg.option<ConfigOptionStrings>("filament_colour")->values.resize(n);'
        self.assertEqual(len(run("src/libslic3r/Foo.cpp", line)), 1)

    def test_variable_bound_to_key(self):
        found = run("src/libslic3r/Foo.cpp",
                    '    auto *opt = cfg.option<ConfigOptionInts>("filament_map");',
                    "    opt->values.push_back(0);")
        self.assertEqual([f[1] for f in found], [12])

    def test_vector_base_resize(self):
        found = run("src/libslic3r/Foo.cpp",
                    "    auto *vec = dynamic_cast<ConfigOptionVectorBase *>(cfg.option(key));",
                    "    vec->resize(count);")
        self.assertEqual([f[1] for f in found], [12])

    def test_filament_presets_edit(self):
        self.assertEqual(len(run("src/libslic3r/Foo.cpp", "    bundle.filament_presets.push_back(name);")), 1)

    def test_short_colour_check(self):
        line = "    if (colors_option->values.size() < filament_count) return;".replace("colors_option", "filament_colors_option")
        self.assertEqual(len(run("src/OrcaSlicer.cpp", line)), 1)


class IgnoredLines(unittest.TestCase):
    def test_reads_and_comments(self):
        self.assertEqual(run("src/libslic3r/Foo.cpp",
                             '    const auto &c = cfg.option<ConfigOptionStrings>("filament_colour")->values;',
                             "    // bundle.set_num_filaments(n);",
                             "    use(filament_colour.size());"), [])

    def test_declaration_is_not_a_call(self):
        self.assertEqual(run("src/libslic3r/Foo.hpp", "    void set_num_filaments(unsigned int n);"), [])

    def test_unqualified_local_named_filament_presets(self):
        self.assertEqual(run("src/slic3r/GUI/Tab.cpp", "    filament_presets.push_back(preset);"), [])

    def test_tests_and_non_cpp_are_not_scanned(self):
        self.assertEqual(run("tests/libslic3r/test_x.cpp", "    bundle.set_num_filaments(n);"), [])
        self.assertEqual(run("src/libslic3r/notes.md", "    bundle.set_num_filaments(n);"), [])


class AllowList(unittest.TestCase):
    LINE = "    bundle.set_num_filaments(n);"

    def test_function_scope(self):
        entries = [("src/libslic3r/Foo.cpp", "function", "bar")]
        self.assertEqual(run("src/libslic3r/Foo.cpp", self.LINE, entries=entries), [])
        other = ("void Foo::baz()", "{", "}")
        self.assertEqual(len(run("src/libslic3r/Foo.cpp", self.LINE, entries=entries, source=other)), 1)

    def test_line_and_file_scope(self):
        self.assertEqual(run("src/libslic3r/Foo.cpp", self.LINE,
                             entries=[("src/libslic3r/Foo.cpp", "line", "set_num_filaments(n)")]), [])
        self.assertEqual(run("src/libslic3r/Foo.cpp", self.LINE, entries=[("src/libslic3r/Foo.cpp", "file", "")]), [])
        self.assertEqual(len(run("src/libslic3r/Foo.cpp", self.LINE, entries=[("src/libslic3r/Bar.cpp", "file", "")])), 1)

    def test_entry_needs_a_reason(self):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as fh:
            fh.write("src/a.cpp :: file\n")
        try:
            with self.assertRaises(ValueError):
                lint.load_allowlist(fh.name)
        finally:
            os.unlink(fh.name)

    def test_shipped_allowlist_parses(self):
        self.assertTrue(lint.load_allowlist())


class SlotKeys(unittest.TestCase):
    def test_matches_the_bundle(self):
        path = os.path.join(ROOT, "src", "libslic3r", "PresetBundle.cpp")
        if not os.path.exists(path):
            self.skipTest("PresetBundle.cpp not in this checkout")
        with open(path, encoding="utf-8") as fh:
            text = fh.read()
        m = re.search(r'PresetBundle::filament_slot_keys\(\)\s*\{(.*?)\n\}', text, re.S)
        if not m:
            self.skipTest("filament_slot_keys() not in this tree")
        keys = set(re.findall(r'"(\w+)"', m.group(1)))
        self.assertLessEqual(keys, set(lint.SLOT_KEYS))


if __name__ == "__main__":
    unittest.main()
