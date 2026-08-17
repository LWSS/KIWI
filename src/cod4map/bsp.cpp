/*
bsp.c — BSP compilation pipeline

Reconstructed from cod2map.exe by Rose.

Main entry point and compilation pipeline: parses arguments, loads map,
builds BSP tree, emits brushes/surfaces/portals/collision, writes d3dbsp.
*/

#include "cod4map.h"

unsigned char _memory_placeholder[256];
unsigned char _stack_placeholder[0x10000];

OptionEntry_t optionsTable[] = {
    { "-platform",           "Required if not copying a bsp between platforms; specifies target platform", (OptionHandler)HandlePlatformOption },
    { "-pcToXenon",          "Copies a PC bsp to a Xenon bsp",                            (OptionHandler)Opt_PcToXenon          },
    { "-xenonToPc",          "Copies a Xenon bsp to a PC bsp",                            (OptionHandler)Opt_XenonToPc          },
    { "-v",                  "Verbose; enables extra compilation messages",               (OptionHandler)Opt_Verbose            }, 
    { "-verboseEntities",    "Includes verbose messages for submodels if '-v' is given",  (OptionHandler)Opt_VerboseEntities    }, 
    { "-onlyEnts",           "Compile doesn't touch triggers, geometry, or lighting",     (OptionHandler)Opt_OnlyEnts           }, 
    { "-sampleScale",        "Scales all lightmaps; 2 doubles pixel size, 0.5 halves it", (OptionHandler)Opt_sampleScale        },
    { "-blockSize",          "Grid size for regular BSP splits; 0 uses largest possible", (OptionHandler)Opt_blockSize          }, 
    { "-subdivisions",       "Divides all geometry on a grid; only works for small maps", (OptionHandler)Opt_subdivisions       }, 
    { "-noSubdivide",        "Ignores the 'tessSize' setting in all materials",           (OptionHandler)Opt_NoSubdivide        }, 
    { "-staticModelCollMaps", "(Legacy support) Use collmaps for static models",           (OptionHandler)Opt_StaticModelCollMaps },
    { "-displayCollMapWarnings", "Display missing collmap warnings",                      (OptionHandler)Opt_DisplayCollMapWarnings },
    { "-smoothAngle",        "Smooth surfaces are only smoothed at angles less than this", (OptionHandler)Opt_SmoothAngle        },
    { "-loadFrom",           "Reads this map instead, but still writes to <mapfile>",     (OptionHandler)Opt_loadFrom           }, 
    { "-noWater",            "Ignores all water brushes",                                 (OptionHandler)Opt_NoWater            }, 
    { "-noCurves",           "Ignores all patch and terrain geometry",                    (OptionHandler)Opt_NoCurves           }, 
    { "-noDetail",           "Ignores all detail brushes",                                (OptionHandler)Opt_NoDetail           }, 
    { "-fullDetail",         "Turns all detail brushes into structural brushes",          (OptionHandler)Opt_FullDetail         }, 
    { "-leakTest",           "Quits immediately if the map leaked",                       (OptionHandler)Opt_LeakTest           }, 
    { "-brushMethod",        "Brush optimization method (players/bullets/all/none)",      (OptionHandler)Opt_brushMethod        },  
    { "-expandPlayer",       "Writes a map for Radiant to see player-to-brush collision", (OptionHandler)Opt_ExpandPlayer       }, 
    { "-expandBullet",       "Writes a map for Radiant to see bullet-to-brush collision", (OptionHandler)Opt_ExpandBullet       }, 
    { "-debugPortals",       "Writes a _portals.map showing portal/structural geometry",  (OptionHandler)Opt_DebugPortals       }, 
    { "-debugLightmaps",     "Fills lightmaps with random colors to show seams",          (OptionHandler)Opt_DebugLightmaps     },
    { "-noReorderTris",      "Disables reordering of optimized triangles for T&L cache",  (OptionHandler)Opt_NoReorderTris      }, 
    { "-listSlowEntities",   "Lists entities that process in more than this many seconds", (OptionHandler)Opt_ListSlowEntities   },
    { "-warnLayerUses",      "Generates warnings for layer combos used this many or fewer times", (OptionHandler)Opt_WarnLayerUses },
    { "-warnLayerArea",      "Generates warnings for layer combos used less than this many square inches", (OptionHandler)Opt_WarnLayerArea },
    { "-reflectionDebug",    "Reflection debug mode. 1 is rainbow colors.",               (OptionHandler)Opt_ReflectionDebug    },
    { NULL, NULL, NULL }
};

char g_fmtIntColonStr[8] = { '%', 'i', ':', '%', 's', '\0', '\0', '\0' };
ContentFlag_t contentsTable[] = {
    { "weaponClip",    CONTENTS_DETAIL | CONTENTS_CLIPSHOT | CONTENTS_MISSILECLIP },
    { "nonColliding",  CONTENTS_DETAIL | CONTENTS_NONCOLLIDING },
    { "detail",        CONTENTS_DETAIL },
    { NULL, 0 }
};

ContentFlag_t toolFlagsTable[] = {
    { "splitGeo",  0x00000100 },
    { NULL, 0 }
};

char *g_scriptClassnames[7] =
{
  "info_vehicle_node",
  "info_vehicle_node_rotate",
  "script_model",
  "script_origin",
  "script_vehicle",
  "misc_prefab",
  "script_struct"
};

Brush_t  *g_buildBrush;
Entity_t *g_currentEntity = 0;
HWND      hWnd;
Plane_t  *g_planeHashTable[PLANE_HASH_SIZE];
Plane_t  *mapplanes = NULL;

int  nummapplanes_storage[4];
int *nummapplanes = &nummapplanes_storage[0];

float   g_mapMins[3];
#define mapMins g_mapMins[0]
#define mapMins_y g_mapMins[1]
#define mapMins_z g_mapMins[2]

float   g_mapMaxs[3];
#define mapMaxs g_mapMaxs[0]
#define mapMaxs_y g_mapMaxs[1]
#define mapMaxs_z g_mapMaxs[2]

int          ArgList;
/* CoD4's BSS default is zero: regular block subdivision is opt-in via the
 * command-line switch or the worldspawn "blocksize" key. */
float        blockSize = 0.0f;
int          brushMethod = 2;
float        smoothAngle = 0.01f;
char         bspLightmapData[MAX_MAP_LIGHTBYTES];
int          byteswapMode;
int          debugLightmaps;
int          displayCollMapWarnings;
int          fulldetail;
int          g_currentEntityIndex;
char         g_loadFromPath[MAX_OS_PATH];
char         g_mapFileExtCheck[4];
char         g_mapSourceFile[MAX_OS_PATH];
int          g_numBrushes;
int          g_numMapBrushes;
int          g_numMapBrushSides;
int          g_numMapDrawSurfs;
int          g_numMapEntities;
int          g_numMapPlanes;
int          g_numPatches;
int          g_onlyEnts;
char         g_outputBasePath[MAX_OS_PATH];
int          g_parseEntityCount;
int          g_removedBrushSides;
int          g_treeNodeCount;
int          leaktest;
unsigned int Msg = 4294967295u;
int          noCurveBrushes;
int          nodetail;
int          nosubdivide;
int          nowater;
int          numActiveBrushes;
int          numBSPPaths;
int          numLightmaps = 1;
int          numMapDrawSurfs;
int          numthreads = -1;
int          reorderTris = 1;
float        sampleScale = 1.0;
int          staticModelCollMaps;
float        listSlowEntities;
int          warnLayerUses = 1;
float        warnLayerArea;
int          reflectionDebug;
int          testExpand;
int          testlevel = 2;
int          verbose;
int          verboseEntities;

/*
================
BuildOutputPathFromLoadSource

CoD4's diagnostic and leak artifacts follow -loadFrom when it is present;
normal BSP output remains anchored to the requested map source.  Keep the
native bounded-copy order so a truncated source path cannot overrun while its
extension is replaced.
================
*/
char *BuildOutputPathFromLoadSource(const char *extension, char *path, int pathSize)
{
  const char *sourcePath;
  int pathLength;

  sourcePath = g_loadFromPath[0] ? g_loadFromPath : g_mapSourceFile;
  I_strncpyz(path, sourcePath, pathSize);
  COM_StripExtension(path, path);
  pathLength = (int)strlen(path);
  return I_strncpyz(path + pathLength, extension, pathSize - pathLength);
}

char g_assertFlags0;
char g_assertFlags1;
char g_assertFlags2;
char g_assertFlags3;
char g_assertFlags4;
char g_assertFlags5;
char g_assertFlags6;
char g_assertFlags7;
char g_assertFlags8;
char g_assertFlags9;
char g_assertFlagsA;
char g_assertFlagsB;
char g_assertFlagsC;
char g_assertFlagsD;
char g_assertFlagsE;
char g_assertFlagsF;
char s_assertDisable_BSP_SetTargetPlatform;
char s_assertDisable_BSP_SetTargetPlatform_0;
char s_assertDisable_OnlyEnts;
char s_assertDisable_ProcessBSPArguments_0;
char s_assertDisable_ProcessModels;
char s_assertDisable_PrintSlowEntity;

void ClampBlockSize(void)
{
  if ( blockSize > 0.0f && blockSize <= MIN_BLOCK_SIZE )
    blockSize = MIN_BLOCK_SIZE;
  else if ( blockSize <= 0.0f )
    blockSize = 0.0f;
}

void SetBlockSizeFromWorldspawn(Entity_t *entity)
{
  const char *value;

  value = ValueForKey(entity, "blocksize");
  Assert(value != NULL, s_assertDisable_ProcessBSPArguments_0);
  if ( blockSize == 0.0f && *value && *value != '0' )
  {
    blockSize = (float)atof(value);
    ClampBlockSize();
    Com_Printf("map blocksize is %g units\n", blockSize);
  }
}


/*
================
EmitWorldBSP

Build BSP tree for worldspawn entity (entity 0).
================
*/
int EmitWorldBSP(void)
{
  Face_t *faces;
  Tree_t *tree;
  Face_t *detailFaces;
  int leaked;

  BeginModel();
  g_entities[0].firstDrawSurf = 0;
  OpenDebugFile();
  ProcessEntityPatches(g_entities);
  PatchMapDrawSurfs(g_entities);
  BrushSides_InitTree(g_entities);
  BrushSides_BuildVisibleHulls(g_entities);
  faces = CollectBrushWindings(g_entities[0].brushes);
  tree = BuildBspTree(faces);
  ProcessBspTree(tree);
  FilterStructuralBrushesIntoBspTree(g_entities, tree);

  if ( FloodEntities(tree) )
  {
    /* no leak — rebuild tree with detail brushes */
    MarkVisibleSides(tree->headnode);
    CullBrushSideVisibleHulls(g_entities[0].brushes, tree->headnode);
    detailFaces = CollectBrushWindings(g_entities[0].brushes);
    FreeTreeBlock(tree);
    tree = BuildBspTree(detailFaces);
    ProcessBspTree(tree);
    FilterStructuralBrushesIntoBspTree(g_entities, tree);
    leaked = 0;
  }
  else
  {
    /* leak detected */
    Com_Printf("**********************\n");
    Com_Printf("******* leaked *******\n");
    Com_Printf("**********************\n");
    LeakFile(tree);
    if ( leaktest )
    {
      Com_Printf("--- MAP LEAKED, ABORTING LEAKTEST ---\n");
      exit(-1);
    }
    leaked = 1;
  }

  /* CoD4 0x409BBD: visible-hull emission follows the world leak/cull path. */
  BrushSides_EmitDrawSurfs(g_entities);

  /* portalize */
  CloseDebugFile();
  PortalizeWorld(g_entities[0].brushes, tree, leaked);
  NumberClusters(tree);
  if ( !leaked )
    WritePortalFile(tree);
  FloodAreas(tree);

  /* detail brushes and draw surfaces */
  FilterDetailBrushesIntoBspTree(g_entities, tree);
  if ( !nosubdivide )
    SubdivideDrawSurfs(g_entities);
  AssignReflectionProbesToCells(tree);
  AssignReflectionProbesToTriSurfaces(tree, 0);
  SortEntityTransientDrawSurfaces();
  /* Native 0x409C53/0x409C58: build the world-only primary-light regions
     after the native draw-surface ordering has settled and before emission.
     This produces BSP lumps 52-54 (region counts, packed hulls, axes). */
  BuildPrimaryLightRegions();
  EmitDrawSurfaces(g_entities, tree);
  EmitLeafBrushes(g_entities, tree);
  AddBrushNeighborBevels(g_entities[0].brushes);

  EndModel(tree->headnode);
  BrushSides_ShutdownTree();
  FreeTreeBlock(tree);

  if ( testExpand )
    return ExpandBrushesAndWriteMap();
  return testExpand;
}

/*
================
EmitEntityBSP

Build BSP for a non-worldspawn entity (submodel).
================
*/
void EmitEntityBSP(void)
{
  Entity_t *e;
  Node_t *node;
  Brush_t *brush, *bc;
  Tree_t *tree;

  BeginModel();
  e = &g_entities[g_currentEntityIndex];
  e->firstDrawSurf = numMapDrawSurfs;
  ProcessEntityPatches(e);
  PatchMapDrawSurfs(e);
  SortEntityTransientDrawSurfaces();

  /* copy all brushes into a single leaf node */
  node = AllocNode();
  node->planenum = PLANENUM_LEAF;
  for ( brush = e->brushes; brush; brush = brush->next )
  {
    bc = CopyBrush(brush);
    bc->next = node->leafBrushes;
    node->leafBrushes = bc;
  }

  /* emit draw surfaces */
  tree = AllocTree();
  tree->headnode = node;
  BrushSides_InitTree(e);
  BrushSides_BuildVisibleHulls(e);
  BrushSides_EmitDrawSurfs(e);
  if ( !nosubdivide )
    SubdivideDrawSurfs(e);
  AssignReflectionProbesToTriSurfaces(tree, e->firstDrawSurf);
  EmitDrawSurfaces(e, tree);
  EmitLeafBrushes(e, tree);
  AddBrushNeighborBevels(e->brushes);

  EndModel(node);
  BrushSides_ShutdownTree();
  FreeTreeBlock(tree);
}

/*
================
EmitPhysicsModel

Physics brushes form a second, collision-only model record for an entity.  The
native path still constructs a leaf copy before emitting its source brush
carrier; retaining that ownership route keeps neighbour-bevel processing in
lockstep with the regular submodel path.
================
*/
void EmitPhysicsModel(void)
{
  Entity_t *ent;
  Node_t *node;
  Brush_t *brush;
  Brush_t *brushCopy;

  BeginPhysicsModel();
  ent = &g_entities[g_currentEntityIndex];
  ent->firstDrawSurf = numMapDrawSurfs;

  node = AllocNode();
  node->planenum = PLANENUM_LEAF;
  for ( brush = ent->physicsBrushes; brush; brush = brush->next )
  {
    brushCopy = CopyBrush(brush);
    brushCopy->next = node->leafBrushes;
    node->leafBrushes = brushCopy;
  }

  AddBrushNeighborBevels(ent->physicsBrushes);
  EndPhysicsModel();
}

/* CoD4 0x409EB0. */
static void PrintSlowEntity(const Entity_t *entity, double elapsedSeconds)
{
  const char *mapName;
  int entityNum;

  if (entity->brushes)
  {
    mapName = MapInfo_GetName(entity->brushes->mapInfoIndex);
    entityNum = entity->brushes->entityNum;
  }
  else if (entity->physicsBrushes)
  {
    mapName = MapInfo_GetName(entity->physicsBrushes->mapInfoIndex);
    entityNum = entity->physicsBrushes->entityNum;
  }
  else
  {
    Assert(entity->patches, s_assertDisable_PrintSlowEntity);
    mapName = MapInfo_GetName(entity->patches->mapInfoIndex);
    entityNum = entity->patches->entityNum;
  }

  Com_Printf("entity %i in map %s took %.1f seconds to process (entity %i of %i to process)\n",
             entityNum, mapName, elapsedSeconds, g_currentEntityIndex, num_entities);
}

/*
================
EmitBSP

Emit BSP data for all entities.
================
*/
int ProcessModels(void)
{
  int savedVerbose;
  Entity_t *ent;
  double lastProgressTime;

  savedVerbose = verbose;
  lastProgressTime = 0.0;

  for ( g_currentEntityIndex = 0; g_currentEntityIndex < num_entities; ++g_currentEntityIndex )
  {
    double entityStartTime;

    ent = &g_entities[g_currentEntityIndex];

    if ( ent->brushes || ent->physicsBrushes || ent->patches )
    {
      Com_DPrintf("############### model %i ###############\n", numBSPModels);

      if ( g_currentEntityIndex )
      {
        entityStartTime = I_FloatTime();
        if ( !verbose && entityStartTime - lastProgressTime > 10.0 )
        {
          Com_Printf("Processing entity %i of %i\n", g_currentEntityIndex + 1, num_entities);
          lastProgressTime = entityStartTime;
        }
        EmitEntityBSP();
        if ( ent->physicsBrushes )
          EmitPhysicsModel();
        if ( listSlowEntities > 0.0f )
        {
          double entityEndTime = I_FloatTime();
          if ( listSlowEntities <= entityEndTime - entityStartTime )
          {
            PrintSlowEntity(ent, entityEndTime - entityStartTime);
            lastProgressTime = entityEndTime;
          }
        }
      }
      else
      {
        EmitWorldBSP();
        lastProgressTime = I_FloatTime();
        if ( !verbose )
          Com_Printf("Finished processing world entity\n\n");
      }

      if ( !verboseEntities )
        verbose = 0;
    }
  }

  /* Native 0x43E5E0 reports the queued source locations for layered
     material combinations which never reached their configured use count. */
  Tris_ReportLayeredMaterialCombinationErrors();
  for (int modelIndex = 1; modelIndex < numBSPModels; ++modelIndex)
  {
    const BspModel_t *model = &bspModels[modelIndex];
    Assert(model->numBrushes || model->numAABBs
        || (model->numTriSoups && model->numTriSoupsUnlayered),
        s_assertDisable_ProcessModels);
  }

  verbose = savedVerbose;
  return numBSPModels;
}

int EmitBSP(void)
{
  return ProcessModels();
}

/*
================
Com_ErrorLevel

Error reporting with severity level.
================
*/
int Com_ErrorLevel(int errorLevel, char *fmt, ...)
{
  char buffer[MAX_STRING_CHARS];
  va_list args;

  va_start(args, fmt);
  vsprintf(buffer, fmt, args);
  va_end(args);
  Com_Printf(buffer);
  return 0;
}

/*
================
FS_Startup_Simple

Initializes the filesystem with default base paths.
================
*/
char *FS_Startup_Simple()
{
  return FS_InitFilesystemFromArgs(g_basePath, g_fsBaseGame, g_fsBaseGame);
}

/*
================
BSPInfo

Print BSP file statistics for one or more map files.
================
*/
int BSPInfo(int argc, const char **argv)
{
  int i;
  char *ext;
  FILE *fp;
  int fileSize;

  if ( argc < 1 )
  {
    Com_Printf("No files to dump info for.\n");
    return 0;
  }

  Swap_InitByteSwap();

  /* dump info for each bsp file */
  for ( i = 0; i < argc; i++ )
  {
    Com_Printf("---------------------\n");
    StripExtension((char *)argv[i]);
    SetBSPFileExtensions("d3d");
    strcpy(g_outputBasePath, argv[i]);
    ext = GetBSPFileExtension();
    DefaultExtension(g_outputBasePath, ext);

    /* load and print sizes */
    fp = fopen(g_outputBasePath, "rb");
    if ( fp )
    {
      fileSize = FS_FileLength(fp);
      fclose(fp);
      Com_Printf("%s: %i\n", g_outputBasePath, fileSize);
      LoadBSPFile(g_outputBasePath);
      PrintBSPFileSizes(fileSize);
      Com_Printf("---------------------\n");
    }
    else
    {
      Com_Error("no bsp files with name '%s'\n", argv[i]);
    }
  }
  return argc;
}

/*
================
OnlyEnts

Re-emit entity data without recompiling BSP geometry.
================
*/
static DiskPrimaryLight_t s_onlyEntsPrimaryLights[255];
static int s_onlyEntsPrimaryLightCount;

static void OnlyEnts_LoadMapFile(char *source)
{
  OnlyEnts_BeginMapLoad();
  if ( strlen(g_loadFromPath) )
    LoadMapFile(g_loadFromPath);
  else
    LoadMapFile(source);
  OnlyEnts_EndMapLoad();
}

static void OnlyEnts_SavePrimaryLights(void)
{
  s_onlyEntsPrimaryLightCount = numBspPrimaryLights;
  memcpy(s_onlyEntsPrimaryLights, bspPrimaryLights,
      sizeof(*bspPrimaryLights) * numBspPrimaryLights);
}

static void OnlyEnts_VerifyPrimaryLights(void)
{
  int errorCount = 0;
  int lightIndex;

  if ( s_onlyEntsPrimaryLightCount != numBspPrimaryLights )
    Com_Error("ERROR: Primary light count changed from %i to %i.  You cannot use '-onlyents' if you add or remove primary lights.\n",
        s_onlyEntsPrimaryLightCount, numBspPrimaryLights);

  for ( lightIndex = 0; lightIndex < numBspPrimaryLights; ++lightIndex )
  {
    const DiskPrimaryLight_t *oldLight = &s_onlyEntsPrimaryLights[lightIndex];
    const DiskPrimaryLight_t *newLight = &bspPrimaryLights[lightIndex];

    if ( oldLight->type == newLight->type )
    {
      if ( newLight->type == 1 || newLight->type == 3 )
      {
        if ( !VectorCompare(oldLight->origin, newLight->origin) )
        {
          Com_Printf("ERROR: Primary light %i moved from (%g %g %g) to (%g %g %g)\n", lightIndex,
              oldLight->origin[0], oldLight->origin[1], oldLight->origin[2],
              newLight->origin[0], newLight->origin[1], newLight->origin[2]);
          ++errorCount;
        }
        if ( newLight->radius > oldLight->radius )
        {
          Com_Printf("ERROR: Primary light %i radius increased from %g to %g\n", lightIndex,
              oldLight->radius, newLight->radius);
          ++errorCount;
        }
        if ( newLight->cosHalfFovOuter < oldLight->cosHalfFovOuter )
        {
          Com_Printf("ERROR: Primary light %i fov_outer increased from %g to %g\n", lightIndex,
              oldLight->cosHalfFovOuter, newLight->cosHalfFovOuter);
          ++errorCount;
        }
        if ( newLight->type == 1 && newLight->rotationLimit > oldLight->rotationLimit )
        {
          Com_Printf("ERROR: Primary light %i maxturn increased from %g to %g\n", lightIndex,
              oldLight->rotationLimit, newLight->rotationLimit);
          ++errorCount;
        }
      }
    }
    else
    {
      Com_Printf("ERROR: Primary light %i changed types\n", lightIndex);
      ++errorCount;
    }

    if ( !VectorCompare(oldLight->dir, newLight->dir) )
    {
      Com_Printf("ERROR: Primary light %i changed direction from (%g %g %g) to (%g %g %g)\n", lightIndex,
          oldLight->dir[0], oldLight->dir[1], oldLight->dir[2],
          newLight->dir[0], newLight->dir[1], newLight->dir[2]);
      ++errorCount;
    }
  }

  if ( errorCount )
    Com_Error("Canceling -onlyents compile due to %i primary light errors\n", errorCount);
}

int OnlyEnts(void)
{
  char *ext;
  char bspPath[MAX_OS_PATH];
  int brushCount;
  int entityIndex;

  ext = GetBSPFileExtension();
  sprintf(bspPath, "%s%s", g_outputBasePath, ext);

  /* load existing bsp, re-parse entities, write back */
  LoadBSPFile(bspPath);
  num_entities = 0;
  SaveExistingReflectionProbes();
  OnlyEnts_SavePrimaryLights();
  OnlyEnts_LoadMapFile(g_mapSourceFile);
  FreeSavedReflectionProbes();
  CreatePrimaryLights();
  OnlyEnts_VerifyPrimaryLights();
  SetModelNumbers();
  UnparseEntitiesWithOrigins();

  brushCount = 0;
  for ( entityIndex = 0; entityIndex < num_entities; ++entityIndex )
    brushCount += CountBrushList(g_entities[entityIndex].brushes);
  if ( brushCount != numBSPBrushes )
  {
    CheckMapSourceTimestamps(bspPath);
    Com_Error("\nERROR: Brush count has changed from %d to %d.  You cannot use '-onlyents' when the brushes have changed.",
        numBSPBrushes, brushCount);
  }

  Assert(g_targetPlatform, s_assertDisable_OnlyEnts);
  return WriteBSPFile(bspPath, g_targetPlatform->bigEndian);
}

/*
================
Com_FatalError

Error handler, prints error and exits.
================
*/
void Com_FatalError(const char *fmt, va_list args)
{
  vprintf(fmt, args);
  Com_Printf("\r\n");
  fflush(stdout);
  exit(-1);
}

/*
================
ParseSmoothAngle

Parse angle string and convert to cosine threshold for smooth shading.
================
*/
long double ParseSmoothAngle(char *str, const char *optionName)
{
  double angle;
  long double result;
  float angleF;

  angle = atof(str);
  angleF = angle;

  if ( angle > 0.0 )
  {
    if ( angleF < 180.0 )
    {
      /* convert angle to cosine threshold with small bias */
      printf("%s = %g\n", optionName, angleF);
      result = cos(angleF * DEG2RAD) - PLANESIDE_EPSILON;
      if ( result < 0.0 )
        return 0.0;
    }
    else
    {
      /* 180+ degrees = smooth everything */
      printf("%s = 180\n", optionName);
      return 1.0;
    }
  }
  else
  {
    /* zero or negative = no smoothing */
    printf("%s = 0\n", optionName);
    return 1.0;
  }
  return result;
}

/*
================
BSP_SetTargetPlatform

Set BSP file extensions based on target platform.
MUST NOT be inlined — original is standalone function.
================
*/
void BSP_SetTargetPlatform()
{
  Assert(g_targetPlatform, s_assertDisable_BSP_SetTargetPlatform);

  /* PC and Xenon both use d3d extensions */
  if ( g_targetPlatform->platformId < PLATFORM_COUNT )
  {
    SetBSPFileExtensions("d3d");
  }
  else if ( !g_assertsDisabled )
  {
    /* unhandled platform */
    AssertFatal(0, s_assertDisable_BSP_SetTargetPlatform_0);
  }
}

/*
================
BSP_ByteSwap

Byte-swap a BSP file between PC and Xenon formats
================
*/
int BSP_ByteSwap(const CHAR *basePath)
{
  SetBSPFileExtensions("d3d");
  FS_Startup(basePath);
  FS_InitFilesystemFromArgs(g_basePath, g_fsBaseGame, g_fsBaseGame);

  /* build source path */
  strcpy(g_outputBasePath, ExpandArg(basePath));
  StripExtension(g_outputBasePath);
  strcat(g_outputBasePath, GetBSPFileExtension());

  /* build target path with swapped platform directory */
  strcpy(g_bspSwapPath, g_outputBasePath);
  FS_ReplacePlatformPath(g_bspSwapPath, (char *)g_sourcePlatform->basePath, g_targetPlatform->basePath);

  printf("\nByte swapping: \n");
  printf("\t %s -> \n", g_outputBasePath);
  printf("\t %s \n", g_bspSwapPath);
  LoadAndWriteBSPFile(g_outputBasePath, g_bspSwapPath, g_targetPlatform->bigEndian);
  return 0;
}

/*
================
Opt_OnlyEnts

Enable -onlyEnts mode.
================
*/
int Opt_OnlyEnts()
{
  Com_Printf("onlyents = true\n");
  g_onlyEnts = 1;
  return 1;
}

/*
================
Opt_Verbose
================
*/
int Opt_Verbose()
{
  verbose = 1;
  return 1;
}

/*
================
Opt_VerboseEntities
================
*/
int Opt_VerboseEntities()
{
  Com_Printf("verboseentities = true\n");
  verboseEntities = 1;
  return 1;
}

/*
================
Opt_NoWater
================
*/
int Opt_NoWater()
{
  Com_Printf("nowater = true\n");
  nowater = 1;
  return 1;
}

/*
================
Opt_NoCurves
================
*/
int Opt_NoCurves()
{
  Com_Printf("nocurves = true\n");
  noCurveBrushes = 1;
  return 1;
}

/*
================
Opt_NoDetail
================
*/
int Opt_NoDetail()
{
  Com_Printf("nodetail = true\n");
  nodetail = 1;
  return 1;
}

/*
================
Opt_FullDetail
================
*/
int Opt_FullDetail()
{
  Com_Printf("fulldetail = true\n");
  fulldetail = 1;
  return 1;
}

/*
================
Opt_NoSubdivide
================
*/
int Opt_NoSubdivide()
{
  Com_Printf("nosubdivide = true\n");
  nosubdivide = 1;
  return 1;
}

/*
================
Opt_LeakTest
================
*/
int Opt_LeakTest()
{
  Com_Printf("leaktest = true\n");
  leaktest = 1;
  return 1;
}

/*
================
Opt_ExpandPlayer
================
*/
int Opt_ExpandPlayer()
{
  Com_Printf("Writing expanded.map for player collision.\n");
  testExpand = 1;
  return 1;
}

/*
================
Opt_ExpandBullet
================
*/
int Opt_ExpandBullet()
{
  Com_Printf("Writing expanded.map for bullet collision.\n");
  testExpand = 2;
  return 1;
}

/*
================
Opt_DebugPortals
================
*/
int Opt_DebugPortals()
{
  debugPortals = 1;
  return 1;
}

int Opt_DebugLightmaps()
{
  debugLightmaps = 1;
  return 1;
}

int Opt_StaticModelCollMaps()
{
  Com_Printf("staticModelCollMaps = true\n");
  staticModelCollMaps = 1;
  return 1;
}

int Opt_DisplayCollMapWarnings()
{
  Com_Printf("displayCollMapWarnings = true\n");
  displayCollMapWarnings = 1;
  return 1;
}

/*
================
Opt_NoReorderTris
================
*/
int Opt_NoReorderTris()
{
  reorderTris = 0;
  return 1;
}

static void RequireOptionValue(int argc)
{
  unsigned int i;

  if ( argc >= 2 )
    return;
  Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
  Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
  for ( i = 0; optionsTable[i].name != NULL; ++i )
    Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
  exit(-1);
}

int Opt_ListSlowEntities(int argc, char **argv)
{
  RequireOptionValue(argc);
  listSlowEntities = (float)atof(argv[1]);
  return 2;
}

int Opt_WarnLayerUses(int argc, char **argv)
{
  RequireOptionValue(argc);
  warnLayerUses = atoi(argv[1]) + 1;
  return 2;
}

int Opt_WarnLayerArea(int argc, char **argv)
{
  RequireOptionValue(argc);
  warnLayerArea = (float)atof(argv[1]);
  return 2;
}

int Opt_ReflectionDebug(int argc, char **argv)
{
  RequireOptionValue(argc);
  reflectionDebug = atoi(argv[1]);
  return 2;
}

/*
================
Opt_PcToXenon
================
*/
int Opt_PcToXenon()
{
  byteswapMode = 1;
  g_sourcePlatform = GetPlatformById(PLATFORM_PC);
  g_targetPlatform = GetPlatformById(PLATFORM_XENON);
  return 1;
}

/*
================
Opt_XenonToPc
================
*/
int Opt_XenonToPc()
{
  byteswapMode = 1;
  g_sourcePlatform = GetPlatformById(PLATFORM_XENON);
  g_targetPlatform = GetPlatformById(PLATFORM_PC);
  return 1;
}

/*
================
ParseBSPOption

Look up a command-line option in the options table and call its handler
================
*/
int ParseBSPOption(const char **argv, int argc)
{
  int i;
  
  /* Search for the option in the table */
  for (i = 0; optionsTable[i].name != NULL; i++)
  {
    if (_stricmp(*argv, optionsTable[i].name) == 0)
    {
      /* Found - call handler */
      if (optionsTable[i].handler)
        return optionsTable[i].handler(argc, argv);
      return 1;
    }
  }
  
  /* Not found - print error and usage */
  Com_Printf("\n\nUnknown argument '%s'\n\n", *argv);
  Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
  Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
  for (i = 0; optionsTable[i].name != NULL; i++)
    Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
  exit(-1);
  return 0;
}

/*
================
ProcessBSPArguments

Iterate through command-line arguments and process each BSP option
================
*/
void ProcessBSPArguments(const char **argv, int argc)
{
  int i;
  int argsUsed;

  /* consume arguments left to right */
  for ( i = argc; i > 0; argv += argsUsed )
  {
    argsUsed = ParseBSPOption(argv, i);

    /* handler must consume at least one argument */
    AssertFatal(argsUsed > 0, s_assertDisable_ProcessBSPArguments_0);
    i -= argsUsed;
  }
}

/*
================
LoadExistingBspIfPresentAndSaveReflectionProbes

Native 0x409F90 preserves reflection probes from an already-built BSP before
the normal map-load path replaces the compiler globals.  A missing BSP is not
an error; the optional snapshot simply remains empty.
================
*/
static void LoadExistingBspIfPresentAndSaveReflectionProbes(void)
{
  char bspPath[MAX_OS_PATH];
  FILE *fp;

  sprintf(bspPath, "%s%s", g_outputBasePath, GetBSPFileExtension());
  fp = fopen(bspPath, "rb");
  if ( fp )
  {
    fclose(fp);
    LoadExistingBSPReflectionProbes(bspPath);
    SaveExistingReflectionProbes();
  }
}

/*
================
main

Entry point — parse arguments, initialize subsystems, run BSP compilation.
================
*/
int main(int argc, const char **argv, const char **envp)
{
  unsigned int i;
  double startTime, endTime;
  char *prtExt;
  char buf[MAX_OS_PATH];

  /* unbuffered stdout — survives crashes when piped/redirected */
  setvbuf(stdout, NULL, _IONBF, 0);
  
#ifndef _WIN64
  /* set x87 FPU to 53-bit precision (double) — matches original exe behavior */
  _controlfp(_PC_53, _MCW_PC);
#endif

  /* restore MSVC6 CRT printf rounding (round-half-up, 3-digit exponents) */
  _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS &= ~_CRT_INTERNAL_PRINTF_STANDARD_ROUNDING;
  _CRT_INTERNAL_LOCAL_PRINTF_OPTIONS |= _CRT_INTERNAL_PRINTF_LEGACY_THREE_DIGIT_EXPONENTS;

  Com_Printf("CoD4Map v1.1 (c) 2002 Id Software Inc. / Infinity Ward\n");

  /* allocate brush points buffer */
  g_brushPoints = malloc(MAX_BRUSH_POINTS * BRUSH_POINT_STRIDE * sizeof(int));
  if (!g_brushPoints)
    exit(1);
  memset(g_brushPoints, 0, MAX_BRUSH_POINTS * BRUSH_POINT_STRIDE * sizeof(int));

  /* allocate map planes array */
  mapplanes = malloc(MAX_MAP_PLANES * sizeof(Plane_t));
  if (!mapplanes)
    exit(1);
  memset(mapplanes, 0, MAX_MAP_PLANES * sizeof(Plane_t));

  Com_SetErrorHandler(Com_FatalError);
  TrisTestWindingBin();

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }

  /* handle -info and -vis modes */
  if ( !strcmp(argv[1], "-info") )
  {
    BSPInfo(argc - 2, argv + 2);
    return 0;
  }
  if ( !strcmp(argv[1], "-vis") )
  {
    VisMain(argc - 1, argv + 1);
    return 0;
  }

  /* parse options and start BSP compilation */
  Com_Printf("---- cod2map ----\n");
  g_loadFromPath[0] = 0;
  ProcessBSPArguments(argv + 1, argc - 2);
  startTime = I_FloatTime();
  ThreadSetDefault();

  if ( byteswapMode )
    return BSP_ByteSwap(argv[argc - 1]);

  if ( !ValidatePlatformSet() )
    return 0;

  BSP_SetTargetPlatform();
  FS_Startup(argv[argc - 1]);
  FS_Startup_Simple();

  /* set up output paths */
  strcpy(g_outputBasePath, ExpandArg(argv[argc - 1]));
  StripExtension(g_outputBasePath);

  /* remove stale output files */
  prtExt = GetPRTFileExtension();
  sprintf(buf, "%s%s", g_outputBasePath, prtExt);
  remove(buf);
  sprintf(buf, "%s.lin", g_outputBasePath);
  remove(buf);

  /* determine source map file */
  strcpy(g_mapSourceFile, ExpandArg(argv[argc - 1]));
  if ( strcmp(&g_mapFileExtCheck[strlen(g_mapSourceFile)], ".reg") )
  {
    sprintf(buf, "%s.reg", g_outputBasePath);
    remove(buf);
    DefaultExtension(g_mapSourceFile, ".map");
  }

  Error_Init();

  if ( g_onlyEnts )
  {
    OnlyEnts();
    return 0;
  }

  /* BSP compilation pipeline */
  LoadExistingBspIfPresentAndSaveReflectionProbes();
  OnlyEnts_BeginMapLoad();
  if ( strlen(g_loadFromPath) )
    LoadMapFile(g_loadFromPath);
  else
    LoadMapFile(g_mapSourceFile);
  OnlyEnts_EndMapLoad();

  FreeSavedReflectionProbes();
  CreatePrimaryLights();
  SetModelNumbers();
  EmitDynEntityMassProperties();
  BeginBSPFile();
  ProcessModels();
  EndBSPFile();

  endTime = I_FloatTime();
  Com_Printf("%5.0f seconds elapsed\n", endTime - startTime);

  return 0;
}

/*
================
Opt_loadFrom

Load map from alternate path.
================
*/
int Opt_loadFrom(int argc, char **argv)
{
  unsigned int i;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }
  strcpy(g_loadFromPath, argv[1]);
  return 2;
}

/*
================
Opt_blockSize

Set BSP block subdivision size.
================
*/
int Opt_blockSize(int argc, char **argv)
{
  unsigned int i;
  double sizeVal;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }

  sizeVal = atof(argv[1]);
  blockSize = sizeVal;

  ClampBlockSize();

  if ( blockSize > 0.0 )
  {
    Com_Printf("blocksize is %g units\n", blockSize);
    return 2;
  }

  blockSize = 0.0;
  Com_Printf("blocksize is disabled\n", 0.0);
  return 2;
}

/*
================
Opt_sampleScale

Set lightmap sample scale factor.
================
*/
int Opt_sampleScale(int argc, char **argv)
{
  unsigned int i;
  float scaleF;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }

  scaleF = (float)atof(argv[1]);
  if ( scaleF <= 0.0f )
    Com_Error("sampleScale must be > 0");
  sampleScale = 1.0 / scaleF;
  Com_Printf("each lightmap sample will be scaled by %g\n", scaleF);
  return 2;
}

/*
================
Opt_brushSmoothAngle

Set smooth shading angle threshold for brush surfaces.
================
*/
int Opt_SmoothAngle(int argc, char **argv)
{
  unsigned int i;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }
  smoothAngle = ParseSmoothAngle(argv[1], "smoothAngle");
  return 2;
}

/*
================
Opt_curveSmoothAngle

Set smooth shading angle threshold for curve surfaces.
================
*/
int Opt_curveSmoothAngle(int argc, char **argv)
{
  unsigned int i;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }
  smoothAngle = ParseSmoothAngle(argv[1], "curveSmoothAngle");
  return 2;
}

/*
================
Opt_terrainSmoothAngle

Set smooth shading angle threshold for terrain surfaces.
================
*/
int Opt_terrainSmoothAngle(int argc, char **argv)
{
  unsigned int i;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }
  smoothAngle = ParseSmoothAngle(argv[1], "terrainSmoothAngle");
  return 2;
}

/*
================
Opt_subdivisions

Set automatic subdivision tessellation size.
================
*/
int Opt_subdivisions(int argc, char **argv)
{
  unsigned int i;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }

  g_matExpandRegion.defaultTessSize = (float)atof(argv[1]);
  if ( g_matExpandRegion.defaultTessSize < 0.0f )
    g_matExpandRegion.defaultTessSize = 0.0;

  if ( g_matExpandRegion.defaultTessSize == 0.0 )
  {
    Com_Printf("automatic subdivision disabled\n");
    return 2;
  }
  Com_Printf("automatically subdividing everything to %g x %g units\n", g_matExpandRegion.defaultTessSize, g_matExpandRegion.defaultTessSize);
  return 2;
}

/*
================
Opt_brushMethod

Set brush collision method (players, bullets, all, none).
================
*/
int Opt_brushMethod(int argc, char **argv)
{
  unsigned int i, j;
  int methodIdx;

  /* brush method lookup table */
  struct { const char *name; int value; } methods[NUM_BRUSH_METHODS];
  methods[0].name = "players";  methods[0].value = 2;
  methods[1].name = "bullets";  methods[1].value = 1;
  methods[2].name = "all";      methods[2].value = -1;
  methods[3].name = "none";     methods[3].value = 0;

  if ( argc < 2 )
  {
    Com_Printf("USAGE: cod2map [options] mapname, where options are 0 or more of the following.\n");
    Com_Printf("Options ignore capitalization; it is only present in the list for clarity.\n");
    for ( i = 0; optionsTable[i].name != NULL; i++ )
      Com_Printf("%-20s %s\n", optionsTable[i].name, optionsTable[i].description);
    exit(-1);
  }

  /* find matching method */
  for ( methodIdx = 0; methodIdx < NUM_BRUSH_METHODS; methodIdx++ )
  {
    if ( !Q_stricmp(argv[1], methods[methodIdx].name) )
    {
      brushMethod = methods[methodIdx].value;
      return 2;
    }
  }

  Com_Printf("Invalid brush method '%s'.  Valid methods are:\n", argv[1]);
  for ( j = 0; j < 4; j++ )
    Com_Printf("  %s\n", methods[j].name);
  exit(-1);
  return 2;
}

/*
================
ProcessWorldBrushes

Assign model numbers to brush entities.
Iterates through all non-worldspawn entities and assigns "model" keys
to entities that have brushes or patches. Model numbers start at "*1".
These model references are used in the BSP to link entities to geometry.
================
*/
int ProcessWorldBrushes(void)
{
  int i;
  int modelNum;
  Entity_t *ent;
  char modelBuffer[12];

  modelNum = 1;
  for ( i = 1; i < num_entities; i++ )
  {
    ent = &g_entities[i];
    if ( ent->brushes || ent->patches )
    {
      sprintf(modelBuffer, "*%i", modelNum++);
      SetKeyValue(ent, "model", modelBuffer);
    }
  }
  return num_entities;
}

/*
================
ProcessCoronaLights

Auto-configure corona light z-cutoff values.

Corona lights are glowing effects around light sources. This function finds
corona entities that don't have zcutoff/zfadeout set, then looks for nearby
misc_model entities (within 8 units) that are hanging lights. If found,
it automatically sets the z-cutoff values for proper rendering.

This prevents coronas from being visible through floors/ceilings by
fading them out when viewed from above/below.
================
*/
int ProcessCoronaLights(void)
{
  int i, j;
  Entity_t *ent, *other;
  const char *classname, *zcutoff, *model;

  for ( i = 1; i < num_entities; i++ )
  {
    ent = &g_entities[i];
    classname = ValueForKey(ent, "classname");
    if ( Q_stricmp(classname, "corona") )
      continue;

    zcutoff = ValueForKey(ent, "zcutoff");
    if ( zcutoff && *zcutoff )
      continue;

    /* find nearby misc_model that is a hanging light */
    for ( j = 1; j < num_entities; j++ )
    {
      other = &g_entities[j];
      if ( Q_stricmp(ValueForKey(other, "classname"), "misc_model") )
        continue;
      if ( VectorDistance(ent->origin, other->origin) > DUPLICATE_MODEL_DIST )
        continue;

      model = ValueForKey(other, "model");
      if ( !Q_stricmp(model, "xmodel/hangingutillight")
        || !Q_stricmp(model, "xmodel/light_ind_ceiling2on") )
      {
        SetKeyValue(ent, "zcutoff", "-0.15");
        SetKeyValue(ent, "zfadeout", "-0.25");
      }
      break;
    }
  }
  return num_entities;
}

/*
================
ProcessVisibility

Initialize visibility/BSP counters.
Resets global counters used during BSP generation before processing begins.
Called after map entities are loaded, before BSP tree building.
================
*/
int ProcessVisibility(void)
{
  numBSPModels = 0;
  numBSPNodes = 0;
  numBSPBrushSides = 0;
  numBSPBrushEdges = 0;
  numBSPLeafSurfaces = 0;
  numBSPLeafBrushes = 0;
  numBSPLeafs = 1;
  return 0;
}
