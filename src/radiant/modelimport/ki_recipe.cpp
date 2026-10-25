// KIWI model importer - recipe JSON. See ki_recipe.h.

#include "ki_recipe.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace kimport {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

template <typename T>
void Read(const json& object, const char* key, T& value) {
    const auto found = object.find(key);
    if (found != object.end() && !found->is_null()) value = found->get<T>();
}

json ToJson(const Recipe& recipe) {
    json pieces = json::array();
    for (const RecipePiece& piece : recipe.pieces)
        pieces.push_back({{"index", piece.index},
                          {"triangles", piece.triangles},
                          {"name", piece.name},
                          {"surface", piece.surface}});
    json variants = json::array();
    for (const RecipeVariant& variant : recipe.textures.variants)
        variants.push_back({{"id", variant.id}, {"enabled", variant.enabled}});
    const CollisionSettings& s = recipe.collision.settings;
    return {
        {"version", recipe.version},
        {"source", recipe.source},
        {"asset", recipe.asset},
        {"units", recipe.units},
        {"scale", recipe.scale},
        {"origin", recipe.origin},
        {"flipWinding", recipe.flipWinding},
        {"uvFlip", recipe.uvFlip},
        {"cullFacing", recipe.cullFacing},
        {"normalGreen", recipe.normalGreen},
        {"surface", recipe.surface},
        {"pieces", pieces},
        {"textures",
         {{"folder", recipe.textures.folder},
          {"prefix", recipe.textures.prefix},
          {"set", recipe.textures.set},
          {"variants", variants},
          {"glassOpacity", recipe.textures.glassOpacity}}},
        {"collision",
         {{"mode", recipe.collision.mode},
          {"material", recipe.collision.material},
          {"shape", s.shape},
          {"minPieceSize", s.minPieceSize},
          {"minThickness", s.minThickness},
          {"kdopRatio", s.kdopRatio},
          {"splitGain", s.splitGain},
          {"maxDepth", s.maxDepth},
          {"wholeBox", s.wholeBox}}},
    };
}

void FromJson(const json& root, Recipe& recipe) {
    recipe = {};
    Read(root, "version", recipe.version);
    Read(root, "source", recipe.source);
    Read(root, "asset", recipe.asset);
    Read(root, "units", recipe.units);
    Read(root, "scale", recipe.scale);
    Read(root, "origin", recipe.origin);
    Read(root, "flipWinding", recipe.flipWinding);
    Read(root, "uvFlip", recipe.uvFlip);
    Read(root, "cullFacing", recipe.cullFacing);
    Read(root, "normalGreen", recipe.normalGreen);
    Read(root, "surface", recipe.surface);
    if (const auto pieces = root.find("pieces"); pieces != root.end() && pieces->is_array())
        for (const json& entry : *pieces) {
            RecipePiece piece;
            Read(entry, "index", piece.index);
            Read(entry, "triangles", piece.triangles);
            Read(entry, "name", piece.name);
            Read(entry, "surface", piece.surface);
            recipe.pieces.push_back(std::move(piece));
        }
    if (const auto textures = root.find("textures"); textures != root.end()) {
        Read(*textures, "folder", recipe.textures.folder);
        Read(*textures, "prefix", recipe.textures.prefix);
        Read(*textures, "set", recipe.textures.set);
        Read(*textures, "glassOpacity", recipe.textures.glassOpacity);
        if (const auto variants = textures->find("variants");
            variants != textures->end() && variants->is_array())
            for (const json& entry : *variants) {
                RecipeVariant variant;
                Read(entry, "id", variant.id);
                Read(entry, "enabled", variant.enabled);
                recipe.textures.variants.push_back(std::move(variant));
            }
    }
    if (const auto collision = root.find("collision"); collision != root.end()) {
        Read(*collision, "mode", recipe.collision.mode);
        Read(*collision, "material", recipe.collision.material);
        Read(*collision, "shape", recipe.collision.settings.shape);
        Read(*collision, "minPieceSize", recipe.collision.settings.minPieceSize);
        Read(*collision, "minThickness", recipe.collision.settings.minThickness);
        Read(*collision, "kdopRatio", recipe.collision.settings.kdopRatio);
        Read(*collision, "splitGain", recipe.collision.settings.splitGain);
        Read(*collision, "maxDepth", recipe.collision.settings.maxDepth);
        Read(*collision, "wholeBox", recipe.collision.settings.wholeBox);
    }
}

}  // namespace

std::string RecipeToJson(const Recipe& recipe) {
    return ToJson(recipe).dump(2);
}

bool RecipeFromJson(const std::string& text, Recipe& recipe, std::string& error) {
    try {
        FromJson(json::parse(text), recipe);
    } catch (const std::exception& exception) {
        error = std::string("recipe JSON: ") + exception.what();
        return false;
    }
    return true;
}

bool LoadRecipe(const fs::path& path, Recipe& recipe, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open recipe " + path.string();
        return false;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    return RecipeFromJson(buffer.str(), recipe, error);
}

bool SaveRecipe(const fs::path& path, const Recipe& recipe, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = path.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << RecipeToJson(recipe) << "\n";
        if (!out) {
            error = "cannot write " + temp.string();
            return false;
        }
    }
    fs::rename(temp, path, ec);
    if (ec) {
        error = "cannot replace " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

double UnitsFactor(const std::string& units) {
    if (units == "m") return 1.0;
    if (units == "cm") return 0.01;
    if (units == "mm") return 0.001;
    if (units == "in") return kMetersPerInch;
    return 0.0;
}

bool ParseOrigin(const std::string& text, xmodel::OriginMode& mode) {
    if (text == "base") mode = xmodel::OriginMode::BaseCenter;
    else if (text == "center") mode = xmodel::OriginMode::BoundsCenter;
    else if (text == "none") mode = xmodel::OriginMode::None;
    else return false;
    return true;
}

bool ParseNormalGreen(const std::string& text, NormalGreen& mode) {
    if (text == "auto") mode = NormalGreen::Auto;
    else if (text == "flip") mode = NormalGreen::Flip;
    else if (text == "keep") mode = NormalGreen::Keep;
    else return false;
    return true;
}

GameRoot DefaultGameRoot() {
    GameRoot root;
    if (const char* env = std::getenv("KIWI_GAME_ROOT"); env && *env) {
        root.root = env;
        return root;
    }
    wchar_t buffer[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    root.root = length ? fs::path(std::wstring(buffer, length)).parent_path() : fs::current_path();
    return root;
}

fs::path RecipePath(const GameRoot& root, const std::string& asset) {
    return root.ModelSource() / (asset + ".kimport");
}

}  // namespace kimport
