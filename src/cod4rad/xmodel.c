/*
 * xmodel.c — XModel system (loading, caching, queries).
 *
 * Source: ..\src\xanim\xmodel.cpp
 * Reconstructed from decompiled output, LST, and CoD2 server source
 * (CoD2rev_Server-master) for naming conventions.
 *
 * See cod2rad64.h for XModel struct layout (all offsets verified from LST).
 *
 * Hunk data types used:
 *   2 = FILEDATA_XMODELSURFS
 *   3 = FILEDATA_XMODELPARTS
 *   4 = FILEDATA_XMODEL (in x64 cod2rad)
 */

#include "cod2rad64.h"

static char s_assertDisable_XModelBad;
static char s_assertDisable_XModelFree;
static char s_assertDisable_XModelGetBoneIndex;


/*
================
XModelBad

Returns the 'bad' flag from the model.
================
*/
int XModelBad(const XModel_t *model)
{
    Assert(model, s_assertDisable_XModelBad);
    return model->bad;
}

/*
================
XModelFree

Frees surface name string references for all 4 LOD levels.
Only operates if the model is not marked bad.
================
*/
void XModelFree(XModel_t *model)
{
    int i;
    int j;

    Assert(model, s_assertDisable_XModelFree);

    if (!XModelBad(model))
    {
        for (i = 0; i < 4; i++)
        {
            if (model->lodInfo[i].surfNames)
            {
                for (j = 0; j < model->lodInfo[i].numsurfs; j++)
                    SL_RemoveRefToString(model->lodInfo[i].surfNames[j]);

                model->lodInfo[i].surfNames = NULL;
            }
        }
    }
}

/*
================
XModelSurfsFindData

Looks up cached XModelSurfs by filename.
Uses hunk data type 2 (FILEDATA_XMODELSURFS).
================
*/
XModelSurfs_t *XModelSurfsFindData(const char *name)
{
    return (XModelSurfs_t *)Hunk_FindDataForFile(FILEDATA_XMODELSURFS, name);
}

/*
================
XModelSurfsSetData

Stores XModelSurfs data in the hunk cache.
Uses hunk data type 2 (FILEDATA_XMODELSURFS).
================
*/
void XModelSurfsSetData(const char *name, XModelSurfs_t *surfs, void *(*alloc)(int))
{
    Hunk_AddDataForFile(FILEDATA_XMODELSURFS, name, surfs, alloc);
}

/*
================
XModelPartsFindData

Looks up cached XModelParts by filename.
Uses hunk data type 3 (FILEDATA_XMODELPARTS).
================
*/
XModelParts_t *XModelPartsFindData(const char *name)
{
    return (XModelParts_t *)Hunk_FindDataForFile(FILEDATA_XMODELPARTS, name);
}

/*
================
XModelPartsSetData

Stores XModelParts data in the hunk cache.
Uses hunk data type 3 (FILEDATA_XMODELPARTS).
================
*/
void XModelPartsSetData(const char *name, XModelParts_t *parts, void *(*alloc)(int))
{
    Hunk_AddDataForFile(FILEDATA_XMODELPARTS, name, parts, alloc);
}

/*
================
XModelPrecache

Finds a cached XModel or loads it from disk.
Uses hunk data type 4.
After loading, stores the model name pointer at model->name.
================
*/
XModel_t *XModelPrecache(const char *name, void *(*alloc)(int), void *(*allocColl)(int))
{
    XModel_t *model;

    model = (XModel_t *)Hunk_FindDataForFile(FILEDATA_XMODEL, name);
    if (model)
        return model;

    model = XModelLoad(name, alloc, allocColl);
    if (model)
    {
        model->name = (const char *)Hunk_AddDataForFile(FILEDATA_XMODEL, name, model, alloc);
        return model;
    }

    return NULL;
}

/*
================
XModelGetBoneIndex

Searches the model's bone name array for a matching name.
Returns the bone index (0-based) or -1 if not found.
Searches backwards from numBones-1 to 0.

Note: This is the simple 2-param version from xmodel.cpp.
The xmodel_utils.cpp version has 4 params with offset.
================
*/
int XModelGetBoneIndex(const XModel_t *model, unsigned int name)
{
    XModelParts_t *parts;
    unsigned short *names;
    int numBones;
    int i;

    parts = model->parts;
    names = parts->hierarchy->names;
    numBones = parts->numBones;

    Assert(numBones < DOBJ_MAX_PARTS,
           s_assertDisable_XModelGetBoneIndex);

    for (i = numBones - 1; i >= 0; i--)
    {
        if (names[i] == name)
            return i;
    }

    return -1;
}

/*
================
XModelGetBounds

Copies model min/max bounds.
================
*/
void XModelGetBounds(const XModel_t *model, float *mins, float *maxs)
{
    mins[0] = model->mins[0];
    mins[1] = model->mins[1];
    mins[2] = model->mins[2];
    maxs[0] = model->maxs[0];
    maxs[1] = model->maxs[1];
    maxs[2] = model->maxs[2];
}

/*
================
XModel_AllocZeroed

Allocates memory and zeros it. Returns the pointer.
================
*/
void *XModel_AllocZeroed(int size)
{
    void *buf;

    buf = Z_Malloc(size);
    memset(buf, 0, size);
    return buf;
}
