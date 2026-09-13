#pragma once
#include <stddef.h>
// Reads loose files then unlocalized IWDs in raw, followed by the same in main.
// IWDs use descending filename order within each directory.
// Normalizes relative paths and appends an uncounted NUL byte. Loose raw files take precedence.
// Caller frees data. Failed reads return NULL and zero size.
// Optional found is also true on I/O/archive errors, so optional dependencies cannot silently disappear.
bool Linker_ReadRawAssetFile(const char *root, const char *relative, void **data, size_t *size,
                             size_t limit = 16 * 1024 * 1024, bool *found = NULL);
typedef bool (*LinkerRawAssetVisitor)(const char *relative, void *context);
// Enumerates direct children in loose raw/main files and unlocalized raw/main IWDs.
// An empty extension enumerates all direct files, including extensionless weapons.
// Names are normalized; duplicates may occur. Read through Linker_ReadRawAssetFile
// to apply source precedence. Callback order is unspecified.
bool Linker_EnumerateRawAssetFiles(const char *root, const char *directory, const char *extension,
                                   LinkerRawAssetVisitor visit, void *context);

// Prefer the selected language directory, then matching localized_<language>_*.iwd
// archives, then the base asset. Errors in a higher-priority source do not fall back.
bool Linker_ReadLocalizedAssetFile(const char *root, const char *relative, const char *language,
                                  void **data, size_t *size, size_t limit = 16 * 1024 * 1024,
                                  bool *found = NULL);
