#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

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

TEST_CASE("filament_slot_keys lists vector options only", "[Preset][Bundle][FilamentSlots]")
{
    for (const std::string &key : PresetBundle::filament_slot_keys()) {
        CAPTURE(key);
        const ConfigOptionDef *def = print_config_def.get(key);
        REQUIRE(def != nullptr);
        // The slot resize visits strings, ints and bools only.
        CHECK((def->type == coStrings || def->type == coInts || def->type == coBools));
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
