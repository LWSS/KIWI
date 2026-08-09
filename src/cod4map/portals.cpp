/*
portals.c — Portal generation, BSP tree, cell partitioning, PRT writing

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

CellBounds_t      cellBoundsData[MAX_CELL_BOUNDS];
CellPortalLink_t *g_cellPortalLinks[MAX_MAP_CELLS];

int   c_active_portals;
int   c_areas;
int   c_floodedleafs;
int   c_inside;
int   c_outside;
int   c_peak_portals;
int   c_solid;
int   c_tinyportals;
int   debugPortals;
char  g_brushSideBitmap[MAX_MAP_CELLS * MAX_MAP_CULLGROUPS / 8];
int   num_solidfaces;
int   num_visclusters;
int   num_visportals;
FILE *Stream;

/* Native portals.cpp keeps this separately from the per-side state.  A
   manual portal face records the two cells it borders in value/culled; this
   counter makes a conflicting second pair fatal after portal emission. */
static int s_portalErrorCount;

char s_assertDisable_BspTreeForMap;
char s_assertDisable_BspTreeForMap;
char s_assertDisable_BspTreeForMap;
char s_assertDisable_BuildSidePlanes;
char s_assertDisable_CreateLeafNode;
char s_assertDisable_CreateLeafNode;
char s_assertDisable_CreateLeafNode;
char s_assertDisable_CreateNode;
char s_assertDisable_CreateNode;
char s_assertDisable_CreateNode;
char s_assertDisable_FilterBrushIntoTree;
char s_assertDisable_FilterBrushIntoTree;
char s_assertDisable_FilterBrushIntoTree;
char s_assertDisable_FilterBrushIntoTree;
char s_assertDisable_FilterBrushIntoTree;
char s_assertDisable_Portal_EntityFlood;
char s_assertDisable_FreeBspTree;
char s_assertDisable_FreeBspTree_r;
char s_assertDisable_InitBspTreeNodes;
char s_assertDisable_InitBspTreeNodes;
char s_assertDisable_SplitBrush;
char s_assertDisable_SplitBrush;
char s_assertDisable_SplitBrush;
char s_assertDisable_SplitBrushList;
char s_assertDisable_WritePortalFile_Node;


/*
================
AllocPortal

Allocates and zero-initializes a portal structure.
================
*/
Portal_t *AllocPortal(void)
{
  Portal_t *p;

  if ( numthreads == 1 )
    c_active_portals++;
  if ( c_active_portals > c_peak_portals )
    c_peak_portals = c_active_portals;

  p = malloc(sizeof(*p));
  memset(p, 0, sizeof(*p));
  return p;
}

/*
================
FreePortal

Frees portal winding and portal memory.
================
*/
void FreePortal(Portal_t *p)
{
  if ( p->winding )
    FreeWinding(p->winding);
  if ( numthreads == 1 )
    --c_active_portals;
  free(p);
}

/*
================
Portal_EntityFlood

Returns true if portal connects two non-opaque leaves.
================
*/
int Portal_EntityFlood(Portal_t *p)
{
  if ( !p->hint )
    return 0;
  Assert(p->nodes[0]->planenum == PLANENUM_LEAF, s_assertDisable_Portal_EntityFlood);
  Assert(p->nodes[1]->planenum == PLANENUM_LEAF, s_assertDisable_Portal_EntityFlood);
  return !p->nodes[0]->opaque && !p->nodes[1]->opaque;
}

/*
================
AddPortalToNode

Links a portal between two BSP nodes (front and back).
Inserts portal at head of each node's portal linked list.
================
*/
void AddPortalToNode(Portal_t *p, Node_t *front, Node_t *back)
{
  if ( p->nodes[0] || p->nodes[1] )
    Com_Error("AddPortalToNode: already included");

  p->nodes[0] = front;
  p->next[0] = front->portals;
  front->portals = p;

  p->nodes[1] = back;
  p->next[1] = back->portals;
  back->portals = p;
}

/*
================
RemovePortalFromNode

Unlinks a portal from a BSP node's portal list.
Walks the node's portal linked list to find and remove the portal.
================
*/
void RemovePortalFromNode(Portal_t *portal, Node_t *l)
{
  Portal_t **pp, *t;

  /* remove reference to the current portal */
  pp = &l->portals;
  while ( 1 )
  {
    t = *pp;
    if ( !t )
      Com_Error("RemovePortalFromNode: portal not in leaf");
    if ( t == portal )
      break;
    if ( t->nodes[0] == l )
      pp = &t->next[0];
    else if ( t->nodes[1] == l )
      pp = &t->next[1];
    else
      Com_Error("RemovePortalFromNode: portal not bounding leaf");
  }

  if ( portal->nodes[0] == l )
  {
    *pp = portal->next[0];
    portal->nodes[0] = NULL;
  }
  else if ( portal->nodes[1] == l )
  {
    *pp = portal->next[1];
    portal->nodes[1] = NULL;
  }
}

/*
================
MakeTreePortals

Creates the 6 axis-aligned bounding portals for the BSP tree.
Pads tree bounds by SIDESPACE (8.0), creates 6 portal planes (one per face
of the bounding box), then clips each portal winding against the other 5.

Local array layout (bplanes[54]):
  [0..41]  bplanes[6] — 7 floats each: normal[3], dist, pad[3]
  [42..47] portals[6] — portal pointers
  [48..50] bounds[0]  — padded mins[3]
  [51..53] bounds[1]  — padded maxs[3]
================
*/
void MakeTreePortals(Tree_t *tree)
{
  int i, j, n;
  Portal_t *p;
  Plane_t bplanes[6];
  Portal_t *bportals[6];
  vec3_t bounds[2];
  Node_t *node;

  node = tree->headnode;

  /* compute padded bounds */
  for ( i = 0; i < 3; i++ )
  {
    #define SIDESPACE 8.0f
    bounds[0][i] = tree->mins[i] - SIDESPACE;
    bounds[1][i] = tree->maxs[i] + SIDESPACE;
    if ( bounds[0][i] >= bounds[1][i] )
      Com_Error("Backwards tree volume.\nThis usually means there are no structural brushes.\n");
  }

  /* initialize outside_node */
  tree->outside_node.planenum = PLANENUM_LEAF;
  tree->outside_node.leafBrushes = NULL;
  tree->outside_node.portals = NULL;
  tree->outside_node.opaque = 0;

  /* create 6 bounding portals (one per axis, two sides each) */
  for ( i = 0; i < 3; i++ )
  {
    for ( j = 0; j < 2; j++ )
    {
      n = j * 3 + i;

      p = AllocPortal();
      memset(&bplanes[n], 0, sizeof(Plane_t));

      if ( j == 0 )
      {
        bplanes[n].normal[i] = 1.0f;
        bplanes[n].dist = bounds[0][i];
      }
      else
      {
        bplanes[n].normal[i] = -1.0f;
        bplanes[n].dist = -bounds[1][i];
      }

      bportals[n] = p;
      memcpy(p, &bplanes[n], sizeof(Plane_t));
      p->winding = BaseWindingForPlane(bplanes[n].normal, bplanes[n].dist);
      AddPortalToNode(p, node, &tree->outside_node);
    }
  }

  /* clip each portal winding against the other 5 bounding planes */
  for ( i = 0; i < BRUSH_AXIAL_SIDES; i++ )
  {
    for ( j = 0; j < BRUSH_AXIAL_SIDES; j++ )
    {
      if ( j != i )
        ClipWindingByPlane(&bportals[i]->winding, bplanes[j].normal, bplanes[j].dist, ON_EPSILON);
    }
  }
}

/*
================
CalcNodeBounds

Calculates bounding box for a BSP node from its portals.
Iterates through all portals connected to this node and expands the
node's bounding box to include all portal winding vertices.
================
*/
void CalcNodeBounds(Node_t *node)
{
  Portal_t *p;
  Winding_t *w;
  int i, s;

  ClearBounds(node->mins, node->maxs);

  for ( p = node->portals; p; p = p->next[s] )
  {
    s = (p->nodes[1] == node);
    w = p->winding;
    if ( !w )
      continue;

    for ( i = 0; i < w->numpoints; i++ )
      AddPointToBounds(w->points[i], node->mins, node->maxs);
  }
}

/*
================
Portal_Passable

Recursively flood-fills through passable portals.
Marks each reachable leaf with an occupant distance value.
Skips already-occupied or opaque nodes.
================
*/
int Portal_Passable(Node_t *node, int dist)
{
  Portal_t *p;
  int s;

  if ( node->occupied )
    return node->occupied;
  if ( node->opaque )
    return node->opaque;

  ++c_floodedleafs;
  node->occupied = dist;

  for ( p = node->portals; p; p = p->next[s] )
  {
    s = (p->nodes[1] == node);
    Portal_Passable(p->nodes[!s], dist + 1);
  }
  return dist;
}

/*
================
IsExcludedEntity

Checks if classname is in the flood-fill exclusion list.
================
*/
int IsExcludedEntity(int unused, const char *classname)
{
  int i;

  for ( i = 0; i < NUM_SCRIPT_CLASSNAMES; i++ )
  {
    if ( !strcmp(classname, g_scriptClassnames[i]) )
      return 1;
  }
  return 0;
}

/*
================
FloodAreas_r

Recursively assigns area numbers through passable portals.
================
*/
int FloodAreas_r(Node_t *node)
{
  Portal_t *p;
  int s;

  if ( node->area != -1 || node->cluster == -1 )
    return -1;

  node->area = c_areas;

  for ( p = node->portals; p; p = p->next[s] )
  {
    s = (p->nodes[1] == node);

    if ( !p->hint )
      continue;

    /* hint portals connect areas — validate both sides are leaves */
    if ( p->nodes[0]->planenum != PLANENUM_LEAF || p->nodes[1]->planenum != PLANENUM_LEAF )
      Com_Error("Portal_EntityFlood: not a leaf");

    if ( !p->nodes[0]->opaque && !p->nodes[1]->opaque )
      FloodAreas_r(p->nodes[!s]);
  }

  return c_areas;
}

/*
================
FindAreas_r

Traverses BSP tree and initiates area flood fills.
================
*/
int FindAreas_r(Node_t *node)
{
  Node_t *n;

  /* recurse down the tree to leaves */
  for ( n = node; n->planenum != PLANENUM_LEAF; n = n->children[1] )
    FindAreas_r(n->children[0]);

  /* start a new area flood from unassigned non-opaque leaves */
  if ( !n->opaque && n->area == -1 )
  {
    FloodAreas_r(n);
    return ++c_areas;
  }

  return n->opaque;
}

/*
================
CheckAreas_r

Warns about leaves with unassigned areas.
================
*/
int CheckAreas_r(Node_t *node)
{
  Node_t *n;

  /* recurse down to leaves */
  for ( n = node; n->planenum != PLANENUM_LEAF; n = n->children[1] )
    CheckAreas_r(n->children[0]);

  if ( !n->opaque )
  {
    if ( n->cluster != -1 && n->area == -1 )
    {
      Com_Printf("WARNING: cluster %d has area set to -1\n", n->cluster);
      return 0;
    }
    return n->cluster;
  }

  return n->opaque;
}

/*
================
FloodAreas

Finds and validates BSP areas via flood fill.
================
*/
int FloodAreas(Tree_t *tree)
{
  Com_DPrintf("--- FloodAreas ---\n");
  FindAreas_r(tree->headnode);
  CheckAreas_r(tree->headnode);
  return Com_DPrintf("%9d areas\n", c_areas);
}

/*
================
FillOutside_r

Recursively marks unoccupied leaves as opaque (solid), then rebuilds the
native three-state opaque summary for each internal node.
================
*/
Node_t *FillOutside_r(Node_t *node)
{
  if ( node->planenum == PLANENUM_LEAF )
  {
    if ( node->occupied )
    {
      ++c_inside;
    }
    else if ( node->opaque == 1 )
    {
      ++c_solid;
    }
    else
    {
      ++c_outside;
      node->opaque = 1;
    }
    return node;
  }

  FillOutside_r(node->children[0]);
  FillOutside_r(node->children[1]);
  if ( node->children[0]->opaque == node->children[1]->opaque )
    node->opaque = node->children[0]->opaque;
  else
    node->opaque = 2;
  return node;
}

/*
================
MarkVisibleSides

Fills outside space and prints fill statistics.
Resets counters, flood-fills the tree, then reports counts.
================
*/
int MarkVisibleSides(Node_t *tree)
{
  c_outside = 0;
  c_inside = 0;
  c_solid = 0;

  Com_DPrintf("--- FillOutside ---\n");
  FillOutside_r(tree);
  Com_DPrintf("%9d solid leafs\n", c_solid);
  Com_DPrintf("%9d leafs filled\n", c_outside);
  return Com_DPrintf("%9d inside leafs\n", c_inside);
}

/*
================
PortalWarning

Prints warning with portal centroid position and brush info.
================
*/
int PortalWarning(Brush_t *brush, Winding_t *w, const char *message)
{
  /* CoD4 0x4305D0 delegates through the common winding diagnostic path. */
  WindingError(0, w, brush->mapInfoIndex, brush->entityNum, brush->brushNum,
               "%s", message);
  return 0;
}

/*
================
SnapNormal

Snaps normal components to 0/1/-1 within epsilon, then normalizes.
================
*/
void SnapNormal(float *normal)
{
  int i;

  /* snap near-axial components to 0, 1, or -1 */
  for ( i = 0; i < 3; i++ )
  {
    if ( normal[i] > -NORMAL_EPSILON && normal[i] < NORMAL_EPSILON )
      normal[i] = 0.0f;
    else if ( normal[i] > 1.0f - NORMAL_EPSILON )
      normal[i] = 1.0f;
    else if ( normal[i] < -1.0f + NORMAL_EPSILON )
      normal[i] = -1.0f;
  }
  VecNormalize(normal);
}

/*
================
FilterBrushIntoTree

Clips portal against brush face edge planes and assigns to face.
================
*/
void FilterBrushIntoTree(Portal_t *portal, Winding_t *brushFace, Brush_t *portalBrush, BrushSide_t *portalFace)
{
  Portal_t *newPortal;
  int i, prevIdx;
  vec3_t edgePlane, edgeDir;
  float edgeDist;
  Winding_t *frontWinding, *backWinding;

  Assert(portalFace != NULL, s_assertDisable_FilterBrushIntoTree);
  Assert(portalBrush != NULL, s_assertDisable_FilterBrushIntoTree);

  if ( !portal->portalFace )
  {
    Assert(!portal->portalBrush, s_assertDisable_FilterBrushIntoTree);

    /* clip portal against each edge plane of the brush face */
    for ( i = 0; i < brushFace->numpoints; i++ )
    {
      prevIdx = (i == 0) ? brushFace->numpoints - 1 : i - 1;
      VectorSubtract(brushFace->points[i], brushFace->points[prevIdx], edgeDir);

      if ( DotProduct210(edgeDir, edgeDir) < ON_EPSILON )
        continue;

      CrossProduct(portal->plane, edgeDir, edgePlane);
      VecNormalize(edgePlane);
      SnapNormal(edgePlane);
      edgeDist = DotProduct102(edgePlane, brushFace->points[i]);
      ClipWindingEpsilon(portal->winding, edgePlane, edgeDist, ON_EPSILON, &frontWinding, &backWinding, 0);

      if ( backWinding )
      {
        if ( frontWinding )
        {
          /* split: front piece becomes a new portal */
          newPortal = AllocPortal();
          memcpy(newPortal, portal, offsetof(Portal_t, nodes));  /* copy plane, onnode, padding, hint */
          newPortal->winding = frontWinding;
          newPortal->portalFace = NULL;
          newPortal->portalBrush = NULL;
          AddPortalToNode(newPortal, portal->nodes[0], portal->nodes[1]);
        }
        FreeWinding(portal->winding);
        portal->winding = backWinding;
      }
      else
      {
        Assert(frontWinding, s_assertDisable_FilterBrushIntoTree);
        FreeWinding(frontWinding);
      }
    }

    portal->portalFace = portalFace;
    portal->portalBrush = portalBrush;
    portalFace->visible = 0;
    return;
  }

  /* portal already assigned — check for overlap */
  if ( portal->portalFace == portalFace )
  {
    Assert(portal->portalBrush == portalBrush, s_assertDisable_FilterBrushIntoTree);
  }
  else
  {
    PortalWarning(portalBrush, portalFace->winding, "first overlapping portal");
    PortalWarning(portal->portalBrush, portal->portalFace->winding, "second overlapping portal");
  }
}

/*
================
FilterBrushIntoTree_r

Recursively walks BSP tree to clip and assign portal to brush face.
================
*/
void FilterBrushIntoTree_r(BrushSide_t *face, Brush_t *brush, float *sidePlanes, Node_t *node, Tree_t *tree)
{
  Node_t *n = node;
  int planeSide, side, i;
  float *plane;
  Winding_t *w;
  Portal_t *p;

  if ( n->planenum == PLANENUM_LEAF )
  {
    if ( n->opaque )
      return;

    p = n->portals;
    if ( !p )
      return;

    plane = MAP_PLANE(face->planenum)->normal;

    /* walk portal linked list, process each matching portal */
    while ( p )
    {
      side = (p->nodes[1] == n);

      if ( !p->nodes[0]->opaque && !p->nodes[1]->opaque && WindingPlaneSide(p->winding, plane, plane[3]) == SIDE_ON )
      {
        /* portal on the face plane — copy winding and clip */
        w = CopyWinding(p->winding);

        /* clip winding against edge planes */
        for ( i = 0; i < face->visibleHull->numpoints && w; i++ )
          ClipWindingByPlane(&w, &sidePlanes[i * 4], sidePlanes[i * 4 + 3], ON_EPSILON);

        /* snap vertices and filter if winding survived */
        if ( w )
        {
          for ( i = 0; i < w->numpoints; i++ )
            SnapPointToPlanes(sidePlanes, face->visibleHull->numpoints, w->points[i], 1.0f, ON_EPSILON);

          FilterBrushIntoTree(p, w, brush, face);
          FreeWinding(w);
        }
      }

      /* advance to next portal in linked list */
      p = p->next[side];
    }
    return;
  }

  /* recurse down both sides of the splitting plane */
  planeSide = WindingPlaneSide(face->winding, MAP_PLANE(n->planenum)->normal, MAP_PLANE(n->planenum)->dist);
  if ( planeSide != SIDE_BACK )
    FilterBrushIntoTree_r(face, brush, sidePlanes, n->children[0], tree);
  if ( planeSide != SIDE_FRONT )
    FilterBrushIntoTree_r(face, brush, sidePlanes, n->children[1], tree);
}

/*
================
BuildSidePlanes

Builds edge side planes for brush faces and assigns portals via BSP walk.
================
*/
void BuildSidePlanes(Brush_t *brushList, Tree_t *tree)
{
  Brush_t *b;
  BrushSide_t *side;
  Winding_t *w;
  int i, j, k, nextIdx, numPts;
  float sidePlanes[MAX_POINTS_ON_WINDING * 4];
  vec3_t edgeDir;
  float *pl;
  double minEps;

  for ( b = brushList; b; b = b->next )
  {
    for ( i = 0; i < b->numSides; i++ )
    {
      side = &b->sides[i];

      /* skip sides with non-negative smoothAngle (not a portal candidate) */
      if ( side->smoothAngleAsInt >= 0 )
        continue;

      w = side->visibleHull;
      if ( !w )
        continue;

      /* build edge side planes from visible hull */
      numPts = w->numpoints;
      for ( j = 0; j < numPts; j++ )
      {
        nextIdx = (j + 1) % numPts;
        VectorSubtract(w->points[nextIdx], w->points[j], edgeDir);

        pl = &sidePlanes[j * 4];
        CrossProduct(edgeDir, MAP_PLANE(side->planenum)->normal, pl);
        VecNormalize(pl);
        pl[3] = DotProduct210(w->points[j], pl);

        /* validate all vertices against this edge plane */
        for ( k = 0; k < numPts; k++ )
        {
          minEps = fabs(pl[3]) * -1e-6;
          if ( minEps > -PLANESIDE_EPSILON )
            minEps = -PLANESIDE_EPSILON;
          Assert(DotProduct021(w->points[k], pl) - pl[3] > minEps, s_assertDisable_BuildSidePlanes);
        }
      }

      /* mark side as unassigned, filter into BSP tree */
      side->visible = 1;
      FilterBrushIntoTree_r(side, b, sidePlanes, tree->headnode, tree);

      if ( side->visible )
        PortalWarning(b, side->visibleHull, "is not at a BSP split, is floating, or is next to a solid brush");

      side->value = CELLNUM_UNKNOWN;
      side->culled = CELLNUM_UNKNOWN;
      side->ownerBrush = b;
    }
  }
}

/*
================
InitBspTreeNodes

Recursively initializes BSP tree nodes with cell numbers.
Sets occupied to INT_MAX and cellnum to CELLNUM_OPAQUE or CELLNUM_UNKNOWN.
================
*/
int InitBspTreeNodes(Node_t *node)
{
  Node_t *cur;

  cur = node;
  node->occupied = INT_MAX;
  node->cellnum = node->opaque ? CELLNUM_OPAQUE : CELLNUM_UNKNOWN;

  for ( ; cur->planenum != PLANENUM_LEAF; cur->occupied = INT_MAX )
  {
    Assert(cur->children[0]->brushlist.parentNode == cur, s_assertDisable_InitBspTreeNodes);
    Assert(cur->children[1]->brushlist.parentNode == cur, s_assertDisable_InitBspTreeNodes);
    InitBspTreeNodes(cur->children[0]);
    cur = cur->children[1];
    cur->cellnum = cur->opaque ? CELLNUM_OPAQUE : CELLNUM_UNKNOWN;
  }

  return cur->opaque;
}

/*
================
BuildBspTree_r

Recursively flood-fills BSP tree to mark opaque leaves.
Marks each reachable non-opaque leaf as opaque and sets cellnum to -1.
Walks through portals to reach neighboring leaves.
================
*/
int BuildBspTree_r(Node_t *node)
{
  Portal_t *p;
  int s;

  if ( node->cellnum == CELLNUM_OPAQUE || node->opaque )
    return -1;

  node->opaque = 1;
  node->cellnum = CELLNUM_OPAQUE;

  for ( p = node->portals; p; p = p->next[s] )
  {
    s = (p->nodes[1] == node);
    if ( !p->portalFace )
      BuildBspTree_r(p->nodes[!s]);
  }
  return -1;
}

/*
================
SplitBrushList

Recursively assigns cell numbers to leaves via portal flood fill.
Assigns the current cell number to each reachable leaf and expands the cell's
bounding box. Detects leaks when a portal connects back to the same cell.
================
*/
void SplitBrushList(Node_t *node, int depth, Tree_t *tree)
{
  Node_t *n = node;
  BspCell_t *cell;
  Portal_t *p;
  int side;

  if ( n->cellnum == numBSPCells || n->opaque )
    return;

  Assert(n->cellnum == CELLNUM_UNKNOWN, s_assertDisable_SplitBrushList);
  n->cellnum = numBSPCells;

  /* expand cell bounding box to include this node's bounds */
  cell = &bspCells[n->cellnum];
  AddBoundsToBounds(n->mins, n->maxs, cell->mins, cell->maxs);

  /* walk portals and recurse into neighbors */
  for ( p = n->portals; p; p = p->next[side] )
  {
    side = (p->nodes[1] == node);
    if ( IsTinyWinding(p->winding) )
      continue;

    if ( p->portalFace )
    {
      /* leak: portal connects back to same cell */
      if ( p->nodes[!side]->cellnum == n->cellnum )
        PortalLeakFile(tree, p);
    }
    else
    {
      SplitBrushList(p->nodes[!side], depth + 1, tree);
    }
  }
}

/*
================
PartitionBrushes

Partitions BSP tree leaves into numbered cells.
Traverses to leaf nodes; when finding an unassigned (CELLNUM_UNKNOWN),
non-opaque leaf, starts a new cell and flood-fills via SplitBrushList.
================
*/
int PartitionBrushes(Node_t *node, Tree_t *tree)
{
  Node_t *n;

  /* recurse down to leaves */
  for ( n = node; n->planenum != PLANENUM_LEAF; n = n->children[1] )
    PartitionBrushes(n->children[0], tree);

  /* start a new cell from unassigned non-opaque leaves */
  if ( n->cellnum == CELLNUM_UNKNOWN && !n->opaque )
  {
    ClearBounds(bspCells[numBSPCells].mins, bspCells[numBSPCells].maxs);
    SplitBrushList(n, 1, tree);
    return ++numBSPCells;
  }

  return n->opaque;
}

/*
================
FreeBspTree

Walks BSP tree and checks portal cell connections.
For each non-opaque leaf, asserts cellnum is assigned, then checks portals
for cross-cell connections and issues warnings.
================
*/
void FreeBspTree(Node_t *node)
{
  Node_t *n;
  Portal_t *p;
  BrushSide_t *face;
  int side;

  /* recurse down to leaves */
  for ( n = node; n->planenum != PLANENUM_LEAF; n = n->children[1] )
    FreeBspTree(n->children[0]);

  if ( n->opaque )
    return;

  /* check portal cell connections */
  Assert(n->cellnum != CELLNUM_UNKNOWN, s_assertDisable_FreeBspTree);
  for ( p = n->portals; p; p = p->next[side] )
  {
    side = (p->nodes[1] == n);
    face = p->portalFace;

    /* warn about portals connecting same cell through a face */
    if ( face && p->nodes[0]->cellnum == p->nodes[1]->cellnum && !face->visible )
    {
      face->visible = 1;
      PortalWarning(p->portalBrush, face->visibleHull, "has the same cell on both sides, portal ignored");
    }
  }
}

/*
================
FreeBspTree_r

Builds per-cell portal connection lists.
For each non-opaque leaf, walks portals and builds a linked list of
portal connections per cell (stored in g_cellPortalLinks array).
================
*/
void FreeBspTree_r(Node_t *node)
{
  Node_t *n;
  BrushSide_t *face;
  CellPortalLink_t *link, *newLink;
  Portal_t *p;
  BspCell_t *cell;
  int side;

  /* recurse down to leaves */
  for ( n = node; n->planenum != PLANENUM_LEAF; n = n->children[1] )
    FreeBspTree_r(n->children[0]);

  if ( n->opaque )
    return;

  Assert(n->cellnum >= 0, s_assertDisable_FreeBspTree_r);
  cell = &bspCells[n->cellnum];

  for ( p = n->portals; p; p = p->next[side] )
  {
    side = (p->nodes[1] == n);
    face = p->portalFace;

    if ( !face || face->visible )
      continue;

    /* check if this portal connection already exists */
    for ( link = g_cellPortalLinks[n->cellnum]; link; link = link->next )
    {
      if ( link->face == face )
        break;
    }

    if ( !link )
    {
      /* add new portal connection */
      newLink = malloc(sizeof(CellPortalLink_t));
      newLink->face = face;
      newLink->neighbor = p->nodes[!side];
      newLink->next = g_cellPortalLinks[n->cellnum];
      g_cellPortalLinks[n->cellnum] = newLink;
      ++cell->portalCount;

      Assert(p->nodes[0]->cellnum != CELLNUM_UNKNOWN, s_assertDisable_FreeBspTree_r);
      Assert(p->nodes[1]->cellnum != CELLNUM_UNKNOWN, s_assertDisable_FreeBspTree_r);

      if ( face->value == CELLNUM_UNKNOWN )
      {
        Assert(face->culled == CELLNUM_UNKNOWN, s_assertDisable_FreeBspTree_r);
        face->value = p->nodes[0]->cellnum;
        face->culled = p->nodes[1]->cellnum;
      }
      else
      {
        Assert(face->culled != CELLNUM_UNKNOWN, s_assertDisable_FreeBspTree_r);
        if ( face->value != p->nodes[0]->cellnum || face->culled != p->nodes[1]->cellnum )
        {
          PortalWarning(face->ownerBrush, face->winding, "portal sees into two cells (aka portal t-junction)");
          ++s_portalErrorCount;
        }
      }
    }
  }
}

/*
================
SplitBrush

Records an occluder edge between two adjacent face planes.
================
*/
void SplitBrush(BspOccluder_t *occluder, char planeIdx1, char planeIdx2)
{
  BspOccluderEdge_t *edges;
  BspOccluderEdge_t *e;
  char planeIndex;
  int planeIndexFull;
  int i;

  edges = bspOccluderEdges;

  /* compute local plane index for this occluder */
  planeIndexFull = numBSPOccluderPlanes - occluder->startPlanes - 1;
  planeIndex = planeIndexFull;
  if ( (unsigned char)planeIndexFull != planeIndexFull && !s_assertDisable_SplitBrush )
  {
    va("%s\n\t", "(planeIndex == numOccluderPlanes - occluder->firstPlane - 1)", planeIndexFull);
    Assert(0, s_assertDisable_SplitBrush);
  }

  /* search existing edges for matching reverse pair */
  for ( i = occluder->startEdges; i < numBSPOccluderEdges; i++ )
  {
    if ( edges[i].facePlane0 == planeIdx2 && edges[i].facePlane1 == planeIdx1 )
      break;
  }

  if ( i >= numBSPOccluderEdges )
  {
    /* no match found — add new edge */
    e = &edges[numBSPOccluderEdges];
    e->facePlane0 = planeIdx1;
    e->facePlane1 = planeIdx2;
    e->planeRef0 = planeIndex;
    e->planeRef1 = planeIndex;
    numBSPOccluderEdges++;
  }
  else
  {
    /* found existing reverse edge — update second plane ref */
    e = &edges[i];
    Assert(e->planeRef1 == e->planeRef0, s_assertDisable_SplitBrush);
    Assert(planeIndex != e->planeRef0, s_assertDisable_SplitBrush);
    e->planeRef1 = planeIndex;
  }
}

/*
================
BspTreeForMap

Builds occluder data from entity brushes (planes, verts, edges, cell tests).
================
*/
int BspTreeForMap(Entity_t *entityData)
{
  Brush_t *brush;
  BspOccluder_t *occ;
  BspOccluderEdge_t *edges;
  BrushSide_t *face;
  Winding_t *w;
  int i, j, k, vertIdx;
  int startEdges, startVerts;
  char vertIndices[MAX_COLLISION_SIDES];
  float planeVec[5];
  vec3_t occMins[MAX_MAP_OCCLUDERS];
  vec3_t occMaxs[MAX_MAP_OCCLUDERS];

  edges = bspOccluderEdges;

  Assert(!g_currentEntityIndex, s_assertDisable_BspTreeForMap);
  Assert(!numBSPOccluders, s_assertDisable_BspTreeForMap);
  Assert(!numBSPOccluderIndexes, s_assertDisable_BspTreeForMap);

  /* phase 1: build occluder planes, verts, and edges from brushes */
  for ( brush = entityData->brushes; brush; brush = brush->next )
  {
    if ( !brush->isOccluder )
      continue;

    if ( brush->numSides > MAX_COLLISION_SIDES )
      Com_Error("more than 256 faces on occluder map %s entity %i brush %in", MapInfo_GetName(brush->mapInfoIndex), brush->entityNum, brush->brushNum);
    if ( numBSPOccluders == MAX_MAP_OCCLUDERS )
      Com_Error("MAX_MAP_OCCLUDERS");

    occ = &bspOccluders[numBSPOccluders];
    ClearBounds(occMins[numBSPOccluders], occMaxs[numBSPOccluders]);
    occ->startPlanes = numBSPOccluderPlanes;
    occ->startEdges = numBSPOccluderEdges;
    occ->startVerts = numBSPPortalVerts;

    /* process each non-bevel face */
    for ( i = 0; i < brush->numSides; i++ )
    {
      face = &brush->sides[i];
      if ( face->bevel || !face->winding )
        continue;

      w = face->winding;
      bspOccluderPlanes[numBSPOccluderPlanes] = face->planenum;
      numBSPOccluderPlanes++;
      mapplanes[face->planenum].flags = 1;

      if ( w->numpoints > MAX_COLLISION_SIDES )
        Com_Error("more than 256 vertices on a face of occluder map %s entity %i brush %in",
                  MapInfo_GetName(brush->mapInfoIndex), brush->entityNum, brush->brushNum);

      /* add unique vertices and build edges */
      for ( j = 0; j < w->numpoints; j++ )
      {
        /* search for existing vertex */
        for ( vertIdx = occ->startVerts; vertIdx < numBSPPortalVerts; vertIdx++ )
        {
          if ( VectorCompareEpsilon(bspPortalVerts[vertIdx], w->points[j], PLANESIDE_EPSILON, 3) )
            break;
        }

        if ( vertIdx >= numBSPPortalVerts )
        {
          /* add new unique vertex */
          if ( numBSPPortalVerts - occ->startVerts > MAX_COLLISION_SIDES - 1 )
            Com_Error("more than 256 unique vertices in occluder map %s entity %i brush %i\n",
                      MapInfo_GetName(brush->mapInfoIndex), brush->entityNum, brush->brushNum);
          VectorCopy(w->points[j], bspPortalVerts[numBSPPortalVerts]);
          AddPointToBounds(bspPortalVerts[numBSPPortalVerts], occMins[numBSPOccluders], occMaxs[numBSPOccluders]);
          numBSPPortalVerts++;
        }

        vertIndices[j] = vertIdx;
        if ( j > 0 )
          SplitBrush(occ, vertIndices[j - 1], vertIdx);
      }

      /* close the edge loop */
      SplitBrush(occ, vertIndices[w->numpoints - 1], vertIndices[0]);
    }

    /* finalize occluder counts */
    startEdges = numBSPOccluderEdges;
    startVerts = (short)numBSPPortalVerts;
    occ->numNewPlanes = numBSPOccluderPlanes - (unsigned short)occ->startPlanes;
    occ->numNewEdges = startEdges - (unsigned short)occ->startEdges;
    occ->numNewVerts = startVerts - (unsigned short)occ->startVerts;
    numBSPOccluders++;

    /* verify all edges are matched */
    for ( k = occ->startEdges; k < startEdges; k++ )
    {
      if ( edges[k].planeRef0 == edges[k].planeRef1 )
        Com_Error("unmatched edge in occluder map %s entity %i brush %i\n", MapInfo_GetName(brush->mapInfoIndex), brush->entityNum, brush->brushNum);
    }
  }

  /* Phase 2 assigns the legacy flat occluder-index lump.  CoD4 keeps the
     compiler-only reflection-probe list at BspCell +44; no occluder range is
     stored in that native cell record. */
  for ( i = 0; i < numBSPCells; i++ )
  {
    for ( j = 0; j < numBSPOccluders; j++ )
    {
      /* AABB overlap test */
      if ( occMins[j][0] >= (double)bspCells[i].maxs[0] )
        continue;
      if ( occMaxs[j][0] <= (double)bspCells[i].mins[0] )
        continue;
      if ( occMins[j][1] >= (double)bspCells[i].maxs[1] )
        continue;
      if ( occMaxs[j][1] <= (double)bspCells[i].mins[1] )
        continue;
      if ( occMins[j][2] >= (double)bspCells[i].maxs[2] )
        continue;
      if ( occMaxs[j][2] <= (double)bspCells[i].mins[2] )
        continue;

      /* plane-side test: skip if cell is fully in front of any occluder plane */
      occ = &bspOccluders[j];
      for ( k = occ->startPlanes; k < numBSPOccluderPlanes; k++ )
      {
        int planeSlot = bspOccluderPlanes[k];
        VectorCopy(mapplanes[planeSlot].normal, planeVec);
        planeVec[3] = mapplanes[planeSlot].dist;
        SetPlaneSignbits(planeVec);
        if ( BoxOnPlaneSide(bspCells[i].mins, bspCells[i].maxs, planeVec) == 1 )
          break;
      }

      /* cell overlaps all planes — add occluder to cell */
      if ( k >= numBSPOccluderPlanes )
      {
        if ( numBSPOccluderIndexes == MAX_MAP_OCCLUDER_INDEXES )
          Com_Error("MAX_MAP_OCCLUDER_INDEXES");
        bspOccluderIndexes[numBSPOccluderIndexes] = j;
        numBSPOccluderIndexes++;
      }
    }

  }

  return numBSPCells;
}

/*
================
SelectSplitPlane

Determines which side of a plane a portal is on (returns planeIdx or planeIdx^1).
================
*/
int SelectSplitPlane(int planeIdx, Node_t *node, float epsilon)
{
  Portal_t *p;
  Plane_t *plane;
  Winding_t *w;
  int i;
  float dot;

  for ( p = node->portals; p; p = p->next[p->nodes[1] == node] )
  {
    w = p->winding;
    plane = MAP_PLANE(planeIdx);

    for ( i = 0; i < w->numpoints; i++ )
    {
      dot = DotProduct021(w->points[i], plane->normal) - plane->dist;
      if ( dot < -epsilon )
        return planeIdx;
      if ( dot > epsilon )
        return planeIdx ^ 1;
    }
  }
  return -1;
}

/*
================
SelectSplitPlane_r

Iteratively calls SelectSplitPlane with decreasing epsilon until
a valid plane side is determined, or errors if epsilon becomes too small.
================
*/
int SelectSplitPlane_r(BrushSide_t *face, Node_t *node)
{
  int planeIdx;
  int result;
  vec3_t center;
  float epsilon;

  planeIdx = face->planenum;

  #define SPLIT_EPSILON_INIT 0.125f
  #define SPLIT_EPSILON_MIN  0.0005f
  epsilon = SPLIT_EPSILON_INIT;
  for ( ;; )
  {
    result = SelectSplitPlane(planeIdx, node, epsilon);
    if ( result != -1 )
      return result;

    epsilon *= 0.5f;
    if ( epsilon <= SPLIT_EPSILON_MIN )
    {
      WindingCenter(face->winding, center);
      Com_Error(
        "couldn't determine plane for portal; shouldn't happen unless there is a bsp leaf with no volume\n"
             "portal center = (%g %g %g)\n",
        center[0], center[1], center[2]);
      return -1;
    }
  }
}

/*
================
GetLeafBrushes

Builds portal vertex/plane data and leaf brush lists for all cells.
================
*/
int GetLeafBrushes(void)
{
  CellPortalLink_t *link;
  BrushSide_t *face;
  Node_t *neighbor;
  Winding_t *w;
  int cellIdx, portalIdx, vertIdx, i;
  int planeResult;
  int leafBrushIdx, bitShift;

  numBSPPortals = 0;
  numBSPCullGroupIndexes = 0;
  leafBrushIdx = 0;

  for ( cellIdx = 0; cellIdx < numBSPCells; cellIdx++ )
  {
    bspCells[cellIdx].firstPortal = numBSPPortals;

    /* build portal data from cell portal links */
    for ( link = g_cellPortalLinks[cellIdx]; link; link = link->next )
    {
      face = link->face;
      neighbor = link->neighbor;

      w = face->visibleHull;
      RemoveColinearPoints(w);

      /* determine plane orientation */
      portalIdx = numBSPPortals++;
      planeResult = SelectSplitPlane_r(face, neighbor);
      planeResult ^= 1;
      bspPortals[portalIdx].planeIdx = planeResult;
      bspPortals[portalIdx].cellIdx = neighbor->cellnum;
      bspPortals[portalIdx].firstVert = numBSPPortalVerts;
      bspPortals[portalIdx].numVerts = w->numpoints;
      mapplanes[planeResult].flags = 1;

      vertIdx = numBSPPortalVerts;
      if ( planeResult == face->planenum )
      {
        for ( i = 0; i < w->numpoints; i++ )
        {
          VectorCopy(w->points[i], bspPortalVerts[vertIdx]);
          vertIdx++;
        }
      }
      else
      {
        for ( i = w->numpoints - 1; i >= 0; i-- )
        {
          VectorCopy(w->points[i], bspPortalVerts[vertIdx]);
          vertIdx++;
        }
      }
      numBSPPortalVerts = vertIdx;
    }

    /* build cull group index list for this cell */
    bspCells[cellIdx].firstCullGroup = leafBrushIdx;
    bspCells[cellIdx].cullGroupCount = 0;
    bitShift = cellIdx << CELL_BITMAP_SHIFT;
    for ( i = 0; i < numBSPCullGroups; i++ )
    {
      if ( (unsigned char)(1 << ((bitShift + i) & 7)) & (unsigned char)g_brushSideBitmap[(bitShift + i) >> 3] )
      {
        bspCullGroupIndexes[leafBrushIdx++] = i;
        bspCells[cellIdx].cullGroupCount++;
      }
    }
    numBSPCullGroupIndexes = leafBrushIdx;
  }

  if ( s_portalErrorCount )
  {
    Com_Error("aborting due to %i bad portals", s_portalErrorCount);
    return 0;
  }

  return numBSPCells;
}

/*
================
LeafNode

Rebuilds BSP tree and generates leaf brush data for entity.
================
*/
int LeafNode(Entity_t *entityData, Node_t *node)
{
  memset(g_cellPortalLinks, 0, sizeof(g_cellPortalLinks));
  Assert(s_portalErrorCount == 0, s_assertDisable_FreeBspTree_r);
  FreeBspTree_r(node);
  BspTreeForMap(entityData);
  return GetLeafBrushes();
}

/*
================
CreateLeafNode

Remaps portal plane indices from map planes to BSP planes via planeMap.
================
*/
int CreateLeafNode(int *planeMap)
{
  int i;
  int mappedPlane;

  Assert(planeMap, s_assertDisable_CreateLeafNode);

  for ( i = 0; i < numBSPPortals; i++ )
  {
    Assert(bspPortals[i].planeIdx >= 0 && bspPortals[i].planeIdx < g_numMapPlanes, s_assertDisable_CreateLeafNode);
    mappedPlane = planeMap[bspPortals[i].planeIdx];
    Assert(mappedPlane >= 0 && mappedPlane < numBSPPlanes, s_assertDisable_CreateLeafNode);
    bspPortals[i].planeIdx = mappedPlane;
  }
  return numBSPPortals;
}

/*
================
CreateNode

Remaps occluder plane indices from map planes to BSP planes via planeMap.
================
*/
int CreateNode(int *planeMap)
{
  int i;
  int mappedPlane;

  Assert(planeMap, s_assertDisable_CreateNode);

  for ( i = 0; i < numBSPOccluderPlanes; i++ )
  {
    Assert(bspOccluderPlanes[i] >= 0 && bspOccluderPlanes[i] < g_numMapPlanes, s_assertDisable_CreateNode);
    mappedPlane = planeMap[bspOccluderPlanes[i]];
    Assert(mappedPlane >= 0 && mappedPlane < numBSPPlanes, s_assertDisable_CreateNode);
    bspOccluderPlanes[i] = mappedPlane;
  }
  return numBSPOccluderPlanes;
}

/*
================
NodeForBounds

Recursively determines cell number for each BSP node.
If both children have the same cell, the node inherits it.
Otherwise, the node gets CELLNUM_UNKNOWN (-2).
================
*/
int NodeForBounds(Node_t *node)
{
  Node_t *n = node;
  int cellNum;

  if ( n->planenum != PLANENUM_LEAF )
  {
    cellNum = NodeForBounds(n->children[0]);
    if ( cellNum == NodeForBounds(n->children[1]) )
    {
      n->cellnum = cellNum;
      return n->cellnum;
    }
    n->cellnum = CELLNUM_UNKNOWN;
  }
  return n->cellnum;
}

/*
================
PortalizeNode

Main portal generation: init nodes, partition cells, validate tree.
================
*/
int PortalizeNode(Tree_t *tree, int skipBuild)
{
  Node_t *rootNode;
  int cell0;
  int cell1;

  numBSPCells = 0;

  InitBspTreeNodes(tree->headnode);

  if ( !skipBuild )
  {
    tree->outside_node.cellnum = CELLNUM_UNKNOWN;
    BuildBspTree_r(&tree->outside_node);
  }

  PartitionBrushes(tree->headnode, tree);
  FreeBspTree(tree->headnode);

  /* if root is internal, propagate cell number from children */
  rootNode = tree->headnode;
  if ( rootNode->planenum != PLANENUM_LEAF )
  {
    cell0 = NodeForBounds(rootNode->children[0]);
    cell1 = NodeForBounds(rootNode->children[1]);
    rootNode->cellnum = (cell0 == cell1) ? cell0 : CELLNUM_UNKNOWN;
  }

  if ( !numBSPCells )
    Com_Error("no cells in map; map is probably empty or has no structural brushes\n");
  return Com_DPrintf("%5i unique cells\n", numBSPCells);
}

/*
================
WriteBrushFace

Writes 3 face points and material to .map file.
================
*/
int WriteBrushFace(float *verts, FILE *file, const char *material)
{
  int i;
  int randOffset;
  int randVal;

  /* write 3 face points */
  for ( i = 0; i < 3; i++ )
  {
    /* UCRT prints -0.0 as "-0" while old MSVC CRT printed "0" */
    float fx = verts[i * 3 + 0];
    float fy = verts[i * 3 + 1];
    float fz = verts[i * 3 + 2];
    if (fx == 0.0f) fx = 0.0f;
    if (fy == 0.0f) fy = 0.0f;
    if (fz == 0.0f) fz = 0.0f;
    fprintf(file, "( %g %g %g ) ", fx, fy, fz);
  }

  randOffset = rand() & PORTAL_RAND_MASK;
  randVal = rand();
  return fprintf(file, "%s %i %i 0 0.25 0.25 0 0\n", material, randVal & PORTAL_RAND_MASK, randOffset);
}

/*
================
WritePortalBrush

Writes a portal as a brush (front face + side faces + back face) to .map file.
================
*/
int WritePortalBrush(FILE *file, float *origin, Winding_t *w, const char *material)
{
  int i, nextIdx;
  float faceVerts[4][3];

  fprintf(file, "{\n");

  /* front face: first 3 winding points in reverse order */
  VectorCopy(w->points[2], faceVerts[0]);
  VectorCopy(w->points[1], faceVerts[1]);
  VectorCopy(w->points[0], faceVerts[2]);
  VectorCopy(origin, faceVerts[3]);
  WriteBrushFace((float *)faceVerts, file, material);

  /* side faces: each edge extruded toward origin */
  for ( i = 0; i < w->numpoints; i++ )
  {
    nextIdx = (i + 1) % w->numpoints;
    VectorCopy(w->points[i], faceVerts[0]);
    VectorCopy(w->points[nextIdx], faceVerts[1]);
    VectorAdd(w->points[i], origin, faceVerts[2]);
    WriteBrushFace((float *)faceVerts, file, "portal_nodraw");
  }

  /* back face: first 3 winding points offset by origin */
  VectorAdd(w->points[0], origin, faceVerts[0]);
  VectorAdd(w->points[1], origin, faceVerts[1]);
  VectorAdd(w->points[2], origin, faceVerts[2]);
  WriteBrushFace((float *)faceVerts, file, "portal_nodraw");

  return fprintf(file, "}\n");
}

/*
================
WritePortalFile_Node

Recursively writes portal brushes for BSP tree to .map file.
Renamed to WritePortalFile_Node to avoid conflict with WritePortalFile(int **).
================
*/
void WritePortalFile_Node(Node_t *node, FILE *file)
{
  Node_t *n;
  Portal_t *p;
  int side;
  const char *matName;

  /* recurse down to leaves */
  for ( n = node; n->planenum != PLANENUM_LEAF; n = n->children[1] )
    WritePortalFile_Node(n->children[0], file);

  /* write each portal on side 0 (avoid double-writing) */
  for ( p = n->portals; p; p = p->next[side] )
  {
    side = (p->nodes[1] == n);
    if ( side )
      continue;

    /* choose material based on portal type */
    if ( p->portalFace )
      matName = "portal";
    else if ( p->nodes[0]->opaque || p->nodes[1]->opaque )
      matName = "portal_debug_solid";
    else
      matName = "portal_debug_trans";

    WritePortalBrush(file, p->plane, p->winding, matName);
  }
}

/*
================
WritePortalFile_r

Writes _portals.map debug file with all portal brushes.
================
*/
void WritePortalFile_r(Tree_t *tree)
{
  FILE *file;
  char filepath[MAX_OS_PATH];

  Assert(tree, s_assertDisable_WritePortalFile_Node);
  sprintf(filepath, "%s_portals.map", g_outputBasePath);
  file = fopen(filepath, "w");
  if ( !file )
    return;

  fprintf(file, "iwmap %i\n{\n\"classname\" \"worldspawn\"\n", IWMAP_VERSION);
  WritePortalFile_Node(tree->headnode, file);
  fprintf(file, "}\n");
  fclose(file);
}

/*
================
PortalizeWorld

Main entry point for portal generation in the BSP tree.
Builds side planes, partitions cells, optionally writes debug portal file.
================
*/
void PortalizeWorld(Brush_t *brushList, Tree_t *tree, int skipBuild)
{
  Com_DPrintf("----- Building Portals and Cells -----\n");

  BuildSidePlanes(brushList, tree);
  PortalizeNode(tree, skipBuild);

  if ( debugPortals )
    WritePortalFile_r(tree);
}

/*
================
CreatePortalWinding

Creates a winding for a portal by clipping against adjacent planes.
Starts with a huge base winding (±131072 units) and clips it against all
brush sides that bound the node, producing a winding that fits the actual geometry.
================
*/
Winding_t *CreatePortalWinding(Node_t *node)
{
  Node_t *currentNode;
  Node_t *walkNode;
  Winding_t *w;
  Plane_t *plane;
  vec3_t flippedNormal;

  currentNode = node;
  plane = MAP_PLANE(node->planenum);
  w = BaseWindingForPlane(plane->normal, plane->dist);

  /* walk up parent chain, clipping base winding against each ancestor's plane */
  #define BASE_WINDING_EPSILON 0.001f
  for ( walkNode = currentNode->brushlist.parentNode; walkNode && w; walkNode = walkNode->brushlist.parentNode )
  {
    plane = MAP_PLANE(walkNode->planenum);

    if ( walkNode->children[0] == currentNode )
      ClipWindingByPlane(&w, plane->normal, plane->dist, BASE_WINDING_EPSILON);
    else
    {
      VectorScale(plane->normal, -1, flippedNormal);
      ClipWindingByPlane(&w, flippedNormal, -plane->dist, BASE_WINDING_EPSILON);
    }

    currentNode = walkNode;
  }

  return w;
}

/*
================
MakeNodePortal

Creates a portal for a BSP node by clipping the base winding against
all existing portals. If the winding survives, links it to both children.
================
*/
void MakeNodePortal(Node_t *node)
{
  Winding_t *w;
  Portal_t *p;
  Portal_t *newPortal;
  float plane[4];
  int s;

  /* create base winding for this node's splitting plane */
  w = CreatePortalWinding(node);

  /* clip against each portal in the node's portal list */
  for ( p = node->portals; p && w; p = p->next[s] )
  {
    if ( p->nodes[0] == node )
    {
      /* front side — use plane directly */
      VectorCopy(p->plane, plane);
      s = 0;
      plane[3] = p->plane[3];
    }
    else if ( p->nodes[1] == node )
    {
      /* back side — flip the plane */
      s = 1;
      VectorScale(p->plane, -1, plane);
      plane[3] = -p->plane[3];
    }
    else
    {
      s = -1;
      plane[3] = 0.0;
      Com_Error("CutNodePortals_r: mislinked portal");
    }

    ClipWindingByPlane(&w, plane, plane[3], ON_EPSILON);
  }

  if ( !w )
    return;

  if ( IsTinyWinding(w) )
  {
    ++c_tinyportals;
    FreeWinding(w);
    return;
  }

  /* allocate and initialize new portal */
  newPortal = AllocPortal();
  memcpy(newPortal, MAP_PLANE(node->planenum), sizeof(Plane_t));
  newPortal->hint = node;
  newPortal->winding = w;

  AddPortalToNode(newPortal, node->children[0], node->children[1]);
}

/*
================
SplitNodePortals

Distributes portals from a node to its front and back children.
For each portal on the node, clips it against the node's splitting plane.
The front winding goes to the front child, back winding to back child.
Tiny windings are discarded and counted. When both sides survive, a new
portal is allocated for the back side.
================
*/
void SplitNodePortals(Node_t *node)
{
  Portal_t *p, *next_portal, *new_portal;
  Node_t *f, *b, *other_node;
  int side;
  float *plane;
  Winding_t *frontwinding, *backwinding;

  plane = MAP_PLANE(node->planenum)->normal;
  f = node->children[0];
  b = node->children[1];

  for ( p = node->portals; p; p = next_portal )
  {
    /* determine which side of the portal this node is on */
    if ( p->nodes[0] == node )
      side = 0;
    else if ( p->nodes[1] == node )
      side = 1;
    else
    {
      side = -1;
      Com_Error("SplitNodePortals: mislinked portal");
    }
    next_portal = p->next[side];
    other_node = p->nodes[!side];
    RemovePortalFromNode(p, p->nodes[0]);
    RemovePortalFromNode(p, p->nodes[1]);

    /* clip portal winding against the splitting plane */
    ClipWindingEpsilon(p->winding, plane, plane[3], PLANESIDE_EPSILON, &frontwinding, &backwinding, 0);

    /* discard tiny front winding */
    if ( frontwinding && IsTinyWinding(frontwinding) )
    {
      if ( !f->tinyportals )
        VectorCopy(frontwinding->points[0], f->referencepoint);
      ++f->tinyportals;
      if ( !other_node->tinyportals )
        VectorCopy(frontwinding->points[0], other_node->referencepoint);
      ++other_node->tinyportals;
      FreeWinding(frontwinding);
      frontwinding = NULL;
      ++c_tinyportals;
    }

    /* discard tiny back winding */
    if ( backwinding && IsTinyWinding(backwinding) )
    {
      if ( !b->tinyportals )
        VectorCopy(backwinding->points[0], b->referencepoint);
      ++b->tinyportals;
      if ( !other_node->tinyportals )
        VectorCopy(backwinding->points[0], other_node->referencepoint);
      ++other_node->tinyportals;
      FreeWinding(backwinding);
      backwinding = NULL;
      ++c_tinyportals;
    }

    if ( !frontwinding && !backwinding )
      continue;

    if ( !frontwinding )
    {
      /* only back survived */
      FreeWinding(backwinding);
      if ( p->nodes[0] || p->nodes[1] )
        Com_Error("AddPortalToNode: already included");
      if ( side == 0 )
        AddPortalToNode(p, b, other_node);
      else
        AddPortalToNode(p, other_node, b);
      continue;
    }

    if ( !backwinding )
    {
      /* only front survived */
      FreeWinding(frontwinding);
      if ( p->nodes[0] || p->nodes[1] )
        Com_Error("AddPortalToNode: already included");
      if ( side == 0 )
        AddPortalToNode(p, f, other_node);
      else
        AddPortalToNode(p, other_node, f);
      continue;
    }

    /* both sides survived — allocate new portal for back winding */
    new_portal = AllocPortal();
    memcpy(new_portal, p, sizeof(Portal_t));
    new_portal->winding = backwinding;
    FreeWinding(p->winding);
    p->winding = frontwinding;

    if ( p->nodes[0] || p->nodes[1] )
      Com_Error("AddPortalToNode: already included");
    if ( new_portal->nodes[0] || new_portal->nodes[1] )
      Com_Error("AddPortalToNode: already included");

    if ( side == 0 )
    {
      AddPortalToNode(p, f, other_node);
      AddPortalToNode(new_portal, b, other_node);
    }
    else
    {
      AddPortalToNode(p, other_node, f);
      AddPortalToNode(new_portal, other_node, b);
    }
  }

  node->portals = NULL;
}

/*
================
ValidateNodeVolumes

Recursively validates BSP node bounding boxes. Warns if a node
has no volume (mins >= maxs) or unbounded volume (beyond map limits).
Then creates portals and splits them, recursing into children.
================
*/
void ValidateNodeVolumes(Node_t *node)
{
  Node_t *n;
  int i;

  for ( n = node; ; n = n->children[1] )
  {
    CalcNodeBounds(n);

    if ( n->mins[0] >= n->maxs[0] )
    {
      Com_Printf("WARNING: node without a volume\n");
      Com_Printf("node has %d tiny portals\n", n->tinyportals);
      Com_Printf("node reference point %1.2f %1.2f %1.2f\n",
        n->referencepoint[0], n->referencepoint[1], n->referencepoint[2]);
    }

    /* check if node extends beyond map bounds */
    for ( i = 0; i < 3; i++ )
    {
      if ( n->mins[i] < MIN_WORLD_COORD || n->maxs[i] > MAX_WORLD_COORD )
      {
        Com_Printf("WARNING: node with unbounded volume\n");
        break;
      }
    }

    if ( n->planenum == PLANENUM_LEAF )
      break;

    MakeNodePortal(n);
    SplitNodePortals(n);
    ValidateNodeVolumes(n->children[0]);
  }
}

/*
================
ProcessBspTree

Main BSP tree construction entry point. Builds the BSP tree
by recursively splitting the brush list, then validates node volumes.
================
*/
int ProcessBspTree(Tree_t *treeRoot)
{
  Com_DPrintf("----- MakeTreePortals -----\n");
  MakeTreePortals(treeRoot);
  ValidateNodeVolumes(treeRoot->headnode);
  return Com_DPrintf("%6d tiny portals\n", c_tinyportals);
}

/*
================
PlaceOccupant

Walks the BSP tree from headnode to find the leaf containing origin,
then flood-fills through portals if the leaf is not opaque.
================
*/
int PlaceOccupant(Node_t *headnode, float *origin, Entity_t *occupant)
{
  Node_t *n;
  Plane_t *plane;

  /* walk BSP tree to find the leaf containing origin */
  n = headnode;
  while ( n->planenum != PLANENUM_LEAF )
  {
    plane = MAP_PLANE(n->planenum);
    if ( DotProduct120(origin, plane->normal) - plane->dist < 0.0 )
      n = n->children[1];
    else
      n = n->children[0];
  }

  if ( n->opaque )
    return 0;

  n->occupant = occupant;
  Portal_Passable(n, 1);
  return 1;
}

/*
================
FloodEntities

Validates the BSP tree by checking if entities are reachable from valid space.
Iterates through all non-worldspawn entities, tests if they're in open space.
Returns 1 if valid (no leaks), 0 if invalid.
================
*/
int FloodEntities(Tree_t *tree)
{
  int i;
  Entity_t *e;
  const char *classname;
  vec3_t origin;
  Node_t *headnode;
  int inside;

  headnode = tree->headnode;
  Com_DPrintf("--- FloodEntities ---\n");

  tree->outside_node.occupied = 0;
  inside = 0;
  c_floodedleafs = 0;

  /* skip worldspawn (entity 0) */
  for ( i = 1; i < num_entities; i++ )
  {
    e = &g_entities[i];
    GetVectorForKey(e, "origin", origin);

    if ( origin[0] == 0.0 && origin[1] == 0.0 && origin[2] == 0.0 )
      continue;

    classname = ValueForKey(e, "classname");
    if ( IsExcludedEntity(0, classname) )
      continue;

    /* offset test point slightly above floor */
    origin[2] += 1.0f;

    if ( PlaceOccupant(headnode, origin, e) )
      inside = 1;
  }

  Com_DPrintf("%5i flooded leafs\n", c_floodedleafs);

  if ( !inside )
  {
    Com_DPrintf("no entities in open -- no filling\n");
    return 0;
  }

  if ( tree->outside_node.occupied )
  {
    Com_DPrintf("entity reached from outside -- no filling\n");
    if ( tree->outside_node.occupied )
      return 0;
  }

  return 1;
}

/*
================
WriteFloat

Writes a float value to file. If within 0.001 of an integer,
writes as integer; otherwise writes as float.
================
*/
int WriteFloat(FILE *file, float value)
{
  if ( fabs(value - Q_rint(value)) >= PLANESIDE_EPSILON )
    return fprintf(file, "%f ", value);
  return fprintf(file, "%i ", (int)Q_rint(value));
}

/*
================
WriteFaceFile_r

Recursively writes non-passable face portals to the .prt file.
For each non-opaque leaf, iterates its portals and writes non-passable
portal windings. Front node writes points in order; back node in reverse.
================
*/
int WriteFaceFile_r(Node_t *node)
{
  Node_t *n;
  Portal_t *p;
  Winding_t *w;
  int i, s;

  for ( n = node; n->planenum != PLANENUM_LEAF; n = n->children[1] )
    WriteFaceFile_r(n->children[0]);

  if ( n->opaque )
    return 0;

  for ( p = n->portals; p; p = p->next[s] )
  {
    s = (p->nodes[1] == n);
    w = p->winding;
    if ( !w )
      continue;

    if ( Portal_EntityFlood(p) )
      continue;

    /* non-passable portal — write to prt file */
    if ( p->nodes[0] == n )
    {
      /* front side — write points in order */
      fprintf(Stream, "%i %i ", w->numpoints, p->nodes[0]->cluster);
      for ( i = 0; i < w->numpoints; i++ )
      {
        fprintf(Stream, "(");
        WriteFloat(Stream, w->points[i][0]);
        WriteFloat(Stream, w->points[i][1]);
        WriteFloat(Stream, w->points[i][2]);
        fprintf(Stream, ") ");
      }
    }
    else
    {
      /* back side — write points in reverse */
      fprintf(Stream, "%i %i ", w->numpoints, p->nodes[1]->cluster);
      for ( i = w->numpoints - 1; i >= 0; i-- )
      {
        fprintf(Stream, "(");
        WriteFloat(Stream, w->points[i][0]);
        WriteFloat(Stream, w->points[i][1]);
        WriteFloat(Stream, w->points[i][2]);
        fprintf(Stream, ") ");
      }
    }
    fprintf(Stream, "\n");
  }
  return 0;
}

/*
================
NumberLeafs_r

Recursively numbers leaf nodes and counts portals.
Assigns sequential leaf numbers to non-opaque leaves.
For each leaf, walks its portals and counts passable vs non-passable.
================
*/
int NumberLeafs_r(Node_t *node)
{
  Node_t *n;
  Portal_t *p;
  int s;

  for ( n = node; n->planenum != PLANENUM_LEAF; n = n->children[1] )
  {
    n->cluster = CLUSTER_INTERNAL;
    NumberLeafs_r(n->children[0]);
  }

  n->area = -1;

  if ( n->opaque )
  {
    n->cluster = -1;
    return n->opaque;
  }

  n->cluster = num_visclusters++;

  for ( p = n->portals; p; p = p->next[s] )
  {
    s = (p->nodes[1] == n);
    if ( p->nodes[0] == n )
    {
      if ( Portal_EntityFlood(p) )
        ++num_visportals;
      else
        ++num_solidfaces;
    }
    else
    {
      if ( !Portal_EntityFlood(p) )
        ++num_solidfaces;
    }
  }
  return 0;
}

/*
================
NumberClusters

Initializes counters and numbers all BSP leaf nodes.
Prints statistics: total leafs, passable portals, solid portals.
================
*/
int NumberClusters(Tree_t *tree)
{
  num_visclusters = 0;
  num_visportals = 0;
  num_solidfaces = 0;

  Com_DPrintf("--- NumberClusters ---\n");
  NumberLeafs_r(tree->headnode);
  Com_DPrintf("%5i visclusters\n", num_visclusters);
  Com_DPrintf("%5i visportals\n", num_visportals);
  return Com_DPrintf("%5i solidfaces\n", num_solidfaces);
}

/*
================
WritePortalFilePRT_r

Recursively writes passable portal data to the .prt file.
For each non-opaque leaf, iterates its portals and writes passable ones.
Checks portal plane alignment to determine front/back node ordering in output.
================
*/
int WritePortalFilePRT_r(Node_t *node)
{
  Portal_t *p;
  Winding_t *w;
  Node_t *frontLeaf, *backLeaf;
  vec3_t planeNormal;
  float planeDist;
  int i, s;

  if ( node->planenum != PLANENUM_LEAF )
  {
    WritePortalFilePRT_r(node->children[0]);
    return WritePortalFilePRT_r(node->children[1]);
  }

  if ( node->opaque )
    return 0;

  for ( p = node->portals; p; p = p->next[s] )
  {
    s = (p->nodes[1] == node);
    w = p->winding;

    if ( !w || p->nodes[0] != node || !Portal_EntityFlood(p) )
      continue;

    /* check winding orientation against portal plane */
    PlaneFromTriangle(w, planeNormal, &planeDist);
    #define PORTAL_PLANE_DOT 0.99
    if ( DotProduct120(planeNormal, p->plane) >= PORTAL_PLANE_DOT )
    {
      frontLeaf = p->nodes[0];
      backLeaf = p->nodes[1];
    }
    else
    {
      frontLeaf = p->nodes[1];
      backLeaf = p->nodes[0];
    }

    fprintf(Stream, "%i %i %i ", w->numpoints, frontLeaf->cluster, backLeaf->cluster);
    for ( i = 0; i < w->numpoints; i++ )
    {
      fprintf(Stream, "(");
      WriteFloat(Stream, w->points[i][0]);
      WriteFloat(Stream, w->points[i][1]);
      WriteFloat(Stream, w->points[i][2]);
      fprintf(Stream, ") ");
    }
    fprintf(Stream, "\n");
  }
  return 0;
}

/*
================
WritePortalFile

Writes the complete .prt portal file for vis processing.
Creates the file, writes header (PRT1 format), leaf/portal counts,
then writes passable portals and face portals.
================
*/
int WritePortalFile(Tree_t *tree)
{
  char *prtExt;
  char filepath[MAX_OS_PATH];

  Com_DPrintf("--- WritePortalFile ---\n");
  prtExt = GetPRTFileExtension();
  sprintf(filepath, "%s%s", g_outputBasePath, prtExt);
  Com_Printf("writing %s\n", filepath);

  Stream = fopen(filepath, "w");
  if ( !Stream )
    Com_Error("Error opening %s", filepath);

  fprintf(Stream, "%s\n", "PRT1");
  fprintf(Stream, "%i\n", num_visclusters);
  fprintf(Stream, "%i\n", num_visportals);
  fprintf(Stream, "%i\n", num_solidfaces);

  WritePortalFilePRT_r(tree->headnode);
  WriteFaceFile_r(tree->headnode);
  return fclose(Stream);
}
