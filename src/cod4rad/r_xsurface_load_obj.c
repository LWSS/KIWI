/*
 * r_xsurface_load_obj.c - CoD4 v25 raw XSurface loading for cod4rad.
 *
 * The retail loader expands the variable v25 vertex records into a temporary
 * 64-byte representation and then packs them for the renderer.  cod4rad needs
 * float positions, normals, and UVs instead, so this loader expands the same
 * wire records directly into XSurfaceTempVert_t plus compiler blend entries.
 */

#include "cod2rad64.h"

extern float vec3_origin[3];

#define XMODEL_MAX_RIGID_VERT_LISTS 128
#define TANGENT_FRAME_EPSILON       0.002f

static char s_assertDisable_R_XSurfaceLoadObj_boneOffset;
static char s_assertDisable_R_XSurfaceLoadObj_tangentFrame;
static char s_assertDisable_R_XSurfaceLoadObj_triIndices;
static char s_assertDisable_R_XSurfaceLoadObj_allocCount;

static int R_XSurfaceSetPartBit(XModel_t *model, int *partBits,
                                unsigned short boneIndex)
{
    int numBones;

    numBones = model->parts ? model->parts->numBones : DOBJ_MAX_PARTS;
    if (boneIndex >= DOBJ_MAX_PARTS || boneIndex >= numBones)
    {
        Com_Printf("^1ERROR: xmodelsurfs references bone %u, but the model has %d bones\n",
                   (unsigned int)boneIndex, numBones);
        return 0;
    }

    /* cod4rad's DObj code uses the low-bit-first partBits representation. */
    partBits[boneIndex >> 5] |= (int)(1u << (boneIndex & 0x1F));
    return 1;
}

/*
================
R_XSurfaceLoadObj

CoD4 v25 surface wire layout:
  byte tileMode, u16 opaque surface value, u16 vertCount, u16 triCount
  repeated { u16 rigidVertCount, u16 boneIndex }, terminated by count == 0
  u16 totalBlendCount unless there is exactly one rigid list
  vertices, then triCount * 3 u16 indices

The fixed vertex prefix is normal[3], color[4], uv[2], binormal[3],
tangent[3] (48 bytes).  A one-list rigid vertex then has position[3]
(60 bytes total).  Every other vertex has byte secondaryWeightCount,
u16 primaryBone, position[3], followed by that many {u16 bone, u16 weight}
records (63 + 4*n bytes total).
================
*/
XSurface_t *R_XSurfaceLoadObj(XModel_t *model, int *partBits,
                              const unsigned char **pos, void *(*alloc)(int))
{
    XSurface_t *surface;
    unsigned char *vertCursor;
    int rigidVertListCount;
    int rigidVertCount;
    unsigned short singleRigidBone;
    int useSingleRigidRecord;
    int numBlends;
    int blendsRead;
    int allocSize;
    int originalTriCount;
    int allocCount;
    int i;

    surface = (XSurface_t *)alloc(sizeof(XSurface_t));
    model->memUsage += sizeof(XSurface_t);
    memset(surface, 0, sizeof(XSurface_t));

    surface->tileMode = **pos;
    *pos += 1;

    /* Retail reads this u16 but does not retain it in XSurface. */
    *pos += 2;

    surface->vertCount = *(const unsigned short *)*pos;
    *pos += 2;
    surface->triCount = *(const unsigned short *)*pos;
    *pos += 2;

    if (!surface->triCount)
    {
        Com_Printf("^1ERROR: xmodelsurfs contains a surface with no triangles\n");
        return NULL;
    }

    rigidVertListCount = 0;
    rigidVertCount = 0;
    singleRigidBone = 0;

    for (;;)
    {
        unsigned short listVertCount;
        unsigned short boneIndex;

        listVertCount = *(const unsigned short *)*pos;
        *pos += 2;
        if (!listVertCount)
            break;

        if (rigidVertListCount >= XMODEL_MAX_RIGID_VERT_LISTS)
        {
            Com_Printf("^1ERROR: xmodelsurfs contains too many rigid vertex lists\n");
            return NULL;
        }

        boneIndex = *(const unsigned short *)*pos;
        *pos += 2;
        if (!rigidVertListCount)
            singleRigidBone = boneIndex;

        rigidVertCount += listVertCount;
        rigidVertListCount++;
    }

    surface->deformed = (rigidVertCount != surface->vertCount);
    if (surface->deformed)
        rigidVertListCount = 0;

    useSingleRigidRecord = (rigidVertListCount == 1);
    surface->boneOffset = -1;

    if (useSingleRigidRecord)
    {
        int boneOffset;

        numBlends = 0;
        if (!R_XSurfaceSetPartBit(model, partBits, singleRigidBone))
            return NULL;

        boneOffset = (int)singleRigidBone << 6;
        Assert((short)boneOffset == boneOffset,
               s_assertDisable_R_XSurfaceLoadObj_boneOffset);
        surface->boneOffset = (short)boneOffset;
    }
    else
    {
        numBlends = *(const unsigned short *)*pos;
        *pos += 2;
    }

    allocSize = surface->vertCount * (int)sizeof(XSurfaceTempVert_t)
              + numBlends * (int)sizeof(XSurfaceBlendEntry_t);
    surface->verts = alloc(allocSize);
    model->memUsage += allocSize;
    memset(surface->verts, 0, allocSize);

    vertCursor = (unsigned char *)surface->verts;
    blendsRead = 0;

    for (i = 0; i < surface->vertCount; i++)
    {
        XSurfaceTempVert_t *vert;
        float check[3];

        vert = (XSurfaceTempVert_t *)vertCursor;

        vert->normal[0] = *(const float *)*pos; *pos += 4;
        vert->normal[1] = *(const float *)*pos; *pos += 4;
        vert->normal[2] = *(const float *)*pos; *pos += 4;

        *(unsigned int *)vert->color = *(const unsigned int *)*pos;
        *pos += 4;

        vert->texcoordU = *(const float *)*pos; *pos += 4;
        vert->texcoordV = *(const float *)*pos; *pos += 4;

        vert->binormal[0] = *(const float *)*pos; *pos += 4;
        vert->binormal[1] = *(const float *)*pos; *pos += 4;
        vert->binormal[2] = *(const float *)*pos; *pos += 4;

        vert->tangent[0] = *(const float *)*pos; *pos += 4;
        vert->tangent[1] = *(const float *)*pos; *pos += 4;
        vert->tangent[2] = *(const float *)*pos; *pos += 4;

        check[0] = vert->normal[0] * vert->tangent[0]
                 + vert->normal[1] * vert->tangent[1]
                 + vert->normal[2] * vert->tangent[2];
        check[1] = vert->tangent[0] * vert->binormal[0]
                 + vert->tangent[1] * vert->binormal[1]
                 + vert->tangent[2] * vert->binormal[2];
        check[2] = vert->binormal[0] * vert->normal[0]
                 + vert->binormal[1] * vert->normal[1]
                 + vert->binormal[2] * vert->normal[2];
        Assert(VectorCompareEpsilon(check, vec3_origin,
                                    TANGENT_FRAME_EPSILON, 3),
               s_assertDisable_R_XSurfaceLoadObj_tangentFrame);

        if (useSingleRigidRecord)
        {
            vert->numWeights = 0;
            vert->boneOffset = (unsigned short)surface->boneOffset;
            vert->pos[0] = *(const float *)*pos; *pos += 4;
            vert->pos[1] = *(const float *)*pos; *pos += 4;
            vert->pos[2] = *(const float *)*pos; *pos += 4;
            vertCursor += sizeof(XSurfaceTempVert_t);
        }
        else
        {
            unsigned char numWeights;
            unsigned short boneIndex;
            int boneOffset;
            int weightIndex;

            numWeights = **pos;
            *pos += 1;
            if (numWeights >= 4)
            {
                Com_Printf("^1ERROR: xmodelsurfs vertex has %u secondary weights (maximum is 3)\n",
                           (unsigned int)numWeights);
                return NULL;
            }
            if (numWeights && !surface->deformed)
            {
                Com_Printf("^1ERROR: rigid xmodelsurfs vertex has blend weights\n");
                return NULL;
            }
            vert->numWeights = numWeights;

            boneIndex = *(const unsigned short *)*pos;
            *pos += 2;
            if (!R_XSurfaceSetPartBit(model, partBits, boneIndex))
                return NULL;

            boneOffset = (int)boneIndex << 6;
            if ((unsigned short)boneOffset != boneOffset)
            {
                Com_Printf("^1ERROR: xmodelsurfs bone offset does not fit in u16\n");
                return NULL;
            }
            vert->boneOffset = (unsigned short)boneOffset;

            vert->pos[0] = *(const float *)*pos; *pos += 4;
            vert->pos[1] = *(const float *)*pos; *pos += 4;
            vert->pos[2] = *(const float *)*pos; *pos += 4;

            vertCursor += sizeof(XSurfaceTempVert_t);

            for (weightIndex = 0; weightIndex < numWeights; weightIndex++)
            {
                XSurfaceBlendEntry_t *blend;

                if (blendsRead >= numBlends)
                {
                    Com_Printf("^1ERROR: xmodelsurfs blend count exceeds its header value\n");
                    return NULL;
                }

                blend = (XSurfaceBlendEntry_t *)vertCursor;
                boneIndex = *(const unsigned short *)*pos;
                *pos += 2;
                if (!R_XSurfaceSetPartBit(model, partBits, boneIndex))
                    return NULL;

                boneOffset = (int)boneIndex << 6;
                if ((unsigned short)boneOffset != boneOffset)
                {
                    Com_Printf("^1ERROR: xmodelsurfs blend bone offset does not fit in u16\n");
                    return NULL;
                }

                blend->data[0] = vert->pos[0];
                blend->data[1] = vert->pos[1];
                blend->data[2] = vert->pos[2];
                blend->boneOffset = (unsigned short)boneOffset;
                blend->weight = *(const unsigned short *)*pos;
                *pos += 2;

                vertCursor += sizeof(XSurfaceBlendEntry_t);
                blendsRead++;
            }
        }
    }

    if (blendsRead != numBlends)
    {
        Com_Printf("^1ERROR: xmodelsurfs blend count does not match its vertex records\n");
        return NULL;
    }

    originalTriCount = surface->triCount;
    allocCount = (originalTriCount + 1) & ~1;
    surface->triIndices = (unsigned short *)alloc(6 * (originalTriCount + 1));
    Assert(surface->triIndices, s_assertDisable_R_XSurfaceLoadObj_triIndices);

    for (i = 0; i < originalTriCount * 3; i++)
    {
        surface->triIndices[i] = *(const unsigned short *)*pos;
        *pos += 2;
        if (surface->triIndices[i] >= surface->vertCount)
        {
            Com_Printf("^1ERROR: xmodelsurfs triangle index %u exceeds vertex count %u\n",
                       (unsigned int)surface->triIndices[i],
                       (unsigned int)surface->vertCount);
            return NULL;
        }
    }

    if (allocCount != originalTriCount)
    {
        Assert(allocCount == originalTriCount + 1,
               s_assertDisable_R_XSurfaceLoadObj_allocCount);
        surface->triIndices[i] = surface->triIndices[i - 1];
        surface->triIndices[i + 1] = surface->triIndices[i - 1];
        surface->triIndices[i + 2] = surface->triIndices[i - 1];
        surface->triCount++;
    }

    return surface;
}
