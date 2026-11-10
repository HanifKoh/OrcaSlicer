#!/usr/bin/env python3
"""Tests for scripts/check_per_n_resizes.py (stdlib unittest, no external deps).

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import check_per_n_resizes as lint  # noqa: E402

SAMPLE_BUNDLE = """\
const std::vector<std::string> &PresetBundle::filament_slot_keys()
{
    static const std::vector<std::string> keys = {
        "filament_colour",
        "filament_map",
        "filament_mixed_components",
    };
    return keys;
}
"""


def setUpModule():
    lint.configure(lint.parse_slot_keys(SAMPLE_BUNDLE))


def diff_of(path, added):
    body = "".join(f"+{line}\n" for line in added)
    return (f"diff --git a/{path} b/{path}\n--- a/{path}\n+++ b/{path}\n"
            f"@@ -10,0 +11,{len(added)} @@\n{body}")


def run(path, *added, entries=(), func="bar", context=()):
    """Scan lines added at line 11 of a file whose first lines are `context`, then the added ones."""
    source = [f"void Foo::{func}()", "{"] + list(context) + [""] * (8 - len(context)) + list(added)
    return lint.scan(diff_of(path, added), lambda _p: source, list(entries))


CPP = "src/libslic3r/Foo.cpp"
BIND = '    auto *opt = cfg.option<ConfigOptionStrings>("filament_colour");'


class SlotKeys(unittest.TestCase):
    def test_parser_reads_the_function(self):
        self.assertEqual(lint.parse_slot_keys(SAMPLE_BUNDLE),
                         ["filament_colour", "filament_map", "filament_mixed_components"])

    def test_parser_returns_nothing_without_the_function(self):
        self.assertEqual(lint.parse_slot_keys("int x;"), [])


class FlaggedLines(unittest.TestCase):
    def test_owner_calls(self):
        for call in ("bundle.set_num_filaments(n);", "cfg.set_num_extruders(4);",
                     "resize_flush_volumes_matrix(cfg, 2, 1);"):
            self.assertEqual(len(run(CPP, "    " + call)), 1, call)

    def test_values_resize_by_key(self):
        self.assertEqual(len(run(CPP, '    cfg.option<ConfigOptionStrings>("filament_colour")->values.resize(n);')), 1)

    def test_variable_bound_on_an_earlier_unchanged_line(self):
        found = run(CPP, "    opt->values.resize(n);", context=[BIND, "    foo();"])
        self.assertEqual(len(found), 1)

    def test_binding_forms(self):
        for bind in ('    if (auto *opt = cfg.option<ConfigOptionStrings>("filament_colour")) {',
                     '    auto *opt{cfg.option<ConfigOptionStrings>("filament_colour")};',
                     '    auto *opt = cfg.option<ConfigOptionStrings>(\n        "filament_colour");'):
            self.assertEqual(len(run(CPP, "    opt->values.push_back(c);", context=bind.split("\n"))), 1, bind)

    def test_vector_base(self):
        found = run(CPP, "    vec->resize(count);",
                    context=["    auto *vec = dynamic_cast<ConfigOptionVectorBase *>(cfg.option(key));"])
        self.assertEqual(len(found), 1)
        found = run(CPP, "    vec->set_at(src, i, j);",
                    context=["    ConfigOptionVectorBase *vec{dynamic_cast<ConfigOptionVectorBase *>(o)};"])
        self.assertEqual(len(found), 1)

    def test_whole_array_replacement(self):
        self.assertEqual(len(run(CPP, '    cfg.set_key_value("filament_colour", new ConfigOptionStrings(c));')), 1)
        self.assertEqual(len(run(CPP, '    *cfg.option<ConfigOptionFloats>("nozzle_diameter") = ConfigOptionFloats(v);')), 1)
        self.assertEqual(len(run(CPP, "    opt->set(other);", context=[BIND])), 1)

    def test_filament_presets_edit(self):
        self.assertEqual(len(run(CPP, "    bundle.filament_presets.push_back(name);")), 1)

    def test_short_array_check(self):
        line = "    const bool grown = project_filament_colors_option->values.size() < size_t(n);"
        self.assertEqual(len(run("src/OrcaSlicer.cpp", line)), 1)

    def test_block_comment_before_code(self):
        self.assertEqual(len(run(CPP, "    /* x */ bundle.set_num_filaments(n);")), 1)

    def test_continuation_line(self):
        found = run(CPP, '    cfg.option<ConfigOptionStrings>(', '        "filament_colour")->values.resize(n);')
        self.assertEqual([f[1] for f in found], [11])


class IgnoredLines(unittest.TestCase):
    def test_reads_comments_and_logs(self):
        self.assertEqual(run(CPP,
                             '    const auto &c = cfg.option<ConfigOptionStrings>("filament_colour")->values;',
                             "    // bundle.set_num_filaments(n);",
                             '    BOOST_LOG_TRIVIAL(info) << "filament_colour.push_back(x)";',
                             "    use(filament_colour.size());"), [])

    def test_ordinary_locals_with_a_stem(self):
        self.assertEqual(run(CPP, "    nozzle_diameters.push_back(d);", "    m_filament_colors.emplace_back(c);",
                             "    used_filament_map.insert(i);", '    config.erase("print_host");'), [])

    def test_bounds_guard_that_returns(self):
        self.assertEqual(run(CPP, "    if (project_filament_colors_option->values.size() <= idx) return;"), [])

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
        entries = [(CPP, "function", "bar")]
        self.assertEqual(run(CPP, self.LINE, entries=entries), [])
        self.assertEqual(len(run(CPP, self.LINE, entries=entries, func="baz")), 1)

    def test_line_and_file_scope(self):
        self.assertEqual(run(CPP, self.LINE, entries=[(CPP, "line", "set_num_filaments(n)")]), [])
        self.assertEqual(run(CPP, self.LINE, entries=[(CPP, "file", "")]), [])
        self.assertEqual(len(run(CPP, self.LINE, entries=[("src/libslic3r/Bar.cpp", "file", "")])), 1)

    def parse(self, text):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as fh:
            fh.write(text + "\n")
        try:
            return lint.load_allowlist(fh.name)
        finally:
            os.unlink(fh.name)

    def test_scope_text_with_namespace_separators_is_kept_whole(self):
        entries = self.parse("src/a.cpp :: line resize_slots(std::max(a, b) :: grows via std::max on purpose")
        self.assertEqual(entries, [("src/a.cpp", "line", "resize_slots(std::max(a, b)")])

    def test_invalid_entries_are_rejected(self):
        for bad in ("src/a.cpp :: file", "src/a.cpp :: file ::", " :: file :: r", "src/a.cpp :: file x :: r",
                    "src/a.cpp :: line short :: r", "src/a.cpp :: function :: r", "src/a.cpp :: nope x :: r",
                    "src/a.cpp::file::r"):
            with self.assertRaises(ValueError, msg=bad):
                self.parse(bad)

    def test_shipped_allowlist_parses(self):
        self.assertTrue(lint.load_allowlist())

    def test_stale_entries(self):
        source = ["void Foo::bar()", "{", "    set_num_filaments(n);", "}"]
        entries = [(CPP, "function", "bar"), (CPP, "function", "gone"), (CPP, "line", "set_num_filaments(n)"),
                   (CPP, "line", "no such text")]
        stale = lint.stale_entries(entries, lambda _p: source)
        self.assertEqual(stale, [(CPP, "function", "gone"), (CPP, "line", "no such text")])


class Annotations(unittest.TestCase):
    def test_escaping(self):
        self.assertEqual(lint.esc_prop("a:b,c%d\n"), "a%3Ab%2Cc%25d%0A")
        self.assertEqual(lint.esc_data("a:b%\r\n"), "a:b%25%0D%0A")


if __name__ == "__main__":
    unittest.main()
