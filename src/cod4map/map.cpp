/*
map.c — Map file parser

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char s_assertDisable_AdjustBrushesForOrigin;
char s_assertDisable_AdjustBrushesForOrigin;
char s_assertDisable_LoadPrefab;
char s_assertDisable_LoadPrefab;
char s_assertDisable_CreateNewFloatPlane;
char s_assertDisable_ParseRawBrush;
char s_assertDisable_ProcessScriptVehicles;

#define MAX_MAP_INFOS 32768

typedef struct MapInfo_s
{
  const char *sourceMapName;
  float origin[3];
  float angles[3];
  float scale;
} MapInfo_t;

typedef char MapInfo_t_must_be_0x20[(sizeof(MapInfo_t) == COD4MAP_NATIVE_LAYOUT(0x20, 0x28)) ? 1 : -1];

static MapInfo_t s_mapInfo[MAX_MAP_INFOS];
static int s_mapInfoCount;
static FILE *s_lightgridAutoFile;
static FILE *s_lightgridNotFile;

typedef struct ScriptColorToken_s {
  int parentPrefab;
  char *token;
  char replacement[32];
  struct ScriptColorToken_s *next;
} ScriptColorToken_t;

/* Native 0x41EE90 owns a process-lifetime std::map keyed by
   (parentPrefab, token), plus a per-leading-character counter at 0x123AA4D8.
   Keep the STL implementation detail out of the compiler carrier. */
static ScriptColorToken_t *s_scriptColorTokens;
static int s_scriptColorTokenIndices[256];

static void SkipMapLayerInfo(char **parsePtr);

static int NextScriptColorTokenIndex(const char *token, char *prefix, int *index)
{
  unsigned char key = (unsigned char)token[0];

  *prefix = (char)key;
  *index = s_scriptColorTokenIndices[key];
  return ++s_scriptColorTokenIndices[key];
}

static void NormalizeScriptColorTokens(Epair_t *epairs, int parentPrefab, const char *key)
{
  char *value = (char *)EPairList_ValueForKey(epairs, key);
  char *copy;
  char *token;
  char destination[1024];

  if (!*value)
    return;

  copy = _strdup(value);
  destination[0] = '\0';
  for (token = strtok(copy, " "); token; token = strtok(NULL, " "))
  {
    ScriptColorToken_t *entry;

    for (entry = s_scriptColorTokens; entry; entry = entry->next)
    {
      if (entry->parentPrefab == parentPrefab && !strcmp(entry->token, token))
        break;
    }
    if (!entry)
    {
      char prefix;
      int index;

      NextScriptColorTokenIndex(token, &prefix, &index);
      entry = (ScriptColorToken_t *)malloc(sizeof(*entry));
      if (!entry)
        Com_Error("NormalizeScriptColorTokens: out of memory");
      entry->parentPrefab = parentPrefab;
      entry->token = _strdup(token);
      _snprintf(entry->replacement, sizeof(entry->replacement), "%c%i ", prefix, index);
      entry->replacement[sizeof(entry->replacement) - 1] = '\0';
      entry->next = s_scriptColorTokens;
      s_scriptColorTokens = entry;
    }
    strcat(destination, entry->replacement);
  }
  EPairList_SetKeyValue(epairs, key, destination);
  free(copy);
}

int MapInfo_Add(const char *sourceMapName, const float *origin,
                const float *angles, float scale)
{
  MapInfo_t *info;

  Assert(sourceMapName != NULL, s_assertDisable_LoadPrefab);
  Assert(origin != NULL, s_assertDisable_LoadPrefab);
  Assert(angles != NULL, s_assertDisable_LoadPrefab);
  if ( s_mapInfoCount == MAX_MAP_INFOS )
    return MAX_MAP_INFOS;

  info = &s_mapInfo[s_mapInfoCount++];
  info->sourceMapName = sourceMapName;
  VectorCopy(origin, info->origin);
  VectorCopy(angles, info->angles);
  info->scale = scale;
  return s_mapInfoCount - 1;
}

const char *MapInfo_GetName(int mapInfoIndex)
{
  if ( mapInfoIndex >= s_mapInfoCount )
    return "(unknown map; ran out of room for map names)";
  return s_mapInfo[mapInfoIndex].sourceMapName;
}

void MapInfo_GetTransform(int mapInfoIndex, float *transformMtx, float *scale)
{
  if ( mapInfoIndex >= s_mapInfoCount )
  {
    VectorClear(transformMtx);
    AnglesToMatrix(vec3_origin_float, transformMtx + 3);
    *scale = 1.0f;
    return;
  }

  VectorCopy(s_mapInfo[mapInfoIndex].origin, transformMtx);
  AnglesToMatrix(s_mapInfo[mapInfoIndex].angles, transformMtx + 3);
  *scale = s_mapInfo[mapInfoIndex].scale;
}

static int MapInfo_IsDistinctSource(int mapInfoIndex)
{
  int i;

  for (i = 0; i < mapInfoIndex; ++i)
  {
    if (!strcmp(s_mapInfo[i].sourceMapName,
      s_mapInfo[mapInfoIndex].sourceMapName))
    {
      return 0;
    }
  }
  return 1;
}

static int GetMapSourceTimestamp(const char *fileName, FILETIME *lastWriteTime)
{
  HANDLE file;

  file = CreateFileA(fileName, GENERIC_READ, FILE_SHARE_READ, NULL,
    OPEN_EXISTING, 0, NULL);
  if (file == INVALID_HANDLE_VALUE)
  {
    file = CreateFileA(FS_BuildOSPath(va("..\\map_source\\%s", fileName)),
      GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
      return 0;
  }
  {
    const int result = GetFileTime(file, NULL, NULL, lastWriteTime);
    CloseHandle(file);
    return result;
  }
}

/* Native 0x419200.  OnlyEnts calls this before its changed-brush fatal so a
   stale parent map/prefab is reported even when its geometry count matches. */
int CheckMapSourceTimestamps(const char *mapFileName)
{
  FILETIME requestedTime;
  FILETIME sourceTime;
  int i;

  if (!GetMapSourceTimestamp(mapFileName, &requestedTime))
    return 0;

  for (i = 0; i < s_mapInfoCount; ++i)
  {
    if (MapInfo_IsDistinctSource(i)
      && GetMapSourceTimestamp(s_mapInfo[i].sourceMapName, &sourceTime)
      && CompareFileTime(&requestedTime, &sourceTime) < 0)
    {
      printf("Newer source map: %s\n", s_mapInfo[i].sourceMapName);
    }
  }
  return i;
}

/* Native 0x41A520/0x41A590.  These helpers own the temporary lightgrid
   sidecar lifecycle: derive its name from the active map path, and discard
   an empty stream rather than leaving a zero-byte artifact behind. */
char *BuildLightgridPath(char *destination, const char *suffix)
{
  strcpy(destination, g_mapSourceFile);
  COM_StripExtension(destination, destination);
  return strcat(destination, suffix);
}

void CloseLightgridFile(FILE *stream, const char *suffix)
{
  char fileName[260];
  long length;

  if (!stream)
    return;

  length = ftell(stream);
  fclose(stream);
  if (!length)
  {
    BuildLightgridPath(fileName, suffix);
    remove(fileName);
  }
}

size_t WriteLightgridRecord(FILE *stream, const unsigned short *gridPosition)
{
  if (stream)
    return fwrite(gridPosition, sizeof(*gridPosition), 3, stream);
  return 0;
}

FILE *OnlyEnts_BeginMapLoad(void)
{
  char fileName[260];

  BuildLightgridPath(fileName, ".grid_auto");
  s_lightgridAutoFile = fopen(fileName, "wb");
  BuildLightgridPath(fileName, ".grid_not");
  s_lightgridNotFile = fopen(fileName, "wb");
  return s_lightgridNotFile;
}

void OnlyEnts_EndMapLoad(void)
{
  CloseLightgridFile(s_lightgridAutoFile, ".grid_auto");
  CloseLightgridFile(s_lightgridNotFile, ".grid_not");
  s_lightgridAutoFile = NULL;
  s_lightgridNotFile = NULL;
}

int EmitLightgridVolume(Brush_t *brush)
{
  int hasVolume = 0;
  int hasSky = 0;
  int sideIndex;
  FILE *stream;
  unsigned short gridMins[3];
  unsigned short gridMaxs[3];
  unsigned short gridPosition[3];
  float point[3];
  unsigned int x;
  unsigned int y;
  unsigned int z;

  for ( sideIndex = 0; sideIndex < brush->numSides; ++sideIndex )
  {
    const BrushSide_t *side = &brush->sides[sideIndex];
    const char *materialName;

    if ( side->bevel )
      continue;
    materialName = side->shaderInfo->name;
    if ( !strcmp(materialName, "lightgrid_volume") )
      hasVolume = 1;
    else if ( !strcmp(materialName, "lightgrid_sky") )
      hasSky = 1;
    else
      return 0;
  }

  if ( !CreateBrushWindings(brush) )
  {
    FreeBrushContents(brush);
    return 1;
  }

  if ( hasVolume && hasSky )
  {
    printf("Map %s, Entity %i, Brush %i: mixed lightgrid materials",
      MapInfo_GetName(brush->mapInfoIndex), brush->entityNum, brush->brushNum);
    FreeBrushContents(brush);
    return 1;
  }

  stream = hasVolume ? s_lightgridAutoFile : s_lightgridNotFile;
  gridMins[0] = (unsigned short)(int)floorf((brush->eMins[0] + 131072.0f) * (1.0f / 32.0f));
  gridMins[1] = (unsigned short)(int)floorf((brush->eMins[1] + 131072.0f) * (1.0f / 32.0f));
  gridMins[2] = (unsigned short)(int)floorf((brush->eMins[2] + 131072.0f) * (1.0f / 64.0f));
  gridMaxs[0] = (unsigned short)(int)floorf((brush->eMaxs[0] + 131072.0f) * (1.0f / 32.0f));
  gridMaxs[1] = (unsigned short)(int)floorf((brush->eMaxs[1] + 131072.0f) * (1.0f / 32.0f));
  gridMaxs[2] = (unsigned short)(int)floorf((brush->eMaxs[2] + 131072.0f) * (1.0f / 64.0f));

  for ( z = gridMins[2]; z <= gridMaxs[2]; ++z )
  {
    point[2] = (float)(((int)z << 6) - 0x20000);
    for ( y = gridMins[1]; y <= gridMaxs[1]; ++y )
    {
      point[1] = (float)(((int)y << 5) - 0x20000);
      for ( x = gridMins[0]; x <= gridMaxs[0]; ++x )
      {
        point[0] = (float)(((int)x << 5) - 0x20000);
        if ( BrushPointInsideNonBevelPlanes(brush, point) )
        {
          gridPosition[0] = (unsigned short)x;
          gridPosition[1] = (unsigned short)y;
          gridPosition[2] = (unsigned short)z;
          WriteLightgridRecord(stream, gridPosition);
        }
      }
    }
  }

  FreeBrushContents(brush);
  return 1;
}


/*
================
SetBrushContents

Sets content/surface flags for a brush by examining all sides.
================
*/
#define SURF_IGNORE_MASK  (SURF_NODAMAGE|SURF_NOIMPACT|SURF_NOMARKS|SURF_NOLIGHTMAP|SURF_POINTLIGHT|SURF_NOSTEPS)
#define SURF_PORTAL_BRUSH (SURF_PORTAL|SURF_NONSOLID|SURF_NODRAW)

void SetBrushContents(Brush_t *b)
{
  BrushSide_t *side;
  int contentFlags, surfaceFlags, allContentFlags;
  int mixed, i;

  /* initialize from first side */
  side = &b->sides[0];
  contentFlags = side->contentFlags;
  surfaceFlags = side->surfaceFlags;
  allContentFlags = contentFlags;
  b->contentShaderInfo = side->shaderInfo;
  mixed = 0;

  /* accumulate flags from remaining sides
     NOTE: surfaceFlags are read from smoothAngleAsInt (offset [88]) because
     during parse init, surfaceFlags are stored in the smoothAngle union field.
     The actual surfaceFlags field at [92] is not yet populated at this point. */
  for ( i = 1; i < b->numSides; i++ )
  {
    side = &b->sides[i];
    if ( !side->shaderInfo )
      continue;
    if ( side->contentFlags != contentFlags )
      mixed = 1;
    surfaceFlags |= side->surfaceFlags;
    allContentFlags |= side->contentFlags;
  }

  if ( mixed )
    Com_DPrintf("Map %s, Entity %i, Brush %i: mixed face contents\n",
                MapInfo_GetName(b->mapInfoIndex), b->entityNum, b->brushNum);

  /* check for detail & structural */
  if ( (contentFlags & CONTENTS_DETAIL) && (contentFlags & CONTENTS_STRUCTURAL) )
  {
    Com_Printf("Entity %i, Brush %i: mixed CONTENTS_DETAIL and CONTENTS_STRUCTURAL\n",
      num_entities - 1, g_numBrushes);
    contentFlags &= ~CONTENTS_DETAIL;
  }

  if ( fulldetail )
    contentFlags &= ~CONTENTS_DETAIL;

  /* all translucent brushes that aren't structural become detail */
  if ( (contentFlags & CONTENTS_TRANSLUCENT) && !(contentFlags & CONTENTS_STRUCTURAL) )
    contentFlags |= CONTENTS_DETAIL;

  if ( allContentFlags & CONTENTS_MANTLE )
    contentFlags |= CONTENTS_MANTLE;

  /* detail or structural */
  if ( contentFlags & CONTENTS_DETAIL )
  {
    ++*nummapplanes;
    b->detail = 1;
  }
  else
  {
    ++g_numMapEntities;
    b->detail = 0;
  }

  b->forceVisible = surfaceFlags < 0;
  b->isOpaque = !(contentFlags & CONTENTS_TRANSLUCENT);

  b->isOccluder = (contentFlags & CONTENTS_NONCOLLIDING) != 0;
  if ( !b->isOccluder && !b->isOpaque
    && (surfaceFlags & 0xFFFFDBCE) == 0x80004080 )
    b->isOccluder = 1;

  b->contentFlags = contentFlags;
}

/*
================
ValidateBrushSides

Validates brush sides, removes duplicates, checks for opposite planes.
Returns 1 if brush is valid, 0 if invalid (has opposite planes).
================
*/
int ValidateBrushSides(Brush_t *b)
{
  int i, j, planenum;

  for ( i = 1; i < b->numSides; )
  {
    planenum = b->sides[i].planenum;

    if ( planenum == -1 )
    {
      Com_Printf("Map %s, Entity %i, Brush %i: degenerate plane\n",
        MapInfo_GetName(b->mapInfoIndex), b->entityNum, b->brushNum);
      goto remove_side;
    }

    /* check against all previously validated sides */
    for ( j = 0; j < i; j++ )
    {
      if ( planenum == b->sides[j].planenum )
      {
        Com_Printf("Map %s, Entity %i, Brush %i: duplicate plane\n",
          MapInfo_GetName(b->mapInfoIndex), b->entityNum, b->brushNum);
        goto remove_side;
      }
      if ( planenum == (b->sides[j].planenum ^ 1) )
      {
        Com_Printf("Map %s, Entity %i, Brush %i: mirrored plane\n",
          MapInfo_GetName(b->mapInfoIndex), b->entityNum, b->brushNum);
        return 0;
      }
    }

    /* side is valid */
    i++;
    continue;

  remove_side:
  
    /* shift subsequent sides down */
    for ( j = i; j < b->numSides - 1; j++ )
      memcpy(&b->sides[j], &b->sides[j + 1], sizeof(BrushSide_t));
    b->numSides--;
  }

  return 1;
}

/*
================
FinalizeEntity

Finalizes an entity after parsing, linking its brushes and patches
into the global lists for BSP processing.
================
*/
void FinalizeEntity(Entity_t *ent, int parentEntity)
{
  Brush_t *b, *next;
  Patch_t *pm;

  /* move all brushes to worldspawn entity */
  for ( b = ent->brushes; b; b = next )
  {
    next = b->next;
    b->outputNum = parentEntity;
    b->next = g_entities[0].brushes;
    g_entities[0].brushes = b;
  }
  ent->brushes = NULL;

  /* move all patches to worldspawn entity */
  pm = ent->patches;
  if ( pm )
  {
    for ( ; pm->next; pm = pm->next )
      pm->outputNum = parentEntity;
    pm->outputNum = parentEntity;
    pm->next = g_entities[0].patches;
    g_entities[0].patches = ent->patches;
    ent->patches = NULL;
  }
}

/* Native 0x41A6C0 moves physics-material brushes out of the normal entity
   chain after parsing.  The two lists share the Brush_t next link, so retain
   a pointer-to-link while unlinking and prepend each selected brush. */
void SplitEntityPhysicsBrushes(Entity_t *ent)
{
  Brush_t **link;
  Brush_t *brush;

  Assert(ent, s_assertDisable_LoadPrefab);
  Assert(!ent->physicsBrushes, s_assertDisable_LoadPrefab);
  link = &ent->brushes;
  while ((brush = *link) != NULL)
  {
    if (brush->contentShaderInfo
      && (brush->contentShaderInfo->toolFlagsWord & TOOL_PHYSICS))
    {
      *link = brush->next;
      brush->next = ent->physicsBrushes;
      ent->physicsBrushes = brush;
    }
    else
    {
      link = &brush->next;
    }
  }
}

/*
================
LoadPrefab

Loads and parses a prefab .map file.
================
*/
void LoadPrefab(const char *prefabName, float *transformMtx, float brushScale,
                int parentEntity, int prefabFlags)
{
  void *fileData;
  char filePath[MAX_OS_PATH];

  Assert(prefabName, s_assertDisable_LoadPrefab);
  Assert(transformMtx, s_assertDisable_LoadPrefab);

  strcpy(filePath, FS_BuildOSPath(va("map_source\\%s", prefabName)));

  if ( LoadFile(filePath, &fileData) >= 0 )
  {
    ParseMapFile(filePath, _strdup(prefabName), fileData, transformMtx,
                 brushScale, parentEntity, prefabFlags);
    free(fileData);
  }
  else
  {
    printf("ERROR: Prefab '%s' not found\n", prefabName);
  }
}

/*
================
CreateNewFloatPlane

Creates a new plane entry in mapplanes array, plus its back side (plane+1).
Returns the plane index (even number, back side is index+1).
================
*/
int CreateNewFloatPlane(float *normal, float dist)
{
  Plane_t *p, *back, temp;
  int type, hash;

  Assert(Vec3IsNormalized(normal), s_assertDisable_CreateNewFloatPlane);

  if ( g_numMapPlanes + 2 > MAX_MAP_PLANES )
    Com_Error("MAX_MAP_PLANES");

  p = MAP_PLANE(g_numMapPlanes);
  back = MAP_PLANE(g_numMapPlanes + 1);

  /* set front plane */
  VectorCopy(normal, p->normal);
  p->dist = dist;

  /* determine plane type: 0=x, 1=y, 2=z axial, 3=non-axial */
  if ( p->normal[0] == 1.0 || p->normal[0] == -1.0 )
    type = 0;
  else if ( p->normal[1] == 1.0 || p->normal[1] == -1.0 )
    type = 1;
  else if ( p->normal[2] == 1.0 || p->normal[2] == -1.0 )
    type = 2;
  else
    type = 3;

  p->type = type;
  back->type = type;

  /* set back plane (negated normal and dist)
     NOTE: 0.0 - x instead of -x to avoid negative zero for zero components */
  back->normal[0] = 0.0 - normal[0];
  back->normal[1] = 0.0 - normal[1];
  back->normal[2] = 0.0 - normal[2];
  back->dist = -dist;

  g_numMapPlanes += 2;

  /* always put axial planes facing positive first */
  if ( type < 3 && (p->normal[0] < 0.0 || p->normal[1] < 0.0 || p->normal[2] < 0.0) )
  {
    /* flip front and back */
    temp = *p;
    *p = *back;
    *back = temp;

    /* insert swapped planes into hash */
    hash = ((int)fabs(p->dist) / PLANE_HASH_DIST_DIV) & (PLANE_HASH_SIZE - 1);
    p->hash_chain = g_planeHashTable[hash];
    g_planeHashTable[hash] = p;
    hash = ((int)fabs(back->dist) / PLANE_HASH_DIST_DIV) & (PLANE_HASH_SIZE - 1);
    back->hash_chain = g_planeHashTable[hash];
    g_planeHashTable[hash] = back;
    return g_numMapPlanes - 1;
  }

  /* insert planes into hash */
  hash = ((int)fabs(p->dist) / PLANE_HASH_DIST_DIV) & (PLANE_HASH_SIZE - 1);
  p->hash_chain = g_planeHashTable[hash];
  g_planeHashTable[hash] = p;
  hash = ((int)fabs(back->dist) / PLANE_HASH_DIST_DIV) & (PLANE_HASH_SIZE - 1);
  back->hash_chain = g_planeHashTable[hash];
  g_planeHashTable[hash] = back;
  return g_numMapPlanes - 2;
}

/*
================
SnapPlaneNormal

Snaps near-axial plane normals to exact axial (1,0,0), (0,1,0), (0,0,1).
================
*/
#define SNAP_NORMAL_EPSILON 1e-7

int SnapPlaneNormal(float *normal)
{
  int i;

  for ( i = 0; i < 3; i++ )
  {
    if ( fabs(normal[i] - 1.0) < SNAP_NORMAL_EPSILON )
    {
      VectorClear(normal);
      normal[i] = 1.0f;
      return 1;
    }
    if ( fabs(normal[i] + 1.0) < SNAP_NORMAL_EPSILON )
    {
      VectorClear(normal);
      normal[i] = -1.0f;
      return 1;
    }
  }
  return 0;
}

/*
================
FindFloatPlane

Finds or creates a plane in the global plane table.
Uses a hash table for fast lookup.
================
*/
int FindFloatPlane(float *normal, float dist)
{
  int hashBase, hashOffset;
  Plane_t *p;

  /* snap normal to axial if close */
  SnapPlaneNormal(normal);

  /* snap distance to integer if close */
  if ( fabs(dist - Q_rint(dist)) < PLANESIDE_EPSILON )
    dist = Q_rint(dist);

  /* search hash table with linear probing (-1, 0, +1) */
  hashBase = ((int)fabs(dist) / PLANE_HASH_DIST_DIV) & (PLANE_HASH_SIZE - 1);
  for ( hashOffset = -1; hashOffset <= 1; hashOffset++ )
  {
    p = g_planeHashTable[((unsigned short)hashOffset + (unsigned short)hashBase) & (PLANE_HASH_SIZE - 1)];

    /* walk hash chain */
    for ( ; p; p = p->hash_chain )
    {
      if ( fabs(p->normal[0] - normal[0]) < SNAP_NORMAL_EPSILON
        && fabs(p->normal[1] - normal[1]) < SNAP_NORMAL_EPSILON
        && fabs(p->normal[2] - normal[2]) < SNAP_NORMAL_EPSILON
        && fabs(p->dist - dist) < PLANESIDE_EPSILON )
      {
        return (int)(p - mapplanes);
      }
    }
  }

  /* not found — create new */
  return CreateNewFloatPlane(normal, dist);
}

/*
================
BestSideMaterialForNormal

Finds best-matching material for a given normal direction.
================
*/
ShaderInfo_t *BestSideMaterialForNormal(float *normal)
{
  double bestDot;
  ShaderInfo_t *best;
  BrushSide_t *side;
  float dot;
  int i;

  /* find side whose plane normal best matches the input normal */
  bestDot = 0.0;
  best = NULL;

  for ( i = 0; i < g_buildBrush->numSides; i++ )
  {
    side = &g_buildBrush->sides[i];
    if ( !side->shaderInfo )
      continue;

    dot = DotProduct210(MAP_PLANE(side->planenum)->normal, normal);
    if ( dot > bestDot )
    {
      best = side->shaderInfo;
      bestDot = dot;
    }
  }
  return best;
}

/*
================
AddEdgeBevels

Adds bevel planes for non-axial edges of brush sides.
Called from AddBrushBevels after axial bevels are added.
================
*/
#define NUM_AXIAL_BEVELS   6
#define MAX_BUILD_SIDES    300
#define BEVEL_MIN_EDGE_LEN 0.5f
#define BEVEL_MAX_Z        0.866f   /* sin(60 deg) */
#define BEVEL_SNAP_DIST    0.001

int AddEdgeBevels()
{
  BrushSide_t *ns;
  Winding_t *w;
  float edgeDir[3], bevelNormal[3];
  float *p, dist;
  int i, j, prev, dir, planeIdx, sideIdx;

  /* iterate non-axial sides (first 6 are axial bevels) */
  for ( i = NUM_AXIAL_BEVELS; i < g_buildBrush->numSides; i++ )
  {
    w = g_buildBrush->sides[i].winding;
    if ( !w )
      continue;

    prev = w->numpoints - 1;
    for ( j = 0; j < w->numpoints; j++ )
    {
      p = w->points[prev];
      VectorSubtract(w->points[j], w->points[prev], edgeDir);

      /* edge must be long enough, non-axial, and mostly horizontal */
      { double edgeLen = VecNormalize(edgeDir);
      if ( edgeLen < BEVEL_MIN_EDGE_LEN
        || SnapPlaneNormal(edgeDir)
        || edgeDir[2] > BEVEL_MAX_Z || edgeDir[2] < -BEVEL_MAX_Z )
      {
        prev = j;
        continue;
      } }

      /* bevel normal perpendicular to edge in XY plane */
      bevelNormal[0] = edgeDir[1];
      bevelNormal[1] = -edgeDir[0];
      bevelNormal[2] = 0.0f;
      VecNormalize(bevelNormal);

      /* try both directions */
      for ( dir = 0; dir < 2; dir++ )
      {
        bevelNormal[0] = -bevelNormal[0];
        bevelNormal[1] = -bevelNormal[1];
        bevelNormal[2] = -bevelNormal[2];

        dist = (float)((double)bevelNormal[0] * (double)p[0]
                     + (double)bevelNormal[1] * (double)p[1]
                     + (double)bevelNormal[2] * (double)p[2]);

        SnapPlaneNormal(bevelNormal);

        if ( fabs(dist - Q_rint(dist)) < BEVEL_SNAP_DIST )
          dist = Q_rint(dist);

        if ( TestBrushAgainstPlane(g_buildBrush, bevelNormal, dist, 0.5) != 1 )
          continue;

        planeIdx = FindFloatPlane(bevelNormal, dist);
        for ( sideIdx = 0; sideIdx < g_buildBrush->numSides; ++sideIdx )
        {
          if ( g_buildBrush->sides[sideIdx].planenum == planeIdx )
            break;
        }
        if ( sideIdx != g_buildBrush->numSides )
          continue;

        /* add new bevel side */
        if ( g_buildBrush->numSides == MAX_BUILD_SIDES )
          Com_Error("MAX_BUILD_SIDES");
        ns = &g_buildBrush->sides[g_buildBrush->numSides++];
        memset(ns, 0, sizeof(BrushSide_t));
        ns->planenum = planeIdx;
        ns->contentFlags = g_buildBrush->sides[0].contentFlags;
        ns->bevel = 1;
        ns->shaderInfo = BestSideMaterialForNormal(bevelNormal);
        ++g_numMapBrushes;

        /* reload winding pointer after potential realloc */
        w = g_buildBrush->sides[i].winding;
        p = w->points[prev];
      }

      prev = j;
    }
  }
  return i;
}

/*
================
AddBrushBevels

Adds axial bevel planes to brush.
================
*/
int AddBrushBevels()
{
  Brush_t *bb;
  BrushSide_t sidetemp;
  float normal[3];
  float dist;
  int axis, dir, i, orderIdx;

  bb = g_buildBrush;
  orderIdx = 0;

  /* add axial bevel planes for each axis and direction */
  for ( axis = 0; axis < 3; axis++ )
  {
    for ( dir = -1; dir <= 1; dir += 2 )
    {
      /* find side matching this axis direction */
      dist = (float)dir;
      for ( i = 0; i < bb->numSides; i++ )
      {
        if ( MAP_PLANE(bb->sides[i].planenum)->normal[axis] == dist )
          break;
      }

      /* not found — create new axial bevel */
      if ( i == bb->numSides )
      {
        if ( bb->numSides == MAX_BUILD_SIDES )
          Com_Error("MAX_BUILD_SIDES");

        memset(&bb->sides[i], 0, sizeof(BrushSide_t));
        VectorClear(normal);
        normal[axis] = (float)dir;
        dist = (dir == 1) ? bb->eMaxs[axis] : -bb->eMins[axis];
        bb->numSides++;
        bb->sides[i].planenum = FindFloatPlane(normal, dist);
        bb->sides[i].contentFlags = bb->sides[0].contentFlags;
        bb->sides[i].bevel = 1;
        bb->sides[i].shaderInfo = BestSideMaterialForNormal(normal);
        ++g_numMapDrawSurfs;
      }

      /* swap found/created side to orderIdx position */
      if ( i != orderIdx )
      {
        memcpy(&sidetemp, &bb->sides[orderIdx], sizeof(BrushSide_t));
        memcpy(&bb->sides[orderIdx], &bb->sides[i], sizeof(BrushSide_t));
        memcpy(&bb->sides[i], &sidetemp, sizeof(BrushSide_t));
      }
      orderIdx++;
    }
  }

  return AddEdgeBevels();
}

/*
================
AddBrushToWorld

Adds a completed brush to the world/entity brush list.
================
*/
#define CONTENTS_CLIP_MASK (CONTENTS_MISSILECLIP | CONTENTS_VEHICLECLIP | CONTENTS_AI_NOSIGHT | CONTENTS_CLIPSHOT | CONTENTS_PLAYERCLIP | CONTENTS_MONSTERCLIP)
/* TOOL_ORIGIN already defined in cod4map.h */

Brush_t *AddBrushToWorld(int mapInfoIndex)
{
  Brush_t *copy;

  /* create winding polygons for each brush side */
  if ( !CreateBrushWindings(g_buildBrush) )
  {
    FreeBrushContents(g_buildBrush);
    return NULL;
  }

  /* clip brushes become detail */
  if ( g_buildBrush->contentFlags & CONTENTS_CLIP_MASK )
  {
    g_buildBrush->detail = 1;
    ++*nummapplanes;
  }

  /* origin brush — compute entity origin from brush center */
  if ( g_buildBrush->sides[0].shaderInfo->toolFlagsWord & TOOL_ORIGIN )
  {
    if ( num_entities == 1 )
    {
      Com_Error("Entity %i, Brush %i: origin brushes not allowed in world\n", 0, g_numBrushes);
    }
    else
    {
      float origin[3];
      char buf[32];
      int axis;

      for ( axis = 0; axis < 3; ++axis )
      {
        origin[axis] = floorf(
          (g_buildBrush->eMins[axis] + g_buildBrush->eMaxs[axis]) * 0.5f
          + 0.5f);
      }
      sprintf(buf, "%.0f %.0f %.0f", origin[0], origin[1], origin[2]);
      SetKeyValue(&g_entities[num_entities - 1], "origin", buf);
      VectorCopy(origin, g_entities[num_entities - 1].origin);
    }
    FreeBrushContents(g_buildBrush);
    return NULL;
  }

  /* normal brush — compact, bevel, copy, link into entity */
  CompactBrushSides();
  AddBrushBevels();

  copy = CopyBrush(g_buildBrush);

  FreeBrushContents(g_buildBrush);

  copy->entityNum = g_parseEntityCount - 1;
  copy->brushNum = g_numBrushes;
  copy->mapInfoIndex = mapInfoIndex;
  copy->original = copy;

  copy->next = g_currentEntity->brushes;
  g_currentEntity->brushes = copy;

  return copy;
}

/*
================
ParseBrushSideTexture

Parses material name and texture coordinates for a brush side.
================
*/
ShaderInfo_t *ParseBrushSideTexture(char **parsePtr, float *texVecs, double a3, const char *mapName, float *rawPlane, float *transformMtx, float brushScale, float texScaleFactor)
{
  char *materialName;
  ShaderInfo_t *si;
  float texBuf[2][4];
  float texOffset[2], texScale[2];
  float texRotation, texAngle;
  double scaleRatio;

  Com_SetSpaceDelimited(1);
  materialName = COM_ParseExt(parsePtr);
  Com_SetSpaceDelimited(0);
  si = LoadMaterial(materialName);

  texScale[0] = COM_ParseFloat(parsePtr);
  texScale[1] = COM_ParseFloat(parsePtr);
  texOffset[0] = COM_ParseFloat(parsePtr);
  texOffset[1] = COM_ParseFloat(parsePtr);
  texRotation = COM_ParseFloat(parsePtr);
  texAngle = COM_ParseFloat(parsePtr);

  if ( texScale[0] == 0.0 || texScale[1] == 0.0 || texScale[0] != texScale[0] || texScale[1] != texScale[1] )
    Com_Error("Map %s, Entity %i, Brush %i:  Material with 0 texture scale.\nOpen and re-save '%s' from Radiant to fix this.", g_mapSourceFile, g_parseEntityCount - 1, g_numBrushes, g_mapSourceFile);

  if ( !strcmp(materialName, "lightmap_gray") )
  {
    float roundedStep;

    texOffset[0] = 0.0f;
    texOffset[1] = 0.0f;
    roundedStep = (float)(floor(((double)texRotation + 45.0) / 90.0) * 90.0);
    texRotation -= roundedStep;
    texScale[0] = (float)fabs((double)texScale[0]);
    texScale[1] = (float)fabs((double)texScale[1]);
  }

  BuildTextureVecs(rawPlane, texScale, texOffset, texRotation, texAngle, texBuf[0]);

  /* transform and scale S texture vector */
  texBuf[0][3] = -texBuf[0][3];

  TransformPlane(transformMtx, brushScale, texBuf[0], texVecs);
  scaleRatio = (double)texScaleFactor / (double)brushScale;
  texVecs[0] = scaleRatio * texVecs[0];
  texVecs[1] = scaleRatio * texVecs[1];
  texVecs[2] = scaleRatio * texVecs[2];
  texVecs[3] = -(scaleRatio * texVecs[3]);

  /* transform and scale T texture vector */
  texBuf[1][3] = -texBuf[1][3];
  TransformPlane(transformMtx, brushScale, texBuf[1], texVecs + 4);
  texVecs[7] = -texVecs[7];
  {
    volatile float scaleRatioF = (float)scaleRatio;
    texVecs[4] = scaleRatioF * texVecs[4];
    texVecs[5] = scaleRatioF * texVecs[5];
    texVecs[6] = scaleRatioF * texVecs[6];
    texVecs[7] = scaleRatioF * texVecs[7];
  }

  return si;
}

/*
================
ParseRawBrush

Parses brush geometry from .map file.
================
*/
#define EQUAL_EPSILON_SQ 0.0000010000001f

static void ParseOptionalMapString(char **parsePtr, const char *keyword, const char *defaultValue, char *destination)
{
  char *token;

  token = COM_Parse(parsePtr);
  if ( !strcmp(token, keyword) )
  {
    token = COM_ParseExt(parsePtr);
    strcpy(destination, *token ? token : defaultValue);
    return;
  }

  Com_UngetToken();
  strcpy(destination, defaultValue);
}

static int ParseSmoothing(char **parsePtr)
{
  char smoothingName[1028];

  ParseOptionalMapString(parsePtr, "smoothing", "smoothing_hard", smoothingName);
  if ( !_stricmp(smoothingName, "smoothing_hard") )
    return 0;
  if ( !_stricmp(smoothingName, "smoothing_smooth") )
    return 1;
  if ( !_stricmp(smoothingName, "smoothing_smooth_other") )
    return 2;

  Com_Error("ParseSmoothing(): Unknown smoothing name '%s'", smoothingName);
  return 0;
}

char *ParseRawBrush(char **parsePtr, float *transformMtx, float brushScale, const char *mapName)
{
  Brush_t *bb;
  BrushSide_t *side;
  double scaleSquared;
  float planePoint0[3], planePoint1[3], planePoint2[3];
  float transformedPlane[4], rawPlane[4];
  char *result;
  const char *token;
  int brushFlags, toolFlags;
  char layerName[1028];

  bb = g_buildBrush;
  scaleSquared = brushScale * brushScale;

  Assert(scaleSquared > EQUAL_EPSILON_SQ, s_assertDisable_ParseRawBrush);

  bb->numSides = 0;
  bb->detail = 0;

  /* parse content flags — single call, no loop (matches original binary) */
  ParseOptionalMapString(parsePtr, "layer", "000_Global", layerName);
  brushFlags = ParseContentsFlags(parsePtr);

  toolFlags = ParseToolFlags(parsePtr);

  /* parse brush sides */
  for ( result = COM_Parse(parsePtr); *result; result = COM_Parse(parsePtr) )
  {
    token = result;
    result = 0;
    if ( !strcmp(token, "}") )
      break;

    Com_UngetToken();

    if ( bb->numSides == MAX_BUILD_SIDES )
      Com_Error("MAX_BUILD_SIDES (%i) -- brush too complex", MAX_BUILD_SIDES);

    /* parse 3 plane points */
    COM_Parse1DMatrix(parsePtr, 3, planePoint0);
    COM_Parse1DMatrix(parsePtr, 3, planePoint1);
    COM_Parse1DMatrix(parsePtr, 3, planePoint2);

    /* The native parser discards a degenerate side before reserving a
       BuildBrush slot, then skips the rest of its texture line. */
    if ( !PlaneFromPoints(rawPlane, planePoint0, planePoint1, planePoint2) )
    {
      Com_SkipRestOfLine(parsePtr);
      continue;
    }

    side = &bb->sides[bb->numSides];
    memset(side, 0, sizeof(BrushSide_t));
    ++bb->numSides;

    /* compute and register plane */
    TransformPlane(transformMtx, brushScale, rawPlane, transformedPlane);
    side->planenum = FindFloatPlane(transformedPlane, transformedPlane[3]);

    /* parse texture layer and lightmap layer */
    side->shaderInfo = ParseBrushSideTexture(parsePtr, side->texVecs[0], scaleSquared, mapName, rawPlane, transformMtx, brushScale, 1.0);
    side->lmShaderInfo = ParseBrushSideTexture(parsePtr, side->lmTexVecs[0], scaleSquared, mapName, rawPlane, transformMtx, brushScale, sampleScale);
    side->compileFlags = ParseSmoothing(parsePtr);

    /* set content flags from material, then apply brush flags */
    side->contentFlags = side->shaderInfo->contentFlags;

    if ( brushFlags & CONTENTS_DETAIL )
      side->contentFlags |= CONTENTS_DETAIL;
    if ( brushFlags & CONTENTS_NONCOLLIDING )
      side->contentFlags |= CONTENTS_NONCOLLIDING;
    if ( brushFlags & (CONTENTS_CLIPSHOT | CONTENTS_MISSILECLIP) )
      side->contentFlags &= ~(CONTENTS_DETAIL | CONTENTS_SOLID);
    if ( brushFlags & CONTENTS_CLIPSHOT )
      side->contentFlags |= CONTENTS_CLIPSHOT;
    if ( brushFlags & CONTENTS_MISSILECLIP )
      side->contentFlags |= CONTENTS_MISSILECLIP;

    side->surfaceFlags = side->shaderInfo->surfaceFlags;
    side->toolFlags = toolFlags | side->shaderInfo->toolFlagsWord;
  }
  return result;
}


/*
================
FinishBrush

Finalizes brush after parsing: validates sides, creates windings, adds to entity.
================
*/
int FinishBrush(char **parsePtr, float *transformMtx, float brushScale,
                int mapInfoIndex, int prefabFlags)
{
  int result;
  Brush_t *bb;
  const char *mapName;

  mapName = MapInfo_GetName(mapInfoIndex);

  /* parse brush sides from map file */
  ParseRawBrush(parsePtr, transformMtx, brushScale, mapName);

  bb = g_buildBrush;
  bb->parentBrush = -1;
  bb->siblingBrush = -1;
  bb->entityNum = g_parseEntityCount - 1;
  bb->brushNum = g_numBrushes;
  bb->mapInfoIndex = mapInfoIndex;

  result = ValidateBrushSides(g_buildBrush);

  if ( result )
  {
    if ( EmitLightgridVolume(g_buildBrush) )
      return result;

    SetBrushContents(g_buildBrush);

    if ( nodetail && (bb->contentFlags & CONTENTS_DETAIL)
      || nowater && (bb->contentFlags & CONTENTS_WATER) )
    {
      FreeBrushContents(g_buildBrush);
    }
    else
    {
      int sideIndex;

      if ( prefabFlags & 1 )
      {
        for ( sideIndex = 0; sideIndex < bb->numSides; ++sideIndex )
        {
          if ( !strncmp(bb->sides[sideIndex].shaderInfo->name, "clip", 4) )
            return result;
        }
      }
      AddBrushToWorld(mapInfoIndex);
    }
  }

  return result;
}

/*
================
AdjustBrushesForOrigin

Adjusts brush planes and patch vertices for entity origin.
================
*/
static void ComputeEntityOrigin(Entity_t *ent)
{
  Brush_t *brush;
  Patch_t *patch;
  float mins[3], maxs[3], origin[3];
  char originText[32];
  int vertex, vertexCount;

  Assert(ent, s_assertDisable_AdjustBrushesForOrigin);
  ClearBounds(mins, maxs);
  for (brush = ent->brushes; brush; brush = brush->next)
  {
    if (brush->numSides)
    {
      AddPointToBounds(brush->eMins, mins, maxs);
      AddPointToBounds(brush->eMaxs, mins, maxs);
    }
  }
  for (patch = ent->patches; patch; patch = patch->next)
  {
    vertexCount = patch->height * patch->width;
    for (vertex = 0; vertex < vertexCount; ++vertex)
      AddPointToBounds(patch->vertexData[vertex].pos, mins, maxs);
  }
  for (vertex = 0; vertex < 3; ++vertex)
    origin[vertex] = floorf((mins[vertex] + maxs[vertex]) * 0.5f + 0.5f);
  sprintf(originText, "%.0f %.0f %.0f", origin[0], origin[1], origin[2]);
  SetKeyValue(ent, "origin", originText);
  VectorCopy(origin, ent->origin);
}

void AdjustBrushesForOrigin(Entity_t *ent)
{
  Brush_t *b;
  BrushSide_t *side;
  float *plane;
  float newdist;
  Patch_t *pm;
  MeshVert_t *mv;
  int i, r;

  Assert(!(!ent), s_assertDisable_AdjustBrushesForOrigin);
  Assert(!(!ent->brushes && !ent->patches), s_assertDisable_AdjustBrushesForOrigin);
  
  /* adjust brush planes for entity origin */
  for ( b = ent->brushes; b; b = b->next )
  {
    for ( i = 0; i < b->numSides; i++ )
    {
      side = &b->sides[i];
      plane = MAP_PLANE(side->planenum)->normal;
      newdist = MAP_PLANE(side->planenum)->dist - DotProduct210(plane, ent->origin);
      side->planenum = FindFloatPlane(plane, newdist);
      if ( side->winding )
      {
        FreeWinding(side->winding);
        side->winding = NULL;
      }
      side->texVecs[0][3] += DotProduct201(side->texVecs[0], ent->origin);
      side->texVecs[1][3] += DotProduct120(side->texVecs[1], ent->origin);
      side->lmTexVecs[0][3] += DotProduct120(side->lmTexVecs[0], ent->origin);
      side->lmTexVecs[1][3] += DotProduct120(side->lmTexVecs[1], ent->origin);
    }
    CreateBrushWindings(b);
  }
  
  /* adjust patch vertices for entity origin */
  for ( pm = ent->patches; pm; pm = pm->next )
  {
    mv = pm->vertexData;
    for ( i = 0; i < pm->height * pm->width; i++ )
    {
      VectorSubtract(mv[i].pos, ent->origin, mv[i].pos);
    }
  }
}

/*
================
ParseMapEntity

Parses a single entity from the map file.
================
*/
int ParseMapEntity(char **parsePtr, float *transformMtx, float brushScale,
                   int mapInfoIndex, int parentPrefab, int prefabFlags)
{
  char *token, *sub;
  Epair_t *ep;
  const char *val;
  
  /* originAndMatrix: [0..2]=origin, [3..11]=rotation matrix (must be contiguous for TransformPoint) */
  float originAndMatrix[12];
  #define entityRotation (&originAndMatrix[3])
  float childTransform[12];
  float entityAngles[3];
  const char *mapName;

  mapName = MapInfo_GetName(mapInfoIndex);

  token = COM_Parse(parsePtr);
  if ( !*token )
    return 0;

  if ( strcmp(token, "{") )
  {
    Com_Error(
      "ParseEntity: { not found, found %s on line %d - last entity was at: <%4.2f, %4.2f, %4.2f>...",
      token,
      Com_GetCurrentParseLine(),
      g_entities[num_entities].origin[0],
      g_entities[num_entities].origin[1],
      g_entities[num_entities].origin[2]);
  }

  if ( num_entities == MAX_MAP_ENTITIES )
    Com_Error("num_entities == MAX_MAP_ENTITIES");

  g_numBrushes = 0;
  g_currentEntity = &g_entities[num_entities];
  memset(g_currentEntity, 0, sizeof(Entity_t));
  g_currentEntity->entityNum = g_parseEntityCount;
  g_currentEntity->mapInfoIndex = mapInfoIndex;
  ++g_parseEntityCount;
  ++num_entities;
  while ( 1 )
  {
    token = COM_Parse(parsePtr);
    if ( !*token )
      Com_Error("ParseEntity: EOF without closing brace");
    if ( !strcmp(token, "}") )
      break;

    if ( !strcmp(token, "{") )
    {
      /* brush or patch primitive */
      sub = COM_Parse(parsePtr);
      if ( !*sub )
        break;
      if ( !strcmp(sub, "curve") )
      {
        ++g_numPatches;
        ParsePatch(0.0, parsePtr, 0, transformMtx, brushScale, mapName);
        ++g_numBrushes;
      }
      else
      {
        if ( !strcmp(sub, "mesh") )
        {
          ++g_numPatches;
          ParsePatch(0.0, parsePtr, 1, transformMtx, brushScale, mapName);
        }
        else
        {
          Com_UngetToken();
          FinishBrush(parsePtr, transformMtx, brushScale, mapInfoIndex,
            prefabFlags);
        }
        ++g_numBrushes;
      }
    }
    else
    {
      /* key-value pair */
      ep = ParseEPair(token, parsePtr);
      ep->next = g_currentEntity->epairs;
      g_currentEntity->epairs = ep;
    }
  }
  
  /* Native treats func_cullgroup as the older spelling of func_group. */
  val = ValueForKey(g_currentEntity, "classname");
  if ( !strcmp(val, "func_cullgroup") )
  {
    SetKeyValue(g_currentEntity, "classname", "func_group");
    val = ValueForKey(g_currentEntity, "classname");
  }

  /* Native 0x41CB20 gives every non-world brush model a rounded bounds
     center when the map did not supply an origin brush, then localizes its
     geometry.  func_group geometry remains in its parent coordinate space. */
  if ( strcmp(val, "func_group") )
  {
    if ( g_currentEntity->origin[0] == 0.0
      && g_currentEntity->origin[1] == 0.0
      && g_currentEntity->origin[2] == 0.0 )
    {
      if ( strcmp(val, "worldspawn")
        && (g_currentEntity->brushes || g_currentEntity->patches) )
      {
        Assert(num_entities != 1, s_assertDisable_AdjustBrushesForOrigin);
        ComputeEntityOrigin(g_currentEntity);
        AdjustBrushesForOrigin(g_currentEntity);
      }
    }
    else if ( g_currentEntity->brushes || g_currentEntity->patches )
    {
      AdjustBrushesForOrigin(g_currentEntity);
    }
    else
    {
        Com_Error(
          "Map %s, Entity %i, Brush 0, Origin %f %f %f: Model is an origin brush only",
          mapName, g_parseEntityCount - 1,
          g_currentEntity->origin[0], g_currentEntity->origin[1], g_currentEntity->origin[2]);
    }
  }

  /* compute child transform matrix */
  if ( g_currentEntity->brushes || g_currentEntity->patches || g_currentEntity->terrainList )
  {
    memcpy(childTransform, transformMtx, sizeof(childTransform));
  }
  else
  {
    GetVectorForKey(g_currentEntity, "origin", originAndMatrix);
    GetVectorForKey(g_currentEntity, "angles", entityAngles);
    AnglesToMatrix(entityAngles, entityRotation);
    TransformPoint(originAndMatrix, transformMtx, childTransform);
    ApplyRotationToAngles(&childTransform[3], entityAngles);

    g_currentEntity->origin[0] = childTransform[0];
    g_currentEntity->origin[1] = childTransform[1];
    g_currentEntity->origin[2] = childTransform[2];
    SetKeyValue(g_currentEntity, "origin",
      va(FMT_VECTOR3, g_currentEntity->origin[0], g_currentEntity->origin[1], g_currentEntity->origin[2]));

    if ( !VectorCompare(entityAngles, vec3_origin_float) || *ValueForKey(g_currentEntity, "angles") )
    {
      /* +0.0f flushes negative zero (MSVC6 prints "0" for -0, modern prints "-0") */
      SetKeyValue(g_currentEntity, "angles",
        va(FMT_VECTOR3, entityAngles[0] + 0.0f, entityAngles[1] + 0.0f, entityAngles[2] + 0.0f));
    }
  }

  /* classify entity */
  val = ValueForKey(g_currentEntity, "classname");

  if ( !_stricmp(val, "worldspawn") )
  {
    SetReflectionProbeColorCorrection(g_currentEntity);
    SetReflectionProbeIgnorePortals(g_currentEntity);
    SetCollisionNodeLimitFromWorldspawn(g_currentEntity);
    SetBlockSizeFromWorldspawn(g_currentEntity);
  }

  if ( !strcmp(val, "group_info") )
  {
    --num_entities;
    return 1;
  }
  if ( !strcmp(val, "func_group")
    || !strcmp(val, "worldspawn") && num_entities > 1 )
  {
    FinalizeEntity(g_currentEntity, -1);
    --num_entities;
    return 1;
  }
  if ( !strcmp(val, "reflection_probe") )
  {
    AddReflectionProbe(g_currentEntity);
    --num_entities;
    return 1;
  }
  if ( strcmp(val, "func_cullgroup") )
  {
    if ( !strcmp(val, "misc_prefab") )
    {
      val = ValueForKey(g_currentEntity, "model");
      if ( !*val )
        Com_Error("misc_prefab at (%g %g %g) has no model",
          g_currentEntity->origin[0], g_currentEntity->origin[1], g_currentEntity->origin[2]);
      --num_entities;
      if ( IntForKey(g_currentEntity, "spawnflags") & 1 )
        prefabFlags |= 1;
      LoadPrefab(val, childTransform, brushScale, num_entities, prefabFlags);
    }
    if ( parentPrefab != NO_PARENT_PREFAB )
    {
      val = ValueForKey(g_currentEntity, "targetname");
      if ( !strncmp(val, "auto", 4) )
        SetKeyValue(g_currentEntity, "targetname", va("pf%i_%s", parentPrefab, val));
      val = ValueForKey(g_currentEntity, "target");
      if ( !strncmp(val, "auto", 4) )
        SetKeyValue(g_currentEntity, "target", va("pf%i_%s", parentPrefab, val));
      val = ValueForKey(g_currentEntity, "target");
      if ( !strncmp(val, "auto", 4) )
        SetKeyValue(g_currentEntity, "target", va("pf%i_%s", parentPrefab, val));
      val = ValueForKey(g_currentEntity, "script_exploder");
      if ( *val )
      {
        SetKeyValue(g_currentEntity, "script_exploder", va("%i%s", parentPrefab, val));
      }
    }
    NormalizeScriptColorTokens(g_currentEntity->epairs, parentPrefab,
      "script_color_allies");
    NormalizeScriptColorTokens(g_currentEntity->epairs, parentPrefab,
      "script_color_axis");
    SplitEntityPhysicsBrushes(g_currentEntity);
    return 1;
  }

  /* func_cullgroup */
  if ( numBSPCullGroups >= MAX_MAP_CULLGROUPS )
    Com_Error("MAX_MAP_CULLGROUPS (%i) exceeded\n", MAX_MAP_CULLGROUPS);
  FinalizeEntity(g_currentEntity, numBSPCullGroups);
  ++numBSPCullGroups;
  --num_entities;
  #undef entityRotation
  return 1;
}

/*
================
ParseCollisionMapEntity

Parses a single entity from a collision map file.
================
*/
int ParseCollisionMapEntity(int mapInfoIndex, char **parsePtr, float *origin, float *angles, float modelScale)
{
  char *token, *sub;
  float rotMatrix[9];
  float fullTransform[12];
  int i;

  token = COM_Parse(parsePtr);
  if ( !*token )
    return 0;
  if ( strcmp(token, "{") )
    Com_Error("ParseCollMapEntity: { not found, found %s on line %d", token, Com_GetCurrentParseLine());

  if ( num_entities == MAX_MAP_ENTITIES )
    Com_Error("num_entities == MAX_MAP_ENTITIES");

  g_numBrushes = 0;
  g_currentEntity = &g_entities[num_entities];
  memset(g_currentEntity, 0, sizeof(Entity_t));

  /* build full transform: [0..2]=origin, [3..11]=rotation matrix */
  AnglesToMatrix(angles, rotMatrix);
  VectorCopy(origin, fullTransform);
  for ( i = 0; i < 9; i++ )
    fullTransform[3 + i] = rotMatrix[i];

  /* parse brushes */
  while ( 1 )
  {
    token = COM_Parse(parsePtr);
    if ( !*token )
      Com_Error("ParseEntity: EOF without closing brace");
    if ( !strcmp(token, "}") )
      break;

    if ( !strcmp(token, "{") )
    {
      sub = COM_Parse(parsePtr);
      if ( !*sub )
        break;
      if ( !strcmp(sub, "curve") )
      {
        ++g_numPatches;
        Com_Error("cannot have curves in collision maps");
        ++g_numBrushes;
      }
      else
      {
        if ( !strcmp(sub, "mesh") )
        {
          ++g_numPatches;
          Com_Error("cannot have patch terrain in collision maps");
        }
        else if ( !strcmp(sub, "physics_cylinder") )
        {
          Com_Error("cannot have physics cylinders in collision maps");
        }
        else if ( !strcmp(sub, "physics_box") )
        {
          Com_Error("cannot have physics boxes in collision maps");
        }
        else
        {
          Com_UngetToken();
          FinishBrush(parsePtr, fullTransform, modelScale, mapInfoIndex, 0);
        }
        ++g_numBrushes;
      }
    }
    else
    {
      Epair_t *ep = ParseEPair(token, parsePtr);
      ep->next = g_currentEntity->epairs;
      g_currentEntity->epairs = ep;
    }
  }

  FinalizeEntity(g_currentEntity, -1);
  return 1;
}

/*
================
LoadCollisionMap

Loads a collision map file for an xmodel/misc_model.
================
*/
void LoadCollisionMap(char *collmapPath, const char *modelName, float *origin, float *angles, float modelScale)
{
  char *versionStr;
  void *fileData;
  char *parsePtr;
  int mapInfoIndex;

  mapInfoIndex = MapInfo_Add(collmapPath, origin, angles, modelScale);

  if ( LoadFile(collmapPath, &fileData) >= 0 )
  {
    InitMapParsing(collmapPath);
    parsePtr = fileData;
    if ( strcmp(COM_Parse(&parsePtr), "iwmap") )
      Com_Error("collmap '%s' is missing 'iwmap' version specification\n", collmapPath);
    versionStr = COM_Parse(&parsePtr);
    if ( atol(versionStr) != IWMAP_VERSION )
      Com_Error("collmap '%s' is version '%s'; should be version '%i'\n", collmapPath, versionStr, IWMAP_VERSION);
    SkipMapLayerInfo(&parsePtr);
    while ( ParseCollisionMapEntity(mapInfoIndex, &parsePtr, origin, angles, modelScale) )
      ;
    Com_EndParseSession();
    free(fileData);
  }
  else if ( displayCollMapWarnings )
  {
    Com_Printf("WARNING: No collision map file for misc_model \"%s\"\n", modelName);
  }
}

/*
================
ProcessMiscModels

Loads collision maps for all misc_model entities.
================
*/
#define MIN_MODEL_SCALE 0.001f
#define DEFAULT_MODEL_SCALE 1.0f

int ProcessMiscModels()
{
  Entity_t *ent;
  const char *model;
  float entityAngles[3], entityOrigin[3];
  float scale;
  char collmapPath[MAX_OS_PATH], baseName[MAX_OS_PATH];
  int i;

  for ( i = 1; i < num_entities; i++ )
  {
    ent = &g_entities[i];
    if ( Q_stricmp("misc_model", ValueForKey(ent, "classname")) )
      continue;

    model = ValueForKey(ent, "model");
    if ( !*model )
      continue;

    /* build collision map path from model name */
    FS_ExtractBasename(model, baseName);
    sprintf(collmapPath, "%s/%s.map", FS_BuildOSPath("collmaps"), baseName);

    /* get entity transform */
    GetVectorForKey(ent, "origin", entityOrigin);
    GetVectorForKey(ent, "angles", entityAngles);
    scale = FloatForKey(ent, "modelscale");
    if ( scale <= MIN_MODEL_SCALE )
      scale = DEFAULT_MODEL_SCALE;

    LoadCollisionMap(collmapPath, model, entityOrigin, entityAngles, scale);
  }
  return num_entities;
}

/*
================
ParseVehicleCollMap

Parses vehicle collision map entities.
================
*/
int ParseVehicleCollMap(char **parsePtr, const char *vehicleName, int mapInfoIndex)
{
  char *token, *sub;
  float identityMtx[12];

  /* validate iwmap header */
  if ( strcmp(COM_Parse(parsePtr), "iwmap") )
    Com_Error("collmap '%s' is missing 'iwmap' version specification\n", vehicleName);
  token = COM_Parse(parsePtr);
  if ( atol(token) != IWMAP_VERSION )
    Com_Error("collmap '%s' is version '%s'; should be version '%i'\n", vehicleName, token, IWMAP_VERSION);

  /* CoD4 vehicle collmaps can carry the same layer-header lines as regular
     iwmap files (for example \"000_Global\" flags active).  Native
     ParseVehicleCollMap (0x41C480) consumes every non-entity line before
     accepting the first opening brace. */
  while ( 1 )
  {
    token = COM_Parse(parsePtr);
    if ( !*token )
      return 0;
    if ( !strcmp(token, "{") )
      break;
    Com_SkipRestOfLine(parsePtr);
  }

  if ( num_entities == MAX_MAP_ENTITIES )
    Com_Error("num_entities == MAX_MAP_ENTITIES");

  g_numBrushes = 0;
  g_currentEntity = &g_entities[num_entities];
  memset(g_currentEntity, 0, sizeof(Entity_t));
  ++g_parseEntityCount;
  ++num_entities;

  /* identity transform: origin(0,0,0) + identity 3x3 rotation */
  memset(identityMtx, 0, sizeof(identityMtx));
  identityMtx[3] = 1.0f;
  identityMtx[7] = 1.0f;
  identityMtx[11] = 1.0f;

  /* parse brushes */
  while ( 1 )
  {
    token = COM_Parse(parsePtr);
    if ( !*token )
      Com_Error("ParseVehicleCollMap: EOF without closing brace");
    if ( !strcmp(token, "}") )
      break;

    if ( !strcmp(token, "{") )
    {
      sub = COM_Parse(parsePtr);
      if ( !*sub )
        break;
      if ( !strcmp(sub, "curve") )
      {
        Com_Error("cannot have curves in collision maps");
        ++g_numBrushes;
      }
      else
      {
        if ( !strcmp(sub, "mesh") )
          Com_Error("cannot have patch terrain in collision maps");
        else
        {
          Com_UngetToken();
          FinishBrush(parsePtr, identityMtx, DEFAULT_MODEL_SCALE,
            mapInfoIndex, 0);
        }
        ++g_numBrushes;
      }
    }
    else
    {
      ParseEPair(token, parsePtr);
    }
  }

  SetKeyValue(&g_entities[num_entities - 1], "targetname", vehicleName);
  SetKeyValue(&g_entities[num_entities - 1], "classname", "script_vehicle_collmap");
  return 1;
}

/*
================
ProcessScriptVehicles

Loads collision maps for script_vehicle entities not already
provided by script_vehicle_collmap.
================
*/
int ProcessScriptVehicles()
{
  Entity_t *ent;
  const char *model;
  void *fileData;
  char *parsePtr;
  char baseName[MAX_OS_PATH], collmapPath[MAX_OS_PATH];
  int i, j;

  for ( i = 1; i < num_entities; i++ )
  {
    ent = &g_entities[i];
    /* CoD4 0x41C1B0 accepts both vehicle entity classes before looking up
       their collision-map companion.  The resulting brush set is consumed
       by the later visible-hull clipping pass. */
    if ( Q_stricmp("script_vehicle", ValueForKey(ent, "classname"))
      && Q_stricmp("script_vehicle_mp", ValueForKey(ent, "classname")) )
      continue;

    model = ValueForKey(ent, "model");
    if ( !*model )
      continue;

    /* build collision map path */
    FS_ExtractBasename(model, baseName);
    sprintf(collmapPath, "%s%s.map", FS_BuildOSPath("collmaps/"), baseName);

    /* check if a script_vehicle_collmap already provides collision */
    for ( j = 0; j < num_entities; j++ )
    {
      if ( j == i )
        continue;
      if ( !Q_stricmp("script_vehicle_collmap", ValueForKey(&g_entities[j], "classname"))
        && !Q_stricmp(model, ValueForKey(&g_entities[j], "targetname")) )
        break;
    }

    /* no existing collmap — load from file */
    if ( j == num_entities )
    {
      if ( LoadFile(collmapPath, &fileData) >= 0 )
      {
        InitMapParsing(collmapPath);
        parsePtr = fileData;
        {
          int mapInfoIndex = MapInfo_Add(collmapPath, vec3_origin_float,
            vec3_origin_float, DEFAULT_MODEL_SCALE);
          ParseVehicleCollMap(&parsePtr, model, mapInfoIndex);
        }
        Com_EndParseSession();
        free(fileData);
      }
      else
      {
        Com_Printf("WARNING: No collision map file for script_vehicle \"%s\"\n", baseName);
      }
    }
  }
  return i;
}

/*
================
ExpandBrushesAndWriteMap

Expands brushes and writes expanded.map for testing.
================
*/
int ExpandBrushesAndWriteMap()
{
  Brush_t *src, *copy, *outList;
  Plane_t *pl;
  float horzExpand, vertExpand;
  int i;

  /* set expansion amounts based on test mode */
  if ( testExpand == 1 )
  {
    horzExpand = PLAYER_CAPSULE_RADIUS;
    vertExpand = PLAYER_CAPSULE_HALFHEIGHT;
  }
  else
  {
    Assert(testExpand == 2, s_assertDisable_ProcessScriptVehicles);
    horzExpand = 0.0f;
    vertExpand = 0.0f;
  }

  /* copy all worldspawn brushes with expanded planes */
  outList = NULL;
  for ( src = g_entities[0].brushes; src; src = src->next )
  {
    copy = CopyCollisionBrush(src);
    copy->next = outList;

    /* expand each side's plane by capsule dimensions */
    for ( i = 0; i < copy->numSides; i++ )
    {
      pl = &mapplanes[copy->sides[i].planenum];
      copy->sides[i].planenum = FindFloatPlane(pl->normal,
        fabs(vertExpand * pl->normal[2]) + pl->dist + horzExpand);
    }

    outList = copy;
  }

  WriteBSPBrushMap("expanded.map", outList);
  Com_Error("can't proceed after writing expanded.map");
  return 0;
}

/*
================
ParseMapFile

Parses the map file content, calling ParseMapEntity for each entity.
Expects "iwmap 4" header followed by entity blocks.
================
*/
static void SkipMapLayerInfo(char **parsePtr)
{
  char *token;

  for ( ;; )
  {
    token = COM_Parse(parsePtr);
    if ( !*parsePtr || !token || token[0] == '{' )
      break;
    COM_ParseExt(parsePtr);
  }
  Com_UngetToken();
}

int ParseMapFile(char *filename, char *sourceMapName, char *fileData,
                 float *transformMtx, float brushScale, int parentPrefab,
                 int prefabFlags)
{
  char *token, *parsePtr;
  float mapAngles[3];
  int mapInfoIndex, result, savedEntityCount;

  parsePtr = fileData;
  ApplyRotationToAngles(transformMtx + 3, mapAngles);
  mapInfoIndex = MapInfo_Add(sourceMapName, transformMtx, mapAngles, brushScale);

  /* validate iwmap header */
  if ( strcmp(COM_Parse(&parsePtr), "iwmap") )
    Com_Error("'%s' is missing 'iwmap' version specification\n", filename);
  token = COM_Parse(&parsePtr);
  if ( atol(token) != IWMAP_VERSION )
    Com_Error("'%s' is version '%s'; should be version '%i'\n", filename, token, IWMAP_VERSION);

  SkipMapLayerInfo(&parsePtr);

  /* save and reset entity counter for nested prefab parsing */
  savedEntityCount = g_parseEntityCount;
  g_parseEntityCount = 0;

  /* parse entities until EOF */
  int entityCount = 0;
  do {
    result = ParseMapEntity(&parsePtr, transformMtx, brushScale, mapInfoIndex,
                            parentPrefab, prefabFlags);
    if ( entityCount == 0 )
      {} /* PrintMaterialSummary — not in original binary */
    entityCount++;
  } while ( result );

  g_parseEntityCount = savedEntityCount;
  return result;
}

/*
================
LoadMapFile

Loads and parses a .map file, creating entities and brushes.
================
*/
int LoadMapFile(char *filename)
{
  Brush_t *b;
  char *filenameCopy;
  void *fileData;
  float identityMtx[12];
  int brushCount;

  Com_DPrintf("--- LoadMapFile ---\n");
  Com_Printf("Loading map file %s\n", filename);
  if ( LoadFile(filename, &fileData) < 0 )
    Com_Error("file '%s' not found\n", filename);

  /* initialize parsing state */
  InitMapParsing(filename);
  num_entities = 0;
  s_mapInfoCount = 0;
  numMapDrawSurfs = 0;
  memset(g_drawSurfs, 0, sizeof(g_drawSurfs));
  *nummapplanes = 0;

  /* allocate the shared build brush */
  g_buildBrush = AllocBrush(MAX_BUILD_SIDES);

  /* identity transform: origin(0,0,0) + identity 3x3 rotation */
  memset(identityMtx, 0, sizeof(identityMtx));
  identityMtx[3] = 1.0f;
  identityMtx[7] = 1.0f;
  identityMtx[11] = 1.0f;

  /* parse the map file */
  filenameCopy = _strdup(filename);
  ParseMapFile(filename, filenameCopy, fileData, identityMtx, 1.0f,
               NO_PARENT_PREFAB, 0);

  /* process misc_model and script_vehicle entities */
  if ( staticModelCollMaps )
    ProcessMiscModels();
  ProcessScriptVehicles();

  /* calculate world bounds from all brushes */
  ClearBounds(&mapMins, &mapMaxs);
  for ( b = g_entities[0].brushes; b; b = b->next )
  {
    AddPointToBounds(b->eMins, &mapMins, &mapMaxs);
    AddPointToBounds(b->eMaxs, &mapMins, &mapMaxs);
  }

  /* print summary statistics */
  brushCount = CountBrushList(g_entities[0].brushes);
  Com_DPrintf("%5i total world brushes\n", brushCount);
  Com_DPrintf("%5i detail brushes\n", *nummapplanes);
  Com_DPrintf("%5i patches\n", g_numPatches);
  Com_DPrintf("%5i boxbevels\n", g_numMapDrawSurfs);
  Com_DPrintf("%5i edgebevels\n", g_numMapBrushes);
  Com_DPrintf("%5i entities\n", num_entities);
  Com_DPrintf("%5i planes\n", g_numMapPlanes);
  Com_DPrintf("%5i areaportals\n", g_numMapBrushSides);
  Com_DPrintf(
    "size: %5.0f,%5.0f,%5.0f to %5.0f,%5.0f,%5.0f\n",
    mapMins,
    mapMins_y,
    mapMins_z,
    mapMaxs,
    mapMaxs_y,
    mapMaxs_z);

  Com_EndParseSession();
  return SortLoadedMaterials();
}

/*
================
ParseFlagTokens

Parses "contents" or "toolFlags" from brush. Reads keyword token, if it
matches then reads flag names until ";" and returns OR'd flags.
================
*/
int ParseFlagTokens(char **parsePtr, const char *keyword, ContentFlag_t *flagTable)
{
  char *token;
  int tableIdx;
  int flags;

  token = COM_Parse(parsePtr);

  if ( strcmp(token, keyword) )
  {
    /* token wasn't the expected keyword, push it back */
    Com_UngetToken();
    return 0;
  }

  /* accumulate flags until semicolon terminator */
  flags = 0;
  while (1)
  {
    token = COM_ParseExt(parsePtr);
    if ( !*token )
      Com_Error("missing token for '%s'", keyword);
    if ( *token == ';' )
      break;

    /* look up flag name in table */
    for ( tableIdx = 0; ; ++tableIdx )
    {
      if ( !flagTable[tableIdx].name )
        Com_Error("'%s' is not a known token for '%s'", token, keyword);
      if ( !strcmp(token, flagTable[tableIdx].name) )
      {
        flags |= flagTable[tableIdx].flags;
        break;
      }
    }
  }
  return flags;
}

/*
================
ParseContentsFlags

Parses "contents" keyword from map file and returns content flags.
================
*/
int ParseContentsFlags(char **parsePtr)
{
  return ParseFlagTokens(parsePtr, "contents", contentsTable);
}

/*
================
ParseToolFlags

Parses "toolFlags" keyword from map file and returns tool flags.
================
*/
int ParseToolFlags(char **parsePtr)
{
  return ParseFlagTokens(parsePtr, "toolFlags", toolFlagsTable);
}

/*
================
InitMapParsing

Initializes parse session for a .map file and configures parsing options.
================
*/
int InitMapParsing(char *mapFilePath)
{
  Com_BeginParseSession(mapFilePath);
  Com_SetSpaceDelimited(0);
  Com_SetParseNegativeNumbers(1);
  return 0;
}
