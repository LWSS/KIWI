/*
writebsp.c — BSP file writing and emission

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char s_assertDisable_EmitBrushes;
char s_assertDisable_EmitBrushes;
char s_assertDisable_EndBSPFile;
char s_assertDisable_VerifyBSPBrushCount;
char s_assertDisable_RemapBrushSidePlanes;
char s_assertDisable_RemapBrushSidePlanes;
char s_assertDisable_RemapBrushSidePlanes;
char s_assertDisable_RemapNodePlanes;
char s_assertDisable_RemapNodePlanes;
char s_assertDisable_BeginModel;
char s_assertDisable_EndModel;
static char s_assertDisable_GetBrushModelIndex;
static char s_assertDisable_GetPhysicsModelIndex;
static char s_assertDisable_BeginPhysicsModel;
static char s_assertDisable_EndPhysicsModel;

/*
================
BeginBSPFile

The native triangle output descriptors are represented by the statically
initialized draw-surface contexts in tris.cpp.  The remaining native work is
the BSP counter reset performed before ProcessModels.
================
*/
int BeginBSPFile(void)
{
  numBSPModels = 0;
  numBSPNodes = 0;
  numBSPBrushSides = 0;
  numBSPBrushEdges = 0;
  numBSPLeafSurfaces = 0;
  numBSPLeafBrushes = 0;
  numBSPLeafs = 1;
  if ( TrisLmap_NativeRouteEnabled() )
    numLightmaps = 0;
  Tris_InitDrawSurfaceContexts();
  return 0;
}

/*
================
GetBrushModelIndex

The entity key is the authoritative disk-model index.  CoD4 emits dynamic
models after ordinary brush models, so entity traversal order is not in
general the same as the model-array order.
================
*/
static unsigned int GetBrushModelIndex(unsigned int entIndex)
{
  const Entity_t *ent;
  const char *modelName;
  unsigned int brushModelIndex;

  Assert(entIndex < MAX_MAP_ENTITIES, s_assertDisable_GetBrushModelIndex);
  if ( !entIndex )
    return 0;

  ent = &g_entities[entIndex];
  Assert(ent->brushes || ent->patches, s_assertDisable_GetBrushModelIndex);
  modelName = ValueForKey(ent, "model");
  Assert(modelName, s_assertDisable_GetBrushModelIndex);
  Assert(modelName[0] == '*', s_assertDisable_GetBrushModelIndex);

  brushModelIndex = (unsigned int)atoi(modelName + 1);
  Assert(brushModelIndex < MAX_MAP_MODELS, s_assertDisable_GetBrushModelIndex);
  return brushModelIndex;
}

/*
================
GetPhysicsModelIndex

Physics brushes are emitted as an independent model record.  As with ordinary
brush models, the source entity's key rather than its traversal order selects
the disk record.
================
*/
static unsigned int GetPhysicsModelIndex(unsigned int entIndex)
{
  const Entity_t *ent;
  const char *modelName;
  unsigned int brushModelIndex;

  Assert(entIndex < MAX_MAP_ENTITIES, s_assertDisable_GetPhysicsModelIndex);
  if ( !entIndex )
    return 0;

  ent = &g_entities[entIndex];
  Assert(ent->physicsBrushes, s_assertDisable_GetPhysicsModelIndex);
  modelName = ValueForKey(ent, "physicsmodel");
  Assert(modelName, s_assertDisable_GetPhysicsModelIndex);
  Assert(modelName[0] == '*', s_assertDisable_GetPhysicsModelIndex);

  brushModelIndex = (unsigned int)atoi(modelName + 1);
  Assert(brushModelIndex < MAX_MAP_MODELS, s_assertDisable_GetPhysicsModelIndex);
  return brushModelIndex;
}

/*
================
PrintMaterialNameExpansion

Generated layered-material names are encoded as `*<material-index>` segments.
The material-capacity diagnostic expands those segments so that the actual
source materials are visible in the report, matching the CoD4 compiler.
================
*/
static char s_assertDisable_PrintMaterialNameExpansion;

static void PrintMaterialNameExpansion(const char *mtlName)
{
  const char *nameIter;
  int materialIndex;

  Assert(mtlName[0] == '*' && isdigit((unsigned char)mtlName[1]), s_assertDisable_PrintMaterialNameExpansion);

  nameIter = mtlName + 1;
  for (;;)
  {
    materialIndex = 0;
    do
    {
      materialIndex = 10 * materialIndex + *nameIter++ - '0';
    } while (isdigit((unsigned char)*nameIter));

    Com_Printf("        %s\n", bspMaterials[materialIndex].material);

    if (*nameIter == 'n')
      ++nameIter;
    if (!*nameIter)
      break;

    Assert(*nameIter == '_', s_assertDisable_PrintMaterialNameExpansion);
    ++nameIter;
  }
}

static int CompareMaterialList(const void *elem0, const void *elem1)
{
  const Dmaterial_t *material0 = *(const Dmaterial_t *const *)elem0;
  const Dmaterial_t *material1 = *(const Dmaterial_t *const *)elem1;

  if (material0->material[0] == '*')
  {
    if (material1->material[0] != '*')
      return 1;
  }
  else if (material1->material[0] == '*')
  {
    return -1;
  }

  return _stricmp(material0->material, material1->material);
}

static void PrintMaterialList(void)
{
  Dmaterial_t *sortedMaterials[MAX_MAP_MATERIALS];
  int i;

  for (i = 0; i < numBSPMaterials; ++i)
    sortedMaterials[i] = &bspMaterials[i];

  qsort(sortedMaterials, numBSPMaterials, sizeof(sortedMaterials[0]), CompareMaterialList);

  Com_Printf("\n\nMaterial list:\n");
  for (i = 0; i < numBSPMaterials; ++i)
  {
    Com_Printf("%4i: %s\n", i + 1, sortedMaterials[i]->material);
    if (sortedMaterials[i]->material[0] == '*')
      PrintMaterialNameExpansion(sortedMaterials[i]->material);
  }
}

/*
================
SetModelNumbers

CoD4 assigns ordinary brush models first, then reserves the remaining model
range for dyn_* geometry and physics brush models.  This ordering is part of
the serialized entity/BSP relationship and differs from the CoD2 donor.
================
*/
void SetModelNumbers(void)
{
  Entity_t *ent;
  const char *classname;
  char modelName[12];
  int entityIndex;
  int modelIndex;

  modelIndex = 1;

  for ( entityIndex = 1; entityIndex < num_entities; ++entityIndex )
  {
    ent = &g_entities[entityIndex];
    if ( ent->brushes || ent->patches )
    {
      classname = ValueForKey(ent, "classname");
      if ( strncmp(classname, "dyn_", 4) )
      {
        if ( modelIndex == 1023 )
          Com_Error("Error: Too many non dyn entity brush models.  Max is [%d]\n", 1023);
        sprintf(modelName, "*%i", modelIndex);
        SetKeyValue(ent, "model", modelName);
        ++modelIndex;
      }
    }
  }

  for ( entityIndex = 1; entityIndex < num_entities; ++entityIndex )
  {
    ent = &g_entities[entityIndex];
    classname = ValueForKey(ent, "classname");
    if ( !strncmp(classname, "dyn_", 4) )
    {
      if ( ent->brushes || ent->patches )
      {
        if ( modelIndex == 4095 )
          Com_Error("Error: Too many dyn entity brush models.  Max is [%d]\n", 4095);
        sprintf(modelName, "*%i", modelIndex);
        SetKeyValue(ent, "model", modelName);
        ++modelIndex;
      }

      if ( ent->physicsBrushes )
      {
        if ( modelIndex == 4095 )
          Com_Error("Error: Too many dyn entity brush models.  Max is [%d]\n", 4095);
        sprintf(modelName, "*%i", modelIndex);
        SetKeyValue(ent, "physicsmodel", modelName);
        ++modelIndex;
      }
    }
  }
}


/*
================
EmitMaterial

Emits a BSP material/shader entry. Searches existing materials for a match
by name and surface flags; if not found, adds a new entry to the material table.
================
*/
int AddBspMaterial(const char *materialName, int surfaceFlags, int contentFlags)
{
  int i;

  if ( !materialName )
    materialName = "$default";
  if ( strlen(materialName) >= sizeof(bspMaterials[0].material) )
    Com_Error("AddBspMaterial: material name exceeds %i characters",
              (int)sizeof(bspMaterials[0].material) - 1);

  for ( i = 0; i < numBSPMaterials; i++ )
  {
    if ( surfaceFlags == bspMaterials[i].surfaceFlags
      && contentFlags == bspMaterials[i].contentFlags
      && !Q_stricmp(materialName, bspMaterials[i].material) )
      return i;
  }

  /* CoD4 reserves the last slot as a recoverable $default fallback. */
  if ( numBSPMaterials == MAX_MAP_MATERIALS - 1 )
  {
    PrintMaterialList();
    Com_Printf("MAX_MAP_MATERIALS (%i) reached -- too many unique materials used in the map.\n", MAX_MAP_MATERIALS);
    Com_Printf("Using 'weaponclip' in Radiant also turns one material into two.");
    materialName = "$default";
    contentFlags = 1;
  }
  else if ( numBSPMaterials == MAX_MAP_MATERIALS )
  {
    return MAX_MAP_MATERIALS - 1;
  }

  i = numBSPMaterials;
  ++numBSPMaterials;
  strcpy(bspMaterials[i].material, materialName);
  _strlwr(bspMaterials[i].material);
  bspMaterials[i].surfaceFlags = surfaceFlags;
  bspMaterials[i].contentFlags = contentFlags;
  return i;
}

int EmitMaterial(ShaderInfo_t *material, int contentFlags)
{
  if ( !material )
  {
    material = LoadMaterial("default");
    Assert(material, s_assertDisable_EmitBrushes);
  }

  return AddBspMaterial(material->name, material->surfaceFlags, contentFlags);
}

/*
================
RemapNodePlanes

Remaps BSP node plane indices through the planeMap lookup table.
After unused planes are stripped, node plane numbers must be updated
to reference the new compacted plane array.
================
*/
int RemapNodePlanes(int *planeMap)
{
  int mappedPlane;
  int i;

  for ( i = 0; i < numBSPNodes; i++ )
  {
    if ( bspNodes[i].planeNum < 0 )
      continue;

    Assert(bspNodes[i].planeNum >= 0 && bspNodes[i].planeNum < g_numMapPlanes, s_assertDisable_RemapNodePlanes);
    mappedPlane = planeMap[bspNodes[i].planeNum];
    Assert(mappedPlane >= 0 && mappedPlane < numBSPPlanes, s_assertDisable_RemapNodePlanes);
    bspNodes[i].planeNum = mappedPlane;
  }
  return numBSPNodes;
}

/*
================
RemapBrushSidePlanes

Remaps brush side plane indices through the planeMap lookup table.
After unused planes are stripped, brush side plane numbers must be
updated to reference the new compacted plane array.
================
*/
void RemapBrushSidePlanes( int *planeMap )
{
  BspBrushSide_t *side;
  int i, j, mapped;

  Assert( planeMap, s_assertDisable_RemapBrushSidePlanes );

  side = bspBrushSidesData;

  for ( i = 0; i < numBSPBrushes; i++ )
  {
    /* skip first 6 axial sides (store float dist, not plane index) */
    side += BRUSH_AXIAL_SIDES;

    /* remap plane indices for non-axial sides */
    for ( j = BRUSH_AXIAL_SIDES; j < bspBrushes[i].numSides; j++, side++ )
    {
      Assert( side->planeNum >= 0 && side->planeNum < g_numMapPlanes, s_assertDisable_RemapBrushSidePlanes );
      mapped = planeMap[side->planeNum];
      Assert( mapped >= 0 && mapped < numBSPPlanes, s_assertDisable_RemapBrushSidePlanes );
      side->planeNum = mapped;
    }
  }
}

/*
================
EmitBrushes

Writes the brush list to BSP brush and brushside arrays.
For each brush, emits its material, side count, and per-side plane
distances with material indices. Collision sides (first 6) store
axial plane distances; additional sides store plane indices.
================
*/
void EmitBrushes( double a1, Brush_t *brushes )
{
  Brush_t *b;
  BrushSide_t *side;
  BspBrush_t *db;
  BrushAdjacencyWinding_t *adjacency;
  int firstSide, sideIdx, j, k;
  int edge;
  Plane_t *plane;

  /* walk the linked list of brushes */
  for ( b = brushes; b; b = b->next )
  {
    if ( b->isOccluder || b->original->isOccluder )
      continue;

    Assert( b->numCollisionSides >= BRUSH_AXIAL_SIDES, s_assertDisable_EmitBrushes );

    if ( numBSPBrushes == MAX_MAP_BRUSHES )
      Com_Error( "MAX_MAP_BRUSHES" );

    /* record BSP brush index and emit brush header */
    b->bspBrushIdx = numBSPBrushes;
    db = &bspBrushes[numBSPBrushes++];
    db->numSides = 0;
    db->shaderNum = (short)EmitMaterial( b->contentShaderInfo, b->contentFlags );

    firstSide = numBSPBrushSides;

    /* emit each collision side */
    for ( j = 0; j < b->numCollisionSides; j++ )
    {
      sideIdx = numBSPBrushSides;
      if ( sideIdx == MAX_MAP_BRUSHSIDES )
        Com_Error( "MAX_MAP_BRUSHSIDES " );

      side = &b->collisionSides[j];
      plane = MAP_PLANE(side->planenum);
      db->numSides++;
      numBSPBrushSides = sideIdx + 1;

      if ( sideIdx - firstSide >= BRUSH_AXIAL_SIDES )
      {
        /* non-axial side: store plane index */
        plane->flags = 1;
        bspBrushSidesData[sideIdx].planeNum = side->planenum;
      }
      else if ( ((sideIdx - firstSide) & 1) == 0 )
      {
        /* even axial side: store negated plane distance */
        bspBrushSidesData[sideIdx].dist = -plane->dist;
      }
      else
      {
        /* odd axial side: store plane distance */
        bspBrushSidesData[sideIdx].dist = plane->dist;
      }

      bspBrushSidesData[sideIdx].shaderNum = EmitMaterial(side->shaderInfo, b->contentFlags);

      adjacency = side->adjacencyWinding;
      if ( !adjacency )
      {
        bspBrushSideEdgeCounts[sideIdx] = 0;
        continue;
      }

      Assert(adjacency->numsides >= 0 && adjacency->numsides <= 255,
             s_assertDisable_EmitBrushes);
      bspBrushSideEdgeCounts[sideIdx] = (unsigned char)adjacency->numsides;
      Assert(bspBrushSideEdgeCounts[sideIdx] == adjacency->numsides,
             s_assertDisable_EmitBrushes);

      for ( k = 0; k < adjacency->numsides; ++k )
      {
        if ( numBSPBrushEdges == MAX_MAP_BRUSHEDGES )
          Com_Error("MAX_MAP_BRUSHEDGES");
        edge = adjacency->sides[k];
        if ( edge < 0 || edge > 255 )
          Com_Error("MAX_EDGES_PER_SIDE");
        bspBrushEdges[numBSPBrushEdges] = (unsigned char)edge;
        Assert(bspBrushEdges[numBSPBrushEdges] < b->numCollisionSides,
               s_assertDisable_EmitBrushes);
        ++numBSPBrushEdges;
      }
    }

    Assert( db->numSides >= BRUSH_AXIAL_SIDES, s_assertDisable_EmitBrushes );
  }
}

/*
================
BeginModel

Sets up a new BSP brush model. Computes bounding box from entity
brushes and patches, then records the model's first triSoup,
first collision AABB, and first brush indices.
================
*/
int BeginModel(void)
{
  BspModel_t *mod;
  Entity_t *ent;
  Brush_t *br;
  Patch_t *p;
  int i;
  vec3_t mins, maxs;

  if ( numBSPModels >= MAX_MAP_MODELS )
    Com_Error("MAX_MAP_MODELS");

  mod = &bspModels[GetBrushModelIndex(g_currentEntityIndex)];
  ent = &g_entities[g_currentEntityIndex];
  ClearBounds(mins, maxs);

  /* bound the brushes */
  for ( br = ent->brushes; br; br = br->next )
  {
    if ( br->numSides )
    {
      AddPointToBounds(br->eMins, mins, maxs);
      AddPointToBounds(br->eMaxs, mins, maxs);
    }
  }

  /* bound the patches */
  for ( p = ent->patches; p; p = p->next )
  {
    for ( i = 0; i < p->width * p->height; i++ )
      AddPointToBounds(p->vertexData[i].pos, mins, maxs);
  }

  /* store bounds and offsets into model record */
  VectorCopy(mins, mod->mins);
  VectorCopy(maxs, mod->maxs);
  Assert(numBSPTriSoups <= USHRT_MAX, s_assertDisable_BeginModel);
  mod->firstTriSoup = (unsigned short)numBSPTriSoups;
  Assert(numBSPUnlayeredTriSoups <= USHRT_MAX, s_assertDisable_BeginModel);
  mod->firstTriSoupUnlayered = (unsigned short)numBSPUnlayeredTriSoups;
  mod->numTriSoups = 0;
  mod->numTriSoupsUnlayered = 0;
  mod->firstAABB = numBSPCollisionAABBs;
  mod->firstBrush = numBSPBrushes;
  return numBSPBrushes;
}

/*
================
BeginPhysicsModel

Initializes the separately indexed disk model used for an entity's
physics-brush carrier.  Physics models have brush bounds only: patches never
participate in this path.
================
*/
int BeginPhysicsModel(void)
{
  BspModel_t *mod;
  Entity_t *ent;
  Brush_t *br;
  vec3_t mins, maxs;

  Assert(numBSPModels < MAX_MAP_MODELS, s_assertDisable_BeginPhysicsModel);

  ent = &g_entities[g_currentEntityIndex];
  mod = &bspModels[GetPhysicsModelIndex(g_currentEntityIndex)];
  ClearBounds(mins, maxs);

  for ( br = ent->physicsBrushes; br; br = br->next )
  {
    if ( br->numSides )
    {
      AddPointToBounds(br->eMins, mins, maxs);
      AddPointToBounds(br->eMaxs, mins, maxs);
    }
  }

  VectorCopy(mins, mod->mins);
  VectorCopy(maxs, mod->maxs);
  Assert(numBSPTriSoups <= USHRT_MAX, s_assertDisable_BeginPhysicsModel);
  mod->firstTriSoup = (unsigned short)numBSPTriSoups;
  Assert(numBSPUnlayeredTriSoups <= USHRT_MAX, s_assertDisable_BeginPhysicsModel);
  mod->firstTriSoupUnlayered = (unsigned short)numBSPUnlayeredTriSoups;
  mod->numTriSoups = 0;
  mod->numTriSoupsUnlayered = 0;
  mod->firstAABB = numBSPCollisionAABBs;
  mod->firstBrush = numBSPBrushes;
  return numBSPBrushes;
}

/*
================
EmitPlanes

Emits used map planes to the BSP plane array, skipping unused planes.
Builds a planeMap lookup table to remap old plane indices to new
compacted indices, then remaps all node, brush side, leaf, and
collision node plane references.
================
*/
void EmitPlanes(void)
{
  int *planeMap;
  int i;

  planeMap = malloc(sizeof(int) * g_numMapPlanes);

  for ( i = 0; i < g_numMapPlanes; i++ )
  {
    if ( mapplanes[i].flags )
    {
      planeMap[i] = numBSPPlanes;
      VectorCopy(mapplanes[i].normal, bspPlanes[numBSPPlanes].normal);
      bspPlanes[numBSPPlanes].dist = mapplanes[i].dist;
      numBSPPlanes++;
    }
    else
    {
      planeMap[i] = -1;
    }
  }

  RemapNodePlanes(planeMap);
  RemapBrushSidePlanes(planeMap);
  CreateLeafNode(planeMap);
  free(planeMap);
}

/*
================
EmitCollisionAABBs_r

Recursively emits collision AABB records from the collision tree.
Each AABB stores center and half-extents computed from node bounds.
Child links reference either sub-AABBs (internal nodes) or brush
indices (leaf nodes) with material indices.
================
*/
int EmitCollisionAABBs_r( CmCollideBox_t *nodeList )
{
  CmCollideBox_t *node;
  CollisionAabbTree_t *aabb;
  int startIdx, aabbIdx, count;

  if ( !nodeList )
    return 0;

  startIdx = numBSPCollisionAABBs;
  aabbIdx = startIdx;
  count = 0;

  /* first pass: emit AABB records for all nodes in the linked list */
  for ( node = nodeList; node; node = node->next )
  {
    if ( aabbIdx == MAX_MAP_COLLISION_AABBS )
      Com_Error( "MAX_MAP_COLLISIONAABBS (%i) exceeded", MAX_MAP_COLLISION_AABBS );

    aabb = &bspCollisionAABBs[aabbIdx];

    /* center = (mins + maxs) * 0.5, halfSize = maxs - center */
    aabb->origin[0] = MIDF(node->mins[0], node->maxs[0]);
    aabb->origin[1] = MIDF(node->mins[1], node->maxs[1]);
    aabb->origin[2] = MIDF(node->mins[2], node->maxs[2]);
    aabb->halfSize[0] = node->maxs[0] - aabb->origin[0];
    aabb->halfSize[1] = node->maxs[1] - aabb->origin[1];
    aabb->halfSize[2] = node->maxs[2] - aabb->origin[2];

    aabbIdx++;
    numBSPCollisionAABBs = aabbIdx;
    count++;
  }

  /* second pass: fill in child links for each AABB */
  aabb = &bspCollisionAABBs[startIdx];
  for ( node = nodeList; node; node = node->next, aabb++ )
  {
    if ( node->child )
    {
      /* internal node: recurse into children */
      aabb->u.firstChildIndex = numBSPCollisionAABBs;
      aabb->childCount = EmitCollisionAABBs_r( node->child );
      aabb->materialIndex = bspCollisionAABBs[aabb->u.firstChildIndex].materialIndex;
    }
    else
    {
      /* leaf node: reference brush partition and material */
      CmPartition_t *part = node->partition;
      DrawSurf_t *ds = part->triArray->drawSurf;
      MapDrawSurf_t *nativeDrawSurf = NativeMapDrawSurfFor(ds);
      aabb->u.partitionIndex = part->emitIndex;
      aabb->childCount = 0;
      aabb->materialIndex = EmitMaterial(ds->shaderInfo,
        nativeDrawSurf ? nativeDrawSurf->contentFlags : ds->surfaceFlags);
    }
  }

  return count;
}

/*
================
EmitLeaf

Emits a leaf node to the BSP file. Stores the leaf's cluster, area,
terrain type, and brush references. For non-opaque leaves, also emits
collision AABBs from the leaf's collision tree.
================
*/
int EmitLeaf( Node_t *node )
{
  BspLeaf_disk_t *leaf_p;
  Brush_t *b;
  int leafIdx;

  if ( numBSPLeafs >= MAX_MAP_LEAFS )
    Com_Error( "MAX_MAP_LEAFS" );

  leafIdx = numBSPLeafs++;
  leaf_p = &bspLeafs[leafIdx];
  leaf_p->cluster = node->cluster;
  leaf_p->cellnum = node->cellnum;
  leaf_p->firstLeafBrush = numBSPLeafBrushes;

  /* emit leaf brushes — skip detail brushes */
  for ( b = node->leafBrushes; b; b = b->next )
  {
    /* CoD4 0x4640D0 filters occluders, not detail brushes. */
    if ( b->isOccluder || b->original->isOccluder )
      continue;
    if ( numBSPLeafBrushes >= MAX_MAP_LEAFBRUSHES )
      Com_Error( "MAX_MAP_LEAFBRUSHES" );
    bspLeafBrushes[numBSPLeafBrushes++] = b->original->bspBrushIdx;
  }
  leaf_p->numLeafBrushes = numBSPLeafBrushes - leaf_p->firstLeafBrush;

  /* emit collision AABBs for non-opaque leaves */
  if ( node->opaque )
  {
    leaf_p->firstCollisionAABB = 0;
    leaf_p->numCollisionAABBs = 0;
    return 0;
  }

  leaf_p->firstCollisionAABB = numBSPCollisionAABBs;
  leaf_p->numCollisionAABBs = EmitCollisionAABBs_r( (CmCollideBox_t *)node->collisionAABBs );
  return leaf_p->numCollisionAABBs;
}

/*
================
EmitDrawNode_r

Recursively emits BSP nodes. For leaf nodes, calls EmitLeaf and returns
the negative leaf index. For internal nodes, stores the splitting plane,
bounding box, and recursively emits both children.
================
*/
int EmitDrawNode_r( Node_t *node )
{
  BspNode_disk_t *n;
  int nodeIdx, i;

  /* leaf node */
  if ( node->planenum == PLANENUM_LEAF )
  {
    EmitLeaf( node );
    return -numBSPLeafs;
  }

  /* emit internal node */
  if ( numBSPNodes == MAX_MAP_NODES )
    Com_Error( "MAX_MAP_NODES" );

  nodeIdx = numBSPNodes++;
  n = &bspNodes[nodeIdx];

  /* bounding box — float to int truncation */
  n->mins[0] = (int)node->mins[0];
  n->mins[1] = (int)node->mins[1];
  n->mins[2] = (int)node->mins[2];
  n->maxs[0] = (int)node->maxs[0];
  n->maxs[1] = (int)node->maxs[1];
  n->maxs[2] = (int)node->maxs[2];

  if ( node->planenum & 1 )
    Com_Error( "WriteDrawNodes_r: odd planenum" );
  n->planeNum = node->planenum;
  mapplanes[node->planenum].flags = 1;

  /* recursively output children */
  for ( i = 0; i < 2; i++ )
  {
    if ( node->children[i]->planenum == PLANENUM_LEAF )
    {
      n->children[i] = -1 - numBSPLeafs;
      EmitLeaf( node->children[i] );
    }
    else
    {
      n->children[i] = numBSPNodes;
      EmitDrawNode_r( node->children[i] );
    }
  }

  return nodeIdx;
}

/*
================
EndBSPFile

Finishes the BSP file and writes it to disk. Emits planes, unparses
entities, reorders draw verts, then writes the final BSP file.
================
*/
int EndBSPFile(void)
{
  char *ext;
  char path[MAX_OS_PATH];

  EmitPlanes();
  UnparseEntities();
  /* CoD4 0x449E50 finalizes its unlayered context first, then its layered
     context.  These two context-aware passes own the same output counters
     directly, so no separate 0x449C40 counter-copy shim is needed. */
  Tris_ReorderUnlayeredDrawVerts();
  Tris_ReorderDrawVerts();
  VerifyBSPBrushCount();
  if ( Error_HasErrors() )
    Com_Error("\nNot writing map due to errors\n");
  ext = GetBSPFileExtension();
  sprintf(path, "%s%s", g_outputBasePath, ext);
  Com_Printf("Writing %s\n", path);
  Assert(g_targetPlatform, s_assertDisable_EndBSPFile);
  return WriteBSPFile(path, g_targetPlatform->bigEndian);
}

/*
================
VerifyBSPBrushCount

Checks that every ordinary and physics brush was emitted exactly once.
================
*/
int VerifyBSPBrushCount(void)
{
  int brushCount;
  int entityIndex;

  brushCount = 0;
  for ( entityIndex = 0; entityIndex < num_entities; ++entityIndex )
  {
    brushCount += CountBrushList(g_entities[entityIndex].brushes);
    brushCount += CountBrushList(g_entities[entityIndex].physicsBrushes);
  }

  Assert(brushCount == numBSPBrushes, s_assertDisable_VerifyBSPBrushCount);
  return brushCount;
}

/*
================
EndPhysicsModel

The native physics-model writer emits collision brushes without a draw-node
tree.  Empty physics records are consequently omitted from the disk model
array, just like an empty ordinary model record.
================
*/
int EndPhysicsModel(void)
{
  BspModel_t *mod;
  int firstLeaf;
  int hasTriSoups;
  int hasAllTriSoups;

  Com_DPrintf("--- EndPhysicsModel ---\n");

  firstLeaf = numBSPLeafs;
  EmitBrushes(0.0, g_entities[g_currentEntityIndex].physicsBrushes);
  mod = &bspModels[GetPhysicsModelIndex(g_currentEntityIndex)];

  Assert(numBSPTriSoups - mod->firstTriSoup <= USHRT_MAX, s_assertDisable_EndPhysicsModel);
  mod->numTriSoups = (unsigned short)(numBSPTriSoups - mod->firstTriSoup);
  Assert(numBSPUnlayeredTriSoups - mod->firstTriSoupUnlayered <= USHRT_MAX, s_assertDisable_EndPhysicsModel);
  mod->numTriSoupsUnlayered = (unsigned short)(numBSPUnlayeredTriSoups - mod->firstTriSoupUnlayered);

  hasTriSoups = mod->numTriSoups != 0;
  hasAllTriSoups = mod->numTriSoupsUnlayered != 0;
  Assert(hasTriSoups == hasAllTriSoups, s_assertDisable_EndPhysicsModel);

  mod->numAABBs = bspLeafs[firstLeaf].numCollisionAABBs;
  mod->numBrushes = numBSPBrushes - mod->firstBrush;
  if ( mod->numAABBs || mod->numBrushes || hasTriSoups )
    ++numBSPModels;
  else
    Com_Printf("Ignoring empty brush model entity\nMap %s entity %i\n",
               MapInfo_GetName(g_entities[g_currentEntityIndex].mapInfoIndex),
               g_entities[g_currentEntityIndex].entityNum);

  return numBSPModels;
}

/*
================
EndModel

Finishes processing a BSP model. Emits brushes and the BSP node tree
for the current entity, then updates the model record with surface
counts, leaf cluster reference, and brush counts.
================
*/
int EndModel(Node_t *headnode)
{
  BspModel_t *mod;
  int firstLeaf;

  Com_DPrintf("--- EndModel ---\n");

  EmitBrushes(0.0, g_entities[g_currentEntityIndex].brushes);

  firstLeaf = numBSPLeafs;
  mod = &bspModels[GetBrushModelIndex(g_currentEntityIndex)];

  EmitDrawNode_r(headnode);

  Assert(numBSPTriSoups - mod->firstTriSoup <= USHRT_MAX, s_assertDisable_EndModel);
  mod->numTriSoups = (unsigned short)(numBSPTriSoups - mod->firstTriSoup);
  Assert(numBSPUnlayeredTriSoups - mod->firstTriSoupUnlayered <= USHRT_MAX, s_assertDisable_EndModel);
  mod->numTriSoupsUnlayered = (unsigned short)(numBSPUnlayeredTriSoups - mod->firstTriSoupUnlayered);
  mod->numAABBs = bspLeafs[firstLeaf].numCollisionAABBs;
  mod->numBrushes = numBSPBrushes - mod->firstBrush;
  numBSPModels++;

  return numBSPModels;
}

/*
================
SampleEntitySolidForMass

Returns true when a sampling point lies inside one of the entity's physics
brushes (or, if absent, its ordinary brushes).  The scale-dependent plane
epsilon is retained from the native CoD4 integrator.
================
*/
static qboolean SampleEntitySolidForMass(const vec3_t point, const vec3_t sampleSize, const Entity_t *ent)
{
  Brush_t *brush;
  unsigned int sideIndex;

  brush = ent->physicsBrushes ? ent->physicsBrushes : ent->brushes;
  for ( ; brush; brush = brush->next )
  {
    if ( brush->numSides )
    {
      for ( sideIndex = 0; sideIndex < (unsigned int)brush->numSides; ++sideIndex )
      {
        const Plane_t *plane = &mapplanes[brush->sides[sideIndex].planenum];
        double planeScale = log(plane->normal[0] * sampleSize[0])
                          + log(plane->normal[1] * sampleSize[1])
                          + log(plane->normal[2] * sampleSize[2]);
        double distance = DotProduct(point, plane->normal) - plane->dist - planeScale;

        if ( distance > 0.1 )
          break;
      }

      if ( sideIndex == (unsigned int)brush->numSides )
        return qtrue;
    }
  }

  return qfalse;
}

/*
================
ComputeEntityMassPropertiesBySampling

The compiler estimates unit-mass collision properties from a 32-cubed grid
through the entity bounds.  Accumulation is first performed about the map
origin, then translated to the sampled centre of mass, as in dMassTranslate.
================
*/
static void ComputeEntityMassPropertiesBySampling(const vec3_t mins, const vec3_t maxs,
                                                   const Entity_t *ent, vec3_t center,
                                                   vec3_t moments, vec3_t products)
{
  vec3_t sampleSize, point;
  unsigned int x, y, z;
  unsigned int sampleCount;
  double sum[3];
  double inertia[3];
  double product[3];

  for ( x = 0; x < 3; ++x )
  {
    sampleSize[x] = (maxs[x] - mins[x]) / 32.0f;
    point[x] = mins[x] + sampleSize[x] * 0.5f;
    sum[x] = 0.0;
    inertia[x] = 0.0;
    product[x] = 0.0;
  }
  sampleCount = 0;

  for ( x = 0; x < 32; ++x )
  {
    point[1] = mins[1] + sampleSize[1] * 0.5f;
    for ( y = 0; y < 32; ++y )
    {
      point[2] = mins[2] + sampleSize[2] * 0.5f;
      for ( z = 0; z < 32; ++z )
      {
        if ( SampleEntitySolidForMass(point, sampleSize, ent) )
        {
          ++sampleCount;
          sum[0] += point[0];
          sum[1] += point[1];
          sum[2] += point[2];
          inertia[0] += point[1] * point[1] + point[2] * point[2];
          inertia[1] += point[0] * point[0] + point[2] * point[2];
          inertia[2] += point[0] * point[0] + point[1] * point[1];
          product[0] -= point[0] * point[1];
          product[1] -= point[0] * point[2];
          product[2] -= point[1] * point[2];
        }
        point[2] += sampleSize[2];
      }
      point[1] += sampleSize[1];
    }
    point[0] += sampleSize[0];
  }

  VectorClear(center);
  VectorClear(moments);
  VectorClear(products);
  if ( !sampleCount )
    return;

  center[0] = (float)(sum[0] / sampleCount);
  center[1] = (float)(sum[1] / sampleCount);
  center[2] = (float)(sum[2] / sampleCount);

  moments[0] = (float)(inertia[0] / sampleCount - center[1] * center[1] - center[2] * center[2]);
  moments[1] = (float)(inertia[1] / sampleCount - center[0] * center[0] - center[2] * center[2]);
  moments[2] = (float)(inertia[2] / sampleCount - center[0] * center[0] - center[1] * center[1]);
  products[0] = (float)(product[0] / sampleCount + center[0] * center[1]);
  products[1] = (float)(product[1] / sampleCount + center[0] * center[2]);
  products[2] = (float)(product[2] / sampleCount + center[1] * center[2]);
}

/*
================
EmitEntityMassProperties

Stores sampled physics properties on a dynamic entity for downstream runtime
physics loading.
================
*/
static void EmitEntityMassProperties(Entity_t *ent)
{
  Brush_t *brush;
  vec3_t mins, maxs;
  vec3_t center, moments, products;

  ClearBounds(mins, maxs);
  brush = ent->physicsBrushes ? ent->physicsBrushes : ent->brushes;
  for ( ; brush; brush = brush->next )
  {
    if ( brush->numSides )
    {
      AddPointToBounds(brush->eMins, mins, maxs);
      AddPointToBounds(brush->eMaxs, mins, maxs);
    }
  }

  ComputeEntityMassPropertiesBySampling(mins, maxs, ent, center, moments, products);
  SetKeyValue(ent, "centerofmass", va("%g %g %g", center[0], center[1], center[2]));
  SetKeyValue(ent, "momofinertia", va("%g %g %g", moments[0], moments[1], moments[2]));
  SetKeyValue(ent, "prodofinertia", va("%g %g %g", products[0], products[1], products[2]));
}

/*
================
EmitDynEntityMassProperties

Native preprocessing emits mass-property entity keys only for dyn_* entities
that carry a collision brush list.
================
*/
void EmitDynEntityMassProperties(void)
{
  int entityIndex;

  for ( entityIndex = 1; entityIndex < num_entities; ++entityIndex )
  {
    Entity_t *ent = &g_entities[entityIndex];
    if ( (ent->brushes || ent->physicsBrushes)
      && !strncmp(ValueForKey(ent, "classname"), "dyn_", 4) )
      EmitEntityMassProperties(ent);
  }
}
