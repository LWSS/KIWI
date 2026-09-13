#include <universal/q_shared.h>
#include <physics/phys_local.h>
#include "database.h"
#include "db_physics_geometry.h"
#include <limits.h>

static bool LoadArray(void **array, size_t size, int alignment)
{
    if ((uintptr_t)*array == UINTPTR_MAX)
    {
        *array = DB_AllocStreamPos(alignment);
        Load_Stream(true, (uint8_t *)*array, size);
        return true;
    }
    if (*array)
    {
        DB64_ConvertOffsetRange((uintptr_t *)array, size);
    }
    return false;
}

static void LoadBrush(BrushWrapper **brush)
{
    if (!LoadArray((void **)brush, sizeof(BrushWrapper), 15))
    {
        return;
    }
    BrushWrapper *value = *brush;
    char error[256];
    if (!DB64_ValidateBrush(value, false, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
    // Native brushes put their plane storage before side pointers, allowing
    // sides to refer directly to the shared plane array.
    LoadArray((void **)&value->planes, value->numsides * sizeof(cplane_s), 15);
    if (LoadArray((void **)&value->sides, value->numsides * sizeof(cbrushside_t), 15))
    {
        for (unsigned int i = 0; i < value->numsides; ++i)
        {
            LoadArray((void **)&value->sides[i].plane, sizeof(cplane_s), 15);
        }
    }
    LoadArray((void **)&value->baseAdjacentSide, value->totalEdgeCount * sizeof(uint8_t), 0);
}

void DB64_LoadPhysicsGeometry(PhysGeomList **list)
{
    if (!LoadArray((void **)list, sizeof(PhysGeomList), 15))
    {
        return;
    }
    PhysGeomList *value = *list;
    if (value->count > INT_MAX / sizeof(PhysGeomInfo) || (!!value->geoms != (value->count != 0)))
    {
        Com_Error(ERR_DROP, "Invalid native physics geometry count");
    }
    if (LoadArray((void **)&value->geoms, value->count * sizeof(PhysGeomInfo), 15))
    {
        for (unsigned int i = 0; i < value->count; ++i)
        {
            LoadBrush(&value->geoms[i].brush);
        }
    }
    char error[256];
    if (!DB64_ValidatePhysicsGeometry(value, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
}
