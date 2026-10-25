#pragma once

// KIWI model importer - the recipe: everything needed to rebuild one source model's
// assets, as a JSON file `<game root>\model_source\<asset>.kimport`. The Radiant
// Import window and the MCP server both edit recipes; ki_build turns one into
// xmodels + collmaps.

#include "ki_collision.h"
#include "ki_convert.h"

#include <filesystem>
#include <string>
#include <vector>

namespace kimport {

struct RecipePiece {
    int index = 0;            // BuildPieces order
    int triangles = 0;        // stale-check: must match the mesh
    std::string name;
    std::string surface;      // surface type name; "" = the recipe default
};

struct RecipeVariant {
    std::string id = "1";     // InstaMAT variant id
    bool enabled = true;
};

struct Recipe {
    int version = 1;
    std::string source;                    // .fbx / .obj path
    std::string asset;                     // variant "1" asset name
    std::string units = "in";              // m | cm | mm | in
    double scale = 1.0;
    std::string origin = "base";           // base | center | none
    bool flipWinding = false;
    bool uvFlip = true;
    // KIWI (2026-10-22): "" or +x -x +y -y +z -z - triangles facing that model axis are
    // dropped (a sign mounted on a wall loses its back).
    std::string cullFacing;
    std::string normalGreen = "auto";      // auto | flip | keep
    std::string surface = "concrete";      // default surface type name
    std::vector<RecipePiece> pieces;       // only pieces with an override, plus names

    struct Textures {
        std::string folder = "F:/3dlocal/INSTAMAT_EXPORTS";
        std::string prefix;
        std::string set;                   // texture set name; "" = the only set; "*" = one per source material
        std::vector<RecipeVariant> variants;
        double glassOpacity = 0.35;
    } textures;

    // The hull geometry is NOT here: it is the brushes of collmaps\<asset>.map (the
    // master; variants get copies at build). auto = rebuilt from the mesh at every
    // build; manual = the master was hand-edited (Radiant) and is only copied;
    // none = no player collision.
    struct Collision {
        std::string mode = "auto";         // auto | manual | none
        std::string material = "clip";
        CollisionSettings settings;
    } collision;
};

bool LoadRecipe(const std::filesystem::path& path, Recipe& recipe, std::string& error);
bool SaveRecipe(const std::filesystem::path& path, const Recipe& recipe, std::string& error);

// JSON text round trip (used by the MCP server to hand a recipe to an AI).
std::string RecipeToJson(const Recipe& recipe);
bool RecipeFromJson(const std::string& text, Recipe& recipe, std::string& error);

double UnitsFactor(const std::string& units);   // meters per unit, 0 if unknown
bool ParseOrigin(const std::string& text, xmodel::OriginMode& mode);
bool ParseNormalGreen(const std::string& text, NormalGreen& mode);

// Game-root layout (the folder holding raw\, main\, map_source\).
struct GameRoot {
    std::filesystem::path root;
    std::filesystem::path Raw() const { return root / "raw"; }
    std::filesystem::path Collmaps() const { return root / "collmaps"; }
    std::filesystem::path ModelSource() const { return root / "model_source"; }
};

// The game root: KIWI_GAME_ROOT, else the folder of the running exe.
GameRoot DefaultGameRoot();

std::filesystem::path RecipePath(const GameRoot& root, const std::string& asset);

}  // namespace kimport
