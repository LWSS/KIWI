/*
tris_lightmap.c — native triangle lightmap grouping and assignment (TrisLmap_*)

KIWI: this file once also held the CoD2 shadow-midpoint caster system; it was dead
in CoD4 and is gone.
*/

#include "cod4map.h"

/* Native TRIS_TRANSIENT_LMAP state, recovered from 0x451980. */
static TrisLmapGroup_t *s_trisLmapGroupList;
static int s_trisLmapGroupCount;

/* 0x4540B0/0x455370 reserve 2448 material heads while assembling the native
   assignment order.  The heads use TrisLmapTransient_t::nextInMaterial. */
/* 0x451600 reserves 0x4c8 material-list entries on the stack. */
#define TRIS_LMAP_MAX_MATERIALS 1224
static TriSurf_t *s_trisLmapMaterialHeads[TRIS_LMAP_MAX_MATERIALS];
static int s_trisLmapMaterialCount;
static TrisLmapAssignmentSidecar_t *s_trisLmapAssignments;
static TrisLmapRasterScratch_t s_trisLmapRasterScratch;
/* 0x453C60's temporary surface array and its two exact collection counts. */
static TriSurf_t **s_trisLmapSeparateSurfs;
static int s_trisLmapSeparateLmapCount;
static int s_trisLmapSeparateNoLmapCount;

#define TRIS_LMAP_GROUP_EPSILON 0.00007324219f
#define TRIS_LMAP_ASSIGNMENT_EPSILON 0.000097656251f

static int TrisLmap_TexelFloor(float uv);
static int TrisLmap_TexelCeil(float uv);
static void TrisLmap_ValidateGroup(const TrisLmapGroup_t *group);

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

/* 0x455070.  The clone predicate reads the native 0xAC property carrier
   directly.  In particular, an unassigned (-1) group never needs cloning;
   sidecars are output-bridge mirrors and must not participate here. */
int TrisLmap_AssignmentNeedsClone(const TriSurf_t *surf, const TriSurfProps_t *props,
                                  const TrisLmapAssignmentPayload_t *payload)
{
    Assert(surf, s_assertDisable_TrisLmapCreateSurface);
    Assert(props, s_assertDisable_TrisLmapCreateSurface);
    Assert(payload, s_assertDisable_TrisLmapCreateSurface);

    if (props->lmapGroupId == -1)
        return 0;
    if (props->lmapGroupId != payload->groupId)
        return 1;
    if (props->lmapIndex != LIGHTSTYLE_NONE && props->lmapIndex != payload->lightmapIndex)
        return 1;
    return !VectorCompareEpsilon((float *)&props->lmapVecs[0], (float *)payload->vecs[0],
                                 TRIS_LMAP_ASSIGNMENT_EPSILON, 4)
        || !VectorCompareEpsilon((float *)&props->lmapVecs[4], (float *)payload->vecs[1],
                                 TRIS_LMAP_ASSIGNMENT_EPSILON, 4);
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

/* 0x453E80 orders surfaces by the ADDRESS of their property carrier, then by the
   address of their LMAP group carrier (which decides the group that keeps the
   original property object).  Native's address order is arbitrary (freed blocks
   get reused) but was the same every run on its 32-bit heap.  KIWI FIX: on x64
   the heap is randomized, so address order changed run to run - surface order,
   vertex numbering and therefore the smoothed-normal summation order (1-7 ulp
   normal/tangent noise) all varied, and the int-narrowed address difference could
   overflow.  Sort by creation order instead - a deterministic stand-in, not a
   reproduction of native's order: the property sidecar's allocSerial and the
   group's id (both counters), then the collection index so equal keys keep their
   list order. */
typedef struct TrisLmapSeparateKey_s
{
    unsigned int propsSerial;
    int          groupId;
    int          order;
    TriSurf_t   *surf;
} TrisLmapSeparateKey_t;

static int TrisLmap_CompareSeparateKeys(const void *left, const void *right)
{
    const TrisLmapSeparateKey_t *a = (const TrisLmapSeparateKey_t *)left;
    const TrisLmapSeparateKey_t *b = (const TrisLmapSeparateKey_t *)right;

    if (a->propsSerial != b->propsSerial)
        return a->propsSerial < b->propsSerial ? -1 : 1;
    if (a->groupId != b->groupId)
        return a->groupId < b->groupId ? -1 : 1;
    return a->order - b->order;
}

static void TrisLmap_SortSurfPropsAndGroup(TriSurf_t **surfs, int count)
{
    TrisLmapSeparateKey_t *keys;
    int index;

    keys =(TrisLmapSeparateKey_t *)malloc(sizeof(*keys) * count);
    if (!keys)
        Com_Error("TrisLmap_SortSurfPropsAndGroup: out of memory");
    for (index = 0; index < count; ++index)
    {
        TriSurfPropsSidecar_t *sidecar = TrisPropsSidecar_Get(surfs[index]->props);

        Assert(sidecar, s_assertDisable_TrisLmapCreateSurface);
        Assert(surfs[index]->lmap, s_assertDisable_TrisLmapCreateSurface);
        keys[index].propsSerial = sidecar ? sidecar->allocSerial : 0xFFFFFFFFu;
        keys[index].groupId = surfs[index]->lmap->group->id;
        keys[index].order = index;
        keys[index].surf = surfs[index];
    }
    qsort(keys, count, sizeof(*keys), TrisLmap_CompareSeparateKeys);
    for (index = 0; index < count; ++index)
        surfs[index] = keys[index].surf;
    free(keys);
}

/* 0x453E40: retain every surface with an LMAP carrier and count the rest.
   The caller preallocates this array to GetTrisCount(), exactly as native. */
static void TrisLmap_CollectSeparateCandidate(TriSurf_t *surf, bool isTarget)
{
    (void)isTarget;
    if (surf->lmap)
        s_trisLmapSeparateSurfs[s_trisLmapSeparateLmapCount++] = surf;
    else
        ++s_trisLmapSeparateNoLmapCount;
}

/* 0x453C60.  Native code gathers every active LMAP surface, sorts by props
   then group carrier, and keeps the first group's props while cloning the
   original props for every additional group. */
int TrisLmap_SeparatePropsByGroup(void)
{
    TriSurf_t **surfs;
    int surfCount;
    int index;

    surfCount = GetTrisCount();
    if (!surfCount)
        return 0;
    surfs = (TriSurf_t **)malloc(surfCount * sizeof(*surfs));
    if (!surfs)
        Com_Error("TrisLmap_SeparatePropsByGroup: out of memory");

    s_trisLmapSeparateSurfs = surfs;
    s_trisLmapSeparateLmapCount = 0;
    s_trisLmapSeparateNoLmapCount = 0;
    ForEachSurf(TrisLmap_CollectSeparateCandidate, NULL);
    Assert(s_trisLmapSeparateLmapCount + s_trisLmapSeparateNoLmapCount == surfCount,
           s_assertDisable_TrisLmapCreateSurface);
    s_trisLmapSeparateSurfs = NULL;
    if (!s_trisLmapSeparateLmapCount)
    {
        free(surfs);
        return 0;
    }
    TrisLmap_SortSurfPropsAndGroup(surfs, s_trisLmapSeparateLmapCount);   /* KIWI FIX: was qsort by address */

    for (index = 0; index < s_trisLmapSeparateLmapCount; )
    {
        TriSurfProps_t *sourceProps = surfs[index]->props;
        TriSurfProps_t *assignedProps = sourceProps;
        TrisLmapGroup_t *sourceGroup = surfs[index]->lmap->group;

        ++index;
        while (index < s_trisLmapSeparateLmapCount && surfs[index]->props == sourceProps)
        {
            if (surfs[index]->lmap->group != sourceGroup)
            {
                sourceGroup = surfs[index]->lmap->group;
                assignedProps = TrisLmap_CloneProps(surfs[index], sourceProps);
            }
            surfs[index]->props = assignedProps;
            ++index;
        }
    }
    free(surfs);
    return s_trisLmapSeparateLmapCount;
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
    Assert(GetTrisTransientMode() == 4, s_assertDisable_TrisLmapValidateSurface);

    lmap = surf->lmap;
    Assert(lmap->vecs[0][0] != 0.0f || lmap->vecs[0][1] != 0.0f
        || lmap->vecs[0][2] != 0.0f || lmap->vecs[0][3] != 0.0f,
        s_assertDisable_TrisLmapValidateSurface);
    Assert(lmap->vecs[1][0] != 0.0f || lmap->vecs[1][1] != 0.0f
        || lmap->vecs[1][2] != 0.0f || lmap->vecs[1][3] != 0.0f,
        s_assertDisable_TrisLmapValidateSurface);
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
    Assert(GetTrisTransientMode() == 4, s_assertDisable_TrisLmapCreateSurface);
    Assert(surf->props->lmapVecs[0] != 0.0f || surf->props->lmapVecs[1] != 0.0f
        || surf->props->lmapVecs[2] != 0.0f || surf->props->lmapVecs[3] != 0.0f,
        s_assertDisable_TrisLmapCreateSurface);
    Assert(surf->props->lmapVecs[4] != 0.0f || surf->props->lmapVecs[5] != 0.0f
        || surf->props->lmapVecs[6] != 0.0f || surf->props->lmapVecs[7] != 0.0f,
        s_assertDisable_TrisLmapCreateSurface);

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

        /* Native is a >= 512 veto, rather than a later allocator clamp. */
        int mergedSize = LightmapTexelSize(mergedMins[axis], mergedMaxs[axis]);
        int growSize = LightmapTexelSize(groupGrow->bounds[0][axis], groupGrow->bounds[1][axis]);
        int killSize = LightmapTexelSize(groupKill->bounds[0][axis], groupKill->bounds[1][axis]);

        if (mergedSize >= LIGHTMAP_SIZE)
            return 0;
        /* reject a merged span with a gap larger than the two source texel
           spans plus their shared edge */
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

    /* Native shifts the source group itself before validating its members.
       Until ownership changes, 0x451290 therefore checks the shifted local
       coordinates against these shifted source bounds. */
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
        /* Native validates against the shifted source group before its later
           pass changes lmap->group.  The destination bounds are not widened
           until after that, so switching ownership here makes the exact
           group-bounds validation spuriously fail. */
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

    /* 0x451C70(-0.00007324219, group->bounds): undo the create-time pad
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
        // KIWI: name the block -- a single group larger than one page lands here too.
        Com_Error("lightmap allocation failed... may be out of lightmap memory (%i x %i texel block, material %s)",
                  width, height,
                  group->firstSurf->props && group->firstSurf->props->si ? group->firstSurf->props->si->name : "?");
    }

    /* The allocator's y/x output order is intentional and matches the native:
       the first output offsets the first projected axis. */
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
        /* The reserved rectangle is transposed as well as translated.  Publish
           the projections in atlas-axis order; offsetting the original rows
           alone leaves geometry outside its allocation (and lets the reclaim
           pass return occupied texels to the allocator). */
        if (allocation->isTransposed)
        {
            float row[4];
            memcpy(row, surf->lmap->vecs[0], sizeof(row));
            memcpy(surf->lmap->vecs[0], surf->lmap->vecs[1], sizeof(row));
            memcpy(surf->lmap->vecs[1], row, sizeof(row));
        }
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

/* 0x454F90.  Assignment is published to the native property carrier first;
   the sidecar is retained only as a compatibility mirror for output paths
   which still explicitly request an assignment payload. */
static void TrisLmap_SetGroupAssignment(TrisLmapGroup_t *group, const TrisLmapAllocation_t *allocation)
{
    TriSurf_t *surf;
    TrisLmapAssignmentPayload_t payload;

    if ((unsigned int)allocation->lightmapIndex >= LIGHTSTYLE_NONE)
        Com_Error("lmapIndex doesn't index LIGHTMAP_NONE\n\t%i not in [0, %i)",
                  allocation->lightmapIndex, LIGHTSTYLE_NONE);
    payload.groupId = group->id;
    payload.lightmapIndex = allocation->lightmapIndex;
    for (surf = group->firstSurf; surf; surf = surf->lmap->nextInGroup)
    {
        TriSurfProps_t *props = surf->props;
        memcpy(payload.vecs, surf->lmap->vecs, sizeof(payload.vecs));
        if (TrisLmap_AssignmentNeedsClone(surf, props, &payload))
        {
            props = TrisLmap_CloneProps(surf, props);
            /* Native resets a clone's core atlas slot before 0x454F90
               publishes the new group/index pair. */
            props->lmapIndex = 31;
            surf->props = props;
        }
        /* the group/index and both four-float lightmap rows live in the native carrier and become authoritative here. */
        props->lmapGroupId = payload.groupId;
        props->lmapIndex = payload.lightmapIndex;
        memcpy(&props->lmapVecs[0], payload.vecs[0], sizeof(payload.vecs));
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

/* 0x4540B0, called from TrisLmap_BuildAndAssign. */
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
    /* Native compares the first winding dword, i.e. its point count.  It does
       not compare the winding allocation address. */
    if (left->winding->numpoints != right->winding->numpoints)
        return left->winding->numpoints < right->winding->numpoints;
    /* MaterialInfo is the native carrier reached through props[0].  Its
       dwords +36 and +32 are contents then surfaceFlags respectively;
       adapter ShaderInfo_t retains them as contentFlags/surfaceFlags. */
    if (left->props->si->contentFlags != right->props->si->contentFlags)
        return left->props->si->contentFlags < right->props->si->contentFlags;
    if (left->props->si->surfaceFlags != right->props->si->surfaceFlags)
        return left->props->si->surfaceFlags < right->props->si->surfaceFlags;
    /* Native's last key is the TriSurf address.  KIWI FIX: on the randomized x64
       heap that varied run to run; creation order is a deterministic stand-in. */
    return left->allocSerial < right->allocSerial;
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
    int index;

    Assert(surf && surf->props && surf->props->si, s_assertDisable_TrisLmapCreateSurface);
    index = FindLoadedMaterialIndex(surf->props->si);
    if (index < 0 || index >= TRIS_LMAP_MAX_MATERIALS)
        Com_Error("TrisLmap_LoadedMaterialIndex: material index %i exceeds table capacity %i",
                  index, TRIS_LMAP_MAX_MATERIALS);
    return index;
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
            VectorCopy(winding->points[pointIndex], points[outputCount]);
            ++outputCount;
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
            /* every clipped point is evaluated as kill then grow */
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
    if (!LoadedMaterialsShareTechniqueSet(current->props->si,
                                          candidate->props->si))
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
    if (!g_currentEntityIndex)
    {
        TrisTimerCheck(NULL);
        printf("%s...\n", "assigning lightmaps");
        g_trisTimerStart = I_FloatTime();
    }
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
