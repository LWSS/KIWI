#pragma once
#include <stdint.h>

// KIWI native x64 stream ABI, independent of retail IWff version 5.
// A version bump is required whenever any serialized engine layout changes.
#define DB64_FASTFILE_MAGIC "KIWIfF64"
#define DB64_FASTFILE_VERSION 2
#define DB64_STREAM_ALIGNMENT 16

struct DB64FilePrefix
{
    char magic[8];
    uint32_t version;
};
static_assert(sizeof(DB64FilePrefix) == 12);

// A typed name follows in stream block four instead of an inline asset payload.
#define DB64_EXTERNAL_ASSET_TOKEN (UINTPTR_MAX - 2)
struct DB64ExternalAssetRecord
{
    int32_t type;
    uint32_t nameSize; // Includes the terminating NUL; limited to 1024 bytes.
};
static_assert(sizeof(DB64ExternalAssetRecord) == 8);
