/*
 * xmodel_load_obj.c — XModel file loading and parsing.
 *
 * Source: ..\src\xanim\xmodel_load_obj.cpp
 * Reconstructed from decompiled output, LST, and CoD2 server source.
 */

#include "cod2rad64.h"

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
 * XModelLodInfo layout (x64, 40 bytes, confirmed from LST):
 *   +0x00  const char *filename      (8 bytes)
 *   +0x08  short numsurfs            (2 bytes + padding)
 *   +0x10  unsigned short *surfNames (8 bytes)
 *   +0x18  XModelSurfs_t *modelSurfs   (8 bytes)
 *   +0x20  other fields              (8 bytes)
 *
 * lodInfo[0] starts at XModel + 0x10, stride 0x28 (40 bytes).
 * Up to 4 LOD levels (0..3).
 *
 * Hunk data types used for surfs caching: type 2.
 */


/*
================
XModelCalcBasePose

Computes the base bone matrices for a model's parts.
Root bones get identity quaternions. Non-root bones get
quaternions from compressed shorts multiplied into parent,
then MatrixTransformVectorQuatTrans for translation.
After computing all bones, sets anim and skel partBits to -1.

Parts skel layout (from LST):
  parts+0x28: partBits.anim[4]  (16 bytes, set to 0xFF)
  parts+0x38: partBits.control[4] (16 bytes, NOT touched)
  parts+0x48: partBits.skel[4]  (16 bytes, set to 0xFF)
  parts+0x58: mat[0]            (DObjAnimMat array)

================
*/
void XModelCalcBasePose(XModelParts_t *modelParts)
{
    static const float SHORT_TO_QUAT = 3.0518509e-5f; /* 0x38000100 */

    unsigned char *parentList;
    DObjAnimMat_t *quatTrans;
    int numBones;
    float *trans;
    short *quats;
    int numRootBones;
    float vLenSq;
    float tempQuat[4];

    parentList = modelParts->hierarchy->parentList;
    numBones = modelParts->numBones;
    quats = modelParts->quats;
    trans = modelParts->trans;
    quatTrans = &modelParts->skel_mat[0];
    numRootBones = modelParts->numRootBones;

    /* root bones: identity quaternion */
    while (numRootBones)
    {
        quatTrans->quat[0] = 0.0f;
        quatTrans->quat[1] = 0.0f;
        quatTrans->quat[2] = 0.0f;
        quatTrans->quat[3] = 1.0f;
        quatTrans->trans[0] = 0.0f;
        quatTrans->trans[1] = 0.0f;
        quatTrans->trans[2] = 0.0f;
        quatTrans->transWeight = 2.0f;
        numRootBones--;
        quatTrans++;
    }

    /* non-root bones: decompress quats and apply hierarchy */
    numRootBones = numBones - modelParts->numRootBones;

    while (numRootBones)
    {
        tempQuat[0] = (float)quats[0] * SHORT_TO_QUAT;
        tempQuat[1] = (float)quats[1] * SHORT_TO_QUAT;
        tempQuat[2] = (float)quats[2] * SHORT_TO_QUAT;
        tempQuat[3] = (float)quats[3] * SHORT_TO_QUAT;

        QuatMultiply(tempQuat, quatTrans[-*parentList].quat, quatTrans->quat);

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

        /* binary builds 3x3 rotation matrix from parent quat, then does mat*vec + parent.trans */
        {
            extern void DObjAnimMatToAxis(void *animMat, float *outMat); /* xanim_public_41B080 */
            float mat[9];
            DObjAnimMat_t *parent = &quatTrans[-(int)*parentList];
            DObjAnimMatToAxis(parent, mat);
            quatTrans->trans[0] = mat[0]*trans[0] + mat[3]*trans[1] + mat[6]*trans[2] + parent->trans[0];
            quatTrans->trans[1] = mat[1]*trans[0] + mat[4]*trans[1] + mat[7]*trans[2] + parent->trans[1];
            quatTrans->trans[2] = mat[2]*trans[0] + mat[5]*trans[1] + mat[8]*trans[2] + parent->trans[2];
        }

        numRootBones--;
        quats += 4;
        trans += 3;
        quatTrans++;
        parentList++;
    }

    /* mark all bones as computed in skel partBits */
    modelParts->skel_partBits.anim[0] = -1;
    modelParts->skel_partBits.anim[1] = -1;
    modelParts->skel_partBits.anim[2] = -1;
    modelParts->skel_partBits.anim[3] = -1;
    modelParts->skel_partBits.skel[0] = -1;
    modelParts->skel_partBits.skel[1] = -1;
    modelParts->skel_partBits.skel[2] = -1;
    modelParts->skel_partBits.skel[3] = -1;
}

/*
================
XModelReadCompressedQuat

Reads 3 compressed quaternion shorts from a byte stream,
computes the 4th component via unit quaternion constraint:
  q3 = sqrt(32767² - q0² - q1² - q2²)

The constant 0x3FFF0001 = 32767² = 1073676289.

================
*/
void XModelReadCompressedQuat(const unsigned char **pos, short *quat)
{
    int q0, q1, q2;
    int q3sq;
    int q3;

    quat[0] = *(short *)*pos;  *pos += 2;
    quat[1] = *(short *)*pos;  *pos += 2;
    quat[2] = *(short *)*pos;  *pos += 2;

    q0 = (int)quat[0];
    q1 = (int)quat[1];
    q2 = (int)quat[2];

    q3sq = 0x3FFF0001 - q0 * q0 - q1 * q1 - q2 * q2;

    if (q3sq > 0)
    {
        q3 = (int)floorf(sqrtf((float)q3sq) + 0.5f);
    }
    else
    {
        q3 = 0;
    }

    Assert(q3 == (short)q3, s_assertDisable_XModelReadCompressedQuat);

    quat[3] = (short)q3;
}

/*
================
XModelPartsLoadFile

Reads an xmodelparts file from disk. Parses bone hierarchy,
compressed quaternions, translations, and bone names.

File format (after version short == 20):
  +0x00  short numChildBones (non-root)
  +0x02  short numRootBones
  For each non-root bone (numChildBones entries):
    1 byte parent index
    3 floats trans (12 bytes)
    3 shorts compressed quat (6 bytes, 4th computed)
  For each bone (totalBones entries):
    null-terminated name string
  byte[totalBones] partClassification data

Allocations:
  bone names:      totalBones * 2 bytes (unsigned short array)
  hierarchy:       numChildBones + 15 bytes (ptr + parentList + pad)
  parts:           totalBones * 32 + 88 bytes (XModelParts_s + skel)
  quats:           numChildBones * 8 bytes (4 shorts per bone)
  trans:           numChildBones * 16 bytes (3 floats + pad per bone)
  partClassify:    totalBones bytes

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
    int parentIdx;
    int i;

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

    if (fileSize == 0)
    {
        Com_Printf("^1ERROR: xmodelparts '%s' has 0 length\n", name);
        FS_FreeFile(buf);
        return NULL;
    }

    Assert(buf, s_assertDisable_XModelPartsLoadFile_bufValid);
    pos = buf;

    /* check version */
    version = *(short *)pos;
    pos += 2;

    if (version != 20)
    {
        FS_FreeFile(buf);
        Com_Printf("^1ERROR: xmodelparts '%s' out of date (version %i, expecting %i)\n",
                    name, (int)version, 20);
        return NULL;
    }

    /* read bone counts */
    numChildBones = *(unsigned short *)pos;
    numRootBones = *(unsigned short *)(pos + 2);
    pos += 4;
    totalBones = numChildBones + numRootBones;

    /* allocate bone names array */
    boneNames = (unsigned short *)alloc(totalBones * 2);
    model->memUsage += totalBones * 2;

    if (totalBones >= DOBJ_MAX_PARTS)
    {
        FS_FreeFile(buf);
        Com_Printf("^1ERROR: xmodel '%s' has more than %d bones\n", name, DOBJ_MAX_PARTS - 1);
        return NULL;
    }

    /* allocate hierarchy: ptr(8) + parentList + padding */
    hierarchy = (XBoneHierarchy_t *)alloc(numChildBones + 15);
    model->memUsage += numChildBones + 15;
    hierarchy->names = boneNames;

    /* allocate parts with embedded skel: header(88) + mat[totalBones](totalBones*32) */
    parts = (XModelParts_t *)alloc(totalBones * 32 + 88);
    model->memUsage += totalBones * 32 + 88;
    parts->hierarchy = hierarchy;

    /* allocate quats and trans if there are non-root bones */
    if (numChildBones > 0)
    {
        parts->quats = (short *)alloc(numChildBones * 8);
        model->memUsage += numChildBones * 8;
        parts->trans = (float *)alloc(numChildBones * 16);
        model->memUsage += numChildBones * 16;
    }
    else
    {
        parts->quats = NULL;
        parts->trans = NULL;
    }

    /* allocate part classification */
    parts->partClassification = (unsigned char *)alloc(totalBones);
    model->memUsage += totalBones;

    /* store bone counts */
    parts->numBones = (short)totalBones;
    parts->numRootBones = (short)numRootBones;

    /* read non-root bone data */
    quats = parts->quats;
    trans = parts->trans;

    for (i = numRootBones; i < totalBones; i++)
    {
        /* read parent index (1 byte) */
        parentIdx = *pos;
        Assert(parentIdx >= 0, s_assertDisable_XModelPartsLoadFile_index);
        Assert(parentIdx < i, s_assertDisable_XModelPartsLoadFile_indexLt);

        hierarchy->parentList[i - numRootBones] = (unsigned char)(i - parentIdx);
        Assert((i - parentIdx) == hierarchy->parentList[i - numRootBones],
               s_assertDisable_XModelPartsLoadFile_parentList);

        /* read trans (3 floats, 12 bytes at pos+1) */
        trans[0] = *(float *)(pos + 1);
        trans[1] = *(float *)(pos + 5);
        trans[2] = *(float *)(pos + 9);
        pos += 13;

        /* read compressed quat (3 shorts, computes 4th) */
        XModelReadCompressedQuat(&pos, quats);

        quats += 4;
        trans += 3;
    }

    /* read bone names */
    for (i = 0; i < totalBones; i++)
    {
        int len = (int)strlen((const char *)pos) + 1;
        boneNames[i] = SL_GetStringOfLen((const char *)pos, 0, len, 10);
        pos += len;
    }

    /* copy part classification data */
    memcpy(parts->partClassification, pos, totalBones);

    /* clean up and compute base pose */
    FS_FreeFile(buf);
    XModelCalcBasePose(parts);

    return parts;
}

/*
================
XModelReadCollSurfs

Reads collision surface data from the model file stream.
Parses collision triangles (plane normals, svec, tvec) and
surface bounds (mins/maxs with margin), boneIdx, contents.

XModelCollSurf_s (48 bytes = 0x30):
  +0x00  XModelCollTri_t *collTris
  +0x08  int numCollTris
  +0x0C  float mins[3]       (file value - margin)
  +0x18  float maxs[3]       (file value + margin)
  +0x24  int boneIdx
  +0x28  int contents         (masked with 0xDFFFFFFB)
  +0x2C  int surfFlags

XModelCollTri_s (48 bytes = 0x30):
  +0x00  float plane[4]      (normal xyz + distance)
  +0x10  float svec[4]       (barycentric s vector + offset)
  +0x20  float tvec[4]       (barycentric t vector + offset)

================
*/
void XModelReadCollSurfs(const unsigned char **pos, XModel_t *model,
                          void *(*alloc)(int), const char *name)
{
    int numCollSurfs;
    int numCollTris;
    int i;
    int j;
    float n0, n1, n2, lenSq;
    XModelCollSurf_t *surf;
    XModelCollTri_t *tri;

    Assert(!model->contents, s_assertDisable_XModelReadCollSurfs_contents);

    /* read numCollSurfs */
    numCollSurfs = *(int *)*pos;
    *pos += 4;
    model->numCollSurfs = numCollSurfs;

    if (numCollSurfs == 0)
    {
        Assert(!XMODEL_COLLSURFS(model), s_assertDisable_XModelReadCollSurfs_noSurfs);
        return;
    }

    /* allocate collision surfaces array */
    XMODEL_COLLSURFS(model) = (XModelCollSurf_t *)alloc(numCollSurfs * sizeof(XModelCollSurf_t));

    for (i = 0; i < numCollSurfs; i++)
    {
        surf = &XMODEL_COLLSURFS(model)[i];

        /* read numCollTris */
        surf->numCollTris = *(int *)*pos;
        *pos += 4;
        Assert(surf->numCollTris, s_assertDisable_XModelReadCollSurfs_tris);

        /* allocate triangles */
        surf->collTris = (XModelCollTri_t *)alloc(surf->numCollTris * sizeof(XModelCollTri_t));

        /* read each collision triangle */
        for (j = 0; j < surf->numCollTris; j++)
        {
            tri = &surf->collTris[j];

            /* read plane normal and distance */
            tri->plane[0] = *(float *)*pos; *pos += 4;
            tri->plane[1] = *(float *)*pos; *pos += 4;
            tri->plane[2] = *(float *)*pos; *pos += 4;
            tri->plane[3] = *(float *)*pos; *pos += 4;

            /* assert plane normal is approximately unit length */
            n0 = tri->plane[0];
            n1 = tri->plane[1];
            n2 = tri->plane[2];
            lenSq = n0 * n0 + n1 * n1 + n2 * n2;
            Assert(fabsf(sqrtf(lenSq) - 1.0f) < COLL_NORMAL_EPSILON,
                   s_assertDisable_XModelReadCollSurfs_normal);

            /* read svec (4 floats) */
            tri->svec[0] = *(float *)*pos; *pos += 4;
            tri->svec[1] = *(float *)*pos; *pos += 4;
            tri->svec[2] = *(float *)*pos; *pos += 4;
            tri->svec[3] = *(float *)*pos; *pos += 4;

            /* read tvec (4 floats) */
            tri->tvec[0] = *(float *)*pos; *pos += 4;
            tri->tvec[1] = *(float *)*pos; *pos += 4;
            tri->tvec[2] = *(float *)*pos; *pos += 4;
            tri->tvec[3] = *(float *)*pos; *pos += 4;
        }

        /* read surface bounds (with margin adjustment) */
        surf->mins[0] = *(float *)*pos - COLL_BOUNDS_MARGIN; *pos += 4;
        surf->mins[1] = *(float *)*pos - COLL_BOUNDS_MARGIN; *pos += 4;
        surf->mins[2] = *(float *)*pos - COLL_BOUNDS_MARGIN; *pos += 4;
        surf->maxs[0] = *(float *)*pos + COLL_BOUNDS_MARGIN; *pos += 4;
        surf->maxs[1] = *(float *)*pos + COLL_BOUNDS_MARGIN; *pos += 4;
        surf->maxs[2] = *(float *)*pos + COLL_BOUNDS_MARGIN; *pos += 4;

        /* read boneIdx */
        surf->boneIdx = *(int *)*pos;
        *pos += 4;

        /* read contents (masked) */
        surf->contents = *(int *)*pos & 0xDFFFFFFB;
        *pos += 4;

        Assert(!surf->contents || surf->boneIdx >= 0,
               s_assertDisable_XModelReadCollSurfs_boneIdx);

        /* read surfFlags */
        surf->surfFlags = *(int *)*pos;
        *pos += 4;

        /* accumulate contents into model */
        model->contents |= surf->contents;
    }
}

/*
================
R_XModelSurfsReadData

Reads surface data from the file buffer for each surface.
Calls R_XSurfaceLoadObj for each surface in a loop,
storing the returned XSurface pointers in the surfsArray.

Params (7, from LST caller analysis):
  model        — the XModel being loaded
  surfFilename — surface filename (unused in body, for API compat)
  surfsArray   — output: array of XSurface pointers (numsurfs entries)
  partBits     — output: bone bits (passed through to each surface load)
  numsurfs     — number of surfaces to read
  pos          — pointer to current read position in file buffer
  alloc        — allocation callback

================
*/
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
    }
}

/*
================
XModelSurfsLoadFile

Reads an xmodelsurfs file from disk, validates version
and surface count, allocates an XModelSurfs struct, and
fills it by calling R_XModelSurfsReadData.

File format:
  +0x00  short version     (must be 20 = 0x14)
  +0x02  short numsurfs    (must match expected)
  +0x04  surface data...

XModelSurfs allocation: numsurfs * 8 + 24 bytes
  +0x00  XSurface_t *surfs   (→ points to inline data at +24)
  +0x08  partBits area     (16 bytes)
  +0x18  XSurface ptrs[]   (numsurfs * 8 bytes, inline)

================
*/
XModelSurfs_t *XModelSurfsLoadFile(XModel_t *model, const char *surfFilename,
                                  void *(*alloc)(int), int numsurfs,
                                  const char *modelName)
{
    char filename[64];
    unsigned char *buf;
    int fileSize;
    const unsigned char *pos;
    short version;
    short fileNumSurfs;
    int allocSize;
    XModelSurfs_t *surfs;

    if (Com_sprintf(filename, sizeof(filename), "xmodelsurfs/%s", surfFilename) < 0)
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

    if (fileSize == 0)
    {
        Com_Printf("^1ERROR: xmodelsurf '%s' has 0 length\n", surfFilename);
        FS_FreeFile(buf);
        return NULL;
    }

    Assert(buf, s_assertDisable_XModelSurfsLoadFile_bufValid);

    pos = buf;

    /* check version */
    version = *(short *)pos;
    pos += 2;

    if (version != 20)
    {
        FS_FreeFile(buf);
        Com_Printf("^1ERROR: xmodelsurfs '%s' out of date (version %i, expecting %i)\n",
                    surfFilename, (int)version, 20);
        return NULL;
    }

    /* check surface count */
    fileNumSurfs = *(short *)pos;
    pos += 2;

    if (fileNumSurfs != numsurfs)
    {
        FS_FreeFile(buf);
        Com_Printf("^1ERROR: File conflict (between non-iwd and iwd) for xmodelsurfs '%s' (model '%s')\n",
                    surfFilename, modelName);
        return NULL;
    }

    /* allocate XModelSurfs: header (24 bytes) + surface pointers (numsurfs * 8) */
    allocSize = numsurfs * 8 + 24;
    surfs = (XModelSurfs_t *)alloc(allocSize);
    model->memUsage += allocSize;

    /* set surfs pointer to inline data at offset 24 (right after the struct header) */
    surfs->surfs = (XSurface_t **)(surfs + 1);

    /* parse surface data from file */
    R_XModelSurfsReadData(model, surfFilename,
                          surfs->surfs,
                          surfs->partBits,
                          numsurfs, &pos, alloc);

    FS_FreeFile(buf);
    return surfs;
}

/*
================
XModelLoadFile

Reads the main xmodel file, allocates the XModel struct,
parses collision surfaces, loads parts (bones), reads
bone info (bounds/radius), and sets up LOD surface filenames.

This is the master model loading function. Flow:
  1. Read "xmodel/%s" file (fallback to "shadow" variant)
  2. Parse header (version + basic fields)
  3. Allocate XModel struct (256 + LOD string space)
  4. Call XModelReadCollSurfs to parse collision data
  5. Set up LOD filenames/numsurfs/surfNames in the model
  6. Find or load xmodelparts (XModelPartsFindData / XModelPartsLoadFile)
  7. Allocate and fill boneInfo array (bounds + radius per bone)
  8. Copy mins/maxs/collLod from parsed header


Note: the header parser and SL helper are not yet reconstructed.
================
*/
XModel_t *XModelLoadFile(const char *name, void *(*alloc)(int),
                        void *(*allocColl)(int))
{
    char filename[64];
    unsigned char *buf;
    int fileSize;
    const unsigned char *pos;
    unsigned char headerData[0x1080]; /* parsed header buffer */
    int lodStringLens[4];
    int totalStringLen;
    int allocSize;
    XModel_t *model;
    XModelParts_t *parts;
    const char *partsName;
    int numBones;
    int numLods;
    int i;
    int j;

    if (Com_sprintf(filename, sizeof(filename), "xmodel/%s", name) < 0)
    {
        Com_Printf("^1ERROR: filename '%s' too long\n", filename);
        return NULL;
    }

    fileSize = FS_ReadFile(filename, (void **)&buf);

    if (fileSize < 0)
    {
        Assert(!buf, s_assertDisable_XModelLoadFile_buf);

        /* try shadow model fallback */
        if (strstr(name, "shadow"))
            return NULL;

        Com_Printf("^1ERROR: xmodel '%s' not found\n", name);
        return NULL;
    }

    if (fileSize == 0)
    {
        Com_Printf("^1ERROR: xmodel '%s' has 0 length\n", name);
        FS_FreeFile(buf);
        return NULL;
    }

    /* parse header (version check + basic model data) */
    pos = buf;
    if (!XModel_ReadHeader(name, &pos, headerData))
    {
        FS_FreeFile(buf);
        return NULL;
    }

    /* compute LOD filename string lengths */
    totalStringLen = 0;
    for (i = 0; i < 4; i++)
    {
        lodStringLens[i] = (int)strlen((const char *)&headerData[i * 0x404]) + 1;
        totalStringLen += lodStringLens[i];
    }

    /* allocate XModel: struct (256 bytes) + inline LOD strings */
    allocSize = totalStringLen + 256;
    model = (XModel_t *)alloc(allocSize);
    model->memUsage = allocSize;

    /* read collision surfaces */
    XModelReadCollSurfs(&pos, model, allocColl, name);

    /* set up LOD filenames and surface data */
    {
        char *stringDst = (char *)model + 256; /* inline strings after struct */
        short numLodsCount = 0;

        for (i = 0; i < 4; i++)
        {
            /* copy filename string inline */
            const char *src = (const char *)&headerData[i * 0x404];
            char *dst = stringDst;
            while (*src)
                *dst++ = *src++;
            *dst = 0;

            model->lodInfo[i].filename = stringDst;

            if (*stringDst)
            {
                numLodsCount++;

                /* read numsurfs from stream */
                model->lodInfo[i].numsurfs = *(short *)pos;
                pos += 2;

                /* allocate and read surfNames */
                int ns = model->lodInfo[i].numsurfs;
                model->lodInfo[i].surfNames = (unsigned short *)alloc(ns * 2);
                model->memUsage += ns * 2;

                for (j = 0; j < ns; j++)
                {
                    /* read surface name via SL string lookup */
                    int len = (int)strlen((const char *)pos) + 1;
                    model->lodInfo[i].surfNames[j] = SL_GetStringOfLen((const char *)pos, 0, len, 8);
                    pos += len;
                }
            }
            else
            {
                model->lodInfo[i].surfNames = NULL;
            }

            /* read LOD distance from parsed header */
            /* model->lodInfo[i].dist = headerData[...]; */

            stringDst += lodStringLens[i];
        }

        model->numLods = numLodsCount;
        Assert(model->numLods, s_assertDisable_XModelLoadFile_numLods);
    }

    /* find or load xmodelparts */
    partsName = model->lodInfo[0].filename;

    /* try cache first (hunk type 3) */
    parts = (XModelParts_t *)Hunk_FindDataForFile(3, partsName);

    if (!parts)
    {
        parts = XModelPartsLoadFile(model, partsName, alloc);

        if (!parts)
        {
            Com_Printf("^1ERROR: Cannot find xmodelparts '%s'.\n", partsName);
        }
        else
        {
            Hunk_AddDataForFile(3, partsName, parts, alloc);
        }
    }

    model->parts = parts;

    if (!parts)
    {
        FS_FreeFile(buf);
        XModelFree(model);
        return NULL;
    }

    /* allocate and fill boneInfo array */
    numBones = parts->numBones;
    {
        int boneInfoSize = numBones * 40; /* XBoneInfo = 40 bytes */
        XBoneInfo_t *boneInfo = (XBoneInfo_t *)alloc(boneInfoSize);
        model->memUsage += boneInfoSize;

        for (i = 0; i < numBones; i++)
        {
            /* read 6 floats: bounds[0][3] and bounds[1][3] from stream */
            float b0 = *(float *)pos; pos += 4;
            float b1 = *(float *)pos; pos += 4;
            float b2 = *(float *)pos; pos += 4;
            float b3 = *(float *)pos; pos += 4;
            float b4 = *(float *)pos; pos += 4;
            float b5 = *(float *)pos; pos += 4;

            boneInfo[i].bounds[0][0] = b0;
            boneInfo[i].bounds[0][1] = b1;
            boneInfo[i].bounds[0][2] = b2;
            boneInfo[i].bounds[1][0] = b3;
            boneInfo[i].bounds[1][1] = b4;
            boneInfo[i].bounds[1][2] = b5;

            /* compute offset = center of bounds */
            boneInfo[i].offset[0] = (b0 + b3) * 0.5f;
            boneInfo[i].offset[1] = (b1 + b4) * 0.5f;
            boneInfo[i].offset[2] = (b2 + b5) * 0.5f;

            /* compute radiusSquared */
            float dx = b3 - boneInfo[i].offset[0];
            float dy = b4 - boneInfo[i].offset[1];
            float dz = b5 - boneInfo[i].offset[2];
            boneInfo[i].radiusSquared = dx * dx + dy * dy + dz * dz;
        }

        model->boneInfo = boneInfo;
    }

    FS_FreeFile(buf);

    /* copy mins/maxs from parsed header */
    model->mins[0] = *(float *)&headerData[0x1010];
    model->mins[1] = *(float *)&headerData[0x1014];
    model->mins[2] = *(float *)&headerData[0x1018];
    model->maxs[0] = *(float *)&headerData[0x101C];
    model->maxs[1] = *(float *)&headerData[0x1020];
    model->maxs[2] = *(float *)&headerData[0x1024];

    /* set collLod and flags from header */
    model->collLod = *(short *)&headerData[0x1028];
    Assert(model->collLod < model->numLods,
           s_assertDisable_XModelLoadFile_collLod);

    model->flags = headerData[0x102C];

    return model;
}

/*
================
XModelSurfsLoad

Loads surface data for all LOD levels of a model.
For each LOD with a non-empty filename: finds cached surfs
or loads from disk via XModelSurfsLoadFile, then caches.
Returns true if all LODs loaded successfully.

================
*/
int XModelSurfsLoad(XModel_t *model, void *(*alloc)(int))
{
    int i;
    const char *filename;
    XModelSurfs_t *surfs;

    Assert(model, s_assertDisable_XModelSurfsLoad);
    Assert(model->lodInfo[0].filename[0],
           s_assertDisable_XModelSurfsLoad_filename);

    for (i = 0; i < 4; i++)
    {
        filename = model->lodInfo[i].filename;
        if (!*filename)
            break;

        surfs = (XModelSurfs_t *)Hunk_FindDataForFile(2, filename);

        if (!surfs)
        {
            surfs = XModelSurfsLoadFile(model, filename, alloc,
                        model->lodInfo[i].numsurfs, model->name);

            if (!surfs)
            {
                Com_Printf("^1ERROR: Cannot find 'xmodelsurfs '%s'.\n", filename);
            }
            else
            {
                Hunk_AddDataForFile(2, filename, surfs, alloc);
            }
        }

        model->lodInfo[i].modelSurfs = surfs;

        if (!surfs)
            return 0;
    }

    return 1;
}

/*
================
XModel_ReadHeader

Reads the xmodel file header: version check, LOD filenames
with distances, model bounds, and collLod.

headerBuffer layout (0x1030 bytes):
  +0x0000  LOD 0: char filename[0x400] + float dist
  +0x0404  LOD 1: char filename[0x400] + float dist
  +0x0808  LOD 2: char filename[0x400] + float dist
  +0x0C0C  LOD 3: char filename[0x400] + float dist
  +0x1010  float mins[3]
  +0x101C  float maxs[3]
  +0x1028  int collLod
  +0x102C  byte numLods

================
*/
int XModel_ReadHeader(const char *name, const unsigned char **pos,
                       unsigned char *headerBuffer)
{
    short version;
    int i;

    /* read and check version */
    version = *(short *)*pos;
    *pos += 2;

    if (version != 20)
    {
        Com_Printf("^1ERROR: xmodel '%s' out of date (version %i, expecting %i)\n",
                    name, (int)version, 20);
        return 0;
    }

    /* read numLods byte */
    headerBuffer[0x102C] = **pos;
    *pos += 1;

    /* read 7 floats: mins[3], maxs[3], radius */
    for (i = 0; i < 6; i++) /* mins[3] + maxs[3], no 7th float */
    {
        *(float *)&headerBuffer[0x1010 + i * 4] = *(float *)*pos;
        *pos += 4;
    }

    /* read 4 LOD entries: distance float + filename string */
    for (i = 0; i < 4; i++)
    {
        int base = i * 0x404;
        char *dst;

        /* read LOD distance float */
        *(float *)&headerBuffer[base + 0x400] = *(float *)*pos;
        *pos += 4;

        /* copy filename string (binary copies locally, then advances pos via strlen) */
        {
            const unsigned char *strStart = *pos;
            dst = (char *)&headerBuffer[base];
            while (*strStart)
            {
                *dst++ = (char)*strStart++;
            }
            *dst = 0;

            /* advance pos past the string + null terminator */
            *pos += (int)strlen((const char *)*pos) + 1;
        }
    }

    /* read collLod int */
    *(int *)&headerBuffer[0x1028] = *(int *)*pos;
    *pos += 4;

    return 1;
}
