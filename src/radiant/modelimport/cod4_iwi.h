#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace cod4 {

// Standalone CoD4 IWI v6 writer.
//
// The files produced here use the load-object layout consumed by CoD4 when
// useFastFile=0: a 28-byte GfxImageFileHeader followed by mip levels from the
// smallest level to the largest. Color images are BC1/DXT1 when every source
// alpha byte is opaque and BC3/DXT5 otherwise. SpecularGloss always uses
// BC3/DXT5: RGB is reflection color and A is gloss/cosine power (1 is the
// sharpest reflection). There is no separate cosine-power material image slot.
//
// NORMAL-MAP CHANNEL ENCODING (the baker-facing contract): input RGB is a
// conventional tangent-space normal. Each source mip is normalized, converted
// to slopes x = nx/nz and y = ny/nz, then stored as
//
//     DXT5 alpha = round(255 * (x + 2.08) / 4.08)
//     DXT5 green = round(255 * (y + 2.06452) / 4.06452)
//     DXT5 red = DXT5 blue = DXT5 green
//
// after slope clamping. This exactly inverts the CoD4 shader constants
// (4.08, 4.06452, -2.08, -2.06452). Green is not flipped; +Y/-Y belongs to the
// source-art convention. Mips are filtered from the conventional source
// normals first and slope-encoded one level at a time, matching KIWI.

enum class IwiKind : std::uint8_t {
    Color,
    Normal,
    SpecularGloss,
};

enum class IwiFormat : std::uint8_t {
    Argb8 = 1,
    Rgb8 = 2,
    Dxt1 = 11,
    Dxt3 = 12,
    Dxt5 = 13,
};

struct IwiOptions {
    bool mipmaps = true;

    // This switch applies only to Color. Normal and SpecularGloss force DXT5,
    // just as KIWI does. Any image with a base dimension below four pixels
    // falls back to uncompressed ARGB8 because DXT block pitch is ambiguous on
    // that D3D9 path.
    bool compressColor = true;
};

struct IwiWriteResult {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mipCount = 0;
    IwiFormat format = IwiFormat::Argb8;
    std::uint64_t fileSize = 0;
    bool sourceUsesAlpha = false;
};

// Safe span API. rgba must contain exactly width * height * 4 bytes in RGBA8
// row-major order. CoD4's mip counter requires power-of-two dimensions; this
// standalone in-memory API refuses non-POT input instead of silently resampling
// baker output.
bool WriteIwi(const std::filesystem::path& path,
              std::uint32_t width,
              std::uint32_t height,
              std::span<const std::uint8_t> rgba,
              IwiKind kind,
              const IwiOptions& options,
              IwiWriteResult* result,
              std::string& error);

// Convenience form matching the export-stage call site. The pointer is assumed
// to address width * height * 4 RGBA8 bytes and color compression is enabled.
bool WriteIwi(const std::filesystem::path& path,
              std::uint32_t width,
              std::uint32_t height,
              const std::uint8_t* rgba,
              IwiKind kind,
              bool mipmaps,
              std::string& error);

// Reopens an IWI and applies the KIWI/engine round-trip invariants: tag and
// version, supported format, dimensions/mip count, all four picmip sizes, exact
// level byte count, and smallest-first payload extent.
bool RoundTrip(const std::filesystem::path& path, std::string& error);

// ---- IWI reading (stock game images out of raw/images or main/*.iwd) ----
//
// Mips come back LARGEST FIRST (mip 0 first — GPU upload order); the file
// stores them smallest first. Payload bytes are returned exactly as stored:
// BC1/BC2/BC3 blocks for the DXT formats (upload directly as
// VK_FORMAT_BC1/2/3_*), BGRA8 for Argb8, BGR8 (24bpp) for Rgb8.
// Cubemap/volume images (flags & 4|8) are rejected with a distinctive error
// containing "cubemap".
struct IwiMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t offset = 0;   // into the source byte buffer
    std::size_t size = 0;
};

struct IwiImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    IwiFormat format = IwiFormat::Argb8;
    std::vector<IwiMip> mips;   // [0] = largest
};

bool ReadIwi(std::span<const std::uint8_t> bytes, IwiImage& out, std::string& error);

}  // namespace cod4
