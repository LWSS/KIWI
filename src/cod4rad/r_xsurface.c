/*
 * r_xsurface.c — XSurface runtime utilities (index copy, vertex deform).
 */

#include "cod2rad64.h"

#define WEIGHT_SCALE_65536  0.0000152587890625f

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
    int indexCount = surface->triCount * 3;
    int i;

    /* Process each 16-bit index independently, including odd triangle counts. */
    for (i = 0; i < indexCount; ++i)
    {
        unsigned int index = (unsigned int)surface->triIndices[i] + baseIndex;
        if (index > USHRT_MAX)
            ErrorMsg("R_XSurfaceCopyIndices: vertex index exceeds 16 bits\n");
        dstIndices[i] = (unsigned short)index;
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
        Assert(v, 0);

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
        Assert(v, 0);

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

            /* blend weights: additional bone contributions */
            if (v->numWeights > 0)
            {
                float firstWeight;
                float secondaryWeight;
                int numWeights;
                int weightIndex;
                XSurfaceBlendEntry_t *blend;
                XSurfaceBlendEntry_t *weightBlend;
                BoneMatrix_t *blendMat;

                /* v25 stores each secondary weight as u16; the primary weight
                 * is the remainder after all secondary influences. */
                secondaryWeight = 0.0f;
                weightBlend = (XSurfaceBlendEntry_t *)(vp + 0x40);
                for (weightIndex = 0; weightIndex < v->numWeights; weightIndex++)
                    secondaryWeight += (float)weightBlend[weightIndex].weight
                                     * WEIGHT_SCALE_65536;

                firstWeight = 1.0f - secondaryWeight;
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
