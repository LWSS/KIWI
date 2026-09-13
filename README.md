# KIWI

## About the project
COD4-Inspired engine. Not Backwards compatible. WIP.

## Native x64 fastfiles

Build `KIWI-linker_pc64`, or select BSP → LIGHT → FASTFILE in Radiant's
Build dialog. Each selected stage must succeed before the next starts. Fastfile-only
builds use the existing BSP. `src/database/` remains on disk and is excluded from
project sources; the engine uses `src/database64/`.

This is a custom KIWI asset workflow. Rebuilding the retail startup/common zones
or supplying their missing source assets is not required to build your custom
FastFiles. Only assets referenced by your map, its dependencies, and your CSV
must be available. Use your own zone sources and asset set.

Fresh x64 CMake builds include native loading by default. For an existing build
directory, set `KIWI_RAW_ONLY=OFF` explicitly because CMake preserves cached options.
At runtime, `+set useFastFile 1` selects native loading; loose-file mode remains
the runtime default. Native mode also loads the engine's startup zones, so a
playable standalone game needs your own startup asset zones as well as its map
FastFile. The map linker does not require those zones unless your CSV uses `ignore`.

```
linker_pc64.exe -root "F:\path\to\game" -native-map maps/mp/mp_test.d3dbsp mp_test
linker_pc64.exe -root "F:\path\to\game" -native-map maps/mp/mp_test.d3dbsp -native zone_source/mp_test.csv mp_test
```

These commands write `zone/english/mp_test.ff` in KIWI's native `KIWIfF64` format.
The current stream ABI is version 2; rebuild native zones created with version 1.
Version 2 supports typed references to assets in previously loaded zones. Missing
external assets fail loading; they do not silently become defaults. Zone unloading and asset override restoration follow the engine registry lifecycle.
`-language`, `-output`, and `-nocompress` override the defaults. Writes are
transactional: a failed compilation preserves the previous output.

A native map contains one BSP's render world, collision world, entities, primary
lights and multiplayer metadata. Compilation includes referenced materials,
techniques, shaders, images, static models, physics geometry, dynamic entities,
model pieces and effects. Effect-to-effect dependencies are collected into the zone.
Map builds also include their impact-effect table and its effects automatically.
A custom workflow can explicitly skip this table with `omit,impactfx:<map>` in its
additional CSV. Such a zone does not provide impact effects for gameplay.
Radiant automatically adds `zone_source/<map>.csv` when it exists. This CSV lists
additional assets; the map itself is already included:

```csv
rawfile,maps/mp/mp_test.gsc
fx,weather/rain_mp_cargoship
xmodel,barbwire_arm
sound,common.csv/grenade_bounce_default
```

`include,name` and `include_pc,name` expand `zone_source/name.csv`. Console/PS3
includes are skipped. Includes may nest; cycles, traversal paths and oversized
expansions are rejected. `ignore,zone` reads a previously built `zone.ff` and its
`.ff.assets` index, first beside the output, then under `zone/<language>/` in the
asset root. Build dependencies first. The index includes serialized child assets;
its checksum rejects stale or mismatched FastFiles. Matching assets become typed
external references, so the dependency zone must be loaded first. Current imports
still require their source files even when serialization uses an external reference.

`omit,sound:alias_name` explicitly leaves an asset out, including when a shared
script discovers it. Omission takes precedence over direct rows regardless of
row order and is printed in the build log. Supported omission types are `sound`,
`rawfile`, `xmodel`, `weapon`, `fx`, and `material`. Required native references
still must resolve; omitting a weapon's required sound, for example, fails the
build. Scripts are retained verbatim, so do not call omitted assets at runtime.

Map compilation also includes existing `<map>.gsc`, `<map>_fx.gsc`, and
`maps/createfx/<map>_fx.gsc` scripts, along with their static script dependencies.

Supported CSV rows are `map`, `gfxworld`, `mapworlds`, `rawfile`, `stringtable`, `localize`, `image`, `material`, `font`, `techset`, `lightdef`,
`xmodel`, `xanim` (version 17), `weapon`, `fx`, `impactfx`, `physpreset`, `soundcurve` (`.vfcurve`), `snddriverglobals` (`singleton`), and `loaded_sound` (PCM WAV). For a standalone CSV zone,
use `-native zone_source.csv -output output.ff zone_name`. Inputs are resolved from
loose `raw/` files, then unlocalized `raw/` IWDs, then loose `main/` files
and unlocalized `main/` IWDs. Multiple maps in one
native zone are rejected.

`material,",name"` emits a reference to a material loaded by another zone.
The registry resolves it by name, or creates its usual default entry if absent;
load `$default` before unresolved material references. The stock deferred menu
material `$levelbriefing` uses this representation when no raw material exists.
Other missing material sources still fail compilation.

`localize,ui` expands `raw/<language>/localizedstrings/ui.str` into native
localization entries such as `UI_BACK`. `-language` selects the language;
missing language files and untranslated entries fall back to English. StringEd
version 1, `#same`, literal newline escapes and argument placeholders are supported.
Malformed files and duplicate references within a file fail the build.

`font,fonts/bigDevFont` imports a raw font with its glyphs, base material and glow
material. Font files retain their existing raw paths and do not need a new extension.
Font and material sources prefer the selected language directory (for example,
`raw/english/fonts`), then matching `localized_<language>_*.iwd` archives,
then the base asset path when absent. Other languages are excluded.

`menufile,ui/default.menu` uses the engine's raw menu parser, including its
preprocessor, then serializes the menu graph, materials, and focus-sound dependencies.
Focus sounds share the normal manifest sound imports, including alias dependencies,
load-spec overrides, and embedded streamed audio. `loadmenu` list
files (such as `ui_mp/menus.txt`) expand their referenced menus, reject missing
files, and load repeated references to the same file once.
Menu actions also discover literal `play` and `setbackground` dependencies in
semicolon-separated commands, including key handlers and list-box double-click actions.

Native weapon serialization preserves model, material, effect, and sound references,
weapon strings, script-string tags, and both accuracy graphs. Original graphs use
their own stored counts. `weapon,ak47_mp` imports `weapons/mp/ak47_mp`, sharing the
engine's field table and importing its accuracy files. Models, materials, effects,
sounds, animations, alternate weapons, and notetrack sounds are included automatically.
Missing surface-specific bounce sounds use the weapon's default bounce alias;
unreadable sound tables fail the build. Explicit sound rows retain their variant overrides.

`impactfx,mp_test` applies global `fx/*.csv` files alphabetically, then map-specific
`fx/maps/mp_test/*.csv` overrides. Explicit blank effects remain blank; missing or
blank flesh variants use the ordinary flesh effect. Missing required entries fail
the build. Each zone supports one impact table, stored under the empty name used
by the engine's lookup.

The linker generates `$white`, `$black`, `$black_3d`, `$black_cube`, `$gray`, and
`$identitynormalmap`, and `$pixelcostcolorcode` for image rows and material dependencies.
They need no `.iwi` source.

`sound,<alias>` finds and merges loaded PCM alias variants across sound-alias
tables, including audio, falloff curves and speaker maps. Use
`sound,<table>.csv/<alias>` to select one table explicitly. Map zones apply that
map's multiplayer load specification. Streamed aliases retain streaming semantics
and include their audio bytes as binary rawfile assets. In fastfile mode, Miles
callbacks read packaged audio through an owned memory stream, falling back to
filesystem/IWD audio when no packaged asset is available. Playback verification remains pending.
Loaded audio currently requires PCM WAV.
Secondary and chained aliases are included transitively; missing aliases fail the build.
Sound visuals in nested effects and dynamic-entity destruction effects also queue their aliases.
Literal script playback calls discover sound aliases, including secondary/chained
dependencies. Names assembled from variables still require explicit manifest rows.
For standalone `-native` builds, the final zone-name argument supplies the sound
load-spec level. A map row uses its BSP map name instead. Alias errors distinguish
missing names, excluded variants, and unreadable source audio.
To reuse sound variants from another source context, explicitly select them with
`sound,alias_name,source_level,all_mp` (or `all_sp`). This only changes that alias's
variant selection; it does not change the map's game mode or bypass audio validation.

Native loading requires `KIWI_RAW_ONLY=OFF`. Complete client runtime verification
remains pending; successful linker/loader tests do not establish that a map is
independently playable from its fastfile. IWI bitmap,
DXT and wavelet images are supported; DXN and volume image importing remain unsupported.
Retail CoD4 fastfiles are incompatible.

For `.gsc` rawfile rows, the linker recursively includes scripts named by
`#include` and `path::function`, ignoring comments and string contents. Repeated
and cyclic script references are deduplicated. Dynamically constructed script
names and asset names still require explicit CSV entries. Literal single-argument
`loadfx`, `precachemodel`, and `precacheshader` calls add native FX, model, and
material assets automatically; computed arguments are not evaluated.

## Raw archive compatibility

Invoking the linker without `-native` or `-native-map` still writes the separate
`KIWIPK64` raw archive format. It contains one BSP and the other files under `raw/`,
excluding other BSPs and tool outputs. `-verify file.ff` currently verifies this
archive format only. Raw archives retain runtime parsing and are not native zones.

With `KIWI_RAW_ONLY=ON`, the engine can mount matching raw
archives automatically. `fs_usePackages=0` disables this for loose-file development.
Retail fastfiles and native zones must not be treated as raw archives.

Mounted raw archives take precedence over loose files and remain mounted until
another BSP is loaded or the filesystem shuts down. Set `fs_usePackages=0` and
restart for loose-file editing; this raw-archive behavior does not apply to native zones.

Stock `sound,table,level,all_mp` (or `all_sp`) rows expand matching aliases from
`soundaliases/table.csv`. The level column may be empty; this preserves the
sound load-spec empty-level behavior, including menu aliases. Alias-specific
rows remain supported; use `table.csv/alias` to select one alias explicitly.

Stock `weapon,mp/name` rows are accepted and register the weapon as `name`, matching
the engine. Sound-alias `platform` metadata is reserved for XMAUpdate conversion;
the linker uses load specifications for inclusion, as in the stock PC zones.
