#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct DB64MemoryStream
{
    int size;
    int position;
    unsigned char *data;
};

static inline DB64MemoryStream *DB64_CreateMemoryStream(const void *data, int size)
{
    if (!data || size < 0)
    {
        return NULL;
    }
    DB64MemoryStream *stream = (DB64MemoryStream *)malloc(sizeof(DB64MemoryStream) + (size_t)size);
    if (stream)
    {
        stream->size = size;
        stream->position = 0;
        stream->data = (unsigned char *)(stream + 1);
        memcpy(stream->data, data, size);
    }
    return stream;
}

// Miles seek origins: start=0, current=1, end=2. Invalid seeks leave position unchanged.
static inline int DB64_SeekMemoryStream(DB64MemoryStream *stream, int offset, unsigned int origin)
{
    if (!stream || origin > 2)
    {
        return -1;
    }
    const int64_t base = origin == 0 ? 0 : (origin == 1 ? stream->position : stream->size);
    const int64_t position = base + offset;
    if (position < 0 || position > INT_MAX)
    {
        return -1;
    }
    stream->position = (int)position;
    return stream->position;
}

static inline unsigned int DB64_ReadMemoryStream(DB64MemoryStream *stream, void *buffer, unsigned int size)
{
    if (!stream || (!buffer && size) || stream->position >= stream->size)
    {
        return 0;
    }
    const unsigned int remaining = stream->size - stream->position;
    const unsigned int count = size < remaining ? size : remaining;
    if (count)
    {
        memcpy(buffer, stream->data + stream->position, count);
        stream->position += count;
    }
    return count;
}
