#include <universal/q_shared.h>
#include <physics/phys_local.h>
#include "db_physics_geometry.h"
#include <limits.h>
#include <stdio.h>
#include <math.h>

static bool Invalid(char *error, size_t size, const char *text)
{
    if (size)
    {
        snprintf(error, size, "%s", text);
    }
    return false;
}

bool DB64_ValidateBrush(const BrushWrapper *brush, bool children, char *error, size_t errorSize)
{
    if (!brush || brush->numsides > 250 || brush->totalEdgeCount < 0 || brush->totalEdgeCount > 65536 ||
        (!!brush->sides != (brush->numsides != 0)) || (!!brush->planes != (brush->numsides != 0)) ||
        (!!brush->baseAdjacentSide != (brush->totalEdgeCount != 0)))
    {
        return Invalid(error, errorSize, "Invalid native physics brush arrays");
    }
    for (int i = 0; i < 3; ++i)
    {
        if (!isfinite(brush->mins[i]) || !isfinite(brush->maxs[i]) || brush->mins[i] > brush->maxs[i])
        {
            return Invalid(error, errorSize, "Invalid native physics brush bounds");
        }
        for (int j = 0; j < 2; ++j)
        {
            if (brush->edgeCount[j][i] &&
                (brush->firstAdjacentSideOffsets[j][i] < 0 ||
                 brush->firstAdjacentSideOffsets[j][i] + brush->edgeCount[j][i] > brush->totalEdgeCount))
            {
                return Invalid(error, errorSize, "Invalid native physics axial edge range");
            }
        }
    }
    if (!children)
    {
        return true;
    }
    for (unsigned int i = 0; i < brush->numsides; ++i)
    {
        const cbrushside_t *side = &brush->sides[i];
        if (!side->plane ||
            (side->edgeCount && (side->firstAdjacentSideOffset < 0 ||
                                 side->firstAdjacentSideOffset + side->edgeCount > brush->totalEdgeCount)))
        {
            return Invalid(error, errorSize, "Invalid native physics brush side");
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!isfinite(side->plane->normal[axis]))
            {
                return Invalid(error, errorSize, "Invalid native physics brush plane");
            }
        }
        if (!isfinite(side->plane->dist))
        {
            return Invalid(error, errorSize, "Invalid native physics brush plane distance");
        }
    }
    for (int i = 0; i < brush->totalEdgeCount; ++i)
    {
        if (brush->baseAdjacentSide[i] >= brush->numsides + 6)
        {
            return Invalid(error, errorSize, "Native physics edge references an invalid side");
        }
    }
    return true;
}

bool DB64_ValidatePhysicsGeometry(const PhysGeomList *list, char *error, size_t errorSize)
{
    if (!list || list->count > INT_MAX / sizeof(PhysGeomInfo) || (!!list->geoms != (list->count != 0)))
    {
        return Invalid(error, errorSize, "Invalid native physics geometry count");
    }
    for (int i = 0; i < 3; ++i)
    {
        if (!isfinite(list->mass.centerOfMass[i]) || !isfinite(list->mass.momentsOfInertia[i]) ||
            !isfinite(list->mass.productsOfInertia[i]))
        {
            return Invalid(error, errorSize, "Invalid native physics mass properties");
        }
    }
    for (unsigned int i = 0; i < list->count; ++i)
    {
        const PhysGeomInfo *geom = &list->geoms[i];
        if (geom->brush)
        {
            if (!DB64_ValidateBrush(geom->brush, true, error, errorSize))
            {
                return false;
            }
        }
        else if (geom->type != PHYS_GEOM_BOX && geom->type != PHYS_GEOM_CYLINDER)
        {
            return Invalid(error, errorSize, "Unsupported native model physics primitive");
        }
        if (!geom->brush)
        {
            const int dimensions = geom->type == PHYS_GEOM_CYLINDER ? 2 : 3;
            for (int axis = 0; axis < dimensions; ++axis)
            {
                if (!(geom->halfLengths[axis] > 0))
                {
                    return Invalid(error, errorSize, "Degenerate native physics primitive");
                }
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                for (int other = 0; other <= axis; ++other)
                {
                    double dot = 0;
                    for (int component = 0; component < 3; ++component)
                    {
                        dot += (double)geom->orientation[axis][component] * geom->orientation[other][component];
                    }
                    if (!isfinite(dot) || fabs(dot - (axis == other ? 1 : 0)) > 0.001)
                    {
                        return Invalid(error, errorSize, "Native physics primitive axes are not orthonormal");
                    }
                }
            }
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!isfinite(geom->offset[axis]) || !isfinite(geom->halfLengths[axis]) || geom->halfLengths[axis] < 0)
            {
                return Invalid(error, errorSize, "Invalid native physics primitive dimensions");
            }
            for (int j = 0; j < 3; ++j)
            {
                if (!isfinite(geom->orientation[axis][j]))
                {
                    return Invalid(error, errorSize, "Invalid native physics primitive orientation");
                }
            }
        }
    }
    return true;
}
