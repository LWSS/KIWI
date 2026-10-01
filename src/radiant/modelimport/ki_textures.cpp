// KIWI model importer - InstaMAT texture-set discovery. See ki_textures.h.
//
// A map file name (resolution suffix and map name removed) is
//     <prefix>_<Set>[_<variant>]_<Set>
// <Set> is InstaMAT's texture-set name: a word, optionally followed by "_<N>"
// ("Default", "Default_1", "Metal"). It appears twice - right after the prefix and
// as the head of "<Set><Map>" - which is what anchors the split. Splitting on "the
// shortest repeated head/tail" (the first version) invented phantom prefixes such as
// "cementpost_white" out of "cementpost_white_2_Default_2_Default" (critic review
// 2026-09-30); the set name must look like a set name, not like "2_Default".

#include "ki_textures.h"

#include "xmodel_write.h"

#include <stb_image.h>   // declarations only; the implementation is in ki_convert.cpp

#include <algorithm>
#include <cctype>
#include <system_error>
#include <utility>

namespace kimport {
namespace {

namespace fs = std::filesystem;

std::string Lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool EndsWithNoCase(const std::string& text, const std::string& suffix) {
    if (suffix.size() > text.size()) return false;
    return Lower(text.substr(text.size() - suffix.size())) == suffix;
}

bool AllDigits(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(),
                                        [](char c) { return c >= '0' && c <= '9'; });
}

bool IsWord(const std::string& text) {
    if (text.empty() || !std::isalpha(static_cast<unsigned char>(text[0]))) return false;
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

std::vector<std::string> Split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = text.find(separator, start);
        parts.push_back(text.substr(start, at == std::string::npos ? std::string::npos
                                                                  : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return parts;
}

std::string Join(const std::vector<std::string>& parts, std::size_t begin, std::size_t end) {
    std::string out;
    for (std::size_t i = begin; i < end; ++i) {
        if (i != begin) out += '_';
        out += parts[i];
    }
    return out;
}

// Map name endings, longest first so "base_color" wins over a shorter key.
struct MapKey {
    const char* key;
    MapKind kind;
};
constexpr MapKey kMapKeys[] = {
    {"transparency", MapKind::Opacity}, {"base_color", MapKind::BaseColor},
    {"basecolor", MapKind::BaseColor},  {"metalness", MapKind::Metalness},
    {"roughness", MapKind::Roughness},  {"metallic", MapKind::Metalness},
    {"diffuse", MapKind::BaseColor},    {"opacity", MapKind::Opacity},
    {"albedo", MapKind::BaseColor},     {"normal", MapKind::Normal},
    {"alpha", MapKind::Opacity},
};

// InstaMAT's mesh-space bakes share the "normal" ending; never a texture-set map.
bool IsMeshMap(const std::string& lowerName) {
    for (const char* skip : {"meshnormal", "normaldirect", "normalobject", "normalworld",
                             "ambientocclusion", "curvature", "thickness", "position"})
        if (lowerName.find(skip) != std::string::npos) return true;
    return false;
}

struct ParsedName {
    std::string body;      // everything before the map key: prefix_set..._set
    MapKind kind = MapKind::Count;
    int width = 0;
    int height = 0;
};

// "<body><Map>[-WxH]" -> body + map kind. False for anything that is not a map.
bool ParseMapName(const fs::path& path, ParsedName& out) {
    if (Lower(path.extension().string()) != ".png") return false;
    std::string name = path.stem().string();
    if (const std::size_t dash = name.rfind('-'); dash != std::string::npos) {
        const std::string tail = Lower(name.substr(dash + 1));
        const std::size_t x = tail.find('x');
        if (x != std::string::npos && AllDigits(tail.substr(0, x)) &&
            AllDigits(tail.substr(x + 1))) {
            out.width = std::atoi(tail.substr(0, x).c_str());
            out.height = std::atoi(tail.substr(x + 1).c_str());
            name.resize(dash);
        }
    }
    if (IsMeshMap(Lower(name))) return false;
    for (const MapKey& key : kMapKeys) {
        if (EndsWithNoCase(name, key.key)) {
            out.body = name.substr(0, name.size() - std::char_traits<char>::length(key.key));
            out.kind = key.kind;
            return !out.body.empty();
        }
    }
    return false;
}

bool SameNoCase(const std::string& a, const std::string& b) {
    return Lower(a) == Lower(b);
}

// <Set> = tokens[i, i+k): a word, or a word + "_<digits>".
bool IsSetName(const std::vector<std::string>& t, std::size_t i, std::size_t k) {
    if (i + k > t.size()) return false;
    if (k == 1) return IsWord(t[i]);
    if (k == 2) return IsWord(t[i]) && AllDigits(t[i + 1]);
    return false;
}

struct Decomposed {
    std::string prefix;
    std::string set;
    std::string variant;   // "" = bare (no variant number in the name)
};

// tokens = body split on '_'. With prefixTokens > 0 the prefix length is fixed
// (scanning for a known prefix); otherwise the rightmost valid head is taken.
bool Decompose(const std::vector<std::string>& t, std::size_t prefixTokens, Decomposed& out) {
    const std::size_t n = t.size();
    for (std::size_t k : {std::size_t{2}, std::size_t{1}}) {
        if (n < 2 * k + 1 || !IsSetName(t, n - k, k)) continue;
        auto headMatches = [&](std::size_t h) {
            for (std::size_t j = 0; j < k; ++j)
                if (!SameNoCase(t[h + j], t[n - k + j])) return false;
            return true;
        };
        std::size_t head = 0;
        bool found = false;
        if (prefixTokens > 0) {
            if (prefixTokens + 2 * k <= n && headMatches(prefixTokens)) {
                head = prefixTokens;
                found = true;
            }
        } else {
            for (std::size_t h = n - 2 * k; h >= 1; --h) {
                if (headMatches(h)) {
                    head = h;
                    found = true;
                    break;
                }
            }
        }
        if (!found) continue;
        out.prefix = Join(t, 0, head);
        out.set = Join(t, head, head + k);
        out.variant = Join(t, head + k, n - k);
        return true;
    }
    return false;
}

bool NumericLess(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

bool VariantLess(const TextureVariant& a, const TextureVariant& b) {
    const bool an = AllDigits(a.id);
    const bool bn = AllDigits(b.id);
    if (an != bn) return an;
    if (an) return NumericLess(a.id, b.id);
    return a.id < b.id;
}

fs::path& Slot(TextureFiles& files, MapKind kind) {
    switch (kind) {
    case MapKind::BaseColor: return files.baseColor;
    case MapKind::Normal: return files.normal;
    case MapKind::Metalness: return files.metalness;
    case MapKind::Roughness: return files.roughness;
    default: return files.opacity;
    }
}

}  // namespace

bool ScanTextureSets(const fs::path& folder, const std::string& prefix, TextureScan& scan,
                     std::string& error) {
    scan = {};
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        error = "texture folder not found: " + folder.string();
        return false;
    }
    const std::vector<std::string> prefixTokens = Split(prefix, '_');
    std::map<std::string, std::map<std::string, TextureVariant>> found;   // lower keys
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        ParsedName parsed;
        if (!ParseMapName(it->path(), parsed)) continue;
        const std::vector<std::string> tokens = Split(parsed.body, '_');
        if (tokens.size() <= prefixTokens.size()) continue;
        bool prefixMatches = true;
        for (std::size_t i = 0; i < prefixTokens.size() && prefixMatches; ++i)
            prefixMatches = SameNoCase(tokens[i], prefixTokens[i]);
        if (!prefixMatches) continue;
        Decomposed parts;
        if (!Decompose(tokens, prefixTokens.size(), parts)) continue;
        TextureVariant& entry = found[Lower(parts.set)][Lower(parts.variant)];
        entry.set = parts.set;
        entry.id = parts.variant;
        fs::path& slot = Slot(entry.files, parsed.kind);
        // Two aliases for one map (BaseColor + Diffuse): keep the newer file.
        const long long stamp = it->last_write_time(fileError).time_since_epoch().count();
        if (!slot.empty()) {
            std::error_code oldError;
            const long long oldStamp =
                fs::last_write_time(slot, oldError).time_since_epoch().count();
            if (oldStamp >= stamp) continue;
        }
        slot = it->path();
        entry.newestWrite = std::max(entry.newestWrite, stamp);
        if (parsed.kind == MapKind::BaseColor) {
            entry.width = parsed.width;
            entry.height = parsed.height;
        }
    }
    if (ec) {
        error = "cannot scan " + folder.string() + ": " + ec.message();
        return false;
    }
    for (auto& [setKey, variants] : found) {
        // A bare export (no number) is variant "1" - unless an explicit "_1_"
        // export also exists, then the bare one is "base" so neither hides the other.
        const bool explicitOne = variants.count("1") > 0;
        std::vector<TextureVariant> list;
        for (auto& [id, variant] : variants) {
            if (variant.id.empty()) variant.id = explicitOne ? "base" : "1";
            list.push_back(std::move(variant));
        }
        std::sort(list.begin(), list.end(), VariantLess);
        scan.sets[list.front().set] = std::move(list);
    }
    return true;
}

std::vector<std::string> ListTexturePrefixes(const fs::path& folder, std::string& error) {
    std::error_code ec;
    std::map<std::string, std::pair<long long, std::string>> prefixes;   // lower -> (newest, as written)
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fileError;
        if (!it->is_regular_file(fileError)) continue;
        ParsedName parsed;
        if (!ParseMapName(it->path(), parsed) || parsed.kind != MapKind::BaseColor) continue;
        Decomposed parts;
        if (!Decompose(Split(parsed.body, '_'), 0, parts) || parts.prefix.empty()) continue;
        const long long stamp = it->last_write_time(fileError).time_since_epoch().count();
        auto& slot = prefixes[Lower(parts.prefix)];
        if (stamp > slot.first) slot = {stamp, parts.prefix};
    }
    if (ec) error = "cannot scan " + folder.string() + ": " + ec.message();
    std::vector<std::pair<long long, std::string>> sorted;
    for (auto& [key, value] : prefixes) sorted.push_back(value);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto& entry : sorted) out.push_back(entry.second);
    return out;
}

std::string VariantAssetName(const std::string& baseAsset, const std::string& variantId) {
    // "base" (a bare export beside an explicit "_1_") gets its own name so the two
    // never overwrite each other.
    if (variantId.empty() || variantId == "1") return baseAsset;
    return xmodel::SanitizeModelName(baseAsset + "_" + variantId);
}

bool LoadThumbnail(const fs::path& path, int maxSize, std::vector<unsigned char>& rgba,
                   int& width, int& height, std::string& error) {
    rgba.clear();
    width = height = 0;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int channels = 0;
    unsigned char* pixels =
        stbi_load(path.string().c_str(), &sourceWidth, &sourceHeight, &channels, 4);
    if (!pixels) {
        error = "cannot read " + path.string();
        return false;
    }
    // Integer box filter: every output pixel averages a whole step x step block.
    int step = 1;
    while (sourceWidth / step > maxSize || sourceHeight / step > maxSize) ++step;
    width = (std::max)(1, sourceWidth / step);
    height = (std::max)(1, sourceHeight / step);
    rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            unsigned sum[4] = {};
            for (int sy = 0; sy < step; ++sy) {
                const unsigned char* row =
                    pixels + (static_cast<std::size_t>(y * step + sy) * sourceWidth + x * step) * 4;
                for (int sx = 0; sx < step; ++sx)
                    for (int c = 0; c < 4; ++c) sum[c] += row[sx * 4 + c];
            }
            unsigned char* out = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
            for (int c = 0; c < 4; ++c) out[c] = static_cast<unsigned char>(sum[c] / (step * step));
        }
    stbi_image_free(pixels);
    return true;
}

}  // namespace kimport
