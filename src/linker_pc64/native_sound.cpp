#include <universal/q_shared.h>
#include <sound/snd_public.h>
#include "native_sound.h"
#include <database64/db_sound_assets.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <math.h>
#include <errno.h>

static bool CurveSpace(char **cursor)
{
    for (;;)
    {
        while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' || **cursor == '\n')
        {
            ++*cursor;
        }
        if ((*cursor)[0] == '/' && (*cursor)[1] == '/')
        {
            while (**cursor && **cursor != '\n')
            {
                ++*cursor;
            }
        }
        else if ((*cursor)[0] == '/' && (*cursor)[1] == '*')
        {
            char *end = strstr(*cursor + 2, "*/");
            if (!end)
            {
                return false;
            }
            *cursor = end + 2;
        }
        else
        {
            return true;
        }
    }
}

static bool CurveNumber(char **cursor, double *value)
{
    if (!CurveSpace(cursor))
    {
        return false;
    }
    char *end;
    errno = 0;
    *value = strtod(*cursor, &end);
    if (end == *cursor || errno == ERANGE || !isfinite(*value) ||
        (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n' && *end != '/'))
    {
        return false;
    }
    *cursor = end;
    return true;
}

static unsigned int Read16(const uint8_t *data)
{
    return data[0] | (data[1] << 8);
}
static unsigned int Read32(const uint8_t *data)
{
    return Read16(data) | (Read16(data + 2) << 16);
}
static bool Invalid(char *error, size_t size, const char *text)
{
    if (size)
    {
        snprintf(error, size, "%s", text);
    }
    return false;
}

bool Linker_ImportSoundCurve(const void *data, size_t size, const char *name, SndCurve **curve,
                             char *error, size_t errorSize)
{
    if (!curve || (!error && errorSize))
    {
        return false;
    }
    *curve = NULL;
    if (!data || size < 8 || size > 8192 || !name || !name[0] || strlen(name) >= 64 ||
        memcmp(data, "SNDCURVE", 8) || memchr(data, 0, size))
    {
        return Invalid(error, errorSize, "Invalid SNDCURVE source");
    }
    char text[8193];
    memcpy(text, data, size);
    text[size] = 0;
    char *cursor = text + 8;
    double count;
    SndCurve value = {};
    if (!CurveNumber(&cursor, &count) || count < 2 || count > 8 || count != floor(count))
    {
        return Invalid(error, errorSize, "Sound curve requires 2 through 8 knots");
    }
    value.knotCount = (int)count;
    for (int i = 0; i < value.knotCount; ++i)
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            double coordinate;
            if (!CurveNumber(&cursor, &coordinate) || coordinate < 0 || coordinate > 1)
            {
                return Invalid(error, errorSize, "Invalid sound curve coordinate");
            }
            value.knots[i][axis] = (float)coordinate;
        }
    }
    if (!CurveSpace(&cursor) || *cursor)
    {
        return Invalid(error, errorSize, "Unexpected data after sound curve knots");
    }
    // Match the raw engine loader's endpoint correction.
    value.knots[0][0] = 0;
    value.knots[0][1] = 1;
    value.knots[value.knotCount - 1][0] = 1;
    value.knots[value.knotCount - 1][1] = 0;
    for (int i = 1; i < value.knotCount; ++i)
    {
        if (value.knots[i][0] <= value.knots[i - 1][0])
        {
            return Invalid(error, errorSize, "Sound curve knot distances must increase");
        }
    }
    SndCurve *result = (SndCurve *)malloc(sizeof(SndCurve) + strlen(name) + 1);
    if (!result)
    {
        return Invalid(error, errorSize, "Out of memory importing sound curve");
    }
    *result = value;
    char *ownedName = (char *)(result + 1);
    strcpy(ownedName, name);
    result->filename = ownedName;
    *curve = result;
    return true;
}
void Linker_FreeSound(LoadedSound *sound)
{
    free(sound);
}

bool Linker_ImportPcmWave(const void *data, size_t size, const char *name, LoadedSound **sound, char *error,
                          size_t errorSize)
{
    if (!sound || (!error && errorSize))
    {
        return false;
    }
    *sound = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    const uint8_t *bytes = (const uint8_t *)data;
    if (!bytes || !name || !name[0] || size < 12 || size > INT_MAX || memcmp(bytes, "RIFF", 4) ||
        memcmp(bytes + 8, "WAVE", 4) || Read32(bytes + 4) != size - 8)
    {
        return Invalid(error, errorSize, "Invalid RIFF WAVE header or length");
    }
    const uint8_t *format = NULL;
    const uint8_t *samples = NULL;
    unsigned int sampleBytes = 0;
    size_t offset = 12;
    while (offset < size)
    {
        if (size - offset < 8)
        {
            return Invalid(error, errorSize, "Truncated WAVE chunk header");
        }
        const unsigned int length = Read32(bytes + offset + 4);
        const uint8_t *chunk = bytes + offset;
        offset += 8;
        if (length > size - offset || (size_t)length + (length & 1) > size - offset)
        {
            return Invalid(error, errorSize, "Truncated WAVE chunk data");
        }
        if (!memcmp(chunk, "fmt ", 4))
        {
            if (format || length < 16)
            {
                return Invalid(error, errorSize, "Invalid or duplicate WAVE format chunk");
            }
            format = bytes + offset;
        }
        else if (!memcmp(chunk, "data", 4))
        {
            if (samples)
            {
                return Invalid(error, errorSize, "Duplicate WAVE data chunk");
            }
            samples = bytes + offset;
            sampleBytes = length;
        }
        offset += length + (length & 1);
    }
    if (!format || !samples || !sampleBytes || Read16(format) != 1)
    {
        return Invalid(error, errorSize, "WAVE importer requires PCM audio with format and data chunks");
    }
    const unsigned int channels = Read16(format + 2);
    const unsigned int rate = Read32(format + 4);
    const unsigned int bits = Read16(format + 14);
    if ((channels != 1 && channels != 2) || (bits != 8 && bits != 16) || !rate ||
        Read16(format + 12) != channels * (bits / 8) || sampleBytes % (channels * (bits / 8)) ||
        (uint64_t)rate * channels * (bits / 8) != Read32(format + 8))
    {
        return Invalid(error, errorSize, "Invalid PCM sample format or block alignment");
    }
    const size_t nameSize = strlen(name) + 1;
    if (nameSize > SIZE_MAX - sizeof(LoadedSound) - sampleBytes)
    {
        return Invalid(error, errorSize, "Sound allocation overflow");
    }
    LoadedSound *output = (LoadedSound *)calloc(1, sizeof(LoadedSound) + nameSize + sampleBytes);
    if (!output)
    {
        return Invalid(error, errorSize, "Out of memory importing WAVE");
    }
    char *ownedName = (char *)(output + 1);
    memcpy(ownedName, name, nameSize);
    output->name = ownedName;
    output->sound.data = (uint8_t *)ownedName + nameSize;
    memcpy(output->sound.data, samples, sampleBytes);
    output->sound.info.format = 1;
    output->sound.info.data_len = sampleBytes;
    output->sound.info.rate = rate;
    output->sound.info.bits = bits;
    output->sound.info.channels = channels;
    output->sound.info.samples = sampleBytes / (channels * (bits / 8));
    output->sound.info.block_size = channels * (bits / 8);
    output->sound.info.data_ptr = output->sound.data;
    output->sound.info.initial_ptr = output->sound.data;
    *sound = output;
    return true;
}
