/* 2D polygon splitting and grid subdivision. */
#include "cod4map.h"

#define MAX_VERTS_PER_POLY 0x4000
#define POLY2D_BUFFER_VERTS (MAX_VERTS_PER_POLY + 4)
#define POLY2D_BUFFER_FLOATS (POLY2D_BUFFER_VERTS * 2)

static char s_assertDisable_SplitPoly2D_coords;
static char s_assertDisable_SplitPoly2D_vertCount;
static char s_assertDisable_SplitPoly2D_axis;
static char s_assertDisable_SplitPoly2D_coordsFront;
static char s_assertDisable_SplitPoly2D_frontCountOut;
static char s_assertDisable_SplitPoly2D_coordsBack;
static char s_assertDisable_SplitPoly2D_backCountOut;
static char s_assertDisable_SplitPoly2D_frontBackDistinct;

/* Native 0x42AC30. */
static float Poly2DAreaAndCentroid(const float *coords, int vertCount, float *centroid)
{
  float area;
  float centroidX;
  float centroidY;
  int i;

  area = coords[0] * (coords[2 * vertCount - 1] - coords[3])
       + coords[2 * vertCount - 2] * (coords[2 * vertCount - 3] - coords[1]);
  centroidX = coords[1] * (coords[2] - coords[2 * vertCount - 2])
            * (coords[2] + coords[0] + coords[2 * vertCount - 2])
            + coords[2 * vertCount - 1] * (coords[0] - coords[2 * vertCount - 4])
            * (coords[0] + coords[2 * vertCount - 2] + coords[2 * vertCount - 4]);
  centroidY = coords[0] * (coords[2 * vertCount - 1] - coords[3])
            * (coords[3] + coords[1] + coords[2 * vertCount - 1])
            + coords[2 * vertCount - 2] * (coords[2 * vertCount - 3] - coords[1])
            * (coords[1] + coords[2 * vertCount - 1] + coords[2 * vertCount - 3]);

  for ( i = 1; i < vertCount - 1; ++i )
  {
    area += coords[2 * i] * (coords[2 * i - 1] - coords[2 * i + 3]);
    centroidX += coords[2 * i + 1] * (coords[2 * i + 2] - coords[2 * i - 2])
               * (coords[2 * i + 2] + coords[2 * i] + coords[2 * i - 2]);
    centroidY += coords[2 * i] * (coords[2 * i - 1] - coords[2 * i + 3])
               * (coords[2 * i + 3] + coords[2 * i + 1] + coords[2 * i - 1]);
  }

  centroid[0] = centroidX / (area * 3.0f);
  centroid[1] = centroidY / (area * 3.0f);
  return area;
}

/* Native 0x42AE50. */
void SplitPoly2D(
    const float *coords,
    int vertCount,
    unsigned int axis,
    float splitCoord,
    float *coordsFront,
    int *frontCountOut,
    float *coordsBack,
    int *backCountOut)
{
  enum { POLY_FRONT, POLY_BACK, POLY_ON };
  int sides[MAX_VERTS_PER_POLY];
  int frontCount;
  int backCount;
  int i;
  int j;
  float frontEpsilon;
  float backEpsilon;
  float fraction;
  float intersection[2];

  Assert(coords, s_assertDisable_SplitPoly2D_coords);
  Assert(vertCount >= 3 && vertCount <= MAX_VERTS_PER_POLY, s_assertDisable_SplitPoly2D_vertCount);
  Assert(axis == 0 || axis == 1, s_assertDisable_SplitPoly2D_axis);
  Assert(coordsFront, s_assertDisable_SplitPoly2D_coordsFront);
  Assert(frontCountOut, s_assertDisable_SplitPoly2D_frontCountOut);
  Assert(coordsBack, s_assertDisable_SplitPoly2D_coordsBack);
  Assert(backCountOut, s_assertDisable_SplitPoly2D_backCountOut);
  Assert(coords != coordsFront, s_assertDisable_SplitPoly2D_coordsFront);
  Assert(coords != coordsBack, s_assertDisable_SplitPoly2D_coordsBack);
  Assert(coordsFront != coordsBack, s_assertDisable_SplitPoly2D_frontBackDistinct);

  *frontCountOut = 0;
  *backCountOut = 0;
  frontCount = 0;
  backCount = 0;
  frontEpsilon = splitCoord + 0.001f;
  backEpsilon = splitCoord - 0.001f;

  for ( i = 0; i < vertCount; ++i )
  {
    sides[i] = POLY_ON;
    if ( frontEpsilon >= coords[2 * i + axis] )
    {
      if ( backEpsilon > coords[2 * i + axis] )
      {
        sides[i] = POLY_BACK;
        ++backCount;
      }
    }
    else
    {
      sides[i] = POLY_FRONT;
      ++frontCount;
    }
  }

  if ( !backCount )
  {
    memcpy(coordsFront, coords, sizeof(float) * 2 * vertCount);
    *frontCountOut = vertCount;
    return;
  }
  if ( !frontCount )
  {
    memcpy(coordsBack, coords, sizeof(float) * 2 * vertCount);
    *backCountOut = vertCount;
    return;
  }

  i = vertCount - 1;
  for ( j = 0; j < vertCount; ++j )
  {
    if ( sides[i] == POLY_ON )
    {
      coordsFront[2 * *frontCountOut] = coords[2 * i];
      coordsFront[2 * *frontCountOut + 1] = coords[2 * i + 1];
      ++*frontCountOut;
      coordsBack[2 * *backCountOut] = coords[2 * i];
      coordsBack[2 * *backCountOut + 1] = coords[2 * i + 1];
      ++*backCountOut;
    }
    else
    {
      if ( sides[i] == POLY_BACK )
      {
        coordsBack[2 * *backCountOut] = coords[2 * i];
        coordsBack[2 * *backCountOut + 1] = coords[2 * i + 1];
        ++*backCountOut;
      }
      else
      {
        coordsFront[2 * *frontCountOut] = coords[2 * i];
        coordsFront[2 * *frontCountOut + 1] = coords[2 * i + 1];
        ++*frontCountOut;
      }

      if ( sides[j] != POLY_ON && sides[j] != sides[i] )
      {
        fraction = (coords[2 * i + axis] - splitCoord)
                 / (coords[2 * i + axis] - coords[2 * j + axis]);
        intersection[1 - axis] = (coords[2 * j + 1 - axis] - coords[2 * i + 1 - axis]) * fraction
                               + coords[2 * i + 1 - axis];
        intersection[axis] = splitCoord;
        coordsFront[2 * *frontCountOut] = intersection[0];
        coordsFront[2 * *frontCountOut + 1] = intersection[1];
        ++*frontCountOut;
        coordsBack[2 * *backCountOut] = intersection[0];
        coordsBack[2 * *backCountOut + 1] = intersection[1];
        ++*backCountOut;
      }
    }
    i = j;
  }

  if ( *frontCountOut > MAX_VERTS_PER_POLY || *backCountOut > MAX_VERTS_PER_POLY )
    Com_Error("MAX_VERTS_PER_POLY exceeded on 2d poly\n");
}

/* Native 0x42B350. polyBuffers has four 0x4004-vertex buffers. */
int Subdivide2DPolygonGrid(
    float *polyBuffers,
    int vertCount,
    int gridCountX,
    int gridCountY,
    float gridMinsX,
    float gridMinsY,
    float gridSizeX,
    float gridSizeY,
    Poly2DSubdivideCallback callback,
    void *userData)
{
  float areaThresholdMin;
  float areaThresholdMax;
  int currentRowBuffer;
  int remainingRowVertCount;
  int columnInputBuffer;
  int columnOutputBuffer;
  int scratchBuffer;
  int polygonIndex;
  float splitY;
  int i;
  int j;
  int k;
  int frontCount;
  int cellVertCount;
  float splitX;
  float area;
  float areaMagnitude;
  float centroid[2];
  int result;

  areaThresholdMin = gridSizeX * 0.000002000000222324161f * gridSizeY;
  areaThresholdMax = gridSizeX * 0.5f * gridSizeY;
  currentRowBuffer = 0;
  remainingRowVertCount = vertCount;
  columnInputBuffer = 1;
  columnOutputBuffer = 2;
  scratchBuffer = 3;
  polygonIndex = 0;
  splitY = gridMinsY;
  result = vertCount;

  for ( i = 0; i < gridCountY && remainingRowVertCount; ++i )
  {
    result = gridCountY - 1;
    if ( i == gridCountY - 1 )
    {
      columnInputBuffer = currentRowBuffer;
      cellVertCount = remainingRowVertCount;
    }
    else
    {
      int swap;
      splitY += gridSizeY;
      SplitPoly2D(polyBuffers + POLY2D_BUFFER_FLOATS * currentRowBuffer, remainingRowVertCount, 1, splitY,
          polyBuffers + POLY2D_BUFFER_FLOATS * scratchBuffer, &frontCount,
          polyBuffers + POLY2D_BUFFER_FLOATS * columnInputBuffer, &cellVertCount);
      swap = currentRowBuffer;
      currentRowBuffer = scratchBuffer;
      scratchBuffer = swap;
      remainingRowVertCount = frontCount;
    }

    splitX = gridMinsX;
    for ( j = 0; j < gridCountX && cellVertCount; ++j )
    {
      if ( j == gridCountX - 1 )
      {
        int swap = columnInputBuffer;
        columnInputBuffer = columnOutputBuffer;
        columnOutputBuffer = swap;
        frontCount = cellVertCount;
      }
      else
      {
        int swap;
        splitX += gridSizeX;
        SplitPoly2D(polyBuffers + POLY2D_BUFFER_FLOATS * columnInputBuffer, cellVertCount, 0, splitX,
            polyBuffers + POLY2D_BUFFER_FLOATS * scratchBuffer, &frontCount,
            polyBuffers + POLY2D_BUFFER_FLOATS * columnOutputBuffer, &cellVertCount);
        swap = columnInputBuffer;
        columnInputBuffer = scratchBuffer;
        scratchBuffer = swap;
      }

      if ( frontCount >= 3 )
      {
        area = Poly2DAreaAndCentroid(polyBuffers + POLY2D_BUFFER_FLOATS * columnOutputBuffer, frontCount, centroid);
        areaMagnitude = (float)fabs((double)area);
        if ( areaThresholdMin <= areaMagnitude )
        {
          if ( areaThresholdMax > areaMagnitude )
          {
            centroid[0] = 0.0f;
            centroid[1] = 0.0f;
            for ( k = 0; k < frontCount; ++k )
            {
              centroid[0] += polyBuffers[POLY2D_BUFFER_FLOATS * columnOutputBuffer + 2 * k];
              centroid[1] += polyBuffers[POLY2D_BUFFER_FLOATS * columnOutputBuffer + 2 * k + 1];
            }
            centroid[0] *= 1.0f / (float)frontCount;
            centroid[1] *= 1.0f / (float)frontCount;
          }
          callback(areaMagnitude, centroid, polyBuffers + POLY2D_BUFFER_FLOATS * columnOutputBuffer, frontCount, userData, polygonIndex++);
        }
      }
      result = j + 1;
    }
  }

  return result;
}
