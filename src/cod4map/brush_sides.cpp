/* CoD4 brush-side visible-hull preparation (0x407820-0x4087F0). */
#include "cod4map.h"

typedef struct BrushSideTreeNode_s {
  Brush_t **brushes;                 /* +0x00 leaf brush range */
  int brushCount;                    /* +0x04 */
  struct BrushSideTreeNode_s *children; /* +0x08 contiguous children */
  int childCount;                    /* +0x0C, zero for a leaf */
  vec3_t mins;                       /* +0x10 */
  vec3_t maxs;                       /* +0x1C */
} BrushSideTreeNode_t;

typedef char brush_side_tree_node_size_must_be_0x28[
    sizeof(BrushSideTreeNode_t) == 0x28 ? 1 : -1];

typedef struct BrushSideGlobals_s {
  unsigned char isTreeInitialized;
  unsigned char hasOpaqueBrushes;
  unsigned char pad[2];
  Brush_t **opaqueBrushes;
  BrushSideTreeNode_t *nodes;
} BrushSideGlobals_t;

typedef void (*BrushSideTreeCallback_t)(Brush_t *opaqueBrush, void *data);

typedef struct BrushSideClipContext_s {
  float plane[4];
  WindingList_t *windingList;
  const DrawSurf_t *source;
} BrushSideClipContext_t;

static BrushSideGlobals_t brushSideGlob;
static int s_brushSideClipCount;
static char s_assertDisable_BrushSides;

/* CoD4 0x407E70's two query vectors are never initialized locally.  The
   immediately preceding 0x407820 frame leaves its AabbTreeBuilder header at
   exactly those six 32-bit stack slots: {opaqueBrushesPtr, opaqueCount, 4} followed
   by {0, freedMinsPtr, freedMaxsPtr}.  In the 32-bit tool these integer bit
   patterns are all tiny positive floats, so 0x408350's +/- 0.1 expansion is
   the deterministic origin query below.  Do not substitute subject bounds:
   that changes the native fragment list and downstream portal topology. */
static const vec3_t s_nativeVisibleHullQueryPoint = { 0.0f, 0.0f, 0.0f };

static void BrushSideTree_CalcBounds_r(BrushSideTreeNode_t *node)
{
  int i;

  ClearBounds(node->mins, node->maxs);
  if ( node->childCount )
  {
    for ( i = 0; i < node->childCount; ++i )
    {
      BrushSideTree_CalcBounds_r(&node->children[i]);
      AddBoundsToBounds(node->children[i].mins, node->children[i].maxs, node->mins, node->maxs);
    }
  }
  else
  {
    for ( i = 0; i < node->brushCount; ++i )
      AddBoundsToBounds(node->brushes[i]->eMins, node->brushes[i]->eMaxs, node->mins, node->maxs);
  }
}

/* CoD4 0x407CD0. */
static qboolean BrushSideTree_IsOpaqueBrush(const Brush_t *brush)
{
  return brush->isOpaque;
}

/* CoD4 0x408410. */
static void BrushSideTree_ForBounds_r(BrushSideTreeNode_t *node, const float *mins,
                                      const float *maxs, void *data,
                                      BrushSideTreeCallback_t callback)
{
  int i;

  if ( node->childCount )
  {
    for ( i = 0; i < node->childCount; ++i )
    {
      BrushSideTreeNode_t *child = &node->children[i];
      if ( BoundsIntersect(child->mins, child->maxs, (float *)mins, (float *)maxs) )
        BrushSideTree_ForBounds_r(child, mins, maxs, data, callback);
    }
  }
  else
  {
    for ( i = 0; i < node->brushCount; ++i )
    {
      Brush_t *brush = node->brushes[i];
      if ( BoundsIntersect(brush->eMins, brush->eMaxs, (float *)mins, (float *)maxs) )
        callback(brush, data);
    }
  }
}

/* CoD4 0x408350. */
static void BrushSideTree_ForBounds(BrushSideTreeNode_t *node, const float *mins,
                                    const float *maxs, void *data,
                                    BrushSideTreeCallback_t callback)
{
  vec3_t expandedMins;
  vec3_t expandedMaxs;

  expandedMins[0] = mins[0] - 0.1f;
  expandedMins[1] = mins[1] - 0.1f;
  expandedMins[2] = mins[2] - 0.1f;
  expandedMaxs[0] = maxs[0] + 0.1f;
  expandedMaxs[1] = maxs[1] + 0.1f;
  expandedMaxs[2] = maxs[2] + 0.1f;
  BrushSideTree_ForBounds_r(node, expandedMins, expandedMaxs, data, callback);
}

/* CoD4 0x4080F0.  Returns the link after the winding currently being processed. */
static WindingList_t **BrushSides_SubtractOpaqueBrush(WindingList_t **link,
                                                       const float *sideNormal,
                                                       Brush_t *opaqueBrush)
{
  float *crossPlanes[256];
  WindingList_t *node = *link;
  int crossCount = 0;
  int i;

  Assert(opaqueBrush->numSides <= 256, s_assertDisable_BrushSides);
  for ( i = 0; i < opaqueBrush->numSides; ++i )
  {
    float *plane = (float *)MAP_PLANE(opaqueBrush->sides[i].planenum);
    int side = WindingPlaneSide(node->winding, plane, plane[3]);

    if ( side == SIDE_CROSS )
      crossPlanes[crossCount++] = plane;
    else if ( side == SIDE_FRONT || (side == SIDE_ON && DotProduct021(plane, sideNormal) >= 0.0f) )
      return &node->next;
  }

  if ( ++s_brushSideClipCount == 2543 )
    return &node->next;

  for ( i = crossCount; i; )
  {
    Winding_t *front;
    Winding_t *back;
    int side;

    --i;
    side = ClipWinding(node->winding, crossPlanes[i], crossPlanes[i][3], 0.1f, &front, &back);
    if ( side == SIDE_BACK )
      continue;
    if ( side != SIDE_CROSS )
      return &node->next;

    FreeWinding(node->winding);
    node->winding = back;
    {
      WindingList_t *frontNode = AllocWindingListNode();
      frontNode->winding = front;
      frontNode->next = node;
      *link = frontNode;
      link = &frontNode->next;
    }
  }

  *link = node->next;
  FreeWinding(node->winding);
  FreeWindingListNode(node);
  return link;
}

/* CoD4 0x408010. */
static void BrushSides_ClipOpaqueBrush(Brush_t *opaqueBrush, void *data)
{
  Brush_t *subject = (Brush_t *)data;
  int i;

  Assert(BrushSideTree_IsOpaqueBrush(opaqueBrush), s_assertDisable_BrushSides);
  if ( opaqueBrush == subject )
    return;
  if ( opaqueBrush->detail && (!subject->detail || subject->forceVisible) )
    return;

  for ( i = 0; i < subject->numSides; ++i )
  {
    BrushSide_t *side = &subject->sides[i];
    WindingList_t **link = (WindingList_t **)&side->visibleHull;
    float *plane = (float *)MAP_PLANE(side->planenum);

    while ( *link )
      link = BrushSides_SubtractOpaqueBrush(link, plane, opaqueBrush);
  }
}

/* CoD4 0x4086A0. */
static void BrushSides_ClipOpaqueCallback(Brush_t *opaqueBrush, void *data)
{
  BrushSideClipContext_t *context = (BrushSideClipContext_t *)data;
  WindingList_t **link;

  Assert(BrushSideTree_IsOpaqueBrush(opaqueBrush), s_assertDisable_BrushSides);
  if ( context->source
    && opaqueBrush->entityNum == context->source->entityNum
    && opaqueBrush->brushNum == context->source->brushNum
    && opaqueBrush->mapInfoIndex == context->source->mapInfoIndex )
    return;

  for ( link = &context->windingList; *link; )
    link = BrushSides_SubtractOpaqueBrush(link, context->plane, opaqueBrush);
}

/* CoD4 0x4085A0. */
static WindingList_t *BrushSides_ClipWindingWithTree(Winding_t *winding,
                                                      const DrawSurf_t *source)
{
  BrushSideClipContext_t context;
  vec3_t mins;
  vec3_t maxs;
  int i;

  Assert(brushSideGlob.isTreeInitialized, s_assertDisable_BrushSides);
  context.windingList = AllocWindingListNode();
  context.windingList->winding = winding;
  context.source = source;
  if ( !brushSideGlob.hasOpaqueBrushes )
    return context.windingList;

  VectorCopy(winding->points[0], mins);
  VectorCopy(winding->points[0], maxs);
  for ( i = 0; i < winding->numpoints; ++i )
    AddPointToBounds(winding->points[i], mins, maxs);
  WindingHasPlane(winding, context.plane);
  BrushSideTree_ForBounds(brushSideGlob.nodes, mins, maxs, &context,
                           BrushSides_ClipOpaqueCallback);
  return context.windingList;
}

/* CoD4 0x408750. */
Winding_t *BrushSides_BuildVisibleHull(Winding_t *winding, const DrawSurf_t *source,
                                       const float *planeNormal)
{
  WindingList_t *list = BrushSides_ClipWindingWithTree(winding, source);
  Winding_t *result;

  if ( !list )
    return NULL;
  if ( list->next )
  {
    WindingList_t *node;
    result = WindingFromWindingList(planeNormal, list);
    while ( list )
    {
      node = list;
      list = list->next;
      FreeWinding(node->winding);
      FreeWindingListNode(node);
    }
  }
  else
  {
    result = list->winding;
    FreeWindingListNode(list);
  }
  return result;
}

/* CoD4 0x4087F0. */
qboolean BrushSides_IsWindingVisible(Winding_t *winding)
{
  WindingList_t *list = BrushSides_ClipWindingWithTree(winding, NULL);
  qboolean visible = list != NULL;

  while ( list )
  {
    WindingList_t *node = list;
    list = list->next;
    FreeWinding(node->winding);
    FreeWindingListNode(node);
  }
  return visible;
}

/* CoD4 0x407820. */
void BrushSides_InitTree(Entity_t *entities)
{
  AabbTreeBuilder_t builder;
  AabbTreeNode_t *builderNodes;
  float *mins;
  float *maxs;
  Brush_t *brush;
  int opaqueBrushCount = 0;
  int brushIndex = 0;
  int maxNodes;
  int nodeCount;
  int i;

  Assert(!brushSideGlob.isTreeInitialized, s_assertDisable_BrushSides);
  brushSideGlob.isTreeInitialized = 1;
  for ( brush = entities[0].brushes; brush; brush = brush->next )
    opaqueBrushCount += BrushSideTree_IsOpaqueBrush(brush);

  if ( !opaqueBrushCount )
  {
    brushSideGlob.hasOpaqueBrushes = 0;
    return;
  }

  brushSideGlob.opaqueBrushes = (Brush_t **)malloc(sizeof(*brushSideGlob.opaqueBrushes) * opaqueBrushCount);
  mins = (float *)malloc(sizeof(*mins) * 3 * opaqueBrushCount);
  maxs = (float *)malloc(sizeof(*maxs) * 3 * opaqueBrushCount);
  maxNodes = (opaqueBrushCount - 1) / 2 + 1;
  builderNodes = (AabbTreeNode_t *)malloc(sizeof(*builderNodes) * maxNodes);
  if ( !brushSideGlob.opaqueBrushes || !mins || !maxs || !builderNodes )
    Com_Error("Out of memory");

  for ( brush = entities[0].brushes; brush; brush = brush->next )
  {
    if ( BrushSideTree_IsOpaqueBrush(brush) )
    {
      brushSideGlob.opaqueBrushes[brushIndex] = brush;
      VectorCopy(brush->eMins, &mins[3 * brushIndex]);
      VectorCopy(brush->eMaxs, &maxs[3 * brushIndex]);
      ++brushIndex;
    }
  }
  Assert(brushIndex == opaqueBrushCount, s_assertDisable_BrushSides);

  builder.itemData = brushSideGlob.opaqueBrushes;
  builder.itemCount = opaqueBrushCount;
  builder.itemStride = sizeof(*brushSideGlob.opaqueBrushes);
  builder.hasBoundsData = NULL;
  builder.itemMins = mins;
  builder.itemMaxs = maxs;
  builder.nodes = builderNodes;
  builder.maxNodes = maxNodes;
  builder.minPartitionSize = 4;
  builder.minLeafItems = 8;
  nodeCount = AabbBuildTree(&builder);
  free(mins);
  free(maxs);

  brushSideGlob.nodes = (BrushSideTreeNode_t *)malloc(sizeof(*brushSideGlob.nodes) * nodeCount);
  if ( !brushSideGlob.nodes )
    Com_Error("Out of memory");
  for ( i = 0; i < nodeCount; ++i )
  {
    brushSideGlob.nodes[i].brushes = &brushSideGlob.opaqueBrushes[builderNodes[i].firstItem];
    brushSideGlob.nodes[i].brushCount = builderNodes[i].itemCount;
    brushSideGlob.nodes[i].childCount = builderNodes[i].childCount;
    brushSideGlob.nodes[i].children = builderNodes[i].childCount
      ? &brushSideGlob.nodes[builderNodes[i].firstChild] : NULL;
  }
  free(builderNodes);
  BrushSideTree_CalcBounds_r(brushSideGlob.nodes);
  brushSideGlob.hasOpaqueBrushes = 1;
}

/* CoD4 0x407D70. */
static void BrushSides_CopyWindings(Entity_t *entity)
{
  Brush_t *brush;
  int sideIndex;

  for ( brush = entity->brushes; brush; brush = brush->next )
  {
    for ( sideIndex = 0; sideIndex < brush->numSides; ++sideIndex )
    {
      BrushSide_t *side = &brush->sides[sideIndex];
      side->visibleHull = side->winding ? CopyWinding(side->winding) : NULL;
    }
  }
}

/* CoD4 0x407E70. */
static void BrushSides_ClipVisibleHulls(Brush_t *brush)
{
  int sideIndex;

  for ( sideIndex = 0; sideIndex < brush->numSides; ++sideIndex )
  {
    BrushSide_t *side = &brush->sides[sideIndex];
    WindingList_t *node;

    if ( side->winding )
    {
      node = AllocWindingListNode();
      node->winding = CopyWinding(side->winding);
      side->visibleHull = (Winding_t *)node;
    }
    else
    {
      side->visibleHull = NULL;
    }
  }

  BrushSideTree_ForBounds(brushSideGlob.nodes, s_nativeVisibleHullQueryPoint,
                          s_nativeVisibleHullQueryPoint, brush,
                          BrushSides_ClipOpaqueBrush);

  for ( sideIndex = 0; sideIndex < brush->numSides; ++sideIndex )
  {
    BrushSide_t *side = &brush->sides[sideIndex];
    WindingList_t *list = (WindingList_t *)side->visibleHull;

    if ( !list )
      continue;
    if ( !list->next )
    {
      side->visibleHull = list->winding;
      FreeWindingListNode(list);
      continue;
    }

    side->visibleHull = WindingFromWindingList(MAP_PLANE(side->planenum)->normal, list);
    while ( list )
    {
      WindingList_t *node = list;
      list = list->next;
      FreeWinding(node->winding);
      FreeWindingListNode(node);
    }
  }
}

/* CoD4 0x407DF0. */
void BrushSides_BuildVisibleHulls(Entity_t *entities)
{
  Brush_t *brush;

  Assert(brushSideGlob.isTreeInitialized, s_assertDisable_BrushSides);
  if ( !brushSideGlob.hasOpaqueBrushes )
  {
    BrushSides_CopyWindings(&entities[0]);
    return;
  }
  for ( brush = entities[0].brushes; brush; brush = brush->next )
    BrushSides_ClipVisibleHulls(brush);
}

/* CoD4 0x408510.  Visible hull generation deliberately precedes BSP
   portal/detail processing; this emits one native transient draw surface for
   every side which retained a hull. */
void BrushSides_EmitDrawSurfs(Entity_t *entities)
{
  Brush_t *brush;
  int sideIndex;

  for ( brush = entities->brushes; brush; brush = brush->next )
  {
    for ( sideIndex = 0; sideIndex < brush->numSides; ++sideIndex )
    {
      BrushSide_t *side = &brush->sides[sideIndex];

      if ( side->visibleHull )
        DrawSurfaceForSide(brush, side, side->visibleHull);
    }
  }
}

/* CoD4 0x407CF0. */
void BrushSides_ShutdownTree(void)
{
  Assert(brushSideGlob.isTreeInitialized, s_assertDisable_BrushSides);
  brushSideGlob.isTreeInitialized = 0;
  if ( brushSideGlob.hasOpaqueBrushes )
  {
    free(brushSideGlob.opaqueBrushes);
    free(brushSideGlob.nodes);
  }
}
