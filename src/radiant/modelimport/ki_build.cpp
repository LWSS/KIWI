// KIWI model importer - recipe orchestration. See ki_build.h.

#include "ki_build.h"

#include "ki_autocollide.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace kimport {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

// Lowercase letters and digits only: "tri_window" and "triwindow" compare equal.
std::string AlnumKey(const std::string& text) {
    std::string key;
    for (char c : text) {
        if (c >= 'A' && c <= 'Z') key.push_back(static_cast<char>(c - 'A' + 'a'));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) key.push_back(c);
    }
    return key;
}

// `whole` ends with `part` at a '_' boundary ("waterpipe_holder" / "pipe_holder").
bool EndsWithToken(const std::string& whole, const std::string& part) {
    return whole.size() > part.size() + 1 &&
           whole.compare(whole.size() - part.size(), part.size(), part) == 0 &&
           whole[whole.size() - part.size() - 1] == '_';
}

void SourceStamp(const std::string& source, long long& size, long long& stamp) {
    std::error_code ec;
    size = static_cast<long long>(fs::file_size(source, ec));
    if (ec) size = -1;
    stamp = fs::last_write_time(source, ec).time_since_epoch().count();
}

bool MakeRequest(const Recipe& recipe, const SourceState* state, ConvertRequest& request,
                 std::string& error) {
    request = {};
    request.mesh = state ? &state->mesh : nullptr;
    request.sourcePath = recipe.source;
    request.assetName = recipe.asset;
    request.unitsFactor = UnitsFactor(recipe.units);
    if (!(request.unitsFactor > 0.0)) {
        error = "units '" + recipe.units + "' must be m, cm, mm or in";
        return false;
    }
    request.scale = recipe.scale;
    if (!ParseOrigin(recipe.origin, request.originMode)) {
        error = "origin '" + recipe.origin + "' must be base, center or none";
        return false;
    }
    if (!ParseNormalGreen(recipe.normalGreen, request.normalGreen)) {
        error = "normalGreen '" + recipe.normalGreen + "' must be auto, flip or keep";
        return false;
    }
    request.flipUv = recipe.uvFlip;
    if (!cod4::ParseSurfaceType(recipe.surface, request.surfaceType) ||
        request.surfaceType <= 0) {
        error = "surface '" + recipe.surface + "' is not a CoD4 surface type";
        return false;
    }
    request.glassOpacity = recipe.textures.glassOpacity;
    return true;
}

fs::path SettingsPath(const GameRoot& root) {
    return root.ModelSource() / "settings.json";
}

}  // namespace

bool InspectSource(const Recipe& recipe, SourceState& state, const LogFn& log,
                   std::string& error) {
    long long size = -1;
    long long stamp = 0;
    SourceStamp(recipe.source, size, stamp);
    const bool sameMesh = !state.loadedFrom.empty() && state.loadedFrom == recipe.source &&
                          state.loadedFlipWinding == recipe.flipWinding &&
                          state.sourceSize == size && state.sourceStamp == stamp;
    if (!sameMesh) {
        SourceState fresh;
        if (!LoadSourceMesh(recipe.source, recipe.flipWinding, fresh.mesh, fresh.objPath, log,
                            error))
            return false;
        fresh.pieceSet = pieces::BuildPieces(ToPieceMesh(fresh.mesh));
        fresh.loadedFrom = recipe.source;
        fresh.loadedFlipWinding = recipe.flipWinding;
        fresh.sourceSize = size;
        fresh.sourceStamp = stamp;
        state = std::move(fresh);
    }
    const bool sameSpace = sameMesh && state.loadedUnits == recipe.units &&
                           state.loadedScale == recipe.scale &&
                           state.loadedOrigin == recipe.origin &&
                           state.loadedUvFlip == recipe.uvFlip;
    if (sameSpace) return true;
    ConvertRequest request;
    if (!MakeRequest(recipe, &state, request, error)) return false;
    if (!BuildModelSpaceMesh(request, state.pieceSet, state.modelMesh, error)) return false;
    state.loadedUnits = recipe.units;
    state.loadedScale = recipe.scale;
    state.loadedOrigin = recipe.origin;
    state.loadedUvFlip = recipe.uvFlip;
    return true;
}

void SyncPieces(Recipe& recipe, const SourceState& state, const LogFn& log) {
    std::vector<RecipePiece> synced;
    std::size_t dropped = 0;
    for (std::size_t i = 0; i < state.pieceSet.pieces.size(); ++i) {
        RecipePiece piece;
        piece.index = static_cast<int>(i);
        piece.triangles = static_cast<int>(state.pieceSet.pieces[i].triangleCount);
        piece.name = state.pieceSet.pieces[i].name;
        for (const RecipePiece& old : recipe.pieces)
            if (old.index == piece.index) {
                if (old.triangles == piece.triangles) piece.surface = old.surface;
                else if (!old.surface.empty()) ++dropped;
            }
        synced.push_back(std::move(piece));
    }
    if (dropped > 0 && log)
        log("Warning: " + std::to_string(dropped) +
            " piece surface overrides no longer match the mesh and were reset");
    recipe.pieces = std::move(synced);
}

bool PieceTypes(const Recipe& recipe, const SourceState& state, std::vector<int>& types,
                std::string& error) {
    types.assign(state.pieceSet.pieces.size(), 0);
    for (const RecipePiece& piece : recipe.pieces) {
        if (piece.surface.empty()) continue;
        if (piece.index < 0 || static_cast<std::size_t>(piece.index) >= types.size() ||
            static_cast<int>(state.pieceSet.pieces[piece.index].triangleCount) !=
                piece.triangles) {
            error = "recipe piece " + std::to_string(piece.index) +
                    " no longer matches the mesh; re-sync the pieces";
            return false;
        }
        int type = 0;
        if (!cod4::ParseSurfaceType(piece.surface, type)) {
            error = "piece " + std::to_string(piece.index) + " surface '" + piece.surface +
                    "' is not a CoD4 surface type";
            return false;
        }
        types[piece.index] = type;
    }
    return true;
}

// ---- collision ---------------------------------------------------------------------

fs::path MasterCollmapPath(const GameRoot& root, const Recipe& recipe) {
    return root.Collmaps() / (recipe.asset + ".map");
}

void GenerateRecipeHulls(const Recipe& recipe, const SourceState& state,
                         std::vector<Hull>& hulls, const LogFn& log) {
    // The same rules as for built models (ki_autocollide): props get automatic hulls,
    // trees only their trunk, other plants none. The source is described as a "raw
    // model" whose surfaces are the recipe's surface types (default + piece overrides).
    RawXModel raw;
    raw.name = recipe.asset;
    raw.positions = state.modelMesh.positions;
    std::vector<int> types;
    std::string ignored;
    if (!PieceTypes(recipe, state, types, ignored)) types.assign(state.pieceSet.pieces.size(), 0);
    int defaultType = cod4::kSurfaceTypeConcrete;
    cod4::ParseSurfaceType(recipe.surface, defaultType);
    std::map<int, std::uint32_t> surfaceOfType;
    for (std::uint32_t piece : state.modelMesh.trianglePiece) {
        const int type = piece < types.size() && types[piece] > 0 ? types[piece] : defaultType;
        auto [it, inserted] =
            surfaceOfType.try_emplace(type, static_cast<std::uint32_t>(raw.surfaces.size()));
        if (inserted) {
            RawXModelSurface surface;
            surface.material = cod4::SurfaceTypeName(type);
            surface.surfaceType = type;
            raw.surfaces.push_back(surface);
        }
        raw.triangleSurface.push_back(it->second);
    }
    const AutoClass cls = ClassifyXModel(raw);
    if (cls == AutoClass::Prop) {
        // Source pieces keep Plasticity bodies apart even where they touch.
        GenerateHulls(state.modelMesh, state.pieceSet, recipe.collision.settings, hulls, log);
        return;
    }
    std::string note;
    AutoHullsForXModel(raw, cls, recipe.collision.settings, TrunkSettings{}, hulls, note, log);
    if (log) log("Collision: " + note);
}

bool RegenerateCollision(Recipe& recipe, const GameRoot& root, SourceState& state, bool force,
                         std::vector<Hull>& hulls, const LogFn& log, std::string& error) {
    if (recipe.collision.mode == "manual" && !force) {
        error = "collision for '" + recipe.asset +
                "' was edited by hand (mode manual); pass force to replace it";
        return false;
    }
    if (!InspectSource(recipe, state, log, error)) return false;
    GenerateRecipeHulls(recipe, state, hulls, log);
    if (recipe.collision.mode != "none") recipe.collision.mode = "auto";
    return WriteCollmap(MasterCollmapPath(root, recipe), hulls, recipe.collision.material,
                        error);
}

bool CurrentHulls(const Recipe& recipe, const GameRoot& root, SourceState& state,
                  std::vector<Hull>& hulls, const LogFn& log, std::string& error) {
    hulls.clear();
    if (recipe.collision.mode == "none") return true;
    if (recipe.collision.mode == "manual") {
        std::string material;
        if (ReadCollmap(MasterCollmapPath(root, recipe), hulls, material, error)) return true;
        if (log) log("Warning: " + error + "; regenerating the hulls");
        error.clear();
    }
    if (!InspectSource(recipe, state, log, error)) return false;
    GenerateRecipeHulls(recipe, state, hulls, log);
    return true;
}

// ---- textures ----------------------------------------------------------------------

namespace {

nlohmann::json ReadSettings(const GameRoot& root) {
    std::ifstream in(SettingsPath(root), std::ios::binary);
    if (!in) return nlohmann::json::object();
    try {
        nlohmann::json settings = nlohmann::json::parse(in);
        return settings.is_object() ? settings : nlohmann::json::object();
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

}  // namespace

std::string GetImportSetting(const GameRoot& root, const std::string& key) {
    const nlohmann::json settings = ReadSettings(root);
    const auto found = settings.find(key);
    return found != settings.end() && found->is_string() ? found->get<std::string>()
                                                         : std::string();
}

bool SetImportSetting(const GameRoot& root, const std::string& key, const std::string& value,
                      std::string& error) {
    nlohmann::json settings = ReadSettings(root);
    settings[key] = value;
    std::error_code ec;
    fs::create_directories(root.ModelSource(), ec);
    std::ofstream out(SettingsPath(root), std::ios::binary | std::ios::trunc);
    out << settings.dump(2) << "\n";
    if (!out) {
        error = "cannot write " + SettingsPath(root).string();
        return false;
    }
    return true;
}

std::string DefaultTextureFolder(const GameRoot& root) {
    if (const char* env = std::getenv("KIWI_TEXTURE_FOLDER"); env && *env) return env;
    return GetImportSetting(root, "textureFolder");
}

bool SaveDefaultTextureFolder(const GameRoot& root, const std::string& folder,
                              std::string& error) {
    return SetImportSetting(root, "textureFolder", folder, error);
}

bool ResolveTextures(const Recipe& recipe, TextureScan& scan,
                     const std::vector<TextureVariant>*& variants, std::string& setName,
                     std::string& error) {
    variants = nullptr;
    if (recipe.textures.prefix.empty()) {
        error = "no texture prefix set for '" + recipe.asset + "'";
        return false;
    }
    if (recipe.textures.folder.empty()) {
        error = "no texture folder set for '" + recipe.asset + "'";
        return false;
    }
    if (!ScanTextureSets(recipe.textures.folder, recipe.textures.prefix, scan, error))
        return false;
    if (scan.sets.empty()) {
        error = "no InstaMAT textures named '" + recipe.textures.prefix + "_...' in " +
                recipe.textures.folder;
        return false;
    }
    if (recipe.textures.set == kAllTextureSets) {
        // One set per source material: the variant list is the first set's
        // (MaterialTextureSets pairs the rest).
        variants = &scan.sets.begin()->second;
        setName = scan.sets.begin()->first;
        return true;
    }
    if (!recipe.textures.set.empty()) {
        for (const auto& [name, list] : scan.sets)
            if (Lower(name) == Lower(recipe.textures.set)) {
                variants = &list;
                setName = name;
            }
        if (!variants) {
            error = "texture set '" + recipe.textures.set + "' not found for prefix '" +
                    recipe.textures.prefix + "'";
            return false;
        }
        return true;
    }
    if (scan.sets.size() > 1u) {
        std::string names;
        for (const auto& [name, list] : scan.sets) names += (names.empty() ? "" : ", ") + name;
        error = "prefix '" + recipe.textures.prefix + "' has several texture sets (" + names +
                "); set textures.set to one of them, or \"" + kAllTextureSets +
                "\" for one set per source material";
        return false;
    }
    variants = &scan.sets.begin()->second;
    setName = scan.sets.begin()->first;
    return true;
}

bool MaterialTextureSets(const TextureScan& scan, const std::vector<std::string>& materialNames,
                         std::vector<const std::vector<TextureVariant>*>& perMaterial,
                         std::vector<std::string>& setNames, std::string& error) {
    perMaterial.clear();
    setNames.clear();
    if (materialNames.size() < 2u) {
        error = "the source has " + std::to_string(materialNames.size()) +
                " material(s); one set per material needs several";
        return false;
    }
    for (const std::string& material : materialNames) {
        // Blender numbers duplicate material names "Default.001"; InstaMAT names the
        // matching texture set "Default_1".
        std::string wanted = material;
        const std::size_t dot = wanted.rfind('.');
        if (dot != std::string::npos && dot + 1u < wanted.size() &&
            wanted.find_first_not_of("0123456789", dot + 1u) == std::string::npos)
            wanted = wanted.substr(0, dot) + "_" + std::to_string(std::stoi(wanted.substr(dot + 1u)));
        const std::vector<TextureVariant>* found = nullptr;
        for (const auto& [name, list] : scan.sets)
            if (Lower(name) == Lower(wanted)) {
                found = &list;
                setNames.push_back(name);
            }
        if (!found) {
            error = "source material '" + material + "' has no texture set '" + wanted + "'";
            return false;
        }
        perMaterial.push_back(found);
    }
    return true;
}

std::string GuessTexturePrefix(const fs::path& source, const fs::path& folder) {
    std::string ignored;
    const std::vector<std::string> prefixes = ListTexturePrefixes(folder, ignored);
    const std::string stem = Lower(source.stem().string());
    const std::string stemKey = AlnumKey(stem);
    // 1. exact, 2. same letters/digits ("tri_window" vs "triwindow"),
    // 3. one ends with the other on a token boundary,
    // 4. the stem ends with a multi-token prefix of 8+ characters even mid-word
    //    ("waterpipe_holder" source, "pipe_holder" textures). Longest wins per tier.
    for (const std::string& prefix : prefixes)
        if (Lower(prefix) == stem) return prefix;
    for (const std::string& prefix : prefixes)
        if (AlnumKey(prefix) == stemKey) return prefix;
    std::string best;
    for (const std::string& prefix : prefixes) {
        const std::string lower = Lower(prefix);
        if ((EndsWithToken(stem, lower) || EndsWithToken(lower, stem)) &&
            prefix.size() > best.size())
            best = prefix;
    }
    if (!best.empty()) return best;
    for (const std::string& prefix : prefixes) {
        const std::string lower = Lower(prefix);
        if (lower.size() >= 8u && lower.find('_') != std::string::npos &&
            stem.size() > lower.size() &&
            stem.compare(stem.size() - lower.size(), lower.size(), lower) == 0 &&
            prefix.size() > best.size())
            best = prefix;
    }
    return best;
}

// ---- recipes -----------------------------------------------------------------------

std::vector<SourceEntry> ScanSources(const GameRoot& root, const fs::path& folder,
                                     double sinceHours, std::string& error) {
    std::vector<SourceEntry> list;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    const std::string textureFolder = DefaultTextureFolder(root);
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        const std::string ext = Lower(it->path().extension().string());
        if (ext != ".fbx" && ext != ".obj") continue;
        if (ext == ".obj" && fs::exists(fs::path(it->path()).replace_extension(".fbx"), fileError))
            continue;
        const auto written = it->last_write_time(fileError);
        const double ageHours = std::chrono::duration<double>(now - written).count() / 3600.0;
        if (sinceHours > 0.0 && ageHours > sinceHours) continue;
        SourceEntry entry;
        entry.source = it->path();
        entry.ageHours = ageHours;
        if (!textureFolder.empty())
            entry.texturePrefixGuess = GuessTexturePrefix(it->path(), textureFolder);
        // The name an import with the defaults and this prefix would really get.
        NewRecipeOptions options;
        options.texturePrefix = entry.texturePrefixGuess;
        entry.asset = PlannedAssetName(it->path(), options, root);
        entry.hasRecipe = fs::exists(RecipePath(root, entry.asset), fileError);
        list.push_back(std::move(entry));
    }
    if (ec) error = "cannot scan " + folder.string() + ": " + ec.message();
    std::sort(list.begin(), list.end(),
              [](const SourceEntry& a, const SourceEntry& b) { return a.ageHours < b.ageHours; });
    return list;
}

std::string NewRecipeAssetName(const fs::path& source, const NewRecipeOptions& options) {
    std::string asset = options.asset.empty()
                            ? xmodel::SanitizeModelName("kiwi_" + source.stem().string())
                            : xmodel::SanitizeModelName(options.asset);
    if (asset.size() > 46u) asset.resize(46u);
    return asset;
}

namespace {

// Every variant name (<asset>_<id>) must fit the 46-character limit the _col/_nml/_spc
// image suffixes leave, so a long asset name is shortened for its longest variant id
// (plus "_m<k>" for a multi-material source).
std::string FitAssetToVariants(std::string asset, const std::vector<TextureVariant>& variants,
                               std::size_t materialCount = 1) {
    std::size_t longestSuffix = 0;
    for (const TextureVariant& variant : variants)
        if (variant.id != "1") longestSuffix = std::max(longestSuffix, variant.id.size() + 1u);
    if (materialCount > 1u) longestSuffix += 2u + std::to_string(materialCount - 1u).size();
    if (asset.size() + longestSuffix > 46u) {
        asset.resize(46u - longestSuffix);
        while (!asset.empty() && asset.back() == '_') asset.pop_back();
    }
    return asset;
}

}  // namespace

std::string PlannedAssetName(const fs::path& source, const NewRecipeOptions& options,
                             const GameRoot& root) {
    const std::string asset = NewRecipeAssetName(source, options);
    Recipe probe;
    probe.asset = asset;
    probe.textures.folder =
        options.textureFolder.empty() ? DefaultTextureFolder(root) : options.textureFolder;
    probe.textures.prefix =
        !options.texturePrefix.empty()  ? options.texturePrefix
        : probe.textures.folder.empty() ? std::string()
                                        : GuessTexturePrefix(source, probe.textures.folder);
    if (probe.textures.folder.empty() || probe.textures.prefix.empty()) return asset;
    TextureScan scan;
    const std::vector<TextureVariant>* variants = nullptr;
    std::string setName;
    std::string error;
    if (!ResolveTextures(probe, scan, variants, setName, error)) {
        // Several sets: a multi-material source, one set per material.
        if (scan.sets.size() < 2u) return asset;
        return FitAssetToVariants(asset, scan.sets.begin()->second, scan.sets.size());
    }
    return FitAssetToVariants(asset, *variants);
}

bool NewRecipe(const fs::path& source, const NewRecipeOptions& options, const GameRoot& root,
               Recipe& recipe, SourceState& state, const LogFn& log, std::string& error) {
    recipe = {};
    recipe.source = source.string();
    recipe.asset = NewRecipeAssetName(source, options);
    recipe.units = options.units;
    recipe.surface = options.surface;
    recipe.textures.folder =
        options.textureFolder.empty() ? DefaultTextureFolder(root) : options.textureFolder;
    if (recipe.textures.folder.empty() && log)
        log("Warning: no texture folder (set KIWI_TEXTURE_FOLDER or textures.folder)");
    recipe.textures.prefix =
        !options.texturePrefix.empty()   ? options.texturePrefix
        : recipe.textures.folder.empty() ? std::string()
                                         : GuessTexturePrefix(source, recipe.textures.folder);
    if (!InspectSource(recipe, state, log, error)) return false;
    SyncPieces(recipe, state, log);

    if (recipe.textures.prefix.empty()) {
        if (log)
            log("Warning: no texture prefix matches '" + source.stem().string() + "' in " +
                recipe.textures.folder + "; set textures.prefix");
    } else {
        TextureScan scan;
        const std::vector<TextureVariant>* variants = nullptr;
        std::string setName;
        std::string textureError;
        if (ResolveTextures(recipe, scan, variants, setName, textureError)) {
            recipe.textures.set = setName;
            for (const TextureVariant& variant : *variants)
                recipe.textures.variants.push_back({variant.id, !variant.files.baseColor.empty()});
            const std::string fitted = FitAssetToVariants(recipe.asset, *variants);
            if (fitted != recipe.asset) {
                recipe.asset = fitted;
                if (log) log("Asset name shortened to '" + recipe.asset + "' to fit the variants");
            }
        } else if (scan.sets.size() > 1u) {
            // Several sets: one per source material when every material has one.
            std::vector<const std::vector<TextureVariant>*> perMaterial;
            std::vector<std::string> setNames;
            std::string materialError;
            if (MaterialTextureSets(scan, state.mesh.materialNames, perMaterial, setNames,
                                    materialError)) {
                recipe.textures.set = kAllTextureSets;
                for (std::size_t k = 0; k < setNames.size() && log; ++k)
                    log("Material '" + state.mesh.materialNames[k] + "' -> texture set '" +
                        setNames[k] + "'");
                for (const TextureVariant& variant : *perMaterial[0])
                    recipe.textures.variants.push_back({variant.id, !variant.files.baseColor.empty()});
                const std::string fitted =
                    FitAssetToVariants(recipe.asset, *perMaterial[0], perMaterial.size());
                if (fitted != recipe.asset) {
                    recipe.asset = fitted;
                    if (log) log("Asset name shortened to '" + recipe.asset + "' to fit the variants");
                }
            } else if (log) {
                log("Warning: " + textureError + " (" + materialError + ")");
            }
        } else if (log) {
            log("Warning: " + textureError);
        }
    }
    std::vector<Hull> hulls;
    return RegenerateCollision(recipe, root, state, false, hulls, log, error);
}

bool BuildRecipe(const Recipe& recipe, const GameRoot& root, SourceState& state,
                 const LogFn& log, BuildReport& report, std::string& error) {
    report = {};
    if (!InspectSource(recipe, state, log, error)) return false;
    std::vector<int> types;
    if (!PieceTypes(recipe, state, types, error)) return false;

    TextureScan scan;
    const std::vector<TextureVariant>* variants = nullptr;
    std::string setName;
    if (!ResolveTextures(recipe, scan, variants, setName, error)) return false;
    std::vector<const std::vector<TextureVariant>*> perMaterial;
    std::vector<std::string> materialSets;
    if (recipe.textures.set == kAllTextureSets &&
        !MaterialTextureSets(scan, state.mesh.materialNames, perMaterial, materialSets, error))
        return false;

    // auto: rebuilt from the mesh every build (units/scale/origin edits can never
    // leave stale hulls); manual: the hand-edited master, as authored.
    std::vector<Hull> hulls;
    if (!CurrentHulls(recipe, root, state, hulls, log, error)) return false;
    const bool generated = recipe.collision.mode == "auto";

    std::size_t builtCount = 0;
    for (const RecipeVariant& wanted : recipe.textures.variants) {
        if (!wanted.enabled) continue;
        const TextureVariant* variant = nullptr;
        for (const TextureVariant& candidate : *variants)
            if (Lower(candidate.id) == Lower(wanted.id)) variant = &candidate;
        if (!variant) {
            report.warnings.push_back("variant '" + wanted.id + "' has no textures; skipped");
            if (log) log("Warning: " + report.warnings.back());
            continue;
        }
        if (variant->files.baseColor.empty()) {
            report.warnings.push_back("variant '" + wanted.id + "' has no BaseColor; skipped");
            if (log) log("Warning: " + report.warnings.back());
            continue;
        }
        // Multi-material: the same variant id from every material's set.
        std::vector<TextureFiles> materialFiles;
        std::string missingSet;
        for (std::size_t k = 0; k < perMaterial.size() && missingSet.empty(); ++k) {
            const TextureVariant* match = nullptr;
            for (const TextureVariant& candidate : *perMaterial[k])
                if (Lower(candidate.id) == Lower(wanted.id)) match = &candidate;
            if (!match || match->files.baseColor.empty()) missingSet = materialSets[k];
            else materialFiles.push_back(match->files);
        }
        if (!missingSet.empty()) {
            report.warnings.push_back("variant '" + wanted.id + "' has no BaseColor in set '" +
                                      missingSet + "'; skipped");
            if (log) log("Warning: " + report.warnings.back());
            continue;
        }
        ConvertRequest request;
        if (!MakeRequest(recipe, &state, request, error)) return false;
        request.assetName = VariantAssetName(recipe.asset, variant->id);
        request.pieceType = types;
        request.textures = variant->files;
        request.materialTextures = std::move(materialFiles);
        request.outputRoot = root.Raw();
        request.templateRoot = root.Raw();
        if (log) log("== " + request.assetName + " (texture variant " + variant->id + ")");
        ConvertResult result;
        if (!Convert(request, log, result, error)) {
            error = request.assetName + ": " + error;
            return false;
        }

        BuildReport::Built built;
        built.asset = request.assetName;
        built.variant = variant->id;
        const fs::path collmap = root.Collmaps() / (request.assetName + ".map");
        if (!hulls.empty() && !generated && collmap == MasterCollmapPath(root, recipe)) {
            // Manual mode, variant 1: this IS the hand-edited master - leave it alone.
            built.hulls = hulls.size();
            built.collmap = collmap;
        } else if (!hulls.empty()) {
            // Generated hulls live in the space this build computed; the writer's own
            // offset is authoritative (identical in practice; shift if it ever is not).
            std::vector<Hull> placed = hulls;
            if (generated) {
                const std::array<double, 3> delta{
                    state.modelMesh.originOffsetInches[0] - result.originOffsetInches[0],
                    state.modelMesh.originOffsetInches[1] - result.originOffsetInches[1],
                    state.modelMesh.originOffsetInches[2] - result.originOffsetInches[2]};
                if (delta[0] != 0.0 || delta[1] != 0.0 || delta[2] != 0.0)
                    for (Hull& hull : placed) TranslateHull(hull, delta);
            }
            if (!WriteCollmap(collmap, placed, recipe.collision.material, error)) return false;
            built.hulls = placed.size();
            built.collmap = collmap;
            if (log)
                log("Collmap: " + collmap.string() + " (" + std::to_string(placed.size()) +
                    " " + recipe.collision.material + " hulls, " + recipe.collision.mode + ")");
        } else {
            std::error_code ec;
            if (fs::remove(collmap, ec) && log) log("Collmap removed: " + collmap.string());
        }
        report.built.push_back(std::move(built));
        ++builtCount;
    }
    if (builtCount == 0) {
        error = "no enabled texture variant could be built";
        return false;
    }
    return true;
}

}  // namespace kimport
