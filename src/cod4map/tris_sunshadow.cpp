/*
shadows.c — Shadow map compilation

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

/*
================
TriangleFacesSun

Carrier-independent facing test used by the native sun-shadow preprocessor.

cod4map.exe: 0x45AAC0
================
*/
int TriangleFacesSun(float *vert0, float *vert1, float *vert2, float *sunDirection)
{
  float plane[4];

  if (!PlaneFromPoints(plane, vert0, vert1, vert2))
    return 0;
  return DotProduct(plane, sunDirection) <= 0.009999999776482582f;
}

/*
================
Vector2MA

Native two-dimensional multiply-add.  The native ABI returns the direction
argument (not the output), and callers use only the two output components.

cod4map.exe: 0x45AB20
================
*/
float *Vector2MA(float *start, float scale, float *direction, float *out)
{
  out[0] = start[0] + scale * direction[0];
  out[1] = start[1] + scale * direction[1];
  return direction;
}

/*
================
ProjectedWindingContainsPoint

Native wrapper around the shared projected-winding containment primitive.

cod4map.exe: 0x45AB50
================
*/
int ProjectedWindingContainsPoint(const float *verts, int vertCount, float *plane,
                                  const float *point, float epsilon)
{
  int axisU;
  int axisV;

  GetProjectionAxes(plane, &axisU, &axisV);
  return ProjectedWindingContainsCoplanarPoint(verts, vertCount, axisU, axisV, point, epsilon);
}

/*
================
CastsSunShadowConfirmed

Converts the native transient sun-shadow classification to the boolean
serialized in BspTriSoup_t::castsSunShadow.

cod4map.exe: 0x45A700
================
*/
int CastsSunShadowConfirmed(unsigned char castsSunShadow)
{
  AssertMsg(castsSunShadow == CASTS_SUN_SHADOW_CONFIRMED ||
            castsSunShadow == CASTS_SUN_SHADOW_DENIED,
            s_assertDisable_CastsSunShadowConfirmed,
            "%s\n\t%i",
            "(castsSunShadow == CASTS_SUN_SHADOW_CONFIRMED || castsSunShadow == CASTS_SUN_SHADOW_DENIED)",
            castsSunShadow);
  return castsSunShadow == CASTS_SUN_SHADOW_CONFIRMED;
}

/* Native transient TriSurfProps stores its MapDrawSurf pointer at +0.  KIWI
   retains ShaderInfo at that native slot for its established consumers, so
   the exact source-surface relation is carried beside it. */
static MapDrawSurf_t *SunShadowMapDrawSurf(const TriSurfProps_t *props)
{
  TriSurfPropsSidecar_t *sidecar;

  Assert(props, s_assertDisable_CastsSunShadowConfirmed);
  sidecar = TrisPropsSidecar_Get((TriSurfProps_t *)props);
  Assert(sidecar, s_assertDisable_CastsSunShadowConfirmed);
  Assert(sidecar->mapDrawSurf, s_assertDisable_CastsSunShadowConfirmed);
  return sidecar->mapDrawSurf;
}

/* CoD4 0x45A670.  Coalesced props represent several input draw surfaces;
   the native category must therefore be written to every leaf, not merely
   the composite owner. */
char SetCoalescedSunShadowCategory(TriSurf_t *surf, char category)
{
  TriSurfProps_t *props;
  CoalesceNode_t *node;

  Assert(surf, s_assertDisable_CastsSunShadowConfirmed);
  props = surf->props;
  Assert(props, s_assertDisable_CastsSunShadowConfirmed);
  if (props->coalesceChain)
  {
    TriSurfProps_t *firstLeaf = (TriSurfProps_t *)props->coalesceChain->value;

    Assert(SunShadowMapDrawSurf(props) == SunShadowMapDrawSurf(firstLeaf),
           s_assertDisable_CastsSunShadowConfirmed);
    for (node = props->coalesceChain; node; node = node->next)
      SunShadowMapDrawSurf((TriSurfProps_t *)node->value)->castsSunShadow = (unsigned char)category;
  }
  else
  {
    SunShadowMapDrawSurf(props)->castsSunShadow = (unsigned char)category;
  }
  return category;
}

/* CoD4 0x45A650. */
int ConfirmCoalescedSunShadowCategory(TriSurf_t *surf)
{
  return SetCoalescedSunShadowCategory(surf, CASTS_SUN_SHADOW_CONFIRMED);
}

/* CoD4 0x45A7F0. */
int DisableSunShadowTrisForEntityRange(void)
{
  int drawSurfIndex;

  for (drawSurfIndex = g_entities[g_currentEntityIndex].firstDrawSurf;
       drawSurfIndex < numMapDrawSurfs;
       ++drawSurfIndex)
  {
    g_nativeMapDrawSurfs[drawSurfIndex].castsSunShadow = CASTS_SUN_SHADOW_CONFIRMED;
  }
  return drawSurfIndex;
}

/* CoD4 0x45A9E0.  Returning zero terminates the native grid-tree trace. */
unsigned char SunShadowSegmentHitCallback(TriSurf_t *surf, float *segmentStart,
                                          float *segmentEnd, int userData)
{
  float startSide;
  float endSide;
  float fraction;
  float hitPoint[3];
  float *plane;

  (void)userData;
  Assert(surf, s_assertDisable_CastsSunShadowConfirmed);
  Assert(surf->props, s_assertDisable_CastsSunShadowConfirmed);
  if ((surf->props->si->gameFlags & 2) == 0)
    return 1;

  plane = surf->props->plane;
  startSide = DotProduct(segmentStart, plane) - plane[3];
  endSide = DotProduct(segmentEnd, plane) - plane[3];
  if (startSide * endSide >= 0.0f)
    return 1;

  fraction = startSide / (startSide - endSide);
  hitPoint[0] = (segmentEnd[0] - segmentStart[0]) * fraction + segmentStart[0];
  hitPoint[1] = (segmentEnd[1] - segmentStart[1]) * fraction + segmentStart[1];
  hitPoint[2] = (segmentEnd[2] - segmentStart[2]) * fraction + segmentStart[2];
  return ProjectedWindingContainsPoint(&surf->winding->points[0][0], surf->winding->numpoints,
                                       plane, hitPoint, 0.0099999998f) == 0;
}

/* CoD4 0x45A970. */
int SunShadowVertexIsExposed(MeshVert_t *vert)
{
  float segmentStart[3];
  float segmentEnd[3];

  Vector2MA(vert->pos, 0.5f, vert->normal, segmentStart);
  segmentStart[2] = vert->pos[2] - 0.5f;
  VectorCopy(segmentStart, segmentEnd);
  segmentEnd[2] -= 16384.0f;
  return GridTree_SegmentTraverse(segmentStart, segmentEnd, 0, SunShadowSegmentHitCallback);
}

/* CoD4 0x45A830. */
void ClassifySunShadowTri(MapDrawSurf_t *drawSurf, float *sunDirection)
{
  int index;

  drawSurf->castsSunShadow = CASTS_SUN_SHADOW_CONFIRMED;
  for (index = 0; index < drawSurf->vertCount; ++index)
  {
    if (drawSurf->verts[index].lmSt[1] < 0.5f)
      return;
  }
  for (index = 0; index < drawSurf->u.indexed.indexCount; index += 3)
  {
    int *indexes = drawSurf->u.indexed.indexes;

    if (TriangleFacesSun(drawSurf->verts[indexes[index]].pos,
                         drawSurf->verts[indexes[index + 1]].pos,
                         drawSurf->verts[indexes[index + 2]].pos,
                         sunDirection))
      return;
  }
  for (index = 0; index < drawSurf->vertCount; ++index)
  {
    if (!SunShadowVertexIsExposed(&drawSurf->verts[index]))
      return;
  }
  drawSurf->castsSunShadow = CASTS_SUN_SHADOW_DENIED;
}

/* CoD4 0x45A750. */
int PreprocessSunShadowTriCategories(void)
{
  float sunAngles[3];
  float sunDirection[3];
  int drawSurfIndex;

  if (g_currentEntityIndex)
    return DisableSunShadowTrisForEntityRange();

  GetVectorForKey(&g_entities[0], "sundirection", sunAngles);
  AngleVectors(sunAngles, sunDirection, NULL, NULL);
  for (drawSurfIndex = 0; drawSurfIndex < numMapDrawSurfs; ++drawSurfIndex)
  {
    MapDrawSurf_t *drawSurf = &g_nativeMapDrawSurfs[drawSurfIndex];

    if (drawSurf->isTerrain)
      ClassifySunShadowTri(drawSurf, sunDirection);
    else
      drawSurf->castsSunShadow = CASTS_SUN_SHADOW_CONFIRMED;
  }
  return drawSurfIndex;
}
