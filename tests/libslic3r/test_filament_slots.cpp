#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintConfigCardinality.hpp"

using namespace Slic3r;

namespace {

// Give the edited printer n nozzles of a tool changer: the top-up adds one physical slot per nozzle.
void set_nozzle_count(PresetBundle &bundle, size_t n)
{
    Preset &printer = bundle.printers.get_edited_preset();
    printer.config.option<ConfigOptionFloats>("nozzle_diameter", true)->values.assign(n, 0.4);
    printer.config.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = false;
}

const std::vector<std::string> &strings(const PresetBundle &bundle, const char *key)
{
    return bundle.project_config.option<ConfigOptionStrings>(key)->values;
}

std::vector<unsigned char> &mixed_flags(PresetBundle &bundle)
{
    return bundle.project_config.option<ConfigOptionBools>("filament_is_mixed", true)->values;
}

using Report = std::vector<std::pair<std::string, size_t>>;

Report reported(const PresetBundle &bundle)
{
    Report report;
    for (const PresetBundle::FilamentSlotMismatch &mismatch : bundle.filament_slot_mismatches())
        report.emplace_back(mismatch.key, mismatch.length);
    return report;
}

std::string describe(const std::vector<PresetBundle::FilamentSlotMismatch> &mismatches)
{
    std::string text;
    for (const PresetBundle::FilamentSlotMismatch &mismatch : mismatches)
        text += mismatch.key + " " + std::to_string(mismatch.length) + "; ";
    return text;
}

Preset &add_preset(PresetCollection &presets, const std::string &name)
{
    return presets.load_preset(std::string(), name, DynamicPrintConfig(presets.default_preset().config), false);
}

// A printer and a filament preset to select, both named "Test ...", with the printer selected.
void add_test_presets(PresetBundle &bundle)
{
    add_preset(bundle.printers, "Test Printer");
    bundle.printers.select_preset_by_name("Test Printer", true);
    add_preset(bundle.filaments, "Test Filament");
}

// Save a three-slot selection of the test presets the way the app does at exit.
void save_three_slot_selection(AppConfig &app_config)
{
    PresetBundle bundle;
    add_test_presets(bundle);
    bundle.filaments.select_preset_by_name("Test Filament", true);
    bundle.set_num_filaments(3u, "#FF0000");
    bundle.filament_presets.assign(3, "Test Filament");
    bundle.export_selections(app_config);
}

// A filament preset the AMS trays below resolve to; returns its filament id.
std::string add_tray_filament(PresetBundle &bundle)
{
    Preset &filament     = add_preset(bundle.filaments, "Test PLA");
    filament.filament_id = "GFSL00";
    filament.config.option<ConfigOptionStrings>("filament_type", true)->values = { "PLA" };
    return filament.filament_id;
}

// Load the next AMS tray with that filament in the given colour.
void add_tray(PresetBundle &bundle, const std::string &filament_id, const std::string &colour)
{
    const std::string  slot = std::to_string(bundle.filament_ams_list.size());
    DynamicPrintConfig tray;
    tray.set_key_value("filament_id", new ConfigOptionStrings{ filament_id });
    tray.set_key_value("filament_colour", new ConfigOptionStrings{ colour });
    tray.set_key_value("filament_colour_type", new ConfigOptionStrings{ "1" });
    tray.set_key_value("filament_multi_colour", new ConfigOptionStrings{ colour });
    tray.set_key_value("filament_type", new ConfigOptionStrings{ "PLA" });
    tray.set_key_value("ams_id", new ConfigOptionStrings{ "0" });
    tray.set_key_value("slot_id", new ConfigOptionStrings{ slot });
    tray.set_key_value("filament_slot_placeholder", new ConfigOptionBools{ false });
    bundle.filament_ams_list.emplace(static_cast<int>(bundle.filament_ams_list.size()), std::move(tray));
}

// Five slots, the last a mix of the second and third.
void set_mixed_tail(PresetBundle &bundle)
{
    bundle.set_num_filaments(5u, "#FF0000");
    mixed_flags(bundle) = { false, false, false, false, true };
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values      = { "", "", "", "", "2,3" };
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_sublayer_ratios")->values = { "", "", "", "", "0.5,0.5" };
}

} // namespace

TEST_CASE("Every slot array follows the preset list through growth, shrink and delete", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(4u, "#FF0000");
    CHECK(bundle.num_filament_slots() == 4);
    CHECK(bundle.filament_slots_consistent());

    bundle.set_num_filaments(6u);
    CHECK(bundle.num_filament_slots() == 6);
    CHECK(bundle.filament_slots_consistent());

    bundle.update_num_filaments(2u);
    CHECK(bundle.num_filament_slots() == 5);
    CHECK(bundle.filament_slots_consistent());

    bundle.set_num_filaments(2u);
    CHECK(bundle.num_filament_slots() == 2);
    CHECK(bundle.filament_slots_consistent());
}

TEST_CASE("filament_slot_keys lists one-per-filament vector options only", "[Preset][Bundle][FilamentSlots]")
{
    for (const std::string &key : PresetBundle::filament_slot_keys()) {
        CAPTURE(key);
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        // The slot resize visits strings, ints and bools only.
        CHECK((def->type == coStrings || def->type == coInts || def->type == coBools));
        // The cardinality classification sizes the same keys at apply, so the two lists cannot drift.
        CHECK(cardinality_of(key) == ConfigCardinality::PerFilament);
    }
}

TEST_CASE("A slot added without a colour takes the next palette colour", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(2u, "#FF0000");
    bundle.set_num_filaments(4u);

    const auto &colours = strings(bundle, "filament_colour");
    REQUIRE(colours.size() == 4);
    CHECK(colours[2] == "#00C1AE");
    CHECK(colours[3] == "#F4E2C1");
    CHECK(strings(bundle, "filament_multi_colour")[2] == "#00C1AE");
    CHECK(strings(bundle, "filament_colour_type")[2] == "1");
    // One sequence, shared with every caller that asks for a colour.
    CHECK(bundle.next_filament_colour() == "#ED1C24");
}

TEST_CASE("The nozzle-count top-up grows every slot array with the preset list", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(4u, "#FF0000");
    set_nozzle_count(bundle, 5);
    bundle.update_multi_material_filament_presets();

    CHECK(bundle.num_filament_slots() == 5);
    CHECK(bundle.num_physical_filaments() == 5);
    CHECK(bundle.filament_slots_consistent());
    const auto &colours = strings(bundle, "filament_colour");
    REQUIRE(colours.size() == 5);
    CHECK(colours[4] != colours[0]);
    CHECK(bundle.filament_presets[4] == bundle.filament_presets[3]);
    CHECK(bundle.take_pending_slot_relocations().empty());

    SECTION("and leaves a list that already reaches the nozzle count alone") {
        bundle.update_multi_material_filament_presets();
        CHECK(bundle.num_filament_slots() == 5);
        CHECK(strings(bundle, "filament_colour") == colours);
    }
}

TEST_CASE("The top-up inserts physical slots ahead of the mixed tail and reports the move", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(5u, "#FF0000");
    mixed_flags(bundle) = { false, false, false, false, true };
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values = { "", "", "", "", "2,3" };
    set_nozzle_count(bundle, 6);
    bundle.update_multi_material_filament_presets();

    CHECK(bundle.num_filament_slots() == 7);
    CHECK(bundle.num_physical_filaments() == 6);
    CHECK(bundle.num_mixed_filaments() == 1);
    CHECK(bundle.filament_slots_consistent());
    CHECK(bundle.is_mixed_filament(6));
    CHECK_FALSE(bundle.is_mixed_filament(4));
    CHECK_FALSE(bundle.is_mixed_filament(5));
    CHECK(strings(bundle, "filament_mixed_components")[6] == "2,3");
    CHECK(strings(bundle, "filament_colour")[6] == "#FF0000");

    CHECK(bundle.take_pending_slot_relocations() == std::map<int, int>{ { 4, 6 } });
    CHECK(bundle.take_pending_slot_relocations().empty());
}

TEST_CASE("insert_filament_slot places the colour at the position and copies the preset before it", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(3u, "#FF0000");
    bundle.filament_presets = { "A", "B", "C" };
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = { "#111111", "#222222", "#333333" };
    bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = { "#111111 #AAAAAA", "#222222", "#333333" };

    const std::map<int, int> moved = bundle.insert_filament_slot(1, "#00FF00");
    CHECK(bundle.filament_presets == std::vector<std::string>{ "A", "A", "B", "C" });
    CHECK(strings(bundle, "filament_colour") == std::vector<std::string>{ "#111111", "#00FF00", "#222222", "#333333" });
    // A kept slot keeps its colour pack; the new one starts as its single colour.
    CHECK(strings(bundle, "filament_multi_colour") == std::vector<std::string>{ "#111111 #AAAAAA", "#00FF00", "#222222", "#333333" });
    CHECK(moved == std::map<int, int>{ { 1, 2 }, { 2, 3 } });
    CHECK(bundle.filament_slots_consistent());

    SECTION("appending at the end moves nothing") {
        CHECK(bundle.insert_filament_slot(4, "#0000FF").empty());
        CHECK(bundle.num_filament_slots() == 5);
        CHECK(strings(bundle, "filament_colour")[4] == "#0000FF");
        CHECK(bundle.filament_slots_consistent());
    }
}

TEST_CASE("A bundle without selections keeps its default first colour through the top-up", "[Preset][Bundle][FilamentSlots]")
{
    // The arrays still hold the option defaults; the preset list is empty until load_selections.
    PresetBundle bundle;
    const std::string default_colour = strings(bundle, "filament_colour").front();
    REQUIRE(bundle.num_filament_slots() == 0);

    SECTION("one nozzle") {
        set_nozzle_count(bundle, 1);
        bundle.update_multi_material_filament_presets();
        CHECK(bundle.num_filament_slots() == 1);
        CHECK(strings(bundle, "filament_colour") == std::vector<std::string>{ default_colour });
        CHECK(bundle.filament_slots_consistent());
    }
    SECTION("more nozzles grow the arrays behind the default") {
        set_nozzle_count(bundle, 3);
        bundle.update_multi_material_filament_presets();
        CHECK(bundle.num_filament_slots() == 3);
        CHECK(bundle.filament_slots_consistent());
        const auto &colours = strings(bundle, "filament_colour");
        CHECK(colours[0] == default_colour);
        CHECK(colours[1] != default_colour);
        CHECK(colours[2] != colours[1]);
    }
}

TEST_CASE("num_physical_filaments counts the preset list minus the mixed tail", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(5u, "#FF0000");
    mixed_flags(bundle) = { false, false, false, false, true };
    CHECK(bundle.num_mixed_filaments() == 1);
    CHECK(bundle.num_physical_filaments() == 4);

    SECTION("missing flags mean physical filaments") {
        mixed_flags(bundle) = { false, false };
        CHECK(bundle.num_mixed_filaments() == 0);
        CHECK(bundle.num_physical_filaments() == 5);
    }
    SECTION("flags past the slot count do not count") {
        mixed_flags(bundle) = { false, false, false, false, false, true, true };
        CHECK(bundle.num_mixed_filaments() == 0);
        CHECK(bundle.num_physical_filaments() == 5);
    }
}

// A printer feeding its filaments through shared nozzles maps any number of them onto its nozzles,
// so the top-up never adds a slot it has not been given, whatever the nozzle count.
TEST_CASE("A single-extruder multi-material printer keeps the slots it has", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(2u, "#FF0000");
    mixed_flags(bundle) = { false, true };
    Preset &printer = bundle.printers.get_edited_preset();
    printer.config.option<ConfigOptionFloats>("nozzle_diameter", true)->values.assign(3, 0.4);
    printer.config.option<ConfigOptionBool>("single_extruder_multi_material", true)->value = true;

    bundle.update_multi_material_filament_presets();
    CHECK(bundle.num_filament_slots() == 2);
    CHECK(bundle.num_mixed_filaments() == 1);
    CHECK(bundle.take_pending_slot_relocations().empty());
    CHECK(bundle.filament_slots_consistent());

    // One filament is enough for it: the user may delete down to one, and it stays deleted.
    bundle.update_num_filaments(1);
    CHECK(bundle.num_filament_slots() == 1);
    CHECK(bundle.take_pending_slot_relocations().empty());
    CHECK(bundle.filament_slots_consistent());
}

TEST_CASE("Deleting a slot the top-up puts back shifts the flush matrix", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(2u, "#FF0000");
    set_nozzle_count(bundle, 2);
    bundle.update_multi_material_filament_presets();
    bundle.take_pending_slot_relocations();
    bundle.project_config.option<ConfigOptionFloats>("flush_volumes_vector", true)->values = { 10, 10, 10, 10 };
    bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values = { 0, 11, 12, 0, 0, 21, 22, 0 };

    bundle.update_num_filaments(0);
    // The second filament is now first and a new slot follows it, so no pair of the deleted one survives.
    REQUIRE(bundle.num_filament_slots() == 2);
    CHECK(bundle.project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values == std::vector<double>{ 0, 20, 20, 0, 0, 20, 20, 0 });
}

TEST_CASE("insert_filament_slot sizes arrays that run behind the preset list first", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(3u, "#FF0000");
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = { "#111111" };

    const std::map<int, int> moved = bundle.insert_filament_slot(1, "#00FF00");
    CHECK(bundle.num_filament_slots() == 4);
    CHECK(strings(bundle, "filament_colour").size() == 4);
    CHECK(strings(bundle, "filament_colour")[1] == "#00FF00");
    CHECK(moved == std::map<int, int>{ { 1, 2 }, { 2, 3 } });
    CHECK(bundle.filament_slots_consistent());
}

TEST_CASE("A new slot starts on nozzle 1 with a standard-flow nozzle map", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(1u, "#FF0000");
    bundle.project_config.option<ConfigOptionInts>("filament_map", true)->values        = { 2 };
    bundle.project_config.option<ConfigOptionInts>("filament_nozzle_map", true)->values = { 7 };
    bundle.project_config.option<ConfigOptionInts>("filament_volume_map", true)->values = { 1 };

    bundle.set_num_filaments(2u);
    const auto ints = [&bundle](const char *key) { return bundle.project_config.option<ConfigOptionInts>(key)->values; };
    CHECK(ints("filament_map") == std::vector<int>{ 2, 1 });
    CHECK(ints("filament_nozzle_map") == std::vector<int>{ 7, 0 });
    CHECK(ints("filament_volume_map") == std::vector<int>{ 1, static_cast<int>(NozzleVolumeType::nvtStandard) });
    CHECK(strings(bundle, "filament_colour_type") == std::vector<std::string>{ "1", "1" });
}

TEST_CASE("The bundle reports each slot array out of step with the slot count", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(4u, "#FF0000");
    REQUIRE(reported(bundle).empty());

    SECTION("short and long arrays, by length") {
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.resize(3);
        bundle.project_config.option<ConfigOptionInts>("filament_nozzle_map")->values.resize(6);
        bundle.ams_multi_color_filment.resize(2);
        CHECK(reported(bundle) == Report{ { "filament_colour", 3 }, { "filament_nozzle_map", 6 }, { "ams_multi_color_filment", 2 } });
        CHECK_FALSE(bundle.filament_slots_consistent());
    }
    SECTION("a missing array as empty") {
        bundle.project_config.erase("filament_colour_type");
        CHECK(reported(bundle) == Report{ { "filament_colour_type", 0 } });
    }
}

TEST_CASE("Normalising seeds what a writer left out of a slot array", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(4u, "#FF0000");
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = { "#111111", "#222222", "#333333", "#444444" };

    SECTION("slots in step are left alone") {
        const DynamicPrintConfig before = bundle.project_config;
        bundle.normalize_filament_slots();
        CHECK(bundle.filament_slot_repairs() == 0);
        CHECK(bundle.project_config == before);
    }
    SECTION("a short colour list takes the next palette colour") {
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.resize(3);
        bundle.normalize_filament_slots();
        CHECK(strings(bundle, "filament_colour") == std::vector<std::string>{ "#111111", "#222222", "#333333", "#00C1AE" });
        CHECK(bundle.filament_slots_consistent());
        CHECK(bundle.filament_slot_repairs() == 1);
        REQUIRE(bundle.last_filament_slot_repair().size() == 1);
        CHECK(bundle.last_filament_slot_repair()[0].key == "filament_colour");
        CHECK(bundle.last_filament_slot_repair()[0].length == 3);
    }
    SECTION("a short nozzle map leaves the new slot without a nozzle") {
        bundle.project_config.option<ConfigOptionInts>("filament_nozzle_map")->values = { 7, 7, 7 };
        bundle.normalize_filament_slots();
        CHECK(bundle.project_config.option<ConfigOptionInts>("filament_nozzle_map")->values == std::vector<int>{ 7, 7, 7, 0 });
        CHECK(bundle.filament_slots_consistent());
        CHECK(bundle.filament_slot_repairs() == 1);
    }
    SECTION("short maps and colour packs take the new-slot defaults") {
        bundle.project_config.option<ConfigOptionStrings>("filament_colour_type")->values  = { "0", "0" };
        bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour")->values = { "#111111 #AAAAAA", "#222222" };
        bundle.project_config.option<ConfigOptionInts>("filament_map")->values             = { 2, 2 };
        bundle.project_config.option<ConfigOptionInts>("filament_volume_map")->values      = { 1, 1 };
        bundle.normalize_filament_slots();
        CHECK(strings(bundle, "filament_colour_type") == std::vector<std::string>{ "0", "0", "1", "1" });
        CHECK(strings(bundle, "filament_multi_colour") == std::vector<std::string>{ "#111111 #AAAAAA", "#222222", "#333333", "#444444" });
        CHECK(bundle.project_config.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{ 2, 2, 1, 1 });
        const int standard = static_cast<int>(NozzleVolumeType::nvtStandard);
        CHECK(bundle.project_config.option<ConfigOptionInts>("filament_volume_map")->values == std::vector<int>{ 1, 1, standard, standard });
        CHECK(bundle.filament_slot_repairs() == 1);
    }
    SECTION("a short AMS colour list keeps its packs and gets empty ones") {
        bundle.ams_multi_color_filment = { { "#111111", "#AAAAAA" } };
        bundle.normalize_filament_slots();
        CHECK(bundle.ams_multi_color_filment == std::vector<std::vector<std::string>>{ { "#111111", "#AAAAAA" }, {}, {}, {} });
        CHECK(bundle.filament_slot_repairs() == 1);
    }
    SECTION("a long array is cut at the end") {
        bundle.project_config.option<ConfigOptionStrings>("filament_colour_type")->values = { "0", "1", "0", "1", "0" };
        bundle.normalize_filament_slots();
        CHECK(strings(bundle, "filament_colour_type") == std::vector<std::string>{ "0", "1", "0", "1" });
        CHECK(bundle.filament_slot_repairs() == 1);
    }
    SECTION("each call that finds the slots out of step counts once") {
        bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.resize(3);
        bundle.ams_multi_color_filment.resize(2);
        bundle.normalize_filament_slots();
        bundle.normalize_filament_slots();
        CHECK(bundle.filament_slot_repairs() == 1);
        CHECK(reported(bundle).empty());
    }
}

TEST_CASE("Normalising seeds a physical slot's missing values ahead of the mixed tail", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    set_mixed_tail(bundle);
    const std::vector<unsigned char> tail_last = { false, false, false, false, true };

    SECTION("gradient arrays a writer left out") {
        for (const char *key : { "filament_mixed_gradient", "filament_mixed_gradient_per_part" })
            bundle.project_config.option<ConfigOptionBools>(key)->values.resize(3);
        for (const char *key : { "filament_mixed_gradient_range", "filament_mixed_gradient_curve" })
            bundle.project_config.option<ConfigOptionStrings>(key)->values.resize(3);
        bundle.normalize_filament_slots();
        CHECK(bundle.filament_slots_consistent());
        CHECK(mixed_flags(bundle) == tail_last);
        CHECK(bundle.project_config.option<ConfigOptionBools>("filament_mixed_gradient")->values == std::vector<unsigned char>(5, false));
        CHECK(strings(bundle, "filament_mixed_gradient_curve") == std::vector<std::string>(5));
    }
    SECTION("mixed metadata cut short behind its mixed slot") {
        // A writer added a physical slot and left the seven mixed arrays a slot short, the mix last.
        for (const char *key : { "filament_is_mixed", "filament_mixed_gradient", "filament_mixed_gradient_per_part" }) {
            auto &values = bundle.project_config.option<ConfigOptionBools>(key)->values;
            values.erase(values.begin());
        }
        for (const char *key : { "filament_mixed_components", "filament_mixed_sublayer_ratios", "filament_mixed_gradient_range",
                                 "filament_mixed_gradient_curve" }) {
            auto &values = bundle.project_config.option<ConfigOptionStrings>(key)->values;
            values.erase(values.begin());
        }
        REQUIRE(mixed_flags(bundle) == std::vector<unsigned char>{ false, false, false, true });
        bundle.normalize_filament_slots();
        CHECK(bundle.filament_slots_consistent());
        CHECK(mixed_flags(bundle) == tail_last);
        CHECK(strings(bundle, "filament_mixed_components") == std::vector<std::string>{ "", "", "", "", "2,3" });
        CHECK(strings(bundle, "filament_mixed_sublayer_ratios")[4] == "0.5,0.5");
    }
    SECTION("a colour list short of a physical slot's colour keeps the mix's colour on the mix") {
        auto &colours = bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values;
        colours       = { "#000000", "#111111", "#222222", "#333333", "#777777" };
        colours.erase(colours.begin() + 3);
        bundle.normalize_filament_slots();
        CHECK(strings(bundle, "filament_colour") == std::vector<std::string>{ "#000000", "#111111", "#222222", "#00C1AE", "#777777" });
        CHECK(mixed_flags(bundle) == tail_last);
        CHECK(strings(bundle, "filament_mixed_components")[4] == "2,3");
    }
}

// A writer adds a physical slot ahead of the mix to the preset list alone: each array gets the new
// slot's values where the slot is, and the mix keeps its own.
TEST_CASE("A slot added ahead of the mixed tail gets its values there in every array", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(4u, "#FF0000");
    mixed_flags(bundle) = { false, false, false, true };
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values = { "", "", "", "2,3" };
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values           = { "#000000", "#111111", "#222222", "#777777" };
    bundle.project_config.option<ConfigOptionInts>("filament_map")->values                 = { 2, 2, 2, 2 };
    bundle.ams_multi_color_filment = { { "#000000" }, { "#111111" }, { "#222222" }, { "#777777" } };
    REQUIRE(bundle.filament_slots_consistent());

    const std::string added = bundle.filament_presets[2];
    bundle.filament_presets.insert(bundle.filament_presets.begin() + 3, added);
    bundle.normalize_filament_slots();

    CHECK(bundle.filament_slots_consistent());
    CHECK(mixed_flags(bundle) == std::vector<unsigned char>{ false, false, false, false, true });
    CHECK(strings(bundle, "filament_mixed_components")[4] == "2,3");
    CHECK(strings(bundle, "filament_colour") == std::vector<std::string>{ "#000000", "#111111", "#222222", "#00C1AE", "#777777" });
    CHECK(strings(bundle, "filament_multi_colour")[3] == "#00C1AE");
    CHECK(bundle.project_config.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{ 2, 2, 2, 1, 2 });
    CHECK(bundle.ams_multi_color_filment ==
          std::vector<std::vector<std::string>>{ { "#000000" }, { "#111111" }, { "#222222" }, {}, { "#777777" } });
}

// The writers below leave the slots in step on their own, so the normaliser they end with has
// nothing to repair.

TEST_CASE("Loading the saved selections at start-up leaves the slots in step", "[Preset][Bundle][FilamentSlots]")
{
    AppConfig app_config;
    save_three_slot_selection(app_config);
    PresetBundle bundle;
    add_test_presets(bundle);
    bundle.load_selections(app_config);

    REQUIRE(bundle.num_filament_slots() == 3);
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

TEST_CASE("Switching back to a printer's saved selections leaves the slots in step", "[Preset][Bundle][FilamentSlots]")
{
    AppConfig app_config;
    save_three_slot_selection(app_config);
    PresetBundle bundle;
    add_test_presets(bundle);
    bundle.update_selections(app_config);

    REQUIRE(bundle.num_filament_slots() == 3);
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

TEST_CASE("Loading a project leaves the slots in step", "[Preset][Bundle][FilamentSlots]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.opt<ConfigOptionFloats>("filament_diameter")->values.assign(3, 1.75);
    config.opt<ConfigOptionStrings>("filament_colour")->values = { "#FF0000", "#00FF00", "#0000FF" };
    Preset::normalize(config);

    PresetBundle bundle;
    bundle.load_config_model("test.3mf", std::move(config));

    REQUIRE(bundle.num_filament_slots() == 3);
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

TEST_CASE("Syncing the AMS leaves the slots in step", "[Preset][Bundle][FilamentSlots][AMS]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(2u, "#111111");
    const std::string filament_id = add_tray_filament(bundle);
    for (const std::string &colour : { "#123456", "#654321", "#ABCDEF", "#FEDCBA" })
        add_tray(bundle, filament_id, colour);
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                 maps;
    MergeFilamentInfo                                         merge_info;
    // An overwriting sync replaces the two slots with the four trays.
    REQUIRE(bundle.sync_ams_list(unknowns, false, maps, false, merge_info) == 4);

    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

// The sync sets the mix aside and puts it back behind the synced slots. Every array must come back
// with the mix's own values, or a repair would shift the last physical slot's flow type onto the mix.
TEST_CASE("Syncing the AMS keeps each slot's values with a mix behind a High Flow slot", "[Preset][Bundle][FilamentSlots][AMS]")
{
    const int standard  = static_cast<int>(NozzleVolumeType::nvtStandard);
    const int high_flow = static_cast<int>(NozzleVolumeType::nvtHighFlow);
    PresetBundle bundle;
    set_mixed_tail(bundle);
    const std::string filament_id = add_tray_filament(bundle);
    DynamicPrintConfig &project   = bundle.project_config;
    project.option<ConfigOptionStrings>("filament_colour")->values = { "#000000", "#111111", "#222222", "#333333", "#777777" };
    project.option<ConfigOptionInts>("filament_map")->values        = { 1, 1, 2, 2, 1 };
    project.option<ConfigOptionInts>("filament_nozzle_map")->values = { 0, 0, 0, 3, 4 };
    project.option<ConfigOptionInts>("filament_volume_map")->values = { standard, standard, standard, high_flow, standard };
    REQUIRE(bundle.filament_slots_consistent());

    const std::vector<std::string> trays = { "#A00000", "#00A000", "#0000A0", "#A0A000" };
    for (const std::string &colour : trays)
        add_tray(bundle, filament_id, colour);
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                 maps;
    MergeFilamentInfo                                         merge_info;
    REQUIRE(bundle.sync_ams_list(unknowns, false, maps, false, merge_info) == 5);

    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
    CHECK(bundle.filament_slots_consistent());
    CHECK(mixed_flags(bundle) == std::vector<unsigned char>{ false, false, false, false, true });
    CHECK(strings(bundle, "filament_mixed_components") == std::vector<std::string>{ "", "", "", "", "2,3" });
    CHECK(strings(bundle, "filament_mixed_sublayer_ratios")[4] == "0.5,0.5");
    CHECK(strings(bundle, "filament_colour") == std::vector<std::string>{ "#A00000", "#00A000", "#0000A0", "#A0A000", "#777777" });
    const auto ints = [&project](const char *key) { return project.option<ConfigOptionInts>(key)->values; };
    CHECK(ints("filament_volume_map") == std::vector<int>{ standard, standard, standard, high_flow, standard });
    CHECK(ints("filament_nozzle_map") == std::vector<int>{ 0, 0, 0, 3, 4 });
    CHECK(ints("filament_map").size() == 5);
    CHECK(bundle.ams_multi_color_filment.size() == 5);
}
