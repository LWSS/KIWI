#pragma once
#include <stddef.h>
struct XSurface;

void DB64_LoadModelSurface(XSurface *surface, bool atStreamStart);
bool DB64_ValidateModelSurface(const XSurface *surface, unsigned int boneCount, char *error, size_t errorSize);
