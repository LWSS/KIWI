/*
surface.c — Draw surface management

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

/*
================
AllocDrawSurface

Allocates a new draw surface from the global pool.
================
*/
DrawSurf_t *AllocDrawSurface(void)
{
  MapDrawSurf_t *nativeDrawSurf;
  DrawSurf_t *drawSurf;

  nativeDrawSurf = AllocMapDrawSurf();
  drawSurf = &g_drawSurfs[nativeDrawSurf - g_nativeMapDrawSurfs];
  /* These are donor-only mirrors of native MapDrawSurf defaults.  Set them
     at allocation so BSP orchestration does not need the unmatched CoD2
     AllocateLightmaps pre-pass. */
  drawSurf->lightStyle = LIGHTSTYLE_NONE;
  drawSurf->ownerGroup = NULL;
  drawSurf->nextInGroup = NULL;
  return drawSurf;
}

MapDrawSurf_t *NativeMapDrawSurfFor(DrawSurf_t *drawSurf)
{
  return &g_nativeMapDrawSurfs[drawSurf - g_drawSurfs];
}

/*
================
OpenDebugFile

Opens the debug polygon file for surface visualization.
================
*/
FILE *OpenDebugFile(void)
{
  char *extension;
  FILE *fileHandle;
  char filePath[MAX_OS_PATH_SHORT];

  extension = GetPolyFileExtension();
  sprintf(filePath, "%s%s", g_outputBasePath, extension);
  fileHandle = fopen(filePath, "wt");
  g_debugPolyFile = fileHandle;
  return fileHandle;
}

/*
================
CloseDebugFile

Closes the debug polygon file.
================
*/
void CloseDebugFile(void)
{
  if ( g_debugPolyFile )
  {
    fclose(g_debugPolyFile);
    g_debugPolyFile = NULL;
  }
}

/*
================
WriteDebugSurfaceToFile

Writes draw surface triangles to the debug polygon file.
================
*/
void WriteDebugSurfaceToFile(TerrainNode_t *tn)
{
  MeshVert_t *v;
  int i;

  if ( !g_debugPolyFile )
    return;

  for ( i = 0; i < tn->numIndexes; i++ )
  {
    /* write material header every 3 indices (one per triangle) */
    if ( i % 3 == 0 )
    {
      fprintf(g_debugPolyFile, "%s\n", tn->shaderInfo->name);
      fprintf(g_debugPolyFile, "3\n");
    }

    v = &tn->verts[tn->indexes[i]];
    fprintf(g_debugPolyFile, "( %g %g %g %g %g )\n",
            v->pos[0], v->pos[1], v->pos[2], v->st[0], v->st[1]);
  }
}

/*
================
FilterWindingIntoTree_r

Recursively filters a winding through the BSP tree, clipping against
node planes. At leaf nodes, checks winding validity against the side.
================
*/
void FilterWindingIntoTree_r(Winding_t *winding, BrushSide_t *side, Node_t *node)
{
  Winding_t *frontWinding, *backWinding;

  if ( !winding )
    return;

  if ( node->planenum == PLANENUM_LEAF )
  {
    /* at a leaf: add winding to side's visible hull if not opaque */
    if ( !node->opaque )
      CheckWinding(winding, &side->visibleHull, MAP_PLANE(side->planenum)->normal);
    FreeWinding(winding);
    return;
  }

  /* side on same plane as node — goes entirely to front child */
  if ( side->planenum == node->planenum )
  {
    FilterWindingIntoTree_r(winding, side, node->children[0]);
    return;
  }

  /* side on flipped plane — goes entirely to back child */
  if ( side->planenum == (node->planenum ^ 1) )
  {
    FilterWindingIntoTree_r(winding, side, node->children[1]);
    return;
  }

  /* split winding by node plane, recurse both halves */
  ClipWindingEpsilon(winding, MAP_PLANE(node->planenum)->normal, MAP_PLANE(node->planenum)->dist, SNAP_GRID_SIZE, &frontWinding, &backWinding, 1);
  FreeWinding(winding);
  FilterWindingIntoTree_r(frontWinding, side, node->children[0]);
  FilterWindingIntoTree_r(backWinding, side, node->children[1]);
}

/*
================
DrawSurfaceForSide

Creates a draw surface from a brush side, computing lightmap and
detail texture coordinates for each vertex from the side's tex vecs.
================
*/
DrawSurf_t *DrawSurfaceForSide(Brush_t *brush, BrushSide_t *side, Winding_t *winding)
{
  DrawSurf_t *surf;
  MapDrawSurf_t *nativeDrawSurf;
  MeshVert_t *verts, *mv;
  Plane_t *plane;
  int i;
  float minBounds[2], maxBounds[2];
  float midS, midT;

  #define MAX_FACE_WINDING_POINTS 64
  if ( winding->numpoints > MAX_FACE_WINDING_POINTS )
    Com_Error("DrawSurfaceForSide: w->numpoints = %i", winding->numpoints);

  surf = AllocDrawSurface();
  surf->entityNum = brush->entityNum;
  surf->brushNum = brush->brushNum;
  /* Native 0x439F80 keeps the originating map-info index in its transient
     record.  The KIWI carrier stores it in its expanded tail. */
  surf->mapInfoIndex = brush->mapInfoIndex;
  surf->mapName = MapInfo_GetName(brush->mapInfoIndex);
  surf->shaderInfo = side->shaderInfo;
  surf->materialName = side->materialName;
  surf->surfaceFlags = side->surfaceFlags;
  surf->contentFlags = side->contentFlags;
  surf->mapBrush = brush;
  surf->sideRef = side;
  surf->fogNum = -1;
  surf->outputNum = brush->outputNum;

  /* allocate vertex buffer */
  surf->numVerts = winding->numpoints;
  verts = malloc(winding->numpoints * sizeof(MeshVert_t));
  surf->verts = verts;
  memset(verts, 0, winding->numpoints * sizeof(MeshVert_t));

  minBounds[0] = minBounds[1] = BOUNDS_INIT_MAX;
  maxBounds[0] = maxBounds[1] = BOUNDS_INIT_MIN;

  /* compute vertex positions, normals, and texture coordinates */
  plane = MAP_PLANE(side->planenum);
  for ( i = 0; i < winding->numpoints; i++ )
  {
    mv = &verts[i];
    VectorCopy(winding->points[i], mv->pos);
    VectorCopy(plane->normal, mv->normal);

    /* texture coords: dot(pos, texVec) + texVec[3] */
    mv->st[0] = DotProduct021(mv->pos, side->texVecs[0]) + side->texVecs[0][3];
    mv->st[1] = DotProduct021(mv->pos, side->texVecs[1]) + side->texVecs[1][3];
    AddPointToBounds2D(mv->st, minBounds, maxBounds);

    /* lightmap coords */
    mv->lmSt[0] = DotProduct021(mv->pos, side->lmTexVecs[0]) + side->lmTexVecs[0][3];
    mv->lmSt[1] = DotProduct021(mv->pos, side->lmTexVecs[1]) + side->lmTexVecs[1][3];
  }

  /* for global textures, center the texture coordinates */
  if ( surf->shaderInfo->globalTexture )
  {
    midS = MIDF(maxBounds[0], minBounds[0]);
    midT = MIDF(maxBounds[1], minBounds[1]);
    minBounds[0] = (float)fistp_sub(midS, FISTP_HALF_BIAS);
    minBounds[1] = (float)fistp_sub(midT, FISTP_HALF_BIAS);

    for ( i = 0; i < winding->numpoints; i++ )
    {
      verts[i].st[0] -= minBounds[0];
      verts[i].st[1] -= minBounds[1];
    }
  }

  /* Native 0x439F80 MapDrawSurf sidecar.  Keep the expanded DrawSurf_t for
     the donor pipeline, while preserving the exact 0x44-byte carrier needed
     by the CoD4 primary-light and sun-shadow paths. */
  nativeDrawSurf = NativeMapDrawSurfFor(surf);
  nativeDrawSurf->material = side->shaderInfo;
  nativeDrawSurf->lightmapMaterial = side->lmShaderInfo;
  /* Preserve native source offsets +88/+96/+100 verbatim.  Their semantic
     interpretation varies across surface creators. */
  nativeDrawSurf->contentFlags = side->contentFlags;
  nativeDrawSurf->toolFlags = side->toolFlags;
  nativeDrawSurf->compileOrSmoothing = side->compileFlags;
  nativeDrawSurf->entityNum = brush->entityNum;
  nativeDrawSurf->sourceIndex.brushNum = brush->brushNum;
  nativeDrawSurf->sourceInfo.mapInfoIndex = brush->mapInfoIndex;
  nativeDrawSurf->vertCount = surf->numVerts;
  nativeDrawSurf->verts = surf->verts;
  nativeDrawSurf->outputNum = brush->outputNum;
  nativeDrawSurf->isPatch = 0;
  nativeDrawSurf->isTerrain = 0;
  nativeDrawSurf->u.brush.side = side;

  return surf;
}

/*
================
WriteDebugWindingToFile

Writes winding vertices with computed lightmap S/T coords to the debug polygon file.
================
*/
void WriteDebugWindingToFile(BrushSide_t *side, Winding_t *winding)
{
  int i;
  float *p;
  float s, t;

  if ( !g_debugPolyFile )
    return;

  fprintf(g_debugPolyFile, "%s\n", side->shaderInfo->name);
  fprintf(g_debugPolyFile, "%i\n", winding->numpoints);

  for ( i = 0; i < winding->numpoints; i++ )
  {
    p = winding->points[i];

    /* compute texture coords — FPU order preserved */
    s = side->texVecs[0][0] * p[0] + p[1] * side->texVecs[0][1] + side->texVecs[0][2] * p[2] + side->texVecs[0][3];
    t = p[1] * side->texVecs[1][1] + side->texVecs[1][2] * p[2] + p[0] * side->texVecs[1][0] + side->texVecs[1][3];

    fprintf(g_debugPolyFile, "( %g %g %g %g %g )\n", p[0], p[1], p[2], s, t);
  }
}

/*
================
SubdivideFace_r

Recursively subdivides a face surface along axis-aligned planes
at subdivision boundaries, then creates draw surfaces for each piece.
================
*/
void SubdivideFace_r(DrawSurf_t *ds, Winding_t *winding, float subdivisions)
{
  int i, axis;
  float mins[3], maxs[3];
  vec3_t planePoint, planeNormal;
  float planeDist, invSubdivisions;
  int subFloor, subCeil, subRange;
  Winding_t *frontWinding, *backWinding;

  if ( !winding )
    return;

  if ( winding->numpoints < 3 )
    Com_Error("SubdivideDrawSurf: Bad w->numpoints");

  /* compute winding bounds */
  ClearBounds(mins, maxs);
  for ( i = 0; i < winding->numpoints; i++ )
    AddPointToBounds(winding->points[i], mins, maxs);

  invSubdivisions = 1.0f / subdivisions;

  /* try each axis for subdivision */
  for ( axis = 0; axis < 3; axis++ )
  {
    subFloor = (int)(floor(invSubdivisions * mins[axis]) * subdivisions);
    subCeil = (int)(ceil(invSubdivisions * maxs[axis]) * subdivisions);
    subRange = subCeil - subFloor;

    if ( (double)subRange <= subdivisions )
      continue;

    /* build split plane along this axis */
    VectorClear(planePoint);
    VectorClear(planeNormal);
    planeNormal[axis] = -1.0f;
    planePoint[axis] = (double)subFloor + subdivisions;
    planeDist = DotProduct210(planeNormal, planePoint);

    ClipWindingEpsilon(winding, planeNormal, planeDist, ON_EPSILON, &frontWinding, &backWinding, 0);

    if ( frontWinding )
    {
      if ( backWinding )
      {
        /* both halves survived — recurse on each */
        SubdivideFace_r(ds, frontWinding, subdivisions);
        SubdivideFace_r(ds, backWinding, subdivisions);
        return;
      }
      winding = frontWinding;
    }
    else
    {
      winding = backWinding;
    }
  }

  /* no more subdivisions needed — emit the face */
  CheckWindingInPlane(winding, MAP_PLANE(ds->sideRef->planenum)->normal, MAP_PLANE(ds->sideRef->planenum)->dist);
  DrawSurfaceForSide(ds->mapBrush, ds->sideRef, winding)->fogNum = ds->fogNum;
}

/*
================
WindingFromDrawSurf

Creates a winding from draw surface vertices, copying xyz positions.
================
*/
Winding_t *WindingFromDrawSurf(DrawSurf_t *surf)
{
  Winding_t *w;
  MeshVert_t *verts;
  int i;

  w = AllocWinding(surf->numVerts);
  w->numpoints = surf->numVerts;

  verts = surf->verts;
  for ( i = 0; i < surf->numVerts; i++ )
    VectorCopy(verts[i].pos, w->points[i]);

  return w;
}

/*
================
SubdivideDrawSurfs

Iterates draw surfaces for an entity, subdividing faces whose
material specifies a subdivision value.
================
*/
void SubdivideDrawSurfs(Entity_t *entity)
{
  DrawSurf_t *surf;
  BrushSide_t *side;
  ShaderInfo_t *si;
  Winding_t *w;
  float tessSize;
  int end;
  int i;

  Com_DPrintf("----- SubdivideDrawSurfs -----\n");

  end = numMapDrawSurfs;
  for ( i = entity->firstDrawSurf; i < end; i++ )
  {
    surf = &g_drawSurfs[i];
    side = surf->sideRef;
    if ( !side || surf->isPatch || surf->isTerrain )
      continue;

    si = side->shaderInfo;
    if ( !si )
      continue;

    tessSize = si->subdivisions;
    if ( tessSize == 0.0f )
    {
      if ( g_matExpandRegion.defaultTessSize == 0.0f )
        continue;
      tessSize = g_matExpandRegion.defaultTessSize;
    }

    /* subdivide this face */
    w = WindingFromDrawSurf(surf);
    surf->numVerts = 0;
    NativeMapDrawSurfFor(surf)->vertCount = 0;
    SubdivideFace_r(surf, w, tessSize);
  }
}

/* CoD4 0x4163C0.  Node_t::opaque is three-valued while the face tree is
   finalized: 0=open leaf, 1=opaque leaf, and 2=mixed node. */
static qboolean WindingFullyCulledByBspTree(Node_t *node, Winding_t *winding)
{
  for ( ;; )
  {
    Winding_t *front;
    Winding_t *back;
    int side;

    if ( node->opaque != 2 )
      return node->opaque == 1;

    side = ClipWinding(winding, MAP_PLANE(node->planenum)->normal,
                       MAP_PLANE(node->planenum)->dist, 0.1f, &front, &back);
    if ( side == SIDE_ON )
      return WindingFullyCulledByBspTree(node->children[0], winding)
          && WindingFullyCulledByBspTree(node->children[1], winding);
    if ( side == SIDE_CROSS )
    {
      qboolean culled = WindingFullyCulledByBspTree(node->children[0], front)
                     && WindingFullyCulledByBspTree(node->children[1], back);

      FreeWinding(front);
      FreeWinding(back);
      return culled;
    }

    node = node->children[side];
  }
}

/* CoD4 0x416330.  This runs after MarkVisibleSides and before the world
   detail-tree rebuild, discarding only hulls wholly enclosed by opaque leaves. */
void CullBrushSideVisibleHulls(Brush_t *brushes, Node_t *treeRoot)
{
  Brush_t *brush;
  int sideIndex;

  for ( brush = brushes; brush; brush = brush->next )
  {
    for ( sideIndex = 0; sideIndex < brush->numSides; ++sideIndex )
    {
      BrushSide_t *side = &brush->sides[sideIndex];

      if ( side->visibleHull
        && WindingFullyCulledByBspTree(treeRoot, side->visibleHull) )
      {
        FreeWinding(side->visibleHull);
        side->visibleHull = NULL;
      }
    }
  }
}

/*
================
ClipSidesIntoTree

Walks all brushes for an entity, filters each side's winding into the
BSP tree, then creates draw surfaces for visible sides.
================
*/
int ClipSidesIntoTree(Entity_t *entity, Tree_t *tree)
{
  Brush_t *b;
  BrushSide_t *side;
  Winding_t *w, *hull;
  ShaderInfo_t *si;
  int i;

  Com_DPrintf("----- ClipSidesIntoTree -----\n");

  for ( b = entity->brushes; b; b = b->next )
  {
    for ( i = 0; i < b->numSides; i++ )
    {
      side = &b->sides[i];
      if ( !side->winding )
        continue;

      /* filter a copy of the winding through the BSP tree */
      w = CopyWinding(side->winding);
      side->visibleHull = NULL;
      FilterWindingIntoTree_r(w, side, tree->headnode);

      hull = side->visibleHull;
      if ( !hull )
        continue;

      si = side->shaderInfo;
      if ( !si )
        continue;

      /* if material has noClip, use original winding instead of clipped hull */
      if ( si->noClip )
        hull = side->winding;

      /* write debug output for nodraw visible surfaces */
      if ( (si->surfaceFlags & SURF_NODRAW) && !(si->surfaceFlags & SURF_NOCASTSHADOW) )
        WriteDebugWindingToFile(side, hull);

      if ( side->bevel )
        Com_Error("monkey tried to create draw surface for brush bevel");

      CheckWindingInPlane(hull, MAP_PLANE(side->planenum)->normal, MAP_PLANE(side->planenum)->dist);
      DrawSurfaceForSide(b, side, hull);
    }
  }

  return 0;
}
