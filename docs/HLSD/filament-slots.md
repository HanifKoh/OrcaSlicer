# Filament slots and per-N array sizing — High Level Design

## Why it exists

A print carries settings that come one per *something*: one value per extruder
(`nozzle_diameter`, `retraction_length`), one per filament (`filament_diameter`,
`filament_colour`), one per extruder-variant slot (`machine_max_*`), one per
filament slot of the project (`filament_map`, `filament_is_mixed`). Every consumer
indexes those arrays by an extruder or filament id, and a value read past the end of
an array is a crash somewhere far from the code that left it short: multi-material
segmentation sized its state from `filament_colour` while the painting carried ids up
to `filament_diameter.size()`, the wipe tower and the brim read `filament_colour` by
the id the grouping handed them, the CLI's flush-volume rebuild read the project's
colours at the project's length after it had loaded more filaments.

The counts used to be derived in four places from four arrays, and the arrays were
sized in as many places by hand. This design gives each count one owner and each
array one sizing path, so a short array cannot reach a consumer, and a new per-N
option is sized everywhere once it is classified.

## The two layers

The slicer sees a flat `DynamicPrintConfig`; the GUI and the CLI assemble it from a
printer, a process and one filament preset per slot plus the project's own per-slot
arrays. The two layers size arrays at different times and from different counts, so
each has its own rule.

| Layer | Count owner | Sizing path | When it runs |
|---|---|---|---|
| Config (libslic3r) | `axis_counts()` reads extruders from `nozzle_diameter`, filaments from `filament_diameter`, variants from the variant lists | `DynamicPrintConfig::enforce_cardinality()` | `Preset::normalize` at load (plain per-filament options), `Print::apply` for the slice, the CLI after its filament merge |
| Bundle (`PresetBundle`) | `num_filament_slots()` is the length of `filament_presets` | `set_num_filaments`, `insert_filament_slot`, `update_num_filaments`, the nozzle-count top-up | Whenever a slot is added, inserted or removed, and whenever the printer's nozzle count exceeds the physical slots |

The bundle produces `filament_diameter` with one entry per slot when it assembles the
full config, so the config layer's filament count is the bundle's slot count by
construction. The config layer still enforces at `Print::apply`, because projects and
presets written by other tools, and the CLI's own merge, reach it without the bundle.

## Config layer: one declared cardinality per option

`PrintConfigCardinality` classifies every vector option into one `ConfigCardinality`:
`PerExtruder`, `PerFilament`, the three variant classes and their dual form, `Scalar`
(owner-sized or fixed-shape vectors the sizer leaves alone) and `Custom` (vectors that
are not per-N: matrices, geometry, bookkeeping lists). The classification is built from
the option sets the other subsystems already own (`printer_extruder_options`,
`Preset::filament_options()`, the variant sets), plus two lists of the bundle's own:
the per-filament project keys that are not filament-preset fields and the explicit
`Custom` list.

`enforce_cardinality()` resizes every per-N option to `expected_size(cardinality,
counts)`: a short array is extended by repeating its first value (the option's default
when it is empty), a long one is cut to the count. `Scalar` and `Custom` options are
never touched.

Classification is complete by construction: the `[Cardinality]` test walks every
vector option in `print_config_def` and fails on one that is neither per-N nor listed
as `Custom`. Porting a feature that adds a per-filament or per-extruder vector option
therefore means adding it to the right set once; it is then sized at load, at apply
and in the CLI without any further code.

## Bundle layer: the slot list and its arrays

A project has `num_filament_slots()` filament slots. `filament_presets` names the
preset of each slot, and every array named in `PresetBundle::filament_slot_keys()`
holds exactly one value per slot:

- `filament_colour`, `filament_colour_type`, `filament_multi_colour`
- `filament_map`, `filament_nozzle_map`, `filament_volume_map`
- `filament_is_mixed`, `filament_mixed_components`, `filament_mixed_sublayer_ratios`,
  `filament_mixed_gradient`, `filament_mixed_gradient_range`,
  `filament_mixed_gradient_curve`, `filament_mixed_gradient_per_part`

plus the AMS colour list `ams_multi_color_filment`, which is not a config option.
Every config key in the list is `PerFilament` in the cardinality classification, and a
test holds the two lists to that.
Physical filaments come first; mixed-colour slots, which are virtual and have no
nozzle of their own, sit at the tail. `num_physical_filaments()` is the slot count
minus the mixed flags, and `filament_slots_consistent()` states the invariant for
tests and debugging.

Three operations change the slot count. Growth goes through one private resize,
`resize_filament_slot_arrays`, that walks `filament_slot_keys()` and seeds every new
entry; removal walks the same list and erases one entry from each array. The project
loader pads the seven mixed-colour arrays of an older project (`filament_is_mixed` and
the six `filament_mixed_*`) with
`resize_mixed_filament_metadata` before the selections size the rest.

- `set_num_filaments(n, colour)` grows or shrinks at the raw tail. It is the right
  call for a mixed slot (which belongs at the tail) and for plain growth without a
  mixed tail.
- `insert_filament_slot(pos, colour)` adds one physical slot at `pos`, copying the
  preset of the slot before it, and returns the old-to-new index of every slot it
  pushed up. The sidebar inserts ahead of the mixed tail with it and re-points the
  model from the answer with `remap_model_filament_slots`.
- `update_num_filaments(idx)` removes one slot and remaps the mixed components that
  referenced the slots behind it.

A slot added without a colour takes the next colour of the fixed palette, which lives
in the bundle (`next_filament_colour()`) so the GUI, the project load and the tests
draw from one sequence. Which slots are new is decided by the colour array's length,
not by the preset list: a bundle that has not been given its selections yet runs the
preset list ahead of the arrays, and its default colours must survive.

### The nozzle-count top-up

A tool changer feeds filament N from nozzle N, so a printer with more nozzles than
the project has physical filaments needs the missing slots.
`update_multi_material_filament_presets()` adds them: one `insert_filament_slot` per
missing nozzle, placed ahead of the mixed tail, with a copy of the last physical preset
and a palette colour. A saved selection can list colours for more slots than it names
presets for; a slot the top-up adds at the end takes the colour saved for it. A single-extruder multi-material printer counts every slot,
mixed ones included, against its nozzles, but a slot it adds still goes ahead of the
mixed tail. The top-up runs on every preset load and extruder-count change (a printer
switch reaches it through the extruder-count handler when the count differs), so a
project opened with fewer filaments than the printer has nozzles gets its slots on
every path, not only when the extruder count happened to change.

When a mixed tail exists, the insertion moves the mixed slots up, and the model's
extruder ids and paint states must follow. The bundle has no model, so it records the
moves (`take_pending_slot_relocations()`), and the owner of the model applies them:
`Plater::load_files` for a freshly parsed project, right after the published-3MF
relocations; for the live model `GUI_App::load_current_presets`, the user-preset loads
and the removal of a preset bundle, the plugin host's local bundle reload, the printer
tab's extruder-count handler and page rebuild, the sidebar's add and delete, the AMS
sync and the subscribed-bundle updates. A
selection restore after a failed settings import puts the snapshot's slots back and
drops the moves recorded in between. A surplus of physical slots when the nozzle count drops is
still removed one slot at a time through the sidebar's delete, which renumbers the
model as it goes.

## The flush matrix

`flush_volumes_matrix` is `Custom`: one block of `filaments²` cells per nozzle, so no
single count sizes it. `resize_flush_volumes_matrix` (PrintConfig.cpp) brings it to a
filament and nozzle count while keeping the cells of the pairs and nozzles the old
layout shares with the new one; a new cell is the sum of the pair's per-filament flush
volumes. The bundle's top-up and the CLI both use it, so a project's authored pairs
survive whichever way it is opened.

## CLI

The CLI builds its config from the project and the loaded presets itself, without a
bundle. After its filament merge it calls `enforce_cardinality()`, so the flush-volume
code, the filament grouping and every other consumer that runs before `Print::apply`
see arrays of the loaded filament count, and it resizes a project's flush matrix with
the shared helper. The mixed-slot arrays are padded with their defaults first, so a mix
in the first slot is not replicated, and their loaded length still feeds the "mixed slot
without a filament" check. A second `enforce_cardinality()` runs after the defaults
merge, because an option no preset or project defined arrives there as a single value
and the exported project must hold every array at its full length whichever way it was
assembled. A model given several filaments but no colours gets the default colours at
the first pass, so its matrix is computed for those filaments, as the GUI does for a new
project.

## Tests

- `[Cardinality]` (`tests/libslic3r/test_printconfigcardinality.cpp`): the
  classification is complete, `enforce_cardinality` extends and cuts to the declared
  length, `Preset::normalize` sizes the plain per-filament options, the shipped profile
  corpus satisfies the invariant.
- `[FilamentSlots]` (`tests/libslic3r/test_filament_slots.cpp`): every slot array
  follows the preset list through growth, shrink, insert and delete; the top-up grows
  the arrays with the list, inserts ahead of the mixed tail and reports the move; a
  single-extruder multi-material printer tops up in total; new slots draw from the
  palette; a bundle without selections keeps its default colour; the flush matrix
  resize keeps the pairs and nozzles the old layout had.
- `[FilamentMixer]` and `[Published]` (`tests/libslic3r/test_preset_bundle_loading.cpp`):
  the mixed-slot and published-3MF flows on top of the same operations.
- `cli_short_palette` (`tests/cli/test_cli_short_palette.sh`): a two-filament project
  carrying one colour and a three-filament matrix slices on the CLI and exports one
  colour per filament with the matrix pairs it had; a one-filament project given two
  filaments slices with two.
