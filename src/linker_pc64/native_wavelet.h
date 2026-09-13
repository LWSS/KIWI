#pragma once
#include <stddef.h>
struct GfxImage;
struct GfxImageFileHeader;
bool Linker_ImportWavelet(const GfxImageFileHeader *header, const void *data, size_t size, const char *name,
                          unsigned char semantic, unsigned char track, GfxImage **image, char *error, size_t errorSize);
