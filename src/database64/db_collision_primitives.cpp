#include <universal/q_shared.h>
#include "database.h"
#include "db_collision_primitives.h"

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

void DB64_LoadBrushSide(cbrushside_t *side, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)side, sizeof(cbrushside_t));
    if (!side->plane)
    {
        Com_Error(ERR_DROP, "Native collision brush side has no plane");
    }
    LoadArray((void **)&side->plane, sizeof(cplane_s), 15);
}

void DB64_LoadCollisionPartition(CollisionPartition *partition, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)partition, sizeof(CollisionPartition));
    if ((!!partition->borders != (partition->borderCount != 0)) || partition->firstTri < 0)
    {
        Com_Error(ERR_DROP, "Invalid native collision partition");
    }
    LoadArray((void **)&partition->borders, partition->borderCount * sizeof(CollisionBorder), 15);
}

static size_t EdgeExtent(size_t current, int offset, unsigned int count)
{
    if (!count)
    {
        return current;
    }
    if (offset < 0)
    {
        Com_Error(ERR_DROP, "Negative native collision edge offset");
    }
    const size_t end = (size_t)offset + count;
    return end > current ? end : current;
}

void DB64_LoadCollisionBrush(cbrush_t *brush, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)brush, sizeof(cbrush_t));
    // Adjacent-side entries are bytes and include the six implicit axial sides.
    if (brush->numsides > 250 || (!!brush->sides != (brush->numsides != 0)))
    {
        Com_Error(ERR_DROP, "Invalid native collision brush side count");
    }
    if (LoadArray((void **)&brush->sides, brush->numsides * sizeof(cbrushside_t), 15))
    {
        for (unsigned int i = 0; i < brush->numsides; ++i)
        {
            DB64_LoadBrushSide(&brush->sides[i], false);
        }
    }
    size_t edges = 0;
    for (int direction = 0; direction < 2; ++direction)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            edges =
                EdgeExtent(edges, brush->firstAdjacentSideOffsets[direction][axis], brush->edgeCount[direction][axis]);
        }
    }
    for (unsigned int i = 0; i < brush->numsides; ++i)
    {
        edges = EdgeExtent(edges, brush->sides[i].firstAdjacentSideOffset, brush->sides[i].edgeCount);
    }
    if (edges && !brush->baseAdjacentSide)
    {
        Com_Error(ERR_DROP, "Native collision brush is missing its edges");
    }
    LoadArray((void **)&brush->baseAdjacentSide, edges, 0);
    // Gaps between edge spans need not be initialized. Validate used entries only.
    for (unsigned int sideIndex = 0; sideIndex < brush->numsides + 6; ++sideIndex)
    {
        const int offset = sideIndex < 6 ? brush->firstAdjacentSideOffsets[sideIndex & 1][sideIndex >> 1]
                                         : brush->sides[sideIndex - 6].firstAdjacentSideOffset;
        const unsigned int count =
            sideIndex < 6 ? brush->edgeCount[sideIndex & 1][sideIndex >> 1] : brush->sides[sideIndex - 6].edgeCount;
        for (unsigned int i = 0; i < count; ++i)
        {
            if (brush->baseAdjacentSide[offset + i] >= brush->numsides + 6)
            {
                Com_Error(ERR_DROP, "Native collision edge references an invalid side");
            }
        }
    }
}
