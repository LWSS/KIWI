/*
brush.c — Brush operations

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

int *g_brushPoints = NULL;
int  useOptimizedSplit;

char g_assertArrayIndex;
char g_assertBrushNeighbor_0;
char g_assertBrushNeighbor_1;
char g_assertBrushNeighbor_2;
char g_assertBrushNeighbor_3;
char g_assertBrushNeighbor_4;
char g_assertBrushNeighbor_5;
char g_assertBrushNeighbor_6;
char g_assertBrushNeighbor_7;
char g_assertBrushNeighbor_8;
char g_assertBrush_0;
char g_assertBrush_1;
char g_assertBrush_2;
char g_assertBrush_3;
char g_assertBrush_5;
char g_assertBrush_9;
char g_assertBrush_10;
char g_assertBrush_11;
char g_assertBrush_12;
char g_assertBrush_15;
char g_assertBrush_16;
char g_assertBrush_17;
char g_assertBrush_18;
char g_assertBrush_19;
char g_assertBrush_20;
char g_assertBrush_21;
char g_assertBrush_22;
char g_assertBrush_23;
char g_assertBrush_24;
char g_assertCapsuleHeight;
char g_assertCapsuleRadius;
char g_assertWindingNull;
char g_assertWindingZero;


/*
================
CountBrushList

Counts the number of brushes in a linked list.
================
*/
int CountBrushList(Brush_t *brushes)
{
  int c;

  c = 0;
  for ( ; brushes; brushes = brushes->next )
  {
    if ( !brushes->isOccluder && !brushes->original->isOccluder )
      ++c;
  }
  return c;
}

/* CoD4 0x4054E0. */
void FreeBrushAdjacencyWinding(BrushAdjacencyWinding_t *winding)
{
  if ( winding->numsides == (int)0xDEADDEAD )
    Com_Error("FreeWinding: freed a freed winding");
  winding->numsides = (int)0xDEADDEAD;
  free(winding);
}

/*
================
AllocBrush

Allocates a new brush with the given number of sides.
================
*/
Brush_t *AllocBrush(int numSides)
{
  Brush_t *brush;
  int size;

  size = sizeof(Brush_t) + sizeof(BrushSide_t) * numSides;
  brush = malloc(size);
  memset(brush, 0, size);
  brush->outputNum = -1;
  return brush;
}

/* CoD4 0x405520: adjacency winding copies contain one count followed by the
   corresponding side indices. */
static BrushAdjacencyWinding_t *CopyBrushAdjacencyWinding(const BrushAdjacencyWinding_t *source)
{
  BrushAdjacencyWinding_t *copy;
  size_t size;

  Assert(source != NULL, g_assertBrush_0);
  Assert(source->numsides >= 0, g_assertBrush_0);
  size = sizeof(*copy) + sizeof(copy->sides[0]) * source->numsides;
  copy = (BrushAdjacencyWinding_t *)malloc(size);
  if ( !copy )
    Com_Error("Out of memory");
  memcpy(copy, source, size);
  return copy;
}

/*
================
FreeBrush

Frees a single brush and all its side windings.
If collision sides were separately allocated, frees those too.
================
*/
Brush_t *FreeBrushContents(Brush_t *brush)
{
  int i;

  /* Native 0x402630 frees the inline sides, then the separately allocated
     collision-side array itself. */
  for ( i = 0; i < brush->numSides; i++ )
  {
    if ( brush->sides[i].winding )
    {
      FreeWinding(brush->sides[i].winding);
      brush->sides[i].winding = NULL;
    }
    if ( brush->sides[i].adjacencyWinding )
    {
      FreeBrushAdjacencyWinding(brush->sides[i].adjacencyWinding);
      brush->sides[i].adjacencyWinding = NULL;
    }
  }

  /* free collision sides if separately allocated */
  if ( brush->collisionSides != brush->sides )
  {
    free(brush->collisionSides);
    brush->collisionSides = NULL;
  }

  return brush;
}

void FreeBrush(Brush_t *brush)
{
  FreeBrushContents(brush);
  free(brush);
}

int BrushPointInsideNonBevelPlanes(const Brush_t *brush, const float *point)
{
  int sideIndex;

  for ( sideIndex = 0; sideIndex < brush->numSides; ++sideIndex )
  {
    const BrushSide_t *side = &brush->sides[sideIndex];
    const Plane_t *plane;

    if ( side->bevel )
      continue;
    plane = MAP_PLANE(side->planenum);
    if ( DotProduct(point, plane->normal) - plane->dist > 0.1f )
      return 0;
  }
  return 1;
}

/*
================
FreeBrushList

Frees a linked list of brushes.
================
*/
void FreeBrushList(Brush_t *brushes)
{
  Brush_t *next;

  for ( ; brushes; brushes = next )
  {
    next = brushes->next;
    FreeBrush(brushes);
  }
}

/*
================
CopyBrush

Creates a deep copy of a brush structure including all side windings.
================
*/
Brush_t *CopyBrush(Brush_t *srcBrush)
{
  size_t brushSize;
  Brush_t *destBrush;
  int i;

  Assert(srcBrush->numCollisionSides == 0, g_assertBrush_0);
  Assert(srcBrush->collisionSides == 0, g_assertBrush_1);

  /* copy brush */
  brushSize = sizeof(Brush_t) + sizeof(BrushSide_t) * srcBrush->numSides;
  destBrush = AllocBrush(srcBrush->numSides);
  memcpy(destBrush, srcBrush, brushSize);

  /* copy side windings */
  for ( i = 0; i < srcBrush->numSides; i++ )
  {
    if ( srcBrush->sides[i].winding )
      destBrush->sides[i].winding = CopyWinding(srcBrush->sides[i].winding);
    if ( srcBrush->sides[i].adjacencyWinding )
      destBrush->sides[i].adjacencyWinding = CopyBrushAdjacencyWinding(srcBrush->sides[i].adjacencyWinding);
  }

  return destBrush;
}

/*
================
CopyCollisionBrush

Creates a deep copy of a brush's collision data (collision sides + windings).
================
*/
Brush_t *CopyCollisionBrush(Brush_t *srcBrush)
{
  Brush_t *destBrush;
  BrushSide_t *srcSides;
  BrushSide_t *destSides;
  int i;

  Assert(srcBrush->numCollisionSides != 0, g_assertBrush_2);
  Assert(srcBrush->collisionSides != 0, g_assertBrush_3);

  /* copy brush header */
  destBrush = AllocBrush(srcBrush->numCollisionSides);
  memcpy(destBrush, srcBrush, sizeof(Brush_t));

  /* copy collision sides */
  srcSides = srcBrush->collisionSides;
  destSides = destBrush->sides;
  for ( i = 0; i < srcBrush->numCollisionSides; ++i )
  {
    memcpy(&destSides[i], &srcSides[i], sizeof(BrushSide_t));
    /* deep copy winding */
    if ( srcSides[i].winding )
    {
      destSides[i].winding = CopyWinding(srcSides[i].winding);
    }
    if ( srcBrush->sides[i].adjacencyWinding )
      destSides[i].adjacencyWinding = CopyBrushAdjacencyWinding(srcBrush->sides[i].adjacencyWinding);
  }
  destBrush->numSides = srcBrush->numCollisionSides;
  return destBrush;
}

/*
================
BoundBrush

Sets the bounding box of a brush from all winding points.
Returns 1 if valid bounds, 0 if degenerate.
================
*/
int BoundBrush(Brush_t *brush)
{
  int i, j;
  Winding_t *w;

  /* set bounds from winding points */
  ClearBounds(brush->eMins, brush->eMaxs);
  for ( i = 0; i < brush->numSides; i++ )
  {
    w = brush->sides[i].winding;
    if ( !w )
      continue;
    for ( j = 0; j < w->numpoints; j++ )
      AddPointToBounds(w->points[j], brush->eMins, brush->eMaxs);
  }

  /* check for degenerate volume */
  for ( i = 0; i < 3; i++ )
  {
    if ( brush->eMins[i] < MIN_WORLD_COORD || brush->eMaxs[i] > MAX_WORLD_COORD || brush->eMins[i] >= brush->eMaxs[i] )
      return 0;
  }
  return 1;
}

/*
================
ReplacePointInWinding

Replaces a point in a winding polygon based on axis comparison.
Used by AddPointToWinding when the new point lies on an edge.
================
*/
void ReplacePointInWinding(Winding_t *winding, float *point, int axisI, int axisJ, int index0, int index1)
{
  long double diffI;
  int axis;
  double val0;
  float *pt0;
  float *pt1;
  float diffJ;
  float chosenDiff;

  /* compute axis deltas */
  diffI = winding->points[index1][axisI] - winding->points[index0][axisI];
  diffJ = winding->points[index1][axisJ] - winding->points[index0][axisJ];

  Assert(winding->points[index0][axisI] != winding->points[index1][axisI]
      || winding->points[index0][axisJ] != winding->points[index1][axisJ], g_assertBrush_9);

  /* pick the axis with the larger delta */
  axis = axisJ;
  chosenDiff = (float)diffI;
  if ( fabs(diffJ) <= fabs(diffI) )
  {
    axis = axisI;
    axisJ = axisI;
  }
  else
  {
    chosenDiff = diffJ;
  }

  val0 = winding->points[index0][axis];
  pt0 = &winding->points[index0][axis];
  pt1 = &winding->points[index1][axis];

  /* decreasing order: index0 > index1 on chosen axis */
  if ( chosenDiff <= 0.0 )
  {
    Assert(val0 > *pt1, g_assertBrush_11);
    if ( point[axis] > *pt0 )
    {
      VectorCopy(point, winding->points[index0]);
    }
    else if ( point[axis] < *pt1 )
    {
      VectorCopy(point, winding->points[index1]);
    }
    return;
  }

  /* increasing order: index0 < index1 on chosen axis */
  Assert(val0 < *pt1, g_assertBrush_10);
  if ( point[axis] < *pt0 )
  {
    VectorCopy(point, winding->points[index0]);
    return;
  }
  if ( point[axis] > *pt1 )
  {
    VectorCopy(point, winding->points[index1]);
  }
}

/*
================
AddPointToWinding

Inserts a new point into a winding polygon in the correct sorted position.
Uses 2D signed area to find the best insertion edge.
================
*/
void AddPointToWinding(float *newPoint, Winding_t *winding, int sortAxisX, int sortAxisY)
{
  double signedArea;
  int bestIdx;
  float minSignedArea;
  int i;

  bestIdx = -1;
  minSignedArea = FLT_MAX;
  i = 0;

  if ( winding->numpoints <= 0 )
  {
    Assert(0, g_assertBrush_12);
  }
  else
  {
    /* find the edge with the smallest signed area */
    float newX = newPoint[sortAxisX];
    float newY = newPoint[sortAxisY];
    int numPts = winding->numpoints;
    int prevIdx = numPts - 1;

    do
    {
      float prevX = winding->points[prevIdx][sortAxisX];
      float prevY = winding->points[prevIdx][sortAxisY];
      float currX = winding->points[i][sortAxisX];
      float currY = winding->points[i][sortAxisY];
      signedArea = (double)(currY - newY) * prevX
                 + (double)(newY - prevY) * currX
                 + (double)(prevY - currY) * newX;

      if ( minSignedArea > signedArea )
      {
        minSignedArea = (float)signedArea;
        bestIdx = i;
      }

      prevIdx = i;
      ++i;
    }
    while ( i < numPts );

    if ( bestIdx < 0 )
    {
      Assert(0, g_assertBrush_12);
    }
  }

  if ( minSignedArea < -PLANESIDE_EPSILON )
  {
    /* point is outside -- insert at best edge */
    memcpy(winding->points[bestIdx + 1], winding->points[bestIdx], sizeof(vec3_t) * (winding->numpoints - bestIdx));
    VectorCopy(newPoint, winding->points[bestIdx]);
    ++winding->numpoints;
  }
  else if ( minSignedArea <= PLANESIDE_EPSILON )
  {
    /* point is on edge -- replace endpoint */
    ReplacePointInWinding(winding, newPoint, sortAxisX, sortAxisY, (winding->numpoints + bestIdx - 1) % winding->numpoints, bestIdx);
  }
}

/*
================
BrushVolume

Calculates the volume of a brush by summing tetrahedra.
================
*/

double BrushVolume(Brush_t *brush)
{
  int i;
  Winding_t *w;
  vec3_t corner;
  float d, area, volume;
  Plane_t *plane;

  if ( !brush )
    return 0.0;
  if ( brush->numSides <= 0 )
    return 0.0;

  /* grab the first valid point as the corner */
  w = NULL;
  for ( i = 0; i < brush->numSides; i++ )
  {
    w = brush->sides[i].winding;
    if ( w )
      break;
  }
  if ( !w )
    return 0.0;
  VectorCopy(w->points[0], corner);

  /* make tetrahedrons to all other faces */
  volume = 0;
  for ( ; i < brush->numSides; i++ )
  {
    w = brush->sides[i].winding;
    if ( !w )
      continue;
    plane = MAP_PLANE(brush->sides[i].planenum);
    d = -(DotProduct210(corner, plane->normal) - plane->dist);
    area = (float)WindingArea(w);
    volume = FMA1(volume, d, area);
  }

  volume /= 3;
  return volume;
}

/*
================
WriteBSPBrushMap

Writes a map file with the split BSP brushes.
================
*/

int WriteBSPBrushMap(char *name, Brush_t *list)
{
  FILE *f;
  BrushSide_t *s;
  int i;
  Winding_t *w;

  /* note it */
  Com_Printf("writing %s\n", name);

  /* open the map file */
  f = fopen(name, "wb");
  if ( !f )
    Com_Error("Can't write %s", name);

  fprintf(f, "{\n\"classname\" \"worldspawn\"\n");

  for ( ; list; list = list->next )
  {
    fprintf(f, "{\n");
    for ( i = 0, s = list->sides; i < list->numSides; i++, s++ )
    {
      w = s->winding;
      if ( !w )
        continue;
      fprintf(f, "( %.0f %.0f %.0f ) ", w->points[0][0], w->points[0][1], w->points[0][2]);
      fprintf(f, "( %.0f %.0f %.0f ) ", w->points[1][0], w->points[1][1], w->points[1][2]);
      fprintf(f, "( %.0f %.0f %.0f ) ", w->points[2][0], w->points[2][1], w->points[2][2]);
      fprintf(f, "%s 0 0 0 1 1\n", s->shaderInfo->name);
    }
    fprintf(f, "}\n");
  }
  fprintf(f, "}\n");

  return fclose(f);
}

/*
================
AllocTree

Allocates and initializes a BSP tree structure.
================
*/
Tree_t *AllocTree()
{
  Tree_t *tree;

  tree = malloc(sizeof(*tree));
  memset(tree, 0, sizeof(*tree));
  ClearBounds(tree->mins, tree->maxs);

  return tree;
}

/*
================
AllocNode

Allocates and zero-initializes a BSP node.
================
*/
Node_t *AllocNode()
{
  Node_t *node;

  node = malloc(sizeof(*node));
  memset(node, 0, sizeof(*node));

  return node;
}

/*
================
IsTinyWinding

Tests if a winding would be crunched out of existence by vertex snapping.
Returns 1 (true) if fewer than 3 edges have length > EDGE_LENGTH (0.2).
================
*/
int IsTinyWinding(Winding_t *w)
{
  int i, j;
  vec_t len;
  vec3_t delta;
  int edges;

  edges = 0;
  for ( i = 0; i < w->numpoints; i++ )
  {
    j = i == w->numpoints - 1 ? 0 : i + 1;
    VectorSubtract(w->points[j], w->points[i], delta);
    len = (vec_t)VectorLength(delta);
    if ( len > EDGE_LENGTH )
    {
      if ( ++edges == 3 )
        return 0;
    }
  }
  return 1;
}

/*
================
IsHugeWinding

Returns true if the winding still has one of the points
from basewinding for plane (outside MIN/MAX_WORLD_COORD).
================
*/
int IsHugeWinding(Winding_t *w)
{
  int i, j;

  for ( i = 0; i < w->numpoints; i++ )
  {
    for ( j = 0; j < 3; j++ )
      if ( w->points[i][j] <= MIN_WORLD_COORD || w->points[i][j] >= MAX_WORLD_COORD )
        return 1;
  }
  return 0;
}

/*
================
BrushMostlyOnSide

Determines which side of a plane a brush is mostly on.
Returns 1 (front) or 2 (back).
================
*/
int BrushMostlyOnSide(Brush_t *brush, volatile float *plane)
{
  int i, j;
  Winding_t *w;
  vec_t d, max;
  int side;

  max = 0;
  side = PSIDE_FRONT;
  for ( i = 0; i < brush->numSides; i++ )
  {
    w = brush->sides[i].winding;
    if ( !w )
      continue;
    for ( j = 0; j < w->numpoints; j++ )
    {
      d = DotProduct201(w->points[j], plane) - plane[3];
      if ( d > max )
      {
        max = d;
        side = PSIDE_FRONT;
      }
      if ( -d > max )
      {
        max = -d;
        side = PSIDE_BACK;
      }
    }
  }
  return side;
}

/* CoD4 0x403A50: discard tiny non-bevel faces and compact the side array. */
static int RemoveTinyBrushSides(Brush_t *brush)
{
  int oldSideCount = brush->numSides;
  int removedCount = 0;
  int writeIndex = 0;
  int i;

  for ( i = 0; i < oldSideCount; ++i )
  {
    BrushSide_t *side = &brush->sides[i];
    Winding_t *winding = side->winding;

    if ( (winding && !IsTinyWinding(winding)) || (++removedCount, side->bevel) )
    {
      if ( writeIndex != i )
        memcpy(&brush->sides[writeIndex], side, sizeof(*side));
      ++writeIndex;
    }
    else if ( winding )
    {
      FreeWinding(winding);
    }
  }

  brush->numSides = writeIndex;
  return oldSideCount - removedCount >= 4;
}

/* CoD4 0x403980: rebuild a brush after appending one clipping plane. */
static Brush_t *CreateBrushWithAddedPlane(Brush_t *source, int planeIndex)
{
  Brush_t *brush;
  size_t sourceSize;
  int oldSideCount;
  int i;

  oldSideCount = source->numSides;
  brush = AllocBrush(oldSideCount + 1);
  sourceSize = sizeof(*source) + sizeof(source->sides[0]) * oldSideCount;
  memcpy(brush, source, sourceSize);
  for ( i = 0; i < oldSideCount; ++i )
    brush->sides[i].winding = NULL;
  brush->numSides = oldSideCount + 1;
  brush->sides[oldSideCount].planenum = planeIndex ^ 1;

  if ( CreateBrushWindings(brush) && RemoveTinyBrushSides(brush) )
    return brush;

  FreeBrush(brush);
  return NULL;
}

/* CoD4 0x403B40. */
void *SplitBrushByPlane(Brush_t *brush, int planeIdx, Brush_t **frontBrush, Brush_t **backBrush)
{
  Plane_t *plane;
  float maxFront;
  float maxBack;
  int i;
  int j;

  *frontBrush = NULL;
  *backBrush = NULL;
  plane = MAP_PLANE(planeIdx);
  maxFront = 0.0f;
  maxBack = 0.0f;

  for ( i = 0; i < brush->numSides; ++i )
  {
    Winding_t *winding = brush->sides[i].winding;
    if ( !winding )
      continue;
    for ( j = 0; j < winding->numpoints; ++j )
    {
      float distance = DotProduct210(winding->points[j], plane->normal) - plane->dist;
      if ( distance > maxFront )
        maxFront = distance;
      if ( distance < maxBack )
        maxBack = distance;
    }
  }

  if ( maxFront < ON_EPSILON )
  {
    *backBrush = CopyBrush(brush);
    return *backBrush;
  }
  if ( maxBack > -ON_EPSILON )
  {
    *frontBrush = CopyBrush(brush);
    return *frontBrush;
  }

  *frontBrush = CreateBrushWithAddedPlane(brush, planeIdx);
  *backBrush = CreateBrushWithAddedPlane(brush, planeIdx ^ 1);
  return *backBrush;
}

/*
================
TestBrushAgainstPlane

Tests which side of a plane a brush lies on.
Returns: 0 = front, 1 = back, 2 = on-plane, 3 = spanning.
================
*/
int TestBrushAgainstPlane(Brush_t *brush, float *normal, float dist, float epsilon)
{
  int i, j;
  Winding_t *w;
  double d;
  int anyFront, anyBack;
  int allOnPlane;

  anyFront = 0;
  anyBack = 0;

  for ( i = 0; i < brush->numSides; i++ )
  {
    w = brush->sides[i].winding;
    if ( !w )
      continue;

    Assert(w->numpoints != 0, g_assertBrush_15);
    allOnPlane = 1;
    if ( w->numpoints <= 0 )
      return 2;

    /* classify each point against the plane */
    for ( j = 0; j < w->numpoints; j++ )
    {
      d = DotProduct201(w->points[j], normal) - dist;
      if ( d > epsilon )
      {
        if ( anyBack )
          return 3;
        anyFront = 1;
        allOnPlane = 0;
      }
      else if ( d < -epsilon )
      {
        if ( anyFront )
          return 3;
        anyBack = 1;
        allOnPlane = 0;
      }
    }
    if ( allOnPlane )
      return 2;
  }

  if ( anyFront || anyBack )
    return anyFront == 0;

  /* assert: at least one side should be front or back */
  AssertMsg(anyFront || anyBack, g_assertBrush_16, "%s\n\t(brush->sideCount) = %i", "(anyFront || anyBack)", brush->numSides);
  return anyFront == 0;
}

/*
================
CountPointsOnSides

Counts how many brush points lie on each side (total and solo).
totalPoints[i] = total points touching side i
soloPoints[i]  = points touching ONLY side i (no other sides)
================
*/
int CountPointsOnSides(int *totalPoints, int *soloPoints, int numSides, int numPoints)
{
  int *pointEntry;
  int sideIdx;
  int foundSolo;

  memset(soloPoints, 0, numSides * sizeof(int));
  memset(totalPoints, 0, numSides * sizeof(int));

  for (pointEntry = g_brushPoints + BRUSH_POINT_DATA_OFS; numPoints > 0; numPoints--, pointEntry += BRUSH_POINT_STRIDE)
  {
    if (!*pointEntry)  /* point doesn't touch any side */
      continue;

    /* count which sides this point touches */
    foundSolo = 0;
    for (sideIdx = 0; sideIdx < numSides; sideIdx++)
    {
      if (!(POINT_SIDE_WORD(pointEntry, sideIdx) & SIDE_BIT(sideIdx)))
        continue;
      ++totalPoints[sideIdx];
      if (*pointEntry == 1)  /* point touches exactly one side (solo) */
      {
        foundSolo = 1;
        break;
      }
    }
    if (foundSolo)
      ++soloPoints[sideIdx];
  }
  return 0;
}

/*
================
CullSideFromPoints

Removes a side from all point entries in the point buffer
and updates the solo/total point counts accordingly.
When a point's side count drops to 1, increment soloPoints for its remaining side.
When a point's side count drops to 0, decrement soloPoints for the culled side.
================
*/
void CullSideFromPoints(int numPoints, int culledSideIndex, int *soloPoints, int *totalPoints, int numSides)
{
  int sideBit;
  int *pointEntry;
  int bitmask;
  int remainingSide;

  sideBit = SIDE_BIT(culledSideIndex);

  for (pointEntry = g_brushPoints + BRUSH_POINT_DATA_OFS; numPoints > 0; numPoints--, pointEntry += BRUSH_POINT_STRIDE)
  {
    if (!*pointEntry)  /* point is inactive */
      continue;

    bitmask = POINT_SIDE_WORD(pointEntry, culledSideIndex);
    if (!(bitmask & sideBit))  /* point doesn't touch the culled side */
      continue;

    /* remove the culled side from this point */
    POINT_SIDE_WORD(pointEntry, culledSideIndex) = bitmask & ~sideBit;
    --*pointEntry;
    --totalPoints[culledSideIndex];

    if (!*pointEntry)
    {
      /* point no longer touches any side — was solo on culled side */
      --soloPoints[culledSideIndex];
      continue;
    }

    if (*pointEntry == 1)  /* point now touches exactly one side (became solo) */
    {
      /* find which side it now belongs to */
      for (remainingSide = 0; remainingSide < numSides; remainingSide++)
      {
        if (POINT_SIDE_WORD(pointEntry, remainingSide) & SIDE_BIT(remainingSide))
        {
          ++soloPoints[remainingSide];
          break;
        }
      }
    }
  }
  AssertFatal(soloPoints[culledSideIndex] == 0, g_assertBrush_18);
  AssertFatal(totalPoints[culledSideIndex] == 0, g_assertBrush_19);
}

/*
================
FindNextSoloSide

Finds the next side index (after startSide) that has solo points.
Returns -1 if no more sides with solo points exist.
================
*/
int FindNextSoloSide(int startSide, int *soloPoints, int numSides)
{
  int sideIdx;

  for (sideIdx = startSide + 1; sideIdx < numSides; sideIdx++)
  {
    if (soloPoints[sideIdx] < 0)
      Assert(0, g_assertBrush_20);
    if (soloPoints[sideIdx] > 0)
      return sideIdx;
  }
  return numSides;
}

/*
================
AddBevelSideForNeighbor

Checks if a neighbor brush side should be added as a bevel plane
to the reference brush. If the neighbor's plane clips some winding
points of the ref brush (but doesn't fully contain them), adds
the plane as a new collision side.
================
*/
void AddBevelSideForNeighbor(int neighborSideIdx, Brush_t *refBrush, Brush_t *neighborBrush)
{
  BrushSide_t *nSide;
  BrushSide_t *side;
  BrushSide_t *newSides;
  BrushSide_t *oldSides;
  BrushSide_t *bevelSide;
  Plane_t *plane;
  Winding_t *w;
  int planeIdx;
  int needsBevel;
  int onPlaneCount;
  int i, j;
  double d;

  if (refBrush->numCollisionSides == MAX_COLLISION_SIDES)
    return;

  /* get neighbor's side data */
  nSide = &neighborBrush->collisionSides[neighborSideIdx];
  planeIdx = nSide->planenum;
  plane = MAP_PLANE(planeIdx);

  /* 0x404F17 walks the collision-side array, not the original brush-side
     array.  The appended bevels have no winding, but they must still take
     part in the same/opposite-planenum rejection at 0x404F32; otherwise a
     later neighbour can append the same bevel again and break adjacency
     point-cycle topology. */
  needsBevel = 0;
  for (i = 0; i < refBrush->numCollisionSides; i++)
  {
    side = &refBrush->collisionSides[i];

    /* if this side uses the same or opposite plane, no bevel needed */
    if ((planeIdx ^ side->planenum) <= 1)
      return;

    w = side->winding;
    if (!w || w->numpoints <= 0)
      continue;

    /* test winding points against the plane */
    onPlaneCount = 0;
    for (j = 0; j < w->numpoints; j++)
    {
      d = DotProduct201(w->points[j], plane->normal) - plane->dist;
      if (d > ON_EPSILON)
        return;  /* point in front — plane doesn't clip this brush */
      if (d > -ON_EPSILON)
        onPlaneCount++;
    }

    if (onPlaneCount >= 2)
    {
      if (onPlaneCount == w->numpoints)
        return;  /* all coplanar — no bevel needed */
      needsBevel = 1;
    }
  }

  if (!needsBevel)
    return;

  /* grow collision sides array by 1 */
  newSides = malloc(sizeof(BrushSide_t) * (refBrush->numCollisionSides + 1));
  memcpy(newSides, refBrush->collisionSides, sizeof(BrushSide_t) * refBrush->numCollisionSides);
  oldSides = refBrush->collisionSides;
  if (oldSides != refBrush->sides)  /* not inline sides */
    free(oldSides);
  refBrush->collisionSides = newSides;

  /* initialize the new bevel side */
  bevelSide = &newSides[refBrush->numCollisionSides];
  memset(bevelSide, 0, sizeof(BrushSide_t));
  bevelSide->planenum = planeIdx;
  bevelSide->shaderInfo = nSide->shaderInfo;
  bevelSide->materialName = nSide->materialName;
  bevelSide->bevel = 1;
  refBrush->numCollisionSides++;
}

/*
================
AddNeighborBevelsForBrushPair

Adds bevel sides between a pair of neighboring brushes.
Processes both directions: brush[0]->brush[1] and brush[1]->brush[0].
brushPair[0] and brushPair[1] are the two brush pointers.
================
*/
int AddNeighborBevelsForBrushPair(Brush_t **brushPair)
{
  int dir;
  int sideIdx;

  /* process both directions: brush[0]→brush[1] then brush[1]→brush[0] */
  for (dir = 0; dir < 2; dir++)
  {
    /* skip 6 axial bbox sides */
    for (sideIdx = BRUSH_AXIAL_SIDES; sideIdx < brushPair[dir]->numCollisionSides; sideIdx++)
      AddBevelSideForNeighbor(sideIdx, brushPair[1 - dir], brushPair[dir]);
  }
  return 0;
}

/*
================
BrushBoundsOverlap

Tests if two brush bounding boxes overlap within an epsilon tolerance.
================
*/
int BrushBoundsOverlap(Brush_t *b1, Brush_t *b2, float epsilon)
{
  int axis;

  /* check overlap on each axis: b1.eMaxs[i]+eps >= b2.eMins[i] && b2.eMaxs[i]+eps >= b1.eMins[i] */
  for ( axis = 0; axis < 3; ++axis )
  {
    if ( epsilon + b1->eMaxs[axis] < b2->eMins[axis] || epsilon + b2->eMaxs[axis] < b1->eMins[axis] )
      return 0;
  }
  return 1;
}

/*
================
FindBrushNeighbors

Finds all brushes in the entity that overlap with refBrush's bounds
and share the same content flags. Stores up to neighborLimit pointers.
================
*/
int FindBrushNeighbors(Brush_t *refBrush, Brush_t *brushes, Brush_t **neighbors, int neighborLimit)
{
  Brush_t *b;
  int neighborCount;

  Assert(refBrush != 0, g_assertBrushNeighbor_1);
  Assert(brushes != 0, g_assertBrushNeighbor_2);
  Assert(neighbors != 0, g_assertBrushNeighbor_3);
  Assert(neighborLimit > 0, g_assertBrushNeighbor_4);
  /* walk entity brush list, collecting overlapping brushes */
  b = brushes;
  for ( neighborCount = 0; b; b = b->next )
  {
    /* same content flags, overlapping bounds, not the same brush */
    if ( b->contentFlags == refBrush->contentFlags && BrushBoundsOverlap(b, refBrush, -ON_EPSILON) && b != refBrush )
    {
      Assert(neighborCount < neighborLimit, g_assertBrushNeighbor_5);
      neighbors[neighborCount++] = b;
      if ( neighborCount == neighborLimit )
        break;
    }
  }
  return neighborCount;
}

/*
================
ExpandPlaneForCapsule

Copies a plane and expands its distance for capsule collision.
Adds sphereRadius + fabs(capsuleHalfHeight * normal.z) to dist.
================
*/
float *ExpandPlaneForCapsule(float *srcPlane, float *destPlane, float sphereRadius, float capsuleHalfHeight)
{
  VectorCopy(srcPlane, destPlane);
  /* expand dist for capsule */
#ifdef _WIN64
  destPlane[3] = (float)(fabs((double)capsuleHalfHeight * srcPlane[2]) + (double)srcPlane[3] + sphereRadius);
#else
  destPlane[3] = (float)fabs(capsuleHalfHeight * srcPlane[2]) + srcPlane[3] + sphereRadius;
#endif
  return srcPlane;
}

/*
================
AddBrushPoint

Callback for BrushToPoints to add intersection points.
Called when a valid 3-plane intersection point is found. Checks if the point
lies inside all OTHER brush planes (not the 3 that formed it), then stores
the point in the global g_brushPoints buffer.
Point storage in g_brushPoints with stride 6:
  [0-2] = point xyz coordinates
  [3-5] = the 3 plane indices that form this vertex
Returns pointCount + 1 if point accepted, pointCount if rejected.
================
*/
int AddBrushPoint(Brush_t *brush, BrushSide_t *sides, int numSides, float capsuleRad, float capsuleHalf,
                  int *planeIndices, int checkBevel, float *point, int pointCount)
{
  BrushSide_t *side;
  Plane_t *plane;
  int i, offset;

  (void)capsuleRad;
  (void)capsuleHalf;
  /* check point against all non-bevel brush planes */
  for (i = 0; i < numSides; i++)
  {
    side = &sides[i];
    if (checkBevel && side->bevel)
      continue;
    /* skip the 3 planes that form this point */
    if (side->planenum == planeIndices[0] || side->planenum == planeIndices[1] || side->planenum == planeIndices[2])
      continue;
    plane = MAP_PLANE(side->planenum);
    if (DotProduct210(point, plane->normal) - plane->dist > ON_EPSILON)
      return pointCount;  /* point outside this plane */
  }

  /* point passed all plane checks — add it */
  if (pointCount == MAX_BRUSH_POINTS)
    Com_Error("More than %i pts from plane intersections on %i-sided brush\n", MAX_BRUSH_POINTS, numSides);

  /* store point with stride 6: xyz + 3 plane indices */
  offset = BRUSH_POINT_COMPACT_STRIDE * pointCount;
  memcpy(&g_brushPoints[offset], point, sizeof(vec3_t));
  g_brushPoints[offset + 3] = planeIndices[0];
  g_brushPoints[offset + 4] = planeIndices[1];
  g_brushPoints[offset + 5] = planeIndices[2];

  return pointCount + 1;
}

/*
================
FindPointInBuffer

Searches the point buffer for an existing point matching the given coords.
Used to avoid duplicate points when building windings.
Iterates with BRUSH_POINT_STRIDE per entry.
Returns index of matching point, or numPts if not found.
================
*/
int FindPointInBuffer(float *points, int numPoints, float *target)
{
  int i;

  for (i = 0; i < numPoints; i++, points += BRUSH_POINT_STRIDE)
  {
    if (VectorCompareEpsilon(target, points, PLANESIDE_EPSILON, 3))
      return i;
  }
  return i;
}

/*
================
AddBrushPointCapsule

Callback for BrushToPoints with capsule collision support.
Similar to AddBrushPoint but includes capsule radius/height expansion.
Stores point in g_brushPoints with BRUSH_POINT_STRIDE per entry:
  [0-2]   = point xyz
  [3]     = unused
  [4-19]  = per-side data (dot products, flags, etc.)
Returns new point count, or numPoints if point rejected.
================
*/
int AddBrushPointCapsule(Brush_t *brush, BrushSide_t *sides, int numSides, float capsRadius, float capsHeight,
                         int *planeIndices, int checkBevel, float *point, int numPoints)
{
  float expandedRadius;
  float expandedZ;
  int existingIdx;
  int *pointEntry;
  int bitMask;
  int arrayIndex;
  int sideIndex;
  int *bitmaskPtr;
  BrushSide_t *side;
  Plane_t *plane;
  float expandedPlane[4];
  float dot;

  (void)sides;
  (void)numSides;
  (void)checkBevel;

  if (numPoints == MAX_BRUSH_POINTS)
    Com_Error("More than %i points from plane intersections on %i-sided brush\n", MAX_BRUSH_POINTS, brush->numSides);

  /* bounds check: point must be within expanded brush bounds */
  expandedRadius = capsRadius + ON_EPSILON;
  expandedZ = expandedRadius + capsHeight;
  if (brush->eMins[0] - expandedRadius > point[0]
    || expandedRadius + brush->eMaxs[0] < point[0]
    || brush->eMins[1] - expandedRadius > point[1]
    || expandedRadius + brush->eMaxs[1] < point[1]
    || brush->eMins[2] - expandedZ > point[2]
    || expandedZ + brush->eMaxs[2] < point[2])
  {
    return numPoints;
  }

  /* check if point already exists in buffer */
  existingIdx = FindPointInBuffer((float *)g_brushPoints, numPoints, point);
  pointEntry = &g_brushPoints[BRUSH_POINT_STRIDE * existingIdx];
  if (existingIdx < numPoints)
    return numPoints;  /* duplicate point */

  /* new point — initialize entry */
  memset(pointEntry, 0, BRUSH_POINT_STRIDE * sizeof(int));
  memcpy(pointEntry, point, sizeof(vec3_t));

  if (brush->numSides <= 0)
    return numPoints + 1;

  /* classify point against each brush side (expanded for capsule) */
  bitmaskPtr = pointEntry + 4;
  arrayIndex = 0;
  bitMask = 1;
  for (sideIndex = 0; sideIndex < brush->numSides; sideIndex++)
  {
    side = &brush->sides[sideIndex];

    /* skip the 3 planes that form this point */
    if (side->planenum != planeIndices[0] && side->planenum != planeIndices[1] && side->planenum != planeIndices[2])
    {
      plane = MAP_PLANE(side->planenum);
      ExpandPlaneForCapsule(plane->normal, expandedPlane, capsRadius, capsHeight);
      dot = DotProduct102(point, expandedPlane) - expandedPlane[3];
      Assert(arrayIndex == sideIndex >> 5, g_assertArrayIndex);
      Assert(bitMask == SIDE_BIT(sideIndex), g_assertBrush_5);
      if (dot > ON_EPSILON)
      {
        if ((*bitmaskPtr & bitMask) == 0)
        {
          *bitmaskPtr |= bitMask;      /* mark as in front of plane */
          ++pointEntry[BRUSH_POINT_DATA_OFS];  /* increment side count */
        }
      }
      else if (dot < -ON_EPSILON)
      {
        bitmaskPtr[SIDE_BEHIND_OFS] |= bitMask;  /* mark as behind plane */
      }
    }

    bitMask *= 2;
    if (!bitMask)  /* overflow — move to next bitmask word */
    {
      bitMask = 1;
      ++arrayIndex;
      ++bitmaskPtr;
    }
  }
  return numPoints + 1;
}

/*
================
BrushToPoints

Converts a brush (defined by planes) into a set of vertex points by finding
all valid intersections of 3 planes. This is the core algorithm for building
brush geometry.
For each combination of 3 non-parallel planes:
  1. Compute intersection point (PlaneIntersection3)
  2. Verify point is inside all other planes
  3. Call callback to store the point
Returns number of valid points found.
================
*/
int BrushToPoints(
        Brush_t *brush,
        BrushSide_t *sides,
        int numSides,
        float capsuleRadius,
        float capsuleHalfHeight,
        int checkBevel,
        int (*addPointFn)(Brush_t *, BrushSide_t *, int, float, float, int *, int, float *, int))
{
  Plane_t *p0, *p1, *p2;
  long double capsuleOffset0, capsuleOffset1, capsuleOffset2;
  long double adjustedDist0, adjustedDist2;
  /* Stack layout struct — member order matches original binary stack frame */
  struct {
    float planeData0[4];      /* first plane (normal + adjusted dist) */
    float planeData1[4];      /* second plane */
    float planeData2[4];      /* third plane */
    float intersectPoint[4];  /* intersection point (x,y,z) + padding */
    int outerSideIdx;
    int _padding1[3];
    float *planePointers[4];  /* pointers to 3 local plane copies */
    int storedPlaneIdx[4];    /* plane indices [0,1,2] + padding */
    BrushSide_t *currentSidePtr; /* tracks currentSide (binary layout) */
    int numSidesCopy;
    BrushSide_t *sidesBase;
    int pointCount;
    int sideIdx1;
    int sideIdx2;
    int planeIdx0;
    BrushSide_t *currentSide;
  } stk;

  Assert(capsuleRadius >= 0.0f, g_assertCapsuleRadius);
  Assert(capsuleHalfHeight >= 0.0f, g_assertCapsuleHeight);

  /* set up plane pointer array for PlaneIntersection3 */
  stk.planePointers[0] = stk.planeData0;
  stk.planePointers[1] = stk.planeData1;
  stk.planePointers[2] = stk.planeData2;
  stk.numSidesCopy = numSides;
  stk.pointCount = 0;
  stk.outerSideIdx = 0;

  if ( numSides - 2 <= 0 )
    return stk.pointCount;

  stk.sidesBase = sides;
  stk.currentSide = sides;
  stk.currentSidePtr = sides;

  /* iterate all triples of brush sides to find intersection points */
  for ( ; stk.outerSideIdx < stk.numSidesCopy - 2;
        stk.outerSideIdx++, stk.currentSide++, stk.currentSidePtr++ )
  {
    /* skip bevel sides */
    if ( checkBevel && stk.currentSide->bevel )
      continue;

    stk.planeIdx0 = stk.currentSide->planenum;
    p0 = MAP_PLANE(stk.planeIdx0);

    /* copy first plane, adjusting dist for capsule collision */
    stk.planeData0[0] = p0->normal[0];
    capsuleOffset0 = capsuleHalfHeight * p0->normal[2];
    stk.planeData0[1] = p0->normal[1];
    stk.planeData0[2] = p0->normal[2];
    stk.sideIdx1 = stk.outerSideIdx + 1;
    adjustedDist0 = fabs(capsuleOffset0) + p0->dist + capsuleRadius;
    stk.storedPlaneIdx[0] = stk.planeIdx0;
    stk.planeData0[3] = adjustedDist0;

    /* find second plane of triple */
    for ( ; stk.sideIdx1 < stk.numSidesCopy - 1; stk.sideIdx1++ )
    {
      stk.planeIdx0 = stk.storedPlaneIdx[0];

      if ( checkBevel && stk.sidesBase[stk.sideIdx1].bevel )
        continue;

      stk.storedPlaneIdx[1] = stk.sidesBase[stk.sideIdx1].planenum;
      if ( stk.storedPlaneIdx[1] == stk.planeIdx0 )
        continue;

      p1 = MAP_PLANE(stk.storedPlaneIdx[1]);
      stk.planeData1[0] = p1->normal[0];
      capsuleOffset1 = capsuleHalfHeight * p1->normal[2];
      stk.planeData1[1] = p1->normal[1];
      stk.planeData1[2] = p1->normal[2];
      stk.sideIdx2 = stk.sideIdx1 + 1;

      /* find third plane and compute intersection */
      for ( stk.planeData1[3] = fabs(capsuleOffset1) + p1->dist + capsuleRadius;
            stk.sideIdx2 < stk.numSidesCopy; ++stk.sideIdx2 )
      {
        if ( checkBevel && stk.sidesBase[stk.sideIdx2].bevel )
          continue;

        stk.storedPlaneIdx[2] = stk.sidesBase[stk.sideIdx2].planenum;
        if ( stk.storedPlaneIdx[2] == stk.storedPlaneIdx[1] || stk.storedPlaneIdx[2] == stk.storedPlaneIdx[0] )
          continue;

        p2 = MAP_PLANE(stk.storedPlaneIdx[2]);
        capsuleOffset2 = capsuleHalfHeight * p2->normal[2];
        stk.planeData2[0] = p2->normal[0];
        stk.planeData2[1] = p2->normal[1];
        adjustedDist2 = fabs(capsuleOffset2) + p2->dist + capsuleRadius;
        stk.planeData2[2] = p2->normal[2];
        stk.planeData2[3] = adjustedDist2;

        /* compute 3-plane intersection point */
        if ( PlaneIntersection3(stk.planePointers, stk.intersectPoint) )
        {
          SnapPlaneIntersection(stk.planePointers, stk.intersectPoint, SNAP_GRID_SIZE, SNAP_EPSILON);
          stk.pointCount = addPointFn(brush, sides, numSides, capsuleRadius, capsuleHalfHeight,
                                      stk.storedPlaneIdx, checkBevel, stk.intersectPoint, stk.pointCount);
        }
      }
    }
  }

  return stk.pointCount;
}

/*
================
FilterPointsOnPlane

Filters brush vertex points to find those that lie on a specific plane.
A point lies on a plane if one of its 3 generating plane indices matches.

Point buffer layout (per point, 6 ints):
  [0-2] = xyz coordinates (3 floats)
  [3-5] = plane indices that form this vertex (3 ints)
================
*/
int FilterPointsOnPlane(int numPoints, int targetPlaneIdx, BrushPoint_t *pointsBuffer, vec3_t *outputBuffer, int maxOutput)
{
  BrushPoint_t *srcPoints = pointsBuffer;
  vec3_t *outPoints = outputBuffer;
  int currentOutput;
  int i, j;
  int isDuplicate;

  currentOutput = 0;
  if ( numPoints <= 0 )
    return 0;

  for ( i = 0; i < numPoints; i++ )
  {
    /* check if this point was formed by the target plane */
    if ( targetPlaneIdx != srcPoints[i].planeIndices[0] &&
         targetPlaneIdx != srcPoints[i].planeIndices[1] &&
         targetPlaneIdx != srcPoints[i].planeIndices[2] )
      continue;

    /* check for duplicate points in output */
    isDuplicate = 0;
    for ( j = 0; j < currentOutput; j++ )
    {
      if ( VectorCompareEpsilon(outPoints[j], srcPoints[i].xyz, 0.001f, 3) )
      {
        isDuplicate = 1;
        break;
      }
    }
    if ( isDuplicate )
      continue;

    /* add point to output */
    if ( currentOutput == maxOutput )
      Com_Error("Winding point limit (%i) exceeded on brush face", maxOutput);

    VectorCopy(srcPoints[i].xyz, outPoints[currentOutput]);
    currentOutput++;
  }

  return currentOutput;
}

/*
================
GetWindingFromPoints

Creates a winding polygon from brush vertex points for a specific plane.

Process:
  1. Filter points that lie on this plane (FilterPointsOnPlane)
  2. Build the canonical convex hull and orient it to the plane
================
*/
Winding_t *GetWindingFromPoints(BrushPoint_t *pointBuffer, int planeIdx, int numPoints)
{
  Plane_t *plane;
  vec3_t filteredPoints[MAX_POINTS_ON_WINDING];
  int filteredCount;

  /* filter points that lie on this plane */
  filteredCount = FilterPointsOnPlane(numPoints, planeIdx, pointBuffer, filteredPoints, MAX_POINTS_ON_WINDING);
  if ( filteredCount < 3 )
    return NULL;

  /* Native uses the shared signed-axis convex hull builder here. */
  plane = MAP_PLANE(planeIdx);
  return WindingFromPoints(plane->normal, filteredPoints, filteredCount);
}

/*
================
CreateBrushWindings

Processes a brush and creates windings for all sides.
This is the main entry point for brush geometry processing.

Process:
  1. Call BrushToPoints to compute all vertex points from plane intersections
  2. If fewer than 4 points, brush is degenerate - return 0
  3. For each non-beveled side:
     a. Create winding from points (GetWindingFromPoints)
     b. Store winding in side
     c. Verify winding lies on plane (CheckWindingInPlane)
  4. Add brush to BSP tree (BoundBrush)
================
*/
int CreateBrushWindings(Brush_t *brush)
{
  Brush_t *b = brush;
  BrushSide_t *sides = b->sides;
  BrushSide_t *side;
  int numPoints;
  Winding_t *w;
  int i;

  /* compute all vertex points from plane intersections */
  numPoints = BrushToPoints(brush, sides, b->numSides, 0.0f, 0.0f, qtrue, AddBrushPoint);

  /* degenerate brush - fewer than 4 points */
  if ( numPoints < MIN_BRUSH_POINTS )
  {
    return 0;
  }

  /* walk the list of brush sides */
  for ( i = 0; i < b->numSides; ++i )
  {
    side = &sides[i];
    Assert(side->winding == NULL, g_assertWindingNull);
    /* skip bevel sides */
    if ( !side->bevel )
    {
      /* create winding from computed points */
      w = GetWindingFromPoints((BrushPoint_t *)g_brushPoints, side->planenum, numPoints);
      side->winding = w;
      CheckWindingInPlane(w, MAP_PLANE(side->planenum)->normal, MAP_PLANE(side->planenum)->dist);
    }
  }

  /* find brush bounds */
  return BoundBrush(b);
}

/*
================
FilterBrushIntoBspTree

Recursively filters a brush into BSP tree. At each internal node,
the brush is split by the node's plane and the resulting pieces
are filtered into the appropriate child nodes.
At leaf nodes (planeNum == -1), the brush is added to the leaf's brush list.
================
*/
int FilterBrushIntoBspTree(Brush_t *brush, Node_t *node)
{
  Brush_t *originalBrush;
  Brush_t *backBrush;
  int frontCount;

  if ( !brush )
    return 0;

  if ( node->planenum == PLANENUM_LEAF )
  {
    /* leaf node: add brush to this leaf's brush list */
    brush->next = node->leafBrushes;
    node->leafBrushes = brush;

    /* Native 0x4036B0 writes Node_t +0x24 here.  That slot is the leaf
       solid/opaque state used throughout portals.cpp, not a separate portal
       marker.  Brush byte +0x1A is the opaque-material predicate. */
    if ( !brush->detail && brush->isOpaque )
    {
      float brushVolume = BoundsVolume(brush->eMins, brush->eMaxs);
      float nodeVolume = BoundsVolume(node->mins, node->maxs);
      float minimumVolume = fminf(1.0f, nodeVolume * 0.125f);

      if ( brushVolume >= minimumVolume )
        node->opaque = 1;
    }
    return 1;
  }

  /* split brush by this node's plane */
  originalBrush = brush;
  SplitBrushByPlane(brush, node->planenum, &brush, &backBrush);
  FreeBrush(originalBrush);

  /* recursively filter front and back pieces */
  frontCount = FilterBrushIntoBspTree(brush, node->children[0]);
  return frontCount + FilterBrushIntoBspTree(backBrush, node->children[1]);
}

/*
================
FilterDetailBrushesIntoBspTree

Fragments all detail brushes into the structural BSP leafs.
================
*/
int FilterDetailBrushesIntoBspTree(Entity_t *entity, Tree_t *tree)
{
  Brush_t *b;
  int c_unique;
  int c_clusters;
  Brush_t *newb;

  Com_DPrintf("----- FilterDetailBrushesIntoTree -----\n");
  b = entity->brushes;
  c_unique = 0;
  for ( c_clusters = 0; b; b = b->next )
  {
    if ( b->detail )
    {
      ++c_unique;
      newb = CopyBrush(b);
      c_clusters += FilterBrushIntoBspTree(newb, tree->headnode);
    }
  }
  Com_DPrintf("%5i detail brushes\n", c_unique);
  return Com_DPrintf("%5i cluster references\n", c_clusters);
}

/*
================
FilterStructuralBrushesIntoBspTree

Iterates through all brushes in the entity and filters structural brushes
(detail == 0) into the BSP tree. Detail brushes are skipped.

For each structural brush:
1. Copy the brush (since filtering may split it)
2. Call FilterBrushIntoBspTree to distribute through BSP
3. Mark brush sides that have windings as visible
================
*/
int FilterStructuralBrushesIntoBspTree(Entity_t *entity, Tree_t *tree)
{
  Brush_t *b;
  int brushCount;
  int fragmentCount;
  Brush_t *brushCopy;

  Com_DPrintf("----- FilterStructuralBrushesIntoTree -----\n");
  b = entity->brushes;
  brushCount = 0;

  for ( fragmentCount = 0; b; b = b->next )
  {
    /* only process structural brushes */
    if ( !b->detail )
    {
      ++brushCount;

      brushCopy = CopyBrush(b);
      fragmentCount += FilterBrushIntoBspTree(brushCopy, tree->headnode);
    }
  }

  Com_DPrintf("%5i structural brushes\n", brushCount);
  return Com_DPrintf("%5i cluster references\n", fragmentCount);
}

/*
================
BrushPointsAllInsidePlanes

Tests if all given points lie inside (or on) all non-bevel planes
of a brush, expanded for capsule collision.
Returns 1 if all points are inside, 0 if any point is outside.
================
*/
int BrushPointsAllInsidePlanes(Brush_t *brush, int numPoints, float capsRadius, float capsHeight, vec3_t *pointsBuffer)
{
  vec3_t *points = pointsBuffer;
  Plane_t *plane;
  float expandedDist;
  int i, j;

  for ( i = 0; i < brush->numSides; i++ )
  {
    if ( brush->sides[i].bevel || numPoints <= 0 )
      continue;

    plane = MAP_PLANE(brush->sides[i].planenum);
#ifdef _WIN64
    expandedDist = (float)(fabs((double)capsHeight * plane->normal[2]) + (double)plane->dist + capsRadius);
#else
    expandedDist = fabs(capsHeight * plane->normal[2]) + plane->dist + capsRadius;
#endif

    /* check each point against this plane */
    for ( j = 0; j < numPoints; j++ )
    {
      if ( DotProduct021(plane->normal, points[j]) - expandedDist > ON_EPSILON )
        return 0;  /* point outside this plane */
    }
  }

  return 1;  /* all points inside all planes */
}

/*
================
FilterBrushPointsAndTestBrushes

Filters brush points that are relevant to a specific side (sideIndex),
then tests if ANY neighbor brush fully contains all those points.
Returns 1 if a neighbor contains all filtered points (side is redundant).
================
*/
int FilterBrushPointsAndTestBrushes(int sideIndex, int numPoints, float capsRadius, float capsHeight, Brush_t **neighbors, int numNeighbors)
{
  int sideBit;
  int sideWord;
  int *entry;
  int sideCount;
  int i;
  
  /* output buffer for filtered points — MAX_BRUSH_POINTS * sizeof(vec3_t) total */
  struct { int firstWord; int _pad; char data[MAX_BRUSH_POINTS * sizeof(vec3_t) - 8]; } newBrushPts;
  vec3_t *outPoints = (vec3_t *)&newBrushPts.firstWord;
  unsigned int newBrushPtCount;

  sideBit = SIDE_BIT(sideIndex);
  sideWord = sideIndex >> 5;
  newBrushPtCount = 0;

  /* filter points relevant to this side */
  entry = g_brushPoints;
  for ( i = 0; i < numPoints; i++, entry += BRUSH_POINT_STRIDE )
  {
    sideCount = entry[BRUSH_POINT_DATA_OFS];

    /* sideBits layout: [DATA_OFS+1 .. +8] = "on" bits, [DATA_OFS+9 .. +16] = "behind" bits
       skip if: point is behind this side, or touches >1 side, or solo but not on this side */
    if ( sideBit & entry[BRUSH_POINT_DATA_OFS + 1 + sideWord + SIDE_BEHIND_OFS] )
      continue;
    if ( (unsigned int)sideCount > 1 )
      continue;
    if ( sideCount && !(sideBit & entry[BRUSH_POINT_DATA_OFS + 1 + sideWord]) )
      continue;

    Assert(newBrushPtCount < MAX_BRUSH_POINTS, g_assertBrush_17);
    VectorCopy((float *)entry, outPoints[newBrushPtCount]);
    newBrushPtCount++;
  }

  /* test if any neighbor brush contains all filtered points */
  for ( i = 0; i < numNeighbors; i++ )
  {
    if ( BrushPointsAllInsidePlanes(neighbors[i], newBrushPtCount, capsRadius, capsHeight, outPoints) )
      return 1;  /* neighbor contains all points — side is redundant */
  }
  return 0;
}

/*
================
RemoveRedundantBrushPlanes

Identifies and marks brush collision planes that are redundant
(their points are all contained by neighbor brushes).
Iterates through sides with solo points, testing each against neighbors.
Returns count of redundant planes removed.
================
*/
int RemoveRedundantBrushPlanes(Brush_t *brush, Brush_t **neighbors, int numNeighbors, float capsRadius, float capsHeight, char passIndex, char *redundancyFlags)
{
  int numPoints;
  int numSides;
  int sideIdx;
  int totalPoints[MAX_COLLISION_SIDES];
  int soloPoints[MAX_COLLISION_SIDES];
  int redundantCount;

  /* compute brush points with capsule expansion */
  numPoints = BrushToPoints(
         brush,
         brush->collisionSides,
         brush->numCollisionSides,
         capsRadius,
         capsHeight,
         qfalse,
         AddBrushPointCapsule);

  /* assert: need at least 4 points for a valid brush */
  if ( numPoints < MIN_BRUSH_POINTS && !g_assertBrush_21 )
  {
    va("%s\n", "(ptCount >= 4)", numPoints);
    Assert(0, g_assertBrush_21);
  }

  /* count how many points touch each side */
  numSides = brush->numCollisionSides;
  CountPointsOnSides(totalPoints, soloPoints, numSides, numPoints);

  /* iterate through sides that have solo points (starting after 6 bbox sides) */
  redundantCount = 0;
  for ( sideIdx = FindNextSoloSide(5, soloPoints, numSides);
        sideIdx < numSides;
        sideIdx = FindNextSoloSide(sideIdx, soloPoints, brush->numCollisionSides) )
  {
    if ( redundancyFlags[sideIdx] != passIndex )
      continue;

    if ( !FilterBrushPointsAndTestBrushes(sideIdx, numPoints, capsRadius, capsHeight, neighbors, numNeighbors) )
      continue;

    /* side is redundant */
    redundantCount++;
    redundancyFlags[sideIdx]++;
    CullSideFromPoints(numPoints, sideIdx, soloPoints, totalPoints, brush->numCollisionSides);
    sideIdx = 5;  /* restart search from beginning */
  }

  return redundantCount;
}

/*
================
RemoveRedundantCollisionPlanes

Removes collision planes from a brush that are fully covered by
neighbor brushes. Runs up to 2 passes with different capsule sizes
(sphere-only and sphere+capsule). After marking redundant sides,
compacts the collision sides array by removing marked entries.
================
*/
int RemoveRedundantCollisionPlanes(Brush_t *brush, Brush_t **neighbors, int numNeighbors)
{
  int redundantCount;
  int numPasses;
  int passIdx;
  int result;
  int writeIdx, readIdx;
  BrushSide_t *collSides;
  char sideFlags[MAX_COLLISION_SIDES];
  
  /* capsule size pairs: pass 0 = sphere-only, pass 1 = player capsule */
  float capsuleParams[4] = { 0.0f, 0.0f, PLAYER_CAPSULE_RADIUS, PLAYER_CAPSULE_HALFHEIGHT };

  memset(sideFlags, 0, brush->numCollisionSides);
  redundantCount = 0;
  numPasses = 0;

  for ( passIdx = 0; passIdx < 2; passIdx++ )
  {
    if ( !(brushMethod & (1 << passIdx)) )
      continue;

    result = RemoveRedundantBrushPlanes(brush, neighbors, numNeighbors,
               capsuleParams[2 * passIdx], capsuleParams[2 * passIdx + 1], numPasses, sideFlags);
    redundantCount = result;
    if ( !result )
      return result;
    numPasses++;
  }

  /* if collision sides are inline (same as brush sides), separate them */
  if ( brush->collisionSides == brush->sides )
  {
    if ( brush->numCollisionSides != brush->numSides && !g_assertBrush_22 )
    {
      va("%i !", brush->numCollisionSides, brush->numSides);
      va("%s\n\t%s", "brush->collisionSideCount == brush->sideCount", 0);
      Assert(0, g_assertBrush_22);
    }
    collSides = malloc(sizeof(BrushSide_t) * brush->numCollisionSides);
    memcpy(collSides, brush->sides, sizeof(BrushSide_t) * brush->numCollisionSides);
    brush->collisionSides = collSides;
  }

  /* compact: remove marked sides by shifting non-redundant ones down */
  collSides = brush->collisionSides;
  writeIdx = BRUSH_AXIAL_SIDES;
  for ( readIdx = BRUSH_AXIAL_SIDES; readIdx < brush->numCollisionSides; readIdx++ )
  {
    if ( sideFlags[readIdx] == numPasses )
      continue;  /* redundant side — skip */

    if ( writeIdx != readIdx )
    {
      Assert(brush->collisionSides != brush->sides, g_assertBrush_23);
      Assert(writeIdx < readIdx, g_assertBrush_24);
      memcpy(&collSides[writeIdx], &collSides[readIdx], sizeof(BrushSide_t));
    }
    writeIdx++;
  }

  Assert(writeIdx == brush->numCollisionSides - redundantCount, g_assertBrushNeighbor_0);
  brush->numCollisionSides -= redundantCount;
  g_removedBrushSides += redundantCount;
  return redundantCount;
}

/*
================
AddBrushNeighborBevels

Adds neighbor bevel planes to each brush for collision optimization,
then removes redundant collision planes that are fully covered by neighbors.
================
*/
int AddBrushNeighborBevels(Brush_t *brushes)
{
  Brush_t *br, *b, *neighborBrush;
  int numNeighbors;
  Brush_t *neighborBuffer[MAX_COLLISION_SIDES];
  double startTime, endTime;
  Brush_t *brushPair[2];

  startTime = I_FloatTime();
  if ( !g_currentEntityIndex )
    printf("Adding brush neighbor bevels...\n");

  /* Phase 1: Add neighbor bevel planes to each brush */
  for ( br = brushes; br; br = br->next )
  {
    /* init collision sides = brush sides (separated later if bevels are added) */
    br->numCollisionSides = br->numSides;
    br->collisionSides = br->sides;

    if ( br->isOccluder )
      continue;

    brushPair[1] = br;
    for ( neighborBrush = brushes; neighborBrush != br; neighborBrush = neighborBrush->next )
    {
      Assert(neighborBrush != NULL, g_assertBrushNeighbor_6);
      if ( !neighborBrush->isOccluder && BrushBoundsOverlap(br, neighborBrush, -ON_EPSILON) )
      {
        brushPair[0] = neighborBrush;
        AddNeighborBevelsForBrushPair(brushPair);
      }
    }
  }

  /* Phase 2: Remove redundant collision planes */
  if ( !g_currentEntityIndex )
    printf("Removing redundant brush collision planes...\n");

  for ( b = brushes; b; b = b->next )
  {
    Assert(b->numCollisionSides >= BRUSH_AXIAL_SIDES, g_assertBrushNeighbor_7);
    if ( b->isOccluder || b->numCollisionSides == BRUSH_AXIAL_SIDES )
      continue;

    numNeighbors = FindBrushNeighbors(b, brushes, neighborBuffer, MAX_COLLISION_SIDES);
    if ( numNeighbors )
    {
      RemoveRedundantCollisionPlanes(b, neighborBuffer, numNeighbors);
      Assert(b->numCollisionSides >= BRUSH_AXIAL_SIDES, g_assertBrushNeighbor_8);
    }
  }

  endTime = I_FloatTime();
  if ( !g_currentEntityIndex )
  {
    printf("removed %i brush sides\n", g_removedBrushSides);
    printf("elapsed time %.0f seconds\n", endTime - startTime);
  }

  /* CoD4 0x403EB0 finishes by producing each collision-side edge cycle. */
  for ( b = brushes; b; b = b->next )
    BuildBrushSideAdjacencies(b);

  return g_currentEntityIndex;
}

/*
================
CompactBrushSides

Removes empty brush sides. After validation, some sides may be
marked for removal. This function compacts the remaining valid
sides to form a contiguous array.
================
*/
Brush_t *CompactBrushSides()
{
  Brush_t *b;
  int dst, src;

  b = g_buildBrush;
  dst = 0;

  /* compact valid sides (with windings) to the front */
  for ( src = 0; src < b->numSides; src++ )
  {
    if ( !b->sides[src].winding )
      continue;
    if ( dst != src )
      memcpy(&b->sides[dst], &b->sides[src], sizeof(BrushSide_t));
    dst++;
  }

  b->numSides = dst;
  return b;
}
