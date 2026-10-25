#pragma once

// KIWI model importer - recipe orchestration: inspect a source, make a recipe for a
// new model, (re)generate its collision, and build every enabled texture variant
// into raw\ xmodels + collmaps\ hulls. Shared by KIWI-modelimport.exe (CLI + MCP) and
// the Radiant Model Import window.

#include "ki_mesh.h"
#include "ki_recipe.h"
#include "ki_textures.h"

#include <filesystem>
#include <string>
#include <vector>

namespace kimport {

// A loaded source: the mesh, its pieces, and the model-space geometry (inches,
// origin rebased the way the xmodel writer will) for collision and previews.
struct SourceState {
    std::string loadedFrom;          // recipe.source this was loaded for
    long long sourceSize = -1;       // re-stat on every inspect: a re-export reloads
    long long sourceStamp = 0;
    std::string loadedUnits;
    double loadedScale = 0.0;
    std::string loadedOrigin;
    bool loadedFlipWinding = false;
    std::string loadedCullFacing;    // KIWI: recipe.cullFacing the mesh was culled with
    bool loadedUvFlip = true;
    std::filesystem::path objPath;
    ObjMesh mesh;
    pieces::PieceSet pieceSet;
    ModelSpaceMesh modelMesh;
};

// Loads (or reuses, when the recipe's source file and its size/write time,
// units/scale/origin/winding are unchanged) the source mesh.
bool InspectSource(const Recipe& recipe, SourceState& state, const LogFn& log,
                   std::string& error);

// Recipe piece table refreshed against the mesh: names/triangle counts filled,
// surface overrides kept where index + triangle count still match.
void SyncPieces(Recipe& recipe, const SourceState& state, const LogFn& log);

// Per-piece surface types (0 = default) for the converter.
bool PieceTypes(const Recipe& recipe, const SourceState& state, std::vector<int>& types,
                std::string& error);

// ---- collision: the master hull file is collmaps\<recipe.asset>.map ----------------

std::filesystem::path MasterCollmapPath(const GameRoot& root, const Recipe& recipe);

// Hulls from the current mesh + the recipe's collision settings.
void GenerateRecipeHulls(const Recipe& recipe, const SourceState& state,
                         std::vector<Hull>& hulls, const LogFn& log);

// Regenerates and writes the master collmap. Refuses in manual mode unless `force`
// (that would throw away hand edits); with force the recipe switches back to auto.
bool RegenerateCollision(Recipe& recipe, const GameRoot& root, SourceState& state,
                         bool force, std::vector<Hull>& hulls, const LogFn& log,
                         std::string& error);

// The hulls a build would use: generated in auto mode, read from the master in
// manual mode, none in none mode.
bool CurrentHulls(const Recipe& recipe, const GameRoot& root, SourceState& state,
                  std::vector<Hull>& hulls, const LogFn& log, std::string& error);

// ---- textures ----------------------------------------------------------------------

// Importer settings: string values in <game root>\model_source\settings.json
// ("textureFolder", "sourceFolder", ...). "" when unset.
std::string GetImportSetting(const GameRoot& root, const std::string& key);
bool SetImportSetting(const GameRoot& root, const std::string& key, const std::string& value,
                      std::string& error);

// The default InstaMAT export folder: %KIWI_TEXTURE_FOLDER%, else the "textureFolder"
// setting, else "".
std::string DefaultTextureFolder(const GameRoot& root);
bool SaveDefaultTextureFolder(const GameRoot& root, const std::string& folder,
                              std::string& error);

// textures.set value for a multi-material source: one InstaMAT set per source material.
inline constexpr const char* kAllTextureSets = "*";

// With textures.set "*", `variants`/`setName` are the first set's.
bool ResolveTextures(const Recipe& recipe, TextureScan& scan,
                     const std::vector<TextureVariant>*& variants, std::string& setName,
                     std::string& error);

// The texture set of each source material ("Default.001" -> set "Default_1"), aligned
// with `materialNames`. Fails when a material has no set.
bool MaterialTextureSets(const TextureScan& scan, const std::vector<std::string>& materialNames,
                         std::vector<const std::vector<TextureVariant>*>& perMaterial,
                         std::vector<std::string>& setNames, std::string& error);

// Best texture prefix for a source file in a folder ("" when none fits).
std::string GuessTexturePrefix(const std::filesystem::path& source,
                               const std::filesystem::path& folder);

// ---- recipes -----------------------------------------------------------------------

// FBX/OBJ sources in a folder (an OBJ beside a same-named FBX is skipped: the FBX is
// the source of record), newest first; sinceHours <= 0 = all.
struct SourceEntry {
    std::filesystem::path source;
    double ageHours = 0.0;
    std::string asset;            // the name a new recipe would get
    bool hasRecipe = false;
    std::string texturePrefixGuess;
};
std::vector<SourceEntry> ScanSources(const GameRoot& root, const std::filesystem::path& folder,
                                     double sinceHours, std::string& error);

struct NewRecipeOptions {
    std::string asset;               // "" = kiwi_<source stem>
    std::string surface = "concrete";
    std::string units = "in";
    std::string textureFolder;       // "" = DefaultTextureFolder
    std::string texturePrefix;       // "" = guess from the source name
};

// The asset name from the source / options alone (sanitized, at most 46 characters).
std::string NewRecipeAssetName(const std::filesystem::path& source,
                               const NewRecipeOptions& options);

// The name NewRecipe will really give the recipe and its master collmap: the one above,
// shortened when a texture variant's "_<id>" would not fit (that needs the texture
// folder scan).  Use it for "already exists" checks before importing.
std::string PlannedAssetName(const std::filesystem::path& source, const NewRecipeOptions& options,
                             const GameRoot& root);

// Builds the recipe, syncs pieces, finds the texture variants and writes the
// master collmap (auto hulls).
bool NewRecipe(const std::filesystem::path& source, const NewRecipeOptions& options,
               const GameRoot& root, Recipe& recipe, SourceState& state, const LogFn& log,
               std::string& error);

struct BuildReport {
    struct Built {
        std::string asset;
        std::string variant;
        std::size_t hulls = 0;
        std::filesystem::path collmap;
    };
    std::vector<Built> built;
    std::vector<std::string> warnings;
};

bool BuildRecipe(const Recipe& recipe, const GameRoot& root, SourceState& state,
                 const LogFn& log, BuildReport& report, std::string& error);

}  // namespace kimport
