#pragma once
#include <stddef.h>
struct LinkerCompiledSoundAlias;
struct snd_alias_list_t;
// Scoped to one serial zone import; source edits are reread on the next build.
void Linker_BeginSoundTableCache(const char *root);
void Linker_EndSoundTableCache();
// Distinguishes an absent/excluded optional alias from unreadable source tables.
bool Linker_ProbeSoundAliasForZone(const char *root, const char *aliasName, const char *level,
                                 const char *game, bool *found, char *error, size_t errorSize);
// A NULL csvPath searches and merges all soundaliases/*.csv tables in sorted path order.
bool Linker_CompileSoundAliasFile(const char *root, const char *csvPath, const char *aliasName,
                                   LinkerCompiledSoundAlias **result, char *error, size_t errorSize);
bool Linker_CompileSoundAliasForZone(const char *root, const char *csvPath, const char *aliasName,
                                      const char *level, const char *game, LinkerCompiledSoundAlias **result,
                                      char *error, size_t errorSize, const char *language = NULL);
snd_alias_list_t *Linker_GetSoundAlias(LinkerCompiledSoundAlias *compiled);
void Linker_FreeSoundAlias(LinkerCompiledSoundAlias *compiled);

// Visits matching alias names from one table; absent tables set found to false.
bool Linker_VisitSoundTable(const char *root, const char *path, const char *level, const char *game,
                           bool (*visit)(const char *, void *), void *context, bool *found,
                           char *error, size_t errorSize);
