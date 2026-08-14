/*
 * r_xsurface_load_obj.c — XSurface binary format loading.
 *
 * Source: ..\src\gfx_d3d\r_xsurface_load_obj.cpp
 * Reads per-surface vertex/triangle data from the xmodelsurfs file.
 *
 * Note: The x64 cod2rad version is simpler than the x86 decompiled.
 * It reads raw vertex data + triangle indices but does NOT do
 * rigid vertex lists, XSurfaceTransfer, or blend data extraction.
 * Those may be handled elsewhere in the x64 pipeline.
 *
 * See cod2rad64.h for XSurface struct layout.
 */

#include "cod2rad64.h"

extern float vec3_origin[3];
#define TANGENT_FRAME_EPSILON 0.001f

static char s_assertDisable_R_XSurfaceLoadObj_boneOffset;
static char s_assertDisable_R_XSurfaceLoadObj_tangentFrame;
static char s_assertDisable_R_XSurfaceLoadObj_weights;
static char s_assertDisable_R_XSurfaceLoadObj_blendOffset;
static char s_assertDisable_R_XSurfaceLoadObj_deformed;
static char s_assertDisable_R_XSurfaceLoadObj_blendBone;
static char s_assertDisable_R_XSurfaceLoadObj_triIndices;
static char s_assertDisable_R_XSurfaceLoadObj_allocCount;

/*
================
R_XSurfaceLoadObj

Reads a single XSurface from the binary xmodelsurfs file stream.
Allocates 40-byte XSurface, reads header, vertex data (variable
per-vertex format with position/normal/tangent/bone data),
triangle indices, and handles padding for even tri count.

Per-vertex data in file (variable size):
  Rigid (boneOffset != -1): 48 bytes per vert
    3 floats position, 4 bytes packed normal,
    8 floats tangent/binormal/texcoord data, 3 floats extra
  Deformed (boneOffset == -1): 48+ bytes per vert
    Same as rigid but with bone weight byte, bone index short,
    and additional blend entries (4 bytes each per weight)

================
*/
XSurface_t *R_XSurfaceLoadObj(XModel_t *model, int *partBits,
                             const unsigned char **pos, void *(*alloc)(int))
{
    XSurface_t *surface;
    unsigned char *vertBuf;
    int vertCount;
    int triCount;
    short boneIndex;
    int allocSize;
    int allocCount;
    int i;

    /* allocate XSurface struct (40 bytes) */
    surface = (XSurface_t *)alloc(0x28);
    model->memUsage += 0x28;

    /* read surface header */
    surface->flags = **pos;
    *pos += 1;

    surface->vertCount = *(unsigned short *)*pos;
    *pos += 2;

    surface->triCount = *(unsigned short *)*pos;
    *pos += 2;

    /* read bone offset */
    boneIndex = *(short *)*pos;
    *pos += 2;

    if (boneIndex == -1)
    {
        /* deformed surface */
        short extraCount = *(short *)*pos;
        *pos += 2;

        surface->boneOffset = -1;

        allocSize = (extraCount + surface->vertCount * 4) * 16;
        surface->verts = alloc(allocSize);
        model->memUsage += allocSize;
    }
    else
    {
        /* rigid surface */
        int boneNum = (unsigned short)boneIndex;
        int offset = boneNum << 6;

        Assert((short)offset == offset,
               s_assertDisable_R_XSurfaceLoadObj_boneOffset);

        surface->boneOffset = (short)offset;

        /* set bone bit in partBits */
        partBits[boneNum >> 5] |= 1 << (boneNum & 0x1F);

        /* allocate vertex buffer: vertCount * 64 bytes */
        allocSize = surface->vertCount * 64;
        surface->verts = alloc(allocSize);
        model->memUsage += allocSize;
    }

    /* read vertex data */
    {
        XSurfaceTempVert_t *vert = (XSurfaceTempVert_t *)surface->verts;
        vertCount = surface->vertCount;

        for (i = 0; i < vertCount; i++)
        {
            /* normal (3 floats) */
            vert->normal[0] = *(float *)*pos; *pos += 4;
            vert->normal[1] = *(float *)*pos; *pos += 4;
            vert->normal[2] = *(float *)*pos; *pos += 4;

            /* packed color (4 bytes) */
            *(unsigned int *)vert->color = *(unsigned int *)*pos; *pos += 4;

            /* texcoord U, texcoord V */
            vert->texcoordU = *(float *)*pos; *pos += 4;
            vert->texcoordV = *(float *)*pos; *pos += 4;

            /* tangent (3 floats) */
            vert->tangent[0] = *(float *)*pos; *pos += 4;
            vert->tangent[1] = *(float *)*pos; *pos += 4;
            vert->tangent[2] = *(float *)*pos; *pos += 4;

            /* binormal (3 floats) */
            vert->binormal[0] = *(float *)*pos; *pos += 4;
            vert->binormal[1] = *(float *)*pos; *pos += 4;
            vert->binormal[2] = *(float *)*pos; *pos += 4;

            /* verify tangent frame orthogonality */
            {
                float check[3];
                check[0] = vert->normal[0] * vert->binormal[0]
                         + vert->normal[1] * vert->binormal[1]
                         + vert->normal[2] * vert->binormal[2];
                check[1] = vert->binormal[0] * vert->tangent[0]
                         + vert->binormal[1] * vert->tangent[1]
                         + vert->binormal[2] * vert->tangent[2];
                check[2] = vert->normal[0] * vert->tangent[0]
                         + vert->normal[1] * vert->tangent[1]
                         + vert->normal[2] * vert->tangent[2];
                Assert(VectorCompareEpsilon(check, vec3_origin, TANGENT_FRAME_EPSILON, 3),
                       s_assertDisable_R_XSurfaceLoadObj_tangentFrame);
            }

            if (boneIndex != -1)
            {
                /* rigid: position (3 floats) — no numWeights/boneOffset written */
                vert->pos[0] = *(float *)*pos; *pos += 4;
                vert->pos[1] = *(float *)*pos; *pos += 4;
                vert->pos[2] = *(float *)*pos; *pos += 4;
                vert++;
            }
            else
            {
                /* deformed: weight count, bone index, position offset */
                unsigned char numWeights = **pos; *pos += 1;
                vert->numWeights = numWeights;

                /* bone index and offset */
                {
                    short bi = *(short *)*pos; *pos += 2;
                    int boff = bi << 6;
                    partBits[bi >> 5] |= 1 << (bi & 0x1F);
                    vert->boneOffset = (unsigned short)boff;

                    Assert(boff == (short)boff,
                           s_assertDisable_R_XSurfaceLoadObj_blendOffset);
                }

                /* position offset (3 floats) */
                vert->pos[0] = *(float *)*pos; *pos += 4;
                vert->pos[1] = *(float *)*pos; *pos += 4;
                vert->pos[2] = *(float *)*pos; *pos += 4;

                vert++;

                /* read additional blend bone entries (16 bytes each) */
                if (numWeights > 0)
                {
                    XSurfaceBlendEntry_t *blend = (XSurfaceBlendEntry_t *)vert;
                    unsigned char *blendRaw = (unsigned char *)vert;
                    int w;

                    /* read extra byte before blend loop */
                    blendRaw[-3] = **pos; *pos += 1;

                    for (w = 0; w < numWeights; w++)
                    {
                        /* read bone index from file */
                        short bi = *(short *)*pos; *pos += 2;
                        int boff = bi << 6;
                        partBits[bi >> 5] |= 1 << (bi & 0x1F);
                        blend->boneOffset = (unsigned short)boff;

                        Assert(boff == (unsigned short)boff,
                               s_assertDisable_R_XSurfaceLoadObj_blendBone);

                        /* read 3 floats data */
                        blend->data[0] = *(float *)*pos; *pos += 4;
                        blend->data[1] = *(float *)*pos; *pos += 4;
                        blend->data[2] = *(float *)*pos; *pos += 4;

                        /* read weight */
                        blend->weight = *(unsigned short *)*pos; *pos += 2;
                        blend++;
                    }

                    vert = (XSurfaceTempVert_t *)blend;
                }
            }
        }
    }

    /* allocate and read triangle indices */
    triCount = surface->triCount;
    allocCount = (triCount + 1) & ~1; /* round up to even */

    surface->triIndices = (unsigned short *)alloc((triCount * 3 + 3) * 2);
    Assert(surface->triIndices, s_assertDisable_R_XSurfaceLoadObj_triIndices);

    for (i = 0; i < triCount * 3; i++)
    {
        surface->triIndices[i] = *(unsigned short *)*pos;
        *pos += 2;
    }

    /* pad to even tri count if needed */
    if (allocCount != triCount)
    {
        Assert(allocCount == triCount + 1,
               s_assertDisable_R_XSurfaceLoadObj_allocCount);

        /* duplicate last vertex index 3 times for padding triangle */
        surface->triIndices[i] = surface->triIndices[i - 1];
        surface->triIndices[i + 1] = surface->triIndices[i - 1];
        surface->triIndices[i + 2] = surface->triIndices[i - 1];
        surface->triCount++;
    }

    return surface;
}
