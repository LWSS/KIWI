/* CoD4-only reflection-probe BSP state recovered from cod4map.exe. */

#include "cod4map.h"

#define MAX_MAP_REFLECTION_PROBES 255

DiskGfxReflectionProbe_t bspReflectionProbes[MAX_MAP_REFLECTION_PROBES];
int numBspReflectionProbes;

static DiskGfxReflectionProbe_t *savedReflectionProbes;
static int savedReflectionProbeCount;
static char reflectionProbeColorCorrection[64];
/* cod4map.exe stores this as a one-byte BSS flag at 0x123A9494. */
static unsigned char reflectionProbeIgnorePortals;

/*
 * This half of map_reflection_probe.cpp operates solely on the recovered
 * CoD4 v22 DiskGfxCell layout.  The later per-tri-surface assignment still
 * needs the (currently unrecovered) 0x44-byte compiler tri-surface record.
 */
static float ReflectionProbe_TriangleArea(const float *p0, const float *p1, const float *p2)
{
  float edge0[3];
  float edge1[3];
  float cross[3];

  VectorSubtract((float *)p1, (float *)p0, edge0);
  VectorSubtract((float *)p2, (float *)p0, edge1);
  CrossProduct(edge0, edge1, cross);
  return (float)(0.5 * sqrt(DotProduct(cross, cross)));
}

static float ReflectionProbe_WindingArea(const Winding_t *winding)
{
  float area = 0.0f;
  int pointIndex;

  if ( winding->numpoints < 3 )
    Com_Error("ReflectionProbe_WindingArea: winding->ptCount >= 3");

  for ( pointIndex = winding->numpoints - 1; pointIndex >= 2; --pointIndex )
  {
    area += ReflectionProbe_TriangleArea(
        winding->points[0],
        winding->points[pointIndex - 1],
        winding->points[pointIndex]);
  }
  return area;
}

static Node_t *ReflectionProbe_NodeForPoint(Node_t *node, const float *point)
{
  Node_t *current = node;

  while ( current->planenum != PLANENUM_LEAF )
  {
    Plane_t *plane = MAP_PLANE(current->planenum);
    current = current->children[DotProduct((float *)point, plane->normal) - plane->dist < 0.0f];
  }
  return current;
}

/* Native 0x43C1F0: when selecting a cell for a draw surface, prefer a
   non-opaque child over the geometric plane test.  This keeps a surface that
   lies directly on a cell split from being assigned to solid space. */
static Node_t *ReflectionProbe_NodeForPointNonOpaque(Node_t *node, const float *point)
{
  Node_t *current = node;

  while ( current->planenum != PLANENUM_LEAF )
  {
    if ( current->children[0]->opaque )
      current = current->children[1];
    else if ( current->children[1]->opaque )
      current = current->children[0];
    else
    {
      Plane_t *plane = MAP_PLANE(current->planenum);
      current = current->children[DotProduct((float *)point, plane->normal) - plane->dist < 0.0f];
    }
  }
  return current;
}

static void ReflectionProbe_AddToCell(int probeIndex, int cellIndex)
{
  BspCell_t *cell;

  if ( cellIndex < 0 || cellIndex >= numBSPCells )
    Com_Error("ReflectionProbe_AddToCell: cell index %i is outside [0, %i)", cellIndex, numBSPCells);
  if ( probeIndex < 0 || probeIndex >= numBspReflectionProbes )
    Com_Error("ReflectionProbe_AddToCell: probe index %i is outside [0, %i)", probeIndex, numBspReflectionProbes);

  cell = &bspCells[cellIndex];
  if ( cell->reflectionProbeCount == 64 )
  {
    const float *origin = bspReflectionProbes[probeIndex].origin;
    Com_Error(
        "ERROR: Cell %d has too many reflection probes %d.  Not adding probe at (%f %f %f) to cell.\n",
        cellIndex,
        64,
        origin[0],
        origin[1],
        origin[2]);
  }

  /* The on-disk list reserves zero for the default reflection probe. */
  cell->reflectionProbes[cell->reflectionProbeCount++] = (unsigned char)(probeIndex + 1);
}

static void ReflectionProbe_ClearCellLists(void)
{
  int cellIndex;

  for ( cellIndex = 0; cellIndex < numBSPCells; ++cellIndex )
    bspCells[cellIndex].reflectionProbeCount = 0;
}

static void ReflectionProbe_CopyCellList(int destinationCell, int sourceCell)
{
  BspCell_t *destination;
  const BspCell_t *source;

  if ( destinationCell == sourceCell )
    Com_Error("ReflectionProbe_CopyCellList: destination must differ from source");
  if ( destinationCell < 0 || destinationCell >= numBSPCells
    || sourceCell < 0 || sourceCell >= numBSPCells )
    Com_Error("ReflectionProbe_CopyCellList: cell index outside [0, %i)", numBSPCells);

  destination = &bspCells[destinationCell];
  source = &bspCells[sourceCell];
  if ( destination->reflectionProbeCount )
    Com_Error("ReflectionProbe_CopyCellList: destination reflectionProbeCount must be zero");
  if ( !source->reflectionProbeCount )
    Com_Error("ReflectionProbe_CopyCellList: source reflectionProbeCount must be positive");

  destination->reflectionProbeCount = source->reflectionProbeCount;
  memcpy(destination->reflectionProbes, source->reflectionProbes, source->reflectionProbeCount);
}

static void ReflectionProbe_AccumulatePortalAreas_r(
    int cellTestIndex,
    float *cellAreas,
    Node_t *node)
{
  Portal_t *portal;
  int side;

  if ( node->planenum != PLANENUM_LEAF )
  {
    ReflectionProbe_AccumulatePortalAreas_r(cellTestIndex, cellAreas, node->children[0]);
    ReflectionProbe_AccumulatePortalAreas_r(cellTestIndex, cellAreas, node->children[1]);
    return;
  }
  if ( node->opaque || node->cellnum != cellTestIndex )
    return;

  for ( portal = node->portals; portal; portal = portal->next[side] )
  {
    Node_t *otherNode;
    int otherCell;

    side = portal->nodes[1] == node;
    if ( !Portal_EntityFlood(portal) )
      continue;
    otherNode = portal->nodes[!side];
    otherCell = otherNode->cellnum;
    if ( otherCell < 0 || otherCell >= numBSPCells )
      Com_Error("ReflectionProbe_AccumulatePortalAreas_r: other cell %i outside [0, %i)", otherCell, numBSPCells);
    if ( otherCell != cellTestIndex && bspCells[otherCell].reflectionProbeCount )
      cellAreas[otherCell] += ReflectionProbe_WindingArea(portal->winding);
  }
}

static int ReflectionProbe_FindBestSourceCell(int cellTestIndex, Node_t *treeHead)
{
  float cellAreas[MAX_MAP_CELLS];
  float bestArea = 0.0f;
  int bestCell = -1;
  int cellIndex;

  if ( numBSPCells >= MAX_MAP_CELLS )
    Com_Error("ReflectionProbe_FindBestSourceCell: numCells < MAX_MAP_CELLS");

  memset(cellAreas, 0, sizeof(cellAreas));
  ReflectionProbe_AccumulatePortalAreas_r(cellTestIndex, cellAreas, treeHead);
  for ( cellIndex = 0; cellIndex < numBSPCells; ++cellIndex )
  {
    if ( bestArea < cellAreas[cellIndex] )
    {
      bestArea = cellAreas[cellIndex];
      bestCell = cellIndex;
    }
  }
  return bestCell;
}

static void ReflectionProbe_PropagateCellLists(Node_t *treeHead)
{
  int emptyCount;
  int copiedCount;
  int cellIndex;

  do
  {
    emptyCount = 0;
    copiedCount = 0;
    for ( cellIndex = 0; cellIndex < numBSPCells; ++cellIndex )
    {
      int sourceCell;

      if ( bspCells[cellIndex].reflectionProbeCount )
        continue;
      ++emptyCount;
      sourceCell = ReflectionProbe_FindBestSourceCell(cellIndex, treeHead);
      if ( sourceCell != -1 )
      {
        ReflectionProbe_CopyCellList(cellIndex, sourceCell);
        ++copiedCount;
      }
    }
  } while ( emptyCount && copiedCount );

  /* CoD4 gives cells that cannot reach a probe the default probe (index zero). */
  if ( emptyCount )
  {
    for ( cellIndex = 0; cellIndex < numBSPCells; ++cellIndex )
    {
      if ( !bspCells[cellIndex].reflectionProbeCount )
      {
        bspCells[cellIndex].reflectionProbeCount = 1;
        bspCells[cellIndex].reflectionProbes[0] = 0;
      }
    }
  }
}

void AssignReflectionProbesToCells(Tree_t *tree)
{
  int probeIndex;

  if ( !tree || !tree->headnode )
    Com_Error("AssignReflectionProbesToCells: tree and tree->headnode are required");

  ReflectionProbe_ClearCellLists();
  for ( probeIndex = 0; probeIndex < numBspReflectionProbes; ++probeIndex )
  {
    Node_t *node = ReflectionProbe_NodeForPoint(tree->headnode, bspReflectionProbes[probeIndex].origin);

    if ( node->opaque )
    {
      const float *origin = bspReflectionProbes[probeIndex].origin;
      Com_Error("ERROR: Reflection probe at (%.1f %.1f %.1f) is in solid.\n", origin[0], origin[1], origin[2]);
    }
    ReflectionProbe_AddToCell(probeIndex, node->cellnum);
  }
  if ( numBspReflectionProbes )
    ReflectionProbe_PropagateCellLists(tree->headnode);
}

/* CoD4 0x41E360. */
static float ReflectionProbe_AverageDistanceSq(
    const DiskGfxReflectionProbe_t *probe,
    const MeshVert_t *verts,
    int vertCount)
{
  float distanceSum = 0.0f;
  int vertIndex;

  if ( vertCount <= 0 )
    Com_Error("ReflectionProbe_AverageDistanceSq: vertCount > 0");

  for ( vertIndex = 0; vertIndex < vertCount; ++vertIndex )
  {
    float delta[3];

    VectorSubtract(verts[vertIndex].pos, (float *)probe->origin, delta);
    distanceSum += DotProduct(delta, delta);
  }
  return distanceSum / (float)vertCount;
}

/* CoD4 0x41E400. */
static int ReflectionProbe_FindCellForCentroid(
    Tree_t *tree,
    const MeshVert_t *verts,
    int vertCount)
{
  float centroid[3] = { 0.0f, 0.0f, 0.0f };
  Node_t *node;
  int vertIndex;

  if ( vertCount <= 0 )
    Com_Error("ReflectionProbe_FindCellForCentroid: vertCount > 0");

  for ( vertIndex = 0; vertIndex < vertCount; ++vertIndex )
  {
    centroid[0] += verts[vertIndex].pos[0];
    centroid[1] += verts[vertIndex].pos[1];
    centroid[2] += verts[vertIndex].pos[2];
  }
  centroid[0] = centroid[0] / (float)vertCount + verts[0].normal[0];
  centroid[1] = centroid[1] / (float)vertCount + verts[0].normal[1];
  centroid[2] = centroid[2] / (float)vertCount + verts[0].normal[2];

  node = ReflectionProbe_NodeForPointNonOpaque(tree->headnode, centroid);
  if ( !node )
    Com_Error("ReflectionProbe_FindCellForCentroid: node");
  /* Native 0x41E400 treats its -1 leaf sentinel as "no assigned cell" and
     falls back to cell zero.  KIWI's shared portal tree retains both donor
     negative sentinels (-1 opaque and -2 unknown), and 0x43C1F0 only promises
     a non-opaque leaf; it does not promise that the portal partitioning has
     assigned that leaf a cell.  A cell index is usable only when nonnegative. */
  if ( node->cellnum >= 0 )
    return node->cellnum;
  if ( numBSPCells <= 0 )
    Com_Error("ReflectionProbe_FindCellForCentroid: numCells > 0");
  return 0;
}

/* CoD4 0x41E560. */
static int ReflectionProbe_FindModalVertexCell(
    Tree_t *tree,
    const MeshVert_t *verts,
    int vertCount)
{
  int cellCounts[MAX_MAP_CELLS];
  int bestCell = 0;
  int pointIndex;
  int cellIndex;

  memset(cellCounts, 0, sizeof(cellCounts));
  for ( pointIndex = 0; pointIndex < vertCount; ++pointIndex )
  {
    Node_t *node = ReflectionProbe_NodeForPointNonOpaque(tree->headnode, verts[pointIndex].pos);

    if ( !node )
      Com_Error("ReflectionProbe_FindModalVertexCell: node");
    /* 0x41E560 does not vote an unassigned leaf.  Test the usable range
       rather than the donor CELLNUM_UNKNOWN value: a non-opaque leaf can
       still carry -1 in the native layout, while KIWI can retain -2. */
    if ( node->cellnum >= 0 )
    {
      if ( node->cellnum >= MAX_MAP_CELLS )
        Com_Error("ReflectionProbe_FindModalVertexCell: node->cellnum doesn't index MAX_MAP_CELLS");
      ++cellCounts[node->cellnum];
    }
  }
  if ( numBSPCells <= 0 )
    Com_Error("ReflectionProbe_FindModalVertexCell: numCells > 0");
  for ( cellIndex = 1; cellIndex < numBSPCells; ++cellIndex )
  {
    if ( cellCounts[bestCell] < cellCounts[cellIndex] )
      bestCell = cellIndex;
  }
  return bestCell;
}

/* CoD4 0x41E6B0. */
static unsigned char ReflectionProbe_FindNearest(const MeshVert_t *verts, int vertCount)
{
  float bestDistance = FLT_MAX;
  unsigned char bestProbe = 0;
  int probeIndex;

  for ( probeIndex = 0; probeIndex < numBspReflectionProbes; ++probeIndex )
  {
    float distance = ReflectionProbe_AverageDistanceSq(
        &bspReflectionProbes[probeIndex], verts, vertCount);

    if ( bestDistance > distance )
    {
      bestDistance = distance;
      bestProbe = (unsigned char)(probeIndex + 1);
    }
  }
  if ( !bestProbe )
    Com_Error("ReflectionProbe_FindNearest: bestProbe != REFLECTION_PROBE_NONE");
  return bestProbe;
}

/* CoD4 0x41E190. */
static unsigned char FindReflectionProbeForTriSurface(
    Tree_t *tree,
    const MeshVert_t *verts,
    int vertCount,
    int useCentroid)
{
  BspCell_t *cell;
  float bestDistance;
  unsigned char bestProbe;
  int cellIndex;
  int listIndex;

  if ( vertCount <= 0 )
    Com_Error("FindReflectionProbeForTriSurface: vertCount > 0");
  if ( !numBspReflectionProbes )
    return 0;
  if ( reflectionProbeIgnorePortals )
    return ReflectionProbe_FindNearest(verts, vertCount);

  cellIndex = useCentroid
      ? ReflectionProbe_FindCellForCentroid(tree, verts, vertCount)
      : ReflectionProbe_FindModalVertexCell(tree, verts, vertCount);
  if ( cellIndex < 0 || cellIndex >= numBSPCells )
    Com_Error("FindReflectionProbeForTriSurface: selected cell index is outside BSP cells");

  cell = &bspCells[cellIndex];
  if ( !cell->reflectionProbeCount )
    Com_Error("FindReflectionProbeForTriSurface: cell->reflectionProbeCount > 0");

  bestDistance = FLT_MAX;
  bestProbe = 255;
  for ( listIndex = 0; listIndex < cell->reflectionProbeCount; ++listIndex )
  {
    unsigned char probeIndex = cell->reflectionProbes[listIndex];
    float distance;

    /* 0x41DB50 deliberately fills unreachable cells with the singleton
       list { 0 }.  Zero is the reserved default probe, not an absent list
       element.  The native 0x41E190 computes its address as
       bspReflectionProbes + (probeIndex - 1) * 0x20044; its BSS carries a
       sentinel record immediately before the first real probe.  KIWI stores
       only the on-disk real records, so represent that same singleton
       default directly instead of skipping it and reaching the INVALID
       assertion below.  Propagation never mixes this fallback with a real
       probe list. */
    if ( !probeIndex )
      return 0;
    distance = ReflectionProbe_AverageDistanceSq(
        &bspReflectionProbes[probeIndex - 1], verts, vertCount);
    if ( bestDistance > distance )
    {
      bestDistance = distance;
      bestProbe = probeIndex;
    }
  }
  if ( bestProbe == 255 )
    Com_Error("FindReflectionProbeForTriSurface: bestProbe != REFLECTION_PROBE_INVALID");
  return bestProbe;
}

/* CoD4 0x41E0F0.  This pass must run before EmitDrawSurfaces: the native
   compiler selects from the original 44-byte MapDrawSurf vertex stream while
   reflection-probe cell lists still occupy BspCell +44.  Later BSP emission
   reuses that compiler-only storage for occluder bookkeeping. */
void AssignReflectionProbesToTriSurfaces(Tree_t *tree, int firstDrawSurf)
{
  int drawSurfIndex;

  for ( drawSurfIndex = firstDrawSurf; drawSurfIndex < numMapDrawSurfs; ++drawSurfIndex )
  {
    MapDrawSurf_t *drawSurf = &g_nativeMapDrawSurfs[drawSurfIndex];

    /* CoD4 0x41E760: mtlTex->gameFlags & 0x10. */
    if ( drawSurf->material->gameFlags & 0x10 )
    {
      drawSurf->reflectionProbeIndex = FindReflectionProbeForTriSurface(
          tree,
          drawSurf->verts,
          drawSurf->vertCount,
          !(drawSurf->isPatch || drawSurf->isTerrain));
    }
    else
    {
      drawSurf->reflectionProbeIndex = 0;
    }
  }
}

static unsigned char ReflectionProbe_RandomByte(void)
{
  return (unsigned char)((double)rand() / 32767.0 * 255.0);
}

static void ReflectionProbe_SetDebugColor(DiskGfxReflectionProbe_t *probe)
{
  unsigned char blue;
  unsigned int color;
  unsigned char green;
  unsigned int *pixel;
  int pixelIndex;
  unsigned char red;

  blue = ReflectionProbe_RandomByte();
  red = ReflectionProbe_RandomByte();
  green = ReflectionProbe_RandomByte();
  color = ((unsigned int)blue << 24)
        | ((unsigned int)red << 16)
        | ((unsigned int)green << 8)
        | 0xff;
  pixel = (unsigned int *)probe->pixels;
  for ( pixelIndex = 0; pixelIndex < 32766; ++pixelIndex )
    pixel[pixelIndex] = color;
}

static void ReflectionProbe_ClearPixels(DiskGfxReflectionProbe_t *probe, unsigned char value)
{
  memset(probe->pixels, value, sizeof(probe->pixels));
}

static void ReflectionProbe_CopyPixels(
    DiskGfxReflectionProbe_t *probe,
    const DiskGfxReflectionProbe_t *savedProbe)
{
  memcpy(probe->pixels, savedProbe->pixels, sizeof(probe->pixels));
}

static DiskGfxReflectionProbe_t *FindSavedReflectionProbe(const float *origin)
{
  int probeIndex;

  for ( probeIndex = 0; probeIndex < savedReflectionProbeCount; ++probeIndex )
  {
    if ( VectorCompare((float *)origin, savedReflectionProbes[probeIndex].origin) )
      return &savedReflectionProbes[probeIndex];
  }
  return NULL;
}

void SaveExistingReflectionProbes(void)
{
  const size_t size = sizeof(*savedReflectionProbes) * numBspReflectionProbes;

  savedReflectionProbeCount = numBspReflectionProbes;
  savedReflectionProbes = (DiskGfxReflectionProbe_t *)malloc(size);
  memcpy(savedReflectionProbes, bspReflectionProbes, size);
  numBspReflectionProbes = 0;
}

void FreeSavedReflectionProbes(void)
{
  free(savedReflectionProbes);
  savedReflectionProbes = NULL;
  savedReflectionProbeCount = 0;
}

void SetReflectionProbeColorCorrection(Entity_t *worldspawn)
{
  const char *name;
  unsigned int probeIndex;

  name = ValueForKey(worldspawn, "reflection_color_correction");
  if ( strlen(name) >= sizeof(reflectionProbeColorCorrection) )
    Com_Error("ERROR: color_correction name '%s' exceed %d characters.\n", name, 64);
  if ( *name )
    I_strncpyz(reflectionProbeColorCorrection, name, sizeof(reflectionProbeColorCorrection));

  for ( probeIndex = 0; probeIndex < (unsigned int)numBspReflectionProbes; ++probeIndex )
  {
    if ( !bspReflectionProbes[probeIndex].colorCorrectionFilename[0] )
    {
      I_strncpyz(
          bspReflectionProbes[probeIndex].colorCorrectionFilename,
          reflectionProbeColorCorrection,
          sizeof(bspReflectionProbes[probeIndex].colorCorrectionFilename));
    }
  }
}

void SetReflectionProbeIgnorePortals(Entity_t *worldspawn)
{
  const char *value;

  value = ValueForKey(worldspawn, "reflection_ignore_portals");
  if ( *value && *value != '0' )
    reflectionProbeIgnorePortals = 1;
}

void AddReflectionProbe(Entity_t *entity)
{
  DiskGfxReflectionProbe_t *probe;
  DiskGfxReflectionProbe_t *savedProbe;
  const char *colorCorrection;

  if ( numBspReflectionProbes == MAX_MAP_REFLECTION_PROBES )
    Com_Error("ERROR: More than %i reflection probes\n", MAX_MAP_REFLECTION_PROBES);

  probe = &bspReflectionProbes[numBspReflectionProbes++];
  GetVectorForKey(entity, "origin", probe->origin);

  colorCorrection = ValueForKey(entity, "color_correction");
  if ( strlen(colorCorrection) >= sizeof(probe->colorCorrectionFilename) )
    Com_Error("ERROR: color_correction name '%s' exceed %d characters.\n", colorCorrection, 64);
  if ( *colorCorrection )
    I_strncpyz(probe->colorCorrectionFilename, colorCorrection, sizeof(probe->colorCorrectionFilename));
  else
    I_strncpyz(probe->colorCorrectionFilename, reflectionProbeColorCorrection, sizeof(probe->colorCorrectionFilename));

  savedProbe = FindSavedReflectionProbe(probe->origin);
  if ( reflectionDebug )
    ReflectionProbe_SetDebugColor(probe);
  else if ( savedProbe )
    ReflectionProbe_CopyPixels(probe, savedProbe);
  else
    ReflectionProbe_ClearPixels(probe, 0);
}
