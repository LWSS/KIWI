#pragma once
#include <stdint.h>
struct WaveletHuffmanDecode
{
    int16_t value;
    int16_t bits;
};
extern const WaveletHuffmanDecode waveletDecodeBlue[4096];
extern const WaveletHuffmanDecode waveletDecodeRedGreen[4096];
extern const WaveletHuffmanDecode waveletDecodeAlpha[4096];
