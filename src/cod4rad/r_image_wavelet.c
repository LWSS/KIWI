/*
 * r_image_wavelet.c — Wavelet image decompression.
 */

#include "cod2rad64.h"

static char s_assertDisable_ReadBits_count;
static char s_assertDisable_ReadBits_bit;
static char s_assertDisable_DecodeCoeff_bpp;
static char s_assertDisable_Decompress_bpp;
static char s_assertDisable_Decompress_mip;
static char s_assertDisable_Decompress_overlap;
static char s_assertDisable_Decompress_size;

/*
================
Wavelet_ReadBits

Reads bitCount bits from the wavelet bitstream. Updates the decode
state (bit buffer, bit position, byte pointer). Returns result in
the bits field of the decode state.
================
*/
void Wavelet_ReadBits(int bitCount, WaveletDecodeState_t *state)
{
    unsigned int rawBits;
    int oldBitPos;
    int newBitPos;
    unsigned char *ptr;

    Assert(bitCount > 0 && bitCount <= 16, s_assertDisable_ReadBits_count);
    Assert(state->bitPos < 8, s_assertDisable_ReadBits_bit);

    state->bits >>= bitCount;

    ptr = state->bytePtr;
    rawBits = ((unsigned int)ptr[3] << 24) |
              ((unsigned int)ptr[2] << 16) |
              ((unsigned int)ptr[1] << 8) |
              (unsigned int)ptr[0];

    oldBitPos = state->bitPos;
    newBitPos = oldBitPos + bitCount;
    rawBits >>= oldBitPos;
    rawBits = (unsigned short)(rawBits << (16 - bitCount));

    state->bits |= (unsigned short)rawBits;

    state->bytePtr = ptr + (newBitPos >> 3);
    state->bitPos = newBitPos & 7;
}

/*
================
Wavelet_DecodeCoefficients

Decodes wavelet coefficients from a compressed bitstream using a
Huffman-like lookup table. Reads variable-length codes, handles
escape sequences for raw 9-bit values. Processes width*height
coefficients per call.
================
*/
/* g_waveletDecodeTable0 / g_waveletDecodeTable1 defined in wavelet_tables.c
 * with extracted values from cod2rad64.exe .rdata @ 0x462AB0 and 0x466AB0. */

void Wavelet_DecodeCoefficients(unsigned char *dst, int bytesPerPixel,
                                WaveletDecodeState_t *state)
{
    int channels = state->channels;
    int bpp = state->bpp;
    int i, j;
    int offset;

    if (channels == bpp)
    {
        /* channels == bytesPerPixel: flat decode of all bytes */
        int totalBytes = bytesPerPixel * channels;

        for (i = 0; i < totalBytes; i++)
        {
            unsigned short bits = state->bits;
            int tableIdx = bits & 0xFFF;
            int bitCount = g_waveletDecodeTable1[tableIdx][1];
            int value;

            Wavelet_ReadBits(bitCount, state);
            value = (short)g_waveletDecodeTable1[tableIdx][0];

            if (value == (short)0x8000)
            {
                value = (state->bits & 0x1FF) - 0xFF;
                Wavelet_ReadBits(9, state);
            }

            dst[i] += (unsigned char)value;
        }
    }
    else
    {
        /* channels + 1 == bpp: decode per channel with 0xFF alpha pad */
        Assert(bpp == channels + 1, s_assertDisable_DecodeCoeff_bpp);

        offset = 0;
        for (i = 0; i < bytesPerPixel; i++)
        {
            for (j = 0; j < channels; j++)
            {
                unsigned short bits = state->bits;
                int tableIdx = bits & 0xFFF;
                int bitCount = g_waveletDecodeTable1[tableIdx][1];
                int value;

                Wavelet_ReadBits(bitCount, state);
                value = (short)g_waveletDecodeTable1[tableIdx][0];

                if (value == (short)0x8000)
                {
                    value = (state->bits & 0x1FF) - 0xFF;
                    Wavelet_ReadBits(9, state);
                }

                dst[offset] += (unsigned char)value;
                offset++;
            }
            /* pad alpha byte */
            dst[offset] = 0xFF;
            offset++;
        }
    }
}

/*
 * Two additional decode tables used by Wavelet_Decompress:
 *   g_waveletDecodeTable0 — channel 0 detail coefficients (9-bit escape)
 *   g_waveletDecodeTable1 — channels 1,2 detail coefficients (10-bit escape, delta-coded)
 * g_waveletDecodeTable (word_466AB0) is reused for alpha/single channel.
 */

/*
================
Wavelet_Decompress

Main wavelet decompression function. 6696 bytes — the largest function
in the entire codebase. Performs inverse Haar wavelet transform on
compressed image data, reconstructing pixel values from wavelet
coefficients in 2x2 blocks.

Three decode tables are used per channel type:
  - Table 0 (word_45EAB0): channel 0 detail coefficients, 9-bit escape
  - Table 1 (word_462AB0): channels 1,2 detail coefficients, 10-bit escape,
    delta-coded against channel 0
  - Table 2 (word_466AB0): alpha/single channel, 9-bit escape, independent

The inverse Haar transform reconstructs 4 pixels from 1 LL reference
pixel and 3 detail coefficients (LH, HL, HH):
  pixel(0,0) = (c0+c1+c2 + 2*ref) / 2 + parity
  pixel(0,1) = (2*ref + c0 - c1 - c2) / 2
  pixel(1,0) = (2*ref - c0 + c1 - c2) / 2
  pixel(1,1) = (2*ref - c0 - c1 + c2) / 2

Called from Image_DecodeCompressed via Wavelet_DecodeCoefficients.
================
*/

/* helper: decode one coefficient from a 12-bit Huffman table with 9-bit escape */
static int Wavelet_DecodeValue9(WaveletDecodeState_t *state,
                                unsigned short table[][2])
{
    int tableIdx = state->bits & 0xFFF;
    int bitCount = table[tableIdx][1];
    int value;

    Wavelet_ReadBits(bitCount, state);
    value = (short)table[tableIdx][0];

    if (value == (short)0x8000)
    {
        value = (state->bits & 0x1FF) - 0xFF;
        Wavelet_ReadBits(9, state);
    }

    return value;
}

/* helper: decode one coefficient from a 12-bit Huffman table with 10-bit escape */
static int Wavelet_DecodeValue10(WaveletDecodeState_t *state,
                                 unsigned short table[][2])
{
    int tableIdx = state->bits & 0xFFF;
    int bitCount = table[tableIdx][1];
    int value;

    Wavelet_ReadBits(bitCount, state);
    value = (short)table[tableIdx][0];

    if (value == (short)0x8000)
    {
        value = (state->bits & 0x3FF) - 0x1FE;
        Wavelet_ReadBits(10, state);
    }

    return value;
}

/* helper: inverse Haar wavelet transform for a 2x2 block of one channel */
static void Wavelet_InverseHaar(unsigned char *dst, int off, int bppOff, int strideOff,
                                int c0, int c1, int c2, int ref, int parity)
{
    int ref2 = ref * 2;

    dst[off]                     = (unsigned char)(((c0 + c1 + c2 + ref2) >> 1) + parity);
    dst[off + bppOff]            = (unsigned char)((ref2 + c0 - c1 - c2) >> 1);
    dst[off + strideOff]         = (unsigned char)((ref2 - c0 + c1 - c2) >> 1);
    dst[off + strideOff + bppOff] = (unsigned char)((c2 - c1 - c0 + ref2) >> 1);
}

void Wavelet_Decompress(unsigned char *src, unsigned char *dst,
                        WaveletDecodeState_t *decode)
{
    int bpp, channels, mipLevel, mipW, mipH;
    int stride, twoBpp, bppM1, twoBppM1;
    int initialParity;
    int row, col;
    unsigned char *curSrc, *curDst;

    bpp = decode->bpp;
    channels = decode->channels;
    mipLevel = decode->mipLevel;

    /* assert: bpp == channels || (bpp == 4 && channels == 3) */
    Assert(bpp == channels || (bpp == 4 && channels == 3),
           s_assertDisable_Decompress_bpp);

    /* assert: mipLevel >= 0 */
    Assert(mipLevel >= 0, s_assertDisable_Decompress_mip);

    mipW = decode->width >> mipLevel;
    mipH = decode->height >> mipLevel;

    /* small mip fallback: byte-by-byte copy from bitstream */
    if (mipW <= 1 || mipH <= 1)
    {
        int mipWClamped, mipHClamped, size;
        int i;

        mipWClamped = mipW < 1 ? 1 : mipW;
        mipHClamped = mipH < 1 ? 1 : mipH;
        size = mipWClamped + mipHClamped - 1;

        Assert(size >= 1, s_assertDisable_Decompress_size);

        curDst = dst;
        while (size > 0)
        {
            for (i = 0; i < channels; i++)
            {
                *curDst = *decode->bytePtr;
                curDst++;
                decode->bytePtr++;
            }
            if (bpp != channels)
            {
                *curDst = 0xFF;
                curDst++;
            }
            size--;
        }

        return;
    }

    /* initialize bitstream if first call */
    if (!decode->initialized)
    {
        unsigned char *ptr = decode->bytePtr;
        decode->bitPos = 0;
        decode->initialized = 1;
        decode->bits = (unsigned short)(ptr[0] | ((unsigned short)ptr[1] << 8));
        decode->bytePtr = ptr + 2;
    }

    stride = bpp * mipW;
    twoBpp = bpp * 2;
    bppM1 = bpp - 1;
    twoBppM1 = twoBpp - 1;

    /* read initial parity bit from bitstream */
    initialParity = decode->bits & 1;
    Wavelet_ReadBits(1, decode);

    /* if parity set: decode LL subband coefficients into src buffer */
    if (initialParity)
    {
        Wavelet_DecodeCoefficients(src, (mipH * mipW) / 4, decode);
    }

    /* main 2x2 block inverse wavelet transform loop */
    curSrc = src;
    curDst = dst;

    for (row = 0; row < mipH; row += 2)
    {
        for (col = 0; col < mipW; col += 2)
        {
            int ch0c0, ch0c1, ch0c2;

            /* assert: dst writes don't overlap src reads */
            Assert(curDst + stride + bpp <= curSrc || curDst > curSrc,
                   s_assertDisable_Decompress_overlap);

            if (channels != 1)
            {
                int ch0Parity;

                /*
                 * Channel 0: decode 3 detail coefficients from table 0
                 * (word_45EAB0, 9-bit escape, independent)
                 */
                ch0Parity = decode->bits & 1;
                Wavelet_ReadBits(1, decode);

                ch0c0 = Wavelet_DecodeValue9(decode, g_waveletDecodeTable0);
                ch0c1 = Wavelet_DecodeValue9(decode, g_waveletDecodeTable0);
                ch0c2 = Wavelet_DecodeValue9(decode, g_waveletDecodeTable0);

                /* inverse Haar for channel 0 */
                Wavelet_InverseHaar(curDst, 0, bpp, stride,
                                    ch0c0, ch0c1, ch0c2, curSrc[0], ch0Parity);

                /*
                 * Channels 1,2: decode from table 1 (word_462AB0, 10-bit escape)
                 * Coefficients are delta-coded against channel 0
                 */
                if (channels >= 3)
                {
                    int ch;

                    for (ch = 1; ch <= 2; ch++)
                    {
                        int chParity;
                        int c0, c1, c2;

                        chParity = decode->bits & 1;
                        Wavelet_ReadBits(1, decode);

                        c0 = Wavelet_DecodeValue10(decode, g_waveletDecodeTable1) + ch0c0;
                        c1 = Wavelet_DecodeValue10(decode, g_waveletDecodeTable1) + ch0c1;
                        c2 = Wavelet_DecodeValue10(decode, g_waveletDecodeTable1) + ch0c2;

                        Wavelet_InverseHaar(curDst, ch, bpp, stride,
                                            c0, c1, c2, curSrc[ch], chParity);
                    }
                }
            }

            /*
             * Alpha/remaining channel handling:
             * - channels == 3 with bpp == 4: fill alpha positions with 0xFF
             * - channels != 3: decode from table 2 (word_466AB0, 9-bit escape, independent)
             * - channels == 1: this is the only decode path (skipped channel 0 above)
             */
            if (channels == 3)
            {
                if (bpp != 3)
                {
                    /* pad 4 alpha bytes in the 2x2 block */
                    curDst[bppM1] = 0xFF;
                    curDst[twoBppM1] = 0xFF;
                    curDst[stride + bppM1] = 0xFF;
                    curDst[stride + twoBppM1] = 0xFF;
                }
            }
            else
            {
                int chParity;
                int c0, c1, c2;

                chParity = decode->bits & 1;
                Wavelet_ReadBits(1, decode);

                c0 = Wavelet_DecodeValue9(decode, g_waveletDecodeTable1);
                c1 = Wavelet_DecodeValue9(decode, g_waveletDecodeTable1);
                c2 = Wavelet_DecodeValue9(decode, g_waveletDecodeTable1);

                Wavelet_InverseHaar(curDst, bppM1, bpp, stride,
                                    c0, c1, c2, curSrc[bppM1], chParity);
            }

            /* advance: src by 1 pixel, dst by 2 pixels (one 2x2 block width) */
            curSrc += bpp;
            curDst += twoBpp;
        }

        /* advance dst by one extra row to skip to the next row pair */
        curDst += stride;
    }
}
