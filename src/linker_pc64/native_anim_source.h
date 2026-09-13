#pragma once
#include <stddef.h>

// Validate the complete version-17 raw animation before invoking its converter.
bool Linker_ValidateAnimationSource(const void *data, size_t size, char *error, size_t errorSize);
