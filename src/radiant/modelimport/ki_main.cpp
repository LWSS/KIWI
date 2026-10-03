// KIWI-modelimport.exe - the model importer's command line and MCP server.
//
//   KIWI-modelimport new <source.fbx|obj> [--asset <name>] [--surface <type>]
//                        [--units in|m|cm|mm] [--textures <folder>] [--prefix <p>] [--force]
//   KIWI-modelimport build <asset|recipe.kimport>
//   KIWI-modelimport inspect <asset|recipe.kimport>
//   KIWI-modelimport collision <asset|recipe.kimport> [--shape auto|box|kdop]
//                        [--min-size <in>] [--force]
//   KIWI-modelimport scan <folder> [--since <hours>]
//   KIWI-modelimport collide <name pattern|*> [--material <clip>] [--dry-run]
//                        player collision for already-built raw\xmodel models (no recipe)
//   KIWI-modelimport dump <xmodel name>   read back a built raw\xmodel
// KIWI GLASS
//   KIWI-modelimport material <name> --template <stock material> --color <png> [--normal <png>]
//                        [--spec <png>] [--surface <type>] [--normal-green keep|flip]
//                        material over authored PNGs (e.g. glass)
// KIWI END
//   KIWI-modelimport --mcp   MCP server on stdin/stdout (JSON-RPC 2.0, one message per line)
//
// Recipes live in <game root>\model_source\<asset>.kimport; the game root is the exe's
// folder (bin\Debug) unless KIWI_GAME_ROOT says otherwise. Player-collision hulls live in
// <game root>\collmaps\<asset>.map (the master; texture variants get copies at build).

#include "ki_autocollide.h"
#include "ki_build.h"
#include "ki_xmodel_read.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace kimport;

std::string Lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

fs::path ResolveRecipePath(const GameRoot& root, const std::string& assetOrPath) {
    if (Lower(fs::path(assetOrPath).extension().string()) == ".kimport") return assetOrPath;
    return RecipePath(root, xmodel::SanitizeModelName(assetOrPath));
}

// ---- shared operations (CLI + MCP) ---------------------------------------------

struct Session {
    GameRoot root = DefaultGameRoot();
    std::map<std::string, SourceState> sources;   // by recipe source path
    std::vector<std::string> log;
    bool echo = true;                              // CLI prints log lines as they come

    LogFn Logger() {
        return [this](const std::string& line) {
            log.push_back(line);
            if (echo) std::fprintf(stderr, "%s\n", line.c_str());
        };
    }
    SourceState& StateFor(const Recipe& recipe) { return sources[Lower(recipe.source)]; }
};

json BoundsJson(const ModelSpaceMesh& mesh) {
    return {{"min", {mesh.boundsMin[0], mesh.boundsMin[1], mesh.boundsMin[2]}},
            {"max", {mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2]}}};
}

json InspectJson(Session& session, const Recipe& recipe, std::string& error, bool& ok) {
    ok = false;
    SourceState& state = session.StateFor(recipe);
    if (!InspectSource(recipe, state, session.Logger(), error)) return {};
    json pieces = json::array();
    for (std::size_t i = 0; i < state.pieceSet.pieces.size(); ++i) {
        const auto& piece = state.pieceSet.pieces[i];
        std::string surface;
        for (const RecipePiece& rp : recipe.pieces)
            if (rp.index == static_cast<int>(i)) surface = rp.surface;
        pieces.push_back({{"index", i},
                          {"name", piece.name},
                          {"triangles", piece.triangleCount},
                          {"surface", surface.empty() ? "(default)" : surface}});
    }
    // The master collmap is the hull truth (written at create/regenerate/build).
    json hulls = json::array();
    {
        std::vector<Hull> read;
        std::string material;
        std::string readError;
        if (ReadCollmap(MasterCollmapPath(session.root, recipe), read, material, readError))
            for (const Hull& hull : read)
                hulls.push_back({{"planes", hull.planes.size()},
                                 {"volumeCubicInches", std::round(HullVolume(hull))}});
    }
    json textures = json::object();
    {
        TextureScan scan;
        const std::vector<TextureVariant>* variants = nullptr;
        std::string setName;
        std::string textureError;
        if (ResolveTextures(recipe, scan, variants, setName, textureError)) {
            json list = json::array();
            for (const TextureVariant& v : *variants)
                list.push_back({{"id", v.id},
                                {"asset", VariantAssetName(recipe.asset, v.id)},
                                {"baseColor", v.files.baseColor.filename().string()},
                                {"normal", !v.files.normal.empty()},
                                {"metalness", !v.files.metalness.empty()},
                                {"roughness", !v.files.roughness.empty()},
                                {"opacity", !v.files.opacity.empty()},
                                {"size", std::to_string(v.width) + "x" + std::to_string(v.height)}});
            textures = {{"set", setName}, {"variants", list}};
        } else {
            textures = {{"error", textureError}};
        }
    }
    ok = true;
    return {{"asset", recipe.asset},
            {"source", recipe.source},
            {"materials", state.mesh.materialNames},
            {"boundsInches", BoundsJson(state.modelMesh)},
            {"triangles", state.modelMesh.trianglePiece.size()},
            {"pieces", pieces},
            {"textures", textures},
            {"collision",
             {{"mode", recipe.collision.mode},
              {"material", recipe.collision.material},
              {"collmap", MasterCollmapPath(session.root, recipe).string()},
              {"hulls", hulls}}}};
}

bool LoadOrError(Session& session, const std::string& assetOrPath, Recipe& recipe,
                 fs::path& path, std::string& error) {
    path = ResolveRecipePath(session.root, assetOrPath);
    return LoadRecipe(path, recipe, error);
}

json ScanJson(Session& session, const fs::path& folder, double sinceHours, std::string& error) {
    json list = json::array();
    for (const SourceEntry& entry : ScanSources(session.root, folder, sinceHours, error))
        list.push_back({{"source", entry.source.string()},
                        {"ageHours", std::round(entry.ageHours * 10.0) / 10.0},
                        {"asset", entry.asset},
                        {"hasRecipe", entry.hasRecipe},
                        {"texturePrefixGuess", entry.texturePrefixGuess}});
    return list;
}

// ---- MCP ----------------------------------------------------------------------

json Tool(const char* name, const char* description, json properties, json required) {
    return {{"name", name},
            {"description", description},
            {"inputSchema",
             {{"type", "object"}, {"properties", properties}, {"required", required}}}};
}

json ToolList() {
    const json asset = {{"type", "string"}, {"description", "asset name or .kimport path"}};
    return json::array({
        Tool("scan_sources",
             "List FBX/OBJ sources in a folder (optionally only those modified in the last N "
             "hours), whether each already has a recipe, and the guessed InstaMAT texture prefix.",
             {{"folder", {{"type", "string"}}}, {"sinceHours", {{"type", "number"}}}},
             {"folder"}),
        Tool("list_surface_types", "CoD4 surface type names usable for surfaces.", json::object(),
             json::array()),
        Tool("list_texture_sets",
             "InstaMAT texture sets and variants for a prefix in a texture folder.",
             {{"prefix", {{"type", "string"}}},
              {"folder", {{"type", "string"}, {"description", "default: the configured texture folder"}}}},
             {"prefix"}),
        Tool("set_texture_folder",
             "Set the default InstaMAT export folder (model_source/settings.json).",
             {{"folder", {{"type", "string"}}}}, {"folder"}),
        Tool("create_recipe",
             "Create the import recipe for a source model: pieces, texture variants (all found), "
             "automatic collision hulls. Writes model_source/<asset>.kimport.",
             {{"source", {{"type", "string"}}},
              {"asset", {{"type", "string"}, {"description", "default kiwi_<source stem>"}}},
              {"surface", {{"type", "string"}, {"description", "default surface type, e.g. metal"}}},
              {"units", {{"type", "string"}, {"enum", {"in", "m", "cm", "mm"}}}},
              {"textureFolder", {{"type", "string"}}},
              {"texturePrefix", {{"type", "string"}}},
              {"overwrite", {{"type", "boolean"}}}},
             {"source"}),
        Tool("collide_xmodels",
             "Player collision for models that are ALREADY BUILT in raw\\xmodel (no recipe "
             "needed): props get automatic hulls, trees get trunk-only hulls, grass/herbs/"
             "scrub/bushes/boughs get none. Writes collmaps/<model>.map; skips models a recipe "
             "builds and hand-made collmaps.",
             {{"pattern", {{"type", "string"}, {"description", "substring of the model name; empty = all"}}},
              {"dryRun", {{"type", "boolean"}}},
              {"material", {{"type", "string"}, {"description", "default clip"}}}},
             json::array()),
        Tool("inspect_xmodel", "Surfaces, materials and surface types of a built xmodel.",
             {{"name", {{"type", "string"}}}}, {"name"}),
        Tool("list_recipes", "All recipes in model_source.", json::object(), json::array()),
        Tool("delete_recipe",
             "Delete a recipe (and its master collmap unless keepCollmap). Built assets in raw "
             "are left alone.",
             {{"asset", asset}, {"keepCollmap", {{"type", "boolean"}}}}, {"asset"}),
        Tool("get_recipe", "The full recipe JSON.", {{"asset", asset}}, {"asset"}),
        Tool("inspect", "Pieces (index, name, triangles, surface, hulls), bounds in inches, "
                        "texture variants and collision summary for a recipe.",
             {{"asset", asset}}, {"asset"}),
        Tool("update_recipe",
             "Change recipe fields. `changes` is merged into the recipe JSON (objects merge, "
             "arrays replace), e.g. {\"surface\":\"metal\",\"textures\":{\"prefix\":\"pipe_holder\"},"
             "\"collision\":{\"material\":\"clip_nosight_metal\"}}.",
             {{"asset", asset}, {"changes", {{"type", "object"}}}}, {"asset", "changes"}),
        Tool("set_piece_surfaces",
             "Per-piece surface types: [{\"index\":3,\"surface\":\"glass\"}]; surface \"\" resets "
             "to the recipe default.",
             {{"asset", asset},
              {"surfaces", {{"type", "array"}, {"items", {{"type", "object"}}}}}},
             {"asset", "surfaces"}),
        Tool("set_variants",
             "Enable exactly these texture variant ids (others disabled).",
             {{"asset", asset}, {"variants", {{"type", "array"}, {"items", {{"type", "string"}}}}}},
             {"asset", "variants"}),
        Tool("regenerate_collision",
             "Rebuild the automatic collision hulls (convex clip brushes; concave pieces are "
             "split) into collmaps/<asset>.map. Refuses when the hulls were hand-edited "
             "(mode manual) unless force is true.",
             {{"asset", asset},
              {"shape", {{"type", "string"}, {"enum", {"auto", "box", "kdop"}}}},
              {"minPieceSize", {{"type", "number"}, {"description", "inches"}}},
              {"minThickness", {{"type", "number"}, {"description", "inches"}}},
              {"force", {{"type", "boolean"}}}},
             {"asset"}),
        Tool("build",
             "Build every enabled texture variant into raw xmodels/materials/images plus a "
             "collmaps/<asset>.map player-collision hull.",
             {{"asset", asset}}, {"asset"}),
    });
}

json MergePatch(json target, const json& patch) {
    if (!patch.is_object()) return patch;
    if (!target.is_object()) target = json::object();
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        if (it.value().is_null()) target.erase(it.key());
        else target[it.key()] = MergePatch(target[it.key()], it.value());
    }
    return target;
}

bool CallTool(Session& session, const std::string& name, const json& args, json& out,
              std::string& error) {
    auto str = [&](const char* key, const std::string& fallback = "") {
        return args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : fallback;
    };
    if (name == "list_surface_types") {
        json list = json::array();
        for (int i = 1; i < cod4::kSurfaceTypeCount; ++i) list.push_back(cod4::SurfaceTypeName(i));
        out = list;
        return true;
    }
    if (name == "scan_sources") {
        const double since = args.contains("sinceHours") ? args["sinceHours"].get<double>() : 0.0;
        out = ScanJson(session, str("folder"), since, error);
        return error.empty();
    }
    if (name == "collide_xmodels") {
        // Every built xmodel whose name contains `pattern` (all when empty).
        const std::string pattern = Lower(str("pattern"));
        const bool dryRun = args.contains("dryRun") && args["dryRun"].get<bool>();
        const std::string material = str("material", "clip");
        // Assets a recipe builds are the recipe's business.
        std::map<std::string, std::string> owned;   // asset -> recipe
        {
            std::error_code ec;
            for (fs::directory_iterator it(session.root.ModelSource(), ec), end; !ec && it != end;
                 it.increment(ec)) {
                if (Lower(it->path().extension().string()) != ".kimport") continue;
                Recipe owner;
                std::string ignored;
                if (!LoadRecipe(it->path(), owner, ignored)) continue;
                owned[owner.asset] = owner.asset;
                for (const RecipeVariant& v : owner.textures.variants)
                    owned[VariantAssetName(owner.asset, v.id)] = owner.asset;
            }
        }
        json results = json::array();
        std::map<std::string, int> counts;
        std::error_code ec;
        for (fs::directory_iterator it(session.root.Raw() / "xmodel", ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code fileError;
            if (!it->is_regular_file(fileError)) continue;
            const std::string model = it->path().filename().string();
            if (!pattern.empty() && Lower(model).find(pattern) == std::string::npos) continue;
            json entry = {{"model", model}};
            const fs::path collmap = session.root.Collmaps() / (model + ".map");
            if (owned.count(model)) {
                entry["action"] = "skipped (recipe " + owned[model] + " builds it)";
                ++counts["recipe"];
                results.push_back(entry);
                continue;
            }
            RawXModel raw;
            std::string readError;
            if (!ReadRawXModel(session.root.Raw(), model, raw, readError)) {
                entry["action"] = "skipped (" + readError + ")";
                ++counts["unreadable"];
                results.push_back(entry);
                continue;
            }
            if (raw.multiBone) {
                entry["action"] = "skipped (multi-bone model)";
                ++counts["unreadable"];
                results.push_back(entry);
                continue;
            }
            const AutoClass cls = ClassifyXModel(raw);
            entry["class"] = AutoClassName(cls);
            const bool exists = fs::exists(collmap, fileError);
            if (exists && !IsGeneratedCollmap(collmap)) {
                entry["action"] = "skipped (hand-made collmap kept)";
                ++counts["hand-made"];
                results.push_back(entry);
                continue;
            }
            std::vector<Hull> hulls;
            std::string note;
            CollisionSettings settings;
            AutoHullsForXModel(raw, cls, settings, TrunkSettings{}, hulls, note, LogFn{});
            entry["hulls"] = hulls.size();
            entry["note"] = note;
            if (hulls.empty()) {
                if (exists && !dryRun) fs::remove(collmap, fileError);
                entry["action"] = exists ? "collmap removed" : "no collision";
                ++counts[std::string(AutoClassName(cls)) + " (none)"];
            } else {
                if (!dryRun &&
                    !WriteCollmap(collmap, hulls, material, error,
                                  std::string("auto ") + AutoClassName(cls)))
                    return false;
                entry["action"] = dryRun ? "would write" : "written";
                ++counts[AutoClassName(cls)];
            }
            results.push_back(entry);
        }
        out = {{"dryRun", dryRun}, {"counts", counts}, {"models", results}};
        return true;
    }
    if (name == "inspect_xmodel") {
        RawXModel model;
        if (!ReadRawXModel(session.root.Raw(), str("name"), model, error)) return false;
        json surfaces = json::array();
        for (const RawXModelSurface& surface : model.surfaces) {
            std::array<float, 3> lo{1e30f, 1e30f, 1e30f};
            std::array<float, 3> hi{-1e30f, -1e30f, -1e30f};
            for (std::uint32_t t = surface.firstTriangle;
                 t < surface.firstTriangle + surface.triangleCount; ++t)
                for (int c = 0; c < 3; ++c)
                    for (int a = 0; a < 3; ++a) {
                        lo[a] = std::min(lo[a], model.positions[t * 3 + c][a]);
                        hi[a] = std::max(hi[a], model.positions[t * 3 + c][a]);
                    }
            surfaces.push_back({{"material", surface.material},
                                {"surfaceType", cod4::SurfaceTypeName(surface.surfaceType)},
                                {"technique", surface.techniqueSet},
                                {"gameFlags", surface.gameFlags},
                                {"triangles", surface.triangleCount},
                                {"min", lo},
                                {"max", hi}});
        }
        out = {{"name", model.name},
               {"multiBone", model.multiBone},
               {"min", model.boundsMin},
               {"max", model.boundsMax},
               {"surfaces", surfaces}};
        return true;
    }
    if (name == "set_texture_folder") {
        if (!SaveDefaultTextureFolder(session.root, str("folder"), error)) return false;
        out = {{"textureFolder", DefaultTextureFolder(session.root)}};
        return true;
    }
    if (name == "list_texture_sets") {
        TextureScan scan;
        if (!ScanTextureSets(str("folder", DefaultTextureFolder(session.root)), str("prefix"), scan,
                             error))
            return false;
        out = json::object();
        for (const auto& [set, variants] : scan.sets) {
            json list = json::array();
            for (const TextureVariant& v : variants)
                list.push_back({{"id", v.id}, {"baseColor", v.files.baseColor.string()},
                                {"hasNormal", !v.files.normal.empty()},
                                {"hasOpacity", !v.files.opacity.empty()}});
            out[set] = list;
        }
        return true;
    }
    if (name == "list_recipes") {
        out = json::array();
        std::error_code ec;
        for (fs::directory_iterator it(session.root.ModelSource(), ec), end; !ec && it != end;
             it.increment(ec))
            if (Lower(it->path().extension().string()) == ".kimport")
                out.push_back(it->path().stem().string());
        return true;
    }
    if (name == "create_recipe") {
        NewRecipeOptions options;
        options.asset = str("asset");
        options.surface = str("surface", "concrete");
        options.units = str("units", "in");
        options.textureFolder = str("textureFolder");
        options.texturePrefix = str("texturePrefix");
        const bool overwrite = args.contains("overwrite") && args["overwrite"].get<bool>();
        std::error_code ec;
        // Checked before loading: NewRecipe runs Blender and writes the master collmap.
        const std::string plannedAsset = PlannedAssetName(str("source"), options, session.root);
        const fs::path planned = RecipePath(session.root, plannedAsset);
        if (fs::exists(planned, ec) && !overwrite) {
            error = "recipe " + planned.string() + " already exists (pass overwrite:true)";
            return false;
        }
        Recipe named;
        named.asset = plannedAsset;
        const fs::path master = MasterCollmapPath(session.root, named);
        if (fs::exists(master, ec) && !IsGeneratedCollmap(master) && !overwrite) {
            error = master.string() + " holds hand-made collision that importing would replace "
                    "(pass overwrite:true)";
            return false;
        }
        Recipe recipe;
        SourceState state;
        if (!NewRecipe(str("source"), options, session.root, recipe, state, session.Logger(),
                       error))
            return false;
        const fs::path path = RecipePath(session.root, recipe.asset);
        if (path != planned && fs::exists(path, ec) && !overwrite) {
            error = "recipe " + path.string() + " already exists (pass overwrite:true)";
            return false;
        }
        if (!SaveRecipe(path, recipe, error)) return false;
        session.sources[Lower(recipe.source)] = std::move(state);
        bool ok = false;
        out = InspectJson(session, recipe, error, ok);
        out["recipe"] = path.string();
        return ok;
    }

    // Everything below works on an existing recipe.
    Recipe recipe;
    fs::path path;
    if (!LoadOrError(session, str("asset"), recipe, path, error)) return false;

    if (name == "get_recipe") {
        out = json::parse(RecipeToJson(recipe));
        return true;
    }
    if (name == "inspect") {
        bool ok = false;
        out = InspectJson(session, recipe, error, ok);
        return ok;
    }
    if (name == "update_recipe") {
        const json merged = MergePatch(json::parse(RecipeToJson(recipe)), args["changes"]);
        Recipe updated;
        if (!RecipeFromJson(merged.dump(), updated, error)) return false;
        // Validate before saving: units/origin/surface names.
        if (UnitsFactor(updated.units) <= 0.0) { error = "bad units"; return false; }
        int type = 0;
        if (!cod4::ParseSurfaceType(updated.surface, type) || type <= 0) {
            error = "bad surface '" + updated.surface + "'";
            return false;
        }
        if (!SaveRecipe(path, updated, error)) return false;
        // Auto hulls follow units/scale/origin/collision settings right away, so the
        // master collmap (and inspect) never lag the recipe.
        if (updated.collision.mode == "auto") {
            std::vector<Hull> hulls;
            if (!RegenerateCollision(updated, session.root, session.StateFor(updated), false,
                                     hulls, session.Logger(), error))
                return false;
        }
        bool ok = false;
        out = InspectJson(session, updated, error, ok);
        return ok;
    }
    if (name == "delete_recipe") {
        std::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            error = "cannot delete " + path.string() + ": " + ec.message();
            return false;
        }
        const bool keep = args.contains("keepCollmap") && args["keepCollmap"].get<bool>();
        if (!keep) fs::remove(MasterCollmapPath(session.root, recipe), ec);
        out = {{"deleted", path.string()}};
        return true;
    }
    if (name == "set_piece_surfaces") {
        SourceState& state = session.StateFor(recipe);
        if (!InspectSource(recipe, state, session.Logger(), error)) return false;
        SyncPieces(recipe, state, session.Logger());
        for (const json& entry : args["surfaces"]) {
            const int index = entry.value("index", -1);
            const std::string surface = entry.value("surface", std::string());
            int type = 0;
            if (!surface.empty() && (!cod4::ParseSurfaceType(surface, type) || type <= 0)) {
                error = "bad surface '" + surface + "'";
                return false;
            }
            if (index < 0 || index >= static_cast<int>(recipe.pieces.size())) {
                error = "no piece " + std::to_string(index);
                return false;
            }
            recipe.pieces[index].surface = surface;
        }
        if (!SaveRecipe(path, recipe, error)) return false;
        bool ok = false;
        out = InspectJson(session, recipe, error, ok);
        return ok;
    }
    if (name == "set_variants") {
        std::vector<std::string> wanted;
        for (const json& id : args["variants"]) wanted.push_back(Lower(id.get<std::string>()));
        for (RecipeVariant& variant : recipe.textures.variants)
            variant.enabled =
                std::find(wanted.begin(), wanted.end(), Lower(variant.id)) != wanted.end();
        for (const std::string& id : wanted) {
            bool known = false;
            for (const RecipeVariant& variant : recipe.textures.variants)
                known |= Lower(variant.id) == id;
            if (!known) recipe.textures.variants.push_back({id, true});
        }
        if (!SaveRecipe(path, recipe, error)) return false;
        out = json::parse(RecipeToJson(recipe))["textures"];
        return true;
    }
    if (name == "regenerate_collision") {
        if (args.contains("shape")) recipe.collision.settings.shape = args["shape"].get<std::string>();
        if (args.contains("minPieceSize"))
            recipe.collision.settings.minPieceSize = args["minPieceSize"].get<double>();
        if (args.contains("minThickness"))
            recipe.collision.settings.minThickness = args["minThickness"].get<double>();
        const bool force = args.contains("force") && args["force"].get<bool>();
        std::vector<Hull> generated;
        if (!RegenerateCollision(recipe, session.root, session.StateFor(recipe), force, generated,
                                 session.Logger(), error))
            return false;
        if (!SaveRecipe(path, recipe, error)) return false;
        json hulls = json::array();
        for (const Hull& hull : generated)
            hulls.push_back({{"piece", hull.piece}, {"kind", hull.kind},
                             {"planes", hull.planes.size()},
                             {"volumeCubicInches", std::round(HullVolume(hull))}});
        out = {{"hulls", hulls}, {"collmap", MasterCollmapPath(session.root, recipe).string()}};
        return true;
    }
    if (name == "build") {
        SourceState& state = session.StateFor(recipe);
        BuildReport report;
        if (!BuildRecipe(recipe, session.root, state, session.Logger(), report, error)) return false;
        json built = json::array();
        for (const auto& b : report.built)
            built.push_back({{"asset", b.asset}, {"variant", b.variant}, {"hulls", b.hulls},
                             {"collmap", b.collmap.string()}});
        out = {{"built", built}, {"warnings", report.warnings}};
        return true;
    }
    error = "unknown tool '" + name + "'";
    return false;
}

int RunMcp() {
    Session session;
    session.echo = false;
    std::ios::sync_with_stdio(false);
    std::string line;
    auto send = [](const json& message) {
        const std::string text = message.dump();
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };
    while (std::getline(std::cin, line)) {
        if (line.empty() || line == "\r") continue;
        json request;
        try {
            request = json::parse(line);
        } catch (const std::exception& exception) {
            send({{"jsonrpc", "2.0"}, {"id", nullptr},
                  {"error", {{"code", -32700}, {"message", exception.what()}}}});
            continue;
        }
        const std::string method = request.value("method", std::string());
        const bool notification = !request.contains("id");
        const json id = notification ? json() : request["id"];
        if (notification) continue;   // notifications/initialized and friends
        json result;
        if (method == "initialize") {
            json params = request.value("params", json::object());
            if (!params.is_object()) params = json::object();
            result = {{"protocolVersion", params.value("protocolVersion", "2024-11-05")},
                      {"capabilities", {{"tools", json::object()}}},
                      {"serverInfo", {{"name", "kiwi-import"}, {"version", "1"}}}};
        } else if (method == "ping") {
            result = json::object();
        } else if (method == "tools/list") {
            result = {{"tools", ToolList()}};
        } else if (method == "tools/call") {
            const json params = request.value("params", json::object());
            session.log.clear();
            json out;
            std::string error;
            bool ok = false;
            try {
                ok = CallTool(session, params.value("name", std::string()),
                              params.value("arguments", json::object()), out, error);
            } catch (const std::exception& exception) {
                error = exception.what();
            }
            json payload = ok ? out : json{{"error", error}};
            if (!session.log.empty()) {
                // Keep responses small: the tail of the converter log.
                const std::size_t keep = 60;
                const std::size_t start = session.log.size() > keep ? session.log.size() - keep : 0;
                payload["log"] = std::vector<std::string>(session.log.begin() + start, session.log.end());
            }
            result = {{"content", {{{"type", "text"}, {"text", payload.dump(2)}}}},
                      {"isError", !ok}};
        } else {
            send({{"jsonrpc", "2.0"}, {"id", id},
                  {"error", {{"code", -32601}, {"message", "unknown method " + method}}}});
            continue;
        }
        send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
    }
    return 0;
}

// ---- CLI ----------------------------------------------------------------------

void Usage() {
    std::fprintf(stderr,
                 "KIWI-modelimport new <source.fbx|obj> [--asset n] [--surface t] [--units in|m|cm|mm]\n"
                 "                     [--textures <folder>] [--prefix <p>] [--force]\n"
                 "KIWI-modelimport build <asset|recipe>\n"
                 "KIWI-modelimport inspect <asset|recipe>\n"
                 "KIWI-modelimport collision <asset|recipe> [--shape auto|box|kdop] [--min-size <in>] [--force]\n"
                 "KIWI-modelimport scan <folder> [--since <hours>]\n"
                 "KIWI-modelimport collide <name pattern|*> [--material m] [--dry-run]\n"
                 "KIWI-modelimport dump <xmodel name>\n"
                 // KIWI GLASS
                 "KIWI-modelimport material <name> --template <stock material> --color <png>\n"
                 "                     [--normal <png>] [--spec <png>] [--surface t] [--normal-green keep|flip]\n"
                 // KIWI END
                 "KIWI-modelimport --mcp\n");
}

// KIWI GLASS
// Clones a stock material over PNGs used as authored.
int RunMaterialCli(const std::string& name, std::map<std::string, std::string>& flags) {
    if (!flags.count("--template") || !flags.count("--color")) {
        Usage();
        return 2;
    }
    const GameRoot root = DefaultGameRoot();
    ImageMaterialRequest request;
    request.rawRoot = root.Raw();
    request.name = name;
    request.templateMaterial = root.Raw() / "materials" / flags["--template"];
    request.color = flags["--color"];
    if (flags.count("--normal")) request.normal = flags["--normal"];
    if (flags.count("--spec")) request.specularGloss = flags["--spec"];
    if (flags.count("--normal-green") &&
        (!ParseNormalGreen(flags["--normal-green"], request.normalGreen) ||
         request.normalGreen == NormalGreen::Auto)) {
        std::fprintf(stderr, "Error: --normal-green is keep or flip\n");
        return 2;
    }
    if (flags.count("--surface") && !cod4::ParseSurfaceType(flags["--surface"], request.surfaceType)) {
        std::fprintf(stderr, "Error: '%s' is not a CoD4 surface type\n", flags["--surface"].c_str());
        return 2;
    }
    const LogFn log = [](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); };
    std::vector<fs::path> written;
    std::string error;
    if (!WriteImageMaterial(request, log, written, error)) {
        std::fprintf(stderr, "Error: %s\n", error.c_str());
        return 1;
    }
    json out = {{"material", name}, {"written", json::array()}};
    for (const fs::path& path : written) out["written"].push_back(path.string());
    std::printf("%s\n", out.dump(2).c_str());
    return 0;
}
// KIWI END

int RunCli(int argc, char** argv) {
    if (argc < 3) {
        Usage();
        return 2;
    }
    const std::string verb = argv[1];
    std::map<std::string, std::string> flags;
    std::vector<std::string> positional;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            const bool takesValue = arg != "--force" && arg != "--dry-run";
            flags[arg] = takesValue && i + 1 < argc ? argv[++i] : "1";
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.empty()) {
        Usage();
        return 2;
    }
    // KIWI GLASS
    if (verb == "material") return RunMaterialCli(positional.at(0), flags);
    // KIWI END
    Session session;
    json args;
    std::string tool;
    try {
    if (verb == "new") {
        tool = "create_recipe";
        args = {{"source", positional.at(0)}};
        if (flags.count("--asset")) args["asset"] = flags["--asset"];
        if (flags.count("--surface")) args["surface"] = flags["--surface"];
        if (flags.count("--units")) args["units"] = flags["--units"];
        if (flags.count("--textures")) args["textureFolder"] = flags["--textures"];
        if (flags.count("--prefix")) args["texturePrefix"] = flags["--prefix"];
        args["overwrite"] = flags.count("--force") > 0;
    } else if (verb == "build" || verb == "inspect") {
        tool = verb;
        args = {{"asset", positional.at(0)}};
    } else if (verb == "collision") {
        tool = "regenerate_collision";
        args = {{"asset", positional.at(0)}};
        if (flags.count("--shape")) args["shape"] = flags["--shape"];
        if (flags.count("--min-size")) args["minPieceSize"] = std::stod(flags["--min-size"]);
        args["force"] = flags.count("--force") > 0;
    } else if (verb == "collide") {
        tool = "collide_xmodels";
        args = {{"pattern", positional.at(0) == "*" ? std::string() : positional.at(0)},
                {"dryRun", flags.count("--dry-run") > 0}};
        if (flags.count("--material")) args["material"] = flags["--material"];
    } else if (verb == "dump") {
        tool = "inspect_xmodel";
        args = {{"name", positional.at(0)}};
    } else if (verb == "scan") {
        tool = "scan_sources";
        args = {{"folder", positional.at(0)}};
        if (flags.count("--since")) args["sinceHours"] = std::stod(flags["--since"]);
    } else {
        Usage();
        return 2;
    }
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "Error: bad arguments (%s)\n", exception.what());
        return 2;
    }
    json out;
    std::string error;
    bool ok = false;
    try {
        ok = CallTool(session, tool, args, out, error);
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    if (!ok) {
        std::fprintf(stderr, "Error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s\n", out.dump(2).c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--mcp") return RunMcp();
    return RunCli(argc, argv);
}
