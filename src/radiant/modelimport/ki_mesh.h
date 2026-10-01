#pragma once

// KIWI model importer - source mesh loading (OBJ directly; FBX through a headless
// Blender pass that writes the FBX's RAW coordinates to an OBJ, the conversion
// proven on humangate / tri_window / the power-plant pipes, 2026-09-22..29).

#include "ki_convert.h"

#include <filesystem>
#include <string>

namespace kimport {

// Blender executable: %KIWI_BLENDER% when set, else the newest
// "C:\Program Files\Blender Foundation\Blender *\blender.exe". Empty when none.
std::filesystem::path FindBlender();

// Loads `source` (.obj or .fbx). FBX goes through Blender into a cache OBJ under
// %TEMP%\kiwi_import (keyed by path, size and write time), reused while the FBX is
// unchanged. `objPath` receives the OBJ actually parsed.
bool LoadSourceMesh(const std::filesystem::path& source, bool flipWinding, ObjMesh& mesh,
                    std::filesystem::path& objPath, const LogFn& log, std::string& error);

}  // namespace kimport
