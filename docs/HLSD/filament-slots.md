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
minus the mixed flags. `filament_slot_mismatches()` lists the arrays whose length is
not the slot count, and `filament_slots_consistent()` is true when it is empty.

Four operations change the slot count. Growth goes through one private resize,
`resize_filament_slot_arrays`, that walks `filament_slot_keys()` and seeds every new
entry; removal walks the same list and erases one entry from each array.

The writers that replace the whole slot list end with the same resize, so the arrays
hold one value per slot whether or not the top-up then adds a slot:

- The project loader pads the seven mixed-colour arrays of an older project
  (`filament_is_mixed` and the six `filament_mixed_*`) with
  `resize_mixed_filament_metadata`, then sizes the other arrays to the file's
  filament count: a file writes each array at its own length.
- A selection restored from the app config (`load_selections`, `update_selections`)
  is sized to its preset list: the colour packs come back at the length they were
  saved with, and the nozzle maps are not saved and start on their defaults.
- The AMS sync sets the mixed tail aside, sizes the arrays to the synced physical
  slots, and puts the tail back behind them; a mixed slot keeps its volume and nozzle
  maps and its mix settings.

A writer that sets one slot's value (the sidebar's colour picker, a combo's AMS colour,
the global nozzle maps) writes into the slots the arrays hold and never resizes them.

- `set_num_filaments(n, colour)` grows or shrinks at the raw tail. It is the right
  call for a mixed slot (which belongs at the tail) and for plain growth or shrink
  without a mixed tail. It refuses a shrink while there are mixed slots, because the
  cut would take the mixes instead of filaments.
- `insert_filament_slot(pos, colour)` adds one physical slot at `pos`, copying the
  preset of the slot before it, and returns the old-to-new index of every slot it
  pushed up. The sidebar inserts ahead of the mixed tail with it and re-points the
  model from the answer with `remap_model_filament_slots`.
- `set_num_physical_filaments(n)` brings the physical filaments to `n` and keeps the
  mixed tail last: growth inserts each slot ahead of the tail, a shrink removes the last
  physical slots the way a delete removes one. The mixed slots move with the count, and
  the moves are recorded like the top-up's.
- `update_num_filaments(idx)` removes one slot and remaps the mixed components that
  referenced the slots behind it.

A slot added without a colour takes the next colour of the fixed palette, which lives
in the bundle (`next_filament_colour()`) so the GUI, the project load and the tests
draw from one sequence. Which slots are new is decided by the colour array's length,
not by the preset list: a bundle that has not been given its selections yet runs the
preset list ahead of the arrays, and its default colours must survive.

### Normalising the slots

A writer that leaves a slot array out of step with the slot count is a bug, and the
array is read past its end by the sidebar, the AMS dialogs, the flush dialog and the
colour pickers, which index the bundle's arrays by slot before any slice sizes them.
`normalize_filament_slots()` is the net under the writers above: it puts the arrays
back in step and logs the ones it repaired. The bundle's entry points that rewrite the
slot arrays call it before they return: `load_selections`,
`update_selections`, the project load (`load_config_file_config`), `sync_ams_list`,
`reload_local_bundle` and `restore_selection`. The nozzle-count top-up calls it before it
inserts, because the insertion keeps each array's values up to the colours' length and
counts the mixed flags. On a consistent bundle it costs one length comparison per array.
A bundle without selections has no slots and is left alone.

The repair keeps every value an array has. An array does not say which slot a missing
value belonged to, so the repair takes it to be a physical slot's, the slot a writer
adds:

- In every array and the AMS list the missing value goes ahead of the mixed tail (the
  trailing run of mixed flags), and the tail stays last. All arrays take the same
  position, so a slot's colour, maps and colour pack stay together. If the value was a
  mixed slot's instead, the values of the slots behind it shift by one; only the writer
  can put that right, so the repair counts as a writer bug either way.
- It is seeded as a new slot's: the next palette colour, colour type 1, extruder 1,
  nozzle map 0, standard flow, the slot's colour as its colour pack, empty mixed
  metadata and an empty AMS colour list.
- An extra value is cut at the end, where every reader already stops.

Each distinct mismatch (array, length, slot count) is logged once as an error, with the
values a long array lost. `filament_slot_repairs()` counts the calls that had something
to repair and `last_filament_slot_repair()` lists what the last one found, so a test can
hold a writer to leaving the slots in step.

### The nozzle-count top-up

A tool changer feeds filament N from nozzle N, so a printer with more nozzles than
the project has physical filaments needs the missing slots.
`update_multi_material_filament_presets()` adds them: one `insert_filament_slot` per
missing nozzle, placed ahead of the mixed tail, with a copy of the last physical preset
and a palette colour. A saved selection can list colours for more slots than it names
presets for; a slot the top-up adds at the end takes the colour saved for it. A single-extruder multi-material printer maps any number of
filaments onto its shared nozzles, so one slot is enough for it: the top-up only fills an
empty list, and the user may delete down to one. The top-up runs on every preset load and extruder-count change (a printer
switch reaches it through the extruder-count handler when the count differs), so a
project opened with fewer filaments than the printer has nozzles gets its slots on
every path, not only when the extruder count happened to change.

When a mixed tail exists, the insertion moves the mixed slots up, and the model's
extruder ids and paint states must follow. The bundle has no model, so it records the
moves (`take_pending_slot_relocations()`, old to new index, -1 for a slot
`set_num_physical_filaments` removed), and the owner of the model applies them with
`remap_model_filament_slots`. That follows every reference a model makes to a slot: the
`extruder` and the feature and support filament options of objects, parts and height
ranges, the painted facets and the plates' tool changes. A removed slot is handled as a
delete without a replacement: objects and parts on it go to the first filament, its paint
is cleared, and its other options and tool changes are dropped. The owners are:
`Plater::load_files` for a freshly parsed project, right after the published-3MF
relocations; for the live model `GUI_App::load_current_presets`, the user-preset loads
and the removal of a preset bundle, the plugin host's local bundle reload, the preset
bundle dialog's reload when a bundle folder changes, the printer tab's extruder-count
handler and page rebuild, the sidebar's add and delete, the AMS sync and the
subscribed-bundle updates. A
selection restore after a failed settings import puts the snapshot's slots back and
drops the moves recorded in between. A surplus of physical slots when the nozzle count drops is
still removed one slot at a time through the sidebar's delete, which renumbers the
model as it goes.

`Plater::sync_filament_slots_from_bundle` is the GUI's side of a change the bundle made
on its own: it takes the recorded moves, removes the plates' map entries of the removed
slots, re-points the model, and gives each slot the bundle added a plate map entry at
its own position (`PartPlate::on_filament_added(slot)`, through
`insert_plate_filament_slot`), so a mixed slot keeps the per-plate filament, nozzle and
volume map it had.

A geometry-only import (a 3MF without settings, or one whose settings are skipped, and a
Prusa project) names the filaments of its source by extruder id, and those are physical:
no mixed definition comes with it. `Plater::load_files` grows the project's physical
filaments to the highest id the imported objects use with `set_num_physical_filaments`,
so an id that named one of the project's mixed slots names a filament of its own
afterwards, and the objects already in the project follow the mixes they use.

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

A project with mixed slots given more `--load-filaments` than it has slots takes the
extra filaments as physical ones, ahead of the mixed tail, as the GUI adds a filament.
The CLI assembles the config in load order first, with the extra filaments at the end,
and then moves the mixed slots behind them with `reorder_filament_slots`: every
per-filament list moves with its slot (the variant-expanded ones by
`filament_self_index`, the preset columns, the flush matrix and vector), and the mixed
components, the feature filament ids and a plate's print sequences follow the filaments
they name. A list it cannot lay out is left in place and logged. The `filament_dev_*`
lists hold a run of their own length per filament, so the CLI lays them out again from
the filament configs in the new order. The command line options, the plates' own maps
and print sequences and the model (`remap_model_filament_slots`) move the same way. The
extra filaments therefore print with the tool numbers of the slots ahead of the mixes.

## Tests

- `[Cardinality]` (`tests/libslic3r/test_printconfigcardinality.cpp`): the
  classification is complete, `enforce_cardinality` extends and cuts to the declared
  length, `Preset::normalize` sizes the plain per-filament options, the shipped profile
  corpus satisfies the invariant.
- `[FilamentSlots]` (`tests/libslic3r/test_filament_slots.cpp`): every slot array
  follows the preset list through growth, shrink, insert and delete; the top-up grows
  the arrays with the list, inserts ahead of the mixed tail and reports the move; a
  single-extruder multi-material printer keeps the slots it has; new slots draw from the
  palette; a bundle without selections keeps its default colour; the flush matrix
  resize keeps the pairs and nozzles the old layout had; the normaliser reports each
  array out of step, seeds what is missing, keeps the mixed tail last and counts its
  repairs; the selection load, the project load and the AMS sync leave it nothing to
  repair, and a sync with a mix behind a High Flow slot keeps every slot's values on
  that slot; the physical count grows and shrinks with the mixed tail last and the
  model following; an imported object naming a mixed slot gets a filament of its own; a
  shrink across the mixed tail is refused; a plate's maps take an inserted slot at its
  position; moving or removing a slot re-points every reference the model makes to it;
  the CLI's reorder moves every per-filament value and renumbers the references and a
  plate's print sequences.
- `[FilamentMixer]` and `[Published]` (`tests/libslic3r/test_preset_bundle_loading.cpp`):
  the mixed-slot and published-3MF flows on top of the same operations.
- `[FilamentSlots]` in the same file: a loaded project, a restored selection and an AMS
  sync hold one value per slot in every array without the top-up adding a slot.
- `cli_short_palette` (`tests/cli/test_cli_short_palette.sh`): a two-filament project
  carrying one colour and a three-filament matrix slices on the CLI and exports one
  colour per filament with the matrix pairs it had; a one-filament project given two
  filaments slices with two.
- `cli_mixed_overload` (`tests/cli/test_cli_mixed_overload.sh`): a project of two
  filaments and a mix, naming the mix by the cube's extruder, object and part feature
  filaments, a height range and the plate's first-layer order, given a fourth filament
  exports the extra filament in the third slot and the mix in the fourth with every
  reference on it, and the per-filament lists of the same layout loaded directly.
