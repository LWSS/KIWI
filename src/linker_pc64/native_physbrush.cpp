#include <universal/q_shared.h>
#include <physics/phys_local.h>
#include "native_physbrush.h"
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static double Dot(const double *a, const double *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void Cross(const double *a, const double *b, double *out)
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static bool Bounded(const double (*planes)[4], unsigned int count)
{
    for (unsigned int i = 0; i < count; ++i)
    {
        for (unsigned int j = 0; j < i; ++j)
        {
            double direction[3];
            Cross(planes[i], planes[j], direction);
            const double length = sqrt(Dot(direction, direction));
            if (length < 1e-10)
            {
                continue;
            }
            for (int sign = -1; sign <= 1; sign += 2)
            {
                bool blocked = false;
                for (unsigned int p = 0; p < count; ++p)
                {
                    blocked = blocked || sign * Dot(planes[p], direction) / length > 1e-8;
                }
                if (!blocked)
                {
                    return false;
                }
            }
        }
    }
    return true;
}
static unsigned int Vertices(const double (*planes)[4], unsigned int count, double (*points)[3])
{
    unsigned int used = 0;
    for (unsigned int a = 0; a < count; ++a)
    {
        for (unsigned int b = 0; b < a; ++b)
        {
            for (unsigned int c = 0; c < b; ++c)
            {
                double bc[3], ca[3], ab[3], point[3];
                Cross(planes[b], planes[c], bc);
                const double determinant = Dot(planes[a], bc);
                if (fabs(determinant) < 1e-10)
                {
                    continue;
                }
                Cross(planes[c], planes[a], ca);
                Cross(planes[a], planes[b], ab);
                bool valid = true;
                for (int d = 0; d < 3; ++d)
                {
                    point[d] = (planes[a][3] * bc[d] + planes[b][3] * ca[d] + planes[c][3] * ab[d]) / determinant;
                    valid = valid && isfinite(point[d]) && fabs(point[d]) < 1e8;
                }
                for (unsigned int p = 0; valid && p < count; ++p)
                {
                    valid = Dot(planes[p], point) - planes[p][3] <= 0.001;
                }
                for (unsigned int p = 0; valid && p < used; ++p)
                {
                    double distance = 0;
                    for (int d = 0; d < 3; ++d)
                    {
                        const double delta = points[p][d] - point[d];
                        distance += delta * delta;
                    }
                    valid = distance > 1e-8;
                }
                if (valid)
                {
                    if (used == 1024)
                    {
                        return 0;
                    }
                    memcpy(points[used++], point, sizeof(double[3]));
                }
            }
        }
    }
    return used;
}

void Linker_FreePhysicsBrush(BrushWrapper *brush)
{
    if (brush)
    {
        free(brush->sides);
        free(brush->planes);
        free(brush->baseAdjacentSide);
        free(brush);
    }
}

struct BrushFace
{
    unsigned int count;
    unsigned int vertices[1024];
    double angles[1024];
};

bool Linker_BuildPhysicsBrush(const double (*input)[4], unsigned int count, BrushWrapper **brush, char *error,
                              size_t errorSize)
{
    if (!brush || (!error && errorSize))
    {
        return false;
    }
    *brush = NULL;
    if (!input || count < 4 || count > 32)
    {
        return false;
    }
    double planes[38][4] = {}, points[1024][3];
    unsigned int unique = 0;
    int failedFace = -1;
    bool valid = true;
    for (unsigned int p = 0; valid && p < count; ++p)
    {
        const double length = sqrt(Dot(input[p], input[p]));
        valid = isfinite(length) && length > 1e-10 && isfinite(input[p][3]);
        if (!valid)
        {
            break;
        }
        double plane[4];
        for (int a = 0; a < 4; ++a)
        {
            plane[a] = input[p][a] / length;
        }
        bool duplicate = false;
        for (unsigned int j = 0; j < unique; ++j)
        {
            duplicate = duplicate || (fabs(planes[j][0] - plane[0]) < 1e-8 && fabs(planes[j][1] - plane[1]) < 1e-8 &&
                                      fabs(planes[j][2] - plane[2]) < 1e-8 && fabs(planes[j][3] - plane[3]) < 0.001);
        }
        if (!duplicate)
        {
            memcpy(planes[unique++], plane, sizeof(plane));
        }
    }
    valid = valid && unique >= 4 && Bounded(planes, unique);
    unsigned int pointCount = valid ? Vertices(planes, unique, points) : 0;
    valid = valid && pointCount >= 4;
    BrushWrapper *result = valid ? (BrushWrapper *)calloc(1, sizeof(BrushWrapper)) : NULL;
    valid = valid && result;
    if (valid)
    {
        for (int a = 0; a < 3; ++a)
        {
            double low = DBL_MAX, high = -DBL_MAX;
            for (unsigned int p = 0; p < pointCount; ++p)
            {
                low = fmin(low, points[p][a]);
                high = fmax(high, points[p][a]);
            }
            result->mins[a] = (float)low;
            result->maxs[a] = (float)high;
            if (result->mins[a] > low)
            {
                result->mins[a] = nextafterf(result->mins[a], -FLT_MAX);
            }
            if (result->maxs[a] < high)
            {
                result->maxs[a] = nextafterf(result->maxs[a], FLT_MAX);
            }
            valid = valid && result->mins[a] < result->maxs[a];
        }
        double ordered[38][4] = {};
        unsigned int orderedCount = 6;
        for (unsigned int a = 0; a < 3; ++a)
        {
            ordered[2 * a][a] = -1;
            ordered[2 * a][3] = -result->mins[a];
            ordered[2 * a + 1][a] = 1;
            ordered[2 * a + 1][3] = result->maxs[a];
        }
        for (unsigned int p = 0; p < unique; ++p)
        {
            bool axial = false;
            for (unsigned int a = 0; a < 6; ++a)
            {
                axial = axial || Dot(planes[p], ordered[a]) > 1 - 1e-10;
            }
            if (!axial)
            {
                memcpy(ordered[orderedCount++], planes[p], sizeof(double[4]));
            }
        }
        memcpy(planes, ordered, sizeof(ordered));
        unique = orderedCount;
        // Axial bounding planes do not change the hull. Re-intersecting their rounded
        // float distances creates artificial sliver faces near the original vertices.
    }
    BrushFace *faces = valid ? (BrushFace *)calloc(unique, sizeof(BrushFace)) : NULL;
    valid = valid && faces;
    for (unsigned int p = 0; valid && p < unique; ++p)
    {
        BrushFace *face = &faces[p];
        double center[3] = {};
        for (unsigned int v = 0; v < pointCount; ++v)
        {
            if (fabs(Dot(planes[p], points[v]) - planes[p][3]) <= 0.002)
            {
                face->vertices[face->count++] = v;
                for (int a = 0; a < 3; ++a)
                {
                    center[a] += points[v][a];
                }
            }
        }
        if (face->count < 3)
        {
            face->count = 0;
            continue;
        }
        for (int a = 0; a < 3; ++a)
        {
            center[a] /= face->count;
        }
        double helper[3] = {}, u[3], v[3];
        helper[fabs(planes[p][0]) < 0.8 ? 0 : 1] = 1;
        Cross(planes[p], helper, u);
        Cross(planes[p], u, v);
        for (unsigned int i = 0; i < face->count; ++i)
        {
            double delta[3];
            for (int a = 0; a < 3; ++a)
            {
                delta[a] = points[face->vertices[i]][a] - center[a];
            }
            const double angle = atan2(Dot(delta, v), Dot(delta, u));
            const unsigned int vertex = face->vertices[i];
            unsigned int j = i;
            // Clockwise matches PlaneFromPoints' reversed cross product.
            while (j && face->angles[j - 1] < angle)
            {
                face->angles[j] = face->angles[j - 1];
                face->vertices[j] = face->vertices[j - 1];
                --j;
            }
            face->angles[j] = angle;
            face->vertices[j] = vertex;
        }
        double area = 0;
        for (unsigned int i = 0; i < face->count; ++i)
        {
            double a[3], b[3], cross[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                a[axis] = points[face->vertices[i]][axis] - center[axis];
                b[axis] = points[face->vertices[(i + 1) % face->count]][axis] - center[axis];
            }
            Cross(a, b, cross);
            area += Dot(cross, planes[p]);
        }
        if (fabs(area) < 1e-6)
        {
            face->count = 0;
        }
    }
    if (valid)
    {
        result->numsides = unique - 6;
        result->sides = result->numsides ? (cbrushside_t *)calloc(result->numsides, sizeof(cbrushside_t)) : NULL;
        result->planes = result->numsides ? (cplane_s *)calloc(result->numsides, sizeof(cplane_s)) : NULL;
        result->baseAdjacentSide = (uint8_t *)malloc(unique * 255);
        valid = result->baseAdjacentSide && (!result->numsides || (result->sides && result->planes));
    }
    for (unsigned int p = 0; valid && p < unique; ++p)
    {
        unsigned int edgeCount = 0;
        const unsigned int offset = result->totalEdgeCount;
        for (unsigned int i = 0; valid && i < faces[p].count; ++i)
        {
            const double *a = points[faces[p].vertices[(i + faces[p].count - 1) % faces[p].count]];
            const double *b = points[faces[p].vertices[i]];
            unsigned int adjacent = unique;
            for (unsigned int q = 0; q < unique; ++q)
            {
                if (q != p && faces[q].count >= 3 && fabs(Dot(planes[q], a) - planes[q][3]) <= 0.002 &&
                    fabs(Dot(planes[q], b) - planes[q][3]) <= 0.002)
                {
                    adjacent = q;
                    break;
                }
            }
            if (adjacent == unique || edgeCount == 255)
            {
                failedFace = (int)p;
                valid = false;
                break;
            }
            if (!edgeCount || result->baseAdjacentSide[offset + edgeCount - 1] != adjacent)
            {
                result->baseAdjacentSide[offset + edgeCount++] = (uint8_t)adjacent;
            }
        }
        if (edgeCount > 1 && result->baseAdjacentSide[offset] == result->baseAdjacentSide[offset + edgeCount - 1])
        {
            --edgeCount;
        }
        valid = valid && (!edgeCount || edgeCount >= 3);
        if (p < 6)
        {
            result->edgeCount[p % 2][p / 2] = (uint8_t)edgeCount;
            result->firstAdjacentSideOffsets[p % 2][p / 2] = (int16_t)offset;
        }
        else
        {
            cbrushside_t *side = &result->sides[p - 6];
            cplane_s *plane = &result->planes[p - 6];
            side->plane = plane;
            side->edgeCount = (uint8_t)edgeCount;
            side->firstAdjacentSideOffset = (int16_t)offset;
            plane->type = 3;
            for (int a = 0; a < 3; ++a)
            {
                plane->normal[a] = (float)planes[p][a];
                if (plane->normal[a] < 0)
                {
                    plane->signbits |= 1 << a;
                }
            }
            plane->dist = (float)planes[p][3];
        }
        result->totalEdgeCount += edgeCount;
    }
    free(faces);
    if (!valid || !result->totalEdgeCount)
    {
        Linker_FreePhysicsBrush(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid convex physics brush (%u planes, %u vertices, failed face %d)", unique,
                     pointCount, failedFace);
        }
        return false;
    }
    *brush = result;
    return true;
}
