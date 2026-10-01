// KIWI model importer - source mesh loading. See ki_mesh.h.

#include "ki_mesh.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <system_error>
#include <vector>

namespace kimport {
namespace {

namespace fs = std::filesystem;

// The FBX importer adds a cm->m scale and a Y-up->Z-up rotation on the objects;
// clearing both leaves the stored Plasticity coordinates (inches), which match
// Plasticity's own OBJ export to the inch. One `o` per body, one `usemtl` per
// material, so pieces and material sets survive.
constexpr const char* kFbxToObjScript = R"PY(
import bpy, sys
argv = sys.argv[sys.argv.index("--") + 1:]
src, out = argv[0], argv[1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=src)
import math
for o in bpy.data.objects:
    if o.type != "MESH":
        continue
    # Plasticity: unparented, at the origin, 90 deg X (Y-up) and a uniform unit scale.
    # Anything else is a real node transform that zeroing below would throw away.
    r = [math.degrees(a) % 360.0 for a in o.rotation_euler]
    s = list(o.scale)
    odd_rot = not (abs(r[1]) < 0.01 or abs(r[1] - 360) < 0.01) or \
              not (abs(r[2]) < 0.01 or abs(r[2] - 360) < 0.01) or \
              min(abs(r[0] - 90.0), abs(r[0]), abs(r[0] - 360.0)) > 0.01
    odd_scale = max(s) - min(s) > 1e-6 * max(abs(max(s)), 1e-9)
    odd_loc = max(abs(v) for v in o.location) > 1e-4
    if o.parent is not None or odd_rot or odd_scale or odd_loc:
        print("KIWI_WARN object '%s' has its own transform (parent=%s rot=%s scale=%s loc=%s); "
              "it is discarded - geometry may be misplaced" %
              (o.name, o.parent.name if o.parent else None, [round(v, 3) for v in r],
               [round(v, 4) for v in s], [round(v, 3) for v in o.location]))
for o in bpy.data.objects:
    if o.type == "MESH":
        o.rotation_euler = (0.0, 0.0, 0.0)
        o.scale = (1.0, 1.0, 1.0)
        o.location = (0.0, 0.0, 0.0)
        o.name = o.name.replace("*", "").strip() or o.name
bpy.ops.wm.obj_export(filepath=out, export_materials=True, export_uv=True,
                      export_normals=True, export_triangulated_mesh=False,
                      forward_axis='Y', up_axis='Z', apply_modifiers=True,
                      export_object_groups=True, export_material_groups=False,
                      global_scale=1.0)
print("KIWI_OBJ_WRITTEN", out)
)PY";

std::string Lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

fs::path CacheDirectory() {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) dir = ".";
    dir /= "kiwi_import";
    fs::create_directories(dir, ec);
    return dir;
}

std::string CacheKey(const fs::path& source) {
    std::error_code ec;
    const auto size = fs::file_size(source, ec);
    const auto stamp = fs::last_write_time(source, ec).time_since_epoch().count();
    const std::string text = Lower(fs::absolute(source, ec).string()) + "|" +
                             std::to_string(size) + "|" + std::to_string(stamp);
    const std::size_t hash = std::hash<std::string>{}(text);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

// Runs a command line hidden, waits, and returns the exit code (-1 on launch failure).
int RunHidden(const std::wstring& commandLine, const fs::path& logPath, DWORD timeoutMs,
              std::string& error) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE logFile = CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                 &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | (logFile != INVALID_HANDLE_VALUE ? STARTF_USESTDHANDLES : 0);
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = logFile;
    startup.hStdError = logFile;
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION process{};
    std::wstring mutableLine = commandLine;
    const BOOL started = CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);
    if (!started) {
        error = "could not start Blender (error " + std::to_string(GetLastError()) + ")";
        return -1;
    }
    const DWORD wait = WaitForSingleObject(process.hProcess, timeoutMs);
    DWORD exitCode = 0;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        error = "Blender did not finish within " + std::to_string(timeoutMs / 1000) + " s";
        exitCode = static_cast<DWORD>(-1);
    } else {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}

std::wstring Quote(const fs::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

bool ConvertFbx(const fs::path& fbx, fs::path& obj, const LogFn& log, std::string& error) {
    const fs::path blender = FindBlender();
    if (blender.empty()) {
        error = "FBX import needs Blender: set KIWI_BLENDER to blender.exe or install "
                "Blender under C:\\Program Files\\Blender Foundation";
        return false;
    }
    const fs::path dir = CacheDirectory();
    const std::string key = CacheKey(fbx);
    obj = dir / (fbx.stem().string() + "_" + key + ".obj");
    std::error_code ec;
    if (fs::exists(obj, ec) && fs::file_size(obj, ec) > 0) {
        if (log) log("FBX: reusing converted " + obj.string());
        return true;
    }
    const fs::path script = dir / "kiwi_fbx_to_obj.py";
    {
        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        out << kFbxToObjScript;
        if (!out) {
            error = "cannot write " + script.string();
            return false;
        }
    }
    const fs::path logPath = dir / (fbx.stem().string() + "_" + key + ".blender.log");
    const fs::path partial = obj.string() + ".part.obj";
    fs::remove(partial, ec);
    const std::wstring command = Quote(blender) + L" --background --factory-startup --python " +
                                 Quote(script) + L" -- " + Quote(fbx) + L" " + Quote(partial);
    if (log) log("FBX: converting with " + blender.string());
    const int code = RunHidden(command, logPath, 180000, error);
    if (code != 0 || !fs::exists(partial, ec) || fs::file_size(partial, ec) == 0) {
        if (error.empty())
            error = "Blender FBX conversion failed (exit " + std::to_string(code) + "); see " +
                    logPath.string();
        return false;
    }
    // Forward the script's transform warnings.
    if (log) {
        std::ifstream blenderLog(logPath);
        std::string line;
        while (std::getline(blenderLog, line))
            if (line.rfind("KIWI_WARN ", 0) == 0) log("Warning: FBX " + line.substr(10));
    }
    fs::rename(partial, obj, ec);
    if (ec) {
        error = "cannot move the converted OBJ into place: " + ec.message();
        return false;
    }
    // The .mtl Blender writes beside the partial name is not needed.
    fs::remove(fs::path(partial).replace_extension(".mtl"), ec);
    if (log) log("FBX: converted to " + obj.string());
    return true;
}

}  // namespace

fs::path FindBlender() {
    if (const char* env = std::getenv("KIWI_BLENDER"); env && *env) {
        std::error_code ec;
        if (fs::exists(env, ec)) return env;
    }
    const fs::path root = "C:/Program Files/Blender Foundation";
    std::error_code ec;
    std::vector<fs::path> found;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path exe = it->path() / "blender.exe";
        std::error_code exists;
        if (fs::exists(exe, exists)) found.push_back(exe);
    }
    if (found.empty()) return {};
    // "Blender 3.6" < "Blender 4.2" < "Blender 10.0": compare the numbers.
    auto version = [](const fs::path& exe) {
        const std::string name = exe.parent_path().filename().string();
        int major = 0;
        int minor = 0;
        const std::size_t digit = name.find_first_of("0123456789");
        if (digit != std::string::npos)
            std::sscanf(name.c_str() + digit, "%d.%d", &major, &minor);
        return major * 1000 + minor;
    };
    std::sort(found.begin(), found.end(),
              [&](const fs::path& a, const fs::path& b) { return version(a) < version(b); });
    return found.back();
}

bool LoadSourceMesh(const fs::path& source, bool flipWinding, ObjMesh& mesh,
                    fs::path& objPath, const LogFn& log, std::string& error) {
    mesh = {};
    std::error_code ec;
    if (!fs::exists(source, ec)) {
        error = "source not found: " + source.string();
        return false;
    }
    const std::string ext = Lower(source.extension().string());
    if (ext == ".obj") {
        objPath = source;
    } else if (ext == ".fbx") {
        if (!ConvertFbx(source, objPath, log, error)) return false;
    } else {
        error = "unsupported source type '" + ext + "' (use .fbx or .obj)";
        return false;
    }
    return LoadObj(objPath, flipWinding, mesh, error);
}

}  // namespace kimport
