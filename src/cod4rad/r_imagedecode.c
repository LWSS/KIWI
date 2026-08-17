/*
 * r_imagedecode.c — DXT texture decompression and image decoding.
 */

#include "cod2rad64.h"

/*
================
DecodeDXT1Block

Decodes a single DXT1 compressed block (8 bytes) into 4x4 RGBA pixels.
If hasAlpha is set, treats color0 <= color1 as transparent black.
================
*/
void DecodeDXT1Block(unsigned char *src, ImageDecodeState_t *dst, int blockX,
                     int blockY, int hasAlpha)
{
    unsigned short color0, color1;
    float r0, g0, b0, r1, g1, b1;
    unsigned char colors[4][4]; /* 4 palette entries, RGBA each */
    unsigned int indices;
    int row, col;
    float scale5 = 1.0f / 32.0f;
    float scale6 = 1.0f / 64.0f;

    color0 = *(unsigned short *)src;
    color1 = *(unsigned short *)(src + 2);

    /* extract RGB565 components and normalize to [0,1] */
    r0 = (float)((color0 >> 11) & 0x1F) * scale5;
    g0 = (float)((color0 >> 5) & 0x3F) * scale6;
    b0 = (float)(color0 & 0x1F) * scale5;

    r1 = (float)((color1 >> 11) & 0x1F) * scale5;
    g1 = (float)((color1 >> 5) & 0x3F) * scale6;
    b1 = (float)(color1 & 0x1F) * scale5;

    if (!hasAlpha && color0 <= color1)
    {
        /* 3-color mode + transparent black */
        colors[0][0] = (unsigned char)floorf(r0 * 255.0f + 0.5f);
        colors[0][1] = (unsigned char)floorf(g0 * 255.0f + 0.5f);
        colors[0][2] = (unsigned char)floorf(b0 * 255.0f + 0.5f);
        colors[0][3] = 0xFF;

        colors[1][0] = (unsigned char)floorf(r1 * 255.0f + 0.5f);
        colors[1][1] = (unsigned char)floorf(g1 * 255.0f + 0.5f);
        colors[1][2] = (unsigned char)floorf(b1 * 255.0f + 0.5f);
        colors[1][3] = 0xFF;

        /* color2 = (color0 + color1) / 2 */
        colors[2][0] = (unsigned char)floorf((r0 + r1) * 0.5f * 255.0f + 0.5f);
        colors[2][1] = (unsigned char)floorf((g0 + g1) * 0.5f * 255.0f + 0.5f);
        colors[2][2] = (unsigned char)floorf((b0 + b1) * 0.5f * 255.0f + 0.5f);
        colors[2][3] = 0xFF;

        /* color3 = transparent black */
        colors[3][0] = 0;
        colors[3][1] = 0;
        colors[3][2] = 0;
        colors[3][3] = 0;
    }
    else
    {
        /* 4-color mode */
        colors[0][0] = (unsigned char)floorf(r0 * 255.0f + 0.5f);
        colors[0][1] = (unsigned char)floorf(g0 * 255.0f + 0.5f);
        colors[0][2] = (unsigned char)floorf(b0 * 255.0f + 0.5f);
        colors[0][3] = 0xFF;

        colors[1][0] = (unsigned char)floorf(r1 * 255.0f + 0.5f);
        colors[1][1] = (unsigned char)floorf(g1 * 255.0f + 0.5f);
        colors[1][2] = (unsigned char)floorf(b1 * 255.0f + 0.5f);
        colors[1][3] = 0xFF;

        /* color2 = (2*color0 + color1) / 3 */
        colors[2][0] = (unsigned char)floorf((r0 * 2.0f + r1) / 3.0f * 255.0f + 0.5f);
        colors[2][1] = (unsigned char)floorf((g0 * 2.0f + g1) / 3.0f * 255.0f + 0.5f);
        colors[2][2] = (unsigned char)floorf((b0 * 2.0f + b1) / 3.0f * 255.0f + 0.5f);
        colors[2][3] = 0xFF;

        /* color3 = (color0 + 2*color1) / 3 */
        colors[3][0] = (unsigned char)floorf((r0 + r1 * 2.0f) / 3.0f * 255.0f + 0.5f);
        colors[3][1] = (unsigned char)floorf((g0 + g1 * 2.0f) / 3.0f * 255.0f + 0.5f);
        colors[3][2] = (unsigned char)floorf((b0 + b1 * 2.0f) / 3.0f * 255.0f + 0.5f);
        colors[3][3] = 0xFF;
    }

    /* read 32-bit index table */
    indices = *(unsigned int *)(src + 4);

    /* decode 4x4 block */
    for (row = 0; row < 4; row++)
    {
        int pixelRow = blockY + row;
        for (col = 0; col < 4; col++)
        {
            int pixelCol = blockX + col;
            int pixelOfs = (pixelRow * dst->stride + pixelCol) * 4;
            int idx = indices & 3;
            indices >>= 2;
            dst->pixels[pixelOfs + 0] = colors[idx][0];
            dst->pixels[pixelOfs + 1] = colors[idx][1];
            dst->pixels[pixelOfs + 2] = colors[idx][2];
            dst->pixels[pixelOfs + 3] = colors[idx][3];
        }
    }
}

/*
================
DecodeDXT1

Wrapper for DecodeDXT1Block with hasAlpha = 0.
================
*/
void DecodeDXT1(unsigned char *src, ImageDecodeState_t *dst, int blockX, int blockY)
{
    DecodeDXT1Block(src, dst, blockX, blockY, 0);
}

/*
================
DecodeDXT3

Decodes a DXT3 block (16 bytes). First 8 bytes are explicit 4-bit alpha,
last 8 bytes are DXT1 color block (decoded with hasAlpha=1).
================
*/
void DecodeDXT3(unsigned char *src, ImageDecodeState_t *dst, int blockX, int blockY)
{
    int row, col;

    DecodeDXT1Block(src + 8, dst, blockX, blockY, 1);

    for (row = 0; row < 4; row++)
    {
        for (col = 0; col < 4; col += 2)
        {
            int pixelOfs;
            unsigned char alphaByte = src[row * 2 + col / 2];

            pixelOfs = ((blockY + row) * dst->stride + blockX + col) * 4;
            dst->pixels[pixelOfs + 3] = (unsigned char)((alphaByte & 0x0F) * 0x11);

            pixelOfs = ((blockY + row) * dst->stride + blockX + col + 1) * 4;
            dst->pixels[pixelOfs + 3] = (unsigned char)((alphaByte >> 4) * 0x11);
        }
    }
}

/*
================
DecodeDXT5

Decodes a DXT5 block (16 bytes). First 8 bytes are interpolated alpha,
last 8 bytes are DXT1 color block (decoded with hasAlpha=1).
================
*/
void DecodeDXT5(unsigned char *src, ImageDecodeState_t *dst, int blockX, int blockY)
{
    unsigned char alpha0, alpha1;
    unsigned char alphaTable[8];
    unsigned char alphaIndices[16];
    unsigned int bits;
    int row, col, i;

    DecodeDXT1Block(src + 8, dst, blockX, blockY, 1);

    alpha0 = src[0];
    alpha1 = src[1];
    alphaTable[0] = alpha0;
    alphaTable[1] = alpha1;

    if (alpha0 > alpha1)
    {
        float inv7 = 1.0f / 7.0f;
        alphaTable[2] = (unsigned char)floorf((6 * alpha0 + 1 * alpha1) * inv7 + 0.5f);
        alphaTable[3] = (unsigned char)floorf((5 * alpha0 + 2 * alpha1) * inv7 + 0.5f);
        alphaTable[4] = (unsigned char)floorf((4 * alpha0 + 3 * alpha1) * inv7 + 0.5f);
        alphaTable[5] = (unsigned char)floorf((3 * alpha0 + 4 * alpha1) * inv7 + 0.5f);
        alphaTable[6] = (unsigned char)floorf((2 * alpha0 + 5 * alpha1) * inv7 + 0.5f);
        alphaTable[7] = (unsigned char)floorf((1 * alpha0 + 6 * alpha1) * inv7 + 0.5f);
    }
    else
    {
        float inv5 = 1.0f / 5.0f;
        alphaTable[2] = (unsigned char)floorf((4 * alpha0 + 1 * alpha1) * inv5 + 0.5f);
        alphaTable[3] = (unsigned char)floorf((3 * alpha0 + 2 * alpha1) * inv5 + 0.5f);
        alphaTable[4] = (unsigned char)floorf((2 * alpha0 + 3 * alpha1) * inv5 + 0.5f);
        alphaTable[5] = (unsigned char)floorf((1 * alpha0 + 4 * alpha1) * inv5 + 0.5f);
        alphaTable[6] = 0;
        alphaTable[7] = 255;
    }

    bits = (unsigned int)src[2] | ((unsigned int)src[3] << 8) | ((unsigned int)src[4] << 16);
    for (i = 0; i < 8; i++) { alphaIndices[i] = bits & 7; bits >>= 3; }
    bits = (unsigned int)src[5] | ((unsigned int)src[6] << 8) | ((unsigned int)src[7] << 16);
    for (i = 8; i < 16; i++) { alphaIndices[i] = bits & 7; bits >>= 3; }

    for (row = 0; row < 4; row++)
        for (col = 0; col < 4; col++)
        {
            int pixelOfs = ((blockY + row) * dst->stride + blockX + col) * 4;
            dst->pixels[pixelOfs + 3] = alphaTable[alphaIndices[row * 4 + col]];
        }
}

/*
================
Image_ConvertPixels

Converts source pixel data to RGBA format based on the source format type.
Switch on format byte at srcInfo[+4]: handles BGRA, BGR, LA, L, and DXT formats.
================
*/
void Image_ConvertPixels(ImageDecodeState_t *dst, ImageInfo_t *srcInfo, unsigned char *srcData)
{
    int pixelCount = (int)srcInfo->width * (int)srcInfo->height;
    unsigned char *out = dst->pixels;
    unsigned char *in = srcData;
    int i;
    int fmt = srcInfo->format - 1;

    switch (fmt)
    {
    case 0: /* BGRA → RGBA (format 1,6) */
    case 5:
        for (i = 0; i < pixelCount; i++)
        {
            out[0] = in[2]; /* R = B */
            out[1] = in[1]; /* G */
            out[2] = in[0]; /* B = R */
            out[3] = in[3]; /* A */
            out += 4;
            in += 4;
        }
        break;

    case 1: /* BGR → RGBA (format 2,7) */
    case 6:
        for (i = 0; i < pixelCount; i++)
        {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = 0xFF;
            out += 4;
            in += 3;
        }
        break;

    case 2: /* LA → RGBA (format 3,8) */
    case 7:
        for (i = 0; i < pixelCount; i++)
        {
            out[0] = in[0];
            out[1] = in[0];
            out[2] = in[0];
            out[3] = in[1];
            out += 4;
            in += 2;
        }
        break;

    case 3: /* L → RGBA (format 4,9) */
    case 8:
        for (i = 0; i < pixelCount; i++)
        {
            out[0] = in[0];
            out[1] = in[0];
            out[2] = in[0];
            out[3] = 0xFF;
            out += 4;
            in += 1;
        }
        break;

    case 4: /* A → RGBA (format 5,10) */
    case 9:
        for (i = 0; i < pixelCount; i++)
        {
            out[0] = 0;
            out[1] = 0;
            out[2] = 0;
            out[3] = in[0];
            out += 4;
            in += 1;
        }
        break;

    default:
        break;
    }
}

static char s_assertDisable_Decode_image;
static char s_assertDisable_Decode_imageFile;
static char s_assertDisable_Decode_dim;
static char s_assertDisable_Decode2_image;
static char s_assertDisable_Decode2_imageFile;

extern void *Hunk_AllocateTempMemory(int size);
extern void Hunk_FreeTempMemory(void *ptr);
extern void memset_fast(void *dst, int val, int size);
extern void Image_WaveletDecode(unsigned char *src, unsigned char *dst,
                                ImageDecodeState_t *state);

/*
================
Image_DecodeCompressed

Decodes compressed (wavelet) image data with mipmap and cubemap support.
Allocates intermediate buffers, decompresses each face/mip via wavelet,
converts the first mip of the first face to RGBA via Image_ConvertPixels.
================
*/
void Image_DecodeCompressed(ImageDecodeState_t *dst, ImageInfo_t *srcInfo,
                            unsigned char *srcData, int bytesPerPixel)
{
    int width, height;
    int faceCount, mipCount;
    int totalPixels;
    int mip, face;
    unsigned char *allocPtrs[6];  /* original allocation pointers for freeing */
    unsigned char *workPtrs[6];   /* working pointers advanced during decode */
    WaveletDecodeState_t decodeState;

    Assert(dst, s_assertDisable_Decode_image);
    Assert(srcInfo, s_assertDisable_Decode_imageFile);
    Assert(srcInfo->depth == 1, s_assertDisable_Decode_dim);

    width = srcInfo->width;
    height = srcInfo->height;
    faceCount = (srcInfo->flags & 4) ? 6 : 1;

    if (!(srcInfo->flags & 2))
    {
        /* no mipmaps flag — compute full mip chain */
        mipCount = 0;
        {
            int w = width, h = height;
            while (w > 1 || h > 1)
            {
                mipCount++;
                w = (w > 1) ? w / 2 : 1;
                h = (h > 1) ? h / 2 : 1;
            }
        }
    }
    else
    {
        /* has mipmaps flag — process only mip 0 */
        mipCount = 0;
    }

    totalPixels = width * height * bytesPerPixel;

    /* allocate face buffers */
    memset(allocPtrs, 0, sizeof(allocPtrs));
    for (face = 0; face < faceCount; face++)
    {
        allocPtrs[face] = (unsigned char *)Hunk_AllocateTempMemory(totalPixels);
        workPtrs[face] = allocPtrs[face];
    }

    /* setup wavelet decode state */
    memset(&decodeState, 0, sizeof(decodeState));
    decodeState.width = width;
    decodeState.height = height;
    decodeState.channels = (bytesPerPixel >= 4) ? 3 : bytesPerPixel;
    decodeState.bpp = bytesPerPixel;

    /* decode each mip level from highest to lowest */
    for (mip = mipCount; mip >= 0; mip--)
    {
        int mipW = width >> mip;
        int mipH = height >> mip;
        int mipPixels;
        if (mipW < 1) mipW = 1;
        if (mipH < 1) mipH = 1;
        mipPixels = mipW * mipH * bytesPerPixel;

        decodeState.mipLevel = mip;

        for (face = 0; face < faceCount; face++)
        {
            /* binary passes different src/dst: dst = allocPtr + totalPixels - mipPixels */
            unsigned char *decodeDst = allocPtrs[face] + totalPixels - mipPixels;
            decodeState.initialized = 0;

            Wavelet_Decompress(srcData, decodeDst, &decodeState);
            srcData += mipPixels; /* advance past this mip's compressed data */

            /* convert first face at mip 0 */
            if (face == 0 && mip == 0)
                Image_ConvertPixels(dst, srcInfo, allocPtrs[0]);
        }
    }

    /* free face buffers using original allocation pointers */
    for (face = faceCount - 1; face >= 0; face--)
        Hunk_FreeTempMemory(allocPtrs[face]);
}

/*
================
Image_DecodeDXT

Decodes DXT-compressed image data with mipmap and cubemap support.
Iterates over 4x4 blocks calling the appropriate block decoder
(DXT1/DXT3/DXT5). Only decodes the first face at mip 0 to output.
================
*/
static char s_assertDisable_DXT_image;
static char s_assertDisable_DXT_imageFile;
static char s_assertDisable_DXT_blockSize;

typedef void (*DxtBlockFunc)(unsigned char *, ImageDecodeState_t *, int, int);

void Image_DecodeDXT(ImageDecodeState_t *dst, ImageInfo_t *srcInfo,
                     unsigned char *srcData, int bytesPerBlock)
{
    int width, height;
    int faceCount;
    int mipCount;
    int mip, face;
    int blockExpected;
    DxtBlockFunc blockDecoder;

    Assert(dst, s_assertDisable_DXT_image);
    Assert(srcInfo, s_assertDisable_DXT_imageFile);

    /* verify bytesPerBlock matches format */
    blockExpected = (srcInfo->format == 11) ? 8 : 16;
    Assert(bytesPerBlock == blockExpected, s_assertDisable_DXT_blockSize);

    faceCount = (srcInfo->flags & 4) ? 6 : 1;

    /* select block decoder */
    if (srcInfo->format == 11)
        blockDecoder = DecodeDXT1;
    else if (srcInfo->format == 12)
        blockDecoder = (DxtBlockFunc)DecodeDXT3;
    else
        blockDecoder = (DxtBlockFunc)DecodeDXT5;

    width = srcInfo->width;
    height = srcInfo->height;

    if (!(srcInfo->flags & 2))
    {
        /* no "no-mipmaps" flag — compute full mip chain (same semantics as
         * Image_DecodeUncompressed). flag bit 1 means "no mipmaps"; absent
         * flag means the file contains the full mip chain. */
        mipCount = 0;
        {
            int w = width, h = height;
            while (w > 1 || h > 1)
            {
                mipCount++;
                w = (w > 1) ? w / 2 : 1;
                h = (h > 1) ? h / 2 : 1;
            }
        }

        for (mip = mipCount; mip >= 0; mip--)
        {
            int mipW = width >> mip;
            int mipH = height >> mip;
            int blockW, blockH;
            if (mipW < 1) mipW = 1;
            if (mipH < 1) mipH = 1;
            blockW = (mipW + 3) / 4;
            blockH = (mipH + 3) / 4;

            for (face = 0; face < faceCount; face++)
            {
                if (face == 0 && mip == 0)
                {
                    /* decode this face to output */
                    int bx, by;
                    for (by = 0; by < blockH; by++)
                        for (bx = 0; bx < blockW; bx++)
                        {
                            blockDecoder(srcData + (by * blockW + bx) * bytesPerBlock,
                                        dst, bx * 4, by * 4);
                        }
                }
                srcData += blockW * blockH * bytesPerBlock;
            }
        }
    }
    else
    {
        /* flag bit 1 set — single mip, single face */
        int blockW = (width + 3) / 4;
        int blockH = (height + 3) / 4;
        int bx, by;

        Assert(srcInfo->depth == 1, s_assertDisable_Decode_dim);

        for (by = 0; by < blockH; by++)
            for (bx = 0; bx < blockW; bx++)
            {
                blockDecoder(srcData, dst, bx * 4, by * 4);
                srcData += bytesPerBlock;
            }
    }
}

/*
================
Image_LoadIWI

Loads an IWI image file by name. Builds path "images/" + name + ".iwi",
reads and validates the file, sets up the ImageDecodeState struct
(width, height, RGBA pixel buffer), and dispatches to the appropriate
format-specific decoder.
================
*/
static char s_assertDisable_LoadIWI_name;

/*
================
Image_ValidateHeader

Checks that the image file starts with the "IWi" magic bytes and
has CoD4 version 6. Returns 1 on success, 0 on failure (and prints an
error message via Com_Printf).
================
*/
int Image_ValidateHeader(void *fileData, char *path)
{
    unsigned char *bytes = (unsigned char *)fileData;
    unsigned char version;

    if (bytes[0] != 'I' || bytes[1] != 'W' || bytes[2] != 'i')
    {
        Com_Printf("^1ERROR: image '%s' is not a PC IW image file\n", path);
        return 0;
    }

    version = bytes[3];
    if (version != 6)
    {
        Com_Printf("^1ERROR: image '%s' is version %i but should be %i\n",
                 path, (int)version, 6);
        return 0;
    }

    return 1;
}
extern void *Z_Malloc(int size);                        /* com_memory_429EE0 */

void Image_LoadIWI(const char *imageName, ImageDecodeState_t *dst)
{
    char path[64];
    void *fileData;
    ImageInfo_t *header;
    int width, height;
    unsigned char *pixelData;
    int format;

    Assert(imageName, s_assertDisable_LoadIWI_name);

    /* build file path and load */
    Com_AssembleFilepath("images/", imageName, ".iwi", path, 64);
    if (FS_ReadFile(path, &fileData) < 0)
        Com_ErrorMsg(1, "image '%s' is missing", path);

    /* validate IWI header */
    if (!Image_ValidateHeader(fileData, path))
        Com_ErrorMsg(1, "image '%s' is not valid", path);

    /* copy name to dst struct */
    {
        const char *src = imageName;
        char *dstName = (char *)dst;
        while (*src)
            *dstName++ = *src++;
        *dstName = 0;
    }

    /* setup decode state from header */
    header = (ImageInfo_t *)fileData;
    width = (int)header->width;
    height = (int)header->height;
    dst->stride = width;
    dst->height = height;

    /* allocate RGBA pixel buffer */
    dst->pixels = (unsigned char *)Z_Malloc(width * height * 4);

    /* decode based on format */
    pixelData = (unsigned char *)fileData + 0x1C;
    format = header->format;

    switch (format)
    {
    /* formats 1-5: uncompressed */
    case 1:
        dst->flags44 = 1;
        Image_DecodeUncompressed(dst, header, pixelData, 4);
        break;
    case 2:
        Image_DecodeUncompressed(dst, header, pixelData, 3);
        break;
    case 3:
        dst->flags44 = 1;
        Image_DecodeUncompressed(dst, header, pixelData, 2);
        break;
    case 4:
    case 5:
        Image_DecodeUncompressed(dst, header, pixelData, 1);
        break;
    /* formats 6-10: compressed (wavelet) */
    case 6:
        dst->flags44 = 1;
        Image_DecodeCompressed(dst, header, pixelData, 4);
        break;
    case 7:
        Image_DecodeCompressed(dst, header, pixelData, 3);
        break;
    case 8:
        dst->flags44 = 1;
        Image_DecodeCompressed(dst, header, pixelData, 2);
        break;
    case 9:
    case 10:
        dst->flags44 = 1;
        Image_DecodeCompressed(dst, header, pixelData, 1);
        break;
    /* formats 11-13: DXT compressed */
    case 11:
        Image_DecodeDXT(dst, header, pixelData, 8);
        break;
    case 12:
        dst->flags44 = 1;
        Image_DecodeDXT(dst, header, pixelData, 16);
        break;
    case 13:
        dst->flags44 = 1;
        Image_DecodeDXT(dst, header, pixelData, 16);
        break;
    default:
        break;
    }

    FS_FreeFile(fileData); /* binary frees loaded file data at end */
}

/*
================
Image_DecodeUncompressed

Decodes uncompressed image data with mipmap and cubemap support.
Iterates mip levels from highest to lowest. Only converts the
first face at mip level 0 via Image_ConvertPixels. Advances
srcData past all face data at each mip level.
================
*/
void Image_DecodeUncompressed(ImageDecodeState_t *dst, ImageInfo_t *srcInfo,
                              unsigned char *srcData, int bytesPerPixel)
{
    int width, height;
    int faceCount;
    int mipCount;
    int mip, face;

    Assert(dst, s_assertDisable_Decode2_image);
    Assert(srcInfo, s_assertDisable_Decode2_imageFile);

    width = srcInfo->width;
    height = srcInfo->height;
    faceCount = (srcInfo->flags & 4) ? 6 : 1;

    if (!(srcInfo->flags & 2))
    {
        /* no mipmaps flag — compute full mip chain */
        Assert(srcInfo->depth == 1, s_assertDisable_Decode_dim);

        /* count mip levels */
        mipCount = 0;
        {
            int w = width, h = height;
            while (w > 1 || h > 1)
            {
                mipCount++;
                w = (w > 1) ? w / 2 : 1;
                h = (h > 1) ? h / 2 : 1;
            }
        }

        for (mip = mipCount; mip >= 0; mip--)
        {
            int mipW = width >> mip;
            int mipH = height >> mip;
            int mipSize;
            if (mipW < 1) mipW = 1;
            if (mipH < 1) mipH = 1;
            mipSize = mipW * mipH * bytesPerPixel;

            for (face = 0; face < faceCount; face++)
            {
                if (face == 0 && mip == 0)
                    Image_ConvertPixels(dst, srcInfo, srcData);
                srcData += mipSize;
            }
        }
    }
    else
    {
        /* has mipmaps flag — process only mip 0 */
        /* mipCount = 0 means process only the base mip level */
        mipCount = 0;

        for (mip = mipCount; mip >= 0; mip--)
        {
            int mipW = width >> mip;
            int mipH = height >> mip;
            int mipSize;
            if (mipW < 1) mipW = 1;
            if (mipH < 1) mipH = 1;
            mipSize = mipW * mipH * bytesPerPixel;

            for (face = 0; face < faceCount; face++)
            {
                if (face == 0 && mip == 0)
                    Image_ConvertPixels(dst, srcInfo, srcData);
                srcData += mipSize;
            }
        }
    }
}

