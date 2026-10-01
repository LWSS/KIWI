#include "cod4_iwi.h"

#define STB_DXT_STATIC
#define STB_DXT_USE_ROUNDING_BIAS
#define STB_DXT_IMPLEMENTATION
#include <stb_dxt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cod4 {
namespace {

constexpr std::uint32_t kHeaderSize = 28;
constexpr std::uint32_t kMaxDimension = 4096;
constexpr std::uint8_t kIwiVersion = 6;
constexpr std::uint8_t kFlagNoMipmaps = 2;

constexpr float kNormalDecodeScaleX = 4.08f;
constexpr float kNormalDecodeBiasX = -2.08f;
constexpr float kNormalDecodeScaleY = 4.06452f;
constexpr float kNormalDecodeBiasY = -2.06452f;
constexpr float kNormalSlopeLimit = 4.0f;

struct ImageLevel {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

class ByteWriter {
public:
    void u8(std::uint8_t value) { bytes.push_back(value); }

    void u16(std::uint16_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    }

    void u32(std::uint32_t value) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        bytes.push_back(static_cast<std::uint8_t>(value >> 8));
        bytes.push_back(static_cast<std::uint8_t>(value >> 16));
        bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    }

    std::vector<std::uint8_t> bytes;
};

void SetError(std::string& error, std::string message) {
    error = std::move(message);
}

bool IsPowerOfTwo(std::uint32_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

std::uint32_t CountMipmaps(std::uint8_t flags,
                           std::uint32_t width,
                           std::uint32_t height,
                           std::uint32_t depth) {
    if ((flags & kFlagNoMipmaps) != 0)
        return 1;

    std::uint32_t mipCount = 1;
    for (std::uint32_t mipResolution = 1;
         mipResolution < width || mipResolution < height || mipResolution < depth;
         mipResolution *= 2) {
        ++mipCount;
    }
    return mipCount;
}

std::uint32_t MipDimension(std::uint32_t base, std::uint32_t level) {
    return std::max(1u, base >> level);
}

std::uint64_t LevelSize(IwiFormat format, std::uint32_t width, std::uint32_t height) {
    if (format == IwiFormat::Dxt1)
        return 8ull * ((height + 3) >> 2) * ((width + 3) >> 2);
    if (format == IwiFormat::Dxt3 || format == IwiFormat::Dxt5)
        return 16ull * ((height + 3) >> 2) * ((width + 3) >> 2);
    if (format == IwiFormat::Rgb8)
        return 3ull * width * height;
    return 4ull * width * height;
}

bool SourceUsesAlpha(std::span<const std::uint8_t> rgba) {
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] != 255)
            return true;
    }
    return false;
}

std::vector<ImageLevel> BuildMipChain(std::uint32_t width,
                                      std::uint32_t height,
                                      std::span<const std::uint8_t> rgba,
                                      std::uint32_t mipCount) {
    std::vector<ImageLevel> levels;
    levels.reserve(mipCount);

    ImageLevel base;
    base.width = width;
    base.height = height;
    base.rgba.assign(rgba.begin(), rgba.end());
    levels.push_back(std::move(base));

    while (levels.size() < mipCount) {
        const ImageLevel& source = levels.back();
        ImageLevel next;
        next.width = std::max(1u, source.width / 2);
        next.height = std::max(1u, source.height / 2);
        next.rgba.resize(static_cast<std::size_t>(next.width) * next.height * 4);

        for (std::uint32_t y = 0; y < next.height; ++y) {
            const std::uint32_t y0 = y * 2;
            const std::uint32_t y1 = std::min(source.height, y0 + 2);
            for (std::uint32_t x = 0; x < next.width; ++x) {
                const std::uint32_t x0 = x * 2;
                const std::uint32_t x1 = std::min(source.width, x0 + 2);
                const std::uint32_t sampleCount = (x1 - x0) * (y1 - y0);
                const std::size_t destination =
                    (static_cast<std::size_t>(y) * next.width + x) * 4;

                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    std::uint32_t sum = 0;
                    for (std::uint32_t sy = y0; sy < y1; ++sy) {
                        for (std::uint32_t sx = x0; sx < x1; ++sx) {
                            const std::size_t sourceOffset =
                                (static_cast<std::size_t>(sy) * source.width + sx) * 4;
                            sum += source.rgba[sourceOffset + channel];
                        }
                    }
                    next.rgba[destination + channel] = static_cast<std::uint8_t>(
                        (sum + sampleCount / 2) / sampleCount);
                }
            }
        }
        levels.push_back(std::move(next));
    }

    return levels;
}

std::uint8_t EncodeNormalByte(float slope, float decodeScale, float decodeBias) {
    slope = std::clamp(slope, -kNormalSlopeLimit, kNormalSlopeLimit);
    const float raw = (slope - decodeBias) / decodeScale;
    int value = static_cast<int>(raw * 255.0f + 0.5f);
    value = std::clamp(value, 0, 255);
    return static_cast<std::uint8_t>(value);
}

void EncodeNormalLevel(ImageLevel& level) {
    for (std::size_t offset = 0; offset < level.rgba.size(); offset += 4) {
        float nx = static_cast<float>(level.rgba[offset + 0]) / 127.5f - 1.0f;
        float ny = static_cast<float>(level.rgba[offset + 1]) / 127.5f - 1.0f;
        float nz = static_cast<float>(level.rgba[offset + 2]) / 127.5f - 1.0f;
        const float length = static_cast<float>(
            std::sqrt(static_cast<double>(nx * nx + ny * ny + nz * nz)));
        if (length > 1.0e-6f) {
            nx /= length;
            ny /= length;
            nz /= length;
        } else {
            nx = 0.0f;
            ny = 0.0f;
            nz = 1.0f;
        }

        nz = std::max(nz, 1.0f / 256.0f);
        const std::uint8_t encodedX =
            EncodeNormalByte(nx / nz, kNormalDecodeScaleX, kNormalDecodeBiasX);
        const std::uint8_t encodedY =
            EncodeNormalByte(ny / nz, kNormalDecodeScaleY, kNormalDecodeBiasY);
        level.rgba[offset + 0] = encodedY;
        level.rgba[offset + 1] = encodedY;
        level.rgba[offset + 2] = encodedY;
        level.rgba[offset + 3] = encodedX;
    }
}

void AppendArgb8(const ImageLevel& level, ByteWriter& writer) {
    // D3DFMT_A8R8G8B8 is 0xAARRGGBB, hence BGRA in little-endian memory.
    for (std::size_t offset = 0; offset < level.rgba.size(); offset += 4) {
        writer.u8(level.rgba[offset + 2]);
        writer.u8(level.rgba[offset + 1]);
        writer.u8(level.rgba[offset + 0]);
        writer.u8(level.rgba[offset + 3]);
    }
}

void AppendDxt(const ImageLevel& level, IwiFormat format, ByteWriter& writer) {
    const std::uint32_t blocksWide = (level.width + 3) >> 2;
    const std::uint32_t blocksHigh = (level.height + 3) >> 2;
    const std::size_t blockBytes = format == IwiFormat::Dxt1 ? 8 : 16;
    const int hasAlphaBlock = format == IwiFormat::Dxt5 ? 1 : 0;
    std::array<std::uint8_t, 4 * 4 * 4> block{};
    std::array<std::uint8_t, 16> packed{};

    for (std::uint32_t blockY = 0; blockY < blocksHigh; ++blockY) {
        for (std::uint32_t blockX = 0; blockX < blocksWide; ++blockX) {
            for (std::uint32_t pixelY = 0; pixelY < 4; ++pixelY) {
                const std::uint32_t sourceY =
                    std::min(level.height - 1, blockY * 4 + pixelY);
                for (std::uint32_t pixelX = 0; pixelX < 4; ++pixelX) {
                    const std::uint32_t sourceX =
                        std::min(level.width - 1, blockX * 4 + pixelX);
                    const std::size_t sourceOffset =
                        (static_cast<std::size_t>(sourceY) * level.width + sourceX) * 4;
                    const std::size_t blockOffset =
                        (static_cast<std::size_t>(pixelY) * 4 + pixelX) * 4;
                    std::copy_n(level.rgba.data() + sourceOffset, 4,
                                block.data() + blockOffset);
                }
            }

            stb_compress_dxt_block(packed.data(), block.data(), hasAlphaBlock,
                                   STB_DXT_HIGHQUAL);
            writer.bytes.insert(writer.bytes.end(), packed.begin(),
                                packed.begin() + static_cast<std::ptrdiff_t>(blockBytes));
        }
    }
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool ValidateIwiBlob(const std::vector<std::uint8_t>& bytes, std::string& error) {
    if (bytes.size() < kHeaderSize) {
        SetError(error, "file is shorter than the 28-byte IWI header");
        return false;
    }
    if (bytes[0] != 'I' || bytes[1] != 'W' || bytes[2] != 'i') {
        SetError(error, "file does not have the IWi tag");
        return false;
    }
    if (bytes[3] != kIwiVersion) {
        SetError(error, "IWI version is not 6");
        return false;
    }

    IwiFormat format{};
    switch (bytes[4]) {
    case static_cast<std::uint8_t>(IwiFormat::Argb8):
        format = IwiFormat::Argb8;
        break;
    case static_cast<std::uint8_t>(IwiFormat::Dxt1):
        format = IwiFormat::Dxt1;
        break;
    case static_cast<std::uint8_t>(IwiFormat::Dxt5):
        format = IwiFormat::Dxt5;
        break;
    default:
        SetError(error, "IWI format is not ARGB8, DXT1, or DXT5");
        return false;
    }

    const std::uint8_t flags = bytes[5];
    if ((flags & (4u | 8u)) != 0) {
        SetError(error, "cube and volume IWI files are outside this writer's 2D contract");
        return false;
    }
    const std::int16_t signedWidth = static_cast<std::int16_t>(ReadU16(bytes, 6));
    const std::int16_t signedHeight = static_cast<std::int16_t>(ReadU16(bytes, 8));
    const std::int16_t signedDepth = static_cast<std::int16_t>(ReadU16(bytes, 10));
    if (signedWidth <= 0 || signedHeight <= 0 || signedDepth != 1) {
        SetError(error, "IWI dimensions must be positive 2D dimensions with depth 1");
        return false;
    }

    const std::uint32_t width = static_cast<std::uint32_t>(signedWidth);
    const std::uint32_t height = static_cast<std::uint32_t>(signedHeight);
    if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height)) {
        SetError(error, "IWI dimensions are not powers of two");
        return false;
    }

    const std::uint32_t mipCount = CountMipmaps(flags, width, height, 1);
    if (mipCount == 0) {
        SetError(error, "engine mip counter returned zero levels");
        return false;
    }

    std::vector<std::uint64_t> levelSizes(mipCount);
    std::uint64_t expectedFileSize = kHeaderSize;
    for (std::uint32_t level = 0; level < mipCount; ++level) {
        levelSizes[level] =
            LevelSize(format, MipDimension(width, level), MipDimension(height, level));
        expectedFileSize += levelSizes[level];
    }
    if (expectedFileSize != bytes.size()) {
        SetError(error, "IWI payload size does not match its format and mip dimensions");
        return false;
    }
    if (ReadU32(bytes, 12) != bytes.size()) {
        SetError(error, "fileSizeForPicmip[0] does not equal the file size");
        return false;
    }

    for (std::uint32_t picmip = 0; picmip < 4; ++picmip) {
        const std::uint32_t first = std::min(picmip, mipCount - 1);
        std::uint64_t expected = kHeaderSize;
        for (std::uint32_t level = first; level < mipCount; ++level)
            expected += levelSizes[level];
        if (ReadU32(bytes, 12 + 4 * picmip) != expected) {
            SetError(error, "fileSizeForPicmip table does not match the mip payload");
            return false;
        }
    }

    error.clear();
    return true;
}

bool ReadFile(const std::filesystem::path& path,
              std::vector<std::uint8_t>& bytes,
              std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        SetError(error, "could not open '" + path.string() + "' for reading");
        return false;
    }
    const std::streampos end = input.tellg();
    const std::streamoff fileSize = static_cast<std::streamoff>(end);
    if (fileSize < 0 || static_cast<std::uintmax_t>(fileSize) >
                            std::numeric_limits<std::size_t>::max() ||
        static_cast<std::uintmax_t>(fileSize) >
            static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        SetError(error, "could not determine a usable size for '" + path.string() + "'");
        return false;
    }
    bytes.resize(static_cast<std::size_t>(fileSize));
    input.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) {
        SetError(error, "short read from '" + path.string() + "'");
        return false;
    }
    return true;
}

bool WriteFile(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& bytes,
               std::string& error) {
    std::error_code filesystemError;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystemError);
        if (filesystemError) {
            SetError(error, "could not create '" + parent.string() + "': " +
                                filesystemError.message());
            return false;
        }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        SetError(error, "could not open '" + path.string() + "' for writing");
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        std::filesystem::remove(path, filesystemError);
        SetError(error, "short write to '" + path.string() + "'");
        return false;
    }
    return true;
}

}  // namespace

bool WriteIwi(const std::filesystem::path& path,
              std::uint32_t width,
              std::uint32_t height,
              std::span<const std::uint8_t> rgba,
              IwiKind kind,
              const IwiOptions& options,
              IwiWriteResult* result,
              std::string& error) {
    if (result)
        *result = {};
    error.clear();

    try {
        if (path.empty()) {
            SetError(error, "no IWI output path");
            return false;
        }
        if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
            SetError(error, "IWI dimensions must be in [1, 4096]");
            return false;
        }
        if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height)) {
            SetError(error, "IWI dimensions must be powers of two");
            return false;
        }

        const std::uint64_t expectedSourceBytes = 4ull * width * height;
        if (expectedSourceBytes != rgba.size()) {
            SetError(error, "RGBA8 span size does not equal width * height * 4");
            return false;
        }

        const bool sourceUsesAlpha = SourceUsesAlpha(rgba);
        IwiFormat format = IwiFormat::Argb8;
        switch (kind) {
        case IwiKind::Color:
            if (options.compressColor)
                format = sourceUsesAlpha ? IwiFormat::Dxt5 : IwiFormat::Dxt1;
            break;
        case IwiKind::Normal:
        case IwiKind::SpecularGloss:
            format = IwiFormat::Dxt5;
            break;
        default:
            SetError(error, "unknown IWI image kind");
            return false;
        }

        // KIWI's D3D9 path falls back for a base dimension smaller than one DXT block.
        if (format != IwiFormat::Argb8 && (width < 4 || height < 4))
            format = IwiFormat::Argb8;

        const std::uint8_t flags = options.mipmaps ? 0 : kFlagNoMipmaps;
        const std::uint32_t mipCount = CountMipmaps(flags, width, height, 1);
        std::vector<ImageLevel> levels = BuildMipChain(width, height, rgba, mipCount);
        if (kind == IwiKind::Normal) {
            for (ImageLevel& level : levels)
                EncodeNormalLevel(level);
        }

        std::vector<std::uint64_t> levelSizes(mipCount);
        std::uint64_t fileSize = kHeaderSize;
        for (std::uint32_t level = 0; level < mipCount; ++level) {
            levelSizes[level] =
                LevelSize(format, MipDimension(width, level), MipDimension(height, level));
            fileSize += levelSizes[level];
        }
        if (fileSize > std::numeric_limits<std::int32_t>::max()) {
            SetError(error, "IWI is too large for its signed 32-bit size table");
            return false;
        }

        ByteWriter writer;
        writer.bytes.reserve(static_cast<std::size_t>(fileSize));
        writer.u8('I');
        writer.u8('W');
        writer.u8('i');
        writer.u8(kIwiVersion);
        writer.u8(static_cast<std::uint8_t>(format));
        writer.u8(flags);
        writer.u16(static_cast<std::uint16_t>(width));
        writer.u16(static_cast<std::uint16_t>(height));
        writer.u16(1);
        for (std::uint32_t picmip = 0; picmip < 4; ++picmip) {
            const std::uint32_t first = std::min(picmip, mipCount - 1);
            std::uint64_t sizeForPicmip = kHeaderSize;
            for (std::uint32_t level = first; level < mipCount; ++level)
                sizeForPicmip += levelSizes[level];
            writer.u32(static_cast<std::uint32_t>(sizeForPicmip));
        }

        // The engine reader starts at mipCount - 1, so serialize smallest first.
        for (std::size_t level = levels.size(); level-- > 0;) {
            if (format == IwiFormat::Argb8)
                AppendArgb8(levels[level], writer);
            else
                AppendDxt(levels[level], format, writer);
        }

        if (writer.bytes.size() != fileSize) {
            SetError(error, "internal IWI payload size mismatch");
            return false;
        }
        if (!ValidateIwiBlob(writer.bytes, error))
            return false;
        if (!WriteFile(path, writer.bytes, error))
            return false;

        if (result) {
            result->width = width;
            result->height = height;
            result->mipCount = mipCount;
            result->format = format;
            result->fileSize = fileSize;
            result->sourceUsesAlpha = sourceUsesAlpha;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        SetError(error, std::string("IWI write failed: ") + exception.what());
        return false;
    }
}

bool WriteIwi(const std::filesystem::path& path,
              std::uint32_t width,
              std::uint32_t height,
              const std::uint8_t* rgba,
              IwiKind kind,
              bool mipmaps,
              std::string& error) {
    if (!rgba) {
        SetError(error, "no RGBA8 source buffer");
        return false;
    }
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        SetError(error, "IWI dimensions must be in [1, 4096]");
        return false;
    }
    const std::uint64_t byteCount = 4ull * width * height;
    if (byteCount > std::numeric_limits<std::size_t>::max()) {
        SetError(error, "RGBA8 source size overflows size_t");
        return false;
    }

    IwiOptions options;
    options.mipmaps = mipmaps;
    return WriteIwi(path, width, height,
                    std::span<const std::uint8_t>(rgba,
                                                  static_cast<std::size_t>(byteCount)),
                    kind, options, nullptr, error);
}

bool RoundTrip(const std::filesystem::path& path, std::string& error) {
    error.clear();
    try {
        std::vector<std::uint8_t> bytes;
        if (!ReadFile(path, bytes, error))
            return false;
        if (!ValidateIwiBlob(bytes, error)) {
            error = "'" + path.string() + "': " + error;
            return false;
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        SetError(error, std::string("IWI round-trip failed: ") + exception.what());
        return false;
    }
}

bool ReadIwi(std::span<const std::uint8_t> bytes, IwiImage& out, std::string& error) {
    error.clear();
    out = IwiImage{};
    if (bytes.size() < kHeaderSize) {
        SetError(error, "IWI is shorter than its header");
        return false;
    }
    if (bytes[0] != 'I' || bytes[1] != 'W' || bytes[2] != 'i') {
        SetError(error, "file does not have the IWi tag");
        return false;
    }
    if (bytes[3] != kIwiVersion) {
        SetError(error, "IWI version is not 6");
        return false;
    }
    const std::uint8_t rawFormat = bytes[4];
    const std::uint8_t flags = bytes[5];
    if ((flags & (4u | 8u)) != 0) {
        SetError(error, "cubemap/volume IWI is not supported by this reader");
        return false;
    }
    switch (rawFormat) {
    case static_cast<std::uint8_t>(IwiFormat::Argb8):
    case static_cast<std::uint8_t>(IwiFormat::Rgb8):
    case static_cast<std::uint8_t>(IwiFormat::Dxt1):
    case static_cast<std::uint8_t>(IwiFormat::Dxt3):
    case static_cast<std::uint8_t>(IwiFormat::Dxt5):
        break;
    default:
        SetError(error, "unsupported IWI format " + std::to_string(rawFormat));
        return false;
    }
    const IwiFormat format = static_cast<IwiFormat>(rawFormat);
    const std::uint32_t width = static_cast<std::uint32_t>(bytes[6]) | (bytes[7] << 8);
    const std::uint32_t height = static_cast<std::uint32_t>(bytes[8]) | (bytes[9] << 8);
    if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
        SetError(error, "IWI dimensions out of range");
        return false;
    }

    const std::uint32_t mipCount = CountMipmaps(flags, width, height, 1);

    // Levels are stored smallest first; walk the payload forward, then reverse
    // so the caller sees mip 0 (largest) first.
    std::size_t offset = kHeaderSize;
    std::vector<IwiMip> smallestFirst;
    smallestFirst.reserve(mipCount);
    for (std::uint32_t i = 0; i < mipCount; ++i) {
        const std::uint32_t level = mipCount - 1 - i;   // smallest level index first
        IwiMip mip;
        mip.width = MipDimension(width, level);
        mip.height = MipDimension(height, level);
        mip.size = static_cast<std::size_t>(LevelSize(format, mip.width, mip.height));
        mip.offset = offset;
        if (offset + mip.size > bytes.size()) {
            SetError(error, "IWI payload is shorter than its mip chain");
            return false;
        }
        offset += mip.size;
        smallestFirst.push_back(mip);
    }

    out.width = width;
    out.height = height;
    out.format = format;
    out.mips.assign(smallestFirst.rbegin(), smallestFirst.rend());
    return true;
}

}  // namespace cod4
