/*
shadows_midpoint.c — Shadow midpoint auxiliary vertex system

Reconstructed from cod2map.exe by Rose.

The midpoint shadow system projects caster windings through the shadow
view-projection matrix, clips them against occluders, fixes T-junctions,
simplifies concave holes, and triangulates the results for shadow mesh
emission. Uses SmAuxVert_t for projected vertex data.
*/

#include "cod4map.h"

/* Native TRIS_TRANSIENT_LMAP state, recovered from 0x451980.  This coexists
   with the older donor lightmap pass until the native caller order is wired. */
static TrisLmapGroup_t *s_trisLmapGroupList;
static int s_trisLmapGroupCount;
static char s_assertDisable_TrisLmapCreateSurface;
static char s_assertDisable_TrisLmapValidateSurface;

/* 0x4540B0/0x455370 reserve 2448 material heads while assembling the native
   assignment order.  The heads use TrisLmapTransient_t::nextInMaterial. */
/* 0x451600 reserves 0x4c8 material-list entries on the stack. */
#define TRIS_LMAP_MAX_MATERIALS 1224
static TriSurf_t *s_trisLmapMaterialHeads[TRIS_LMAP_MAX_MATERIALS];
static int s_trisLmapMaterialCount;
static TrisLmapAssignmentSidecar_t *s_trisLmapAssignments;
static TrisLmapRasterScratch_t s_trisLmapRasterScratch;
static int s_trisLmapNativeRouteEnabled;

#define TRIS_LMAP_GROUP_EPSILON 0.00007324219f
#define TRIS_LMAP_ASSIGNMENT_EPSILON 0.000097656251f

static int TrisLmap_TexelFloor(float uv);
static int TrisLmap_TexelCeil(float uv);
static void TrisLmap_ValidateGroup(const TrisLmapGroup_t *group);

void TrisLmap_SetNativeRouteEnabled(int enabled)
{
    s_trisLmapNativeRouteEnabled = enabled != 0;
}

int TrisLmap_NativeRouteEnabled(void)
{
    return s_trisLmapNativeRouteEnabled;
}

/* The native carrier stores group/index at +04/+08 and lmap vecs at +2c/+3c.
   This list carries the per-surface vector payload needed by KIWI's output
   bridge while the core property object remains the exact 0xAC ABI. */
const TrisLmapAssignmentPayload_t *TrisLmap_GetAssignment(const TriSurf_t *surf, const TriSurfProps_t *props)
{
    TrisLmapAssignmentSidecar_t *entry;

    for (entry = s_trisLmapAssignments; entry; entry = entry->next)
    {
        if (entry->surf == surf && entry->props == props)
            return &entry->payload;
    }
    return NULL;
}

/* 0x455070: a native clone is needed only after a property has an assignment
   and its group/index/vectors cease to agree with the requested assignment. */
int TrisLmap_AssignmentNeedsClone(const TriSurf_t *surf, const TriSurfProps_t *props,
                                  const TrisLmapAssignmentPayload_t *payload)
{
    const TrisLmapAssignmentPayload_t *existing;
    TrisLmapAssignmentSidecar_t *entry;

    Assert(surf, s_assertDisable_TrisLmapCreateSurface);
    Assert(props, s_assertDisable_TrisLmapCreateSurface);
    Assert(payload, s_assertDisable_TrisLmapCreateSurface);

    /* Native 0x455070 observes the property object, not just the current
       surface.  Once one surface has claimed a shared props object, another
       group must clone on any incompatible assignment. */
    existing = TrisLmap_GetAssignment(surf, props);
    if (!existing)
    {
        for (entry = s_trisLmapAssignments; entry; entry = entry->next)
        {
            if (entry->props == props)
            {
                existing = &entry->payload;
                break;
            }
        }
    }
    if (!existing)
        return 0; /* native props->groupId == -1 */
    if (existing->groupId != payload->groupId)
        return 1;
    if (existing->lightmapIndex != LIGHTSTYLE_NONE
        && existing->lightmapIndex != payload->lightmapIndex)
    {
        return 1;
    }
    return !VectorCompareEpsilon((float *)existing->vecs[0], (float *)payload->vecs[0], TRIS_LMAP_ASSIGNMENT_EPSILON, 4)
        || !VectorCompareEpsilon((float *)existing->vecs[1], (float *)payload->vecs[1], TRIS_LMAP_ASSIGNMENT_EPSILON, 4);
}

void TrisLmap_SetAssignment(TriSurf_t *surf, TriSurfProps_t *props,
                            const TrisLmapAssignmentPayload_t *payload)
{
    TrisLmapAssignmentSidecar_t *entry;

    Assert(surf, s_assertDisable_TrisLmapCreateSurface);
    Assert(props, s_assertDisable_TrisLmapCreateSurface);
    Assert(payload, s_assertDisable_TrisLmapCreateSurface);

    for (entry = s_trisLmapAssignments; entry; entry = entry->next)
    {
        if (entry->surf == surf && entry->props == props)
        {
            memcpy(&entry->payload, payload, sizeof(entry->payload));
            return;
        }
    }

    entry = (TrisLmapAssignmentSidecar_t *)malloc(sizeof(*entry));
    if (!entry)
        Com_Error("TrisLmap_SetAssignment: out of memory");
    entry->surf = surf;
    entry->props = props;
    memcpy(&entry->payload, payload, sizeof(entry->payload));
    entry->next = s_trisLmapAssignments;
    s_trisLmapAssignments = entry;
}

/* Mirror 0x43DC80/0x44D3B0 for one explicit source/clone props pair.  The
   caller walks cloned coalesce nodes with their corresponding source nodes. */
void TrisLmap_CopyAssignmentForPropsClone(TriSurf_t *sourceSurf, TriSurfProps_t *sourceProps,
                                          TriSurf_t *cloneSurf, TriSurfProps_t *cloneProps)
{
    const TrisLmapAssignmentPayload_t *payload;

    Assert(sourceSurf, s_assertDisable_TrisLmapCreateSurface);
    Assert(sourceProps, s_assertDisable_TrisLmapCreateSurface);
    Assert(cloneSurf, s_assertDisable_TrisLmapCreateSurface);
    Assert(cloneProps, s_assertDisable_TrisLmapCreateSurface);

    payload = TrisLmap_GetAssignment(sourceSurf, sourceProps);
    if (payload)
        TrisLmap_SetAssignment(cloneSurf, cloneProps, payload);
}

/* Surface copies preserve all per-property sidecars.  This is a no-op on the
   donor route, because no assignment sidecars are created until M5 wiring. */
void TrisLmap_CopyAssignmentsForSurface(TriSurf_t *sourceSurf, TriSurf_t *cloneSurf)
{
    TrisLmapAssignmentSidecar_t *entry;

    Assert(sourceSurf, s_assertDisable_TrisLmapCreateSurface);
    Assert(cloneSurf, s_assertDisable_TrisLmapCreateSurface);
    for (entry = s_trisLmapAssignments; entry; entry = entry->next)
    {
        if (entry->surf == sourceSurf)
            TrisLmap_SetAssignment(cloneSurf, entry->props, &entry->payload);
    }
}

void TrisLmap_FreeAssignmentsForSurface(TriSurf_t *surf)
{
    TrisLmapAssignmentSidecar_t **entryLink;
    TrisLmapAssignmentSidecar_t *entry;

    if (!surf)
        return;
    entryLink = &s_trisLmapAssignments;
    while (*entryLink)
    {
        entry = *entryLink;
        if (entry->surf == surf)
        {
            *entryLink = entry->next;
            free(entry);
        }
        else
        {
            entryLink = &entry->next;
        }
    }
}

static void TrisLmap_FreeAllAssignments(void)
{
    TrisLmapAssignmentSidecar_t *entry;
    TrisLmapAssignmentSidecar_t *next;

    for (entry = s_trisLmapAssignments; entry; entry = next)
    {
        next = entry->next;
        free(entry);
    }
    s_trisLmapAssignments = NULL;
}

/* 0x4542C0.  The native allocator consumes the transient lmap carrier as
   soon as its group has assigned the final props values.  The output path
   thereafter reads the assignment sidecar, so retaining the carrier until
   each TriSurf_t is destroyed is both non-native and needlessly expensive on
   production maps. */
static void TrisLmap_FreeAllGroups(void)
{
    while (s_trisLmapGroupList)
    {
        TrisLmapGroup_t *group = s_trisLmapGroupList;
        TriSurf_t *surf;

        for (surf = group->firstSurf; surf; )
        {
            TriSurf_t *nextSurf = surf->lmap->nextInGroup;

            free(surf->lmap);
            surf->lmap = NULL;
            surf = nextSurf;
        }
        s_trisLmapGroupList = group->next;
        free(group->usedGroupBits);
        free(group);
    }
    s_trisLmapGroupCount = 0;
}

/* 0x4540B0 owns this one-megabyte table for the complete assignment pass;
   0x454820 addresses it as span[x + y * 512]. */
void TrisLmap_BeginRasterScratch(void)
{
    Assert(!s_trisLmapRasterScratch.span, s_assertDisable_TrisLmapCreateSurface);
    s_trisLmapRasterScratch.span = (TrisLmapSpan_t *)malloc(LIGHTMAP_TEXELS * sizeof(TrisLmapSpan_t));
    if (!s_trisLmapRasterScratch.span)
        Com_Error("TrisLmap_BeginRasterScratch: out of memory");
    s_trisLmapRasterScratch.originX = 0;
    s_trisLmapRasterScratch.originY = 0;
}

void TrisLmap_EndRasterScratch(void)
{
    free(s_trisLmapRasterScratch.span);
    s_trisLmapRasterScratch.span = NULL;
    s_trisLmapRasterScratch.originX = 0;
    s_trisLmapRasterScratch.originY = 0;
}

/* 0x4547E9: small allocations skip the span/raster reclaim pass entirely. */
int TrisLmap_ShouldRasterGroup(int width, int height)
{
    return width >= 8 && height >= 8;
}

static TrisLmapSpan_t *TrisLmap_GetSpan(int x, int y)
{
    Assert(s_trisLmapRasterScratch.span, s_assertDisable_TrisLmapCreateSurface);
    Assert(x >= 0 && x < LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);
    Assert(y >= 0 && y < LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);
    return &s_trisLmapRasterScratch.span[x + y * LIGHTMAP_SIZE];
}

/* 0x454820 initial fill.  The two span components measure free extent right
   and down, inclusive, for every texel in this assigned group rectangle. */
void TrisLmap_InitRasterRegion(int originX, int originY, int width, int height)
{
    int x;
    int y;
    int endX;
    int endY;

    Assert(width >= 8 && width <= LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);
    Assert(height >= 8 && height <= LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);
    Assert(originX >= 0 && originY >= 0, s_assertDisable_TrisLmapCreateSurface);
    endX = originX + width - 1;
    endY = originY + height - 1;
    Assert(endX < LIGHTMAP_SIZE && endY < LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);

    for (y = originY; y <= endY; ++y)
    {
        for (x = originX; x <= endX; ++x)
        {
            TrisLmapSpan_t *span = TrisLmap_GetSpan(x, y);
            span->width = (unsigned short)(endX - x + 1);
            span->height = (unsigned short)(endY - y + 1);
        }
    }
    s_trisLmapRasterScratch.originX = originX;
    s_trisLmapRasterScratch.originY = originY;
}

/* 0x454C60.  Clearing a projected triangle bounds also repairs the free-span
   values immediately left of, and immediately above, the cleared rectangle. */
static void TrisLmap_ClearRasterRect(int minX, int minY, int maxX, int maxY)
{
    int x;
    int y;

    for (y = minY; y <= maxY; ++y)
    {
        for (x = minX; x <= maxX; ++x)
        {
            TrisLmapSpan_t *span = TrisLmap_GetSpan(x, y);
            span->width = 0;
            span->height = 0;
        }
        for (x = minX - 1; x >= s_trisLmapRasterScratch.originX && TrisLmap_GetSpan(x, y)->width > 0; --x)
            TrisLmap_GetSpan(x, y)->width = (unsigned short)(minX - x);
    }
    for (x = minX; x <= maxX; ++x)
    {
        for (y = minY - 1; y >= s_trisLmapRasterScratch.originY && TrisLmap_GetSpan(x, y)->height > 0; --y)
            TrisLmap_GetSpan(x, y)->height = (unsigned short)(minY - y);
    }
}

static int TrisLmap_ClampInt(int value, int low, int high)
{
    return value < low ? low : (value > high ? high : value);
}

/* 0x454F30 -> 0x453F30 -> 0x454E70.  The native reclaim pass clears the
   projected triangle AABB, after the tesselator has produced each triangle. */
void TrisLmap_RasterTriangle(const float *lmapVecs, const float *p0, const float *p1, const float *p2)
{
    const float *points[3];
    float mins[2];
    float maxs[2];
    float uv[2];
    int index;
    int minX;
    int minY;
    int maxX;
    int maxY;

    Assert(lmapVecs && p0 && p1 && p2, s_assertDisable_TrisLmapCreateSurface);
    points[0] = p0;
    points[1] = p1;
    points[2] = p2;
    for (index = 0; index != 3; ++index)
    {
        uv[0] = DotProduct(points[index], lmapVecs) + lmapVecs[3];
        uv[1] = DotProduct(points[index], lmapVecs + 4) + lmapVecs[7];
        if (!index)
        {
            mins[0] = maxs[0] = uv[0];
            mins[1] = maxs[1] = uv[1];
        }
        else
        {
            if (uv[0] < mins[0]) mins[0] = uv[0];
            if (uv[1] < mins[1]) mins[1] = uv[1];
            if (uv[0] > maxs[0]) maxs[0] = uv[0];
            if (uv[1] > maxs[1]) maxs[1] = uv[1];
        }
    }

    minX = TrisLmap_ClampInt(TrisLmap_TexelFloor(mins[0]), 0, LIGHTMAP_SIZE - 1);
    minY = TrisLmap_ClampInt(TrisLmap_TexelFloor(mins[1]), 0, LIGHTMAP_SIZE - 1);
    maxX = TrisLmap_ClampInt(TrisLmap_TexelCeil(maxs[0]), 0, LIGHTMAP_SIZE - 1);
    maxY = TrisLmap_ClampInt(TrisLmap_TexelCeil(maxs[1]), 0, LIGHTMAP_SIZE - 1);
    TrisLmap_ClearRasterRect(minX, minY, maxX, maxY);
}

static int TrisLmap_SpanScore(int width, int height)
{
    return height + width + height * width;
}

/* 0x454B20/0x454DB0.  Reclaim every best rectangle of at least 2x2, with
   ties resolved by the later scan position just as the native <= update does. */
int TrisLmap_ReclaimRasterRegion(int lightmapIndex, int originX, int originY, int width, int height)
{
    int endX;
    int endY;
    int reclaimed;

    Assert(width >= 8 && width <= LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);
    Assert(height >= 8 && height <= LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);
    endX = originX + width - 1;
    endY = originY + height - 1;
    reclaimed = 0;
    for (;;)
    {
        int bestWidth = 2;
        int bestHeight = 2;
        int bestScore = TrisLmap_SpanScore(2, 2);
        int bestX = -1;
        int bestY = -1;
        int x;
        int y;

        for (y = originY; y <= endY; ++y)
        {
            for (x = originX; x <= endX; ++x)
            {
                TrisLmapSpan_t *span = TrisLmap_GetSpan(x, y);
                int row;
                int candidateWidth;

                if (TrisLmap_SpanScore(span->width, span->height) < bestScore)
                    continue;
                candidateWidth = 0x7fffffff;
                for (row = 1; row <= span->height; ++row)
                {
                    int score;
                    int rowWidth = TrisLmap_GetSpan(x, y + row - 1)->width;
                    if (rowWidth < candidateWidth)
                        candidateWidth = rowWidth;
                    if (row < 2)
                        continue;
                    if (candidateWidth < 2)
                        break;
                    score = TrisLmap_SpanScore(candidateWidth, row);
                    if (bestScore <= score)
                    {
                        bestScore = score;
                        bestWidth = candidateWidth;
                        bestHeight = row;
                        bestX = x;
                        bestY = y;
                    }
                }
            }
        }
        if (bestX == -1)
            break;
        TrisLmap_ClearRasterRect(bestX, bestY, bestX + bestWidth - 1, bestY + bestHeight - 1);
        FreeLMBlock(lightmapIndex, bestX, bestY, bestWidth, bestHeight);
        ++reclaimed;
    }
    return reclaimed;
}

static int TrisLmap_CompareSurfPropsAndGroup(const void *left, const void *right)
{
    const TriSurf_t *leftSurf = *(const TriSurf_t * const *)left;
    const TriSurf_t *rightSurf = *(const TriSurf_t * const *)right;

    if (leftSurf->props < rightSurf->props)
        return -1;
    if (leftSurf->props > rightSurf->props)
        return 1;
    if (leftSurf->lmap->group->id < rightSurf->lmap->group->id)
        return -1;
    if (leftSurf->lmap->group->id > rightSurf->lmap->group->id)
        return 1;
    return 0;
}

/* 0x453C60.  Native code gathers every lmap surface, sorts by props then
   group id, and keeps the first group's props while cloning the original
   props for every additional group.  This function is intentionally dormant
   until group creation and sidecar assignment are wired as one pipeline. */
int TrisLmap_SeparatePropsByGroup(void)
{
    TrisLmapGroup_t *group;
    TriSurf_t *surf;
    TriSurf_t **surfs;
    int count;
    int index;

    count = 0;
    for (group = s_trisLmapGroupList; group; group = group->next)
    {
        for (surf = group->firstSurf; surf; surf = surf->lmap->nextInGroup)
            ++count;
    }
    if (!count)
        return 0;

    surfs = (TriSurf_t **)malloc(count * sizeof(*surfs));
    if (!surfs)
        Com_Error("TrisLmap_SeparatePropsByGroup: out of memory");
    index = 0;
    for (group = s_trisLmapGroupList; group; group = group->next)
    {
        for (surf = group->firstSurf; surf; surf = surf->lmap->nextInGroup)
            surfs[index++] = surf;
    }
    Assert(index == count, s_assertDisable_TrisLmapCreateSurface);
    qsort(surfs, count, sizeof(*surfs), TrisLmap_CompareSurfPropsAndGroup);

    for (index = 0; index < count; )
    {
        TriSurfProps_t *sourceProps = surfs[index]->props;
        TriSurfProps_t *assignedProps = sourceProps;
        int groupId = surfs[index]->lmap->group->id;

        do
        {
            if (surfs[index]->lmap->group->id != groupId)
            {
                groupId = surfs[index]->lmap->group->id;
                assignedProps = TrisLmap_CloneProps(surfs[index], sourceProps);
            }
            surfs[index]->props = assignedProps;
            ++index;
        }
        while (index < count && surfs[index]->props == sourceProps);
    }
    free(surfs);
    return count;
}

/* 0x451290 — validate each surface coordinate against both its local and
   owning-group UV bounds. */
void TrisLmap_ValidateSurface(TriSurf_t *surf)
{
    TrisLmapTransient_t *lmap;
    float uv[2];
    int index;

    Assert(surf, s_assertDisable_TrisLmapValidateSurface);
    Assert(surf->lmap, s_assertDisable_TrisLmapValidateSurface);
    Assert(surf->lmap->group, s_assertDisable_TrisLmapValidateSurface);
    Assert(surf->props, s_assertDisable_TrisLmapValidateSurface);

    lmap = surf->lmap;
    for (index = 0; index < surf->winding->numpoints; ++index)
    {
        uv[0] = DotProduct(surf->winding->points[index], lmap->vecs[0]) + lmap->vecs[0][3];
        uv[1] = DotProduct(surf->winding->points[index], lmap->vecs[1]) + lmap->vecs[1][3];
        Assert(PointInBounds2D(uv, lmap->bounds[0], lmap->bounds[1]), s_assertDisable_TrisLmapValidateSurface);
        Assert(PointInBounds2D(uv, lmap->group->bounds[0], lmap->group->bounds[1]), s_assertDisable_TrisLmapValidateSurface);
    }
}

/* 0x451980 — create the native 0x24 group and 0x40 per-surface carrier. */
TrisLmapGroup_t *TrisLmap_CreateSurface(TriSurf_t *surf)
{
    TrisLmapGroup_t *group;
    TrisLmapTransient_t *lmap;
    float uv[2];
    int index;

    Assert(surf, s_assertDisable_TrisLmapCreateSurface);
    Assert(surf->props, s_assertDisable_TrisLmapCreateSurface);
    Assert(surf->winding, s_assertDisable_TrisLmapCreateSurface);
    Assert(!surf->lmap, s_assertDisable_TrisLmapCreateSurface);

    group = (TrisLmapGroup_t *)malloc(sizeof(*group));
    lmap = (TrisLmapTransient_t *)malloc(sizeof(*lmap));
    if (!group || !lmap)
        Com_Error("TrisLmap_CreateSurface: out of memory");

    memset(group, 0, sizeof(*group));
    memset(lmap, 0, sizeof(*lmap));

    group->next = s_trisLmapGroupList;
    if (group->next)
        group->next->prev = group;
    s_trisLmapGroupList = group;
    group->id = s_trisLmapGroupCount++;
    group->firstSurf = surf;

    ClearBounds2D(group->bounds[0], group->bounds[1]);
    for (index = 0; index < surf->winding->numpoints; ++index)
    {
        uv[0] = DotProduct(surf->winding->points[index], &surf->props->lmapVecs[0]) + surf->props->lmapVecs[3];
        uv[1] = DotProduct(surf->winding->points[index], &surf->props->lmapVecs[4]) + surf->props->lmapVecs[7];
        AddPointToBounds2D(uv, group->bounds[0], group->bounds[1]);
    }

    group->bounds[0][0] -= TRIS_LMAP_GROUP_EPSILON;
    group->bounds[0][1] -= TRIS_LMAP_GROUP_EPSILON;
    group->bounds[1][0] += TRIS_LMAP_GROUP_EPSILON;
    group->bounds[1][1] += TRIS_LMAP_GROUP_EPSILON;

    surf->lmap = lmap;
    lmap->group = group;
    memcpy(lmap->bounds, group->bounds, sizeof(lmap->bounds));
    memcpy(lmap->vecs, surf->props->lmapVecs, sizeof(lmap->vecs));

    TrisLmap_ValidateSurface(surf);
    return group;
}

/* 0x452420 evaluates the shifted source group before changing either linked
   list.  The group bounds already include the 0x451980 epsilon, so they must
   be used directly here--padding them a second time changes texel-edge cases. */
int TrisLmap_CanAbsorbGroup(const TrisLmapGroup_t *groupGrow, const TrisLmapGroup_t *groupKill, const float *lmapShift)
{
    float shiftedKillMins[2];
    float shiftedKillMaxs[2];
    float mergedMins[2];
    float mergedMaxs[2];
    int axis;

    Assert(groupGrow, s_assertDisable_TrisLmapCreateSurface);
    Assert(groupKill, s_assertDisable_TrisLmapCreateSurface);
    Assert(lmapShift, s_assertDisable_TrisLmapCreateSurface);

    for (axis = 0; axis != 2; ++axis)
    {
        shiftedKillMins[axis] = groupKill->bounds[0][axis] + lmapShift[axis];
        shiftedKillMaxs[axis] = groupKill->bounds[1][axis] + lmapShift[axis];
        mergedMins[axis] = groupGrow->bounds[0][axis] < shiftedKillMins[axis]
            ? groupGrow->bounds[0][axis] : shiftedKillMins[axis];
        mergedMaxs[axis] = groupGrow->bounds[1][axis] > shiftedKillMaxs[axis]
            ? groupGrow->bounds[1][axis] : shiftedKillMaxs[axis];

        /* Native 0x452596/0x4525A8 is a >= 512 veto, rather than a later
           allocator clamp. */
        int mergedSize = LightmapTexelSize(mergedMins[axis], mergedMaxs[axis]);
        int growSize = LightmapTexelSize(groupGrow->bounds[0][axis], groupGrow->bounds[1][axis]);
        int killSize = LightmapTexelSize(groupKill->bounds[0][axis], groupKill->bounds[1][axis]);

        if (mergedSize >= LIGHTMAP_SIZE)
            return 0;
        /* 0x4525d2..0x452608 rejects a merged span with a gap larger than
           the two source texel spans plus their shared edge. */
        if (mergedSize > killSize + growSize + 1)
            return 0;
    }
    return 1;
}

/* 0x452420 -- make groupKill part of groupGrow after its UV space has been
   aligned by the touching test.  This is intentionally separate from the
   material/grid candidate search; until that port is wired it has no effect
   on the donor lightmap route. */
int TrisLmap_AbsorbGroup(TrisLmapGroup_t *groupGrow, TrisLmapGroup_t *groupKill, const float *lmapShift)
{
    TriSurf_t *surf;
    TriSurf_t *nextSurf;
    TriSurf_t *tail;
    float mergedMins[2];
    float mergedMaxs[2];
    int axis;

    Assert(groupGrow, s_assertDisable_TrisLmapCreateSurface);
    Assert(groupKill, s_assertDisable_TrisLmapCreateSurface);
    Assert(groupGrow != groupKill, s_assertDisable_TrisLmapCreateSurface);
    Assert(lmapShift, s_assertDisable_TrisLmapCreateSurface);

    if (!TrisLmap_CanAbsorbGroup(groupGrow, groupKill, lmapShift))
        return 0;

    for (axis = 0; axis != 2; ++axis)
    {
        float shiftedMin = groupKill->bounds[0][axis] + lmapShift[axis];
        float shiftedMax = groupKill->bounds[1][axis] + lmapShift[axis];
        mergedMins[axis] = groupGrow->bounds[0][axis] < shiftedMin ? groupGrow->bounds[0][axis] : shiftedMin;
        mergedMaxs[axis] = groupGrow->bounds[1][axis] > shiftedMax ? groupGrow->bounds[1][axis] : shiftedMax;
    }

    /* 0x45265C shifts the source group itself before 0x4526C1 validates
       its members.  Until ownership changes, 0x451290 therefore checks the
       shifted local coordinates against these shifted source bounds. */
    for (axis = 0; axis != 2; ++axis)
    {
        groupKill->bounds[0][axis] += lmapShift[axis];
        groupKill->bounds[1][axis] += lmapShift[axis];
    }

    /* Shift every per-surface coordinate system before relinking.  The two
       W components are the projected-UV offsets used by 0x451290. */
    for (surf = groupKill->firstSurf; surf; surf = nextSurf)
    {
        TrisLmapTransient_t *lmap = surf->lmap;

        nextSurf = lmap->nextInGroup;
        Assert(lmap->group == groupKill, s_assertDisable_TrisLmapCreateSurface);
        for (axis = 0; axis != 2; ++axis)
        {
            lmap->bounds[0][axis] += lmapShift[axis];
            lmap->bounds[1][axis] += lmapShift[axis];
            lmap->vecs[axis][3] += lmapShift[axis];
        }
        /* Native 0x4526C1 validates against the shifted source group before
           its later 0x4526FB pass changes lmap->group.  The destination
           bounds are not widened until 0x452760, so switching ownership here
           makes the exact group-bounds validation spuriously fail. */
        TrisLmap_ValidateSurface(surf);
    }

    for (surf = groupKill->firstSurf; surf; surf = surf->lmap->nextInGroup)
        surf->lmap->group = groupGrow;

    /* 0x452420 joins the kill list ahead of the grow list. */
    tail = groupKill->firstSurf;
    while (tail->lmap->nextInGroup)
        tail = tail->lmap->nextInGroup;
    tail->lmap->nextInGroup = groupGrow->firstSurf;
    if (groupGrow->firstSurf)
        groupGrow->firstSurf->lmap->prevInGroup = tail;
    groupGrow->firstSurf = groupKill->firstSurf;
    groupGrow->firstSurf->lmap->prevInGroup = NULL;

    memcpy(groupGrow->bounds[0], mergedMins, sizeof(mergedMins));
    memcpy(groupGrow->bounds[1], mergedMaxs, sizeof(mergedMaxs));

    if (groupKill->prev)
        groupKill->prev->next = groupKill->next;
    else
        s_trisLmapGroupList = groupKill->next;
    if (groupKill->next)
        groupKill->next->prev = groupKill->prev;
    free(groupKill->usedGroupBits);
    if (groupKill->id == s_trisLmapGroupCount - 1)
        --s_trisLmapGroupCount;
    free(groupKill);
    TrisLmap_ValidateGroup(groupGrow);
    return 1;
}

/* 0x455370 callback, expressed against the KIWI TriSurf_t layout.  A carrier
   exists only for surfaces accepted by the native 0x451590 eligibility pass;
   its `si` identity is the material identity used by this codebase. */
static void TrisLmap_CollectMaterialCallback(TriSurf_t *surf, bool isTarget)
{
    int index;

    (void)isTarget;
    if (!surf->lmap)
        return;

    Assert(surf->props, s_assertDisable_TrisLmapCreateSurface);
    Assert(surf->lmap->group, s_assertDisable_TrisLmapCreateSurface);
    TrisLmap_ValidateSurface(surf);

    for (index = 0; index < s_trisLmapMaterialCount; ++index)
    {
        if (s_trisLmapMaterialHeads[index]->props->si == surf->props->si)
        {
            surf->lmap->nextInMaterial = s_trisLmapMaterialHeads[index];
            s_trisLmapMaterialHeads[index] = surf;
            return;
        }
    }

    if (index == TRIS_LMAP_MAX_MATERIALS)
        Com_Error("TrisLmap_BuildMaterialLists: exceeded %i materials", TRIS_LMAP_MAX_MATERIALS);

    s_trisLmapMaterialHeads[index] = surf;
    surf->lmap->nextInMaterial = NULL;
    ++s_trisLmapMaterialCount;
}

/* Native 0x4540B0 calls 0x455370 through the TriSurf iterator to produce a
   per-material LIFO list.  Sorting and invoking the list stays gated until
   property emission (0x454F90) has an exact port. */
int TrisLmap_BuildMaterialLists(void)
{
    memset(s_trisLmapMaterialHeads, 0, sizeof(s_trisLmapMaterialHeads));
    s_trisLmapMaterialCount = 0;
    ForEachSurf(TrisLmap_CollectMaterialCallback, NULL);
    return s_trisLmapMaterialCount;
}

/* 0x4190F0: native x87 precision is material for texel-edge groups. */
static int TrisLmap_TexelFloor(float uv)
{
  volatile float texel = uv * (float)LIGHTMAP_SIZE - 0.5f;
  return (int)floor(texel);
}

/* 0x419120. */
static int TrisLmap_TexelCeil(float uv)
{
  volatile float texel = uv * (float)LIGHTMAP_SIZE + 0.5f;
  return (int)ceil(texel);
}

/* The allocation and vector-offset prefix of 0x454340.  Do not add the
   0x454820 raster/span reclaim or the 0x454F90 property clone here: both
   consume state that has not yet been recovered. */
int TrisLmap_AssignGroup(TrisLmapGroup_t *group, TrisLmapAllocation_t *allocation)
{
    int width;
    int height;
    int texelAxis0;
    int texelAxis1;
    int axis0;
    int axis1;
    float offset0;
    float offset1;
    TriSurf_t *surf;

    Assert(group, s_assertDisable_TrisLmapCreateSurface);
    Assert(group->firstSurf, s_assertDisable_TrisLmapCreateSurface);
    Assert(allocation, s_assertDisable_TrisLmapCreateSurface);

    /* sub_451C70(-0.00007324219, group->bounds): undo the create-time pad
       before calculating the native request size. */
    group->bounds[0][0] += TRIS_LMAP_GROUP_EPSILON;
    group->bounds[0][1] += TRIS_LMAP_GROUP_EPSILON;
    group->bounds[1][0] -= TRIS_LMAP_GROUP_EPSILON;
    group->bounds[1][1] -= TRIS_LMAP_GROUP_EPSILON;

    width = LightmapTexelSize(group->bounds[0][0], group->bounds[1][0]);
    height = LightmapTexelSize(group->bounds[0][1], group->bounds[1][1]);
    if (!AllocLMBlock(width, height, &allocation->lightmapIndex, &allocation->allocY,
                      &allocation->allocX, &allocation->isTransposed))
    {
        Com_Error("lightmap allocation failed... may be out of lightmap memory");
    }

    /* The allocator's y/x output order is intentional and matches 0x4544B9
       and 0x4544E2: the first output offsets the first projected axis. */
    axis0 = allocation->isTransposed ? 1 : 0;
    axis1 = allocation->isTransposed ? 0 : 1;
    texelAxis0 = TrisLmap_TexelFloor(group->bounds[0][axis0]);
    texelAxis1 = TrisLmap_TexelFloor(group->bounds[0][axis1]);
    offset0 = (float)(allocation->allocY - texelAxis0) * (1.0f / (float)LIGHTMAP_SIZE);
    offset1 = (float)(allocation->allocX - texelAxis1) * (1.0f / (float)LIGHTMAP_SIZE);

    for (surf = group->firstSurf; surf; surf = surf->lmap->nextInGroup)
    {
        Assert(surf->lmap->group == group, s_assertDisable_TrisLmapCreateSurface);
        surf->lmap->vecs[axis0][3] += offset0;
        surf->lmap->vecs[axis1][3] += offset1;
    }

    allocation->width = width;
    allocation->height = height;
    return 1;
}

/* 0x4552F0 and 0x455180.  Native ordering is significant because lightmap
   block allocation is first-fit: sort every material's LIFO surface list by
   group texel score before sorting the material heads themselves. */
static int TrisLmap_GroupScore(TriSurf_t *surf)
{
    TrisLmapGroup_t *group;
    int width;
    int height;

    if (!surf)
        return 0;
    group = surf->lmap->group;
    width = LightmapTexelSize(group->bounds[0][0], group->bounds[1][0]);
    height = LightmapTexelSize(group->bounds[0][1], group->bounds[1][1]);
    return TrisLmap_SpanScore(width, height);
}

static TriSurf_t *TrisLmap_SortMaterialList(TriSurf_t *head, int count)
{
    TriSurf_t *right;
    TriSurf_t *leftHead;
    TriSurf_t *rightHead;
    TriSurf_t **tail;
    int leftCount;
    int rightCount;
    int leftScore;
    int rightScore;

    Assert(count > 0, s_assertDisable_TrisLmapCreateSurface);
    if (count == 1)
        return head;
    right = head;
    leftCount = count / 2;
    for (int index = 0; index < leftCount; ++index)
        right = right->lmap->nextInMaterial;
    rightCount = count - leftCount;
    leftHead = TrisLmap_SortMaterialList(head, leftCount);
    rightHead = TrisLmap_SortMaterialList(right, rightCount);
    tail = &head;
    while (leftCount || rightCount)
    {
        leftScore = TrisLmap_GroupScore(leftHead);
        rightScore = TrisLmap_GroupScore(rightHead);
        if (!leftCount || (rightCount && leftScore <= rightScore))
        {
            *tail = rightHead;
            rightHead = rightHead->lmap->nextInMaterial;
            --rightCount;
        }
        else
        {
            *tail = leftHead;
            leftHead = leftHead->lmap->nextInMaterial;
            --leftCount;
        }
        tail = &(*tail)->lmap->nextInMaterial;
    }
    *tail = NULL;
    return head;
}

static int TrisLmap_CompareMaterialHeads(const void *left, const void *right)
{
    TriSurf_t *const *leftHead = (TriSurf_t *const *)left;
    TriSurf_t *const *rightHead = (TriSurf_t *const *)right;
    return TrisLmap_GroupScore(*rightHead) - TrisLmap_GroupScore(*leftHead);
}

/* 0x419040.  A material's already-used atlases are preferred for all of its
   subsequent allocations.  AllocLMBlock deliberately falls back to the full
   free list if this restricted list cannot satisfy a request. */
static void TrisLmap_AllowLightmap(int lightmapIndex)
{
    LmAllowedNode_t *node;

    for (node = lmAllowedList; node; node = node->next)
    {
        if (node->lmapIdx == lightmapIndex)
            return;
    }

    node = (LmAllowedNode_t *)malloc(sizeof(*node));
    if (!node)
        Com_Error("TrisLmap_AllowLightmap: out of memory");
    node->lmapIdx = lightmapIndex;
    node->next = lmAllowedList;
    lmAllowedList = node;
}

/* 0x4190A0.  The native count is used only for a diagnostic total by this
   caller; releasing the complete LIFO list at material boundaries is what
   makes the restriction material-local. */
static int TrisLmap_ClearAllowedLightmaps(void)
{
    int count = 0;

    while (lmAllowedList)
    {
        LmAllowedNode_t *next = lmAllowedList->next;
        free(lmAllowedList);
        lmAllowedList = next;
        ++count;
    }
    return count;
}

/* Native 0x454F90 mapped into the sidecar ABI.  It retains the native clone
   decision while never writing CoD4's incompatible fields into donor props. */
static void TrisLmap_SetGroupAssignment(TrisLmapGroup_t *group, const TrisLmapAllocation_t *allocation)
{
    TriSurf_t *surf;
    TrisLmapAssignmentPayload_t payload;

    payload.groupId = group->id;
    payload.lightmapIndex = allocation->lightmapIndex;
    for (surf = group->firstSurf; surf; surf = surf->lmap->nextInGroup)
    {
        TriSurfProps_t *props = surf->props;
        memcpy(payload.vecs, surf->lmap->vecs, sizeof(payload.vecs));
        if (TrisLmap_AssignmentNeedsClone(surf, props, &payload))
        {
            props = TrisLmap_CloneProps(surf, props);
            /* Native 0x454FE6/0x454FEC resets a clone's core atlas slot
               before 0x454F90 publishes the new group/index pair. */
            props->lmapIndex = 31;
            surf->props = props;
        }
        /* These are native TriSurfProps_t +04/+08 fields.  The assignment
           sidecar below retains the accompanying vector payload without
           expanding the 0xAC property carrier. */
        props->lmapGroupId = payload.groupId;
        props->lmapIndex = payload.lightmapIndex;
        TrisLmap_SetAssignment(surf, props, &payload);
    }
}

/* 0x454F30 callback passed by 0x454820 into the native 0x45B6F0 route. */
static void TrisLmap_RasterTessCallback(Winding_t *w, Winding_t *wOrig, TriSurfProps_t *props,
                                        int vertIdx0, int vertIdx1, int vertIdx2, int visGroupIndex,
                                        void *userData)
{
    const float *lmapVecs = (const float *)userData;
    (void)wOrig;
    (void)props;
    Assert(visGroupIndex == 0, s_assertDisable_TrisLmapCreateSurface);
    TrisLmap_RasterTriangle(lmapVecs, w->points[vertIdx0], w->points[vertIdx1], w->points[vertIdx2]);
}

/* 0x454010 + 0x454E70 fallback if a malformed winding cannot be clipped. */
static void TrisLmap_RasterWindingBounds(const float *lmapVecs, const Winding_t *w)
{
    float mins[2] = { FLT_MAX, FLT_MAX };
    float maxs[2] = { -FLT_MAX, -FLT_MAX };
    int index;

    for (index = 0; index < w->numpoints; ++index)
    {
        float u = DotProduct(w->points[index], lmapVecs) + lmapVecs[3];
        float v = DotProduct(w->points[index], lmapVecs + 4) + lmapVecs[7];
        if (u < mins[0]) mins[0] = u;
        if (v < mins[1]) mins[1] = v;
        if (u > maxs[0]) maxs[0] = u;
        if (v > maxs[1]) maxs[1] = v;
    }
    TrisLmap_ClearRasterRect(TrisLmap_ClampInt(TrisLmap_TexelFloor(mins[0]), 0, LIGHTMAP_SIZE - 1),
                             TrisLmap_ClampInt(TrisLmap_TexelFloor(mins[1]), 0, LIGHTMAP_SIZE - 1),
                             TrisLmap_ClampInt(TrisLmap_TexelCeil(maxs[0]), 0, LIGHTMAP_SIZE - 1),
                             TrisLmap_ClampInt(TrisLmap_TexelCeil(maxs[1]), 0, LIGHTMAP_SIZE - 1));
}

/* Native 0x454820.  The caller owns the one-megabyte raster scratch for the
   complete 0x4540B0 pass; this routine owns only its region initialization,
   exact sidecar-vector callback traversal, and reclaimed rectangles. */
int TrisLmap_RasterGroup(TrisLmapGroup_t *group, const TrisLmapAllocation_t *allocation)
{
    TriSurf_t *surf;
    int spanWidth;
    int spanHeight;

    Assert(group, s_assertDisable_TrisLmapCreateSurface);
    Assert(allocation, s_assertDisable_TrisLmapCreateSurface);
    Assert(allocation->width >= 8 && allocation->width <= LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);
    Assert(allocation->height >= 8 && allocation->height <= LIGHTMAP_SIZE, s_assertDisable_TrisLmapCreateSurface);
    spanWidth = allocation->isTransposed ? allocation->height : allocation->width;
    spanHeight = allocation->isTransposed ? allocation->width : allocation->height;
    TrisLmap_InitRasterRegion(allocation->allocY, allocation->allocX, spanWidth, spanHeight);
    for (surf = group->firstSurf; surf; surf = surf->lmap->nextInGroup)
    {
        const TrisLmapAssignmentPayload_t *payload = TrisLmap_GetAssignment(surf, surf->props);
        Assert(payload, s_assertDisable_TrisLmapCreateSurface);
        Assert(payload->groupId == group->id, s_assertDisable_TrisLmapCreateSurface);
        PreprocessLmapWinding(surf);
        if (!TrisLmap_TesselateWinding(surf, 0, TrisLmap_RasterTessCallback, (void *)(const float *)payload->vecs))
            TrisLmap_RasterWindingBounds((const float *)payload->vecs, surf->winding);
    }
    return TrisLmap_ReclaimRasterRegion(allocation->lightmapIndex, allocation->allocY, allocation->allocX,
                                        spanWidth, spanHeight);
}

/* 0x4540B0.  Deliberately not called by the existing compiler route: all
   consumers must first select the sidecar payload before this native path can
   become production. */
int TrisLmap_AssignAll(void)
{
    int materialIndex;
    int groupCount;
    int allowedLightmapCount;

    Assert(GetTrisTransientMode() == 4, s_assertDisable_TrisLmapCreateSurface);
    TrisLmap_BuildMaterialLists();
    for (materialIndex = 0; materialIndex < s_trisLmapMaterialCount; ++materialIndex)
    {
        TriSurf_t *surf;
        int count = 0;
        for (surf = s_trisLmapMaterialHeads[materialIndex]; surf; surf = surf->lmap->nextInMaterial)
            ++count;
        s_trisLmapMaterialHeads[materialIndex] = TrisLmap_SortMaterialList(s_trisLmapMaterialHeads[materialIndex], count);
    }
    qsort(s_trisLmapMaterialHeads, s_trisLmapMaterialCount, sizeof(s_trisLmapMaterialHeads[0]), TrisLmap_CompareMaterialHeads);

    groupCount = 0;
    allowedLightmapCount = 0;
    TrisLmap_BeginRasterScratch();
    for (materialIndex = 0; materialIndex < s_trisLmapMaterialCount; ++materialIndex)
    {
        TriSurf_t *surf;
        Assert(!lmAllowedList, s_assertDisable_TrisLmapCreateSurface);
        for (surf = s_trisLmapMaterialHeads[materialIndex]; surf; surf = surf->lmap->nextInMaterial)
        {
            TrisLmapAllocation_t allocation;
            if (surf->props->lmapIndex == 31)
            {
                TrisLmap_AssignGroup(surf->lmap->group, &allocation);
                TrisLmap_SetGroupAssignment(surf->lmap->group, &allocation);
                if (TrisLmap_ShouldRasterGroup(allocation.width, allocation.height))
                    TrisLmap_RasterGroup(surf->lmap->group, &allocation);
                ++groupCount;
            }
            Assert(surf->props->lmapIndex != 31, s_assertDisable_TrisLmapCreateSurface);
            TrisLmap_AllowLightmap(surf->props->lmapIndex);
        }
        allowedLightmapCount += TrisLmap_ClearAllowedLightmaps();
    }
    TrisLmap_EndRasterScratch();
    TrisLmap_FreeAllGroups();
    (void)allowedLightmapCount;
    return groupCount;
}

/* 0x451590: a material is lightmapped when it, or any coalesced layer,
   carries the lightmap game flag.  KIWI retains that flag on ShaderInfo_t. */
static int TrisLmap_PropsNeedLightmap(const TriSurfProps_t *props)
{
    const CoalesceNode_t *node;

    Assert(props, s_assertDisable_TrisLmapCreateSurface);
    if (!props->coalesceChain)
        return props->si && (props->si->gameFlags & 2) != 0;
    for (node = props->coalesceChain; node; node = node->next)
    {
        if (TrisLmap_PropsNeedLightmap((const TriSurfProps_t *)node->value))
            return 1;
    }
    return 0;
}

/* 0x4538B0.  This is deliberately a linked-list merge sort rather than qsort:
   0x451600 consumes its exact stable order when it first populates the grid. */
static int TrisLmap_SurfSortBefore(const TriSurf_t *left, const TriSurf_t *right)
{
    int axis;
    double leftArea;
    double rightArea;
    int compare;
    int leftTessSizeBits;
    int rightTessSizeBits;

    if (left->props->si != right->props->si)
    {
        compare = strcmp(left->props->si->name, right->props->si->name);
        return compare < 0;
    }
    for (axis = 0; axis != 3; ++axis)
    {
        if (left->mins[axis] != right->mins[axis])
            return left->mins[axis] < right->mins[axis];
    }
    for (axis = 0; axis != 3; ++axis)
    {
        if (left->maxs[axis] != right->maxs[axis])
            return left->maxs[axis] < right->maxs[axis];
    }
    leftArea = WindingSignedArea(left->winding, left->props->plane);
    rightArea = WindingSignedArea(right->winding, right->props->plane);
    if (leftArea != rightArea)
        return leftArea < rightArea;
    if (left->winding != right->winding)
        return left->winding < right->winding;
    if (left->props->si->surfaceFlags != right->props->si->surfaceFlags)
        return left->props->si->surfaceFlags < right->props->si->surfaceFlags;
    /* Native follows MaterialInfo::surfaceFlags (+36) with the raw dword at
       +32 (tessSize).  ShaderInfo_t carries the same value as subdivisions;
       compare its representation, not the unrelated contentFlags field. */
    memcpy(&leftTessSizeBits, &left->props->si->subdivisions, sizeof(leftTessSizeBits));
    memcpy(&rightTessSizeBits, &right->props->si->subdivisions, sizeof(rightTessSizeBits));
    if (leftTessSizeBits != rightTessSizeBits)
        return leftTessSizeBits < rightTessSizeBits;
    return left < right;
}

static TriSurf_t *TrisLmap_SortCellMaterialList(TriSurf_t *head, int count)
{
    TriSurf_t *right;
    TriSurf_t *left;
    TriSurf_t *rightHead;
    TriSurf_t **tail;
    int leftCount;
    int rightCount;

    Assert(head, s_assertDisable_TrisLmapCreateSurface);
    Assert(count > 0, s_assertDisable_TrisLmapCreateSurface);
    if (count == 1)
        return head;

    leftCount = count / 2;
    rightCount = count - leftCount;
    right = head;
    for (int index = 0; index < leftCount; ++index)
        right = right->next;
    left = TrisLmap_SortCellMaterialList(head, leftCount);
    rightHead = TrisLmap_SortCellMaterialList(right, rightCount);
    tail = &head;
    while (leftCount || rightCount)
    {
        if (!leftCount || (rightCount && !TrisLmap_SurfSortBefore(left, rightHead)))
        {
            *tail = rightHead;
            rightHead = rightHead->next;
            --rightCount;
        }
        else
        {
            *tail = left;
            left = left->next;
            --leftCount;
        }
        tail = &(*tail)->next;
    }
    *tail = NULL;
    return head;
}

static void TrisLmap_RebuildCellPrevLinks(TriSurf_t *head)
{
    TriSurf_t *previous = NULL;
    TriSurf_t *surf;

    for (surf = head; surf; surf = surf->next)
    {
        surf->prev = previous;
        previous = surf;
    }
}

static int TrisLmap_LoadedMaterialIndex(const TriSurf_t *surf)
{
    ptrdiff_t index;

    Assert(surf && surf->props && surf->props->si, s_assertDisable_TrisLmapCreateSurface);
    /* `TriSurfProps_t` deliberately retains KIWI's ShaderInfo_t rather than
       the executable's Material*.  The material-cache slot is its native
       FindLoadedMaterialIndex analogue: it is assigned in loaded-material
       order and is bounded here by 0x4c8, exactly as 0x451600's table is. */
    index = surf->props->si - g_matExpandRegion.materialCache;
    Assert(index >= 0 && index < TRIS_LMAP_MAX_MATERIALS, s_assertDisable_TrisLmapCreateSurface);
    return (int)index;
}

static int TrisLmap_GroupPairWasTested(const TrisLmapGroup_t *group0, const TrisLmapGroup_t *group1)
{
    const TrisLmapGroup_t *bits;
    int id;

    if (group0->id >= group1->id)
    {
        bits = group0;
        id = group1->id;
    }
    else
    {
        bits = group1;
        id = group0->id;
    }
    return bits->usedGroupBits && (bits->usedGroupBits[id >> 5] & (1u << (id & 31))) != 0;
}

/* 0x452A90/0x452AD0: retain a failed compatibility pair on the larger id. */
static void TrisLmap_MarkGroupPairTested(TrisLmapGroup_t *group0, TrisLmapGroup_t *group1)
{
    TrisLmapGroup_t *bits;
    int id;
    int wordCount;

    if (group0->id >= group1->id)
    {
        bits = group0;
        id = group1->id;
    }
    else
    {
        bits = group1;
        id = group0->id;
    }
    if (!bits->usedGroupBits)
    {
        wordCount = (bits->id + 31) >> 5;
        bits->usedGroupBits = (unsigned int *)malloc(wordCount * sizeof(*bits->usedGroupBits));
        if (!bits->usedGroupBits)
            Com_Error("TrisLmap_MarkGroupPairTested: out of memory");
        memset(bits->usedGroupBits, 0, wordCount * sizeof(*bits->usedGroupBits));
    }
    bits->usedGroupBits[id >> 5] |= 1u << (id & 31);
}

/* 0x4514B0. */
static void TrisLmap_ValidateGroup(const TrisLmapGroup_t *group)
{
    const TriSurf_t *previous = NULL;
    const TriSurf_t *surf;

    Assert(group, s_assertDisable_TrisLmapCreateSurface);
    for (surf = group->firstSurf; surf; )
    {
        Assert(surf->lmap, s_assertDisable_TrisLmapCreateSurface);
        Assert(surf->lmap->group == group, s_assertDisable_TrisLmapCreateSurface);
        Assert(surf->lmap->prevInGroup == previous, s_assertDisable_TrisLmapCreateSurface);
        previous = surf;
        surf = surf->lmap->nextInGroup;
    }
}

static int TrisLmap_PointInBounds3D(const float *point, const float *mins, const float *maxs)
{
    return point[0] >= mins[0] && point[0] <= maxs[0]
        && point[1] >= mins[1] && point[1] <= maxs[1]
        && point[2] >= mins[2] && point[2] <= maxs[2];
}

static int TrisLmap_WindingHasPoint(const Winding_t *winding, const float *point, float epsilon)
{
    int pointIndex;

    for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
    {
        const float *candidate = winding->points[pointIndex];
        if (fabsf(candidate[0] - point[0]) <= epsilon
            && fabsf(candidate[1] - point[1]) <= epsilon
            && fabsf(candidate[2] - point[2]) <= epsilon)
        {
            return 1;
        }
    }
    return 0;
}

/* 0x4522B0. */
static int TrisLmap_TestTouchPoint(const float *point, const TriSurf_t *source, const TriSurf_t *target,
                                   int *hasShift, float *lmapShift)
{
    float sourceUv[2];
    float targetUv[2];
    float shift[2];

    sourceUv[0] = DotProduct(point, source->lmap->vecs[0]) + source->lmap->vecs[0][3];
    sourceUv[1] = DotProduct(point, source->lmap->vecs[1]) + source->lmap->vecs[1][3];
    targetUv[0] = DotProduct(point, target->lmap->vecs[0]) + target->lmap->vecs[0][3];
    targetUv[1] = DotProduct(point, target->lmap->vecs[1]) + target->lmap->vecs[1][3];
    /* 0x4522B0 forms target - source.  0x4520D0 negates that result
       before the reciprocal pass, leaving source - target for 0x452420. */
    shift[0] = targetUv[0] - sourceUv[0];
    shift[1] = targetUv[1] - sourceUv[1];
    if (!*hasShift)
    {
        lmapShift[0] = floorf(shift[0] * (float)LIGHTMAP_SIZE + 0.5f) / (float)LIGHTMAP_SIZE;
        lmapShift[1] = floorf(shift[1] * (float)LIGHTMAP_SIZE + 0.5f) / (float)LIGHTMAP_SIZE;
        *hasShift = 1;
    }
    return fabsf(shift[0] - lmapShift[0]) <= TRIS_LMAP_ASSIGNMENT_EPSILON
        && fabsf(shift[1] - lmapShift[1]) <= TRIS_LMAP_ASSIGNMENT_EPSILON;
}

/* 0x4521D0.  A shared world vertex must map to one stable, texel snapped
   translation in both lightmap spaces. */
static int TrisLmap_TestTouchDirection(const TriSurf_t *source, const TriSurf_t *target,
                                       int *hasShift, float *lmapShift)
{
    int pointIndex;

    for (pointIndex = 0; pointIndex < source->winding->numpoints; ++pointIndex)
    {
        const float *point = source->winding->points[pointIndex];

        if (!TrisLmap_PointInBounds3D(point, target->mins, target->maxs)
            || !TrisLmap_WindingHasPoint(target->winding, point, 0.01f))
        {
            continue;
        }
        if (!TrisLmap_TestTouchPoint(point, source, target, hasShift, lmapShift))
            return 0;
    }
    return 1;
}

static int TrisLmap_CanMergeGroups(const TriSurf_t *groupGrowSurf, const TriSurf_t *groupKillSurf,
                                   const float *lmapShift);

static int TrisLmap_TryCoplanarTouch(TriSurf_t *groupGrowSurf, TriSurf_t *groupKillSurf)
{
    float lmapShift[2];
    int hasShift = 0;

    /* 0x451ED0 can select 0x4520D0 when the first winding crosses the
       second plane but the reciprocal winding remains within the 0.1 band.
       0x4520D0 itself has no full-coplanarity assertion; its two directional
       touch tests are the native rejection mechanism. */
    if (!TrisLmap_TestTouchDirection(groupGrowSurf, groupKillSurf, &hasShift, lmapShift) || !hasShift)
        return 0;
    lmapShift[0] = -lmapShift[0];
    lmapShift[1] = -lmapShift[1];
    if (!TrisLmap_TestTouchDirection(groupKillSurf, groupGrowSurf, &hasShift, lmapShift))
        return 0;
    if (!TrisLmap_CanMergeGroups(groupGrowSurf, groupKillSurf, lmapShift))
        return 0;
    return TrisLmap_AbsorbGroup(groupGrowSurf->lmap->group, groupKillSurf->lmap->group, lmapShift);
}

/* 0x4536E0. */
static int TrisLmap_PlanesCanTouch(const TriSurf_t *surf0, const TriSurf_t *surf1)
{
    TriSurfPropsSidecar_t *sidecar0;
    TriSurfPropsSidecar_t *sidecar1;
    float normalDot = DotProduct(surf0->props->plane, surf1->props->plane);

    if (normalDot >= 0.98400003f)
        return 1;
    if (normalDot <= smoothAngle - 0.001f)
        return 0;
    sidecar0 = TrisPropsSidecar_Get(surf0->props);
    sidecar1 = TrisPropsSidecar_Get(surf1->props);
    Assert(sidecar0 && sidecar1, s_assertDisable_TrisLmapCreateSurface);
    if (!sidecar0->smoothing || !sidecar1->smoothing)
        return 0;
    return sidecar0->smoothing != 2 || sidecar1->smoothing != 2;
}

/* 0x452EA0: equal mappings after the candidate shift cannot produce a
   projected-edge collision. */
static int TrisLmap_SameLmapSpace(const TriSurf_t *surf0, const TriSurf_t *surf1, const float *lmapShift)
{
    const TrisLmapTransient_t *lmap0 = surf0->lmap;
    const TrisLmapTransient_t *lmap1 = surf1->lmap;

    return lmap0->vecs[0][3] == lmap1->vecs[0][3] + lmapShift[0]
        && lmap0->vecs[1][3] == lmap1->vecs[1][3] + lmapShift[1]
        && lmap0->vecs[0][0] == lmap1->vecs[0][0]
        && lmap0->vecs[0][1] == lmap1->vecs[0][1]
        && lmap0->vecs[0][2] == lmap1->vecs[0][2]
        && lmap0->vecs[1][0] == lmap1->vecs[1][0]
        && lmap0->vecs[1][1] == lmap1->vecs[1][1]
        && lmap0->vecs[1][2] == lmap1->vecs[1][2];
}

/* 0x452F40. */
static int TrisLmap_ProjectedEdgesConflict(const float *uv0, const float *uv1,
                                           const float *uv2, const float *uv3)
{
    float determinant;
    float t;
    float u;
    float position0[3];
    float position1[3];
    int axis;

    determinant = (uv1[0] - uv0[0]) * (uv3[1] - uv2[1])
        - (uv1[1] - uv0[1]) * (uv3[0] - uv2[0]);
    if (fabsf(determinant) <= 0.00001f)
        return 0;
    t = ((uv0[1] - uv2[1]) * (uv3[0] - uv2[0])
        - (uv0[0] - uv2[0]) * (uv3[1] - uv2[1])) / determinant;
    if (t < -0.00001f || t > 1.00001f)
        return 0;
    u = ((uv0[1] - uv2[1]) * (uv1[0] - uv0[0])
        - (uv0[0] - uv2[0]) * (uv1[1] - uv0[1])) / determinant;
    if (u < -0.00001f || u > 1.00001f)
        return 0;
    for (axis = 0; axis != 3; ++axis)
    {
        position0[axis] = uv0[axis + 2] + t * (uv1[axis + 2] - uv0[axis + 2]);
        position1[axis] = uv2[axis + 2] + u * (uv3[axis + 2] - uv2[axis + 2]);
    }
    return !VectorCompareEpsilon(position0, position1, 0.1f, 3);
}

/* 0x452B70.  The temporary entries are [u, v, xyz]. */
static int TrisLmap_HasProjectedEdgeCollision(const TriSurf_t *surf0, const TriSurf_t *surf1,
                                              const float *lmapShift)
{
    int edge0;
    int edge1;

    Assert(surf0 && surf1 && surf0 != surf1, s_assertDisable_TrisLmapCreateSurface);
    if (TrisLmap_SameLmapSpace(surf0, surf1, lmapShift))
        return 0;
    for (edge0 = 0; edge0 < surf0->winding->numpoints; ++edge0)
    {
        int next0 = (edge0 + 1) % surf0->winding->numpoints;
        float segment0[2][5];
        int endpoint;

        for (endpoint = 0; endpoint != 2; ++endpoint)
        {
            const float *point = surf0->winding->points[endpoint ? next0 : edge0];
            segment0[endpoint][0] = DotProduct(point, surf0->lmap->vecs[0]) + surf0->lmap->vecs[0][3];
            segment0[endpoint][1] = DotProduct(point, surf0->lmap->vecs[1]) + surf0->lmap->vecs[1][3];
            VectorCopy(point, &segment0[endpoint][2]);
        }
        for (edge1 = 0; edge1 < surf1->winding->numpoints; ++edge1)
        {
            int next1 = (edge1 + 1) % surf1->winding->numpoints;
            float segment1[2][5];

            for (endpoint = 0; endpoint != 2; ++endpoint)
            {
                const float *point = surf1->winding->points[endpoint ? next1 : edge1];
                segment1[endpoint][0] = DotProduct(point, surf1->lmap->vecs[0]) + surf1->lmap->vecs[0][3] + lmapShift[0];
                segment1[endpoint][1] = DotProduct(point, surf1->lmap->vecs[1]) + surf1->lmap->vecs[1][3] + lmapShift[1];
                VectorCopy(point, &segment1[endpoint][2]);
            }
            if (TrisLmap_ProjectedEdgesConflict(segment0[0], segment0[1], segment1[0], segment1[1]))
                return 1;
        }
    }
    return 0;
}

/* 0x4528F0: the overlap proof completed before 0x452420 mutates a group. */
static int TrisLmap_CanMergeGroups(const TriSurf_t *groupGrowSurf, const TriSurf_t *groupKillSurf,
                                   const float *lmapShift)
{
    const TrisLmapGroup_t *groupGrow = groupGrowSurf->lmap->group;
    const TrisLmapGroup_t *groupKill = groupKillSurf->lmap->group;
    const TriSurf_t *killSurf;

    TrisLmap_ValidateGroup(groupGrow);
    TrisLmap_ValidateGroup(groupKill);
    /* 0x4528F0 marks the pair before it can reject any opposing plane or
       projected edge; 0x451CB0 will not reconsider that relationship. */
    TrisLmap_MarkGroupPairTested((TrisLmapGroup_t *)groupGrow, (TrisLmapGroup_t *)groupKill);
    for (killSurf = groupKill->firstSurf; killSurf; killSurf = killSurf->lmap->nextInGroup)
    {
        float shiftedKillMins[2];
        float shiftedKillMaxs[2];
        const TriSurf_t *growSurf;

        shiftedKillMins[0] = killSurf->lmap->bounds[0][0] + lmapShift[0];
        shiftedKillMins[1] = killSurf->lmap->bounds[0][1] + lmapShift[1];
        shiftedKillMaxs[0] = killSurf->lmap->bounds[1][0] + lmapShift[0];
        shiftedKillMaxs[1] = killSurf->lmap->bounds[1][1] + lmapShift[1];
        for (growSurf = groupGrow->firstSurf; growSurf; growSurf = growSurf->lmap->nextInGroup)
        {
            if (DotProduct(growSurf->props->plane, killSurf->props->plane) < -0.99900001f
                && fabsf(growSurf->props->plane[3] + killSurf->props->plane[3]) < 0.1f)
            {
                return 0;
            }
            if (BoundsIntersect2D(growSurf->lmap->bounds[0], growSurf->lmap->bounds[1],
                                  shiftedKillMins, shiftedKillMaxs)
                && (growSurf != groupGrowSurf || killSurf != groupKillSurf)
                && TrisLmap_HasProjectedEdgeCollision(growSurf, killSurf, lmapShift))
            {
                return 0;
            }
        }
    }
    return 1;
}

/* 0x452010: materialize the point-to-plane distances that 0x451ED0 uses
   both to reject non-intersecting windings and, later, to clip. */
static void TrisLmap_GetWindingPlaneDistances(const Winding_t *winding, const float *plane,
                                              float *distances, float *minimum, float *maximum)
{
    int pointIndex;

    Assert(winding->numpoints > 0 && winding->numpoints <= 1024,
           s_assertDisable_TrisLmapCreateSurface);
    *minimum = FLT_MAX;
    *maximum = -FLT_MAX;
    for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
    {
        distances[pointIndex] = DotProduct(winding->points[pointIndex], plane) - plane[3];
        if (distances[pointIndex] < *minimum)
            *minimum = distances[pointIndex];
        if (distances[pointIndex] > *maximum)
            *maximum = distances[pointIndex];
    }
}

/* 0x453400: retain points on the plane and intersections of a winding with
   it.  Its caller has already formed the fixed 1024-entry distance buffer. */
static int TrisLmap_ClipWindingToPlane(const Winding_t *winding, const float *distances,
                                       vec3_t *points, int pointLimit)
{
    int pointIndex;
    int outputCount = 0;

    Assert(winding->numpoints > 0 && winding->numpoints <= pointLimit,
           s_assertDisable_TrisLmapCreateSurface);
    for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
    {
        int nextIndex = (pointIndex + 1) % winding->numpoints;
        float currentDistance = distances[pointIndex];
        float nextDistance = distances[nextIndex];
        int currentSide = currentDistance < -0.1f ? 1 : (currentDistance > 0.1f ? 0 : 2);
        int nextSide = nextDistance < -0.1f ? 1 : (nextDistance > 0.1f ? 0 : 2);

        if (currentSide == 2)
        {
            Assert(outputCount < pointLimit, s_assertDisable_TrisLmapCreateSurface);
            VectorCopy(winding->points[pointIndex], points[outputCount++]);
        }
        else if (nextSide != 2 && nextSide != currentSide)
        {
            float fraction = currentDistance / (currentDistance - nextDistance);

            Assert(outputCount < pointLimit, s_assertDisable_TrisLmapCreateSurface);
            points[outputCount][0] = winding->points[pointIndex][0]
                + fraction * (winding->points[nextIndex][0] - winding->points[pointIndex][0]);
            points[outputCount][1] = winding->points[pointIndex][1]
                + fraction * (winding->points[nextIndex][1] - winding->points[pointIndex][1]);
            points[outputCount][2] = winding->points[pointIndex][2]
                + fraction * (winding->points[nextIndex][2] - winding->points[pointIndex][2]);
            ++outputCount;
        }
    }
    Assert(outputCount > 0 && outputCount <= pointLimit, s_assertDisable_TrisLmapCreateSurface);
    return outputCount;
}

/* 0x453100, the non-coplanar branch of 0x451ED0. */
static int TrisLmap_TryNonCoplanarTouch(TriSurf_t *groupGrowSurf, TriSurf_t *groupKillSurf,
                                        const float *growDistances, const float *killDistances)
{
    vec3_t clippedGrow[1024];
    vec3_t clippedKill[1024];
    vec3_t mins[2];
    vec3_t maxs[2];
    float lmapShift[2];
    int hasShift = 0;
    int pointCount[2];
    int listIndex;

    if (!TrisLmap_PlanesCanTouch(groupGrowSurf, groupKillSurf))
        return 0;
    pointCount[0] = TrisLmap_ClipWindingToPlane(groupGrowSurf->winding, growDistances,
                                                 clippedGrow, 1024);
    pointCount[1] = TrisLmap_ClipWindingToPlane(groupKillSurf->winding, killDistances,
                                                 clippedKill, 1024);
    for (listIndex = 0; listIndex != 2; ++listIndex)
    {
        vec3_t *points = listIndex ? clippedKill : clippedGrow;
        int pointIndex;

        VectorCopy(points[0], mins[listIndex]);
        VectorCopy(points[0], maxs[listIndex]);
        for (pointIndex = 1; pointIndex < pointCount[listIndex]; ++pointIndex)
            AddPointToBounds(points[pointIndex], mins[listIndex], maxs[listIndex]);
    }
    if (!BoundsIntersectTolerance(mins[0], maxs[0], mins[1], maxs[1], 0.1f))
        return 0;
    for (listIndex = 0; listIndex != 2; ++listIndex)
    {
        vec3_t *points = listIndex ? clippedKill : clippedGrow;
        int pointIndex;

        for (pointIndex = 0; pointIndex < pointCount[listIndex]; ++pointIndex)
        {
            /* 0x45334D evaluates every clipped point as kill then grow. */
            if (!TrisLmap_TestTouchPoint(points[pointIndex], groupKillSurf, groupGrowSurf,
                                         &hasShift, lmapShift))
                return 0;
        }
    }
    if (!hasShift)
        return 0;
    if (!TrisLmap_CanMergeGroups(groupGrowSurf, groupKillSurf, lmapShift))
        return 0;
    return TrisLmap_AbsorbGroup(groupGrowSurf->lmap->group, groupKillSurf->lmap->group, lmapShift);
}

/* 0x451ED0.  In particular, 0x453100 is not merely a normal/smoothing
   test: both windings must intersect the other's plane before the native
   clipper is reached.  This makes its non-empty output assertion a genuine
   invariant rather than an input filter. */
static void TrisLmap_TryTouch(TriSurf_t *groupGrowSurf, TriSurf_t *groupKillSurf)
{
    float growDistances[1024];
    float killDistances[1024];
    float growMinimum;
    float growMaximum;
    float killMinimum;
    float killMaximum;

    TrisLmap_ValidateSurface(groupGrowSurf);
    TrisLmap_ValidateSurface(groupKillSurf);
    TrisLmap_GetWindingPlaneDistances(groupGrowSurf->winding, groupKillSurf->props->plane,
                                      growDistances, &growMinimum, &growMaximum);
    if (growMaximum < -0.1f || growMinimum > 0.1f)
        return;
    if (growMaximum > 0.1f || growMinimum < -0.1f)
    {
        TrisLmap_GetWindingPlaneDistances(groupKillSurf->winding, groupGrowSurf->props->plane,
                                          killDistances, &killMinimum, &killMaximum);
        if (killMaximum < -0.1f || killMinimum > 0.1f)
            return;
        if (killMaximum > 0.1f || killMinimum < -0.1f)
        {
            TrisLmap_TryNonCoplanarTouch(groupGrowSurf, groupKillSurf,
                                         growDistances, killDistances);
            return;
        }
    }
    TrisLmap_TryCoplanarTouch(groupGrowSurf, groupKillSurf);
}

static TriSurf_t *s_trisLmapCurrentSurf;

/* 0x451CB0, invoked by the existing 0x450BF0-equivalent grid traversal. */
static void TrisLmap_TouchGridCandidate(TriSurf_t *candidate)
{
    TriSurf_t *current = s_trisLmapCurrentSurf;

    if (!current || !candidate || !current->lmap || !candidate->lmap
        || current->lmap->group == candidate->lmap->group)
    {
        return;
    }
    if (TrisLmap_GroupPairWasTested(current->lmap->group, candidate->lmap->group))
        return;
    if (current->props->si != candidate->props->si)
        return;
    TrisLmap_TryTouch(candidate, current);
}

/* 0x453B80: query an already-populated grid for each eligible member whose
   world bounds intersect the accumulated bounds supplied by its caller. */
static void TrisLmap_TouchListAgainstBounds(TriSurf_t *list, float *queryMins, float *queryMaxs)
{
    TriSurf_t *surf;

    for (surf = list; surf; surf = surf->next)
    {
        if (TrisLmap_PropsNeedLightmap(surf->props)
            && BoundsIntersect(queryMins, queryMaxs, surf->mins, surf->maxs))
        {
            s_trisLmapCurrentSurf = surf;
            GridTree_ForEach(surf->mins, surf->maxs, TrisLmap_TouchGridCandidate);
        }
    }
}

/* 0x453C10. */
static void TrisLmap_InsertListInGrid(TriSurf_t *list)
{
    TriSurf_t *surf;

    for (surf = list; surf; surf = surf->next)
    {
        if (TrisLmap_PropsNeedLightmap(surf->props))
            GridTree_Insert(surf);
    }
}

/* 0x451600: partition one cell by loaded material, stable-sort each partition,
   build/merge its carriers, then return material-index order for subsequent
   tessellation and allocation. */
static TriSurf_t *TrisLmap_GroupCell(TriSurf_t *list, float *cellMins, float *cellMaxs)
{
    TriSurf_t *materialHeads[TRIS_LMAP_MAX_MATERIALS];
    TriSurf_t *output = NULL;
    TriSurf_t *outputLast = NULL;
    TriSurf_t **outputTail = &output;
    vec3_t accumulatedMins;
    vec3_t accumulatedMaxs;
    int materialIndex;

    if (!list)
        return NULL;
    memset(materialHeads, 0, sizeof(materialHeads));
    while (list)
    {
        TriSurf_t *currentList = NULL;
        TriSurf_t *restList = NULL;
        TriSurf_t **currentTail = &currentList;
        TriSurf_t **restTail = &restList;
        TriSurf_t *surf;
        ShaderInfo_t *material = list->props->si;
        vec3_t materialMins;
        vec3_t materialMaxs;
        int count = 0;

        ClearBounds(materialMins, materialMaxs);
        for (surf = list; surf; )
        {
            TriSurf_t *next = surf->next;
            if (surf->props->si == material)
            {
                *currentTail = surf;
                currentTail = &surf->next;
                AddBoundsToBounds(surf->mins, surf->maxs, materialMins, materialMaxs);
                ++count;
            }
            else
            {
                *restTail = surf;
                restTail = &surf->next;
            }
            surf = next;
        }
        *currentTail = NULL;
        *restTail = NULL;
        Assert(count > 0 && currentList, s_assertDisable_TrisLmapCreateSurface);
        currentList = TrisLmap_SortCellMaterialList(currentList, count);
        TrisLmap_RebuildCellPrevLinks(currentList);
        materialIndex = TrisLmap_LoadedMaterialIndex(currentList);
        Assert(!materialHeads[materialIndex], s_assertDisable_TrisLmapCreateSurface);
        materialHeads[materialIndex] = currentList;
        SetGridDivisionPoints(materialMins, materialMaxs);
        for (surf = currentList; surf; surf = surf->next)
        {
            if (!TrisLmap_PropsNeedLightmap(surf->props))
                continue;
            Assert(!surf->lmap, s_assertDisable_TrisLmapCreateSurface);
            TrisLmap_CreateSurface(surf);
            s_trisLmapCurrentSurf = surf;
            GridTree_ForEach(surf->mins, surf->maxs, TrisLmap_TouchGridCandidate);
            GridTree_Insert(surf);
            TrisLmap_ValidateSurface(surf);
        }
        list = restList;
    }

    SetGridDivisionPoints(cellMins, cellMaxs);
    ClearBounds(accumulatedMins, accumulatedMaxs);
    for (materialIndex = 0; materialIndex < TRIS_LMAP_MAX_MATERIALS; ++materialIndex)
    {
        TriSurf_t *head = materialHeads[materialIndex];
        TriSurf_t *tail;

        if (!head)
            continue;
        TrisLmap_TouchListAgainstBounds(head, accumulatedMins, accumulatedMaxs);
        head->prev = outputLast;
        *outputTail = head;
        for (tail = head; tail->next; tail = tail->next)
        {
            tail->next->prev = tail;
            AddBoundsToBounds(tail->mins, tail->maxs, accumulatedMins, accumulatedMaxs);
            outputTail = &tail->next;
        }
        AddBoundsToBounds(tail->mins, tail->maxs, accumulatedMins, accumulatedMaxs);
        outputLast = tail;
        outputTail = &tail->next;
        TrisLmap_InsertListInGrid(head);
    }
    s_trisLmapCurrentSurf = NULL;
    return output;
}

/* 0x442690: first group every cell in isolation, then let each later cell
   touch all earlier cells through one world-sized grid. */
static void TrisLmap_GroupCells(void)
{
    vec3_t worldMins;
    vec3_t worldMaxs;
    int cellCount = g_currentEntityIndex <= 0 ? numBSPCullGroups + numBSPCells : 1;
    int cellIndex;

    for (cellIndex = 0; cellIndex < cellCount; ++cellIndex)
    {
        triSurfCellArray[cellIndex] = TrisLmap_GroupCell(triSurfCellArray[cellIndex],
                                                          cellBoundsData[cellIndex].mins,
                                                          cellBoundsData[cellIndex].maxs);
    }
    ClearBounds(worldMins, worldMaxs);
    for (cellIndex = 0; cellIndex < cellCount; ++cellIndex)
        AddBoundsToBounds(cellBoundsData[cellIndex].mins, cellBoundsData[cellIndex].maxs, worldMins, worldMaxs);
    SetGridDivisionPoints(worldMins, worldMaxs);
    for (cellIndex = 0; cellIndex < cellCount; ++cellIndex)
    {
        int previousCell;

        for (previousCell = 0; previousCell < cellIndex; ++previousCell)
        {
            TrisLmap_TouchListAgainstBounds(triSurfCellArray[cellIndex],
                                             cellBoundsData[previousCell].mins,
                                             cellBoundsData[previousCell].maxs);
        }
        TrisLmap_InsertListInGrid(triSurfCellArray[cellIndex]);
    }
    s_trisLmapCurrentSurf = NULL;
}

/* Native 0x4427E0 adapted to the sidecar ABI: every eligible surface must
   have an assigned lightmap once 0x4540B0 has completed. */
static void TrisLmap_ValidateAssignmentCallback(TriSurf_t *surf, bool isTarget)
{
    (void)isTarget;
    if (TrisLmap_PropsNeedLightmap(surf->props))
    {
        const TrisLmapAssignmentPayload_t *assignment = TrisLmap_GetAssignment(surf, surf->props);
        Assert(assignment, s_assertDisable_TrisLmapCreateSurface);
        Assert(assignment->lightmapIndex >= 0 && assignment->lightmapIndex < 31,
               s_assertDisable_TrisLmapCreateSurface);
    }
}

/* 0x442690/0x451600/0x453C60/0x4540B0 lifecycle bridge.  It is entered only
   from the explicit dormant route switch in TriangulateEntity. */
int TrisLmap_BuildAndAssign(void)
{
    int carrierCount;

    Assert(GetTrisTransientMode() == 4, s_assertDisable_TrisLmapCreateSurface);
    TrisLmap_GroupCells();
    carrierCount = TrisLmap_SeparatePropsByGroup();
    if (carrierCount)
        TrisLmap_AssignAll();
    ForEachSurf(TrisLmap_ValidateAssignmentCallback, NULL);
    return carrierCount;
}

void TrisLmap_FreeSurface(TriSurf_t *surf)
{
    TrisLmapTransient_t *lmap;
    TrisLmapGroup_t *group;

    if (!surf || !surf->lmap)
        return;

    lmap = surf->lmap;
    group = lmap->group;
    if (group)
    {
        if (lmap->prevInGroup)
            lmap->prevInGroup->lmap->nextInGroup = lmap->nextInGroup;
        else
            group->firstSurf = lmap->nextInGroup;
        if (lmap->nextInGroup)
            lmap->nextInGroup->lmap->prevInGroup = lmap->prevInGroup;

        if (!group->firstSurf)
        {
            if (group->prev)
                group->prev->next = group->next;
            else
                s_trisLmapGroupList = group->next;
            if (group->next)
                group->next->prev = group->prev;
            free(group->usedGroupBits);
            free(group);
        }
    }

    free(lmap);
    surf->lmap = NULL;
}

void TrisLmap_Shutdown(void)
{
    TrisLmap_FreeAllGroups();
    TrisLmap_FreeAllAssignments();
    TrisLmap_EndRasterScratch();
}

SmCasterTri_t    *g_smCasterTriPool;
SmCasterTri_t    *g_smCurrentCasterTri;
SmOccluderNode_t *g_smOccluderTree;
SmOccluder_t     *g_smOccluderArray;
TriSurf_t        *g_smCasterWindingList;

vec3_t g_smCasterBoundsMax;
vec3_t g_smCasterBoundsMin;
vec3_t g_smGridMaxs;
vec3_t g_smGridMins;
float  g_smInvViewProjMatrix[16];
float  g_smLightDirX;
float  g_smLightDirY;
float  g_smLightDirZ;
float  g_smLightDir_x;
float  g_smLightDir_y;
float  g_smLightDir_z;
float  g_smLightDist;
float  g_smLightOriginX;
float  g_smLightOriginY;
float  g_smLightOriginZ;
int    g_smNumCasterTris;
int    g_smNumOccluders;
char   g_smPerspectiveEnabled;
float  g_smTransposedInvMatrix[17];
float  g_smViewProjMatrix;
float  g_smViewProjMatrix_01;
float  g_smViewProjMatrix_02;
float  g_smViewProjMatrix_03;
float  g_smViewProjMatrix_10;
float  g_smViewProjMatrix_11;
float  g_smViewProjMatrix_12;
float  g_smViewProjMatrix_13;
float  g_smViewProjMatrix_20;
float  g_smViewProjMatrix_21;
float  g_smViewProjMatrix_22;
float  g_smViewProjMatrix_23;
float  g_smViewProjMatrix_30;
float  g_smViewProjMatrix_31;
float  g_smViewProjMatrix_32;
float  g_smViewProjMatrix_33;

char s_assertDisable_SM_AddCasterTriangle;
char s_assertDisable_SM_AddCasterTriangle;
char s_assertDisable_SM_CreateFragment;
char s_assertDisable_SM_CreateFragment;
char s_assertDisable_SM_CreateFragment;
char s_assertDisable_SM_FlattenSurfToWinding;
char s_assertDisable_SM_InitOccluder;
char s_assertDisable_SM_InitOccluder;
char s_assertDisable_SM_InitOccluder;
char s_assertDisable_SM_InitOccluder;
char s_assertDisable_SM_InitOccluder;
char s_assertDisable_SM_InitOccluder;
char s_assertDisable_SM_InitOccluder;
char s_assertDisable_SM_InitOccluder;
char s_assertDisable_SM_InterpolateVert;
char s_assertDisable_SM_InterpolateVert;
char s_assertDisable_SM_InterpolateVert;
char s_assertDisable_SM_ProjectPoint;
char s_assertDisable_SM_ProjectPoint;
char s_assertDisable_SM_ProjectPoint;
char s_assertDisable_SM_ProjectWindingThroughOccluders;
char s_assertDisable_SM_ProjectWindingThroughOccluders;
char s_assertDisable_SM_RayPlaneIntersect;
char s_assertDisable_SM_RestoreSurfFromWinding;
char s_assertDisable_ShadowMid_ClipWindingByOccluders_r;
char s_assertDisable_ShadowMid_FindBestNotch;
char s_assertDisable_ShadowMid_FixTJunctions;
char s_assertDisable_ShadowMid_IsConvex;
char s_assertDisable_ShadowMid_IsConvex;
char s_assertDisable_ShadowMid_IsConvex;
char s_assertDisable_ShadowMid_IsConvex;
char s_assertDisable_ShadowMid_IsConvex;
char s_assertDisable_ShadowMid_IsConvex;
char s_assertDisable_ShadowMid_IsConvex;
char s_assertDisable_ShadowMid_PlugNotchesInWinding;
char s_assertDisable_ShadowMid_PlugNotchesInWinding;
char s_assertDisable_ShadowMid_Shutdown;
char s_assertDisable_ShadowMid_WindingBehindPlane;


/*
================
ShadowMid_Shutdown

Frees shadow midpoint global buffers (tri array, tree nodes).
Asserts that all fragments have been freed.
================
*/
void ShadowMid_Shutdown(void)
{
  free(g_smCasterTriPool);
  g_smCasterTriPool = NULL;
  g_smNumCasterTris = 0;
  if ( g_smCasterWindingList )
    AssertFatal(!g_smCasterWindingList, s_assertDisable_ShadowMid_Shutdown);
  free(g_smOccluderTree);
  g_smOccluderTree = NULL;
}

/*
================
ShadowCeilWrapper1

ceil() wrapper for shadow math.
================
*/
double ShadowCeilWrapper1(float value)
{
  return ceil(value);
}

/*
================
ShadowCeilWrapper2

ceil() wrapper for shadow math.
================
*/
double ShadowCeilWrapper2(float value)
{
  return ceil(value);
}

/*
================
ShadowMid_ClipWindingByOccluders_r

Recursively clips a winding against occluder side planes.
Accepted fragments go to acceptCallback, rejected to rejectCallback.
================
*/
int ShadowMid_ClipWindingByOccluders_r(Winding_t *winding, int groupId, int (*acceptCallback)(Winding_t *), int (*rejectCallback)(Winding_t *), int startIndex)
{
  SmOccluder_t *occ;
  Winding_t *polygon;
  int i, numSides, skipOccluder;
  int planeSides[4];
  Winding_t *frontOut, *backOut;

  while ( 1 )
  {
    /* find next matching occluder for this group */
    while ( startIndex < g_smNumOccluders )
    {
      if ( g_smOccluderArray[startIndex].casterTri->flag == (char)groupId )
        break;
      startIndex++;
    }
    if ( startIndex >= g_smNumOccluders )
      return acceptCallback(winding);

    occ = &g_smOccluderArray[startIndex];
    polygon = occ->polygon;
    occ->active = 0;
    numSides = polygon->numpoints;
    AssertFatal(numSides >= 3 && numSides <= 4, s_assertDisable_ShadowMid_ClipWindingByOccluders_r);

    /* classify winding against each occluder side plane */
    skipOccluder = 0;
    for ( i = 0; i < numSides; i++ )
    {
      planeSides[i] = WindingPlaneSide(winding, occ->planes[i], occ->planes[i][3]);
      if ( planeSides[i] == SIDE_FRONT )
      { skipOccluder = 1; break; }
    }

    if ( !skipOccluder )
    {
      /* clip winding against each crossing occluder side */
      for ( i = 0; i < numSides; i++ )
      {
        if ( planeSides[i] == SIDE_BACK )
          continue;

        ClipWindingEpsilon(winding, occ->planes[i], occ->planes[i][3], ON_EPSILON, &frontOut, &backOut, 0);
        FreeWinding(winding);

        /* front fragment recurses past this occluder */
        if ( frontOut )
          ShadowMid_ClipWindingByOccluders_r(frontOut, groupId, acceptCallback, rejectCallback, startIndex + 1);

        winding = backOut;
        if ( !winding )
          return 0;
      }

      /* entire winding is behind all sides — rejected by this occluder */
      if ( rejectCallback )
        return rejectCallback(winding);
      occ->active = 1;
    }

    startIndex++;
  }
}

/*
================
ShadowMid_CompareOccluderEntries

qsort comparator for occluder entries: sorts by group ID, then by area.
================
*/
int ShadowMid_CompareOccluderEntries(SmOccluder_t *lhs, SmOccluder_t *rhs)
{
  double areaDiff;

  /* primary sort: group (flag) */
  if ( lhs->casterTri->flag != rhs->casterTri->flag )
    return 1;

  /* secondary sort: area */
  areaDiff = lhs->area - rhs->area;
  if ( areaDiff >= 0.0 )
    return areaDiff > 0.0;
  return -1;
}

/*
================
ShadowMid_ComputeEdgePlane

Computes an edge plane from two projected vertices and a face normal.
Optionally expands the plane distance for shadow volume extrusion.
================
*/
char ShadowMid_ComputeEdgePlane(int vertIdxA, Winding_t *w, int vertIdxB, float *outPlane, float *faceNormal, char expandFlag)
{
  vec3_t edge;

  VectorSubtract(w->points[vertIdxA], w->points[vertIdxB], edge);
  CrossProduct(edge, faceNormal, outPlane);
  VecNormalize(outPlane);
  outPlane[3] = DotProduct210(outPlane, w->points[vertIdxB]);

  /* expand for shadow volume extrusion */
  if ( expandFlag )
    outPlane[3] -= SM_EDGE_EXPAND_DIST;

  return expandFlag;
}

/*
================
ShadowMid_ComputeCornerVertex

Intersects 3 planes to find a corner vertex. Falls back to
bisector projection if the planes don't intersect cleanly.
================
*/
char ShadowMid_ComputeCornerVertex(float *planeA, float *planeB, float *outPoint, float *planeC, float *fallbackPos)
{
  double projDist;
  float *planes[3];
  vec3_t bisector;

  planes[0] = planeC;
  planes[1] = planeA;
  planes[2] = planeB;

  if ( !PlaneIntersection3(planes, outPoint) )
  {
    if ( !fallbackPos )
      return 0;

    /* fallback: bisect planeA and planeC normals, offset from fallbackPos */
    VectorAdd(planeC, planeA, bisector);
    VecNormalize(bisector);
    VectorMA(fallbackPos, -SM_EDGE_EXPAND_DIST, bisector, outPoint);

    /* project onto planeB */
    projDist = planeB[3] - DotProduct(planeB, outPoint);
    VectorMA(outPoint, projDist, planeB, outPoint);
  }

  return 1;
}

/*
================
ShadowMid_PointsMatch3D

Compares two 3D points within a small epsilon (0.001).
================
*/
int ShadowMid_PointsMatch3D(float *pointA, float *pointB)
{
  return VectorCompareEpsilon(pointA, pointB, PLANESIDE_EPSILON, 3);
}

/*
================
ShadowMid_FixTJunctions

Fixes T-junctions in shadow casters by subdividing into a 3D grid
and processing each cell.
================
*/
int ShadowMid_FixTJunctions(void)
{
  float dims[3], stepSize[3];
  int steps[3];
  int ix, iy, iz, i;
  float cellMins[3], cellMaxs[3];

  printf("fixing shadow t-junctions...\n");
  SetGridDivisionPoints(g_smGridMins, g_smGridMaxs);
  TJunc_ProcessBrushList(g_smCasterWindingList);

  /* compute grid dimensions and step counts (128-unit cells) */
  dims[0] = g_smGridMaxs[0] - g_smGridMins[0];
  dims[1] = g_smGridMaxs[1] - g_smGridMins[1];
  dims[2] = g_smGridMaxs[2] - g_smGridMins[2];

  for ( i = 0; i < 3; i++ )
  {
    steps[i] = (int)ceil(dims[i] / SM_GRID_CELL_SIZE + 0.001f);
    AssertFatal(steps[i] > 0, s_assertDisable_ShadowMid_FixTJunctions);
    stepSize[i] = (float)(dims[i] / (double)steps[i] + 0.001f);
  }

  /* subdivide into 3D grid and fix t-junctions per cell */
  for ( iz = 0; iz < steps[2]; iz++ )
  {
    cellMins[2] = FMA1(g_smGridMins[2], (float)iz, stepSize[2]);
    cellMaxs[2] = cellMins[2] + stepSize[2];

    for ( iy = 0; iy < steps[1]; iy++ )
    {
      cellMins[1] = FMA1(g_smGridMins[1], (float)iy, stepSize[1]);
      cellMaxs[1] = cellMins[1] + stepSize[1];

      for ( ix = 0; ix < steps[0]; ix++ )
      {
        cellMins[0] = FMA1(g_smGridMins[0], (float)ix, stepSize[0]);
        cellMaxs[0] = cellMins[0] + stepSize[0];

        GridTree_ForEach(cellMins, cellMaxs, TJunc_ProcessSurface);
        GridTree_ForEach(cellMins, cellMaxs, TjuncFixSurfaceEdges);
        TjuncReset();
      }
    }
  }

  return 0;
}

/*
================
ShadowMid_SimplifyConcaveHoles

Merges holes in concave shadow casters.
================
*/
void ShadowMid_SimplifyConcaveHoles(void)
{
  TriSurf_t *caster;

  printf("simplifying holes in concave shadow casters...\n");
  for ( caster = g_smCasterWindingList; caster; caster = caster->next )
    MergeHoles(caster);
}

/*
================
ShadowMid_IsPointInsideTriangle2D

2D point-in-triangle test used for notch detection in shadow casters.
================
*/
bool ShadowMid_IsPointInsideTriangle2D(float *prevVert, float *testVert, float *nextVert, float *tipVert)
{
  int minSide, sideB;
  double dotA;
  float edgeNormalA[2], edgeNormalB[2];
  float distA, distB;

  /* edge normal A: tip → prev (2D perpendicular) */
  edgeNormalA[0] = tipVert[1] - prevVert[1];
  edgeNormalA[1] = prevVert[0] - tipVert[0];
  Vec2Normalize(edgeNormalA);
  distA = edgeNormalA[0] * tipVert[0] + edgeNormalA[1] * tipVert[1] + SM_EDGE_PLANE_EPSILON;

  /* edge normal B: next → tip (2D perpendicular) */
  edgeNormalB[0] = nextVert[1] - tipVert[1];
  edgeNormalB[1] = tipVert[0] - nextVert[0];
  Vec2Normalize(edgeNormalB);

  /* determine minimum side count needed */
  minSide = 2;
  if ( edgeNormalA[0] * nextVert[0] + edgeNormalA[1] * nextVert[1] - distA <= 0.0 )
    minSide = 1;

  /* classify test point against both edges */
  dotA = edgeNormalA[0] * testVert[0] + edgeNormalA[1] * testVert[1] - distA;
  distB = edgeNormalB[0] * testVert[0] + edgeNormalB[1] * testVert[1]
        - (edgeNormalB[0] * tipVert[0] + edgeNormalB[1] * tipVert[1] + SM_EDGE_PLANE_EPSILON);
  sideB = dotA > 0.0;

  if ( distB <= 0.0 )
    return sideB >= minSide;
  return sideB + 1 >= minSide;
}

/*
================
ShadowMid_ComputeNotchArea

Computes the signed area of a notch between two windings using
2D cross products. Returns true if the area exceeds minArea.
================
*/
bool ShadowMid_ComputeNotchArea(Winding_t *windingA, Winding_t *windingB, int startIdxB, int startIdxA, int notchSize, float minArea, float *outArea)
{
  int i;
  float *tipVert;
  float *lastVertB;
  float *prevVert;
  float *nextVertA;
  int prevIdx, curIdx;
  double prevX, prevY;

  tipVert = windingB->points[startIdxB];
  lastVertB = windingB->points[(startIdxB + notchSize - 1) % windingB->numpoints];
  prevVert = windingA->points[(windingA->numpoints + startIdxA - 1) % windingA->numpoints];
  nextVertA = windingA->points[(startIdxA + 1) % windingA->numpoints];

  /* reject if edges cross or tip is outside the triangle */
  if ( WindingHasCrossingEdge(windingB, tipVert, lastVertB, 0, 1)
    || WindingHasCrossingEdge(windingA, tipVert, lastVertB, 0, 1)
    || !ShadowMid_IsPointInsideTriangle2D(prevVert, lastVertB, nextVertA, tipVert) )
    return 0;

  /* accumulate signed area using 2D cross products (shoelace formula) */
  *outArea = 0.0;
  for ( i = 2; i < notchSize; i++ )
  {
    prevIdx = (startIdxB + i - 1) % windingB->numpoints;
    curIdx = (startIdxB + i) % windingB->numpoints;
    prevX = windingB->points[prevIdx][0] - tipVert[0];
    prevY = windingB->points[prevIdx][1] - tipVert[1];
    *outArea += (float)((windingB->points[curIdx][1] - tipVert[1]) * prevX
              - (windingB->points[curIdx][0] - tipVert[0]) * prevY);
  }

  return *outArea > minArea;
}

/*
================
ShadowMid_FindBestNotch

Finds the largest-area notch between two windings by iterating
over all possible notch sizes and positions.
================
*/
int ShadowMid_FindBestNotch(Winding_t *windingA, Winding_t *windingB, int startIdxA, int startIdxB, int *outStartIdx, int vertCount)
{
  int outerIdx, innerSize, idxA, idxB;
  int bestNotchSize;
  float notchArea, bestArea;

  bestArea = 0.0f;
  bestNotchSize = 0;

  /* try all notch positions and sizes, largest area wins */
  for ( outerIdx = 0; outerIdx <= vertCount - 3; outerIdx++ )
  {
    idxA = (outerIdx + startIdxA) % windingA->numpoints;
    idxB = (startIdxB + windingB->numpoints - outerIdx) % windingB->numpoints;

    for ( innerSize = vertCount - outerIdx; innerSize >= 3; innerSize-- )
    {
      if ( ShadowMid_ComputeNotchArea(windingB, windingA, idxA, idxB, innerSize, bestArea, &notchArea) )
      {
        Assert(notchArea > (double)bestArea, s_assertDisable_ShadowMid_FindBestNotch);
        bestArea = notchArea;
        bestNotchSize = innerSize;
        *outStartIdx = idxA;
      }
    }
  }

  return bestNotchSize;
}

/*
================
ShadowMid_WindingFromAuxData

Creates a 2D winding from auxiliary vertex data (XY coords, Z=0).
================
*/
Winding_t *ShadowMid_WindingFromAuxData(int numPoints, SmAuxVert_t *auxVerts)
{
  Winding_t *w;
  int i;

  /* extract projected XY into a 2D winding (Z=0) */
  w = AllocWinding(numPoints);
  w->numpoints = numPoints;

  for ( i = 0; i < numPoints; i++ )
  {
    w->points[i][0] = auxVerts[i].proj[0];
    w->points[i][1] = auxVerts[i].proj[1];
    w->points[i][2] = 0.0f;
  }

  return w;
}

/*
================
ShadowMid_PlugNotchesInWinding

Plugs notches from neighbor windings into the base winding.
Iterates neighbors, finds shared edges, and removes notch geometry.
================
*/
int ShadowMid_PlugNotchesInWinding(TriSurf_t **_unused, IntWinding_t **neighbors, int neighborCount, int *auxStride, IntWinding_t *baseWinding)
{
  int i, dupCount, bestNotchSize, bestNotchStart;
  int sharedIdxA, sharedIdxB;
  IntWinding_t *neighborWinding;
  Winding_t *tempWinding, *neighborCopy;
  void *ptsBase;

  ptsBase = baseWinding->auxData;
  tempWinding = ShadowMid_WindingFromAuxData(baseWinding->numpoints, (SmAuxVert_t *)ptsBase);

  for ( i = 0; i < neighborCount; i++ )
  {
    dupCount = FindSharedEdge(baseWinding, neighbors[i], &sharedIdxA, &sharedIdxB);

    Assert(dupCount <= baseWinding->numpoints, s_assertDisable_ShadowMid_PlugNotchesInWinding);

    /* skip if not enough shared edges or entire winding is shared */
    if ( dupCount < 3 || dupCount == baseWinding->numpoints )
      continue;

    /* build 2D winding from neighbor's projected aux data */
    neighborWinding = neighbors[i];
    neighborCopy = ShadowMid_WindingFromAuxData(neighborWinding->numpoints, (SmAuxVert_t *)neighborWinding->auxData);

    /* find best notch to plug */
    bestNotchSize = ShadowMid_FindBestNotch(tempWinding, neighborCopy, sharedIdxA, sharedIdxB, &bestNotchStart, dupCount);
    FreeWinding(neighborCopy);

    if ( bestNotchSize )
    {
      /* remove notch from base winding and rebuild temp */
      FreeWinding(tempWinding);
      RemoveSharedBoundaryPoints(baseWinding, *auxStride, bestNotchStart, bestNotchSize);
      AssertFatal(ptsBase == baseWinding->auxData, s_assertDisable_ShadowMid_PlugNotchesInWinding);
      tempWinding = ShadowMid_WindingFromAuxData(baseWinding->numpoints, (SmAuxVert_t *)ptsBase);
    }
  }

  FreeWinding(tempWinding);
  return neighborCount;
}

/*
================
ShadowMid_WindingBehindPlane

Tests if casterB's winding vertices are all behind casterA's plane.
Returns 1 if all behind, 0 if any vertex is in front.
================
*/
char ShadowMid_WindingBehindPlane(TriSurf_t *tsA, TriSurf_t *tsB)
{
  float *planeA;
  SmAuxVert_t *auxVerts;
  int i;

  if ( tsA == tsB )
    return 0;

  /* casterA's original plane was saved into props->texVecs[0..3] by SM_FlattenSurfToWinding */
  planeA = tsA->props->texVecs;

  Assert(tsB->winding->numpoints > 0, s_assertDisable_ShadowMid_WindingBehindPlane);

  /* test each of casterB's original vertices (from aux data) against casterA's saved plane */
  auxVerts = tsB->auxData;
  for ( i = 0; i < tsB->winding->numpoints; i++ )
  {
    if ( DotProduct201(auxVerts[i].orig, planeA) - planeA[3] > 0.0 )
      return 0;
  }

  return 1;
}

/*
================
ShadowMid_IsConvex

Tests if a surface's winding is convex via 2D cross products.
Returns 1 if convex, 0 if any concave angle is found.
================
*/
char ShadowMid_IsConvex(TriSurf_t *ts)
{
  Winding_t *windingData;
  int numPoints, ptIdx;
  int prevPrevIdx, prevIdx;

  Assert(ts, s_assertDisable_ShadowMid_IsConvex);
  Assert(ts->winding, s_assertDisable_ShadowMid_IsConvex);
  windingData = ts->winding;
  numPoints = windingData->numpoints;
  Assert(numPoints >= 3, s_assertDisable_ShadowMid_IsConvex);

  if ( numPoints == 3 )
    return 1;

  /* check 2D convexity via cross product sign at each vertex */
  prevPrevIdx = numPoints - 2;
  prevIdx = numPoints - 1;
  for ( ptIdx = 0; ptIdx < numPoints; ptIdx++ )
  {
    float *cur = windingData->points[ptIdx];
    float *prev = windingData->points[prevIdx];
    float *prevPrev = windingData->points[prevPrevIdx];

    AssertFatal(prev[2] == 0.0f, s_assertDisable_ShadowMid_IsConvex);

    /* 2D cross product: (cur-prev) x (prevPrev-prev) */
    if ( Det2x2(cur[1] - prev[1], prevPrev[0] - prev[0], cur[0] - prev[0], prevPrev[1] - prev[1]) < -PLANESIDE_EPSILON )
      return 0;

    prevPrevIdx = prevIdx;
    prevIdx = ptIdx;
  }

  return 1;
}

/*
================
ShadowMid_EmitTriCallback

Tessellation callback that emits a shadow triangle from a surface.
Extracts 3 vertices from the winding, validates the plane, and emits indices.
================
*/
size_t ShadowMid_EmitTriCallback(TriSurf_t *ts, int vertIdx0, int vertIdx1, int vertIdx2, int cellIndex, int cullGroupIndex)
{
  Winding_t *w;
  float triVerts[9];
  float plane[4];

  Assert(ts, s_assertDisable_ShadowMid_IsConvex);
  Assert(ts->winding, s_assertDisable_ShadowMid_IsConvex);
  Assert(ts->auxData, s_assertDisable_ShadowMid_IsConvex);

  w = ts->winding;
  VectorCopy(w->points[vertIdx0], &triVerts[0]);
  VectorCopy(w->points[vertIdx1], &triVerts[3]);
  VectorCopy(w->points[vertIdx2], &triVerts[6]);

  if ( !PlaneFromPoints(plane, &triVerts[0], &triVerts[3], &triVerts[6]) )
    return 0;

  return EmitShadowTriIndices(triVerts);
}

/*
================
ShadowMid_TriangulateConcaveCasters

Triangulates concave shadow casters via tessellation callbacks.
================
*/
void ShadowMid_TriangulateConcaveCasters(void)
{
  TriSurf_t *ts, *nextTs;

  printf("triangulating concave shadow casters...\n");
  SetTrisTransientMode(1, 1);

  for ( ts = g_smCasterWindingList; ts; ts = nextTs )
  {
    nextTs = ts->next;
    ts->origWinding = CopyWinding(ts->winding);
    TesselateWinding(ts, TESS_CELL_SHADOW, 0, ShadowMid_EmitTriCallback);
    FreeTriSurf(ts);
  }

  g_smCasterWindingList = NULL;
  SetTrisTransientMode(0, 1);
}

/*
================
ShadowMid_ComputeNodeBounds

Recursively computes AABB bounds for a shadow tree node from its
children or leaf tri records.
================
*/
int ShadowMid_ComputeNodeBounds(SmOccluderNode_t *node)
{
  SmOccluderNode_t *child;
  SmCasterTri_t *tri;
  int i;

  ClearBounds(node->mins, node->maxs);

  if ( node->childCount > 0 )
  {
    /* internal node: recurse into children and merge bounds */
    child = node->childPtr;
    for ( i = 0; i < node->childCount; i++ )
    {
      ShadowMid_ComputeNodeBounds(&child[i]);
      AddBoundsToBounds(child[i].mins, child[i].maxs, node->mins, node->maxs);
    }
  }
  else if ( node->triCount > 0 )
  {
    /* leaf node: compute bounds from tri projected extents */
    tri = node->triPtr;
    for ( i = 0; i < node->triCount; i++ )
      AddBoundsToBounds(tri[i].projMins, tri[i].projMaxs, node->mins, node->maxs);
  }

  return 0;
}

/*
================
ShadowMid_SortShadowTris

Sorts shadow triangles and builds an AABB tree from them.
Extracts bounds from tri records, builds tree, then converts
to the shadow node format.
================
*/
void ShadowMid_SortShadowTris(void)
{
  AabbTreeBuilder_t builder;
  int i, maxNodes, treeNodeCount;
  SmOccluderNode_t *occNodes;
  ShadowBuildNode_t *buildNodes;

  printf("sorting shadow tris...\n");

  /* set up AABB tree builder for caster tris */
  builder.itemData = g_smCasterTriPool;
  builder.itemCount = g_smNumCasterTris;
  builder.itemStride = sizeof(SmCasterTri_t);
  builder.hasBoundsData = 0;
  builder.itemMins = malloc(g_smNumCasterTris * sizeof(vec3_t));
  builder.itemMaxs = malloc(g_smNumCasterTris * sizeof(vec3_t));
  if ( !builder.itemMins || !builder.itemMaxs )
    Com_Error("Out of memory in SortShadowCasters getting mins/maxs\n");

  /* extract projected bounds from each tri */
  for ( i = 0; i < g_smNumCasterTris; i++ )
  {
    VectorCopy(g_smCasterTriPool[i].projMins, &builder.itemMins[i * 3]);
    VectorCopy(g_smCasterTriPool[i].projMaxs, &builder.itemMaxs[i * 3]);
  }

  /* compute max nodes: max(1, numTris/8) * 2 - 1 */
  maxNodes = g_smNumCasterTris / 8;
  if ( maxNodes < 1 )
    maxNodes = 1;
  builder.maxNodes = 2 * maxNodes - 1;
  builder.nodes = malloc(builder.maxNodes * sizeof(AabbTreeNode_t));
  builder.minPartitionSize = SM_CASTER_TREE_MIN_PARTITION;
  builder.minLeafItems = AABB_MIN_LEAF_ITEMS;

  treeNodeCount = AabbBuildTree(&builder);
  free(builder.itemMins);
  free(builder.itemMaxs);

  /* allocate and convert to SmOccluderNode_t format */
  g_smOccluderTree = malloc(treeNodeCount * sizeof(SmOccluderNode_t));
  if ( !g_smOccluderTree )
    Com_Error("Out of memory in SortShadowCasters getting aabb nodes\n");

  occNodes = g_smOccluderTree;
  buildNodes = (ShadowBuildNode_t *)builder.nodes;
  for ( i = 0; i < treeNodeCount; i++ )
  {
    occNodes[i].triCount = buildNodes[i].triCount;
    occNodes[i].triPtr = &g_smCasterTriPool[buildNodes[i].triStartIndex];
    occNodes[i].childCount = buildNodes[i].childCount;
    occNodes[i].childPtr = &occNodes[buildNodes[i].firstChildIndex];
  }

  ShadowMid_ComputeNodeBounds(occNodes);
  free(builder.nodes);
}

/*
================
ShadowMid_SnapAndMergeVerts

Snaps and merges shadow caster vertices using a merge map.
Copies all verts into a flat buffer, builds a merge map, then
writes merged positions back into each caster's winding.
================
*/
void ShadowMid_SnapAndMergeVerts(void)
{
  TriSurf_t *ts, *nextTs;
  int totalVerts, baseIdx, newPtCount, i;
  vec3_t *vertBuf;
  int *mergeMapResult;
  int mergeMap[SM_MAX_OCCLUDERS];

  printf("snapping and merging shadow caster vertices...\n");

  /* count total vertices across all casters */
  totalVerts = 0;
  for ( ts = g_smCasterWindingList; ts; ts = ts->next )
    totalVerts += ts->winding->numpoints;

  /* copy all winding verts into a flat buffer */
  vertBuf = malloc(totalVerts * sizeof(vec3_t));
  if ( !vertBuf )
    Com_Error("Out of memory snapping %i shadow verts\n", totalVerts);

  baseIdx = 0;
  for ( ts = g_smCasterWindingList; ts; ts = ts->next )
  {
    memcpy(&vertBuf[baseIdx], ts->winding->points, ts->winding->numpoints * sizeof(vec3_t));
    baseIdx += ts->winding->numpoints;
  }

  /* build and apply merge map */
  mergeMapResult = BuildMergeMap((char *)vertBuf, 3, sizeof(vec3_t), totalVerts, ON_EPSILON);
  ApplyMergeMap((char *)vertBuf, sizeof(vec3_t), totalVerts, 0, mergeMapResult);

  /* snap each caster's winding using the merge map */
  baseIdx = 0;
  for ( ts = g_smCasterWindingList; ts; ts = nextTs )
  {
    nextTs = ts->next;
    newPtCount = SnapAndMergeWinding(baseIdx, ts->winding->numpoints, mergeMapResult, mergeMap, (char *)ts->auxData, sizeof(SmAuxVert_t));
    baseIdx += ts->winding->numpoints;

    if ( newPtCount < 3 )
    {
      /* degenerate after merge — remove */
      UnlinkAndFreeSurf(ts, &g_smCasterWindingList);
      FreeTriSurf(ts);
    }
    else
    {
      /* write merged positions back into winding */
      for ( i = 0; i < newPtCount; i++ )
        VectorCopy(vertBuf[mergeMap[i]], ts->winding->points[i]);

      ts->winding->numpoints = newPtCount;
    }
  }

  FreeSurface(mergeMapResult);
  free(vertBuf);
}

/*
================
ShadowMid_AlwaysAccept

Trivial accept callback that always returns 1.
================
*/
char ShadowMid_AlwaysAccept(void)
{
  return 1;
}

/*
================
ShadowMid_TryMergeConcavePair

Tries to merge a primary shadow caster with subsequent casters that
share edges. Handles swap, splice, and boundary removal cases.
================
*/
int ShadowMid_TryMergeConcavePair(int primaryIdx, TriSurf_t **surfArray, IntWinding_t **windingArray, int count, int auxStride)
{
  IntWinding_t *primaryWinding, *secondaryWinding, *splicedWinding;
  TriSurf_t *primarySurf, *secondarySurf;
  int innerIdx, sharedEdgeCount, adjustedIdxA;
  int sharedIdxA, sharedIdxB;

  primaryWinding = windingArray[primaryIdx];

  for ( innerIdx = primaryIdx + 1; innerIdx < count; )
  {
    primarySurf = surfArray[primaryIdx];
    secondarySurf = surfArray[innerIdx];
    secondaryWinding = windingArray[innerIdx];

    /* check bounds overlap and shared edges */
    if ( !BoundsIntersect(primarySurf->mins, primarySurf->maxs, secondarySurf->mins, secondarySurf->maxs)
      || !(sharedEdgeCount = FindSharedEdge(primaryWinding, secondaryWinding, &sharedIdxA, &sharedIdxB)) )
    {
      innerIdx++;
      continue;
    }

    /* if primary is fully shared, swap primary and secondary */
    if ( sharedEdgeCount == primaryWinding->numpoints )
    {
      surfArray[primaryIdx] = secondarySurf;
      surfArray[innerIdx] = primarySurf;
      windingArray[primaryIdx] = secondaryWinding;
      windingArray[innerIdx] = primaryWinding;
      primaryWinding = windingArray[primaryIdx];

      int swappedB = (sharedIdxA + sharedEdgeCount - 1) % windingArray[innerIdx]->numpoints;
      adjustedIdxA = (primaryWinding->numpoints - sharedEdgeCount + sharedIdxB + 1) % primaryWinding->numpoints;
      sharedIdxA = adjustedIdxA;
      sharedIdxB = swappedB;
      secondaryWinding = windingArray[innerIdx];
    }
    else
    {
      adjustedIdxA = sharedIdxA;
    }

    if ( sharedEdgeCount == secondaryWinding->numpoints )
    {
      /* secondary fully contained — remove shared boundary */
      RemoveSharedBoundaryPoints(primaryWinding, auxStride, adjustedIdxA, sharedEdgeCount);
      FreeTriSurf(surfArray[innerIdx]);
      FreeIntWinding(secondaryWinding);
      count--;
      memmove(&surfArray[innerIdx], &surfArray[innerIdx + 1], (count - innerIdx) * sizeof(TriSurf_t *));
      memmove(&windingArray[innerIdx], &windingArray[innerIdx + 1], (count - innerIdx) * sizeof(windingArray[0]));
    }
    else
    {
      /* partial overlap — splice windings */
      splicedWinding = SpliceWindingsAtSharedEdge(primaryWinding, secondaryWinding, auxStride, primaryIdx, innerIdx, adjustedIdxA, sharedIdxB, sharedEdgeCount);
      AddBoundsToBounds(secondarySurf->mins, secondarySurf->maxs, surfArray[primaryIdx]->mins, surfArray[primaryIdx]->maxs);
      FreeTriSurf(surfArray[innerIdx]);
      memmove(&surfArray[innerIdx], &surfArray[innerIdx + 1], (count - innerIdx - 1) * sizeof(TriSurf_t *));
      FreeIntWinding(primaryWinding);
      FreeIntWinding(secondaryWinding);
      primaryWinding = splicedWinding;
      windingArray[primaryIdx] = splicedWinding;
      count--;
      memmove(&windingArray[innerIdx], &windingArray[innerIdx + 1], (count - innerIdx) * sizeof(windingArray[0]));
    }

    windingArray[count] = NULL;
    innerIdx = primaryIdx + 1;
  }

  return count;
}

/*
================
ShadowMid_TryMergeNotchPair

Tries to merge two shadow casters for notch plugging. Handles
swap, splice, and boundary removal cases for notch pairs.
================
*/
int ShadowMid_TryMergeNotchPair(int primaryIdx, TriSurf_t **surfArray, IntWinding_t **windingArray, int count, int auxStride)
{
  IntWinding_t *primaryWinding, *secondaryWinding, *splicedWinding;
  TriSurf_t *primarySurf, *secondarySurf;
  int innerIdx, sharedEdgeCount, adjustedIdxA;
  int sharedIdxA, sharedIdxB;

  primaryWinding = windingArray[primaryIdx];

  for ( innerIdx = primaryIdx + 1; innerIdx < count; )
  {
    primarySurf = surfArray[primaryIdx];
    secondarySurf = surfArray[innerIdx];
    secondaryWinding = windingArray[innerIdx];

    /* check bounds overlap and shared edges */
    if ( !BoundsIntersect(primarySurf->mins, primarySurf->maxs, secondarySurf->mins, secondarySurf->maxs)
      || !(sharedEdgeCount = FindSharedEdge(primaryWinding, secondaryWinding, &sharedIdxA, &sharedIdxB)) )
    {
      innerIdx++;
      continue;
    }

    /* if primary is fully shared, swap primary and secondary */
    if ( sharedEdgeCount == primaryWinding->numpoints )
    {
      surfArray[primaryIdx] = secondarySurf;
      surfArray[innerIdx] = primarySurf;
      windingArray[primaryIdx] = secondaryWinding;
      windingArray[innerIdx] = primaryWinding;
      primaryWinding = windingArray[primaryIdx];

      int swappedB = (sharedIdxA + sharedEdgeCount - 1) % windingArray[innerIdx]->numpoints;
      adjustedIdxA = (primaryWinding->numpoints - sharedEdgeCount + sharedIdxB + 1) % primaryWinding->numpoints;
      sharedIdxA = adjustedIdxA;
      sharedIdxB = swappedB;
      secondaryWinding = windingArray[innerIdx];
    }
    else
    {
      adjustedIdxA = sharedIdxA;
    }

    if ( sharedEdgeCount == secondaryWinding->numpoints )
    {
      /* secondary fully contained — remove shared boundary */
      RemoveSharedBoundaryPoints(primaryWinding, auxStride, adjustedIdxA, sharedEdgeCount);
      FreeTriSurf(surfArray[innerIdx]);
      FreeIntWinding(secondaryWinding);
      count--;
      memmove(&surfArray[innerIdx], &surfArray[innerIdx + 1], (count - innerIdx) * sizeof(TriSurf_t *));
      memmove(&windingArray[innerIdx], &windingArray[innerIdx + 1], (count - innerIdx) * sizeof(windingArray[0]));
    }
    else
    {
      /* partial overlap — splice windings */
      splicedWinding = SpliceWindingsAtSharedEdge(primaryWinding, secondaryWinding, auxStride, primaryIdx, innerIdx, adjustedIdxA, sharedIdxB, sharedEdgeCount);
      AddBoundsToBounds(secondarySurf->mins, secondarySurf->maxs, surfArray[primaryIdx]->mins, surfArray[primaryIdx]->maxs);
      FreeTriSurf(surfArray[innerIdx]);
      memmove(&surfArray[innerIdx], &surfArray[innerIdx + 1], (count - innerIdx - 1) * sizeof(TriSurf_t *));
      FreeIntWinding(primaryWinding);
      FreeIntWinding(secondaryWinding);
      primaryWinding = splicedWinding;
      windingArray[primaryIdx] = splicedWinding;
      count--;
      memmove(&windingArray[innerIdx], &windingArray[innerIdx + 1], (count - innerIdx) * sizeof(windingArray[0]));
    }

    windingArray[count] = NULL;
    innerIdx = primaryIdx + 1;
  }

  return count;
}

/*
================
SM_BuildProjectionMatrix

Builds shadow projection matrix from light position and direction.
Constructs view matrix, applies perspective if needed, computes inverse and transpose.
================
*/
void SM_BuildProjectionMatrix(void)
{
  float perspMatrix[16];
  float viewMatrix[16];
  float rightVec[3];
  float upVec[3];
  float negDir[3];

  negDir[0] = -g_smLightDirX;
  negDir[1] = -g_smLightDirY;
  negDir[2] = -g_smLightDirZ;
  PerpendicularVector(negDir, rightVec);
  CrossProduct(negDir, rightVec, upVec);
  viewMatrix[1] = rightVec[1];
  viewMatrix[5] = upVec[1];
  viewMatrix[0] = rightVec[0];
  viewMatrix[2] = rightVec[2];
  viewMatrix[9] = negDir[1];
  viewMatrix[4] = upVec[0];
  { float smOrigin[3] = { g_smLightOriginX, g_smLightOriginY, g_smLightOriginZ };
  viewMatrix[3] = -DotProduct210(smOrigin, rightVec); }
  viewMatrix[6] = upVec[2];
  viewMatrix[8] = negDir[0];
  viewMatrix[10] = negDir[2];
  memset(&viewMatrix[12], 0, 12);
  viewMatrix[15] = 1.0;
  { float smO[3] = { g_smLightOriginX, g_smLightOriginY, g_smLightOriginZ };
  viewMatrix[7] = -DotProduct210(upVec, smO);
  viewMatrix[11] = -DotProduct210(negDir, smO); }
  if ( g_smPerspectiveEnabled )
    #define SHADOW_FOV 90.0
    SetPerspectiveProjection(perspMatrix, SHADOW_FOV, SHADOW_FOV, 1.0);
  else
    MatrixIdentity44(perspMatrix);
  MatrixMultiply44(viewMatrix, perspMatrix, &g_smViewProjMatrix);
  MatrixInverse44(&g_smViewProjMatrix, g_smInvViewProjMatrix);
  MatrixTranspose44(g_smInvViewProjMatrix, g_smTransposedInvMatrix);
}
/*
================
SM_ProjectPoint

Projects a 3D point through the shadow projection matrix into 4D homogeneous coordinates.
================
*/
void SM_ProjectPoint(float *projected, float *pt)
{
  Assert(pt, s_assertDisable_SM_ProjectPoint);
  Assert(projected, s_assertDisable_SM_ProjectPoint);
  Assert(pt != projected, s_assertDisable_SM_ProjectPoint);
#ifdef _WIN64
  projected[0] = (float)((double)g_smViewProjMatrix_01*pt[1] + (double)g_smViewProjMatrix_02*pt[2] + (double)g_smViewProjMatrix*pt[0] + (double)g_smViewProjMatrix_03);
  projected[1] = (float)((double)g_smViewProjMatrix_11*pt[1] + (double)g_smViewProjMatrix_12*pt[2] + (double)g_smViewProjMatrix_10*pt[0] + (double)g_smViewProjMatrix_13);
  projected[2] = (float)((double)g_smViewProjMatrix_20*pt[0] + (double)g_smViewProjMatrix_21*pt[1] + (double)g_smViewProjMatrix_22*pt[2] + (double)g_smViewProjMatrix_23);
  projected[3] = (float)((double)g_smViewProjMatrix_31*pt[1] + (double)g_smViewProjMatrix_32*pt[2] + (double)g_smViewProjMatrix_30*pt[0] + (double)g_smViewProjMatrix_33);
#else
  projected[0] = g_smViewProjMatrix_01 * pt[1] + g_smViewProjMatrix_02 * pt[2] + g_smViewProjMatrix * pt[0] + g_smViewProjMatrix_03;
  projected[1] = g_smViewProjMatrix_11 * pt[1] + g_smViewProjMatrix_12 * pt[2] + g_smViewProjMatrix_10 * pt[0] + g_smViewProjMatrix_13;
  projected[2] = g_smViewProjMatrix_20 * pt[0] + g_smViewProjMatrix_21 * pt[1] + g_smViewProjMatrix_22 * pt[2] + g_smViewProjMatrix_23;
  projected[3] = g_smViewProjMatrix_31 * pt[1] + g_smViewProjMatrix_32 * pt[2] + g_smViewProjMatrix_30 * pt[0] + g_smViewProjMatrix_33;
#endif
}

/*
================
SM_ProjectTriRecord

Projects a triangle record's 3 vertices through the shadow projection
matrix and computes the projected bounding box.
================
*/
int SM_ProjectTriRecord(SmCasterTri_t *rec)
{
  float projected[4];
  vec3_t normalized;
  double invW;
  int i;

  ClearBounds(rec->projMins, rec->projMaxs);

  /* project each of the 3 corner vertices and compute normalized bounds */
  for ( i = 0; i < 3; i++ )
  {
    SM_ProjectPoint(projected, &rec->corners[i * 3]);
    invW = 1.0 / projected[3];
    VectorScale(projected, invW, normalized);
    AddPointToBounds(normalized, rec->projMins, rec->projMaxs);
  }

  return 0;
}

/*
================
SM_AddCasterTriangle

Adds a shadow caster triangle record. Computes plane from triangle,
dot-products with shadow direction, stores 80-byte record.
DEAD CODE: Zero cross-references in original exe.
================
*/
void SM_AddCasterTriangle(int usage, float *corners)
{
  float plane[4];
  char flag;
  int count;
  SmCasterTri_t *rec;
  double dot;

  AssertFatal(corners, s_assertDisable_SM_AddCasterTriangle);
  AssertFatal(usage, s_assertDisable_SM_AddCasterTriangle);

  /* compute plane from triangle vertices */
  if ( !PlaneFromPoints(plane, corners, &corners[3], &corners[6]) )
    return;

  /* classify triangle facing relative to shadow light */
  { float smDir[3] = { g_smLightDir_x, g_smLightDir_y, g_smLightDir_z };
  dot = DotProduct(smDir, plane) - g_smLightDist * plane[3]; }

  if ( dot < -PLANESIDE_EPSILON )
  {
    flag = 1;  /* backfacing */
    if ( usage == 2 )
      return;  /* usage 2 skips backfacing */
  }
  else if ( dot > PLANESIDE_EPSILON )
  {
    flag = 0;  /* frontfacing */
  }
  else
  {
    return;    /* edge-on — skip */
  }

  count = g_smNumCasterTris;
  if ( count == MAX_MAP_TRIANGLES )
  {
    Com_Error("SHADOW_CASTER_TRIS_LIMIT (%i) exceeded\n", count);
    count = g_smNumCasterTris;
  }

  /* store caster record */
  rec = &g_smCasterTriPool[count];
  VectorCopy(plane, rec->plane);
  rec->plane[3] = plane[3];
  rec->flag = flag;
  memcpy(rec->corners, corners, 9 * sizeof(float));

  SM_ProjectTriRecord(rec);

  if ( rec->projMaxs[2] > 0.0f )
    g_smNumCasterTris++;
}

/*
================
SM_CreateFragment

Creates a shadow caster fragment from a winding and projected points.
Computes local and global bounds for the fragment.
================
*/
TriSurf_t *SM_CreateFragment(Winding_t *winding, TriSurfProps_t *material, SmAuxVert_t *auxData, double lightDist)
{
  int idx;
  float localMins[3];
  float localMaxs[3];
  TriSurf_t *casterSurf;

  Assert(winding, s_assertDisable_SM_CreateFragment);
  Assert(winding->numpoints >= 3, s_assertDisable_SM_CreateFragment);
  if ( !material )
    material = EmitShadowcasterTriSurface(lightDist, winding);
  casterSurf = PrependTriSurf(winding, auxData, sizeof(SmAuxVert_t), material, &g_smCasterWindingList);
  ClearBounds(localMins, localMaxs);
  for ( idx = 0; idx < winding->numpoints; idx++ )
  {
    SmAuxVert_t *av = &auxData[idx];
    AssertFatal(winding->points[idx][0] == av->orig[0] && winding->points[idx][1] == av->orig[1] && winding->points[idx][2] == av->orig[2], s_assertDisable_SM_CreateFragment);
    AddPointToBounds(av->orig, g_smGridMins, g_smGridMaxs);
    AddPointToBounds(av->proj, localMins, localMaxs);
  }
  AddBoundsToBounds(localMins, localMaxs, g_smCasterBoundsMin, g_smCasterBoundsMax);
  return casterSurf;
}

/*
================
SM_ComputeSidePlane

Computes a shadow volume side plane from an edge and light direction.
Handles both point and directional lights, with optional plane flip.
================
*/
char SM_ComputeSidePlane(float *edgeVert, float *outPlane, float *baseVert, char flipFlag)
{
  vec3_t edgeDir, lightDir;

  VectorSubtract(edgeVert, baseVert, edgeDir);
  if ( g_smPerspectiveEnabled )
  {
    lightDir[0] = g_smLightOriginX - baseVert[0];
    lightDir[1] = g_smLightOriginY - baseVert[1];
    lightDir[2] = g_smLightOriginZ - baseVert[2];
  }
  else
  {
    lightDir[0] = g_smLightDirX;
    lightDir[1] = g_smLightDirY;
    lightDir[2] = g_smLightDirZ;
  }
  CrossProduct(lightDir, edgeDir, outPlane);
  if ( 0.0 == VecNormalize(outPlane) )
    return 0;

  if ( flipFlag == 1 )
    VectorScale(outPlane, -1, outPlane);

  outPlane[3] = (float)((double)outPlane[2] * (double)baseVert[2] + (double)outPlane[1] * (double)baseVert[1] + (double)baseVert[0] * (double)outPlane[0]);
  return 1;
}

/*
================
SM_RayPlaneIntersect

Computes ray-plane intersection parameter t.
================
*/
double SM_RayPlaneIntersect(float *rayDir, float *rayOrigin, float *plane)
{
  float denom;

  denom = DotProduct210(rayDir, plane);
  Assert(denom != 0.0f, s_assertDisable_SM_RayPlaneIntersect);
  return (plane[3] - DotProduct210(rayOrigin, plane)) / denom;
}

/*
================
SM_AllocProjectedPoints

Allocates projected points from winding vertices. Each point is 28 bytes:
origXYZ (12), projXYZ (12), flags (2), pad (2). Projects each vertex through the shadow matrix.
================
*/
SmAuxVert_t *SM_AllocProjectedPoints(Winding_t *winding)
{
  SmAuxVert_t *buf;
  int i;

  buf = malloc(sizeof(SmAuxVert_t) * winding->numpoints);

  for ( i = 0; i < winding->numpoints; i++ )
  {
    /* copy original XYZ */
    VectorCopy(winding->points[i], buf[i].orig);

    /* project and store projXYZW */
    SM_ProjectPoint(buf[i].proj, buf[i].orig);

    buf[i].flagA = 1;
    buf[i].flagB = 1;
  }

  return buf;
}

/*
================
SM_ProjectWindingThroughOccluders

Projects a winding through occluders with vertex nudge. Finds the best
occluder by ray-plane distance, then nudges winding vertices halfway
toward it. Returns a new fragment with projected points.
================
*/
int *SM_ProjectWindingThroughOccluders(Winding_t *winding)
{
  double t = 0.0;
  int i, j;
  vec3_t rayDir;
  float sumDists[SM_MAX_OCCLUDERS];
  float maxDists[SM_MAX_OCCLUDERS];
  char validFlags[SM_MAX_OCCLUDERS];
  SmOccluder_t *bestOcc;
  float bestMaxDist, bestSumDist;
  SmAuxVert_t *projectedPts;

  Assert(winding, s_assertDisable_SM_ProjectWindingThroughOccluders);
  Assert(g_smOccluderArray, s_assertDisable_SM_ProjectWindingThroughOccluders);

  /* initialize per-occluder tracking arrays */
  memset(sumDists, 0, g_smNumOccluders * sizeof(float));
  memset(maxDists, 0, g_smNumOccluders * sizeof(float));
  for ( i = 0; i < g_smNumOccluders; i++ )
    validFlags[i] = !g_smOccluderArray[i].casterTri->flag && g_smOccluderArray[i].active;

  /* for each vertex, cast ray toward light and accumulate occluder distances */
  for ( i = 0; i < winding->numpoints; i++ )
  {
    if ( g_smPerspectiveEnabled )
    {
      rayDir[0] = g_smLightOriginX - winding->points[i][0];
      rayDir[1] = g_smLightOriginY - winding->points[i][1];
      rayDir[2] = g_smLightOriginZ - winding->points[i][2];
    }
    else
    {
      rayDir[0] = g_smLightDirX;
      rayDir[1] = g_smLightDirY;
      rayDir[2] = g_smLightDirZ;
    }
    VecNormalize(rayDir);

    for ( j = 0; j < g_smNumOccluders; j++ )
    {
      if ( !validFlags[j] )
        continue;

      t = SM_RayPlaneIntersect(rayDir, winding->points[i], (float *)g_smOccluderArray[j].casterTri);
      if ( t >= SM_RAY_NEAR_THRESHOLD )
      {
        sumDists[j] += (float)t;
        if ( t > maxDists[j] )
          maxDists[j] = (float)t;
      }
      else
      {
        validFlags[j] = 0;
      }
    }
  }

  /* find best occluder: largest max distance, tiebreak by sum */
  bestOcc = NULL;
  bestMaxDist = 0.0f;
  bestSumDist = 0.0f;
  for ( i = 0; i < g_smNumOccluders; i++ )
  {
    if ( !validFlags[i] )
      continue;

    if ( maxDists[i] + PLANESIDE_EPSILON >= bestMaxDist )
    {
      if ( maxDists[i] - PLANESIDE_EPSILON >= bestMaxDist || bestSumDist <= (double)sumDists[i] )
      {
        bestMaxDist = maxDists[i];
        bestSumDist = sumDists[i];
        bestOcc = &g_smOccluderArray[i];
      }
    }
  }

  /* nudge winding vertices halfway toward the best occluder */
  if ( bestOcc )
  {
    for ( i = 0; i < winding->numpoints; i++ )
    {
      if ( g_smPerspectiveEnabled )
      {
        rayDir[0] = g_smLightOriginX - winding->points[i][0];
        rayDir[1] = g_smLightOriginY - winding->points[i][1];
        rayDir[2] = g_smLightOriginZ - winding->points[i][2];
      }
      else
      {
        rayDir[0] = g_smLightDirX;
        rayDir[1] = g_smLightDirY;
        rayDir[2] = g_smLightDirZ;
      }
      VecNormalize(rayDir);
      t = SM_RayPlaneIntersect(rayDir, winding->points[i], (float *)bestOcc->casterTri) * 0.5;
      VectorMA(winding->points[i], (float)t, rayDir, winding->points[i]);
    }
  }

  projectedPts = SM_AllocProjectedPoints(winding);
  return (int *)SM_CreateFragment(winding, 0, projectedPts, t);
}

/*
================
SM_ProjectWindingThroughOccluders_r

Recursive wrapper that clips a winding by occluders then projects through.
================
*/
int SM_ProjectWindingThroughOccluders_r(Winding_t *winding)
{
  return ShadowMid_ClipWindingByOccluders_r(winding, 0, (int (*)(Winding_t *))SM_ProjectWindingThroughOccluders, 0, 0);
}

/*
================
SM_InitOccluder

Initializes an occluder record with side planes computed from the
caster winding. Tests each side against receiver points to validate.
================
*/
char SM_InitOccluder(vec3_t *receiverPts, int receiverPtCount, SmCasterTri_t *caster, Winding_t *winding, SmOccluder_t *occ)
{
  SmCasterTri_t *tri = caster;
  int i, nextIdx;
  float minDist, maxDist;

  Assert(caster, s_assertDisable_SM_InitOccluder);
  Assert(receiverPts, s_assertDisable_SM_InitOccluder);
  Assert(receiverPtCount >= 3, s_assertDisable_SM_InitOccluder);
  Assert((float *)receiverPts != caster->corners, s_assertDisable_SM_InitOccluder);
  Assert(winding, s_assertDisable_SM_InitOccluder);
  Assert(occ, s_assertDisable_SM_InitOccluder);
  #define MAX_OCCLUDER_WINDING_POINTS 4
  Assert(winding->numpoints <= MAX_OCCLUDER_WINDING_POINTS, s_assertDisable_SM_InitOccluder);

  occ->casterTri = tri;
  occ->polygon = winding;

  /* compute side planes and validate against receiver */
  for ( i = 0; i < winding->numpoints; i++ )
  {
    nextIdx = (i + 1) % winding->numpoints;
    SM_ComputeSidePlane(winding->points[nextIdx], occ->planes[i], winding->points[i], tri->flag);
    WindingPlaneDistExtent(winding, occ->planes[i], occ->planes[i][3], &minDist, &maxDist);

    Assert(maxDist <= ON_EPSILON || minDist < -maxDist, s_assertDisable_SM_InitOccluder);

    /* if all receiver points are on the front side of this plane, reject */
    if ( CheckPointsAgainstPlane(receiverPts, receiverPtCount, occ->planes[i], occ->planes[i][3], ON_EPSILON) )
    {
      FreeWinding(winding);
      return 0;
    }
  }

  occ->area = WindingSignedArea(winding, (float *)tri);
  return 1;
}

/*
================
SM_FindOccluders_r

Recursively finds occluders from the AABB tree for a given caster.
Tests bounds intersection, recurses into children, and initializes
occluder records for matching leaf triangles.

NOTE: caster parameter uses SmCasterBounds_t layout:
  plane[4], vertCount, *receiverVerts, bounds[6] (mins[3]+maxs[3])
================
*/
int SM_FindOccluders_r(SmOccluderNode_t *node, SmCasterBounds_t *caster, int foundCount, int maxOccluders, SmOccluder_t *occluderBuf)
{
  SmCasterBounds_t *casterPtr;
  SmOccluderNode_t *occ;
  int result, childCount, childIdx, leafIdx;
  SmCasterTri_t *tri;
  Winding_t *w;

  casterPtr = caster;
  occ = node;

  /* early out: no overlap between node AABB and caster projected bounds */
  if ( occ->mins[0] >= (double)caster->maxs[0]
    || occ->maxs[0] <= (double)caster->mins[0]
    || occ->mins[1] >= (double)caster->maxs[1]
    || occ->maxs[1] <= (double)caster->mins[1]
    || occ->mins[2] >= (double)caster->maxs[2] )
  {
    return foundCount;
  }
  childCount = occ->childCount;
  if ( childCount )
  {
    /* internal node: recurse into children */
    SmOccluderNode_t *childBase = occ->childPtr;
    result = foundCount;
    for ( childIdx = 0; childIdx < childCount; childIdx++ )
      result = SM_FindOccluders_r(&childBase[childIdx], casterPtr, result, maxOccluders, occluderBuf);
  }
  else
  {
    /* leaf node: test each tri against caster bounds */
    SmCasterTri_t *triBase = occ->triPtr;
    SmOccluder_t *occSlot = &occluderBuf[foundCount];
    for ( leafIdx = 0; leafIdx < occ->triCount; leafIdx++ )
    {
      tri = &triBase[leafIdx];

      /* AABB overlap test: tri projected bounds vs caster bounds */
      if ( (double)tri->projMins[0] < (double)casterPtr->maxs[0]
          && (double)tri->projMaxs[0] > (double)casterPtr->mins[0]
          && (double)tri->projMins[1] < (double)casterPtr->maxs[1]
          && (double)tri->projMaxs[1] > (double)casterPtr->mins[1]
          && (double)tri->projMins[2] < (double)casterPtr->maxs[2] )
        {
          /* build winding from tri corners and clip against caster plane */
          w = AllocWinding(3);
          w->numpoints = 3;
          memcpy(w->points, tri->corners, 9 * sizeof(float));
          ClipWindingByPlane(&w, casterPtr->plane, casterPtr->plane[3], ON_EPSILON);

          if ( w )
          {
            RemoveDuplicatePoints(w, ON_EPSILON);
            if ( w->numpoints >= 3 )
            {
              if ( foundCount == maxOccluders )
                Com_Error("More than %i potential occluders to a shadow casting poly", maxOccluders);

              /* initialize occluder from clipped winding */
              if ( SM_InitOccluder((vec3_t *)casterPtr->receiverVerts, casterPtr->vertCount, tri, w, occSlot) )
              {
                foundCount++;
                occSlot++;
              }
            }
            else
            {
              FreeWinding(w);
            }
          }
          occ = (SmOccluderNode_t *)node;
        }
    }
    return foundCount;
  }
  return result;
}

/*
================
SM_FindOccluders

Finds and sorts occluders for a shadow caster from the AABB tree.
================
*/
size_t SM_FindOccluders(SmCasterBounds_t *caster, int maxOccluders, void *occluderBuf)
{
  size_t foundCount;

  foundCount = SM_FindOccluders_r(g_smOccluderTree, caster, 0, maxOccluders, occluderBuf);
  qsort(occluderBuf, foundCount, sizeof(SmOccluder_t), (int (*)(const void*, const void*))ShadowMid_CompareOccluderEntries);
  return foundCount;
}

/*
================
SM_FindOccludersFlipped

Finds occluders with a flipped caster plane. Negates the plane normal
and distance, then searches the AABB tree and sorts results.
================
*/
size_t SM_FindOccludersFlipped(SmCasterTri_t *caster, void *occluderBuf, int maxOccluders)
{
  size_t foundCount;

  /* build flipped caster record: negate plane, copy bounds and corners pointer */
  SmCasterBounds_t flipped;

  flipped.plane[0] = -caster->plane[0];
  flipped.plane[1] = -caster->plane[1];
  flipped.plane[2] = -caster->plane[2];
  flipped.plane[3] = -caster->plane[3];
  flipped.vertCount = 3;
  flipped.receiverVerts = caster->corners;
  flipped.mins[0] = caster->projMins[0];
  flipped.mins[1] = caster->projMins[1];
  flipped.mins[2] = caster->projMins[2];
  flipped.maxs[0] = caster->projMaxs[0];
  flipped.maxs[1] = caster->projMaxs[1];
  flipped.maxs[2] = caster->projMaxs[2];

  foundCount = SM_FindOccluders_r(g_smOccluderTree, &flipped, 0, maxOccluders, occluderBuf);
  qsort(occluderBuf, foundCount, sizeof(SmOccluder_t), (int (*)(const void*, const void*))ShadowMid_CompareOccluderEntries);
  return foundCount;
}

/*
================
ShadowMid_FindMinimalCastingTris

Iterates all shadow tri records, clips each marked caster through
occluders to find the minimal set of shadow casting tris.
Expands global shadow bounds by 1.0 in each direction afterwards.
================
*/
void ShadowMid_FindMinimalCastingTris(void)
{
  SmOccluder_t occluderBuffer[SM_MAX_OCCLUDERS];  /* stack buffer for occluder entries */
  SmCasterTri_t *tri;
  Winding_t *w;
  int triIdx, i;

  printf("finding minimal shadow casting tris...\n");
  ClearBounds(g_smGridMins, g_smGridMaxs);
  ClearBounds(g_smCasterBoundsMin, g_smCasterBoundsMax);
  g_smOccluderArray = occluderBuffer;

  for ( triIdx = 0; triIdx < g_smNumCasterTris; triIdx++ )
  {
    tri = &g_smCasterTriPool[triIdx];
    if ( tri->flag != 1 )
      continue;

    /* find occluders for this backfacing tri */
    g_smNumOccluders = (int)SM_FindOccludersFlipped(tri, occluderBuffer, SM_MAX_OCCLUDERS);
    g_smCurrentCasterTri = tri;

    /* build winding from tri corners and clip through occluders */
    w = AllocWinding(3);
    w->numpoints = 3;
    memcpy(w->points, tri->corners, 9 * sizeof(float));
    ShadowMid_ClipWindingByOccluders_r(
      w, 1,
      (int (*)(Winding_t *))SM_ProjectWindingThroughOccluders_r,
      (int (*)(Winding_t *))FreeWinding, 0);

    /* free occluder windings */
    for ( i = 0; i < g_smNumOccluders; i++ )
      FreeWinding((Winding_t *)g_smOccluderArray[i].polygon);
  }

  /* expand grid bounds by 1 unit */
  g_smGridMins[0] -= 1.0f;
  g_smGridMins[1] -= 1.0f;
  g_smGridMins[2] -= 1.0f;
  g_smGridMaxs[0] += 1.0f;
  g_smGridMaxs[1] += 1.0f;
  g_smGridMaxs[2] += 1.0f;
}


/*
================
SM_CanMergeSurfaces

Tests if two shadow caster surfaces are coplanar and can be merged.
Checks normal dot product and vertex-to-plane distances.
================
*/
char SM_CanMergeSurfaces(TriSurf_t *tsA, TriSurf_t *tsB)
{
  TriSurfProps_t *propsA = tsA->props;
  TriSurfProps_t *propsB = tsB->props;
  Winding_t *w;
  int i;
  double distA, distB;

  if ( propsA == propsB )
    return 1;

  /* check if normals are close enough to merge */
  if ( DotProduct210(propsA->plane, propsB->plane) < SM_MERGE_DOT_THRESHOLD )
    return 0;

  /* check if all of tsB's vertices lie on both planes */
  w = tsB->winding;
  for ( i = 0; i < w->numpoints; i++ )
  {
    distB = DotProduct201(w->points[i], propsB->plane) - propsB->plane[3];
    if ( distB * distB > SM_COPLANAR_DIST_SQ )
      return 0;
    distA = DotProduct210(w->points[i], propsA->plane) - propsA->plane[3];
    if ( distA * distA > SM_COPLANAR_DIST_SQ )
      return 0;
  }

  return 1;
}

/*
================
SM_InterpolateVert

Interpolates between two auxiliary vertex records by fraction.
Lerps 6 float fields and copies char flags from both endpoints.
================
*/
char SM_InterpolateVert(float *from, float *to, double frac, float *result)
{
  int i;

  Assert(from != NULL, s_assertDisable_SM_InterpolateVert);
  Assert(to != NULL, s_assertDisable_SM_InterpolateVert);
  Assert(result != NULL, s_assertDisable_SM_InterpolateVert);

  /* interpolate 6 floats: origXYZ + projXYZ */
  for ( i = 0; i < 6; i++ )
    result[i] = FMA1(from[i], to[i] - from[i], frac);

  /* copy flag bytes: result gets to's flagB and from's flagA */
  ((SmAuxVert_t *)result)->flagA = ((SmAuxVert_t *)to)->flagB;
  ((SmAuxVert_t *)result)->flagB = ((SmAuxVert_t *)from)->flagA;

  return ((SmAuxVert_t *)to)->flagB;
}

/*
================
SM_FlattenSurfToWinding

Flattens a surface to 2D winding for notch plugging operations.
Copies projected XY from aux data, sets Z=0, saves original plane
and sets up a Z-up plane for 2D operations.
================
*/
float *SM_FlattenSurfToWinding(TriSurf_t *ts)
{
  Winding_t *w;
  float *vert;
  int i;

  CheckWindingInPlane(ts->winding, ts->props->plane, ts->props->plane[3]);
  w = ts->winding;
  Assert(w->numpoints > 0, s_assertDisable_SM_FlattenSurfToWinding);

  /* replace winding XYZ with projected XY from aux data, Z=0 */
  vert = (float *)ts->auxData;
  for ( i = 0; i < w->numpoints; i++ )
  {
    w->points[i][0] = vert[i * SM_AUX_VERT_STRIDE + SM_AUX_PROJ_OFS];  /* projX */
    w->points[i][1] = vert[i * SM_AUX_VERT_STRIDE + SM_AUX_PROJ_OFS + 1];  /* projY */
    w->points[i][2] = 0.0f;
  }

  /* save original plane into texVecs[0..3] for later restoration */
  VectorCopy(ts->props->plane, ts->props->texVecs);
  ts->props->texVecs[3] = ts->props->plane[3];

  /* set Z-up plane for 2D operations */
  VectorClear(ts->props->plane);
  ts->props->plane[2] = 1.0f;
  ts->props->plane[3] = 0.0f;

  return ts->props->plane;
}

/*
================
SM_RestoreSurfFromWinding

Restores a surface from 2D winding back to 3D coordinates.
Copies XYZ from aux data back to winding vertices and restores
the original plane from the saved copy.
================
*/
float *SM_RestoreSurfFromWinding(TriSurf_t *ts)
{
  Winding_t *w;
  float *vert;
  int i;

  w = ts->winding;
  Assert(w->numpoints > 0, s_assertDisable_SM_RestoreSurfFromWinding);

  /* restore original XYZ from aux data back to winding */
  vert = (float *)ts->auxData;
  for ( i = 0; i < w->numpoints; i++ )
    VectorCopy(&vert[i * SM_AUX_VERT_STRIDE], w->points[i]);

  /* restore original plane from saved copy in texVecs */
  VectorCopy(ts->props->texVecs, ts->props->plane);
  ts->props->plane[3] = ts->props->texVecs[3];

  /* clear the saved copy */
  ts->props->texVecs[0] = 0.0f;
  ts->props->texVecs[1] = 0.0f;
  ts->props->texVecs[2] = 0.0f;
  ts->props->texVecs[3] = 0.0f;

  return ts->props->texVecs;
}

/*
================
SM_MergeSurfacesCallback

Merge callback that repeatedly tries to merge concave pairs
until no more merges occur.
================
*/
int SM_MergeSurfacesCallback(TriSurf_t **surfArray, IntWinding_t **windingArray, int count, int auxStride, TriSurf_t *baseSurf, IntWinding_t *baseWinding, int flags)
{
  int currentCount;
  int idx;
  int prevCount;

  currentCount = count;
  do
  {
    idx = 0;
    prevCount = currentCount;
    if ( currentCount <= 0 )
      break;
    do
      currentCount = ShadowMid_TryMergeConcavePair(idx++, surfArray, windingArray, currentCount, auxStride);
    while ( idx < currentCount );
  }
  while ( prevCount != currentCount );
  return currentCount;
}

/*
================
SM_PlugNotchesCallback

Notch callback that merges pairs then plugs remaining notches
in the merged winding.
================
*/
int SM_PlugNotchesCallback(TriSurf_t **surfArray, IntWinding_t **neighbors, int count, int auxStride, TriSurf_t *baseSurf, IntWinding_t *baseWinding, int flags)
{
  int idx;
  int prevCount;

  do
  {
    idx = 0;
    prevCount = count;
    if ( count <= 0 )
      break;
    do
      count = ShadowMid_TryMergeNotchPair(idx++, surfArray, neighbors, count, auxStride);
    while ( idx < count );
  }
  while ( prevCount != count );
  return ShadowMid_PlugNotchesInWinding(surfArray, neighbors, count, (int *)baseSurf, baseWinding);
}

/*
================
MergeShadowCasters

Merges shadow casters into concave groups using visibility grouping.
================
*/
void MergeShadowCasters(Tree_t *visGroups)
{
  printf("merging into concave shadow casters...\n");
  SetTrisTransientMode(2, 0);
  MergeSurfaces_Init(MAX_SHADOW_MERGE_SURFS, (int (*)(TriSurf_t *, TriSurf_t *))SM_CanMergeSurfaces, (MergeCallback_t)1);
  MergeVisGroupList(
    &g_smCasterWindingList,
    g_smGridMins,
    g_smGridMaxs,
    visGroups,
    SM_MergeSurfacesCallback);
  MergeSurfaces_Shutdown();
  ChopConcaveCasterWindings();
  SetTrisTransientMode(0, 1);
}

/*
================
PlugShadowCasterNotches

Plugs notches in shadow casting geometry. Flattens all casters to 2D,
runs notch plugging merge, then restores to 3D.
================
*/
void PlugShadowCasterNotches(void)
{
  TriSurf_t *caster;
  float searchMins[3], searchMaxs[3];

  printf("plugging notches in shadow casting geometry...\n");

  /* flatten all casters to 2D for notch operations */
  for ( caster = g_smCasterWindingList; caster; caster = caster->next )
    SM_FlattenSurfToWinding(caster);

  /* set up 2D search bounds and plug notches */
  MergeSurfaces_Init(MAX_SHADOW_MERGE_SURFS, (int (*)(TriSurf_t *, TriSurf_t *))ShadowMid_WindingBehindPlane, (MergeCallback_t)0);
  SetTrisTransientMode(2, 0);
  searchMins[0] = g_smCasterBoundsMin[0];
  searchMins[1] = g_smCasterBoundsMin[1];
  searchMins[2] = 0.0f;
  searchMaxs[0] = g_smCasterBoundsMax[0];
  searchMaxs[1] = g_smCasterBoundsMax[1];
  searchMaxs[2] = 0.0f;
  PlugNotchesInList(
    &g_smCasterWindingList,
    searchMins,
    searchMaxs,
    ShadowMid_IsConvex,
    SM_PlugNotchesCallback);
  SetTrisTransientMode(0, 1);
  MergeSurfaces_Shutdown();

  /* restore all casters back to 3D */
  for ( caster = g_smCasterWindingList; caster; caster = caster->next )
    SM_RestoreSurfFromWinding(caster);
}

/*
================
BuildMidpointShadowCasters

Main shadow midpoint processing pipeline. Sorts shadow tris,
finds minimal casting set, merges casters, fixes T-junctions,
plugs notches, snaps/merges verts, triangulates concave casters.
================
*/
void BuildMidpointShadowCasters(Tree_t *visGroups)
{
  if ( g_smNumCasterTris )
  {
    GridTree_Init();
    TJunc_Init(sizeof(SmAuxVert_t));  /* aux vertex stride: 7 floats per projected vertex */
    g_lerpAuxDataCallback = (void (*)(float *, float *, double, float *))SM_InterpolateVert;
    ShadowMid_SortShadowTris();
    ShadowMid_FindMinimalCastingTris();
    MergeShadowCasters(visGroups);
    ShadowMid_SimplifyConcaveHoles();
    PlugShadowCasterNotches();
    ShadowMid_FixTJunctions();
    ShadowMid_SnapAndMergeVerts();
    ShadowMid_TriangulateConcaveCasters();
    FreeAllTriSurfs();
    g_lerpAuxDataCallback = NULL;
    TJunc_Init(0);
    GridTree_Shutdown();
  }
  ShadowMid_Shutdown();
}
