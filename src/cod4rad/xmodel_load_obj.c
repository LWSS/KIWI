/*
 * xmodel_load_obj.c - CoD4 v25 raw XModel loading for cod4rad.
 *
 * The map compiler keeps its existing x64 runtime structures and allocation
 * callbacks, but consumes the CoD4 v25 xmodel/xmodelparts/xmodelsurfs wire
 * formats used by the game loader.
 */

#include "cod2rad64.h"

#define XMODEL_COLL_BOUNDS_EPSILON 0.001f
#define XMODEL_COLL_NORMAL_EPSILON 0.01f

static char s_assertDisable_XModelReadCompressedQuat;
static char s_assertDisable_XModelPartsLoadFile_buf;
static char s_assertDisable_XModelPartsLoadFile_bufValid;
static char s_assertDisable_XModelPartsLoadFile_index;
static char s_assertDisable_XModelPartsLoadFile_indexLt;
static char s_assertDisable_XModelPartsLoadFile_parentList;
static char s_assertDisable_XModelReadCollSurfs_contents;
static char s_assertDisable_XModelReadCollSurfs_noSurfs;
static char s_assertDisable_XModelReadCollSurfs_tris;
static char s_assertDisable_XModelReadCollSurfs_normal;
static char s_assertDisable_XModelReadCollSurfs_boneIdx;
static char s_assertDisable_R_XModelSurfsReadData_surfs;
static char s_assertDisable_R_XModelSurfsReadData_bits;
static char s_assertDisable_R_XModelSurfsReadData_count;
static char s_assertDisable_R_XModelSurfsReadData_pos;
static char s_assertDisable_R_XModelSurfsReadData_posData;
static char s_assertDisable_XModelSurfsLoadFile_buf;
static char s_assertDisable_XModelSurfsLoadFile_bufValid;
static char s_assertDisable_XModelLoadFile_buf;
static char s_assertDisable_XModelLoadFile_numLods;
static char s_assertDisable_XModelLoadFile_collLod;
static char s_assertDisable_XModelSurfsLoad;
static char s_assertDisable_XModelSurfsLoad_filename;

/*
================
XModelCalcBasePose

CoD4 stores three signed quaternion components per non-root bone.  The fourth
component is reconstructed by XModelReadCompressedQuat, and all four are
scaled by 1/32767 before concatenating them with the parent base pose.
================
*/
void XModelCalcBasePose(XModelParts_t *modelParts)
{
    static const float SHORT_TO_QUAT = 0.00003051850944757462f;
    unsigned char *parentList;
    DObjAnimMat_t *quatTrans;
    int numBones;
    float *trans;
    short *quats;
    int count;
    float vLenSq;
    float tempQuat[4];

    parentList = modelParts->hierarchy->parentList;
    numBones = modelParts->numBones;
    quats = modelParts->quats;
    trans = modelParts->trans;
    quatTrans = &modelParts->skel_mat[0];

    count = modelParts->numRootBones;
    while (count)
    {
        quatTrans->quat[0] = 0.0f;
        quatTrans->quat[1] = 0.0f;
        quatTrans->quat[2] = 0.0f;
        quatTrans->quat[3] = 1.0f;
        quatTrans->trans[0] = 0.0f;
        quatTrans->trans[1] = 0.0f;
        quatTrans->trans[2] = 0.0f;
        quatTrans->transWeight = 2.0f;
        count--;
        quatTrans++;
    }

    count = numBones - modelParts->numRootBones;
    while (count)
    {
        DObjAnimMat_t *parent;
        float mat[3][3];

        tempQuat[0] = (float)quats[0] * SHORT_TO_QUAT;
        tempQuat[1] = (float)quats[1] * SHORT_TO_QUAT;
        tempQuat[2] = (float)quats[2] * SHORT_TO_QUAT;
        tempQuat[3] = (float)quats[3] * SHORT_TO_QUAT;

        parent = &quatTrans[-(int)*parentList];
        QuatMultiply(tempQuat, parent->quat, quatTrans->quat);

        vLenSq = quatTrans->quat[0] * quatTrans->quat[0]
               + quatTrans->quat[1] * quatTrans->quat[1]
               + quatTrans->quat[2] * quatTrans->quat[2]
               + quatTrans->quat[3] * quatTrans->quat[3];
        if (vLenSq == 0.0f)
        {
            quatTrans->quat[3] = 1.0f;
            quatTrans->transWeight = 2.0f;
        }
        else
        {
            quatTrans->transWeight = 2.0f / vLenSq;
        }

        DObjAnimMatToAxis(parent, mat);
        quatTrans->trans[0] = mat[0][0] * trans[0] + mat[1][0] * trans[1]
                            + mat[2][0] * trans[2] + parent->trans[0];
        quatTrans->trans[1] = mat[0][1] * trans[0] + mat[1][1] * trans[1]
                            + mat[2][1] * trans[2] + parent->trans[1];
        quatTrans->trans[2] = mat[0][2] * trans[0] + mat[1][2] * trans[1]
                            + mat[2][2] * trans[2] + parent->trans[2];

        count--;
        quats += 4;
        trans += 3;
        quatTrans++;
        parentList++;
    }

    /* The cod4rad XModelParts shape embeds the base-pose DSkel bits. */
    modelParts->skel_partBits.anim[0] = -1;
    modelParts->skel_partBits.anim[1] = -1;
    modelParts->skel_partBits.anim[2] = -1;
    modelParts->skel_partBits.anim[3] = -1;
    modelParts->skel_partBits.skel[0] = -1;
    modelParts->skel_partBits.skel[1] = -1;
    modelParts->skel_partBits.skel[2] = -1;
    modelParts->skel_partBits.skel[3] = -1;
}

void XModelReadCompressedQuat(const unsigned char **pos, short *quat)
{
    int q0;
    int q1;
    int q2;
    int q3sq;
    int q3;

    quat[0] = *(const short *)*pos; *pos += 2;
    quat[1] = *(const short *)*pos; *pos += 2;
    quat[2] = *(const short *)*pos; *pos += 2;

    q0 = quat[0];
    q1 = quat[1];
    q2 = quat[2];
    q3sq = 0x3FFF0001 - q0 * q0 - q1 * q1 - q2 * q2;
    if (q3sq > 0)
        q3 = (int)floorf(sqrtf((float)q3sq) + 0.5f);
    else
        q3 = 0;

    Assert(q3 == (short)q3, s_assertDisable_XModelReadCompressedQuat);
    quat[3] = (short)q3;
}

/*
================
XModelPartsLoadFile

v25 wire order:
  u16 version, u16 numChildBones, u16 numRootBones
  child records: byte parentIndex, float trans[3], s16 quat[3]
  bone-name C strings, byte partClassification[numBones], byte useBones
================
*/
XModelParts_t *XModelPartsLoadFile(XModel_t *model, const char *name,
                                   void *(*alloc)(int))
{
    char filename[64];
    unsigned char *buf;
    const unsigned char *pos;
    int fileSize;
    short version;
    int numChildBones;
    int numRootBones;
    int totalBones;
    unsigned short *boneNames;
    XBoneHierarchy_t *hierarchy;
    XModelParts_t *parts;
    short *quats;
    float *trans;
    int hierarchySize;
    int partsSize;
    int parentIdx;
    int useBones;
    int i;

    buf = NULL;
    if (Com_sprintf(filename, sizeof(filename), "xmodelparts/%s", name) < 0)
    {
        Com_Printf("^1ERROR: filename '%s' too long\n", filename);
        return NULL;
    }

    fileSize = FS_ReadFile(filename, (void **)&buf);
    if (fileSize < 0)
    {
        Assert(!buf, s_assertDisable_XModelPartsLoadFile_buf);
        Com_Printf("^1ERROR: xmodelparts '%s' not found\n", name);
        return NULL;
    }
    if (!fileSize)
    {
        Com_Printf("^1ERROR: xmodelparts '%s' has 0 length\n", name);
        FS_FreeFile(buf);
        return NULL;
    }

    Assert(buf, s_assertDisable_XModelPartsLoadFile_bufValid);
    pos = buf;
    version = *(const short *)pos;
    pos += 2;
    if (version != 25)
    {
        FS_FreeFile(buf);
        Com_Printf("^1ERROR: xmodelparts '%s' out of date (version %i, expecting %i)\n",
                   name, (int)version, 25);
        return NULL;
    }

    numChildBones = *(const unsigned short *)pos; pos += 2;
    numRootBones = *(const unsigned short *)pos; pos += 2;
    totalBones = numChildBones + numRootBones;
    if (totalBones >= DOBJ_MAX_PARTS)
    {
        FS_FreeFile(buf);
        Com_Printf("^1ERROR: xmodel '%s' has more than %d bones\n",
                   name, DOBJ_MAX_PARTS - 1);
        return NULL;
    }

    boneNames = (unsigned short *)alloc(totalBones * sizeof(unsigned short));
    model->memUsage += totalBones * sizeof(unsigned short);

    hierarchySize = sizeof(XBoneHierarchy_t);
    if (numChildBones > 1)
        hierarchySize += numChildBones - 1;
    hierarchy = (XBoneHierarchy_t *)alloc(hierarchySize);
    model->memUsage += hierarchySize;
    memset(hierarchy, 0, hierarchySize);
    hierarchy->names = boneNames;

    partsSize = 88 + totalBones * sizeof(DObjAnimMat_t);
    parts = (XModelParts_t *)alloc(partsSize);
    model->memUsage += partsSize;
    memset(parts, 0, partsSize);
    parts->hierarchy = hierarchy;

    if (numChildBones)
    {
        parts->quats = (short *)alloc(numChildBones * 8);
        model->memUsage += numChildBones * 8;
        parts->trans = (float *)alloc(numChildBones * 16);
        model->memUsage += numChildBones * 16;
    }

    parts->partClassification = (unsigned char *)alloc(totalBones);
    model->memUsage += totalBones;
    parts->numBones = (short)totalBones;
    parts->numRootBones = (short)numRootBones;

    quats = parts->quats;
    trans = parts->trans;
    for (i = numRootBones; i < totalBones; i++)
    {
        parentIdx = *pos++;
        Assert(parentIdx >= 0, s_assertDisable_XModelPartsLoadFile_index);
        Assert(parentIdx < i, s_assertDisable_XModelPartsLoadFile_indexLt);
        hierarchy->parentList[i - numRootBones] = (unsigned char)(i - parentIdx);
        Assert((i - parentIdx) == hierarchy->parentList[i - numRootBones],
               s_assertDisable_XModelPartsLoadFile_parentList);

        trans[0] = *(const float *)pos; pos += 4;
        trans[1] = *(const float *)pos; pos += 4;
        trans[2] = *(const float *)pos; pos += 4;
        XModelReadCompressedQuat(&pos, quats);

        quats += 4;
        trans += 3;
    }

    for (i = 0; i < totalBones; i++)
    {
        int len;

        len = (int)strlen((const char *)pos) + 1;
        boneNames[i] = (unsigned short)SL_GetStringOfLen(
            (const char *)pos, 0, len, 10);
        pos += len;
    }

    memcpy(parts->partClassification, pos, totalBones);
    pos += totalBones;
    useBones = *pos++ != 0;

    FS_FreeFile(buf);
    XModelCalcBasePose(parts);
    if (!useBones && numChildBones)
        memset(parts->trans, 0, numChildBones * 16);
    return parts;
}

/*
================
XModelReadCollSurfs

The v25 collision stream contains precomputed plane/svec/tvec records.  The
wire XModelCollSurf is 44 bytes on x86; cod4rad's widened pointer makes the
runtime structure 48 bytes without changing any serialized field.
================
*/
void XModelReadCollSurfs(const unsigned char **pos, XModel_t *model,
                         void *(*alloc)(int), const char *name)
{
    int numCollSurfs;
    int i;
    int j;

    Assert(!model->contents, s_assertDisable_XModelReadCollSurfs_contents);
    numCollSurfs = *(const int *)*pos;
    *pos += 4;
    model->numCollSurfs = numCollSurfs;

    if (!numCollSurfs)
    {
        Assert(!XMODEL_COLLSURFS(model),
               s_assertDisable_XModelReadCollSurfs_noSurfs);
        return;
    }

    XMODEL_COLLSURFS(model) = (XModelCollSurf_t *)alloc(
        numCollSurfs * sizeof(XModelCollSurf_t));
    memset(XMODEL_COLLSURFS(model), 0,
           numCollSurfs * sizeof(XModelCollSurf_t));

    for (i = 0; i < numCollSurfs; i++)
    {
        XModelCollSurf_t *surf;

        surf = &XMODEL_COLLSURFS(model)[i];
        surf->numCollTris = *(const int *)*pos;
        *pos += 4;
        Assert(surf->numCollTris, s_assertDisable_XModelReadCollSurfs_tris);
        surf->collTris = (XModelCollTri_t *)alloc(
            surf->numCollTris * sizeof(XModelCollTri_t));

        for (j = 0; j < surf->numCollTris; j++)
        {
            XModelCollTri_t *tri;
            float lenSq;

            tri = &surf->collTris[j];
            tri->plane[0] = *(const float *)*pos; *pos += 4;
            tri->plane[1] = *(const float *)*pos; *pos += 4;
            tri->plane[2] = *(const float *)*pos; *pos += 4;
            tri->plane[3] = *(const float *)*pos; *pos += 4;

            lenSq = tri->plane[0] * tri->plane[0]
                  + tri->plane[1] * tri->plane[1]
                  + tri->plane[2] * tri->plane[2];
            Assert(fabsf(sqrtf(lenSq) - 1.0f)
                       < XMODEL_COLL_NORMAL_EPSILON,
                   s_assertDisable_XModelReadCollSurfs_normal);

            tri->svec[0] = *(const float *)*pos; *pos += 4;
            tri->svec[1] = *(const float *)*pos; *pos += 4;
            tri->svec[2] = *(const float *)*pos; *pos += 4;
            tri->svec[3] = *(const float *)*pos; *pos += 4;
            tri->tvec[0] = *(const float *)*pos; *pos += 4;
            tri->tvec[1] = *(const float *)*pos; *pos += 4;
            tri->tvec[2] = *(const float *)*pos; *pos += 4;
            tri->tvec[3] = *(const float *)*pos; *pos += 4;
        }

        surf->mins[0] = *(const float *)*pos - XMODEL_COLL_BOUNDS_EPSILON; *pos += 4;
        surf->mins[1] = *(const float *)*pos - XMODEL_COLL_BOUNDS_EPSILON; *pos += 4;
        surf->mins[2] = *(const float *)*pos - XMODEL_COLL_BOUNDS_EPSILON; *pos += 4;
        surf->maxs[0] = *(const float *)*pos + XMODEL_COLL_BOUNDS_EPSILON; *pos += 4;
        surf->maxs[1] = *(const float *)*pos + XMODEL_COLL_BOUNDS_EPSILON; *pos += 4;
        surf->maxs[2] = *(const float *)*pos + XMODEL_COLL_BOUNDS_EPSILON; *pos += 4;
        surf->boneIdx = *(const int *)*pos; *pos += 4;
        surf->contents = *(const int *)*pos & 0xDFFFFFFB; *pos += 4;
        Assert(!surf->contents || surf->boneIdx >= 0,
               s_assertDisable_XModelReadCollSurfs_boneIdx);
        surf->surfFlags = *(const int *)*pos; *pos += 4;
        model->contents |= surf->contents;
    }

    (void)name;
}

void R_XModelSurfsReadData(XModel_t *model, const char *surfFilename,
                           XSurface_t **surfsArray, int *partBits,
                           int numsurfs, const unsigned char **pos,
                           void *(*alloc)(int))
{
    int i;

    Assert(surfsArray, s_assertDisable_R_XModelSurfsReadData_surfs);
    Assert(partBits, s_assertDisable_R_XModelSurfsReadData_bits);
    Assert(numsurfs > 0, s_assertDisable_R_XModelSurfsReadData_count);
    Assert(pos, s_assertDisable_R_XModelSurfsReadData_pos);
    Assert(*pos, s_assertDisable_R_XModelSurfsReadData_posData);

    for (i = 0; i < numsurfs; i++)
    {
        surfsArray[i] = R_XSurfaceLoadObj(model, partBits, pos, alloc);
        if (!surfsArray[i])
            break;
    }

    (void)surfFilename;
}

XModelSurfs_t *XModelSurfsLoadFile(XModel_t *model,
                                   const char *surfFilename,
                                   void *(*alloc)(int), int numsurfs,
                                   const char *modelName)
{
    char filename[64];
    unsigned char *buf;
    const unsigned char *pos;
    int fileSize;
    short version;
    short fileNumSurfs;
    int allocSize;
    int i;
    XModelSurfs_t *surfs;

    buf = NULL;
    if (Com_sprintf(filename, sizeof(filename), "xmodelsurfs/%s",
                    surfFilename) < 0)
    {
        Com_Printf("^1ERROR: filename '%s' too long\n", filename);
        return NULL;
    }

    fileSize = FS_ReadFile(filename, (void **)&buf);
    if (fileSize < 0)
    {
        Assert(!buf, s_assertDisable_XModelSurfsLoadFile_buf);
        Com_Printf("^1ERROR: xmodelsurf '%s' not found\n", surfFilename);
        return NULL;
    }
    if (!fileSize)
    {
        Com_Printf("^1ERROR: xmodelsurf '%s' has 0 length\n", surfFilename);
        FS_FreeFile(buf);
        return NULL;
    }

    Assert(buf, s_assertDisable_XModelSurfsLoadFile_bufValid);
    pos = buf;
    version = *(const short *)pos; pos += 2;
    if (version != 25)
    {
        FS_FreeFile(buf);
        Com_Printf("^1ERROR: xmodelsurfs '%s' out of date (version %i, expecting %i)\n",
                   surfFilename, (int)version, 25);
        return NULL;
    }

    fileNumSurfs = *(const short *)pos; pos += 2;
    if (fileNumSurfs != numsurfs)
    {
        FS_FreeFile(buf);
        Com_Printf("^1ERROR: File conflict (between non-iwd and iwd) for xmodelsurfs '%s' (model '%s')\n",
                   surfFilename, modelName);
        return NULL;
    }

    allocSize = sizeof(XModelSurfs_t)
              + numsurfs * sizeof(XSurface_t *);
    surfs = (XModelSurfs_t *)alloc(allocSize);
    model->memUsage += allocSize;
    memset(surfs, 0, allocSize);
    surfs->surfs = (XSurface_t **)(surfs + 1);

    R_XModelSurfsReadData(model, surfFilename, surfs->surfs,
                          surfs->partBits, numsurfs, &pos, alloc);
    for (i = 0; i < numsurfs; i++)
    {
        if (!surfs->surfs[i])
        {
            FS_FreeFile(buf);
            return NULL;
        }
    }

    FS_FreeFile(buf);
    return surfs;
}

static XModelSurfs_t *XModelSurfsPrecacheData(
    XModel_t *model, const char *filename, void *(*alloc)(int),
    int numsurfs, const char *modelName)
{
    XModelSurfs_t *surfs;

    surfs = XModelSurfsFindData(filename);
    if (surfs)
        return surfs;

    surfs = XModelSurfsLoadFile(model, filename, alloc, numsurfs,
                                modelName);
    if (!surfs)
    {
        Com_Printf("^1ERROR: Cannot find xmodelsurfs '%s'.\n", filename);
        return NULL;
    }

    XModelSurfsSetData(filename, surfs, alloc);
    return surfs;
}

static int XModelReadConfigString(const char *modelName,
                                  const unsigned char **pos,
                                  char *out, int outSize,
                                  const char *fieldName)
{
    int len;

    len = (int)strlen((const char *)*pos) + 1;
    if (len > outSize)
    {
        Com_Printf("^1ERROR: xmodel '%s' %s is too long\n",
                   modelName, fieldName);
        return 0;
    }

    memcpy(out, *pos, len);
    *pos += len;
    return 1;
}

static int XModelSetBoundsFromLod0Surfaces(XModel_t *model)
{
    XModelSurfs_t *modelSurfs;
    int haveBounds;
    int surfIndex;

    modelSurfs = model->lodInfo[0].modelSurfs;
    haveBounds = 0;
    for (surfIndex = 0; surfIndex < model->lodInfo[0].numsurfs;
         surfIndex++)
    {
        XSurface_t *surface;
        const unsigned char *vertCursor;
        int vertIndex;

        surface = modelSurfs->surfs[surfIndex];
        vertCursor = (const unsigned char *)surface->verts;
        for (vertIndex = 0; vertIndex < surface->vertCount; vertIndex++)
        {
            const XSurfaceTempVert_t *vert;
            int axis;

            vert = (const XSurfaceTempVert_t *)vertCursor;
            if (!haveBounds)
            {
                model->mins[0] = vert->pos[0];
                model->mins[1] = vert->pos[1];
                model->mins[2] = vert->pos[2];
                model->maxs[0] = vert->pos[0];
                model->maxs[1] = vert->pos[1];
                model->maxs[2] = vert->pos[2];
                haveBounds = 1;
            }
            else
            {
                for (axis = 0; axis < 3; axis++)
                {
                    if (vert->pos[axis] < model->mins[axis])
                        model->mins[axis] = vert->pos[axis];
                    if (vert->pos[axis] > model->maxs[axis])
                        model->maxs[axis] = vert->pos[axis];
                }
            }

            vertCursor += sizeof(XSurfaceTempVert_t)
                        + vert->numWeights
                        * sizeof(XSurfaceBlendEntry_t);
        }
    }

    return haveBounds;
}

/*
================
XModel_ReadHeader

CoD4 v25 wire order:
  u16 version, byte flags, float mins[3], float maxs[3]
  physicsPreset C string
  four { float distance, xmodelsurfs filename C string }
  s32 collLod
================
*/
int XModel_ReadHeader(const char *name, const unsigned char **pos,
                      XModelConfig_t *config)
{
    short version;
    int i;

    memset(config, 0, sizeof(*config));
    version = *(const short *)*pos;
    *pos += 2;
    if (version != 25)
    {
        Com_Printf("^1ERROR: xmodel '%s' out of date (version %i, expecting %i)\n",
                   name, (int)version, 25);
        return 0;
    }

    config->flags = **pos; *pos += 1;
    config->mins[0] = *(const float *)*pos; *pos += 4;
    config->mins[1] = *(const float *)*pos; *pos += 4;
    config->mins[2] = *(const float *)*pos; *pos += 4;
    config->maxs[0] = *(const float *)*pos; *pos += 4;
    config->maxs[1] = *(const float *)*pos; *pos += 4;
    config->maxs[2] = *(const float *)*pos; *pos += 4;

    if (!XModelReadConfigString(name, pos,
                                config->physicsPresetFilename,
                                sizeof(config->physicsPresetFilename),
                                "physics preset name"))
        return 0;

    for (i = 0; i < 4; i++)
    {
        config->entries[i].dist = *(const float *)*pos;
        *pos += 4;
        if (!XModelReadConfigString(name, pos,
                                    config->entries[i].filename,
                                    sizeof(config->entries[i].filename),
                                    "LOD filename"))
            return 0;
    }

    config->collLod = *(const int *)*pos;
    *pos += 4;
    return 1;
}

/*
================
XModelLoadFile

The main stream is consumed in the retail v25 order: config, collision data,
a first pass over LOD material names, parts, per-bone bounds, then a second
LOD pass that precaches surfaces and interns the material-name strings.
================
*/
XModel_t *XModelLoadFile(const char *name, void *(*alloc)(int),
                         void *(*allocColl)(int))
{
    char filename[64];
    unsigned char *buf;
    const unsigned char *pos;
    const unsigned char *lodDataPos;
    int fileSize;
    XModelConfig_t config;
    int lodStringLens[4];
    int totalStringLen;
    int allocSize;
    XModel_t *model;
    XModelParts_t *parts;
    XBoneInfo_t *boneInfo;
    int numBones;
    int i;
    int j;

    buf = NULL;
    if (Com_sprintf(filename, sizeof(filename), "xmodel/%s", name) < 0)
    {
        Com_Printf("^1ERROR: filename '%s' too long\n", filename);
        return NULL;
    }

    fileSize = FS_ReadFile(filename, (void **)&buf);
    if (fileSize < 0)
    {
        Assert(!buf, s_assertDisable_XModelLoadFile_buf);
        Com_Printf("^1ERROR: xmodel '%s' not found\n", name);
        return NULL;
    }
    if (!fileSize)
    {
        Com_Printf("^1ERROR: xmodel '%s' has 0 length\n", name);
        FS_FreeFile(buf);
        return NULL;
    }

    pos = buf;
    if (!XModel_ReadHeader(name, &pos, &config))
    {
        FS_FreeFile(buf);
        return NULL;
    }

    totalStringLen = 0;
    for (i = 0; i < 4; i++)
    {
        lodStringLens[i] = (int)strlen(config.entries[i].filename) + 1;
        totalStringLen += lodStringLens[i];
    }

    allocSize = sizeof(XModel_t) + totalStringLen;
    model = (XModel_t *)alloc(allocSize);
    memset(model, 0, allocSize);
    model->memUsage = allocSize;

    {
        char *stringDst;

        stringDst = (char *)(model + 1);
        for (i = 0; i < 4; i++)
        {
            memcpy(stringDst, config.entries[i].filename,
                   lodStringLens[i]);
            model->lodInfo[i].filename = stringDst;
            stringDst += lodStringLens[i];
        }
    }

    XModelReadCollSurfs(&pos, model, allocColl, name);

    lodDataPos = pos;
    model->numLods = 0;
    for (i = 0; i < 4; i++)
    {
        if (config.entries[i].filename[0])
        {
            Assert(i == model->numLods,
                   s_assertDisable_XModelLoadFile_numLods);
            model->numLods++;
            model->lodInfo[i].numsurfs =
                *(const unsigned short *)pos;
            pos += 2;
            for (j = 0; j < model->lodInfo[i].numsurfs; j++)
                pos += strlen((const char *)pos) + 1;
        }
    }
    Assert(model->numLods, s_assertDisable_XModelLoadFile_numLods);

    parts = XModelPartsFindData(config.entries[0].filename);
    if (!parts)
    {
        parts = XModelPartsLoadFile(model, config.entries[0].filename,
                                    alloc);
        if (parts)
            XModelPartsSetData(config.entries[0].filename, parts, alloc);
    }
    if (!parts)
    {
        Com_Printf("^1ERROR: Cannot find xmodelparts '%s'.\n",
                   config.entries[0].filename);
        FS_FreeFile(buf);
        XModelFree(model);
        return NULL;
    }
    model->parts = parts;

    numBones = parts->numBones;
    boneInfo = (XBoneInfo_t *)alloc(numBones * sizeof(XBoneInfo_t));
    model->memUsage += numBones * sizeof(XBoneInfo_t);
    for (i = 0; i < numBones; i++)
    {
        float dx;
        float dy;
        float dz;

        boneInfo[i].bounds[0][0] = *(const float *)pos; pos += 4;
        boneInfo[i].bounds[0][1] = *(const float *)pos; pos += 4;
        boneInfo[i].bounds[0][2] = *(const float *)pos; pos += 4;
        boneInfo[i].bounds[1][0] = *(const float *)pos; pos += 4;
        boneInfo[i].bounds[1][1] = *(const float *)pos; pos += 4;
        boneInfo[i].bounds[1][2] = *(const float *)pos; pos += 4;

        boneInfo[i].offset[0] =
            (boneInfo[i].bounds[0][0] + boneInfo[i].bounds[1][0]) * 0.5f;
        boneInfo[i].offset[1] =
            (boneInfo[i].bounds[0][1] + boneInfo[i].bounds[1][1]) * 0.5f;
        boneInfo[i].offset[2] =
            (boneInfo[i].bounds[0][2] + boneInfo[i].bounds[1][2]) * 0.5f;
        dx = boneInfo[i].bounds[1][0] - boneInfo[i].offset[0];
        dy = boneInfo[i].bounds[1][1] - boneInfo[i].offset[1];
        dz = boneInfo[i].bounds[1][2] - boneInfo[i].offset[2];
        boneInfo[i].radiusSquared = dx * dx + dy * dy + dz * dz;
    }
    model->boneInfo = boneInfo;

    pos = lodDataPos;
    for (i = 0; i < 4; i++)
    {
        XModelSurfs_t *surfs;
        int fileNumSurfs;
        int numSurfs;

        if (!config.entries[i].filename[0])
            continue;

        fileNumSurfs = *(const unsigned short *)pos;
        pos += 2;
        if (fileNumSurfs != model->lodInfo[i].numsurfs)
        {
            Com_Printf("^1ERROR: xmodel '%s' LOD surface count changed while loading\n",
                       name);
            FS_FreeFile(buf);
            XModelFree(model);
            return NULL;
        }

        surfs = XModelSurfsPrecacheData(
            model, config.entries[i].filename, alloc,
            model->lodInfo[i].numsurfs, name);
        if (!surfs)
        {
            FS_FreeFile(buf);
            XModelFree(model);
            return NULL;
        }
        model->lodInfo[i].modelSurfs = surfs;

        numSurfs = model->lodInfo[i].numsurfs;
        if (numSurfs)
        {
            model->lodInfo[i].surfNames = (unsigned short *)alloc(
                numSurfs * sizeof(unsigned short));
            model->memUsage += numSurfs * sizeof(unsigned short);
        }

        for (j = 0; j < numSurfs; j++)
        {
            const char *wireName;
            const char *materialName;
            int wireLen;
            int materialLen;

            wireName = (const char *)pos;
            wireLen = (int)strlen(wireName) + 1;
            pos += wireLen;
            materialName = !strcmp(wireName, "$default")
                         ? "$default3d" : wireName;
            materialLen = (int)strlen(materialName) + 1;
            model->lodInfo[i].surfNames[j] = (unsigned short)
                SL_GetStringOfLen(materialName, 0, materialLen, 8);
        }
    }

    if (!XModelSetBoundsFromLod0Surfaces(model))
    {
        Com_Printf("^1ERROR: xmodel '%s' has no LOD0 vertices\n", name);
        FS_FreeFile(buf);
        XModelFree(model);
        return NULL;
    }

    FS_FreeFile(buf);

    Assert(config.collLod == (short)config.collLod,
           s_assertDisable_XModelLoadFile_collLod);
    model->collLod = (short)config.collLod;
    Assert(model->collLod < model->numLods,
           s_assertDisable_XModelLoadFile_collLod);
    model->flags = config.flags;

    /* The game uses config.maxs[0] as model radius after recomputing the AABB
     * from LOD0 vertices.  cod4rad has no radius, PhysPreset, or PhysGeom
     * consumers; the config fields were still consumed to preserve alignment. */
    return model;
}

int XModelSurfsLoad(XModel_t *model, void *(*alloc)(int))
{
    int i;

    Assert(model, s_assertDisable_XModelSurfsLoad);
    Assert(model->lodInfo[0].filename[0],
           s_assertDisable_XModelSurfsLoad_filename);

    for (i = 0; i < model->numLods; i++)
    {
        XModelSurfs_t *surfs;

        if (model->lodInfo[i].modelSurfs)
            continue;

        surfs = XModelSurfsPrecacheData(
            model, model->lodInfo[i].filename, alloc,
            model->lodInfo[i].numsurfs,
            model->name ? model->name : model->lodInfo[i].filename);
        if (!surfs)
            return 0;
        model->lodInfo[i].modelSurfs = surfs;
    }

    return 1;
}
