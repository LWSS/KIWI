#include <universal/q_shared.h>
#include <physics/phys_local.h>
#include "native_physmap.h"
#include "native_physbrush.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <float.h>

struct PhysicsCursor
{
    const char *text;
    size_t size, position;
    bool failed;
};
static bool Token(PhysicsCursor *cursor, char *token, size_t capacity)
{
    for (;;)
    {
        while (cursor->position < cursor->size && (unsigned char)cursor->text[cursor->position] <= 32)
        {
            ++cursor->position;
        }
        if (cursor->position + 1 < cursor->size && cursor->text[cursor->position] == '/' &&
            cursor->text[cursor->position + 1] == '/')
        {
            while (cursor->position < cursor->size && cursor->text[cursor->position] != '\n')
            {
                ++cursor->position;
            }
        }
        else
        {
            break;
        }
    }
    if (cursor->position == cursor->size)
    {
        return false;
    }
    const bool quoted = cursor->text[cursor->position] == '"';
    if (quoted)
    {
        ++cursor->position;
    }
    size_t length = 0;
    while (cursor->position < cursor->size)
    {
        const char value = cursor->text[cursor->position];
        if (quoted && value == '"')
        {
            ++cursor->position;
            token[length] = 0;
            return true;
        }
        if (!quoted &&
            ((unsigned char)value <= 32 || ((value == '{' || value == '}' || value == '(' || value == ')') && length)))
        {
            break;
        }
        if (length + 1 >= capacity || !value)
        {
            cursor->failed = true;
            return false;
        }
        token[length++] = value;
        ++cursor->position;
        if (!quoted && (value == '{' || value == '}' || value == '(' || value == ')'))
        {
            break;
        }
    }
    token[length] = 0;
    cursor->failed = quoted;
    return !quoted && length != 0;
}

static bool SkipEditorEntities(PhysicsCursor *cursor)
{
    char token[1024];
    while (Token(cursor, token, sizeof(token)))
    {
        if (strcmp(token, "{"))
        {
            return false;
        }
        unsigned int depth = 1;
        while (depth)
        {
            if (!Token(cursor, token, sizeof(token)))
            {
                return false;
            }
            if (!strcmp(token, "{"))
            {
                if (++depth > 32)
                {
                    return false;
                }
            }
            else if (!strcmp(token, "}"))
            {
                --depth;
            }
        }
    }
    return !cursor->failed && cursor->position == cursor->size;
}
static bool Match(PhysicsCursor *cursor, const char *expected)
{
    char token[256];
    return Token(cursor, token, sizeof(token)) && !strcmp(token, expected);
}
static bool Numbers(PhysicsCursor *cursor, float *values, unsigned int count)
{
    char token[256];
    for (unsigned int i = 0; i < count; ++i)
    {
        if (!Token(cursor, token, sizeof(token)))
        {
            return false;
        }
        char *end;
        values[i] = strtof(token, &end);
        if (end == token || *end || !isfinite(values[i]))
        {
            return false;
        }
    }
    return true;
}

void Linker_FreePhysicsMap(PhysGeomList *map)
{
    if (map)
    {
        for (unsigned int i = 0; i < map->count; ++i)
        {
            Linker_FreePhysicsBrush(map->geoms[i].brush);
        }
        free(map->geoms);
        free(map);
    }
}

static bool Primitive(PhysicsCursor *cursor, const char *type, PhysGeomInfo *geom)
{
    if (!Match(cursor, "{"))
    {
        return false;
    }
    if (!strcmp(type, "physics_box"))
    {
        geom->type = PHYS_GEOM_BOX;
        if (!Numbers(cursor, geom->orientation[0], 3) || !Numbers(cursor, geom->orientation[1], 3) ||
            !Numbers(cursor, geom->orientation[2], 3) || !Numbers(cursor, geom->offset, 3) ||
            !Numbers(cursor, geom->halfLengths, 3))
        {
            return false;
        }
        for (int i = 0; i < 3; ++i)
        {
            if (geom->halfLengths[i] <= 0)
            {
                return false;
            }
            for (int j = 0; j <= i; ++j)
            {
                double dot = 0;
                for (int a = 0; a < 3; ++a)
                {
                    dot += (double)geom->orientation[i][a] * geom->orientation[j][a];
                }
                if (fabs(dot - (i == j ? 1 : 0)) > 0.001)
                {
                    return false;
                }
            }
        }
    }
    else if (!strcmp(type, "physics_cylinder"))
    {
        float values[8];
        if (!Numbers(cursor, values, 8) || values[6] <= 0 || values[7] <= 0)
        {
            return false;
        }
        const double length =
            sqrt((double)values[0] * values[0] + (double)values[1] * values[1] + (double)values[2] * values[2]);
        if (!length)
        {
            return false;
        }
        geom->type = PHYS_GEOM_CYLINDER;
        for (int a = 0; a < 3; ++a)
        {
            geom->orientation[0][a] = (float)(values[a] / length);
            geom->offset[a] = values[a + 3];
        }
        const double yaw = values[0] || values[1] ? atan2(values[1], values[0]) : 0;
        geom->orientation[1][0] = (float)-sin(yaw);
        geom->orientation[1][1] = (float)cos(yaw);
        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3, c = (a + 2) % 3;
            geom->orientation[2][a] =
                geom->orientation[0][b] * geom->orientation[1][c] - geom->orientation[0][c] * geom->orientation[1][b];
        }
        geom->halfLengths[0] = values[6] * 0.5f;
        geom->halfLengths[1] = values[7];
    }
    else
    {
        return false;
    }
    return Match(cursor, "}") && Match(cursor, "}");
}

static bool InPrimitive(const double *point, const double *half, const PhysGeomInfo *geom)
{
    if (geom->brush)
    {
        const BrushWrapper *brush = geom->brush;
        for (int a = 0; a < 3; ++a)
        {
            if (brush->mins[a] >= point[a] + half[a] || brush->maxs[a] <= point[a] - half[a])
            {
                return false;
            }
        }
        for (unsigned int p = 0; p < brush->numsides; ++p)
        {
            const cplane_s *plane = brush->sides[p].plane;
            double distance = -plane->dist;
            for (int a = 0; a < 3; ++a)
            {
                distance += plane->normal[a] * point[a] - fabs(plane->normal[a]) * half[a];
            }
            if (distance >= 0)
            {
                return false;
            }
        }
        return true;
    }
    if (geom->type == PHYS_GEOM_CYLINDER)
    {
        double axial = 0, radialSquared = 0, axialRadius = 0, voxelRadiusSquared = 0;
        for (int a = 0; a < 3; ++a)
        {
            axial += geom->orientation[0][a] * (point[a] - geom->offset[a]);
            axialRadius += fabs(geom->orientation[0][a]) * half[a];
            voxelRadiusSquared += half[a] * half[a];
        }
        if (fabs(axial) >= geom->halfLengths[0] + axialRadius)
        {
            return false;
        }
        for (int a = 0; a < 3; ++a)
        {
            const double radial = point[a] - geom->offset[a] - axial * geom->orientation[0][a];
            radialSquared += radial * radial;
        }
        const double radius = geom->halfLengths[1] + sqrt(voxelRadiusSquared);
        return radialSquared < radius * radius;
    }
    for (int a = 0; a < 3; ++a)
    {
        double distance = 0, radius = 0;
        for (int b = 0; b < 3; ++b)
        {
            distance += geom->orientation[a][b] * (point[b] - geom->offset[b]);
            radius += fabs(geom->orientation[a][b]) * half[b];
        }
        radius += geom->halfLengths[a];
        if (fabs(distance) >= radius)
        {
            return false;
        }
    }
    return true;
}

static bool Mass(PhysGeomList *map)
{
    // Single primitives have exact unit-mass tensors; compounds retain voxel union integration.
    if (map->count == 1 && !map->geoms[0].brush)
    {
        const PhysGeomInfo *geom = &map->geoms[0];
        double diagonal[3], tensor[3][3] = {};
        if (geom->type == PHYS_GEOM_CYLINDER)
        {
            const double radius = geom->halfLengths[1], halfLength = geom->halfLengths[0];
            diagonal[0] = radius * radius * 0.5;
            diagonal[1] = diagonal[2] = radius * radius * 0.25 + halfLength * halfLength / 3;
        }
        else
        {
            for (int a = 0; a < 3; ++a)
            {
                const double first = geom->halfLengths[(a + 1) % 3], second = geom->halfLengths[(a + 2) % 3];
                diagonal[a] = (first * first + second * second) / 3;
            }
        }
        for (int a = 0; a < 3; ++a)
        {
            map->mass.centerOfMass[a] = geom->offset[a];
            for (int b = 0; b < 3; ++b)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    tensor[a][b] += diagonal[axis] * geom->orientation[axis][a] * geom->orientation[axis][b];
                }
                if (!isfinite(tensor[a][b]) || fabs(tensor[a][b]) > FLT_MAX)
                {
                    return false;
                }
            }
            map->mass.momentsOfInertia[a] = (float)tensor[a][a];
            if (map->mass.momentsOfInertia[a] <= 0)
            {
                return false;
            }
        }
        map->mass.productsOfInertia[0] = (float)tensor[0][1];
        map->mass.productsOfInertia[1] = (float)tensor[0][2];
        map->mass.productsOfInertia[2] = (float)tensor[1][2];
        return true;
    }

    double mins[3] = {DBL_MAX, DBL_MAX, DBL_MAX}, maxs[3] = {-DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (unsigned int i = 0; i < map->count; ++i)
    {
        const PhysGeomInfo *geom = &map->geoms[i];
        for (int a = 0; a < 3; ++a)
        {
            if (geom->brush)
            {
                mins[a] = fmin(mins[a], geom->brush->mins[a]);
                maxs[a] = fmax(maxs[a], geom->brush->maxs[a]);
                continue;
            }
            double radius = 0;
            for (int b = 0; b < 3; ++b)
            {
                const double extent = geom->halfLengths[geom->type == PHYS_GEOM_CYLINDER && b == 2 ? 1 : b];
                radius += fabs(geom->orientation[b][a]) * extent;
            }
            mins[a] = fmin(mins[a], geom->offset[a] - radius);
            maxs[a] = fmax(maxs[a], geom->offset[a] + radius);
        }
    }
    double half[3], reference[3], sum[3] = {}, products[3][3] = {};
    for (int a = 0; a < 3; ++a)
    {
        reference[a] = (mins[a] + maxs[a]) * 0.5;
        half[a] = (maxs[a] - mins[a]) / 64;
        if (!isfinite(half[a]) || half[a] <= 0)
        {
            return false;
        }
    }
    unsigned int count = 0;
    for (int x = 0; x < 32; ++x)
    {
        for (int y = 0; y < 32; ++y)
        {
            for (int z = 0; z < 32; ++z)
            {
                const double point[3] = {mins[0] + (2 * x + 1) * half[0], mins[1] + (2 * y + 1) * half[1],
                                         mins[2] + (2 * z + 1) * half[2]};
                bool inside = false;
                for (unsigned int i = 0; !inside && i < map->count; ++i)
                {
                    inside = InPrimitive(point, half, &map->geoms[i]);
                }
                if (!inside)
                {
                    continue;
                }
                ++count;
                for (int a = 0; a < 3; ++a)
                {
                    sum[a] += point[a] - reference[a];
                    for (int b = 0; b < 3; ++b)
                    {
                        products[a][b] += (point[a] - reference[a]) * (point[b] - reference[b]);
                    }
                }
            }
        }
    }
    if (!count)
    {
        return false;
    }
    for (int a = 0; a < 3; ++a)
    {
        sum[a] /= count;
        map->mass.centerOfMass[a] = (float)(sum[a] + reference[a]);
    }
    for (int a = 0; a < 3; ++a)
    {
        for (int b = 0; b < 3; ++b)
        {
            products[a][b] = products[a][b] / count - sum[a] * sum[b];
        }
    }
    for (int a = 0; a < 3; ++a)
    {
        map->mass.momentsOfInertia[a] =
            (float)(products[(a + 1) % 3][(a + 1) % 3] + products[(a + 2) % 3][(a + 2) % 3]);
        if (!isfinite(map->mass.centerOfMass[a]) || !isfinite(map->mass.momentsOfInertia[a]) ||
            map->mass.momentsOfInertia[a] <= 0)
        {
            return false;
        }
    }
    map->mass.productsOfInertia[0] = (float)-products[0][1];
    map->mass.productsOfInertia[1] = (float)-products[0][2];
    map->mass.productsOfInertia[2] = (float)-products[1][2];
    return isfinite(map->mass.productsOfInertia[0]) && isfinite(map->mass.productsOfInertia[1]) &&
           isfinite(map->mass.productsOfInertia[2]);
}

static bool Brush(PhysicsCursor *cursor, PhysGeomInfo *geom, char *error, size_t errorSize)
{
    double planes[32][4];
    unsigned int count = 0;
    char token[256];
    bool closed = false;
    while (Token(cursor, token, sizeof(token)))
    {
        if (!strcmp(token, "}"))
        {
            closed = true;
            break;
        }
        if (!strcmp(token, "layer") || !strcmp(token, "contents") || !strcmp(token, "toolFlags"))
        {
            while (cursor->position < cursor->size && cursor->text[cursor->position] != '\n')
            {
                ++cursor->position;
            }
            continue;
        }
        float points[3][3];
        if (strcmp(token, "(") || count == 32 || !Numbers(cursor, points[0], 3) || !Match(cursor, ")") ||
            !Match(cursor, "(") || !Numbers(cursor, points[1], 3) || !Match(cursor, ")") || !Match(cursor, "(") ||
            !Numbers(cursor, points[2], 3) || !Match(cursor, ")"))
        {
            return false;
        }
        double first[3], second[3], length = 0;
        for (int a = 0; a < 3; ++a)
        {
            first[a] = (double)points[1][a] - points[0][a];
            second[a] = (double)points[2][a] - points[0][a];
        }
        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3, c = (a + 2) % 3;
            planes[count][a] = second[b] * first[c] - second[c] * first[b];
            length += planes[count][a] * planes[count][a];
        }
        length = sqrt(length);
        if (!isfinite(length) || length <= 1e-10)
        {
            return false;
        }
        planes[count][3] = 0;
        for (int a = 0; a < 3; ++a)
        {
            planes[count][a] /= length;
            planes[count][3] += planes[count][a] * points[0][a];
        }
        ++count;
        while (cursor->position < cursor->size && cursor->text[cursor->position] != '\n')
        {
            ++cursor->position;
        }
    }
    return closed && Linker_BuildPhysicsBrush(planes, count, &geom->brush, error, errorSize);
}

bool Linker_ImportPhysicsMap(const void *data, size_t size, PhysGeomList **map, char *error, size_t errorSize)
{
    if (!map || (!error && errorSize))
    {
        return false;
    }
    *map = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    if (!data || !size || memchr(data, 0, size))
    {
        return false;
    }
    PhysicsCursor cursor = {(const char *)data, size, 0};
    char token[1024];
    bool valid = Match(&cursor, "iwmap") && Match(&cursor, "4");
    bool entity = false;
    while (valid && Token(&cursor, token, sizeof(token)))
    {
        if (!strcmp(token, "{"))
        {
            entity = true;
            break;
        }
    }
    PhysGeomList *result = valid && entity ? (PhysGeomList *)calloc(1, sizeof(PhysGeomList)) : NULL;
    valid = valid && result;
    bool closed = false;
    while (valid && Token(&cursor, token, sizeof(token)))
    {
        if (!strcmp(token, "}"))
        {
            closed = true;
            break;
        }
        if (strcmp(token, "{"))
        {
            valid = Token(&cursor, token, sizeof(token)) && strcmp(token, "{") && strcmp(token, "}");
            continue;
        }
        PhysGeomInfo geom = {};
        const size_t body = cursor.position;
        valid = result->count < 256 && Token(&cursor, token, sizeof(token));
        if (valid)
        {
            if (!strcmp(token, "physics_box") || !strcmp(token, "physics_cylinder"))
            {
                valid = Primitive(&cursor, token, &geom);
            }
            else
            {
                cursor.position = body;
                valid = Brush(&cursor, &geom, error, errorSize);
            }
        }
        if (valid)
        {
            PhysGeomInfo *geoms = (PhysGeomInfo *)realloc(result->geoms, (result->count + 1) * sizeof(PhysGeomInfo));
            valid = geoms != NULL;
            if (valid)
            {
                result->geoms = geoms;
                result->geoms[result->count++] = geom;
            }
        }
        if (!valid)
        {
            Linker_FreePhysicsBrush(geom.brush);
        }
    }
    valid = valid && closed && result->count && SkipEditorEntities(&cursor) && Mass(result);
    if (!valid)
    {
        Linker_FreePhysicsMap(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid iwmap 4 physics map, geometry or mass properties");
        }
        return false;
    }
    *map = result;
    return true;
}
