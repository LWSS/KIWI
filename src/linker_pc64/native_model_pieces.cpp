#include <universal/q_shared.h>
#include <xanim/xmodel.h>
#include <database64/db_package.h>
#include "native_model_pieces.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

void Linker_FreeModelPieces(XModelPieces *pieces)
{
    if (pieces)
    {
        free((void *)pieces->name);
        free(pieces->pieces);
        free(pieces);
    }
}

bool Linker_ImportModelPieces(const void *data, size_t size, const char *name, LinkerResolvePieceModel resolve,
                              void *context, XModelPieces **pieces, char *error, size_t errorSize)
{
    if (!pieces || (!error && errorSize))
    {
        return false;
    }
    *pieces = NULL;
    char normalized[DB64_PACKAGE_PATH];
    bool valid = data && size >= 4 && name && DB64_NormalizePath(name, normalized, sizeof(normalized)) && resolve;
    uint16_t version = 0, count = 0;
    if (valid)
    {
        memcpy(&version, data, sizeof(uint16_t));
        memcpy(&count, (const uint8_t *)data + 2, sizeof(uint16_t));
        valid = version == 1 && count && count <= (size - 4) / 14;
    }
    XModelPieces *result = valid ? (XModelPieces *)calloc(1, sizeof(XModelPieces)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->name = _strdup(normalized);
        result->numpieces = count;
        result->pieces = (XModelPiece *)calloc(count, sizeof(XModelPiece));
        valid = result->name && result->pieces;
    }
    size_t position = 4;
    // Validate the entire record before invoking a dependency compiler.
    for (unsigned int pass = 0; valid && pass < 2; ++pass)
    {
        position = 4;
        for (unsigned int i = 0; valid && i < count; ++i)
        {
            const char *source = (const char *)data + position;
            const char *end = (const char *)memchr(source, 0, size - position);
            valid = end && end != source && end - source < 64;
            if (!valid)
            {
                break;
            }
            position += end - source + 1;
            valid = size - position >= sizeof(float[3]) && DB64_NormalizePath(source, normalized, sizeof(normalized));
            if (!valid)
            {
                break;
            }
            memcpy(result->pieces[i].offset, (const uint8_t *)data + position, sizeof(float[3]));
            position += sizeof(float[3]);
            for (unsigned int axis = 0; axis < 3; ++axis)
            {
                valid = valid && isfinite(result->pieces[i].offset[axis]);
            }
            if (valid && pass)
            {
                result->pieces[i].model = resolve(normalized, context);
                valid = result->pieces[i].model != NULL;
            }
        }
        valid = valid && position == size;
    }
    if (!valid)
    {
        Linker_FreeModelPieces(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid model-pieces v1 source or unresolved model: %s", name ? name : "<null>");
        }
        return false;
    }
    *pieces = result;
    return true;
}
