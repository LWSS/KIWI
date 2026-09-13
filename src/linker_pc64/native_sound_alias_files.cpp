#include <universal/q_shared.h>
#include <universal/com_sndalias.h>
#include "native_sound_alias_files.h"
#include "native_sound_alias_source.h"
#include "native_sound_alias.h"
#include "native_sound.h"
#include "native_asset_files.h"
#include <database64/db_package.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

struct AliasResources
{
    SoundFile file;
    LoadedSound *loaded;
    SndCurve *curve;
    SpeakerMap *speakers;
    char streamPath[DB64_PACKAGE_PATH];
};
struct LinkerCompiledSoundAlias
{
    snd_alias_list_t alias;
    LinkerSoundAliasSource *source;
    AliasResources *resources;
    SndCurve defaultCurve;
    SpeakerMap defaultSpeakers;
};
struct AliasSortEntry
{
    snd_alias_t alias;
    int sequence;
    unsigned int index;
};
static int CompareAliases(const void *left, const void *right)
{
    const AliasSortEntry *a = (const AliasSortEntry *)left;
    const AliasSortEntry *b = (const AliasSortEntry *)right;
    if (a->sequence != b->sequence)
    {
        return a->sequence < b->sequence ? -1 : 1;
    }
    return (a->index > b->index) - (a->index < b->index);
}
void Linker_FreeSoundAlias(LinkerCompiledSoundAlias *compiled)
{
    if (!compiled)
    {
        return;
    }
    if (compiled->resources)
    {
        for (int i = 0; i < compiled->alias.count; ++i)
        {
            Linker_FreeSound(compiled->resources[i].loaded);
            free(compiled->resources[i].curve);
            free(compiled->resources[i].speakers);
        }
    }
    free(compiled->resources);
    free(compiled->alias.head);
    Linker_FreeSoundAliasSource(compiled->source);
    free(compiled);
}
snd_alias_list_t *Linker_GetSoundAlias(LinkerCompiledSoundAlias *compiled)
{
    return compiled ? &compiled->alias : NULL;
}
static bool ReadDependency(const char *root, const char *directory, const char *name, const char *extension,
                            void **bytes, size_t *size, size_t limit, const char *language = NULL)
{
    char path[DB64_PACKAGE_PATH], normalized[DB64_PACKAGE_PATH];
    const int count = snprintf(path, sizeof(path), "%s%s%s", directory, name, extension);
    return count > 0 && count < sizeof(path) && DB64_NormalizePath(path, normalized, sizeof(normalized)) &&
           Linker_ReadLocalizedAssetFile(root, normalized, language, bytes, size, limit);
}

struct AliasTables
{
    char *paths[4096];
    unsigned int count;
};
static bool CollectAliasTable(const char *path, void *context)
{
    AliasTables *tables = (AliasTables *)context;
    for (unsigned int i = 0; i < tables->count; ++i)
    {
        if (!_stricmp(tables->paths[i], path))
        {
            return true;
        }
    }
    if (tables->count == ARRAY_COUNT(tables->paths))
    {
        return false;
    }
    char *copy = _strdup(path);
    if (!copy)
    {
        return false;
    }
    tables->paths[tables->count++] = copy;
    return true;
}
static int CompareAliasTables(const void *left, const void *right)
{
    return _stricmp(*(const char *const *)left, *(const char *const *)right);
}
struct CachedSoundTable
{
    char *path;
    LinkerSoundAliasSource *source;
    size_t size;
};
static thread_local CachedSoundTable s_soundTables[4096];
static thread_local unsigned int s_soundTableCount;
static thread_local size_t s_soundTableBytes;
static thread_local char *s_soundTableRoot;

void Linker_EndSoundTableCache()
{
    for (unsigned int i = 0; i < s_soundTableCount; ++i)
    {
        free(s_soundTables[i].path);
        Linker_FreeSoundAliasSource(s_soundTables[i].source);
    }
    s_soundTableCount = 0;
    s_soundTableBytes = 0;
    free(s_soundTableRoot);
    s_soundTableRoot = NULL;
}

void Linker_BeginSoundTableCache(const char *root)
{
    Linker_EndSoundTableCache();
    s_soundTableRoot = root ? _strdup(root) : NULL;
}

static bool ReadAliasTables(const char *root, const char *csvPath, const char *aliasName,
                            LinkerSoundAliasSource **result, char *error, size_t errorSize)
{
    AliasTables tables = {};
    bool valid = csvPath ? CollectAliasTable(csvPath, &tables) :
        Linker_EnumerateRawAssetFiles(root, "soundaliases", ".csv", CollectAliasTable, &tables);
    qsort(tables.paths, tables.count, sizeof(char *), CompareAliasTables);
    LinkerSoundAliasSource *sources[4096] = {};
    bool borrowed[4096] = {};
    size_t textSize = 0, inputSize = 0;
    unsigned int rowCount = 0;
    for (unsigned int i = 0; valid && i < tables.count; ++i)
    {
        void *bytes = NULL;
        size_t size = 0;
        const bool cache = s_soundTableRoot && !strcmp(root, s_soundTableRoot);
        for (unsigned int entry = 0; cache && entry < s_soundTableCount; ++entry)
        {
            if (!_stricmp(tables.paths[i], s_soundTables[entry].path))
            {
                sources[i] = s_soundTables[entry].source;
                size = s_soundTables[entry].size;
                borrowed[i] = true;
                break;
            }
        }
        valid = borrowed[i] || ReadDependency(root, "", tables.paths[i], "", &bytes, &size, 16 * 1024 * 1024);
        inputSize += size;
        valid = valid && inputSize <= 64 * 1024 * 1024 &&
            (borrowed[i] || Linker_ReadSoundAliasSource(bytes, size, &sources[i], error, errorSize));
        free(bytes);
        if (valid && cache && !borrowed[i] && s_soundTableCount < ARRAY_COUNT(s_soundTables) &&
            size <= 64 * 1024 * 1024 - s_soundTableBytes)
        {
            char *path = _strdup(tables.paths[i]);
            if (path)
            {
                s_soundTables[s_soundTableCount++] = {path, sources[i], size};
                s_soundTableBytes += size;
                borrowed[i] = true;
            }
        }
        if (!valid)
        {
            break;
        }
        for (unsigned int j = 0; j < sources[i]->count; ++j)
        {
            const LinkerSoundAliasRow *row = &sources[i]->rows[j];
            if (_stricmp(row->fields[SA_NAME], aliasName))
            {
                continue;
            }
            ++rowCount;
            for (unsigned int field = 0; field < ARRAY_COUNT(row->fields); ++field)
            {
                textSize += strlen(row->fields[field]) + 1;
            }
        }
        valid = rowCount <= 65536 && textSize <= 64 * 1024 * 1024;
    }
    LinkerSoundAliasSource *merged = NULL;
    if (valid)
    {
        merged = (LinkerSoundAliasSource *)calloc(1, sizeof(LinkerSoundAliasSource));
        valid = merged != NULL;
    }
    if (valid)
    {
        merged->text = (char *)malloc(textSize ? textSize : 1);
        merged->rows = (LinkerSoundAliasRow *)calloc(rowCount ? rowCount : 1, sizeof(LinkerSoundAliasRow));
        valid = merged->text && merged->rows;
    }
    char *cursor = valid ? merged->text : NULL;
    for (unsigned int i = 0; i < tables.count; ++i)
    {
        if (valid)
        {
            for (unsigned int j = 0; j < sources[i]->count; ++j)
            {
                const LinkerSoundAliasRow *row = &sources[i]->rows[j];
                if (_stricmp(row->fields[SA_NAME], aliasName))
                {
                    continue;
                }
                LinkerSoundAliasRow *copy = &merged->rows[merged->count++];
                copy->line = row->line;
                for (unsigned int field = 0; field < ARRAY_COUNT(row->fields); ++field)
                {
                    const size_t length = strlen(row->fields[field]) + 1;
                    memcpy(cursor, row->fields[field], length);
                    copy->fields[field] = cursor;
                    cursor += length;
                }
            }
        }
        if (!borrowed[i])
        {
            Linker_FreeSoundAliasSource(sources[i]);
        }
        free(tables.paths[i]);
    }
    if (!valid)
    {
        Linker_FreeSoundAliasSource(merged);
        return false;
    }
    *result = merged;
    return true;
}

bool Linker_ProbeSoundAliasForZone(const char *root, const char *aliasName, const char *level,
                                 const char *game, bool *found, char *error, size_t errorSize)
{
    if (!found || !root || !aliasName || !level || !game || (!error && errorSize))
    {
        return false;
    }
    *found = false;
    if (errorSize)
    {
        error[0] = 0;
    }
    LinkerSoundAliasSource *source = NULL;
    bool ok = ReadAliasTables(root, NULL, aliasName, &source, error, errorSize);
    for (unsigned int i = 0; ok && i < source->count; ++i)
    {
        bool keep = false;
        ok = Linker_SoundAliasMatches(source->rows[i].fields[SA_LOADSPEC], level, game, &keep, error, errorSize);
        *found = *found || keep;
    }
    Linker_FreeSoundAliasSource(source);
    if (!ok && errorSize && !error[0])
    {
        snprintf(error, errorSize, "Cannot read sound alias tables while probing '%s'", aliasName);
    }
    return ok;
}

bool Linker_CompileSoundAliasForZone(const char *root, const char *csvPath, const char *aliasName,
                                      const char *level, const char *game, LinkerCompiledSoundAlias **result,
                                      char *error, size_t errorSize, const char *language)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    if (!root || !aliasName || !aliasName[0] || !level || !game)
    {
        return false;
    }
    LinkerCompiledSoundAlias *compiled = (LinkerCompiledSoundAlias *)calloc(1, sizeof(LinkerCompiledSoundAlias));
    if (!compiled)
    {
        return false;
    }
    void *bytes = NULL, *channels = NULL, *volumes = NULL;
    size_t size = 0, channelSize = 0, volumeSize = 0;
    bool valid = ReadAliasTables(root, csvPath, aliasName, &compiled->source, error, errorSize) &&
                 ReadDependency(root, "soundaliases/", "channels", ".def", &channels, &channelSize, 16384) &&
                 ReadDependency(root, "soundaliases/", "volumemodgroups", ".def", &volumes, &volumeSize, 8192);
    LinkerSoundAliasConfig config;
    valid = valid && Linker_ReadSoundAliasConfig(channels, channelSize, volumes, volumeSize, &config, error, errorSize);
    free(bytes);
    free(channels);
    free(volumes);
    if (valid)
    {
        unsigned int matchingRows = 0;
        for (unsigned int i = 0; i < compiled->source->count; ++i)
        {
            if (!_stricmp(compiled->source->rows[i].fields[SA_NAME], aliasName))
            {
                ++matchingRows;
                bool keep;
                valid = Linker_SoundAliasMatches(compiled->source->rows[i].fields[SA_LOADSPEC], level, game,
                                                 &keep, error, errorSize);
                if (!valid)
                {
                    break;
                }
                if (keep)
                {
                    ++compiled->alias.count;
                }
            }
        }
        if (valid && !compiled->alias.count && errorSize)
        {
            if (matchingRows)
            {
                snprintf(error, errorSize, "Sound alias '%s' has no variants for level '%s', game '%s'",
                         aliasName, level, game);
            }
            else
            {
                snprintf(error, errorSize, "Sound alias '%s' was not found in %s", aliasName,
                         csvPath ? csvPath : "soundaliases/*.csv");
            }
        }
        valid = valid && compiled->alias.count > 0 && compiled->alias.count <= 4096;
    }
    AliasSortEntry *sorted = NULL;
    if (valid)
    {
        compiled->resources = (AliasResources *)calloc(compiled->alias.count, sizeof(AliasResources));
        compiled->alias.head = (snd_alias_t *)calloc(compiled->alias.count, sizeof(snd_alias_t));
        sorted = (AliasSortEntry *)calloc(compiled->alias.count, sizeof(AliasSortEntry));
        valid = compiled->resources && compiled->alias.head && sorted;
    }
    compiled->defaultCurve.filename = "";
    compiled->defaultCurve.knotCount = 2;
    compiled->defaultCurve.knots[0][1] = compiled->defaultCurve.knots[1][0] = 1;
    Linker_DefaultSpeakerMap(&compiled->defaultSpeakers);
    int index = 0;
    for (unsigned int i = 0; valid && i < compiled->source->count; ++i)
    {
        const LinkerSoundAliasRow *row = &compiled->source->rows[i];
        const char *const *f = row->fields;
        if (_stricmp(f[SA_NAME], aliasName))
        {
            continue;
        }
        bool keep;
        valid = Linker_SoundAliasMatches(f[SA_LOADSPEC], level, game, &keep, error, errorSize);
        if (!valid || !keep)
        {
            continue;
        }
        AliasResources *resources = &compiled->resources[index];
        const bool streamed = !_stricmp(f[SA_TYPE], "streamed");
        if (f[SA_TYPE][0] && _stricmp(f[SA_TYPE], "loaded") && !streamed)
        {
            if (errorSize)
            {
                snprintf(error, errorSize, "Unknown sound type '%s' for alias '%s'", f[SA_TYPE], aliasName);
            }
            valid = false;
            break;
        }
        bytes = NULL;
        valid = ReadDependency(root, "sound/", f[SA_FILE], "", &bytes, &size, INT_MAX, language);
        if (!valid && errorSize)
        {
            snprintf(error, errorSize, "Cannot read sound/%s for alias '%s' (source row %u)",
                     f[SA_FILE], aliasName, row->line);
        }
        if (valid && streamed)
        {
            valid = size > 0 && DB64_NormalizePath(f[SA_FILE], resources->streamPath, sizeof(resources->streamPath));
            char *separator = strrchr(resources->streamPath, '/');
            if (separator)
            {
                *separator = 0;
                resources->file.u.streamSnd.filename.info.raw.dir = resources->streamPath;
                resources->file.u.streamSnd.filename.info.raw.name = separator + 1;
            }
            else
            {
                resources->file.u.streamSnd.filename.info.raw.dir = ".";
                resources->file.u.streamSnd.filename.info.raw.name = resources->streamPath;
            }
        }
        else if (valid)
        {
            valid = Linker_ImportPcmWave(bytes, size, f[SA_FILE], &resources->loaded, error, errorSize);
        }
        free(bytes);
        resources->file.type = streamed ? SAT_STREAMED : SAT_LOADED;
        resources->file.exists = 1;
        if (!streamed)
        {
            resources->file.u.loadSnd = resources->loaded;
        }
        SndCurve *curve = &compiled->defaultCurve;
        SpeakerMap *speakers = &compiled->defaultSpeakers;
        if (valid && f[SA_VOLUMEFALLOFFCURVE][0])
        {
            bytes = NULL;
            valid = ReadDependency(root, "soundaliases/", f[SA_VOLUMEFALLOFFCURVE], ".vfcurve", &bytes, &size, 8192) &&
                    Linker_ImportSoundCurve(bytes, size, f[SA_VOLUMEFALLOFFCURVE], &resources->curve, error, errorSize);
            free(bytes);
            curve = resources->curve;
        }
        if (valid && f[SA_SPEAKERMAP][0] && _stricmp(f[SA_SPEAKERMAP], "default"))
        {
            bytes = NULL;
            valid = ReadDependency(root, "soundaliases/", f[SA_SPEAKERMAP], ".spkrmap", &bytes, &size, 8192) &&
                    Linker_ReadSpeakerMap(bytes, size, f[SA_SPEAKERMAP], &resources->speakers, error, errorSize);
            free(bytes);
            speakers = resources->speakers;
        }
        const int channel = Linker_FindSoundChannel(&config, f[SA_CHANNEL]);
        float volume = 1;
        valid = valid && channel >= 0 && Linker_FindSoundVolume(&config, f[SA_VOL_MOD], &volume) &&
                Linker_BuildSoundAlias(row, channel, volume, &resources->file, curve, speakers, &sorted[index].alias,
                                       &sorted[index].sequence, error, errorSize);
        sorted[index].index = i;
        ++index;
    }
    if (valid)
    {
        qsort(sorted, compiled->alias.count, sizeof(AliasSortEntry), CompareAliases);
        for (int i = 0; i < compiled->alias.count; ++i)
        {
            compiled->alias.head[i] = sorted[i].alias;
        }
        compiled->alias.aliasName = compiled->alias.head[0].aliasName;
        *result = compiled;
    }
    else
    {
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Cannot compile sound alias '%s' from '%s'", aliasName,
                     csvPath ? csvPath : "soundaliases/*.csv");
        }
        Linker_FreeSoundAlias(compiled);
    }
    free(sorted);
    return valid;
}

bool Linker_CompileSoundAliasFile(const char *root, const char *csvPath, const char *aliasName,
                                   LinkerCompiledSoundAlias **result, char *error, size_t errorSize)
{
    return Linker_CompileSoundAliasForZone(root, csvPath, aliasName, "", "all_mp", result, error, errorSize);
}

bool Linker_VisitSoundTable(const char *root, const char *path, const char *level, const char *game,
                           bool (*visit)(const char *, void *), void *context, bool *found,
                           char *error, size_t errorSize)
{
    if (!root || !path || !level || !game || !visit || !found || (!error && errorSize))
    {
        return false;
    }
    void *bytes = NULL;
    size_t size = 0;
    if (!Linker_ReadRawAssetFile(root, path, &bytes, &size, 16 * 1024 * 1024, found))
    {
        return !*found;
    }
    LinkerSoundAliasSource *source = NULL;
    bool valid = Linker_ReadSoundAliasSource(bytes, size, &source, error, errorSize);
    free(bytes);
    for (unsigned int i = 0; valid && i < source->count; ++i)
    {
        bool keep = false;
        valid = Linker_SoundAliasMatches(source->rows[i].fields[SA_LOADSPEC], level, game, &keep, error, errorSize);
        if (valid && keep)
        {
            valid = visit(source->rows[i].fields[SA_NAME], context);
        }
    }
    Linker_FreeSoundAliasSource(source);
    return valid;
}
