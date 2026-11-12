#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <boost/algorithm/string/join.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintConfigCardinality.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

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

// Physical slots named, coloured and flagged as given, then the mixed ones with their components.
void set_slots(PresetBundle &bundle, size_t physical, const std::vector<std::string> &mix_components)
{
    const size_t n = physical + mix_components.size();
    bundle.set_num_filaments(static_cast<unsigned int>(n), "#FFFFFF");
    std::vector<std::string> colours, components(physical);
    for (size_t i = 0; i < n; ++i) {
        bundle.filament_presets[i] = (i < physical ? "P" : "M") + std::to_string(i + 1);
        colours.push_back("#0000" + std::string(i < 10 ? "0" : "") + std::to_string(i));
    }
    components.insert(components.end(), mix_components.begin(), mix_components.end());
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values      = colours;
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values = components;
    mixed_flags(bundle).assign(n, false);
    std::fill(mixed_flags(bundle).begin() + physical, mixed_flags(bundle).end(), true);
}

bool mixed_tail_last(const PresetBundle &bundle)
{
    for (size_t i = 0; i + 1 < bundle.num_filament_slots(); ++i)
        if (bundle.is_mixed_filament(i) && !bundle.is_mixed_filament(i + 1))
            return false;
    return true;
}

// An object of one cube on the given 1-based slot, with one facet painted with paint_state.
ModelObject *add_object_on(Model &model, int slot, int paint_state)
{
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(10., 10., 10.));
    volume->config.set_key_value("extruder", new ConfigOptionInt(slot));
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType(paint_state));
    volume->mmu_segmentation_facets.set_data(selector.serialize());
    return object;
}

bool painted_with(const ModelObject *object, int state)
{
    return TriangleSelector::has_facets(object->volumes.front()->mmu_segmentation_facets.get_data(), EnforcerBlockerType(state));
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
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
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
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
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
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
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
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
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

// The mix moves down with the physical count, and the sync records the move for the owner of the model.
TEST_CASE("Syncing the AMS records where the mixed tail moves", "[Preset][Bundle][FilamentSlots][AMS]")
{
    PresetBundle bundle;
    set_mixed_tail(bundle);
    const std::string filament_id = add_tray_filament(bundle);

    size_t                   trays    = 4;
    std::map<int, int>       expected;
    SECTION("more trays than physical slots") { trays = 6; expected = { { 4, 6 } }; }
    SECTION("fewer trays than physical slots") { trays = 2; expected = { { 4, 2 } }; }
    SECTION("as many trays as physical slots") { trays = 4; }
    for (size_t i = 0; i < trays; ++i)
        add_tray(bundle, filament_id, "#A0000" + std::to_string(i));
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                 maps;
    MergeFilamentInfo                                         merge_info;
    REQUIRE(bundle.sync_ams_list(unknowns, false, maps, false, merge_info) == trays + 1);

    CHECK(mixed_flags(bundle).back());
    CHECK(bundle.take_pending_slot_relocations() == expected);
}

// A slot arrives in the preset list alone, ahead of the mix, so the arrays are a slot short. Each writer counts the
// slots and works on the arrays by position, so it sizes them first.
TEST_CASE("The slot writers put a missing value ahead of the mixed tail before they change the slots", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    bundle.set_num_filaments(4u, "#FF0000");
    mixed_flags(bundle) = { false, false, false, true };
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components")->values = { "", "", "", "2,3" };
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values           = { "#000000", "#111111", "#222222", "#777777" };
    bundle.filament_presets.insert(bundle.filament_presets.begin() + 3, bundle.filament_presets[2]);
    REQUIRE_FALSE(bundle.filament_slots_consistent());

    // The mix stays on its own slot: 4 once the missing value is ahead of it, 5 after an insert at 1, 3 after the slot at 3 goes.
    size_t mix = 4;
    SECTION("set_num_filaments") { bundle.set_num_filaments(6u, "#FF0000"); }
    SECTION("insert_filament_slot") { bundle.insert_filament_slot(1, "#ABCDEF"); mix = 5; }
    SECTION("update_num_filaments") { bundle.update_num_filaments(3); mix = 3; }

    CHECK(bundle.filament_slots_consistent());
    const std::vector<std::string> colours = strings(bundle, "filament_colour");
    REQUIRE(colours.size() > mix);
    CHECK(colours[mix] == "#777777");
    CHECK(mixed_flags(bundle)[mix]);
    CHECK(strings(bundle, "filament_mixed_components")[mix] == "2,3");
}

TEST_CASE("Growing the physical filaments inserts them ahead of the mixed tail and records the moves", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    set_slots(bundle, 2, { "1,2" });
    Model model;
    const ModelObject *on_mix = add_object_on(model, 3, 3);

    bundle.set_num_physical_filaments(4);
    REQUIRE(bundle.num_filament_slots() == 5);
    CHECK(bundle.num_physical_filaments() == 4);
    CHECK(bundle.filament_slots_consistent());
    CHECK(mixed_tail_last(bundle));
    // The new slots copy the last physical preset; the mix keeps its preset, colour and components.
    CHECK(bundle.filament_presets == std::vector<std::string>{ "P1", "P2", "P2", "P2", "M3" });
    CHECK(strings(bundle, "filament_colour")[4] == "#000002");
    CHECK(strings(bundle, "filament_mixed_components")[4] == "1,2");

    const std::map<int, int> moved = bundle.take_pending_slot_relocations();
    CHECK(moved == std::map<int, int>{ { 2, 4 } });
    remap_model_filament_slots(model, moved);
    CHECK(on_mix->volumes.front()->config.extruder() == 5);
    CHECK(painted_with(on_mix, 5));
    CHECK_FALSE(painted_with(on_mix, 3));

    SECTION("and a list that has them already is left alone") {
        bundle.set_num_physical_filaments(4);
        CHECK(bundle.num_filament_slots() == 5);
        CHECK(bundle.take_pending_slot_relocations().empty());
    }
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

TEST_CASE("Shrinking the physical filaments removes the last ones and moves the mixed tail down", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    set_slots(bundle, 4, { "1,4", "2,3" });
    Model model;
    const ModelObject *on_mix     = add_object_on(model, 5, 6);
    const ModelObject *on_removed = add_object_on(model, 3, 3);

    bundle.set_num_physical_filaments(2);
    REQUIRE(bundle.num_filament_slots() == 4);
    CHECK(bundle.num_physical_filaments() == 2);
    CHECK(bundle.filament_slots_consistent());
    CHECK(mixed_tail_last(bundle));
    CHECK(bundle.filament_presets == std::vector<std::string>{ "P1", "P2", "M5", "M6" });
    CHECK(strings(bundle, "filament_colour") == std::vector<std::string>{ "#000000", "#000001", "#000004", "#000005" });
    // A component on a removed filament gets the deleted marker, as a delete gives it.
    CHECK(strings(bundle, "filament_mixed_components") == std::vector<std::string>{ "", "", "1,0", "2,0" });

    const std::map<int, int> moved = bundle.take_pending_slot_relocations();
    CHECK(moved == std::map<int, int>{ { 2, -1 }, { 3, -1 }, { 4, 2 }, { 5, 3 } });
    remap_model_filament_slots(model, moved);
    CHECK(on_mix->volumes.front()->config.extruder() == 3);
    CHECK(painted_with(on_mix, 4));
    CHECK_FALSE(painted_with(on_mix, 6));
    // A removed filament's objects go to the first one and its paint is cleared, as a delete does.
    CHECK(on_removed->volumes.front()->config.extruder() == 1);
    CHECK(on_removed->volumes.front()->mmu_segmentation_facets.empty());

    SECTION("but not below one filament") {
        bundle.set_num_physical_filaments(0);
        CHECK(bundle.num_physical_filaments() == 1);
        CHECK(bundle.filament_slots_consistent());
        CHECK(mixed_tail_last(bundle));
    }
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

TEST_CASE("A tool changer shrunk below its nozzle count gets the missing slots back ahead of the mixed tail", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    set_slots(bundle, 3, { "1,2" });
    set_nozzle_count(bundle, 2);

    bundle.set_num_physical_filaments(1);
    CHECK(bundle.num_physical_filaments() == 2);
    CHECK(bundle.num_filament_slots() == 3);
    CHECK(bundle.filament_slots_consistent());
    CHECK(mixed_tail_last(bundle));
    // The removed filaments are gone, the mix moved down to behind the slot the top-up put back.
    CHECK(bundle.take_pending_slot_relocations() == std::map<int, int>{ { 1, -1 }, { 2, -1 }, { 3, 2 } });
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

// A slot array a value short: the value is a physical slot's, as the normaliser takes it, so the mix keeps
// its own value whether the physical count then grows or shrinks.
TEST_CASE("The physical count call puts a missing value ahead of the mixed tail before the count changes", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    set_slots(bundle, 2, { "1,2" });
    const int  standard   = static_cast<int>(NozzleVolumeType::nvtStandard);
    const int  high_flow  = static_cast<int>(NozzleVolumeType::nvtHighFlow);
    const auto volume_map = [&bundle]() { return bundle.project_config.option<ConfigOptionInts>("filament_volume_map")->values; };
    bundle.project_config.option<ConfigOptionInts>("filament_volume_map")->values = { standard, high_flow };

    SECTION("growing") {
        bundle.set_num_physical_filaments(3);
        CHECK(volume_map() == std::vector<int>{ standard, standard, standard, high_flow });
    }
    SECTION("shrinking") {
        bundle.set_num_physical_filaments(1);
        CHECK(volume_map() == std::vector<int>{ standard, high_flow });
    }
    CHECK(bundle.filament_slot_repairs() == 1);
    CHECK(bundle.filament_slots_consistent());
}

TEST_CASE("An imported object naming a mixed slot gets a physical filament of its own", "[Preset][Bundle][FilamentSlots]")
{
    // A geometry-only import names its source's filaments, which are physical. The project grows its
    // physical filaments to the highest id the import names, ahead of the mixed tail; the objects
    // already in the project follow their mixes, and the imported ones keep their ids.
    PresetBundle bundle;
    Model        project;
    const auto import_up_to = [&bundle, &project](size_t highest_id) {
        if (highest_id > bundle.num_physical_filaments())
            bundle.set_num_physical_filaments(highest_id);
        remap_model_filament_slots(project, bundle.take_pending_slot_relocations());
    };

    SECTION("one mixed slot") {
        set_slots(bundle, 2, { "1,2" });
        const ModelObject *on_mix = add_object_on(project, 3, 3);
        import_up_to(3);
        REQUIRE(bundle.num_filament_slots() == 4);
        CHECK_FALSE(bundle.is_mixed_filament(2));
        CHECK(on_mix->volumes.front()->config.extruder() == 4);
        CHECK(bundle.is_mixed_filament(3));
        CHECK(strings(bundle, "filament_mixed_components")[3] == "1,2");
        CHECK(bundle.filament_slots_consistent());
        CHECK(mixed_tail_last(bundle));
    }
    SECTION("two mixed slots") {
        set_slots(bundle, 2, { "1,2", "2,1" });
        const ModelObject *on_first  = add_object_on(project, 3, 3);
        const ModelObject *on_second = add_object_on(project, 4, 4);
        import_up_to(3);
        REQUIRE(bundle.num_filament_slots() == 5);
        CHECK_FALSE(bundle.is_mixed_filament(2));
        CHECK(on_first->volumes.front()->config.extruder() == 4);
        CHECK(on_second->volumes.front()->config.extruder() == 5);
        CHECK(painted_with(on_second, 5));
        CHECK(strings(bundle, "filament_mixed_components") == std::vector<std::string>{ "", "", "", "1,2", "2,1" });
        CHECK(bundle.filament_slots_consistent());
        CHECK(mixed_tail_last(bundle));
    }
    SECTION("ids within the physical filaments add nothing") {
        set_slots(bundle, 2, { "1,2" });
        const ModelObject *on_mix = add_object_on(project, 3, 3);
        import_up_to(2);
        CHECK(bundle.num_filament_slots() == 3);
        CHECK(on_mix->volumes.front()->config.extruder() == 3);
    }
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

TEST_CASE("set_num_filaments refuses to cut across the mixed tail", "[Preset][Bundle][FilamentSlots]")
{
    PresetBundle bundle;
    set_slots(bundle, 3, { "1,2" });

    bundle.set_num_filaments(2u);
    CHECK(bundle.num_filament_slots() == 4);
    CHECK(bundle.is_mixed_filament(3));
    CHECK(bundle.filament_slots_consistent());

    SECTION("but still appends a slot for a new mix") {
        bundle.set_num_filaments(5u);
        CHECK(bundle.num_filament_slots() == 5);
        CHECK(bundle.filament_slots_consistent());
    }
    INFO(describe(bundle.last_filament_slot_repair()));
    CHECK(bundle.filament_slot_repairs() == 0);
}

TEST_CASE("A plate's filament maps take an inserted slot's defaults at its position", "[Preset][Bundle][FilamentSlots]")
{
    DynamicPrintConfig plate;
    plate.set_key_value("filament_map", new ConfigOptionInts{ 2, 2, 2 });
    plate.set_key_value("filament_nozzle_map", new ConfigOptionInts{ 1, 1, 1 });
    plate.set_key_value("filament_volume_map", new ConfigOptionInts{ 1, 1, 1 });
    const auto ints = [&plate](const char *key) { return plate.option<ConfigOptionInts>(key)->values; };

    // A physical slot inserted ahead of a mixed tail of one: the mix keeps its values behind it.
    insert_plate_filament_slot(plate, 2, 0);
    CHECK(ints("filament_map") == std::vector<int>{ 2, 2, 1, 2 });
    CHECK(ints("filament_nozzle_map") == std::vector<int>{ 1, 1, 0, 1 });
    CHECK(ints("filament_volume_map") == std::vector<int>{ 1, 1, 0, 1 });

    SECTION("a map shorter than the position takes it at its end") {
        insert_plate_filament_slot(plate, 9, 0);
        CHECK(ints("filament_map") == std::vector<int>{ 2, 2, 1, 2, 1 });
    }
    SECTION("a map the plate does not have stays absent") {
        plate.erase("filament_nozzle_map");
        insert_plate_filament_slot(plate, 0, 0);
        CHECK_FALSE(plate.has("filament_nozzle_map"));
        CHECK(ints("filament_map").front() == 1);
    }
}

TEST_CASE("Reordering the filament slots moves every per-filament value and renumbers the references", "[Preset][Bundle][FilamentSlots]")
{
    // Four slots, the third a mix of the first two with two variants of its own, as the CLI assembles
    // a project of three slots loaded with a fourth filament.
    DynamicPrintConfig config;
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{ 0.4 });
    config.set_key_value("filament_type", new ConfigOptionStrings{ "PLA", "PETG", "MIX", "ABS" });
    config.set_key_value("filament_is_mixed", new ConfigOptionBools{ false, false, true, false });
    config.set_key_value("filament_mixed_components", new ConfigOptionStrings{ "", "", "1,2", "" });
    config.set_key_value("filament_settings_id", new ConfigOptionStrings{ "a", "b", "m", "d" });
    config.set_key_value("different_settings_to_system", new ConfigOptionStrings{ "process", "a", "b", "m", "d", "printer" });
    config.set_key_value("filament_self_index", new ConfigOptionInts{ 1, 2, 3, 3, 4 });
    config.set_key_value("filament_extruder_variant", new ConfigOptionStrings{ "a", "b", "m1", "m2", "d" });
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{ 1.0, 1.1, 1.2, 1.25, 1.3 });
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats{ 0, 1, 10, 11, 20, 21, 30, 31 });
    std::vector<double> matrix;
    for (int from = 0; from < 4; ++from)
        for (int to = 0; to < 4; ++to)
            matrix.push_back(10 * from + to);
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats(matrix));
    config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(3));
    config.set_key_value("support_filament", new ConfigOptionInt(4));
    config.set_key_value("sparse_infill_filament_id", new ConfigOptionInt(0));
    REQUIRE(cardinality_of("filament_type") == ConfigCardinality::PerFilament);
    REQUIRE(cardinality_of("filament_flow_ratio") == ConfigCardinality::PerFilamentVariant);

    SECTION("the mix moves behind the extra filament") {
        reorder_filament_slots(config, { 0, 1, 3, 2 });
        CHECK(config.option<ConfigOptionStrings>("filament_type")->values == std::vector<std::string>{ "PLA", "PETG", "ABS", "MIX" });
        CHECK(config.option<ConfigOptionBools>("filament_is_mixed")->values == std::vector<unsigned char>{ 0, 0, 0, 1 });
        CHECK(config.option<ConfigOptionStrings>("filament_mixed_components")->values == std::vector<std::string>{ "", "", "", "1,2" });
        CHECK(config.option<ConfigOptionStrings>("filament_settings_id")->values == std::vector<std::string>{ "a", "b", "d", "m" });
        CHECK(config.option<ConfigOptionStrings>("different_settings_to_system")->values ==
              std::vector<std::string>{ "process", "a", "b", "d", "m", "printer" });
        CHECK(config.option<ConfigOptionInts>("filament_self_index")->values == std::vector<int>{ 1, 2, 3, 4, 4 });
        CHECK(config.option<ConfigOptionStrings>("filament_extruder_variant")->values == std::vector<std::string>{ "a", "b", "d", "m1", "m2" });
        CHECK(config.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{ 1.0, 1.1, 1.3, 1.2, 1.25 });
        CHECK(config.option<ConfigOptionFloats>("flush_volumes_vector")->values == std::vector<double>{ 0, 1, 10, 11, 30, 31, 20, 21 });
        // The pair (ABS, MIX) is now (3, 4): it holds what the pair (4, 3) held.
        CHECK(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values[2 * 4 + 3] == 32);
        CHECK(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values[0 * 4 + 2] == 3);
        CHECK(config.opt_int("outer_wall_filament_id") == 4);
        CHECK(config.opt_int("support_filament") == 3);
        CHECK(config.opt_int("sparse_infill_filament_id") == 0);
    }
    SECTION("a mix follows the filaments it is made of") {
        reorder_filament_slots(config, { 1, 0, 2, 3 });
        CHECK(config.option<ConfigOptionStrings>("filament_mixed_components")->values[2] == "2,1");
        CHECK(config.option<ConfigOptionStrings>("filament_settings_id")->values == std::vector<std::string>{ "b", "a", "m", "d" });
    }
    SECTION("a list of another length is left as it is") {
        config.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000", "#00FF00" });
        reorder_filament_slots(config, { 0, 1, 3, 2 });
        CHECK(config.option<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{ "#FF0000", "#00FF00" });
    }
}

TEST_CASE("Moving the slots re-points every reference the model makes to them", "[Preset][Bundle][FilamentSlots]")
{
    // An object naming slot 3 (a mix, say) every way a model can, and slot 2 as a control.
    Model        model;
    ModelObject *object = add_object_on(model, 3, 3);
    ModelVolume *volume = object->volumes.front();
    object->config.set("extruder", 3);
    object->config.set("sparse_infill_filament_id", 3);
    object->config.set("support_filament", 3);
    object->config.set("support_interface_filament", 2);
    volume->config.set("inner_wall_filament_id", 3);
    ModelConfig &range = object->layer_config_ranges[t_layer_height_range(2., 4.)];
    range.set("extruder", 3);
    range.set("top_surface_filament_id", 3);
    std::vector<CustomGCode::Item> &gcodes = model.plates_custom_gcodes[0].gcodes;
    gcodes.push_back({ 1., CustomGCode::ToolChange, 3, "", "" });
    gcodes.push_back({ 2., CustomGCode::ToolChange, 1, "", "" });

    SECTION("a moved slot is followed everywhere") {
        remap_model_filament_slots(model, { { 2, 3 } });
        CHECK(object->config.extruder() == 4);
        CHECK(object->config.opt_int("sparse_infill_filament_id") == 4);
        CHECK(object->config.opt_int("support_filament") == 4);
        CHECK(object->config.opt_int("support_interface_filament") == 2);
        CHECK(volume->config.extruder() == 4);
        CHECK(volume->config.opt_int("inner_wall_filament_id") == 4);
        CHECK(painted_with(object, 4));
        CHECK(range.extruder() == 4);
        CHECK(range.opt_int("top_surface_filament_id") == 4);
        REQUIRE(gcodes.size() == 2);
        CHECK(gcodes[0].extruder == 4);
        CHECK(gcodes[1].extruder == 1);
    }
    SECTION("a removed slot is dropped as a delete without a replacement drops it") {
        remap_model_filament_slots(model, { { 2, -1 } });
        CHECK(object->config.extruder() == 1);
        CHECK(volume->config.extruder() == 1);
        CHECK_FALSE(object->config.has("sparse_infill_filament_id"));
        CHECK_FALSE(object->config.has("support_filament"));
        CHECK(object->config.opt_int("support_interface_filament") == 2);
        CHECK_FALSE(volume->config.has("inner_wall_filament_id"));
        CHECK(volume->mmu_segmentation_facets.empty());
        CHECK_FALSE(range.has("extruder"));
        CHECK_FALSE(range.has("top_surface_filament_id"));
        REQUIRE(gcodes.size() == 1);
        CHECK(gcodes[0].extruder == 1);
    }
}

TEST_CASE("Reordering the filament slots renumbers a plate's print sequences", "[Preset][Bundle][FilamentSlots]")
{
    // A plate of four slots whose third is the mix: the first layer's order lists filaments, each group
    // of the other layers' holds a layer range and then filaments.
    DynamicPrintConfig plate;
    plate.set_key_value("filament_map", new ConfigOptionInts{ 1, 2, 1, 2 });
    plate.set_key_value("first_layer_print_sequence", new ConfigOptionInts{ 3, 1, 2, 4 });
    plate.set_key_value("other_layers_print_sequence", new ConfigOptionInts{ 2, 5, 3, 1, 2, 4, 6, 9, 4, 3, 2, 1 });
    plate.set_key_value("other_layers_print_sequence_nums", new ConfigOptionInt(2));

    reorder_filament_slots(plate, { 0, 1, 3, 2 });
    CHECK(plate.option<ConfigOptionInts>("filament_map")->values == std::vector<int>{ 1, 2, 2, 1 });
    CHECK(plate.option<ConfigOptionInts>("first_layer_print_sequence")->values == std::vector<int>{ 4, 1, 2, 3 });
    // The layer ranges stay; the filaments follow their slots.
    CHECK(plate.option<ConfigOptionInts>("other_layers_print_sequence")->values == std::vector<int>{ 2, 5, 4, 1, 2, 3, 6, 9, 3, 4, 2, 1 });
}

// ============================================================================
// Slot arrays of the wrong length: each writer of the slot list starts from, or is handed, slot arrays
// that are empty, one entry, one short or one long, and must leave every one at the slot count.
// ============================================================================

namespace {

// Each slot array's length next to the slot count, for a failed consistency check.
std::string slot_lengths(const PresetBundle &bundle)
{
    std::string out = "slots " + std::to_string(bundle.num_filament_slots());
    for (const std::string &key : PresetBundle::filament_slot_keys()) {
        const auto *opt = dynamic_cast<const ConfigOptionVectorBase *>(bundle.project_config.option(key));
        out += ", " + key + " " + (opt != nullptr ? std::to_string(opt->size()) : std::string("absent"));
    }
    return out + ", ams_multi_color_filment " + std::to_string(bundle.ams_multi_color_filment.size());
}

// Resize one slot array of the bundle, the AMS colour list included, to `len` entries.
void resize_slot_array(PresetBundle &bundle, const std::string &key, size_t len)
{
    if (key == "ams_multi_color_filment")
        bundle.ams_multi_color_filment.resize(len, { "#FF0000" });
    else
        static_cast<ConfigOptionVectorBase *>(bundle.project_config.option(key))->resize(len);
}

// A printer and a filament preset for load_selections to find.
void add_selection_presets(PresetBundle &bundle)
{
    bundle.printers.load_preset(std::string(), "Test Printer", DynamicPrintConfig(bundle.printers.default_preset().config), false);
    bundle.filaments.load_preset(std::string(), "Filament A", DynamicPrintConfig(bundle.filaments.default_preset().config), false);
}

std::vector<std::string> slot_arrays_and_ams_list()
{
    std::vector<std::string> keys = PresetBundle::filament_slot_keys();
    keys.emplace_back("ams_multi_color_filment");
    return keys;
}

// Load `trays` AMS trays of one PLA and sync the bundle onto them, overwriting the slots or writing their colours only.
void sync_onto_trays(PresetBundle &bundle, int trays, bool colour_only)
{
    for (int slot = 0; slot < trays; ++slot) {
        DynamicPrintConfig tray;
        tray.set_key_value("filament_id", new ConfigOptionStrings{ "GFA00" });
        tray.set_key_value("filament_colour", new ConfigOptionStrings{ "#FF0000" });
        tray.set_key_value("filament_colour_type", new ConfigOptionStrings{ "1" });
        tray.set_key_value("filament_multi_colour", new ConfigOptionStrings{});
        tray.set_key_value("ams_id", new ConfigOptionStrings{ "0" });
        tray.set_key_value("slot_id", new ConfigOptionStrings{ std::to_string(slot) });
        tray.set_key_value("filament_type", new ConfigOptionStrings{ "PLA" });
        bundle.filament_ams_list.emplace(slot, std::move(tray));
    }
    std::vector<std::pair<DynamicPrintConfig *, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                 maps;
    MergeFilamentInfo                                         merge_info;
    bundle.sync_ams_list(unknowns, false, maps, true, merge_info, colour_only);
}

} // namespace

// Grow, shrink and insert run every slot array through one resize, and the physical count call through the
// normaliser, so they repair a mis-sized one.
TEST_CASE("Growing, shrinking or inserting a slot brings a slot array of the wrong length back to the slot count", "[Preset][Bundle][FilamentSlots][MismatchSweep]")
{
    struct Writer { const char *name; void (*run)(PresetBundle &); };
    const Writer writers[] = {
        { "set_num_filaments(4)",          [](PresetBundle &b) { b.set_num_filaments(4u); } },
        { "set_num_filaments(2)",          [](PresetBundle &b) { b.set_num_filaments(2u); } },
        { "insert_filament_slot(1)",       [](PresetBundle &b) { b.insert_filament_slot(1); } },
        { "set_num_physical_filaments(4)", [](PresetBundle &b) { b.set_num_physical_filaments(4); } },
        { "set_num_physical_filaments(2)", [](PresetBundle &b) { b.set_num_physical_filaments(2); } },
    };
    for (const std::string &key : slot_arrays_and_ams_list())
        for (size_t len : { 0, 1, 2, 4 })
            for (const Writer &writer : writers)
                DYNAMIC_SECTION(key << " at " << len << " of 3, then " << writer.name) {
                    PresetBundle bundle;
                    bundle.set_num_filaments(3u, "#FF0000");
                    resize_slot_array(bundle, key, len);
                    writer.run(bundle);
                    INFO(slot_lengths(bundle));
                    CHECK(bundle.filament_slots_consistent());
                }
}

// update_num_filaments erases one entry from each array and ends in the top-up, whose normalise brings an array that
// was the wrong length back to the slot count.
TEST_CASE("Deleting a slot brings a slot array of the wrong length back to the slot count", "[Preset][Bundle][FilamentSlots][MismatchSweep]")
{
    for (const std::string &key : slot_arrays_and_ams_list())
        for (size_t len : { 0, 1, 2, 4 })
            for (unsigned int deleted : { 0u, 1u, 2u })
                DYNAMIC_SECTION(key << " at " << len << " of 3, then update_num_filaments(" << deleted << ")") {
                    PresetBundle bundle;
                    bundle.set_num_filaments(3u, "#FF0000");
                    resize_slot_array(bundle, key, len);
                    bundle.update_num_filaments(deleted);
                    INFO(slot_lengths(bundle));
                    CHECK(bundle.filament_slots_consistent());
                }
}

// The project load sizes every slot array to the file's filament count, read from filament_colour, and normalises
// before it returns.
TEST_CASE("Loading a project with a short, long or absent slot array leaves every slot array at the slot count", "[Preset][Bundle][FilamentSlots][MismatchSweep]")
{
    for (const std::string &key : PresetBundle::filament_slot_keys())
        for (int len : { -1, 0, 1, 2, 4 }) // -1: the project does not carry the key
            DYNAMIC_SECTION(key << (len < 0 ? std::string(" absent") : " at " + std::to_string(len) + " of 3")) {
                DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
                config.option<ConfigOptionFloats>("filament_diameter", true)->values = { 1.75, 1.75, 1.75 };
                config.option<ConfigOptionStrings>("filament_colour", true)->values  = { "#FF0000", "#00FF00", "#0000FF" };
                for (const std::string &slot_key : PresetBundle::filament_slot_keys())
                    if (slot_key != "filament_colour")
                        static_cast<ConfigOptionVectorBase *>(config.option(slot_key, true))
                            ->resize(3, print_config_def.get(slot_key)->default_value.get());
                if (len < 0)
                    config.erase(key);
                else
                    static_cast<ConfigOptionVectorBase *>(config.option(key))->resize(size_t(len));

                PresetBundle bundle;
                // The filament count is read from filament_colour, and a project without one is refused.
                if (key == "filament_colour" && len <= 0) {
                    CHECK_THROWS(bundle.load_config_model("test.3mf", std::move(config)));
                } else {
                    bundle.load_config_model("test.3mf", std::move(config));
                    INFO(slot_lengths(bundle));
                    CHECK(bundle.filament_slots_consistent());
                }
            }
}

// load_selections sizes the restored slot arrays to its preset list, the AMS colour list and a saved colour-pack list
// of another length included.
TEST_CASE("Restoring saved selections leaves every slot array at the slot count", "[Preset][Bundle][FilamentSlots][MismatchSweep]")
{
    for (const char *setting : { "filament_colors", "filament_multi_colors", "filament_color_types" })
        for (size_t len : { 1, 2, 3, 4 })
            DYNAMIC_SECTION(setting << " with " << len << " entries for 3 slots") {
                PresetBundle bundle;
                add_selection_presets(bundle);
                AppConfig app_config;
                app_config.set("presets", PRESET_PRINTER_NAME, "Test Printer");
                app_config.set_printer_setting("Test Printer", PRESET_FILAMENT_NAME, "Filament A");
                app_config.set_printer_setting("Test Printer", "filament_01", "Filament A");
                app_config.set_printer_setting("Test Printer", "filament_02", "Filament A");
                const std::vector<std::string> values(len, std::string(setting) == "filament_color_types" ? "1" : "#FF0000");
                app_config.set_printer_setting("Test Printer", setting, boost::algorithm::join(values, ","));

                bundle.load_selections(app_config);
                INFO(slot_lengths(bundle));
                CHECK(bundle.num_filament_slots() == 3);
                CHECK(bundle.filament_slots_consistent());
            }
}

// The AMS sync sizes every slot array to the synced slots, or writes a colour-only sync into the slots there are, and
// normalises before it returns.
TEST_CASE("An AMS sync onto a slot array of the wrong length leaves every slot array at the slot count", "[Preset][Bundle][FilamentSlots][MismatchSweep]")
{
    for (const std::string &key : slot_arrays_and_ams_list())
        for (size_t len : { 0, 1, 2, 4 })
            for (int trays : { 2, 3, 4 })
                for (bool colour_only : { false, true }) {
                    DYNAMIC_SECTION(key << " at " << len << " of 3, then a " << (colour_only ? "colour-only " : "") << "sync of " << trays << " trays") {
                        PresetBundle bundle;
                        bundle.set_num_filaments(3u, "#111111");
                        resize_slot_array(bundle, key, len);
                        sync_onto_trays(bundle, trays, colour_only);
                        INFO(slot_lengths(bundle));
                        CHECK(bundle.filament_slots_consistent());
                    }
                }
}
