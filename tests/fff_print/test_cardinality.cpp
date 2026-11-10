// Cardinality through the real Print::apply -> process path. Print::apply runs enforce_cardinality, which sizes
// every per-N array (per-extruder printable height, machine limits, filament_map, ...) to its declared cardinality,
// so a multi-extruder object slices; the mismatch sweep below feeds it arrays of the wrong length.
#include <catch2/catch_all.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Config.hpp>
#include <libslic3r/Point.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfigCardinality.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <cstddef>
#include <exception>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("enforce_cardinality sizes per-N arrays so a multi-extruder object slices", "[Cardinality]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    config.set_key_value("gcode_flavor",               new ConfigOptionEnum<GCodeFlavor>(gcfMarlinFirmware));
    config.set_key_value("layer_height",               new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_line_width",   new ConfigOptionFloatOrPercent(0, false));
    config.set_key_value("z_hop",                       new ConfigOptionFloats({0}));
    // Sequential print so each object uses its own extruder without a wipe tower.
    config.set_key_value("print_sequence",             new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

    // Clear every custom G-code block so the end-of-export placeholder-def check
    // (ORCA_CHECK_GCODE_PLACEHOLDERS) has nothing to complain about. This isolates
    // the per-extruder sizing behaviour from unrelated placeholder validation.
    config.set_key_value("machine_start_gcode",        new ConfigOptionString(""));
    config.set_key_value("machine_end_gcode",          new ConfigOptionString(""));
    config.set_key_value("before_layer_change_gcode",  new ConfigOptionString(""));
    config.set_key_value("layer_change_gcode",         new ConfigOptionString(""));
    config.set_key_value("change_filament_gcode",      new ConfigOptionString(""));
    config.set_key_value("time_lapse_gcode",           new ConfigOptionString(""));
    config.set_key_value("printing_by_object_gcode",   new ConfigOptionString(""));
    config.set_key_value("filament_start_gcode",       new ConfigOptionStrings({"", ""}));
    config.set_key_value("filament_end_gcode",         new ConfigOptionStrings({"", ""}));

    config.set_key_value("nozzle_diameter",          new ConfigOptionFloats({0.4, 0.4}));
    config.set_key_value("printer_extruder_id",      new ConfigOptionInts({1, 2}));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
    config.set_key_value("filament_diameter",        new ConfigOptionFloats({1.75, 1.75}));
    config.set_key_value("filament_colour",          new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("filament_type",            new ConfigOptionStrings({"PLA", "PLA"}));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map",             new ConfigOptionInts({1, 2}));
    config.set_key_value("default_filament_colour",  new ConfigOptionStrings({"#FF0000", "#00FF00"}));
    config.set_key_value("nozzle_temperature",       new ConfigOptionInts({210, 210}));
    config.set_key_value("nozzle_temperature_range_low",  new ConfigOptionInts({190, 190}));
    config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({240, 240}));
    config.set_key_value("flush_multiplier",     new ConfigOptionFloats({1}));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats({0, 0, 0, 0}));

    // Stride-2 (normal, silent) machine limits per extruder.
    config.set_key_value("machine_max_acceleration_x",          new ConfigOptionFloats({500, 0, 1000, 0}));
    config.set_key_value("machine_max_acceleration_y",          new ConfigOptionFloats({700, 0, 1100, 0}));
    config.set_key_value("machine_max_acceleration_z",          new ConfigOptionFloats({100, 0, 300, 0}));
    config.set_key_value("machine_max_acceleration_e",          new ConfigOptionFloats({5000, 0, 8000, 0}));
    config.set_key_value("machine_max_acceleration_extruding",  new ConfigOptionFloats({1200, 0, 2200, 0}));
    config.set_key_value("machine_max_acceleration_retracting", new ConfigOptionFloats({1400, 0, 2400, 0}));
    config.set_key_value("machine_max_acceleration_travel",     new ConfigOptionFloats({1600, 0, 2600, 0}));
    config.set_key_value("machine_max_speed_x",  new ConfigOptionFloats({100, 0, 200, 0}));
    config.set_key_value("machine_max_speed_y",  new ConfigOptionFloats({110, 0, 210, 0}));
    config.set_key_value("machine_max_speed_z",  new ConfigOptionFloats({10, 0, 30, 0}));
    config.set_key_value("machine_max_speed_e",  new ConfigOptionFloats({50, 0, 80, 0}));
    config.set_key_value("machine_max_jerk_x",   new ConfigOptionFloats({8, 0, 12, 0}));
    config.set_key_value("machine_max_jerk_y",   new ConfigOptionFloats({9, 0, 13, 0}));
    config.set_key_value("machine_max_jerk_z",   new ConfigOptionFloats({0.4, 0, 0.6, 0}));
    config.set_key_value("machine_max_jerk_e",   new ConfigOptionFloats({5, 0, 10, 0}));
    config.set_key_value("machine_max_junction_deviation", new ConfigOptionFloats({0.02, 0, 0.05, 0}));

    Model model;
    auto *obj1 = model.add_object();
    obj1->add_volume(cube(20));
    obj1->add_instance();

    auto *obj2 = model.add_object();
    obj2->add_volume(cube(20));
    obj2->add_instance();
    obj2->config.set_key_value("extruder", new ConfigOptionInt(2));

    Print print;
    arrange_objects(model, InfiniteBed{}, ArrangeParams{scaled(min_object_distance(config))});
    for (auto *mo : model.objects) {
        mo->ensure_on_bed();
        print.auto_assign_extruders(mo);
    }

    // apply() runs enforce_cardinality, sizing every per-N array to its cardinality; the slice then completes.
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 2);
    REQUIRE(print.objects().front()->layer_count() > 0);
}

namespace {

// Two filaments on two direct-drive extruders, each extruder with a Standard and a High Flow variant. The variant
// options are laid out unexpanded, as the preset bundle hands them to Print::apply: two columns per extruder for the
// printer and process options, two per filament for the filament options.
DynamicPrintConfig two_extruder_variant_config()
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "layer_height",               0.2      },
        { "initial_layer_print_height", 0.2      },
        { "skirt_loops",                0        } });
    config.set_num_extruders(2);
    config.set_deserialize_strict({
        { "single_extruder_multi_material", false },
        { "nozzle_diameter",                "0.4,0.4" },
        { "extruder_type",                  "Direct Drive,Direct Drive" },
        { "nozzle_volume_type",             "Standard,Standard" },
        { "extruder_printable_area",        "0x0,200x0,200x200,0x200#0x0,200x0,200x200,0x200" },
        { "extruder_printable_height",      "100,100" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2" } });
    size_flush_to_nozzles(config);

    const std::vector<std::string> variants = { "Direct Drive Standard", "Direct Drive High Flow" };
    const auto columns = [&variants, &config](int owners, const char *variant_key, const char *id_key) {
        std::vector<std::string> names;
        std::vector<int>         ids;
        for (int owner = 1; owner <= owners; ++owner)
            for (const std::string &variant : variants) {
                names.push_back(variant);
                ids.push_back(owner);
            }
        config.option<ConfigOptionStrings>(variant_key, true)->values = names;
        config.option<ConfigOptionInts>(id_key, true)->values         = ids;
    };
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values = std::vector<std::string>(2, variants[0] + "," + variants[1]);
    columns(2, "printer_extruder_variant", "printer_extruder_id");
    columns(2, "print_extruder_variant", "print_extruder_id");
    columns(2, "filament_extruder_variant", "filament_self_index");

    // Every per-N option at the length its cardinality declares, so a case cuts one option to empty and nothing else.
    const AxisCounts n = axis_counts(config);
    for (const auto &[key, def] : print_config_def.options)
        if (def.default_value && def.default_value->is_vector() && expected_size(cardinality_of(key), n) != 0) {
            config.option(key, true);
            resize_to_cardinality(config, key, n);
        }
    return config;
}

// One 5 mm cube per extruder.
Model two_extruder_model()
{
    Model model;
    for (int extruder = 1; extruder <= 2; ++extruder) {
        TriangleMesh mesh = make_cube(5., 5., 5.);
        mesh.translate(80.f + 15.f * static_cast<float>(extruder), 150.f, 0.f);
        ModelObject *object = model.add_object();
        object->add_volume(std::move(mesh));
        object->add_instance();
        object->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    }
    return model;
}

struct EmptyCase
{
    const char *key;
    size_t      size_after_apply;
};

} // namespace

TEST_CASE("Print::apply sizes an empty variant-expansion input instead of crashing in the expansion", "[Cardinality]")
{
    // Each key is one the expansion reads through get_at() before anything has sized it: a per-extruder-variant,
    // a dual (normal + silent) per-extruder-variant and a per-process-variant option, the extruder and nozzle
    // volume types, the variant list and the variant id and name columns. One section per key, so `-c "<key>"` runs
    // one alone.
    const std::vector<EmptyCase> cases = {
        { "retraction_length", 2 }, { "machine_max_acceleration_x", 4 }, { "outer_wall_speed", 2 },
        { "extruder_type", 2 }, { "nozzle_volume_type", 2 }, { "extruder_variant_list", 2 },
        { "printer_extruder_variant", 2 }, { "printer_extruder_id", 2 }, { "print_extruder_variant", 2 },
        { "print_extruder_id", 2 },
    };
    for (const EmptyCase &c : cases) {
        DYNAMIC_SECTION(c.key) {
            DynamicPrintConfig config = two_extruder_variant_config();
            auto *opt = dynamic_cast<ConfigOptionVectorBase *>(config.option(c.key));
            REQUIRE(opt != nullptr);
            REQUIRE(opt->size() >= 2);
            opt->resize(0);

            Print print;
            print.apply(two_extruder_model(), config);

            const auto *applied = dynamic_cast<const ConfigOptionVectorBase *>(print.full_print_config().option(c.key));
            REQUIRE(applied != nullptr);
            CHECK(applied->size() == c.size_after_apply);
        }
    }
}

TEST_CASE("size_empty_expansion_inputs leaves a config with no empty option unchanged", "[Cardinality]")
{
    const DynamicPrintConfig before = two_extruder_variant_config();
    DynamicPrintConfig       after  = before;
    after.size_empty_expansion_inputs();
    CHECK(after == before);
}

TEST_CASE("An empty filament_self_index still gives each filament the values of its own variant column", "[Cardinality]")
{
    // A regression guard, not a fail-before test: it passes without the sizing too. An empty index must stay empty
    // through the expansion, which then finds each filament's column from the variant list's own order; filling it
    // with the schema default would assign every column to filament 1 and filament 2 would read filament 1's column.
    DynamicPrintConfig config = two_extruder_variant_config();
    // Columns are filament 1 Standard, filament 1 High Flow, filament 2 Standard, filament 2 High Flow.
    config.option<ConfigOptionFloats>("filament_flow_ratio", true)->values = { 0.90, 0.91, 0.95, 0.96 };
    config.option<ConfigOptionInts>("filament_self_index", true)->values.clear();

    Print print;
    print.apply(two_extruder_model(), config);

    const auto *flow_ratio = print.full_print_config().option<ConfigOptionFloats>("filament_flow_ratio");
    REQUIRE(flow_ratio != nullptr);
    REQUIRE(flow_ratio->values.size() == 2);
    CHECK_THAT(flow_ratio->values[0], Catch::Matchers::WithinAbs(0.90, 1e-9));
    CHECK_THAT(flow_ratio->values[1], Catch::Matchers::WithinAbs(0.95, 1e-9));
}

TEST_CASE("A filament_self_index shorter than the variant list does not stop a multi-extruder slice", "[Cardinality]")
{
    // The filament variant list names four columns, and the list that says which filament owns each is shorter.
    DynamicPrintConfig config = two_extruder_variant_config();
    REQUIRE(config.option<ConfigOptionStrings>("filament_extruder_variant")->values.size() == 4);
    std::vector<int> &self_index = config.option<ConfigOptionInts>("filament_self_index", true)->values;
    REQUIRE(self_index.size() == 4);

    SECTION("empty") { self_index.clear(); }
    SECTION("one column short") { self_index.pop_back(); }

    Print print;
    print.apply(two_extruder_model(), config);
    print.set_status_silent();
    print.process();

    REQUIRE(print.objects().size() == 2);
    CHECK(print.objects().front()->layer_count() > 0);
}

namespace {

// Two 20 mm cubes on two filaments of one nozzle, with a prime tower between the filament changes.
DynamicPrintConfig tower_config()
{
    return multifilament_config(2, {
        { "enable_prime_tower", "1"        },
        { "wipe_tower_x",       "50"       },
        { "wipe_tower_y",       "50"       },
        { "layer_change_gcode", "G92 E0\n" } });
}

const std::vector<std::vector<ConfigBase::SetDeserializeItem>> two_cube_extruders{ { { "extruder", "1" } }, { { "extruder", "2" } } };

} // namespace

TEST_CASE("An empty flush_multiplier is sized at apply so the export does not divide by its length", "[Cardinality]")
{
    DynamicPrintConfig config = tower_config();
    config.option<ConfigOptionFloats>("flush_multiplier", true)->values.clear();

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, config, &two_cube_extruders);
    const std::string gcode_text = gcode(print);

    CHECK_FALSE(gcode_text.empty());
    CHECK(print.full_print_config().option<ConfigOptionFloats>("flush_multiplier")->values.size() == 1);
}

TEST_CASE("An empty flush_volumes_matrix is sized at apply so the wipe tower slices", "[Cardinality]")
{
    DynamicPrintConfig config = tower_config();
    config.option<ConfigOptionFloats>("flush_volumes_matrix", true)->values.clear();

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{ cube(20), cube(20) }, print, model, config, &two_cube_extruders);
    REQUIRE(print.has_wipe_tower());
    const std::string gcode_text = gcode(print);

    CHECK_FALSE(gcode_text.empty());
    CHECK(print.full_print_config().option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 4);
}

TEST_CASE("Print::apply sizes the flush matrix to the nozzles and filaments the config declares", "[Cardinality]")
{
    // Two nozzles and three filaments need three 3 x 3 blocks' worth of cells per nozzle; the schema default has 16.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_deserialize_strict({ { "nozzle_diameter", "0.4,0.4" }, { "filament_diameter", "1.75,1.75,1.75" } });
    REQUIRE(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 16);

    SECTION("filament_colour agrees with filament_diameter") {
        config.set_deserialize_strict({ { "filament_colour", "#FF0000;#00FF00;#0000FF" } });
    }
    SECTION("filament_colour is shorter than filament_diameter") {
        config.set_deserialize_strict({ { "filament_colour", "#FF0000;#00FF00" } });
    }

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance();
    Print print;
    print.apply(model, config);

    const DynamicPrintConfig &applied = print.full_print_config();
    CHECK(applied.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 18);
    CHECK(applied.option<ConfigOptionFloats>("flush_multiplier")->values.size() == 2);
    CHECK(applied.option<ConfigOptionStrings>("filament_colour")->values.size() == 3);
}

TEST_CASE("Print::apply keeps the flush volumes of a single-filament config as they were", "[Cardinality]")
{
    // A command-line slice of one filament carries the 16-cell schema default; it purges nothing and stays as is.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 16);

    Model model;
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->add_instance();
    Print print;
    print.apply(model, config);

    CHECK(print.full_print_config().option<ConfigOptionFloats>("flush_volumes_matrix")->values.size() == 16);
}

// ============================================================================
// Mismatch sweep. On a three-filament, two-extruder config, one array at a time is given the wrong length (empty,
// one entry, one short, one long) or one map entry an out-of-range value, and the config goes through
// Print::apply and, where the case calls for it, on to G-code. After apply every per-N option must have the
// length its cardinality declares, and the print must either export or be refused with a clean error: validate()
// or a SlicingError. A plain build only sees an out-of-bounds read that crashes; the full tier is meant to run
// under AddressSanitizer, which sees the rest.
// ============================================================================

namespace {

enum class Mutation { Empty, One, Short, Long };

const char *mutation_name(Mutation m)
{
    switch (m) {
    case Mutation::Empty: return "empty";
    case Mutation::One:   return "one entry";
    case Mutation::Short: return "one short";
    case Mutation::Long:  return "one long";
    }
    return "?";
}

// The length a mutation gives an array that holds n values.
size_t mutated_length(Mutation m, size_t n)
{
    switch (m) {
    case Mutation::Empty: return 0;
    case Mutation::One:   return 1;
    case Mutation::Short: return n - 1;
    case Mutation::Long:  return n + 1;
    }
    return n;
}

bool is_per_n(ConfigCardinality c) { return c != ConfigCardinality::Scalar && c != ConfigCardinality::Custom; }

const char *cardinality_name(ConfigCardinality c)
{
    switch (c) {
    case ConfigCardinality::Scalar:                 return "Scalar";
    case ConfigCardinality::PerExtruder:            return "PerExtruder";
    case ConfigCardinality::PerFilament:            return "PerFilament";
    case ConfigCardinality::PerExtruderVariant:     return "PerExtruderVariant";
    case ConfigCardinality::PerExtruderVariantDual: return "PerExtruderVariantDual";
    case ConfigCardinality::PerFilamentVariant:     return "PerFilamentVariant";
    case ConfigCardinality::PerProcessVariant:      return "PerProcessVariant";
    case ConfigCardinality::Custom:                 return "Custom";
    }
    return "?";
}

// Custom options whose length follows a rule of their own (a flush block per nozzle, an entry per filament variant,
// a per-extruder map) or that are per-N but not classified so yet. enforce_cardinality leaves them alone, so
// their readers see whatever length the config carries.
const std::vector<std::string> &shaped_custom_keys()
{
    static const std::vector<std::string> keys = {
        "flush_volumes_matrix", "flush_volumes_vector", "flush_multiplier", "flush_multiplier_fast",
        "filament_self_index", "filament_map_2", "filament_max_temperature_drop_when_ec",
        "physical_extruder_map", "nozzle_volume_type", "extruder_variant_list", "extruder_nozzle_count",
        "extruder_nozzle_volume_type", "extruder_nozzle_stats", "deretract_speed_extruder_change",
        "machine_min_extruding_rate", "machine_min_travel_rate",
    };
    return keys;
}

// The arrays the per-N counts are read from, and the variant id lists the variant expansion pairs with them.
const std::vector<std::string> &count_source_keys()
{
    static const std::vector<std::string> keys = {
        "nozzle_diameter", "filament_diameter", "printer_extruder_variant", "printer_extruder_id",
        "print_extruder_variant", "print_extruder_id", "filament_extruder_variant",
    };
    return keys;
}

// Cases that crash the process on this tree, so the sweep leaves them out until they are fixed. Each one reads an
// array that nothing has sized yet:
//  - an empty printer or process variant option, extruder_type, nozzle_volume_type or extruder_variant_list: the
//    variant expansion at the top of Print::apply, through get_at();
//  - an empty filament_self_index: Print::get_filament_unprintable_flow;
//  - an empty flush_volumes_matrix: the flush tables of Print::_make_wipe_tower;
//  - an empty flush multiplier of the prime-volume mode (flush_multiplier, flush_multiplier_fast in Fast mode):
//    GCode::append_full_config divides by its size;
//  - fewer nozzle_diameter entries than filament_map names extruders: GCode::print_machine_envelope.
bool is_known_crash(const DynamicPrintConfig &base, const std::string &key, Mutation m)
{
    static const std::set<std::string> empty_crashes = { "extruder_type", "nozzle_volume_type", "extruder_variant_list",
                                                         "filament_self_index", "flush_volumes_matrix", "nozzle_diameter" };
    const bool              fast_purge = base.opt_enum<PrimeVolumeMode>("prime_volume_mode") == pvmFast;
    const ConfigCardinality c          = cardinality_of(key);
    if (m == Mutation::Empty)
        return c == ConfigCardinality::PerExtruderVariant || c == ConfigCardinality::PerExtruderVariantDual ||
               c == ConfigCardinality::PerProcessVariant || empty_crashes.count(key) > 0 ||
               key == (fast_purge ? "flush_multiplier_fast" : "flush_multiplier");
    return m == Mutation::One && key == "nozzle_diameter";
}

// Cases that read out of bounds without crashing a plain build, so what they do there is luck. They run in the full
// tier only, where AddressSanitizer reports the read:
//  - filament_self_index one entry or one short, filament_extruder_variant one long:
//    Print::get_filament_unprintable_flow indexes the index list by the length of the variant list;
//  - flush_volumes_matrix one entry: the flush tables of Print::_make_wipe_tower read past it.
bool reads_out_of_bounds(const std::string &key, Mutation m)
{
    return (key == "filament_self_index" && (m == Mutation::One || m == Mutation::Short)) ||
           (key == "filament_extruder_variant" && m == Mutation::Long) || (key == "flush_volumes_matrix" && m == Mutation::One);
}

// Whether the default an empty option would be extended from is empty too, looked up as enforce_cardinality does.
bool default_is_empty(const std::string &key)
{
    const ConfigOption *def = FullPrintConfig::defaults().option(key);
    if (def == nullptr)
        if (const ConfigOptionDef *schema = print_config_def.get(key); schema != nullptr)
            def = schema->default_value.get();
    const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(def);
    return vec == nullptr || vec->empty();
}

// Three filaments on two direct-drive extruders, each extruder with a Standard and a High Flow variant. The
// variant options are laid out unexpanded, as the preset bundle hands them to Print::apply: two columns per
// extruder for the printer and process options, two per filament for the filament options.
DynamicPrintConfig sweep_config()
{
    DynamicPrintConfig config = multifilament_config(3, {
        { "enable_prime_tower",         true     },
        { "wipe_tower_x",               "20"     },
        { "wipe_tower_y",               "20"     },
        { "layer_height",               0.2      },
        { "initial_layer_print_height", 0.2      },
        { "skirt_loops",                0        },
        // Relative E addressing, the default, is refused without a reset at each layer.
        { "layer_change_gcode",         "G92 E0" } });
    config.set_num_extruders(2);
    config.set_deserialize_strict({
        { "single_extruder_multi_material", false },
        { "nozzle_diameter",                "0.4,0.4" },
        { "extruder_type",                  "Direct Drive,Direct Drive" },
        { "nozzle_volume_type",             "Standard,Standard" },
        { "extruder_printable_area",        "0x0,200x0,200x200,0x200#0x0,200x0,200x200,0x200" },
        { "extruder_printable_height",      "100,100" },
        { "physical_extruder_map",          "0,1" },
        { "filament_map_mode",              "Manual" },
        { "filament_map",                   "1,2,1" } });
    size_flush_to_nozzles(config);

    const std::vector<std::string> variants = { "Direct Drive Standard", "Direct Drive High Flow" };
    const auto columns = [&variants, &config](int owners, const char *variant_key, const char *id_key) {
        std::vector<std::string> names;
        std::vector<int>         ids;
        for (int owner = 1; owner <= owners; ++owner)
            for (const std::string &variant : variants) {
                names.push_back(variant);
                ids.push_back(owner);
            }
        config.option<ConfigOptionStrings>(variant_key, true)->values = names;
        config.option<ConfigOptionInts>(id_key, true)->values         = ids;
    };
    config.option<ConfigOptionStrings>("extruder_variant_list", true)->values =
        std::vector<std::string>(2, variants[0] + "," + variants[1]);
    columns(2, "printer_extruder_variant", "printer_extruder_id");
    columns(2, "print_extruder_variant", "print_extruder_id");
    columns(3, "filament_extruder_variant", "filament_self_index");

    // Every other per-N option, the project-only slot arrays included, at the length its cardinality declares.
    const AxisCounts n = axis_counts(config);
    for (const auto &[key, def] : print_config_def.options)
        if (def.default_value && def.default_value->is_vector() && is_per_n(cardinality_of(key))) {
            config.option(key, true);
            resize_to_cardinality(config, key, n);
        }
    // The per-N options still classified Custom at their per-N length too, so each has a length to cut.
    config.set_deserialize_strict({
        { "filament_map_2",                         "1,2,1" },
        { "filament_max_temperature_drop_when_ec",  "0,0,0" },
        { "extruder_nozzle_count",                  "1,1" },
        { "extruder_nozzle_volume_type",            "Standard,Standard" },
        { "deretract_speed_extruder_change",        "0,0" } });
    config.option<ConfigOptionStrings>("extruder_nozzle_stats", true)->values = { "Standard#1", "Standard#1" };
    return config;
}

// Three 5 x 5 x 1 mm cubes, one per filament, so every layer changes filament on one nozzle and swaps nozzles. The
// last cube prints with `last_filament`.
Model sweep_model(int last_filament = 3)
{
    Model model;
    for (int filament = 1; filament <= 3; ++filament) {
        TriangleMesh mesh = make_cube(5., 5., 1.);
        mesh.translate(80.f + 15.f * float(filament), 150.f, 0.f);
        ModelObject *object = model.add_object();
        object->add_volume(std::move(mesh));
        object->add_instance();
        object->config.set_key_value("extruder", new ConfigOptionInt(filament == 3 ? last_filament : filament));
    }
    return model;
}

// Per-N options whose length is not the one their cardinality declares. An empty option with an empty default is
// unset rather than mis-sized, as enforce_cardinality treats it.
std::vector<std::string> mis_sized_options(const DynamicPrintConfig &config)
{
    std::vector<std::string> bad;
    const AxisCounts n = axis_counts(config);
    for (const std::string &key : config.keys()) {
        const size_t want = expected_size(cardinality_of(key), n);
        const auto  *vec  = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
        if (want == 0 || vec == nullptr || vec->size() == want || (vec->empty() && default_is_empty(key)))
            continue;
        bad.push_back(key + " has " + std::to_string(vec->size()) + " of " + std::to_string(want));
    }
    return bad;
}

struct SweepOutcome
{
    std::string              thrown;    // an exception other than a SlicingError, empty when none was thrown
    std::string              refused;   // validate()'s error or a SlicingError's message
    std::vector<std::string> mis_sized; // per-N options off their declared length after apply or export
    bool                     exported = false;
};

// Apply `config` to the sweep plate and, when `slice` is set, validate, slice and export it.
SweepOutcome run_sweep_case(const DynamicPrintConfig &config, bool slice, int last_filament = 3)
{
    SweepOutcome out;
    const Model  model = sweep_model(last_filament);
    Print        print;
    try {
        print.apply(model, config);
        out.mis_sized = mis_sized_options(print.full_print_config());
        if (!slice)
            return out;
        if (StringObjectException err = print.validate(); !err.string.empty()) {
            out.refused = err.string;
            return out;
        }
        Slic3r::Test::gcode(print);
        out.exported = true;
        for (std::string &bad : mis_sized_options(print.full_print_config()))
            out.mis_sized.push_back("after export: " + bad);
    } catch (const SlicingError &e) {
        out.refused = e.what();
    } catch (const std::exception &e) {
        out.thrown = e.what();
    }
    return out;
}

// Every per-N option of the base config and the shaped Custom options.
std::set<std::string> all_sweep_keys(const DynamicPrintConfig &base)
{
    std::set<std::string> keys(shaped_custom_keys().begin(), shaped_custom_keys().end());
    for (const std::string &key : base.keys())
        if (is_per_n(cardinality_of(key)))
            keys.insert(key);
    return keys;
}

void check_outcome(const SweepOutcome &out)
{
    INFO("refused: " << out.refused);
    CHECK(out.thrown == "");
    CHECK(out.mis_sized == std::vector<std::string>{});
}

// How far a sweep takes each case: through apply only, on to G-code, or on to G-code with the cases a plain build
// reads out of bounds as well, for the AddressSanitizer run.
enum class SweepTier { Apply, Slice, Full };

// Every key of `keys` given each wrong length on `base`, one section per case, so a failure names its case and
// `-c "<section>"` runs it alone.
void sweep_lengths(const DynamicPrintConfig &base, const std::set<std::string> &keys, SweepTier tier)
{
    for (const std::string &key : keys) {
        const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(base.option(key));
        if (vec == nullptr || vec->empty()) {
            FAIL_CHECK(key << " has no values to cut in the sweep config");
            continue;
        }
        const size_t     n     = vec->size();
        std::set<size_t> tried = { n };
        for (Mutation m : { Mutation::Empty, Mutation::One, Mutation::Short, Mutation::Long }) {
            const size_t len = mutated_length(m, n);
            // A length already tried says nothing new.
            if (!tried.insert(len).second || is_known_crash(base, key, m) || (tier == SweepTier::Slice && reads_out_of_bounds(key, m)))
                continue;
            DYNAMIC_SECTION(key << " " << mutation_name(m)) {
                DynamicPrintConfig config = base;
                static_cast<ConfigOptionVectorBase *>(config.option(key))->resize(len);
                INFO(cardinality_name(cardinality_of(key)) << ", " << n << " -> " << len);
                check_outcome(run_sweep_case(config, tier != SweepTier::Apply));
            }
        }
    }
}

struct ValueCase
{
    const char *key;
    size_t      index;
    int         value;
    bool        known_crash; // crashes the process on this tree, so left out until fixed
};

// Out-of-range entries in the maps the slice indexes with: filament_map names extruders 1..2, physical_extruder_map
// nozzles 0..1, filament_self_index a filament 1..3 per variant column, filament_nozzle_map a nozzle,
// filament_volume_map and nozzle_volume_type a NozzleVolumeType, extruder_type an ExtruderType. The known crashes:
//  - a filament_map entry outside 1..2: GCode::print_machine_envelope indexes the machine limits with it;
//  - a physical_extruder_map entry outside 0..1: GCode::_do_export writes outside its first-filament table and
//    corrupts the heap, which a plain build may survive and AddressSanitizer does not;
//  - a negative nozzle_volume_type or extruder_type: get_nozzle_volume_type_string and get_extruder_variant_string
//    index their name tables with it.
const std::vector<ValueCase> &value_cases()
{
    static const std::vector<ValueCase> cases = {
        { "filament_map", 1, 0, true },           { "filament_map", 1, 3, true },
        { "filament_map", 1, -1, true },          { "physical_extruder_map", 1, 2, true },
        { "physical_extruder_map", 1, -1, true }, { "physical_extruder_map", 0, 100, true },
        { "filament_self_index", 4, 0, false },   { "filament_self_index", 4, 4, false },
        { "filament_self_index", 4, -1, false },  { "filament_nozzle_map", 1, 2, false },
        { "filament_nozzle_map", 1, -1, false },  { "filament_volume_map", 1, 100, false },
        { "filament_volume_map", 1, -1, false },  { "nozzle_volume_type", 1, 100, false },
        { "nozzle_volume_type", 1, -1, true },    { "extruder_type", 1, 100, false },
        { "extruder_type", 1, -1, true },
    };
    return cases;
}

void sweep_values()
{
    const DynamicPrintConfig base = sweep_config();
    for (const ValueCase &c : value_cases()) {
        if (c.known_crash)
            continue;
        DYNAMIC_SECTION(c.key << "[" << c.index << "] = " << c.value) {
            DynamicPrintConfig config = base;
            // ConfigOptionEnumsGeneric is a ConfigOptionInts, so the enum lists take an int the same way.
            auto *opt = dynamic_cast<ConfigOptionInts *>(config.option(c.key));
            REQUIRE(opt != nullptr);
            REQUIRE(c.index < opt->values.size());
            opt->values[c.index] = c.value;
            check_outcome(run_sweep_case(config, true));
        }
    }
}

} // namespace

TEST_CASE("The sweep's base config slices with filament changes on both nozzles", "[Cardinality][MismatchSweep]")
{
    const DynamicPrintConfig config = sweep_config();
    CHECK(mis_sized_options(config) == std::vector<std::string>{});
    // Every option the sweep cuts has a value to cut.
    for (const std::string &key : all_sweep_keys(config)) {
        const auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(config.option(key));
        INFO(key);
        REQUIRE(vec != nullptr);
        CHECK(vec->size() >= 2);
    }

    const SweepOutcome out = run_sweep_case(config, true);
    CHECK(out.thrown == "");
    CHECK(out.refused == "");
    CHECK(out.mis_sized == std::vector<std::string>{});
    CHECK(out.exported);

    Print print;
    print.apply(sweep_model(), config);
    const std::string gcode = Slic3r::Test::gcode(print);
    CHECK(print.has_wipe_tower());
    // Filaments 1 and 3 share the first nozzle and filament 2 has the second, so every layer changes both.
    CHECK(gcode.find("\nT1") != std::string::npos);
    CHECK(gcode.find("\nT2") != std::string::npos);
}

TEST_CASE("Print::apply sizes every per-N option given the wrong length to its declared length", "[Cardinality][MismatchSweep]")
{
    // Apply only: enforce_cardinality treats every option of a cardinality alike, so this is the light check that it
    // still reaches each one. The cases below also slice.
    std::set<std::string> keys;
    for (const std::string &key : sweep_config().keys())
        if (is_per_n(cardinality_of(key)))
            keys.insert(key);
    sweep_lengths(sweep_config(), keys, SweepTier::Apply);
}

TEST_CASE("A count source or a shaped option of the wrong length slices or is refused cleanly", "[Cardinality][MismatchSweep]")
{
    // What enforce_cardinality does not cover: the arrays the counts come from, the Custom options it leaves alone,
    // and the slot arrays and one option of each cardinality that the slice reads after apply.
    std::set<std::string> keys(shaped_custom_keys().begin(), shaped_custom_keys().end());
    keys.insert(count_source_keys().begin(), count_source_keys().end());
    keys.insert(PresetBundle::filament_slot_keys().begin(), PresetBundle::filament_slot_keys().end());
    keys.insert({ "extruder_printable_height", "retraction_length", "machine_max_acceleration_x", "filament_type",
                  "nozzle_temperature", "outer_wall_speed" });
    sweep_lengths(sweep_config(), keys, SweepTier::Slice);
}

TEST_CASE("A flush_multiplier_fast of the wrong length slices or is refused cleanly in Fast prime-volume mode", "[Cardinality][MismatchSweep]")
{
    // Fast mode reads flush_multiplier_fast where the Default mode reads flush_multiplier.
    DynamicPrintConfig base = sweep_config();
    base.set_key_value("prime_volume_mode", new ConfigOptionEnum<PrimeVolumeMode>(pvmFast));
    REQUIRE(run_sweep_case(base, true).exported);
    sweep_lengths(base, { "flush_multiplier_fast" }, SweepTier::Slice);
}

TEST_CASE("An out-of-range map entry slices or is refused cleanly", "[Cardinality][MismatchSweep]")
{
    sweep_values();
}

TEST_CASE("An object on a filament past the filament count slices or is refused cleanly", "[Cardinality][MismatchSweep]")
{
    check_outcome(run_sweep_case(sweep_config(), true, 4));
}

// Hidden: every per-N and shaped option given every wrong length, each sliced and exported. Meant for the nightly
// AddressSanitizer job, where an out-of-bounds read fails the run whether or not it crashes.
TEST_CASE("Every per-N and shaped option of the wrong length slices or is refused cleanly", "[.][Cardinality][MismatchSweepFull]")
{
    const DynamicPrintConfig base = sweep_config();
    sweep_lengths(base, all_sweep_keys(base), SweepTier::Full);
}
