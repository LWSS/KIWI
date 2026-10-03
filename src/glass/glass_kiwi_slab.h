#pragma once
// KIWI (GLASS): pane slab math shared by the game list builder and KIWI Radiant validation.
// No engine includes; vector helpers keep com_math.cpp's arithmetic order.

#include <math.h>

#define GLASS_KIWI_MAX_PANES        1000   // GlassesServer::glasses[1000]
#define GLASS_KIWI_MAX_OUTLINE      22     // GlassRenderer::vertexList[22]
#define GLASS_KIWI_MIN_THICKNESS    0.1f
#define GLASS_KIWI_MAX_THICKNESS    16.0f
#define GLASS_KIWI_MAX_POLYS        32     // brush sides incl. the 6 axial planes
#define GLASS_KIWI_SLAB_MAX_POINTS  64
#define GLASS_KIWI_MAX_WEDGE_DEG    3.0f   // back face this far off parallel still counts (snapped drawing)
#define GLASS_KIWI_MAX_WEDGE_COS    0.99862953f  // cos(3 deg)

enum GlassKiwiSlabResult
{
    GLASS_SLAB_OK,
    GLASS_SLAB_NO_FACES,
    GLASS_SLAB_NOT_SLAB,     // no face within GLASS_KIWI_MAX_WEDGE_DEG of opposite the largest one
    GLASS_SLAB_THICKNESS,
    GLASS_SLAB_POINT_COUNT,
    GLASS_SLAB_NO_AREA,
};

struct GlassKiwiSlabFace
{
    const float *pts;        // ptCount points, ptStride floats apart
    unsigned int ptCount;
    unsigned int ptStride;
    float plane[4];          // outward normal, n.x = d
};

struct GlassKiwiSlab
{
    unsigned int frontFace;
    unsigned int backFace;
    float wedgeDeg;          // back face's angle off parallel (set unless NO_FACES)
    float thickness;         // back-face points' mean depth behind the front plane (set from THICKNESS on)
    float frontArea;         // units^2 (set unless NO_FACES)
    unsigned int numPts;     // merged front points (set from POINT_COUNT on)
    float axis[3][3];        // axis[2] = front outward normal
    float centroid[3];       // front-face centroid, offset applied
    float outline[GLASS_KIWI_MAX_OUTLINE][2]; // clockwise seen from +axis[2]
};

inline float GlassKiwiSlab_Dot(const float *a, const float *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline float GlassKiwiSlab_Length(const float *v)
{
    return sqrtf((v[2] * v[2]) + (v[1] * v[1]) + (v[0] * v[0]));
}

inline void GlassKiwiSlab_Normalize(float *v)
{
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float ilength = length > 0.0f ? 1.0f / length : 1.0f;

    v[0] = v[0] * ilength;
    v[1] = v[1] * ilength;
    v[2] = v[2] * ilength;
}

inline void GlassKiwiSlab_Cross(const float *v0, const float *v1, float *cross)
{
    cross[0] = v0[1] * v1[2] - v0[2] * v1[1];
    cross[1] = v0[2] * v1[0] - v0[0] * v1[2];
    cross[2] = v0[0] * v1[1] - v0[1] * v1[0];
}

inline float GlassKiwiSlab_DistanceSq(const float *p1, const float *p2)
{
    return (p2[2] - p1[2]) * (p2[2] - p1[2])
         + (p2[1] - p1[1]) * (p2[1] - p1[1])
         + (p2[0] - p1[0]) * (p2[0] - p1[0]);
}

// Newell normal length = twice the polygon area
inline float GlassKiwiSlab_PolyArea2(const GlassKiwiSlabFace *face)
{
    float normal[3] = { 0.0f, 0.0f, 0.0f };
    unsigned int i;

    for (i = 0; i < face->ptCount; ++i)
    {
        const float *a = face->pts + i * face->ptStride;
        const float *b = face->pts + ((i + 1) % face->ptCount) * face->ptStride;
        normal[0] += (a[1] - b[1]) * (a[2] + b[2]);
        normal[1] += (a[2] - b[2]) * (a[0] + b[0]);
        normal[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    return GlassKiwiSlab_Length(normal);
}

/*
==============
GlassKiwi_BuildSlab

A pane is one convex slab: front = the largest face, back = the face most opposite it.
offset (may be NULL) is added to every point (the game passes the submodel origin).
==============
*/
inline GlassKiwiSlabResult GlassKiwi_BuildSlab(const GlassKiwiSlabFace *faces, unsigned int numFaces, const float *offset, GlassKiwiSlab *out)
{
    static const float zero[3] = { 0.0f, 0.0f, 0.0f };
    float pts[GLASS_KIWI_SLAB_MAX_POINTS][3];
    const float *frontPlane;
    float *axis0 = out->axis[0], *axis1 = out->axis[1], *axis2 = out->axis[2];
    float bestArea, bestDot, signedArea, len, bestLen;
    unsigned int side, i, numPts;

    if (!offset)
        offset = zero;

    // front = largest face
    out->frontFace = numFaces;
    bestArea = 0.0f;
    for (side = 0; side < numFaces; ++side)
    {
        float area;

        if (faces[side].ptCount < 3)
            continue;
        area = GlassKiwiSlab_PolyArea2(&faces[side]);
        if (area > bestArea)
        {
            bestArea = area;
            out->frontFace = side;
        }
    }
    if (out->frontFace == numFaces)
        return GLASS_SLAB_NO_FACES;
    out->frontArea = bestArea * 0.5f;
    frontPlane = faces[out->frontFace].plane;

    // back = the face most opposite the front; a slight wedge (corners snapped to different
    // things) still counts
    out->backFace = numFaces;
    bestDot = 2.0f;
    for (side = 0; side < numFaces; ++side)
    {
        float dot;

        if (side == out->frontFace || faces[side].ptCount < 3)
            continue;
        dot = GlassKiwiSlab_Dot(faces[side].plane, frontPlane);
        if (dot < bestDot)
        {
            bestDot = dot;
            out->backFace = side;
        }
    }
    out->wedgeDeg = 180.0f;
    if (out->backFace != numFaces)
        out->wedgeDeg = acosf(fminf(fmaxf(-bestDot, -1.0f), 1.0f)) * (180.0f / 3.14159265f);
    if (out->backFace == numFaces || bestDot > -GLASS_KIWI_MAX_WEDGE_COS)
        return GLASS_SLAB_NOT_SLAB;

    // the back face's mean depth behind the front plane (outward normals, n.x = d)
    {
        const GlassKiwiSlabFace *back = &faces[out->backFace];
        float depth = 0.0f;

        for (i = 0; i < back->ptCount; ++i)
            depth += frontPlane[3] - GlassKiwiSlab_Dot(back->pts + i * back->ptStride, frontPlane);
        out->thickness = depth / (float)back->ptCount;
    }
    if (out->thickness < GLASS_KIWI_MIN_THICKNESS || out->thickness > GLASS_KIWI_MAX_THICKNESS)
        return GLASS_SLAB_THICKNESS;

    // front face points, near-duplicates merged
    numPts = 0;
    for (i = 0; i < faces[out->frontFace].ptCount; ++i)
    {
        const float *src = faces[out->frontFace].pts + i * faces[out->frontFace].ptStride;
        float p[3];

        p[0] = src[0] + offset[0];
        p[1] = src[1] + offset[1];
        p[2] = src[2] + offset[2];
        if (numPts && GlassKiwiSlab_DistanceSq(p, pts[numPts - 1]) < 0.01f * 0.01f)
            continue;
        if (numPts == GLASS_KIWI_SLAB_MAX_POINTS)
        {
            out->numPts = faces[out->frontFace].ptCount;
            return GLASS_SLAB_POINT_COUNT;
        }
        pts[numPts][0] = p[0];
        pts[numPts][1] = p[1];
        pts[numPts][2] = p[2];
        ++numPts;
    }
    while (numPts > 1 && GlassKiwiSlab_DistanceSq(pts[0], pts[numPts - 1]) < 0.01f * 0.01f)
        --numPts;
    out->numPts = numPts;
    if (numPts < 3 || numPts > GLASS_KIWI_MAX_OUTLINE)
        return GLASS_SLAB_POINT_COUNT;

    // axis[2] = front outward normal (the game stores thickness negated: back face inside)
    axis2[0] = frontPlane[0];
    axis2[1] = frontPlane[1];
    axis2[2] = frontPlane[2];
    if (fabs(axis2[2]) < 0.99f)
    {
        // vertical-ish pane: axis[1] is up projected onto the pane, axis[0] horizontal
        axis1[0] = -axis2[2] * axis2[0];
        axis1[1] = -axis2[2] * axis2[1];
        axis1[2] = 1.0f - axis2[2] * axis2[2];
        GlassKiwiSlab_Normalize(axis1);
        GlassKiwiSlab_Cross(axis1, axis2, axis0);
    }
    else
    {
        // horizontal pane: axis[0] along the longest front edge
        bestLen = -1.0f;
        for (i = 0; i < numPts; ++i)
        {
            const float *a = pts[i];
            const float *b = pts[(i + 1) % numPts];
            float edge[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };

            len = GlassKiwiSlab_Length(edge);
            if (len > bestLen)
            {
                bestLen = len;
                axis0[0] = edge[0];
                axis0[1] = edge[1];
                axis0[2] = edge[2];
            }
        }
        GlassKiwiSlab_Normalize(axis0);
        GlassKiwiSlab_Cross(axis2, axis0, axis1);
        GlassKiwiSlab_Normalize(axis1);
        GlassKiwiSlab_Cross(axis1, axis2, axis0);
    }

    out->centroid[0] = 0.0f;
    out->centroid[1] = 0.0f;
    out->centroid[2] = 0.0f;
    for (i = 0; i < numPts; ++i)
    {
        out->centroid[0] = out->centroid[0] + pts[i][0];
        out->centroid[1] = out->centroid[1] + pts[i][1];
        out->centroid[2] = out->centroid[2] + pts[i][2];
    }
    len = 1.0f / (float)numPts;
    out->centroid[0] = len * out->centroid[0];
    out->centroid[1] = len * out->centroid[1];
    out->centroid[2] = len * out->centroid[2];

    for (i = 0; i < numPts; ++i)
    {
        float d[3] = { pts[i][0] - out->centroid[0], pts[i][1] - out->centroid[1], pts[i][2] - out->centroid[2] };

        out->outline[i][0] = GlassKiwiSlab_Dot(d, axis0);
        out->outline[i][1] = GlassKiwiSlab_Dot(d, axis1);
    }
    signedArea = 0.0f;
    for (i = 0; i < numPts; ++i)
    {
        unsigned int j = (i + 1) % numPts;
        signedArea += out->outline[i][0] * out->outline[j][1] - out->outline[j][0] * out->outline[i][1];
    }
    if (fabs(signedArea) < 0.0001f)
        return GLASS_SLAB_NO_AREA;
    if (signedArea > 0.0f)
    {
        // the ear clipper (GlassShard::Triangles::Triangulate) only accepts clockwise outlines
        for (i = 0; i < numPts / 2; ++i)
        {
            float tmp[2] = { out->outline[i][0], out->outline[i][1] };
            out->outline[i][0] = out->outline[numPts - 1 - i][0];
            out->outline[i][1] = out->outline[numPts - 1 - i][1];
            out->outline[numPts - 1 - i][0] = tmp[0];
            out->outline[numPts - 1 - i][1] = tmp[1];
        }
    }
    return GLASS_SLAB_OK;
}
