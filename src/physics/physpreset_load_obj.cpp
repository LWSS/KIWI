#include <universal/q_shared.h>
#include "phys_local.h"
#include <universal/com_memory.h>
#include <qcommon/qcommon.h>
#include <universal/com_files.h>

void *(__cdecl *physAlloc)(int);

struct PhysPresetLite
{
    float mass;   // 0
    float bounce; // 4
    float friction; // 8
    int isFrictionInfinity; // 12
    float bulletForceScale; // 16
    float explosiveForceScale; // 20
    const char *sndAliasPrefix; // 24
    float piecesSpreadFraction; // 28
    float piecesUpwardVelocity; // 32
    int tempDefaultToCylinder;
};

cspField_t physPresetFields[10] =
{
    { "mass", offsetof(PhysPresetLite, mass), CSPFT_FLOAT },
    { "bounce", offsetof(PhysPresetLite, bounce), CSPFT_FLOAT },
    { "friction", offsetof(PhysPresetLite, friction), CSPFT_FLOAT },
    { "isFrictionInfinity", offsetof(PhysPresetLite, isFrictionInfinity), CSPFT_QBOOLEAN },
    { "bulletForceScale", offsetof(PhysPresetLite, bulletForceScale), CSPFT_FLOAT },
    { "explosiveForceScale", offsetof(PhysPresetLite, explosiveForceScale), CSPFT_FLOAT },
    { "sndAliasPrefix", offsetof(PhysPresetLite, sndAliasPrefix), CSPFT_STRING },
    { "piecesSpreadFraction", offsetof(PhysPresetLite, piecesSpreadFraction), CSPFT_FLOAT },
    { "piecesUpwardVelocity", offsetof(PhysPresetLite, piecesUpwardVelocity), CSPFT_FLOAT },
    { "tempDefaultToCylinder", offsetof(PhysPresetLite, tempDefaultToCylinder), CSPFT_QBOOLEAN }
};

void __cdecl PhysPreset_Strcpy(uint8_t *member, const char *keyValue)
{
    char v2; // [esp+3h] [ebp-25h]
    char *v3; // [esp+8h] [ebp-20h]
    const char *v4; // [esp+Ch] [ebp-1Ch]
    char *buf; // [esp+20h] [ebp-8h]

    if (*keyValue)
    {
        buf = (char *)physAlloc(strlen(keyValue) + 1);
        v4 = keyValue;
        v3 = buf;
        do
        {
            v2 = *v4;
            *v3++ = *v4++;
        } while (v2);
        *(const char **)member = buf;
    }
    else
    {
        *(const char **)member = "";
    }
}

PhysPreset *__cdecl PhysPresetLoadFile(const char *name, void *(__cdecl *Alloc)(int))
{
    char dest[64]; // [esp+24h] [ebp-2080h] BYREF
    char buffer[8192]; // [esp+64h] [ebp-2040h] BYREF
    PhysPresetLite pStruct;
    PhysPreset *physPreset; // [esp+2090h] [ebp-14h]
    char *last; // [esp+2094h] [ebp-10h]
    signed int filelen; // [esp+2098h] [ebp-Ch]
    int f; // [esp+209Ch] [ebp-8h] BYREF
    int len; // [esp+20A0h] [ebp-4h]

    last = (char*)"PHYSIC";
    len = strlen("PHYSIC");
    if (!strlen(name))
        return 0;
    if (Com_sprintf(dest, 0x40u, "physic/%s", name) >= 0)
    {
        filelen = FS_FOpenFileByMode(dest, &f, FS_READ);
        if (filelen >= 0)
        {
            FS_Read((uint8_t *)buffer, len, f);
            buffer[len] = 0;
            if (!strncmp(buffer, last, len))
            {
                if (filelen - len < 0x2000)
                {
                    FS_Read((uint8_t *)buffer, filelen - len, f);
                    buffer[filelen - len] = 0;
                    FS_FCloseFile(f);
                    if (Info_Validate(buffer))
                    {
                        memset(&pStruct, 0, sizeof(pStruct));
                        pStruct.sndAliasPrefix = "";
                        physAlloc = Alloc;
                        if (ParseConfigStringToStruct((byte*)&pStruct, physPresetFields, 10, buffer, 0, 0, PhysPreset_Strcpy))
                        {
                            iassert(sizeof(PhysPreset) == (sizeof(void *) == 8 ? 56 : 44));

                            physPreset = (PhysPreset *)Alloc(sizeof(PhysPreset));

                            iassert(physPreset);

                            physPreset->mass = pStruct.mass;
                            physPreset->bounce = pStruct.bounce;

                            if (pStruct.isFrictionInfinity)
                                physPreset->friction = FLT_MAX;
                            else
                                physPreset->friction = pStruct.friction;

                            physPreset->bulletForceScale = pStruct.bulletForceScale;
                            physPreset->explosiveForceScale = pStruct.explosiveForceScale;
                            physPreset->sndAliasPrefix = pStruct.sndAliasPrefix;
                            physPreset->piecesSpreadFraction = pStruct.piecesSpreadFraction;
                            physPreset->piecesUpwardVelocity = pStruct.piecesUpwardVelocity;
                            physPreset->tempDefaultToCylinder = pStruct.tempDefaultToCylinder;
                            return physPreset;
                        }
                        else
                        {
                            return 0;
                        }
                    }
                    else
                    {
                        Com_PrintError(CON_CHANNEL_PHYS, "ERROR: physics preset file [%s] is not valid\n", name);
                        return 0;
                    }
                }
                else
                {
                    Com_PrintError(CON_CHANNEL_PHYS, "ERROR: physics preset file [%s] is to big\n", name);
                    FS_FCloseFile(f);
                    return 0;
                }
            }
            else
            {
                Com_PrintError(CON_CHANNEL_PHYS, "ERROR: file [%s] is not a physics preset file\n", name);
                FS_FCloseFile(f);
                return 0;
            }
        }
        else
        {
            Com_PrintError(CON_CHANNEL_PHYS, "ERROR: physics preset '%s' not found\n", name);
            return 0;
        }
    }
    else
    {
        Com_PrintError(CON_CHANNEL_PHYS, "ERROR: filename '%s' too long\n", dest);
        return 0;
    }
}

PhysPreset *__cdecl PhysPresetPrecache(const char *name, void *(__cdecl *Alloc)(int))
{
    PhysPreset *physPreset; // [esp+0h] [ebp-4h]
    PhysPreset *physPreseta; // [esp+0h] [ebp-4h]

    iassert(name);
    iassert(name[0]);
    physPreset = (PhysPreset *)Hunk_FindDataForFile(7, name);
    if (physPreset)
        return physPreset;
    physPreseta = PhysPresetLoadFile(name, Alloc);
    if (physPreseta)
    {
        physPreseta->name = Hunk_SetDataForFile(7, name, physPreseta, Alloc);
        return physPreseta;
    }
    else
    {
        Com_PrintError(CON_CHANNEL_PHYS, "ERROR: Cannot find physics preset '%s'.\n", name);
        return 0;
    }
}


