/*
 * r_xsurface.c — XSurface runtime utilities (index copy, vertex deform).
 */

#include "cod2rad64.h"

#define WEIGHT_SCALE_65536  1.52588e-5f
#define WEIGHT_SCALE_256    0.00390625f

/*
================
XSurfaceGetNumTris

Return triangle count for a surface.
================
*/
int XSurfaceGetNumTris(XSurface_t *surface)
{
    return surface->triCount;
}

/*
================
R_XSurfaceCopyIndices

Copy triangle indices with a base vertex offset added.
================
*/
void R_XSurfaceCopyIndices(XSurface_t *surface, unsigned short *dstIndices, unsigned short baseIndex)
{
    int triCount;
    unsigned int *src;
    unsigned int *dst;
    unsigned int indexPair;
    int i;

    /* assert: surface->triIndices is 4-byte aligned (line 0x37) */
    Assert("(reinterpret_cast< size_t >( surface->triIndices ) & 3) == 0",
           "..\\src\\gfx_d3d\\r_xsurface.cpp", 0x37, 0, 1);

    /* assert: dstIndices is 4-byte aligned (line 0x38) */
    Assert("(reinterpret_cast< size_t >( dstIndices ) & 3) == 0",
           "..\\src\\gfx_d3d\\r_xsurface.cpp", 0x38, 0, 1);

    /* assert: triCount is even (line 0x39) */
    Assert("(surface->triCount & 1) == 0",
           "..\\src\\gfx_d3d\\r_xsurface.cpp", 0x39, 0, 1);

    if (baseIndex)
    {
        /* add baseIndex to each index, processing 2 indices (1 dword) at a time */
        triCount = surface->triCount;
        src = (unsigned int *)surface->triIndices;
        dst = (unsigned int *)dstIndices;
        indexPair = ((unsigned int)baseIndex << 16) | baseIndex;

        for (i = triCount / 2; i > 0; i--)
        {
            dst[0] = src[0] + indexPair;
            dst[1] = src[1] + indexPair;
            dst[2] = src[2] + indexPair;
            src += 3;
            dst += 3;
        }
    }
    else
    {
        /* no offset — straight memcpy */
        triCount = surface->triCount;
        memcpy(dstIndices, surface->triIndices, triCount * 3 * sizeof(unsigned short));
    }
}

/*
================
R_XSurfaceDeformVerts

Transform vertices by pre-computed bone matrices.
Two paths: rigid (shared matrix) and deformed (per-vert matrix with blend weights).
================
*/
void R_XSurfaceDeformVerts(XSurface_t *surface, BoneMatrix_t *boneMats, float *outPositions,
                           float *outTexcoords, float *outNormals)
{
    XSurfaceTempVert_t *v;
    int vertCount;
    short boneOffset;
    BoneMatrix_t *mat;
    int i;

    v = (XSurfaceTempVert_t *)surface->verts;
    vertCount = surface->vertCount;
    boneOffset = surface->boneOffset;

    if (boneOffset != -1)
    {
        /* RIGID: all verts use the same bone matrix */
        Assert("v", "..\\src\\gfx_d3d\\r_xsurface.cpp", 0x85, 0, 1);

        if (vertCount == 0)
            return;

        mat = (BoneMatrix_t *)((char *)boneMats + boneOffset);

        for (i = vertCount; i > 0; i--)
        {
            if (outNormals)
            {
                /* normal = normal * mat3x3 rotation */
                outNormals[0] = v->normal[0] * mat->col0[0]
                              + v->normal[1] * mat->col1[0]
                              + v->normal[2] * mat->col2[0];
                outNormals[1] = v->normal[0] * mat->col0[1]
                              + v->normal[1] * mat->col1[1]
                              + v->normal[2] * mat->col2[1];
                outNormals[2] = v->normal[0] * mat->col0[2]
                              + v->normal[1] * mat->col1[2]
                              + v->normal[2] * mat->col2[2];
            }

            if (outTexcoords)
            {
                outTexcoords[0] = v->texcoordU;
                outTexcoords[1] = v->texcoordV;
                outTexcoords += 2;
            }

            /* pos = pos * mat3x3 + translation */
            outPositions[0] = v->pos[0] * mat->col0[0]
                            + v->pos[1] * mat->col1[0]
                            + v->pos[2] * mat->col2[0]
                            + mat->col3[0];
            outPositions[1] = v->pos[0] * mat->col0[1]
                            + v->pos[1] * mat->col1[1]
                            + v->pos[2] * mat->col2[1]
                            + mat->col3[1];
            outPositions[2] = v->pos[0] * mat->col0[2]
                            + v->pos[1] * mat->col1[2]
                            + v->pos[2] * mat->col2[2]
                            + mat->col3[2];

            v++;
            outPositions += 3;
            if (outNormals)
                outNormals += 3;
        }
    }
    else
    {
        /* DEFORMED: each vert has its own boneOffset, may have blend weights */
        Assert("v", "..\\src\\gfx_d3d\\r_xsurface.cpp", 0xB5, 0, 1);

        if (vertCount == 0)
            return;

        for (i = vertCount; i > 0; i--)
        {
            char *vp = (char *)v;

            mat = (BoneMatrix_t *)((char *)boneMats + v->boneOffset);

            if (outNormals)
            {
                /* normal = normal * mat3x3 rotation */
                outNormals[0] = v->normal[0] * mat->col0[0]
                              + v->normal[1] * mat->col1[0]
                              + v->normal[2] * mat->col2[0];
                outNormals[1] = v->normal[0] * mat->col0[1]
                              + v->normal[1] * mat->col1[1]
                              + v->normal[2] * mat->col2[1];
                outNormals[2] = v->normal[0] * mat->col0[2]
                              + v->normal[1] * mat->col1[2]
                              + v->normal[2] * mat->col2[2];
                outNormals += 3;
            }

            /* pos = pos * mat3x3 + translation */
            outPositions[0] = v->pos[0] * mat->col0[0]
                            + v->pos[1] * mat->col1[0]
                            + v->pos[2] * mat->col2[0]
                            + mat->col3[0];
            outPositions[1] = v->pos[0] * mat->col0[1]
                            + v->pos[1] * mat->col1[1]
                            + v->pos[2] * mat->col2[1]
                            + mat->col3[1];
            outPositions[2] = v->pos[0] * mat->col0[2]
                            + v->pos[1] * mat->col1[2]
                            + v->pos[2] * mat->col2[2]
                            + mat->col3[2];

            /* blend weights: additional bone contributions */
            if (v->numWeights > 0)
            {
                float firstWeight;
                int numWeights;
                XSurfaceBlendEntry_t *blend;
                BoneMatrix_t *blendMat;

                /* scale initial result by first bone weight */
                firstWeight = (float)v->pad3D * WEIGHT_SCALE_256;
                outPositions[0] *= firstWeight;
                outPositions[1] *= firstWeight;
                outPositions[2] *= firstWeight;

                numWeights = v->numWeights;
                blend = (XSurfaceBlendEntry_t *)(vp + 0x40);

                while (numWeights > 0)
                {
                    float w;
                    blendMat = (BoneMatrix_t *)((char *)boneMats + blend->boneOffset);
                    w = (float)blend->weight * WEIGHT_SCALE_65536;

                    outPositions[0] += (blend->data[0] * blendMat->col0[0]
                                      + blend->data[1] * blendMat->col1[0]
                                      + blend->data[2] * blendMat->col2[0]
                                      + blendMat->col3[0]) * w;
                    outPositions[1] += (blend->data[0] * blendMat->col0[1]
                                      + blend->data[1] * blendMat->col1[1]
                                      + blend->data[2] * blendMat->col2[1]
                                      + blendMat->col3[1]) * w;
                    outPositions[2] += (blend->data[0] * blendMat->col0[2]
                                      + blend->data[1] * blendMat->col1[2]
                                      + blend->data[2] * blendMat->col2[2]
                                      + blendMat->col3[2]) * w;

                    blend++;
                    numWeights--;
                }
                /* vp advances past vert (0x40) + blend entries (numWeights * 0x10) */
                v = (XSurfaceTempVert_t *)blend;
            }
            else
            {
                /* no blend weights — advance past vert only */
                v = (XSurfaceTempVert_t *)(vp + 0x40);
            }

            outPositions += 3;
        }
    }
}
