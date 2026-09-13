#include "native_anim_source.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

struct AnimationReader
{
    const uint8_t *data;
    size_t size;
    size_t offset;
};

static bool Take(AnimationReader *reader, size_t count, const uint8_t **value)
{
    if (count > reader->size - reader->offset)
    {
        return false;
    }
    if (value)
    {
        *value = reader->data + reader->offset;
    }
    reader->offset += count;
    return true;
}

static bool Number(AnimationReader *reader, unsigned int width, unsigned int *value)
{
    const uint8_t *bytes;
    if (!Take(reader, width, &bytes))
    {
        return false;
    }
    *value = bytes[0];
    if (width == 2)
    {
        *value |= (unsigned int)bytes[1] << 8;
    }
    return true;
}

static bool String(AnimationReader *reader)
{
    const uint8_t *begin = reader->data + reader->offset;
    const uint8_t *end = (const uint8_t *)memchr(begin, 0, reader->size - reader->offset);
    if (!end || end == begin)
    {
        return false;
    }
    reader->offset += (size_t)(end - begin) + 1;
    return true;
}

static bool Floats(AnimationReader *reader, unsigned int count)
{
    for (unsigned int i = 0; i < count; ++i)
    {
        const uint8_t *bytes;
        float value;
        if (!Take(reader, sizeof(float), &bytes))
        {
            return false;
        }
        memcpy(&value, bytes, sizeof(float));
        if (!isfinite(value))
        {
            return false;
        }
    }
    return true;
}

static bool Indices(AnimationReader *reader, unsigned int count, unsigned int frames)
{
    if (count <= 1 || count == frames)
    {
        return true;
    }
    unsigned int previous = 0;
    for (unsigned int i = 0; i < count; ++i)
    {
        unsigned int index;
        if (!Number(reader, frames <= 256 ? 1 : 2, &index) || index >= frames || (i && index <= previous))
        {
            return false;
        }
        previous = index;
    }
    return true;
}

static bool Quaternion(AnimationReader *reader, unsigned int frames, bool simple)
{
    unsigned int count;
    if (!Number(reader, 2, &count) || count > frames || (!count && !simple) || !Indices(reader, count, frames))
    {
        return false;
    }
    return Take(reader, count * (simple ? 2 : 6), NULL);
}

static bool Translation(AnimationReader *reader, unsigned int frames)
{
    unsigned int count;
    if (!Number(reader, 2, &count) || count > frames || !Indices(reader, count, frames))
    {
        return false;
    }
    if (count <= 1)
    {
        return !count || Floats(reader, 3);
    }
    unsigned int small;
    return Number(reader, 1, &small) && small <= 1 && Floats(reader, 6) &&
           Take(reader, count * (small ? 3 : 6), NULL);
}

bool Linker_ValidateAnimationSource(const void *data, size_t size, char *error, size_t errorSize)
{
    AnimationReader reader = {(const uint8_t *)data, size, 0};
    unsigned int version = 0, frames = 0, bones = 0, flags = 0, assetType = 0, rate = 0;
    bool ok = data && Number(&reader, 2, &version) && version == 17 && Number(&reader, 2, &frames) && frames &&
              Number(&reader, 2, &bones) && bones <= 128 && Number(&reader, 1, &flags) &&
              Number(&reader, 1, &assetType) && Number(&reader, 2, &rate) && rate <= 32767;
    if (ok && (flags & 1))
    {
        ok = frames < 65535;
        ++frames;
    }
    if (ok && (flags & 2))
    {
        ok = Quaternion(&reader, frames, true) && Translation(&reader, frames);
    }
    const uint8_t *simple = NULL;
    if (ok && bones)
    {
        const unsigned int bitBytes = (bones + 7) / 8;
        ok = Take(&reader, bitBytes, NULL) && Take(&reader, bitBytes, &simple);
    }
    for (unsigned int i = 0; ok && i < bones; ++i)
    {
        ok = String(&reader);
    }
    for (unsigned int i = 0; ok && i < bones; ++i)
    {
        ok = Quaternion(&reader, frames, (simple[i / 8] & (1 << (i & 7))) != 0) && Translation(&reader, frames);
    }
    unsigned int notifications = 0;
    ok = ok && Number(&reader, 1, &notifications) && notifications < 255;
    for (unsigned int i = 0; ok && i < notifications; ++i)
    {
        unsigned int frame;
        ok = String(&reader) && Number(&reader, 2, &frame) && frame < frames;
    }
    ok = ok && reader.offset == size;
    if (errorSize)
    {
        if (ok)
        {
            error[0] = 0;
        }
        else
        {
            snprintf(error, errorSize, "Invalid version-17 animation at byte %zu of %zu", reader.offset, size);
        }
    }
    return ok;
}
