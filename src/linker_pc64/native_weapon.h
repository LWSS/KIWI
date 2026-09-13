#pragma once
#include <universal/q_shared.h>
#include <xanim/xanim.h>
#include <stddef.h>

struct LinkerWeaponSource;
// Returned assets are borrowed. Optional sound lookups support surface-sound fallback.
typedef const void *(*LinkerWeaponAsset)(XAssetType type, const char *name, bool required, void *context);
typedef uint16_t (*LinkerWeaponString)(const char *name, void *context);
bool Linker_CompileWeaponFile(const char *root, const char *name, LinkerWeaponAsset resolve,
                             LinkerWeaponString intern, void *context, LinkerWeaponSource **result,
                             char *error, size_t errorSize);
WeaponDef *Linker_GetWeapon(LinkerWeaponSource *source);
void Linker_FreeWeapon(LinkerWeaponSource *source);
