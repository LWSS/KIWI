/*
 * xmodel_utils.c — XModel utility functions.
 *
 * Source: ..\src\xanim\xmodel_utils.cpp
 * Reconstructed from decompiled output, LST, and CoD2 server source.
 *
 * Note: XModelNumBones, XModelGetSurfCount, XModelGetBasePose
 * are inlined in the x64 build (no standalone functions in binary).
 *
 * XModelSurfs layout (x64, from LST):
 *   +0x00  XSurface_t *surfs      (pointer to surface array)
 *   +0x08  int partBits[4]      (bone bits for surfaces, 16 bytes)
 */

#include "cod2rad64.h"

static char s_assertDisable_XModelGetSurfaces;
static char s_assertDisable_XModelGetSurfaces_surf;
static char s_assertDisable_XModelGetSurfaces_lod;
static char s_assertDisable_XModelGetSurfaces_lodSurfs;

/*
================
XModelLoad

Loads an XModel from disk: calls XModelLoadFile to parse
the binary format, then XModelSurfsLoad to load surfaces.
If surface loading fails, frees the model and returns NULL.
================
*/
XModel_t *XModelLoad(const char *name, void *(*alloc)(int), void *(*allocColl)(int))
{
    XModel_t *model;

    model = XModelLoadFile(name, alloc, allocColl);
    if (!model)
        return NULL;

    if (XModelSurfsLoad(model, alloc))
        return model;

    XModelFree(model);
    return NULL;
}

/*
================
XModelGetName

Returns the model's name string (at XModel offset +0xF0).
================
*/
const char *XModelGetName(const XModel_t *model)
{
    return model->name;
}

/*
================
XModelGetSurfaces

Gets the surface pointer, partBits pointer, and surface count
for a given LOD level.

Access pattern (from LST):
  modelSurfs = lodInfo[lod].modelSurfs  (ptr at model + lod*40 + 40)
  *surfaces = modelSurfs->surfs         (ptr at modelSurfs + 0)
  *partBits = modelSurfs->partBits      (ptr at modelSurfs + 8)
  return lodInfo[lod].numsurfs          (short at model + lod*40 + 24)
================
*/
int XModelGetSurfaces(const XModel_t *model, XSurface_t **surfaces,
                      int lod, int **partBits)
{
    XModelSurfs_t *modelSurfs;

    Assert(model, s_assertDisable_XModelGetSurfaces);
    Assert(surfaces, s_assertDisable_XModelGetSurfaces_surf);
    Assert(lod >= 0, s_assertDisable_XModelGetSurfaces_lod);

    modelSurfs = model->lodInfo[lod].modelSurfs;
    Assert(modelSurfs, s_assertDisable_XModelGetSurfaces_lodSurfs);

    *surfaces = modelSurfs->surfs;
    *partBits = modelSurfs->partBits;

    return model->lodInfo[lod].numsurfs;
}
