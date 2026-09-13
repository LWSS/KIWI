#include <universal/q_shared.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_image_wavelet_tables.h>
#include "native_wavelet.h"
#include "native_image.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct WaveletBits
{
    const uint8_t *data;
    size_t size, bit;
    bool valid;
};

static unsigned int Peek(const WaveletBits *bits, unsigned int count)
{
    unsigned int value = 0;
    for (unsigned int i = 0; i < count; ++i)
    {
        const size_t position = bits->bit + i;
        if (position / 8 < bits->size)
        {
            value |= ((bits->data[position / 8] >> (position & 7)) & 1) << i;
        }
    }
    return value;
}

static unsigned int Read(WaveletBits *bits, unsigned int count)
{
    if (bits->bit > bits->size * 8 || count > bits->size * 8 - bits->bit)
    {
        bits->valid = false;
        return 0;
    }
    const unsigned int value = Peek(bits, count);
    bits->bit += count;
    return value;
}

static int Coefficient(WaveletBits *bits, const WaveletHuffmanDecode *table, unsigned int width, int bias)
{
    const WaveletHuffmanDecode entry = table[Peek(bits, 12)];
    Read(bits, entry.bits);
    return entry.value == INT16_MIN ? (int)Read(bits, width) - bias : entry.value;
}

static void ExpandChannel(uint8_t *dst, unsigned int stride, unsigned int bpp, int base, const int *c, int parity)
{
    dst[0] = (uint8_t)(parity + ((c[2] + c[1] + c[0] + base) >> 1));
    dst[bpp] = (uint8_t)((c[0] + base - c[2] - c[1]) >> 1);
    dst[stride] = (uint8_t)((c[1] - c[2] + base - c[0]) >> 1);
    dst[stride + bpp] = (uint8_t)((base - c[0] - c[1] + c[2]) >> 1);
}

static bool Level(WaveletBits *bits, uint8_t *previous, uint8_t *output, unsigned int width, unsigned int height,
                  unsigned int channels, unsigned int bpp)
{
    if (width == 1 || height == 1)
    {
        for (size_t i = 0; i < (size_t)width * height; ++i)
        {
            for (unsigned int channel = 0; channel < channels; ++channel)
            {
                output[i * bpp + channel] = (uint8_t)Read(bits, 8);
            }
            if (bpp != channels)
            {
                output[i * bpp + 3] = 255;
            }
        }
        return bits->valid;
    }
    if (!previous)
    {
        return false;
    }
    if (Read(bits, 1))
    {
        for (size_t i = 0; bits->valid && i < (size_t)width * height / 4; ++i)
        {
            for (unsigned int channel = 0; channel < channels; ++channel)
            {
                previous[i * bpp + channel] =
                    (uint8_t)(previous[i * bpp + channel] + Coefficient(bits, waveletDecodeAlpha, 9, 255));
            }
        }
    }
    for (unsigned int y = 0; bits->valid && y < height; y += 2)
    {
        for (unsigned int x = 0; bits->valid && x < width; x += 2)
        {
            const uint8_t *src = previous + ((size_t)(y / 2) * (width / 2) + x / 2) * bpp;
            uint8_t *dst = output + ((size_t)y * width + x) * bpp;
            int blue[3] = {};
            for (unsigned int channel = 0; channel < channels; ++channel)
            {
                const bool alpha = channels == 1 || (channels != 3 && channel == channels - 1);
                const bool redGreen = !alpha && channel != 0;
                const WaveletHuffmanDecode *table =
                    alpha ? waveletDecodeAlpha : (redGreen ? waveletDecodeRedGreen : waveletDecodeBlue);
                const int parity = Read(bits, 1);
                int c[3];
                for (unsigned int i = 0; i < 3; ++i)
                {
                    c[i] = Coefficient(bits, table, redGreen ? 10 : 9, redGreen ? 510 : 255);
                    if (redGreen)
                    {
                        c[i] += blue[i];
                    }
                    else if (!alpha)
                    {
                        blue[i] = c[i];
                    }
                }
                ExpandChannel(dst + channel, width * bpp, bpp, 2 * src[channel], c, parity);
            }
            if (bpp != channels)
            {
                dst[3] = dst[bpp + 3] = dst[width * bpp + 3] = dst[(width + 1) * bpp + 3] = 255;
            }
        }
    }
    return bits->valid;
}

bool Linker_ImportWavelet(const GfxImageFileHeader *header, const void *data, size_t size, const char *name,
                          unsigned char semantic, unsigned char track, GfxImage **image, char *error, size_t errorSize)
{
    if (!image || (!error && errorSize))
    {
        return false;
    }
    *image = NULL;
    bool valid = header && data && size && size <= INT_MAX && header->format >= IMG_FORMAT_WAVELET_RGBA &&
                 header->format <= IMG_FORMAT_WAVELET_ALPHA && header->dimensions[0] > 0 && header->dimensions[1] > 0;
    if (!valid)
    {
        return false;
    }
    if (header->dimensions[2] != 1 || (header->flags & IMG_FLAG_VOLMAP) ||
        (header->dimensions[0] & (header->dimensions[0] - 1)) ||
        (header->dimensions[1] & (header->dimensions[1] - 1)) ||
        ((header->flags & IMG_FLAG_CUBEMAP) && header->dimensions[0] != header->dimensions[1]))
    {
        return false;
    }
    const unsigned int channelsByFormat[5] = {4, 3, 2, 1, 1};
    const int formats[5] = {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_A8L8, D3DFMT_L8, D3DFMT_A8};
    const unsigned int index = header->format - IMG_FORMAT_WAVELET_RGBA;
    const unsigned int channels = channelsByFormat[index], bpp = channels == 3 ? 4 : channels;
    const unsigned int faces = header->flags & IMG_FLAG_CUBEMAP ? 6 : 1;
    unsigned int levels = 1;
    if (!(header->flags & IMG_FLAG_NOMIPMAPS))
    {
        for (unsigned int resolution = 1;
             resolution < (unsigned int)header->dimensions[0] || resolution < (unsigned int)header->dimensions[1];
             resolution *= 2)
        {
            ++levels;
        }
    }
    size_t offsets[16] = {}, sizes[16] = {}, faceSize = 0;
    for (unsigned int mip = 0; mip < levels; ++mip)
    {
        const unsigned int w = header->dimensions[0] >> mip ? header->dimensions[0] >> mip : 1;
        const unsigned int h = header->dimensions[1] >> mip ? header->dimensions[1] >> mip : 1;
        offsets[mip] = faceSize;
        sizes[mip] = (size_t)w * h * bpp;
        faceSize += sizes[mip];
    }
    valid = faceSize * faces <= INT_MAX && header->fileSizeForPicmip[0] == size + sizeof(GfxImageFileHeader);
    for (unsigned int i = 1; valid && i < 4; ++i)
    {
        valid = header->fileSizeForPicmip[i] >= sizeof(GfxImageFileHeader) &&
                header->fileSizeForPicmip[i] <= header->fileSizeForPicmip[i - 1];
    }
    uint8_t *pixels = valid ? (uint8_t *)malloc(faceSize * faces) : NULL;
    uint8_t *previous = valid ? (uint8_t *)malloc(sizes[0]) : NULL;
    valid = valid && pixels && previous;
    WaveletBits bits = {(const uint8_t *)data, size, 0, true};
    for (int mip = (int)levels - 1; valid && mip >= 0; --mip)
    {
        const unsigned int w = header->dimensions[0] >> mip ? header->dimensions[0] >> mip : 1;
        const unsigned int h = header->dimensions[1] >> mip ? header->dimensions[1] >> mip : 1;
        for (unsigned int face = 0; valid && face < faces; ++face)
        {
            const bool hasPrevious = mip + 1 < levels;
            if (hasPrevious)
            {
                // Delta corrections affect the reconstruction input, not the already-emitted smaller mip.
                memcpy(previous, pixels + face * faceSize + offsets[mip + 1], sizes[mip + 1]);
            }
            valid = Level(&bits, hasPrevious ? previous : NULL, pixels + face * faceSize + offsets[mip], w, h, channels,
                          bpp);
        }
    }
    valid = valid &&
            Linker_CreateImage(name, header->dimensions[0], header->dimensions[1], formats[index], header->flags,
                               IMG_CATEGORY_RAW, semantic, track, pixels, faceSize * faces, image, error, errorSize);
    free(previous);
    free(pixels);
    if (!valid && errorSize)
    {
        snprintf(error, errorSize, "Invalid, truncated or unsupported IWI wavelet stream");
    }
    return valid;
}
