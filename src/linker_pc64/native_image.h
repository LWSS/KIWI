#pragma once
#include <stddef.h>
#include <stdint.h>

struct GfxImage;
// Import converted IWI v6 bitmap/DXT pixels into a compiler-owned load definition.
bool Linker_ImportImage(const void *data, size_t size, const char *name, uint8_t semantic, uint8_t track,
                        GfxImage **image, char *error, size_t errorSize);
void Linker_FreeImage(GfxImage *image);
bool Linker_CreateBuiltinImage(const char *name, uint8_t semantic, uint8_t track,
                               GfxImage **image, char *error, size_t errorSize);
// Pixels use the native loader's face-major, largest-mip-first order.
bool Linker_CreateImage(const char *name, unsigned int width, unsigned int height, int format, uint8_t flags,
                        uint8_t category, uint8_t semantic, uint8_t track, const void *pixels, size_t size,
                        GfxImage **image, char *error, size_t errorSize);
