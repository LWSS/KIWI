/*
 * dobj.c — Dynamic object (skeletal model) system.
 *
 * Source: ..\src\xanim\dobj.cpp
 * IW/Treyarch skeletal animation system — no public reference code.
 * Reconstructed from decompiled output, LST, and CoD2 server source
 * (CoD2rev_Server-master) for naming conventions.
 *
 * See cod2rad64.h for all struct layouts (all offsets verified from LST).
 */

#include "cod2rad64.h"

/* XAnimTree_s — minimal definition for dobj.c usage */
typedef struct XAnimAnims_s
{
    void *pad00;                /* +0x00 */
    unsigned int size;          /* +0x08 */
} XAnimAnims_s;

struct XAnimTree_s {
    XAnimAnims_s   *anims;      /* [0]  anim asset */
    void           *_pad08;     /* [8]  never touched by cod2rad */
    unsigned short *infoArray;  /* [16] variable-length unsigned short array */
    void           *_pad18;     /* [24] never touched by cod2rad */
    unsigned short  entnum;     /* [32] entity index */
    unsigned short  _pad22;     /* [34] never touched by cod2rad */
    unsigned char   inlineData[1]; /* [36] inline data (variable length) */
};

unsigned int g_empty;
float        g_dobj_conflict_tolerance;

/* assert-disable flags */
static char s_assertDisable_DObjGetBoneIndex;
static char s_assertDisable_DObjGetBoneIndex_numBones;
static char s_assertDisable_DObjCreateDuplicateParts_dup;
static char s_assertDisable_DObjCreateDuplicateParts_numModels;
static char s_assertDisable_DObjCreateDuplicateParts_numBones;
static char s_assertDisable_DObjCreateDuplicateParts_numBonesMax;
static char s_assertDisable_DObjCreateDuplicateParts_bufsize;
static char s_assertDisable_DObjCreateDuplicateParts_parent;
static char s_assertDisable_DObjCreateDuplicateParts_idx256;
static char s_assertDisable_DObjCreateDuplicateParts_par256;
static char s_assertDisable_DObjCreateDuplicateParts_order;
static char s_assertDisable_DObjCreateDuplicateParts_dup0;
static char s_assertDisable_DObjCreateDuplicateParts_dup1;
static char s_assertDisable_DObjCreateDuplicateParts_final;
static char s_assertDisable_DObjCreateDuplicateParts_empty;
static char s_assertDisable_DObjSetBounds;
static char s_assertDisable_DObjCreate_models;
static char s_assertDisable_DObjCreate_numModels;
static char s_assertDisable_DObjCreate_submodels;
static char s_assertDisable_DObjCreate_obj;
static char s_assertDisable_DObjCreate_anims;
static char s_assertDisable_DObjCreate_numBones;
static char s_assertDisable_DObjCreate_numBonesMax;
static char s_assertDisable_DObjCreate_parentUndef;
static char s_assertDisable_DObjCreate_numModelsByte;
static char s_assertDisable_DObjCreate_numBonesByte;
static char s_assertDisable_DObjCreate_numModelsPos;
static char s_assertDisable_DObjCreateSkel;
static char s_assertDisable_DObjGetMatrices_count;
static char s_assertDisable_DObjGetMatrices_mat;
static char s_assertDisable_DObjGetSurface;
static char s_assertDisable_DObjGetSurfaceName_lod;
static char s_assertDisable_DObjGetSurfaceName_submat;
static char s_assertDisable_DObjGetBoneName;
static char s_assertDisable_DObjGetBoneName_index;
static char s_assertDisable_DObjGetBoneName_numBones;
static char s_assertDisable_DObjCalcSkel;
static char s_assertDisable_DObjCalcSkel_skel;
static char s_assertDisable_DObjCalcSkel_dup;
static char s_assertDisable_DObjCalcSkel_conflict;
static char s_assertDisable_DObjCalcSkel_notDup;
static char s_assertDisable_DObjCalcSkel_notIgnore;
static char s_assertDisable_DObjCalcSkel_animSet;
static char s_assertDisable_DObjCalcSkel_quatNaN;
static char s_assertDisable_DObjCalcSkel_transNaN;
static char s_assertDisable_DObjCalcSkel_matPos;
static char s_assertDisable_DObjCalcSkel_srcSkel;
static char s_assertDisable_DObjCalcSkel_srcAnim;
static char s_assertDisable_DObjCalcSkel_srcOrder;
static char s_assertDisable_DObjCalcSkel_ignAnim;
static char s_assertDisable_DObjCalcSkel_ignQuatNaN;
static char s_assertDisable_DObjCalcSkel_ignTransNaN;
static char s_assertDisable_DObjCalcSkel_parentLt;
static char s_assertDisable_DObjCalcSkel_notDup2;
static char s_assertDisable_DObjCalcSkel_parentLt2;
static char s_assertDisable_DObjCalcSkel_notIgnore2;
static char s_assertDisable_DObjCalcSkel_animSet2;
static char s_assertDisable_DObjCalcSkel_parentSkel;
static char s_assertDisable_DObjCalcSkel_parentAnim;
static char s_assertDisable_DObjCalcSkel_quatNaN2;
static char s_assertDisable_DObjCalcSkel_transNaN2;
static char s_assertDisable_DObjCalcSkel_notDup3;
static char s_assertDisable_DObjCalcSkel_notIgnore3;
static char s_assertDisable_DObjCalcSkel_animSet3;
static char s_assertDisable_DObjCalcSkel_hierSkel;
static char s_assertDisable_DObjCalcSkel_hierAnim;
static char s_assertDisable_DObjCalcSkel_quatNaN3;
static char s_assertDisable_DObjCalcSkel_transNaN3;
static char s_assertDisable_DObjCalcSkel_matPos2;
static char s_assertDisable_DObjCalcSkel_srcSkel2;
static char s_assertDisable_DObjCalcSkel_srcAnim2;
static char s_assertDisable_DObjCalcSkel_srcOrder2;
static char s_assertDisable_DObjCalcSkel_ignAnim2;
static char s_assertDisable_DObjCalcSkel_ignQuatNaN2;
static char s_assertDisable_DObjCalcSkel_ignTransNaN2;
static char s_assertDisable_DObjCalcAnim;
static char s_assertDisable_DObjCalcAnim_skel;
static char s_assertDisable_DObjGetSurfaces_bits;


/*
================
DObjGetBoneIndex

Searches all models in the DObj for a bone name.
Returns the global bone index, or -1 if not found.
================
*/
int DObjGetBoneIndex(const DObj_t *obj, unsigned int name)
{
    int numModels;
    XModel_t *model;
    int index;
    int offset;
    int i;

    Assert(obj, s_assertDisable_DObjGetBoneIndex);

    numModels = obj->numModels;
    index = 0;

    for (i = 0; i < numModels; i++)
    {
        model = obj->models[i];
        offset = XModelGetBoneIndex(model, name);
        if (offset >= 0)
            return index + offset;

        Assert(model->parts->numBones >= 0,
               s_assertDisable_DObjGetBoneIndex_numBones);

        index += model->parts->numBones;
    }

    return -1;
}

/*
================
DObjCreateDuplicateParts

Creates the duplicate parts list for multi-model DObjs.
Finds bones that appear in multiple models and records
the mapping from duplicate to original.
================
*/
void DObjCreateDuplicateParts(DObj_t *obj)
{
    char duplicatePartBits[0x444]; /* binary assert string: sizeof(duplicatePartBits) = 0x444 */
    int len;
    unsigned char *duplicateParts;
    unsigned short *names;
    int boneIter;
    int localBoneIndex;
    int boneIndex;
    XModel_t *model;
    int numBones;
    int boneCount;
    int index;
    int bRootMeld;

    Assert(!obj->duplicateParts,
           s_assertDisable_DObjCreateDuplicateParts_dup);
    Assert(obj->numModels > 0,
           s_assertDisable_DObjCreateDuplicateParts_numModels);
    Assert(obj->numBones <= DOBJ_MAX_PARTS,
           s_assertDisable_DObjCreateDuplicateParts_numBones);
    Assert((DOBJ_MAX_PART_BITS * sizeof(int) + obj->numBones * 2 + 17) <= 0x444,
           s_assertDisable_DObjCreateDuplicateParts_bufsize);

    duplicateParts = (unsigned char *)((int *)duplicatePartBits + 4);
    memset(duplicatePartBits, 0, sizeof(duplicatePartBits));
    len = 0;
    numBones = obj->models[0]->parts->numBones;
    boneCount = 1;

    while (boneCount < obj->numModels)
    {
        model = obj->models[boneCount];

        if (obj->modelParents[boneCount] == 0xFF)
        {
            names = model->parts->hierarchy->names;
            boneIter = model->parts->numBones;
            Assert(boneIter, s_assertDisable_DObjCreateDuplicateParts_numBones);
            Assert(boneIter < 128, s_assertDisable_DObjCreateDuplicateParts_numBonesMax);
            bRootMeld = 0;
            boneIndex = -1;

            for (localBoneIndex = 0; localBoneIndex < boneIter; localBoneIndex++)
            {
                boneIndex = DObjGetBoneIndex(obj, names[localBoneIndex]);

                Assert(boneIndex >= 0,
                       s_assertDisable_DObjCreateDuplicateParts_parent);

                if (boneIndex != numBones + localBoneIndex)
                {
                    if (!localBoneIndex)
                        bRootMeld = 1;

                    Assert(numBones + localBoneIndex + 1 < 256,
                           s_assertDisable_DObjCreateDuplicateParts_idx256);
                    Assert(boneIndex + 1 < 256,
                           s_assertDisable_DObjCreateDuplicateParts_par256);
                    Assert(boneIndex < numBones + localBoneIndex,
                           s_assertDisable_DObjCreateDuplicateParts_order);

                    index = numBones + localBoneIndex;
                    duplicateParts[len] = numBones + localBoneIndex + 1;
                    ((int *)duplicatePartBits)[index >> 5] |= 1 << (index & 0x1F);

                    Assert(duplicateParts[len],
                           s_assertDisable_DObjCreateDuplicateParts_dup0);

                    duplicateParts[++len] = boneIndex + 1;

                    Assert(duplicateParts[len],
                           s_assertDisable_DObjCreateDuplicateParts_dup1);

                    len++;
                }
            }

            if (!bRootMeld)
            {
                Com_Printf(
                    "WARNING: Attempting to meld model, but root part '%s' of model '%s' not found in model '%s' or any of its descendants\n",
                    SL_ConvertToString(names[0]),
                    model->name,
                    obj->models[0]->name);
            }
        }

        boneCount++;
        numBones += model->parts->numBones;
    }

    Assert(numBones < DOBJ_MAX_PARTS,
           s_assertDisable_DObjCreateDuplicateParts_final);

    /* ensure g_empty exists (lazy initialization) */
    if (!g_empty)
    {
        int emptyBuf[5];
        memset(emptyBuf, 0, sizeof(emptyBuf));
        g_empty = SL_GetStringOfLen((const char *)emptyBuf, 0, 17, 12);
        Assert(g_empty, s_assertDisable_DObjCreateDuplicateParts_empty);
    }

    if (len)
    {
        duplicateParts[len] = 0;
        obj->duplicateParts = SL_GetStringOfLen((const char *)duplicatePartBits, 0, len + 17, 12);
    }
    else
    {
        obj->duplicateParts = g_empty;
    }
}

/*
================
DObjSetBounds

Accumulates bounds from all sub-models into the DObj's
mins/maxs fields.
================
*/
void DObjSetBounds(DObj_t *obj)
{
    float mins[3];
    float maxs[3];
    int numModels;
    int i;

    Assert(obj, s_assertDisable_DObjSetBounds);

    numModels = obj->numModels;

    mins[0] = 0.0f;
    mins[1] = 0.0f;
    mins[2] = 0.0f;
    maxs[0] = 0.0f;
    maxs[1] = 0.0f;
    maxs[2] = 0.0f;

    for (i = 0; i < numModels; i++)
    {
        if (obj->models[i])
        {
            float modelmins[3], modelmaxs[3];
            XModelGetBounds(obj->models[i], modelmins, modelmaxs);
            mins[0] += modelmins[0];
            mins[1] += modelmins[1];
            mins[2] += modelmins[2];
            maxs[0] += modelmaxs[0];
            maxs[1] += modelmaxs[1];
            maxs[2] += modelmaxs[2];
        }
    }

    obj->mins[0] = mins[0];
    obj->mins[1] = mins[1];
    obj->mins[2] = mins[2];
    obj->maxs[0] = maxs[0];
    obj->maxs[1] = maxs[1];
    obj->maxs[2] = maxs[2];
}

/*
================
DObjCreate

Creates a DObj from an array of DObjModel_s.
Sets up models, model parents, bone counts, tree, bounds.
================
*/
void DObjCreate(DObjModel_t *dobjModels, unsigned int numModels,
                XAnimTree_s *tree, DObj_t *obj, unsigned short entnum)
{
    int modelIndex;
    int boneIndex;
    unsigned int name;
    const char *parentName;
    XModel_t *model;
    int numBones;
    int j;
    unsigned int i;

    Assert(dobjModels, s_assertDisable_DObjCreate_models);
    Assert(numModels > 0, s_assertDisable_DObjCreate_numModels);
    Assert((unsigned)numModels <= DOBJ_MAX_SUBMODELS,
           s_assertDisable_DObjCreate_submodels);
    Assert(obj, s_assertDisable_DObjCreate_obj);

    /* clear skel, timeStamp, duplicateParts, ignoreCollision */
    obj->skel = NULL;
    obj->timeStamp = 0;
    obj->duplicateParts = 0;
    obj->ignoreCollision = 0;

    /* set tree */
    if (tree)
    {
        Assert(tree->anims, s_assertDisable_DObjCreate_anims);
        obj->tree = tree;
        /* DObjSetTree inlined: set up animToModel */
        {
            unsigned int animTreeSize;
            unsigned char *parent;
            unsigned char childInfoIndex;

            animTreeSize = tree->anims->size;
            obj->animToModel = tree->inlineData + animTreeSize * 2;
            parent = (unsigned char *)obj->animToModel + animTreeSize * 2;
            childInfoIndex = *parent + 1;

            if (*parent == 0xFF)
            {
                childInfoIndex = 1;
                memset(parent + 1, 0, animTreeSize);
            }

            *parent = childInfoIndex;
        }

        tree->entnum = entnum;
    }
    else
    {
        obj->tree = NULL;
        obj->animToModel = NULL;
    }

    modelIndex = 0;
    numBones = 0;

    for (i = 0; i < numModels; i++)
    {
        model = dobjModels[i].model;

        obj->models[modelIndex] = model;
        obj->modelParents[modelIndex] = 0xFF;
        obj->matOffset[modelIndex] = numBones;

        if (dobjModels[i].ignoreCollision)
            obj->ignoreCollision |= 1 << i;

        if (i)
        {
            Assert(model->parts->numBones,
                   s_assertDisable_DObjCreate_numBones);
            Assert(model->parts->numBones < DOBJ_MAX_PARTS,
                   s_assertDisable_DObjCreate_numBonesMax);

            parentName = dobjModels[i].boneName;

            if (parentName)
            {
                if (parentName[0])
                {
                    name = SL_FindString(parentName);

                    if (name)
                    {
                        for (j = 0; j < modelIndex; j++)
                        {
                            boneIndex = XModelGetBoneIndex(obj->models[j], name);
                            if (boneIndex >= 0)
                            {
                                obj->modelParents[modelIndex] = obj->matOffset[j] + boneIndex;
                                goto setmodel;
                            }
                        }
                    }

                    Com_Printf(
                        "WARNING: Part '%s' not found in model '%s' or any of its descendants\n",
                        parentName,
                        obj->models[0]->name);

                    Assert(obj->modelParents[modelIndex] == 0xFF,
                           s_assertDisable_DObjCreate_parentUndef);
                }
            }
        }

setmodel:
        if (model)
        {
            if (numBones + model->parts->numBones >= DOBJ_MAX_PARTS)
            {
                Com_Error(1,
                    "dobj for xmodel '%s' has more than %d bones",
                    obj->models[0]->name, DOBJ_MAX_PARTS - 1);
            }

            numBones += model->parts->numBones;
        }

        modelIndex++;
    }

    Assert(modelIndex == (unsigned char)modelIndex,
           s_assertDisable_DObjCreate_numModelsByte);
    obj->numModels = modelIndex;

    Assert(numBones == (unsigned char)numBones,
           s_assertDisable_DObjCreate_numBonesByte);
    obj->numBones = numBones;

    Assert(modelIndex > 0,
           s_assertDisable_DObjCreate_numModelsPos);

    DObjSetBounds(obj);
}

/*
================
DObjCreateSkel

Initializes a DSkel_t for a DObj. Stores skel pointer,
timestamp, and zeros all partBits arrays.
================
*/
void DObjCreateSkel(DObj_t *obj, DSkel_t *skel, int time)
{
    int i;

    Assert(skel, s_assertDisable_DObjCreateSkel);

    obj->skel = skel;
    obj->timeStamp = time;

    for (i = 0; i < 4; i++)
    {
        skel->animPartBits[i] = 0;
        skel->controlPartBits[i] = 0;
        skel->skelPartBits[i] = 0;
    }
}

extern void DObjAnimMatToSkelMat(DObjAnimMat_t *mat, void *outMat); /* xanim_public_41B340 */

/*
================
DObjGetMatrices

For each enabled bone (per partBits), calls DObjAnimMatToSkelMat
to compute the world-space matrix into outMatrices. Walks all
boneCount bones, indexing partBits as 32-bit words and using
a rotating bit mask to test each bone's bit.
================
*/
void DObjGetMatrices(DObj_t *obj, int *partBits, void *outMatrices)
{
    int boneCount;
    DObjAnimMat_t *mat;
    unsigned char *outBuf;
    int i;
    unsigned int bit;

    boneCount = obj->numBones;

    Assert(boneCount <= DOBJ_MAX_PARTS,
           s_assertDisable_DObjGetMatrices_count);

    mat = obj->skel ? &obj->skel->mat[0] : (DObjAnimMat_t *)0;
    Assert(mat, s_assertDisable_DObjGetMatrices_mat);

    outBuf = (unsigned char *)outMatrices;
    bit = 1;

    for (i = 0; i < boneCount; i++)
    {
        if (partBits[i >> 5] & bit)
            DObjAnimMatToSkelMat(mat, outBuf);

        mat++;
        outBuf += sizeof(float[16]); /* 4x4 bone matrix = 64 bytes */
        /* rotate bit left by 1 (wraps from 0x80000000 back to 1 every 32 iterations) */
        bit = (bit << 1) | (bit >> 31);
    }
}

/*
================
DObjGetSurface

Returns a pointer to an XSurface_t for the given model,
surface index, and LOD level.

Access pattern (from LST):
  model = obj->models[modelIndex]
  modelSurfs = model->lodInfo[lod].modelSurfs   (ptr at model + lod*40 + 40)
  surfs = modelSurfs->surfs                      (ptr at modelSurfs + 0)
  return surfs[surfIndex]                         (ptr at surfs + surfIndex*8)
================
*/
XSurface_t *DObjGetSurface(const DObj_t *obj, int modelIndex, int surfIndex, int lod)
{
    XModel_t *model;

    Assert(lod >= 0, s_assertDisable_DObjGetSurface);

    model = obj->models[modelIndex];
    return model->lodInfo[lod].modelSurfs->surfs[surfIndex];
}

/*
================
DObjGetSurfaceName

Returns the material name for a surface at a given model,
submat index, and LOD level. Returns "DEFAULT" if the
string handle is zero.

Access pattern (from LST):
  model = obj->models[modelIndex]
  numsurfs = lodInfo[lod].numsurfs     (short at model + lod*40 + 24)
  surfNames = lodInfo[lod].surfNames   (ptr at model + lod*40 + 32)
  handle = surfNames[subMatIndex]      (unsigned short)
  return handle ? SL_ConvertToString(handle) : "DEFAULT"
================
*/
const char *DObjGetSurfaceName(const DObj_t *obj, int modelIndex,
                               unsigned int subMatIndex, int lod)
{
    XModel_t *model;
    unsigned short handle;

    Assert(lod >= 0, s_assertDisable_DObjGetSurfaceName_lod);

    model = obj->models[modelIndex];

    Assert((unsigned)subMatIndex < model->lodInfo[lod].numsurfs,
           s_assertDisable_DObjGetSurfaceName_submat);

    handle = model->lodInfo[lod].surfNames[subMatIndex];

    if (!handle)
        return "DEFAULT";

    return SL_ConvertToString(handle);
}

/*
================
DObjGetBoneName

Returns the string name of a bone at the given global index.
Walks each model to find the local bone index.
================
*/
const char *DObjGetBoneName(const DObj_t *obj, int index)
{
    int numModels;
    int numBones;
    XModel_t *model;
    int count;
    int localIndex;
    int i;

    Assert(obj, s_assertDisable_DObjGetBoneName);

    numModels = obj->numModels;
    count = 0;

    for (i = 0; i < numModels; i++)
    {
        model = obj->models[i];
        numBones = model->parts->numBones;
        localIndex = index - count;

        Assert(localIndex >= 0, s_assertDisable_DObjGetBoneName_index);

        if (localIndex < numBones)
            return SL_ConvertToString(model->parts->hierarchy->names[localIndex]);

        Assert(numBones >= 0, s_assertDisable_DObjGetBoneName_numBones);

        count += numBones;
    }

    return NULL;
}

/*
================
DObjDumpInfo

Prints debug info about a DObj — models, bones, and
duplicate parts mappings.
================
*/
void DObjDumpInfo(const DObj_t *obj)
{
    XModel_t *model;
    unsigned char *partName;
    int bones;
    int numModels;
    int numBones;
    int i;
    int j;

    if (!obj)
    {
        Com_Printf("No Dobj\n");
        return;
    }

    Com_Printf("\nModels:\n");
    numModels = obj->numModels;
    bones = 0;

    for (i = 0; i < numModels; i++)
    {
        model = obj->models[i];
        Com_Printf("%d: '%s'\n", bones, model->name);
        bones += model->parts->numBones;
    }

    Com_Printf("\nBones:\n");
    numBones = obj->numBones;

    for (j = 0; j < numBones; j++)
    {
        Com_Printf("Bone %d: '%s'\n", j, DObjGetBoneName(obj, j));
    }

    if (obj->duplicateParts)
    {
        Com_Printf("\nPart duplicates:\n");

        for (partName = (unsigned char *)(SL_ConvertToString(obj->duplicateParts) + 16);
             *partName;
             partName += 2)
        {
            Com_Printf("%d ('%s') -> %d ('%s')\n",
                        *partName - 1,
                        DObjGetBoneName(obj, *partName - 1),
                        partName[1] - 1,
                        DObjGetBoneName(obj, partName[1] - 1));
        }
    }
    else
    {
        Com_Printf("\nNo part duplicates.\n");
    }

    Com_Printf("\n");
}

/*
 * Quat/matrix helpers — inlined SSE in the binary's DObjCalcSkel, defined here
 * as static helpers so they can share the same inlining choice at our compiler's
 * discretion. DObjAnimMat_t layout: quat[4] at [0], trans[3] at [16], transWeight[4] at [28].
 */

/* q = q * other — first inline at 0x41E284 (root bones with parent model)
 * Evaluation order must match the original's SSE instruction sequence exactly. */
static void LocalQuatMultiplyEquals_v1(float *q, const float *other)
{
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float ox = other[0], oy = other[1], oz = other[2], ow = other[3];
    float t;
    /* q[0] = ((qw*ox + qx*ow) + qz*oy) - qy*oz */
    q[0] = qw*ox + qx*ow + qz*oy - qy*oz;
    /* q[1] = ((qy*ow - qz*ox) + qw*oy) + qx*oz */
    t = qy*ow - qz*ox;
    t = t + qw*oy;
    q[1] = t + qx*oz;
    /* q[2] = ((qz*ow + qy*ox) - qx*oy) + qw*oz */
    t = qz*ow + qy*ox;
    t = t - qx*oy;
    q[2] = t + qw*oz;
    /* q[3] = ((qw*ow - qx*ox) - qy*oy) - qz*oz */
    q[3] = qw*ow - qx*ox - qy*oy - qz*oz;
}

/* q = q * other — second inline at 0x41E375 (non-root bones in hierarchy)
 * Different evaluation order from v1 due to compiler register allocation. */
static void LocalQuatMultiplyEquals_v2(float *q, const float *other)
{
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float ox = other[0], oy = other[1], oz = other[2], ow = other[3];
    float t;
    /* q[0] = ((qx*ow + qw*ox) + qy*oz) - qz*oy */
    t = qx*ow + qw*ox;
    t = t + qy*oz;
    q[0] = t - qz*oy;
    /* q[1] = ((qw*oy - qx*oz) + qy*ow) + qz*ox */
    t = qw*oy - qx*oz;
    t = t + qy*ow;
    q[1] = t + qz*ox;
    /* q[2] = ((qw*oz + qx*oy) - qy*ox) + qz*ow */
    t = qw*oz + qx*oy;
    t = t - qy*ox;
    q[2] = t + qz*ow;
    /* q[3] = ((qw*ow - qx*ox) - qy*oy) - qz*oz */
    q[3] = qw*ow - qx*ox - qy*oy - qz*oz;
}

/* q = conj(other) * q  (reverse order with conjugation) */
static void LocalQuatMultiplyReverseEquals(float *q, const float *other)
{
    float qx = q[0], qy = q[1], qz = q[2], qw = q[3];
    float ox = -other[0], oy = -other[1], oz = -other[2], ow = other[3];
    q[0] = ow*qx + ox*qw + oy*qz - oz*qy;
    q[1] = ow*qy - ox*qz + oy*qw + oz*qx;
    q[2] = ow*qz + ox*qy - oy*qx + oz*qw;
    q[3] = ow*qw - ox*qx - oy*qy - oz*qz;
}

/* DObjAnimMat.transWeight = 2.0 / (x*x + y*y + z*z + w*w).
 * On degenerate (zero/NaN) lenSq the binary resets quat[3]=1.0 and transWeight=2.0
 * (inline at LST 0x41D84E / 0x41D855 — identity rotation fallback). */
static void DObjCalcTransWeight(DObjAnimMat_t *m)
{
    float *q = (float *)m->quat;
    float lenSq = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (lenSq != 0.0f)
    {
        m->transWeight = 2.0f / lenSq;
    }
    else
    {
        q[3] = 1.0f;
        m->transWeight = 2.0f;
    }
}

/* Transforms inout vector by in's rotation matrix and adds in's translation.
 * Matches CoD2rev: MatrixTransformVectorQuatTransEquals(float *inout, DObjAnimMat *in)
 * inout = parentMat->trans (current bone), in = parent DObjAnimMat */
static void LocalMatrixTransformVectorQuatTransEquals(DObjAnimMat_t *current, DObjAnimMat_t *parent)
{
    float *inout = current->trans;
    float s = parent->transWeight;
    float qx = parent->quat[0], qy = parent->quat[1], qz = parent->quat[2], qw = parent->quat[3];

    float xs = qx * s, ys = qy * s, zs = qz * s;
    float xx = qx * xs, yy = qy * ys, zz = qz * zs;
    float xy = qy * xs, xz = qz * xs, yz = qz * ys;
    float wx = qw * xs, wy = qw * ys, wz = qw * zs;

    float a00 = 1.0f - (yy + zz), a01 = xy + wz,          a02 = xz - wy;
    float a10 = xy - wz,          a11 = 1.0f - (xx + zz),  a12 = yz + wx;
    float a20 = xz + wy,          a21 = yz - wx,            a22 = 1.0f - (xx + yy);

    float tx = inout[0], ty = inout[1], tz = inout[2];

    float r0 = tx * a00 + ty * a10 + tz * a20 + parent->trans[0];
    float r1 = tx * a01 + ty * a11 + tz * a21 + parent->trans[1];

    inout[2] = tx * a02 + ty * a12 + tz * a22 + parent->trans[2];
    inout[0] = r0;
    inout[1] = r1;
}

/*
================
DObjCalcSkel

Calculates the full skeleton for the given partBits.
Processes the bone hierarchy — applies parent transforms,
handles duplicate parts, and resolves meld conflicts.

Quaternion math is implemented as static helpers above — the binary has them
inlined as SSE operations inside DObjCalcSkel's body.
Full LST audit completed — all asserts verified.
================
*/
void DObjCalcSkel(const DObj_t *obj, int *partBits)
{
    DSkel_t *skel;
    DObjAnimMat_t *mat;
    DObjAnimMat_t *parentMat;
    const int *savedDuplicatePartBits;
    const unsigned char *duplicateParts;
    int ignorePartBits[4];
    int controlPartBits[4];
    int calcPartBits[4];
    int bFinished;
    int numModels;
    int boneIndex;
    int boneBit;
    int boneIndexHigh;
    int numRootBones;
    int parentIndex;
    int parentBit;
    XModelParts_t *parts;
    unsigned char parent;
    unsigned char *parentList;
    float *quat;
    float *trans;
    int i;

    Assert(obj, s_assertDisable_DObjCalcSkel);

    skel = obj->skel;
    Assert(skel, s_assertDisable_DObjCalcSkel_skel);

    /* check which bones are already calculated */
    bFinished = 1;
    for (i = 0; i < 4; i++)
    {
        ignorePartBits[i] = skel->skelPartBits[i] | ~partBits[i];
        if (ignorePartBits[i] != -1)
            bFinished = 0;
    }

    if (bFinished)
        return;

    /* ensure duplicate parts structure exists */
    if (!obj->duplicateParts)
        DObjCreateDuplicateParts((DObj_t *)obj);

    Assert(obj->duplicateParts, s_assertDisable_DObjCalcSkel_dup);

    savedDuplicatePartBits = (const int *)SL_ConvertToString(obj->duplicateParts);

    /* update skel bits and compute control/calc partBits */
    for (i = 0; i < 4; i++)
    {
        skel->skelPartBits[i] |= partBits[i];
        controlPartBits[i] = skel->controlPartBits[i] & ~ignorePartBits[i];
        calcPartBits[i] = savedDuplicatePartBits[i] | controlPartBits[i] | ignorePartBits[i];

        /* detect control/meld conflict */
        if (savedDuplicatePartBits[i] & controlPartBits[i])
        {
            int conflictBone;
            int scanBit;

            DObjDumpInfo(obj);

            /* scan all bones to find the exact conflicting one */
            conflictBone = 0; /* binary uses r13d which was zeroed */
            scanBit = 1;
            while (conflictBone < obj->numBones)
            {
                if ((controlPartBits[conflictBone >> 5] & scanBit)
                    && (savedDuplicatePartBits[conflictBone >> 5] & scanBit))
                    break;
                conflictBone++;
                scanBit = (scanBit << 1) | (scanBit >> 31);
            }

            if (!g_dobj_conflict_tolerance)
                Assert(0, s_assertDisable_DObjCalcSkel_conflict);
        }
    }

    for (i = 0; i < 4; i++)
        controlPartBits[i] |= ~calcPartBits[i];

    numModels = obj->numModels;
    mat = &skel->mat[0];
    parentMat = mat;
    boneIndex = 0;
    duplicateParts = (const unsigned char *)(savedDuplicatePartBits + 4);

    for (i = 0; i < numModels; i++)
    {
        parts = obj->models[i]->parts;
        parent = obj->modelParents[i];

        if (parent == 0xFF)
        {
            /* no parent model — process root bones independently */
            numRootBones = parts->numRootBones;

            while (numRootBones)
            {
                boneBit = 1 << (boneIndex & 0x1F);
                boneIndexHigh = boneIndex >> 5;

                if (controlPartBits[boneIndexHigh] & boneBit)
                {
                    Assert(boneIndex != *duplicateParts - 1,
                           s_assertDisable_DObjCalcSkel_notDup);
                    Assert(!(ignorePartBits[boneIndexHigh] & boneBit),
                           s_assertDisable_DObjCalcSkel_notIgnore);
                    Assert(skel->animPartBits[boneIndexHigh] & boneBit,
                           s_assertDisable_DObjCalcSkel_animSet);

                    DObjCalcTransWeight(parentMat);

                    Assert(!IS_NAN(parentMat->quat[0]) && !IS_NAN(parentMat->quat[1])
                        && !IS_NAN(parentMat->quat[2]) && !IS_NAN(parentMat->quat[3]),
                           s_assertDisable_DObjCalcSkel_quatNaN);
                    Assert(!IS_NAN(parentMat->trans[0]) && !IS_NAN(parentMat->trans[1])
                        && !IS_NAN(parentMat->trans[2]),
                           s_assertDisable_DObjCalcSkel_transNaN);
                }
                else if (boneIndex == *duplicateParts - 1)
                {
                    duplicateParts += 2;

                    if (!(ignorePartBits[boneIndexHigh] & boneBit))
                    {
                        parentIndex = *(duplicateParts - 1) - 1;

                        Assert(parentMat == &mat[boneIndex],
                               s_assertDisable_DObjCalcSkel_matPos);
                        Assert(skel->skelPartBits[parentIndex >> 5] & (1 << (parentIndex & 0x1F)),
                               s_assertDisable_DObjCalcSkel_srcSkel);
                        Assert(skel->animPartBits[parentIndex >> 5] & (1 << (parentIndex & 0x1F)),
                               s_assertDisable_DObjCalcSkel_srcAnim);
                        Assert(parentIndex < boneIndex,
                               s_assertDisable_DObjCalcSkel_srcOrder);

                        quat = mat[parentIndex].quat;
                        parentMat->quat[0] = quat[0];
                        parentMat->quat[1] = quat[1];
                        parentMat->quat[2] = quat[2];
                        parentMat->quat[3] = quat[3];
                        parentMat->trans[0] = quat[4];
                        parentMat->trans[1] = quat[5];
                        parentMat->trans[2] = quat[6];
                        parentMat->transWeight = quat[7];
                    }
                    else if (skel->skelPartBits[boneIndexHigh] & boneBit)
                    {
                        /* bone already computed — validate consistency */
                        Assert(skel->animPartBits[boneIndexHigh] & boneBit,
                               s_assertDisable_DObjCalcSkel_ignAnim);
                        Assert(!IS_NAN(parentMat->quat[0]) && !IS_NAN(parentMat->quat[1])
                            && !IS_NAN(parentMat->quat[2]) && !IS_NAN(parentMat->quat[3]),
                               s_assertDisable_DObjCalcSkel_ignQuatNaN);
                        Assert(!IS_NAN(parentMat->trans[0]) && !IS_NAN(parentMat->trans[1])
                            && !IS_NAN(parentMat->trans[2]),
                               s_assertDisable_DObjCalcSkel_ignTransNaN);
                    }
                }

                numRootBones--;
                parentMat++;
                boneIndex++;
            }
        }
        else
        {
            /* has parent model — transform root bones relative to parent */
            DObjAnimMat_t *childMat = &mat[parent];
            numRootBones = parts->numRootBones;

            while (numRootBones)
            {
                boneBit = 1 << (boneIndex & 0x1F);
                boneIndexHigh = boneIndex >> 5;

                if (controlPartBits[boneIndexHigh] & boneBit)
                {
                    Assert(parent < boneIndex,
                           s_assertDisable_DObjCalcSkel_parentLt);
                    Assert(boneIndex != *duplicateParts - 1,
                           s_assertDisable_DObjCalcSkel_notDup2);
                    Assert(parent < boneIndex,
                           s_assertDisable_DObjCalcSkel_parentLt2);
                    Assert(!(ignorePartBits[boneIndexHigh] & boneBit),
                           s_assertDisable_DObjCalcSkel_notIgnore2);
                    Assert(skel->animPartBits[boneIndexHigh] & boneBit,
                           s_assertDisable_DObjCalcSkel_animSet2);
                    Assert(skel->skelPartBits[parent >> 5] & (1 << (parent & 0x1F)),
                           s_assertDisable_DObjCalcSkel_parentSkel);
                    Assert(skel->animPartBits[parent >> 5] & (1 << (parent & 0x1F)),
                           s_assertDisable_DObjCalcSkel_parentAnim);

                    if (boneBit & calcPartBits[boneIndexHigh])
                        LocalQuatMultiplyEquals_v1(childMat->quat, parentMat->quat);
                    else
                        LocalQuatMultiplyReverseEquals(parentMat->quat, childMat->quat);

                    DObjCalcTransWeight(parentMat);
                    LocalMatrixTransformVectorQuatTransEquals(parentMat, childMat);

                    Assert(!IS_NAN(parentMat->quat[0]) && !IS_NAN(parentMat->quat[1])
                        && !IS_NAN(parentMat->quat[2]) && !IS_NAN(parentMat->quat[3]),
                           s_assertDisable_DObjCalcSkel_quatNaN2);
                    Assert(!IS_NAN(parentMat->trans[0]) && !IS_NAN(parentMat->trans[1])
                        && !IS_NAN(parentMat->trans[2]),
                           s_assertDisable_DObjCalcSkel_transNaN2);
                }

                numRootBones--;
                parentMat++;
                boneIndex++;
            }
        }

        /* process non-root bones via hierarchy */
        trans = parts->trans;
        parentList = parts->hierarchy->parentList;
        numRootBones = parts->numBones - parts->numRootBones;

        while (numRootBones)
        {
            boneIndexHigh = boneIndex >> 5;
            boneBit = 1 << (boneIndex & 0x1F);

            if (controlPartBits[boneIndexHigh] & boneBit)
            {
                Assert(boneIndex != *duplicateParts - 1,
                       s_assertDisable_DObjCalcSkel_notDup3);
                Assert(!(ignorePartBits[boneIndexHigh] & boneBit),
                       s_assertDisable_DObjCalcSkel_notIgnore3);
                Assert(skel->animPartBits[boneIndexHigh] & boneBit,
                       s_assertDisable_DObjCalcSkel_animSet3);
                Assert(skel->skelPartBits[(boneIndex - *parentList) >> 5]
                    & (1 << ((boneIndex - *parentList) & 0x1F)),
                       s_assertDisable_DObjCalcSkel_hierSkel);
                Assert(skel->animPartBits[(boneIndex - *parentList) >> 5]
                    & (1 << ((boneIndex - *parentList) & 0x1F)),
                       s_assertDisable_DObjCalcSkel_hierAnim);

                if (boneBit & calcPartBits[boneIndexHigh])
                    LocalQuatMultiplyEquals_v2(parentMat[-*parentList].quat, parentMat->quat);
                else
                    LocalQuatMultiplyReverseEquals(parentMat->quat, parentMat[-*parentList].quat);

                DObjCalcTransWeight(parentMat);
                VectorAdd(parentMat->trans, trans, parentMat->trans);
                LocalMatrixTransformVectorQuatTransEquals(parentMat, &parentMat[-*parentList]);

                Assert(!IS_NAN(parentMat->quat[0]) && !IS_NAN(parentMat->quat[1])
                    && !IS_NAN(parentMat->quat[2]) && !IS_NAN(parentMat->quat[3]),
                       s_assertDisable_DObjCalcSkel_quatNaN3);
                Assert(!IS_NAN(parentMat->trans[0]) && !IS_NAN(parentMat->trans[1])
                    && !IS_NAN(parentMat->trans[2]),
                       s_assertDisable_DObjCalcSkel_transNaN3);
            }
            else if (boneIndex == *duplicateParts - 1)
            {
                duplicateParts += 2;

                if (!(ignorePartBits[boneIndexHigh] & boneBit))
                {
                    parentIndex = *(duplicateParts - 1) - 1;

                    Assert(parentMat == &mat[boneIndex],
                           s_assertDisable_DObjCalcSkel_matPos2);
                    Assert(skel->skelPartBits[parentIndex >> 5] & (1 << (parentIndex & 0x1F)),
                           s_assertDisable_DObjCalcSkel_srcSkel2);
                    Assert(skel->animPartBits[parentIndex >> 5] & (1 << (parentIndex & 0x1F)),
                           s_assertDisable_DObjCalcSkel_srcAnim2);
                    Assert(parentIndex < boneIndex,
                           s_assertDisable_DObjCalcSkel_srcOrder2);

                    quat = mat[parentIndex].quat;
                    parentMat->quat[0] = quat[0];
                    parentMat->quat[1] = quat[1];
                    parentMat->quat[2] = quat[2];
                    parentMat->quat[3] = quat[3];
                    parentMat->trans[0] = quat[4];
                    parentMat->trans[1] = quat[5];
                    parentMat->trans[2] = quat[6];
                    parentMat->transWeight = quat[7];
                }
                else if (skel->skelPartBits[boneIndexHigh] & boneBit)
                {
                    /* bone already computed — validate consistency */
                    Assert(skel->animPartBits[boneIndexHigh] & boneBit,
                           s_assertDisable_DObjCalcSkel_ignAnim2);
                    Assert(!IS_NAN(parentMat->quat[0]) && !IS_NAN(parentMat->quat[1])
                        && !IS_NAN(parentMat->quat[2]) && !IS_NAN(parentMat->quat[3]),
                           s_assertDisable_DObjCalcSkel_ignQuatNaN2);
                    Assert(!IS_NAN(parentMat->trans[0]) && !IS_NAN(parentMat->trans[1])
                        && !IS_NAN(parentMat->trans[2]),
                           s_assertDisable_DObjCalcSkel_ignTransNaN2);
                }
            }

            numRootBones--;
            parentMat++;
            trans += 3;
            parentList++;
            boneIndex++;
        }
    }

    /* final validation: assert all duplicate parts consumed */
    Assert(!(*duplicateParts), s_assertDisable_DObjCalcSkel);

    /* validate IS_NAN on quat/trans for all bones with anim bits set */
    {
        int boneIdx;
        int numBones = obj->numBones;
        DObjAnimMat_t *sourceMat = obj->skel->mat;
        int *animBits = obj->skel->animPartBits;

        for (boneIdx = 0; boneIdx < numBones; boneIdx++)
        {
            if (animBits[boneIdx >> 5] & (1 << (boneIdx & 31)))
            {
                Assert(!IS_NAN_FLOAT(sourceMat[boneIdx].quat[0])
                    && !IS_NAN_FLOAT(sourceMat[boneIdx].quat[1])
                    && !IS_NAN_FLOAT(sourceMat[boneIdx].quat[2])
                    && !IS_NAN_FLOAT(sourceMat[boneIdx].quat[3]),
                    s_assertDisable_DObjCalcSkel);
                Assert(!IS_NAN_FLOAT(sourceMat[boneIdx].trans[0])
                    && !IS_NAN_FLOAT(sourceMat[boneIdx].trans[1])
                    && !IS_NAN_FLOAT(sourceMat[boneIdx].trans[2]),
                    s_assertDisable_DObjCalcSkel);
            }
        }
    }
}

/*
================
DObjCalcAnim

Sets up bone animation matrices from the skeleton.
Root bones get identity quaternions. Non-root bones
get quaternions decompressed from 16-bit shorts via
the SHORT_TO_QUAT constant (0x38000100).
================
*/
void DObjCalcAnim(const DObj_t *obj, int *partBits)
{
    static const float SHORT_TO_QUAT = 3.0518509e-5f; /* 0x38000100 */

    DSkel_t *skel;
    DObjAnimMat_t *mat;
    int ignorePartBits[4];
    int bFinished;
    int numModels;
    int numRootBones;
    int numNonRootBones;
    XModelParts_t *parts;
    short *quats;
    int boneIndex;
    int boneBit;
    int i;
    int j;

    Assert(obj, s_assertDisable_DObjCalcAnim);

    skel = obj->skel;
    Assert(skel, s_assertDisable_DObjCalcAnim_skel);

    /* check which bones are already animated */
    bFinished = 1;
    for (i = 0; i < 4; i++)
    {
        ignorePartBits[i] = skel->animPartBits[i] | ~partBits[i];
        if (ignorePartBits[i] != -1)
            bFinished = 0;
    }

    if (bFinished)
        return;

    /* mark bones as animated */
    skel->animPartBits[0] |= partBits[0];
    skel->animPartBits[1] |= partBits[1];
    skel->animPartBits[2] |= partBits[2];
    skel->animPartBits[3] |= partBits[3];

    numModels = obj->numModels;
    mat = &skel->mat[0];
    boneIndex = 0;

    for (i = 0; i < numModels; i++)
    {
        parts = obj->models[i]->parts;
        numRootBones = parts->numRootBones;
        boneBit = 1 << (boneIndex & 0x1F);

        /* process root bones — set to identity if not already done */
        for (j = 0; j < numRootBones; j++)
        {
            if (!(ignorePartBits[boneIndex >> 5] & boneBit))
            {
                mat->quat[0] = 0.0f;
                mat->quat[1] = 0.0f;
                mat->quat[2] = 0.0f;
                mat->quat[3] = 1.0f;
                mat->trans[0] = 0.0f;
                mat->trans[1] = 0.0f;
                mat->trans[2] = 0.0f;
            }
            mat++;
            boneIndex++;
            boneBit = (boneBit << 1) | (boneBit >> 31); /* rol */
        }

        /* process non-root bones — decompress quaternions from shorts */
        quats = parts->quats;
        numNonRootBones = parts->numBones - parts->numRootBones;

        for (j = 0; j < numNonRootBones; j++)
        {
            if (!(ignorePartBits[boneIndex >> 5] & boneBit))
            {
                mat->quat[0] = (float)quats[0] * SHORT_TO_QUAT;
                mat->quat[1] = (float)quats[1] * SHORT_TO_QUAT;
                mat->quat[2] = (float)quats[2] * SHORT_TO_QUAT;
                mat->quat[3] = (float)quats[3] * SHORT_TO_QUAT;
                mat->trans[0] = 0.0f;
                mat->trans[1] = 0.0f;
                mat->trans[2] = 0.0f;
            }
            mat++;
            boneIndex++;
            boneBit = (boneBit << 1) | (boneBit >> 31); /* rol */
            quats += 4;
        }
    }
}

/*
================
DObjGetSurfaces

Gets the combined surface bits across all models for the
given LOD array. Returns total surface count.

Writes surfMap entries as pairs of (modelIndex:16, surfIndex:16)
and OR's each model's surface partBits into the output,
shifted by the model's bone offset (matOffset).

Params (from LST caller analysis):
  obj      — the DObj
  surfMap  — output array of 4-byte entries per surface
  partBits — output 4-int bit array (zeroed by this function)
  lods     — byte array, one LOD level per model (-1 to skip)

XModel access patterns (from LST):
  modelSurfs   = *(model + lod*40 + 40)   (lodInfo[lod].modelSurfs)
  numsurfs     = *(short*)(model + lod*40 + 24)
  surfPartBits = (int*)(modelSurfs + 8)    (4 ints of bone bits)
  boneOffset   = obj->matOffset[i]         (at obj + 0x78 + i)
================
*/
int DObjGetSurfaces(const DObj_t *obj, unsigned short *surfMap,
                    int *partBits, const char *lods)
{
    int numModels;
    int totalSurfs;
    int numSurfs;
    int numBoneBits;
    int lod;
    int shift;
    int wordOff;
    int boneOffset;
    XModel_t *model;
    XModelSurfs_t *modelSurfs;
    int i;
    int j;
    int surfIdx;

    partBits[0] = 0;
    partBits[1] = 0;
    partBits[2] = 0;
    partBits[3] = 0;

    totalSurfs = 0;
    numModels = obj->numModels;
    surfIdx = 0;

    for (i = 0; i < numModels; i++)
    {
        lod = lods[i];

        if (lod < 0)
            goto next_model;

        model = obj->models[i];
        modelSurfs = model->lodInfo[lod].modelSurfs;
        if (!modelSurfs)
            goto next_model;

        numBoneBits = (model->parts->numBones - 1) >> 5;
        Assert((unsigned)numBoneBits < DOBJ_MAX_PART_BITS,
               s_assertDisable_DObjGetSurfaces_bits);

        numSurfs = model->lodInfo[lod].numsurfs;

        if (totalSurfs + numSurfs > 64)
        {
            Com_Printf("ERROR: models with more than %i total surfaces\n", 64);
            {
                int m;
                for (m = 0; m < obj->numModels; m++)
                {
                    XModel_t *mdl = obj->models[m];
                    int mlod = (signed char)lods[m];
                    XSurface_t *_surfs;
                    int *_pb;
                    int mNumSurfs = XModelGetSurfaces(mdl, &_surfs, mlod, &_pb);
                    Com_Printf("  model '%s' lod %i has %i surfaces\n",
                               XModelGetName(mdl), mlod, mNumSurfs);
                }
            }
            Com_Error(1, "Max surfs exceeded - see console for details");
        }

        totalSurfs += numSurfs;

        /* write surfMap entries: (modelIndex, localSurfIndex) per surface */
        for (j = 0; j < numSurfs; j++)
        {
            surfMap[surfIdx * 2 + 1] = j;      /* local surface index */
            surfMap[surfIdx * 2] = (unsigned short)i; /* model index */
            surfIdx++;
        }

        /* shift and OR the model's surface partBits into output */
        boneOffset = obj->matOffset[i];
        shift = boneOffset & 0x1F;
        wordOff = boneOffset >> 5;

        if (shift != 0)
        {
            partBits[wordOff] |= modelSurfs->partBits[0] << shift;

            for (j = 0; j < numBoneBits; j++)
            {
                partBits[wordOff + 1 + j] |=
                    (modelSurfs->partBits[j + 1] << shift) |
                    (modelSurfs->partBits[j] >> (32 - shift));
            }

            partBits[wordOff + 1 + numBoneBits] |=
                modelSurfs->partBits[numBoneBits] >> (32 - shift);
        }
        else
        {
            for (j = 0; j <= numBoneBits; j++)
                partBits[wordOff + j] |= modelSurfs->partBits[j];
        }

next_model:
        ; /* advance to next model */
    }

    return totalSurfs;
}
