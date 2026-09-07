/*
materials.c — Material loading and parsing

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

MatExpandRegion_t g_matExpandRegion;

static ShaderInfo_t *s_loadedMaterialOrder[4096];
static int s_loadedMaterialOrderCount;

/* CoD4 0x123C2900: the compiler cache is a target-indexed 12-byte table,
   not the expanded ShaderInfo_t donor cache.  Keep that native ownership and
   lifetime carrier, with a separate expanded sidecar for KIWI consumers. */
typedef struct NativeLoadedMaterial_s
{
  void *materialData;
  struct NativeTechniqueSetInfo_s *techniqueSetInfo;
  int targetIndex;
} NativeLoadedMaterial_t;

typedef struct NativeTechniqueSetInfo_s
{
  const char *name;
  int targetIndex;
  ShaderInfo_t *expandedMaterial;
  int nativePayload[3];
} NativeTechniqueSetInfo_t;

static NativeLoadedMaterial_t s_nativeLoadedMaterials[4096];
static ShaderInfo_t *s_nativeMaterialExpanded[4096];
static int s_nativeLoadedMaterialCount;
static NativeTechniqueSetInfo_t s_nativeTechniqueSetInfos[4096];
static int s_nativeTechniqueSetInfoCount;

#define NATIVE_MATERIAL_TARGET_PC 4

typedef struct CoalescibleMaterialName_s
{
  const char *techniqueSetName;
  const char *propertyCode;
} CoalescibleMaterialName_t;

/* Native 0x4FC228 table.  Only fields +0 and +12 are consumed by
   FindMaterialNameInStaticList/SelectCoalescibleLayerCount. */
static const CoalescibleMaterialName_t s_coalescibleMaterialNames[] = {
  { "l_sm_a0c0",       "a0c0" },
  { "l_sm_a0c0d0",     "a0c0d0" },
  { "l_sm_a0c0d0n0",   "a0c0d0n0" },
  { "l_sm_a0c0d0n0s0", "a0c0d0n0s0" },
  { "l_sm_a0c0d0s0",   "a0c0d0s0" },
  { "l_sm_a0c0n0",     "a0c0n0" },
  { "l_sm_a0c0n0s0",   "a0c0n0s0" },
  { "l_sm_a0c0s0",     "a0c0s0" },
  { "l_sm_b0c0",       "b0c0" },
  { "l_sm_b0c0d0",     "b0c0d0" },
  { "l_sm_b0c0d0n0",   "b0c0d0n0" },
  { "l_sm_b0c0d0n0s0", "b0c0d0n0s0" },
  { "l_sm_b0c0d0s0",   "b0c0d0s0" },
  { "l_sm_b0c0n0",     "b0c0n0" },
  { "l_sm_b0c0n0s0",   "b0c0n0s0" },
  { "l_sm_b0c0s0",     "b0c0s0" },
  { "l_sm_r0c0",       "r0c0" },
  { "l_sm_r0c0d0",     "r0c0d0" },
  { "l_sm_r0c0d0n0",   "r0c0d0n0" },
  { "l_sm_r0c0d0n0s0", "r0c0d0n0s0" },
  { "l_sm_r0c0d0s0",   "r0c0d0s0" },
  { "l_sm_r0c0n0",     "r0c0n0" },
  { "l_sm_r0c0n0s0",   "r0c0n0s0" },
  { "l_sm_r0c0s0",     "r0c0s0" },
  { "l_sm_t0c0",       "t0c0" },
  { "l_sm_t0c0d0",     "t0c0d0" },
  { "l_sm_t0c0d0n0",   "t0c0d0n0" },
  { "l_sm_t0c0d0n0s0", "t0c0d0n0s0" },
  { "l_sm_t0c0d0s0",   "t0c0d0s0" },
  { "l_sm_t0c0n0",     "t0c0n0" },
  { "l_sm_t0c0n0s0",   "t0c0n0s0" },
  { "l_sm_t0c0s0",     "t0c0s0" },
  { "unlit_multiply",   "m0c0" }
};

static const CoalescibleMaterialName_t *FindMaterialNameInStaticList(
  const ShaderInfo_t *material)
{
  int begin = 0;
  int end = sizeof(s_coalescibleMaterialNames) / sizeof(s_coalescibleMaterialNames[0]);

  while ( begin < end )
  {
    int middle = (begin + end) / 2;
    int comparison = strcmp(material->techniqueSetName,
                            s_coalescibleMaterialNames[middle].techniqueSetName);
    if ( comparison < 0 )
      end = middle;
    else if ( comparison > 0 )
      begin = middle + 1;
    else
      return &s_coalescibleMaterialNames[middle];
  }
  return NULL;
}

static void CopyMaterialString(char *dest, int destSize, const char *source)
{
  if ( !source )
    source = "";
  strncpy(dest, source, destSize - 1);
  dest[destSize - 1] = 0;
}

static const char *MaterialFileString(const void *fileData, int fileSize, int offset)
{
  const char *value;

  if ( offset < 0 || offset >= fileSize )
    return "";
  value = (const char *)fileData + offset;
  if ( !memchr(value, 0, fileSize - offset) )
    return "";
  return value;
}

/* CoD4 0x4246F0.  The raw material allocation is retained by the native
   loaded-material sidecar, but layered-descriptor construction only needs
   this stable boolean.  Resolve it while the file bounds are still known. */
static int Material_HasNonIdentityNormalMap(const void *fileData, int fileSize)
{
  const Material_t *material;
  int textureCount;
  int textureOffset;
  int textureIndex;

  if (!fileData || fileSize < (int)sizeof(Material_t))
    return 0;

  material = (const Material_t *)fileData;
  textureCount = material->textureCount;
  textureOffset = material->textures;
  if (textureCount < 0 || textureCount > 256
      || textureOffset < 0
      || textureOffset > fileSize - textureCount * (int)sizeof(MaterialTextureDef_t))
    return 0;

  for (textureIndex = 0; textureIndex < textureCount; ++textureIndex)
  {
    const MaterialTextureDef_t *texture = (const MaterialTextureDef_t *)
      ((const char *)fileData + textureOffset
       + textureIndex * (int)sizeof(MaterialTextureDef_t));
    const char *name = MaterialFileString(fileData, fileSize, texture->name);

    if (!strcmp(name, "normalMap"))
    {
      const char *value = MaterialFileString(fileData, fileSize, texture->u.image);
      return *value && strcmp(value, "$identitynormalmap") != 0;
    }
  }
  return 0;
}

int HasNonIdentityNormalMap(const ShaderInfo_t *material)
{
  return material && material->hasNonIdentityNormalMap;
}

/* Native 0x424FB0.  Keep this public because the post-snap triangle
   validation uses the same static technique-set table as layer selection. */
int MaterialNameInStaticList(const ShaderInfo_t *material)
{
  return material && FindMaterialNameInStaticList(material) != NULL;
}

static int ExtractTechniqueName(const char *text, const char *token, char *name, int nameSize)
{
  const char *tokenPos;
  const char *end;
  const char *start;
  int length;

  tokenPos = strstr(text, token);
  if ( !tokenPos )
    return 0;
  end = strchr(tokenPos, ';');
  if ( !end )
    return 0;
  start = end;
  while ( start > tokenPos
       && (isalnum((unsigned char)start[-1]) || start[-1] == '_') )
    --start;
  length = (int)(end - start);
  if ( length >= nameSize )
    length = nameSize - 1;
  memcpy(name, start, length);
  name[length] = 0;
  return 1;
}

static void ExtractQuotedTechniqueValue(const char *text, const char *key,
                                        char *value, int valueSize)
{
  const char *keyPos;
  const char *start;
  const char *end;
  int length;

  value[0] = 0;
  keyPos = strstr(text, key);
  if ( !keyPos )
    return;
  start = strchr(keyPos, '"');
  if ( !start )
    return;
  ++start;
  end = strchr(start, '"');
  if ( !end )
    return;
  length = (int)(end - start);
  if ( length >= valueSize )
    length = valueSize - 1;
  memcpy(value, start, length);
  value[length] = 0;
}

static void LoadTechniqueShaderNames(ShaderInfo_t *material, const char *techSetText,
                                     const char *token)
{
  char techniqueName[64];
  char path[MAX_OS_PATH];
  void *fileData;

  material->techniquePixelShader[0] = 0;
  material->techniqueVertexShader[0] = 0;
  if ( !ExtractTechniqueName(techSetText, token, techniqueName, sizeof(techniqueName)) )
    return;
  sprintf(path, "techniques/%s.tech", techniqueName);
  if ( FS_LoadFile(path, &fileData) < 0 )
    return;
  ExtractQuotedTechniqueValue((const char *)fileData, "pixelShader",
                              material->techniquePixelShader,
                              sizeof(material->techniquePixelShader));
  ExtractQuotedTechniqueValue((const char *)fileData, "vertexShader",
                              material->techniqueVertexShader,
                              sizeof(material->techniqueVertexShader));
  FS_FreeFile(fileData);
}

static unsigned int GetTechniqueDepthClass(const char *techSetText)
{
  char techniqueName[64];

  if ( ExtractTechniqueName(techSetText, "\"depth prepass\":",
                            techniqueName, sizeof(techniqueName)) )
    return strcmp(techniqueName, "zprepass") != 0;
  if ( ExtractTechniqueName(techSetText, "build floatz",
                            techniqueName, sizeof(techniqueName)) )
    return 2;
  return 3;
}

static void LoadTechniqueSetSortInfo(ShaderInfo_t *material)
{
  char path[MAX_OS_PATH];
  void *fileData;
  const char *text;

  material->techniqueBuildShadowmapDepth = 0;
  material->techniqueHasLit = 0;
  material->techniqueHasEmissive = 0;
  material->techniqueDepthClass = 3;
  material->techniquePixelShader[0] = 0;
  material->techniqueVertexShader[0] = 0;

  /* Every compiler-side LoadMaterial caller in COD4 passes technique-set
     variant 4; off_52910C[4] is the exact "wc_" prefix. */
  sprintf(path, "techsets/wc_%s.techset", material->techniqueSetName);
  if ( FS_LoadFile(path, &fileData) < 0 )
    return;
  text = (const char *)fileData;
  material->techniqueBuildShadowmapDepth = strstr(text, "\"build shadowmap depth\":") != NULL;
  material->techniqueHasLit = strstr(text, "\"lit\":") != NULL;
  material->techniqueHasEmissive = strstr(text, "\"emissive\":") != NULL;
  material->techniqueDepthClass = GetTechniqueDepthClass(text);
  if ( material->techniqueHasLit )
    LoadTechniqueShaderNames(material, text, "\"lit\":");
  else if ( material->techniqueHasEmissive )
    LoadTechniqueShaderNames(material, text, "\"emissive\":");
  FS_FreeFile(fileData);
}


/*
================
FindMaterialInCache

Looks up a material name in the shader cache
================
*/
ShaderInfo_t *FindMaterialInCache(const char *materialName)
{
  int i;

  for ( i = 0; i < g_matExpandRegion.materialCount; i++ )
  {
    if ( !strcmp(materialName, g_matExpandRegion.materialCache[i].name) )
      return &g_matExpandRegion.materialCache[i];
  }
  return NULL;
}

/* CoD4 0x4248E0.  Native names are resolved from the retained material
   asset, while the expanded ShaderInfo_t remains a KIWI-only sidecar. */
static NativeLoadedMaterial_t *FindLoadedMaterial(const char *materialName,
                                                  int targetIndex)
{
  int materialIndex;

  for (materialIndex = 0; materialIndex < s_nativeLoadedMaterialCount;
       ++materialIndex)
  {
    NativeLoadedMaterial_t *entry = &s_nativeLoadedMaterials[materialIndex];
    const MatFileHeader_t *material;
    const char *storedName;

    if (entry->targetIndex != targetIndex)
      continue;
    material = (const MatFileHeader_t *)entry->materialData;
    storedName = (const char *)entry->materialData + material->nameOfs;
    if (!strcmp(materialName, storedName))
      return entry;
  }
  return NULL;
}

/* CoD4 0x424960.  The actual native technique payload is private to the
   material system; the native 24-byte ownership key and target partition are
   preserved here, while parsed sort data stays in ShaderInfo_t. */
static NativeTechniqueSetInfo_t *FindOrAddTechniqueSetInfo(const char *name,
                                                            int targetIndex,
                                                            ShaderInfo_t *material)
{
  int techniqueIndex;
  NativeTechniqueSetInfo_t *info;

  for (techniqueIndex = 0; techniqueIndex < s_nativeTechniqueSetInfoCount;
       ++techniqueIndex)
  {
    info = &s_nativeTechniqueSetInfos[techniqueIndex];
    if (info->targetIndex == targetIndex && !strcmp(info->name, name))
      return info;
  }

  if (s_nativeTechniqueSetInfoCount >= 4096)
    Com_Error("MAX_TECHNIQUESETINFOS");
  info = &s_nativeTechniqueSetInfos[s_nativeTechniqueSetInfoCount++];
  info->name = name;
  info->targetIndex = targetIndex;
  info->expandedMaterial = material;
  memset(info->nativePayload, 0, sizeof(info->nativePayload));
  return info;
}

/* CoD4 0x424520. */
unsigned int GetLoadedMaterialDepthClass(const ShaderInfo_t *material)
{
  return (material->nativeStateBits & 0x30) ? material->techniqueDepthClass : 3;
}

/* CoD4 0x424200.  Native returns a boolean strict-before predicate. */
int CompareLoadedMaterials(const ShaderInfo_t *first, const ShaderInfo_t *second)
{
  int firstHasLightmap;
  int secondHasLightmap;
  int comparison;
  unsigned int firstDepthClass;
  unsigned int secondDepthClass;

  if ( first == second )
    return 0;
  if ( first->techniqueHasLit != second->techniqueHasLit )
    return first->techniqueHasLit != 0;

  firstHasLightmap = (first->gameFlags & 2) != 0;
  secondHasLightmap = (second->gameFlags & 2) != 0;
  if ( first->techniqueHasLit )
  {
    Assert(!first->techniqueHasEmissive, g_matExpandRegion.assertFlag_info);
    Assert(!second->techniqueHasEmissive, g_matExpandRegion.assertFlag_info);
    if ( first->texSortKey != second->texSortKey )
      return first->texSortKey < second->texSortKey;
    if ( firstHasLightmap != secondHasLightmap )
      return firstHasLightmap;
  }
  else
  {
    Assert(!firstHasLightmap, g_matExpandRegion.assertFlag_info);
    Assert(!secondHasLightmap, g_matExpandRegion.assertFlag_info);
    if ( first->techniqueHasEmissive != second->techniqueHasEmissive )
      return first->techniqueHasEmissive != 0;
    if ( first->texSortKey != second->texSortKey )
      return first->texSortKey < second->texSortKey;
  }

  firstDepthClass = GetLoadedMaterialDepthClass(first);
  secondDepthClass = GetLoadedMaterialDepthClass(second);
  if ( firstDepthClass != secondDepthClass )
    return firstDepthClass < secondDepthClass;

  if ( !strcmp(first->techniqueSetName, second->techniqueSetName) )
  {
    /* sub_4053A0 resolves the raw material name at file offset +0. */
    comparison = strcmp(first->name, second->name);
  }
  else
  {
    comparison = strcmp(first->techniquePixelShader, second->techniquePixelShader);
    if ( !comparison )
      comparison = strcmp(first->techniqueVertexShader, second->techniqueVertexShader);
    if ( !comparison )
      /* Native Material_GetName resolves the technique-set name at +52. */
      comparison = strcmp(first->techniqueSetName, second->techniqueSetName);
  }
  Assert(comparison != 0, g_matExpandRegion.assertFlag_info);
  return comparison < 0;
}

/* CoD4 0x4241D0.  Keep ShaderInfo cache addresses stable and sort its pointer
   adapter, which is behaviorally equivalent to sorting native 12-byte records. */
int SortLoadedMaterials()
{
  int i;

  s_loadedMaterialOrderCount = g_matExpandRegion.materialCount;
  for ( i = 0; i < s_loadedMaterialOrderCount; ++i )
    s_loadedMaterialOrder[i] = &g_matExpandRegion.materialCache[i];
  for ( i = 1; i < s_loadedMaterialOrderCount; ++i )
  {
    ShaderInfo_t *material = s_loadedMaterialOrder[i];
    int insert = i;

    while ( insert > 0
         && CompareLoadedMaterials(material, s_loadedMaterialOrder[insert - 1]) )
    {
      s_loadedMaterialOrder[insert] = s_loadedMaterialOrder[insert - 1];
      --insert;
    }
    s_loadedMaterialOrder[insert] = material;
  }
  return s_loadedMaterialOrderCount;
}

/* CoD4 0x4245D0.  The lightmap grouper indexes the sorted native
   loaded-material table, not the insertion-order expanded cache. */
int FindLoadedMaterialIndex(const ShaderInfo_t *material)
{
  int materialIndex;

  for ( materialIndex = 0; materialIndex < s_loadedMaterialOrderCount; ++materialIndex )
  {
    if ( s_loadedMaterialOrder[materialIndex] == material )
      return materialIndex;
  }
  AssertFatal(0, g_matExpandRegion.assertFlag_info);
  return INT_MAX;
}

/* Native 0x451CB0 compares the canonical technique-set-info pointers in the
   12-byte loaded-material cache, rather than requiring identical materials. */
bool LoadedMaterialsShareTechniqueSet(const ShaderInfo_t *first,
                                      const ShaderInfo_t *second)
{
  const NativeTechniqueSetInfo_t *firstTechnique = NULL;
  const NativeTechniqueSetInfo_t *secondTechnique = NULL;
  int materialIndex;

  for (materialIndex = 0; materialIndex < s_nativeLoadedMaterialCount;
       ++materialIndex)
  {
    if (s_nativeMaterialExpanded[materialIndex] == first)
      firstTechnique = s_nativeLoadedMaterials[materialIndex].techniqueSetInfo;
    if (s_nativeMaterialExpanded[materialIndex] == second)
      secondTechnique = s_nativeLoadedMaterials[materialIndex].techniqueSetInfo;
  }
  Assert(firstTechnique && secondTechnique, g_matExpandRegion.assertFlag_info);
  return firstTechnique == secondTechnique;
}

/* CoD4 0x424550. */
int IsLoadedMaterialBefore(const ShaderInfo_t *first, const ShaderInfo_t *second)
{
  int materialIndex;

  for ( materialIndex = 0; materialIndex < s_loadedMaterialOrderCount; ++materialIndex )
  {
    const ShaderInfo_t *loaded = s_loadedMaterialOrder[materialIndex];

    if ( loaded == first )
      return 1;
    if ( loaded == second )
      return 0;
  }
  AssertFatal(0, g_matExpandRegion.assertFlag_info);
  return 0;
}

/* CoD4 0x425060. */
int SelectCoalescibleLayerCount(ShaderInfo_t *const *materials, int layerCount,
                                int preserveDetailLayers)
{
  const CoalescibleMaterialName_t *names[5];
  int normalLayerCount;
  int layerIndex;

  if ( layerCount == 1 )
    return 1;
  Assert(layerCount >= 2 && layerCount <= 5, g_matExpandRegion.assertFlag_info);

  if ( !preserveDetailLayers )
  {
    if ( (materials[0]->toolFlagsWord & 0x70) == 0x70 )
      return 1;
    for ( layerIndex = 1; layerIndex < layerCount; ++layerIndex )
    {
      if ( (materials[layerIndex]->toolFlagsWord & 0x70) == 0x70 )
      {
        layerCount = layerIndex;
        if ( layerIndex == 1 )
          return 1;
        break;
      }
    }
  }

  names[0] = FindMaterialNameInStaticList(materials[0]);
  if ( !names[0] || names[0]->propertyCode[0] == 'm' )
    return 1;

  normalLayerCount = strchr(names[0]->propertyCode, 'n') != NULL;
  for ( layerIndex = 1; layerIndex < layerCount; ++layerIndex )
  {
    names[layerIndex] = FindMaterialNameInStaticList(materials[layerIndex]);
    if ( !names[layerIndex] )
      return layerIndex;
    if ( names[layerIndex]->propertyCode[0] == 'm'
      && names[0]->propertyCode[0] != 'r' )
      return layerIndex;
    if ( strchr(names[layerIndex]->propertyCode, 'n') )
    {
      if ( layerIndex >= 3 )
        return layerIndex;
      if ( ++normalLayerCount == 3 )
        return layerIndex + 1;
    }
  }
  return layerCount;
}

/*
================
Material_ByteSwap

Byte-swaps material file data for platform endianness.
Converts all multi-byte fields from big-endian to little-endian.
================
*/
int Material_ByteSwap(char *data)
{
  MatFileHeader_t *mat;

  Assert(data, g_matExpandRegion.assertFlag_info);

  Swap_Init();

  mat = (MatFileHeader_t *)data;

  /* swap 16-bit fields */
  mat->toolFlags    = BigShort(mat->toolFlags);
  mat->autoTexScaleW = BigShort(mat->autoTexScaleW);
  mat->autoTexScaleH = BigShort(mat->autoTexScaleH);

  /* swap 32-bit fields */
  mat->locale       = BigLong(mat->locale);
  { union { float f; int i; } u; u.f = mat->maxDeformMove; u.i = BigFloat(u.i); mat->maxDeformMove = u.f; }
  { union { float f; int i; } u; u.f = mat->tessSize; u.i = BigFloat(u.i); mat->tessSize = u.f; }
  mat->surfaceFlags = BigLong(mat->surfaceFlags);
  mat->contentFlags = BigLong(mat->contentFlags);

  return Swap_InitByteSwap();
}

/* material loading statistics */
static int materialsLoaded = 0;
static int materialsCached = 0;
static int materialsFailed = 0;
static char failedMaterials[MAX_TRACKED_FAILURES][MAX_QPATH];

/* 0x44BF80/0x44BF10.  Native colorTint constants are floats but patch
   winding generation consumes BGRA bytes, rounded with the x87 +2^-30
   conversion and saturated to the unsigned-byte domain. */
static unsigned char Material_ColorTintByte(float value)
{
  int component = RoundFloatToInt(value * 255.0f);

  if (component < 0)
    component = 0;
  else if (component > 255)
    component = 255;
  return (unsigned char)component;
}

/* 0x43F9E0/0x43FA30.  The raw material tail is immediately after the
   0x28-byte header.  Only colorTint is needed by the compiler-side draw
   surface path; absent constants use native (1,1,1,1). */
static void Material_LoadColorTint(const void *fileData, int fileSize, ShaderInfo_t *matEntry)
{
  const unsigned char *bytes = (const unsigned char *)fileData;
  const unsigned short *constantCount;
  const int *constantOffset;
  int index;

  matEntry->colorTint[0] = 255;
  matEntry->colorTint[1] = 255;
  matEntry->colorTint[2] = 255;
  matEntry->colorTint[3] = 255;
  if (fileSize < 0x40)
    return;

  constantCount = (const unsigned short *)(bytes + 0x32);
  constantOffset = (const int *)(bytes + 0x3C);
  if (*constantOffset < 0 || *constantOffset > fileSize
      || *constantCount > (unsigned int)(fileSize - *constantOffset) / sizeof(MaterialConstantDefObj_t))
  {
    return;
  }

  for (index = 0; index < *constantCount; ++index)
  {
    const MaterialConstantDefObj_t *constant = (const MaterialConstantDefObj_t *)(bytes + *constantOffset)
        + index;
    const char *constantName;

    if (constant->name < 0 || constant->name >= fileSize)
      continue;
    constantName = (const char *)bytes + constant->name;
    if (strcmp(constantName, "colorTint"))
      continue;

    /* Native 0x44BF10 writes floats RGB[A] into a BGRA byte carrier. */
    matEntry->colorTint[2] = Material_ColorTintByte(constant->literal[0]);
    matEntry->colorTint[1] = Material_ColorTintByte(constant->literal[1]);
    matEntry->colorTint[0] = Material_ColorTintByte(constant->literal[2]);
    matEntry->colorTint[3] = Material_ColorTintByte(constant->literal[3]);
    return;
  }
}

/*
================
LoadMaterial

Loads a material by name, caching and byte-swapping as needed
================
*/
static ShaderInfo_t *LoadMaterialForTarget(const char *materialName, int targetIndex);

ShaderInfo_t *LoadMaterial(const char *materialName)
{
  return LoadMaterialForTarget(materialName, NATIVE_MATERIAL_TARGET_PC);
}

static ShaderInfo_t *LoadMaterialForTarget(const char *materialName, int targetIndex)
{
  ShaderInfo_t *result, *matEntry;
  NativeLoadedMaterial_t *nativeEntry;
  int assertResult;
  void *fileData;
  char filePath[MAX_OS_PATH];

  if (!materialName || strlen(materialName) >= sizeof(((ShaderInfo_t *)0)->name))
    Com_Error("LoadMaterial: material name is missing or exceeds %i characters",
              (int)sizeof(((ShaderInfo_t *)0)->name) - 1);

  nativeEntry = FindLoadedMaterial(materialName, targetIndex);
  if (nativeEntry)
  {
    materialsCached++;
    return s_nativeMaterialExpanded[nativeEntry - s_nativeLoadedMaterials];
  }

  /* Compatibility-only fallback for a cache populated outside LoadMaterial. */
  result = FindMaterialInCache(materialName);

  if ( !result )
  {
    /* validate target platform state */
    if ( !g_targetPlatform && !g_matExpandRegion.assertFlag_platform )
    {
      assertResult = AssertHandler("targetPlatform", ".\\materials.cpp", 165, 0, 1);
      if ( assertResult )
      {
        if ( assertResult == 2 )
          g_matExpandRegion.assertFlag_platform = 1;
      }
      else
      {
        DebugBreak();
      }
    }

    if ( !g_targetPlatform->materialDirectory && !g_matExpandRegion.assertFlag_matDir0 )
    {
      assertResult = AssertHandler("targetPlatform->materialDirectory", ".\\materials.cpp", 166, 0, 1);
      if ( assertResult )
      {
        if ( assertResult == 2 )
          g_matExpandRegion.assertFlag_matDir0 = 1;
      }
      else
      {
        DebugBreak();
      }
    }

    if ( !*g_targetPlatform->materialDirectory && !g_matExpandRegion.assertFlag_matDir )
    {
      assertResult = AssertHandler("targetPlatform->materialDirectory[0]", ".\\materials.cpp", 167, 0, 1);
      if ( assertResult )
      {
        if ( assertResult == 2 )
          g_matExpandRegion.assertFlag_matDir = 1;
      }
      else
      {
        DebugBreak();
      }
    }

    /* build file path and load from IWD */
    sprintf(filePath, "%s/%s", g_targetPlatform->materialDirectory, materialName);

    int fileSize = FS_LoadFile(filePath, &fileData);
    if ( fileSize >= 0 )
    {
      if ( fileSize < 0x40 )
      {
        FS_FreeFile(fileData);
        if ( !strcmp(materialName, "$default") )
          Com_Error("Material '$default' is corrupt");
        printf("Material '%s' is corrupt\n", materialName);
        return LoadMaterialForTarget("$default", targetIndex);
      }

      /* allocate cache slot after successful load */
      if (g_matExpandRegion.materialCount >= 4096
          || s_nativeLoadedMaterialCount >= 4096)
        Com_Error("MAX_MATERIALS");
      matEntry = &g_matExpandRegion.materialCache[g_matExpandRegion.materialCount++];

      /* byte-swap on little-endian platforms */
      int bigEndian = g_targetPlatform->bigEndian;
      int needsSwap = !bigEndian;

      if (needsSwap) {
        Material_ByteSwap((char *)fileData);
      }

      /* parse material header and copy properties to cache entry */
      {
        MatFileHeader_t *matData = (MatFileHeader_t *)fileData;
        unsigned short toolFlagsWord;
        unsigned char gameFlags;
        float tessSize;
        unsigned char contentByte;
        int techniqueSetOffset;

        strcpy(matEntry->name, materialName);
        CopyMaterialString(matEntry->referenceImageName,
                           sizeof(matEntry->referenceImageName),
                           MaterialFileString(fileData, fileSize, matData->refImageOfs));
        techniqueSetOffset = fileSize >= 56
                           ? *(const int *)((const char *)fileData + 52)
                           : 0;
        CopyMaterialString(matEntry->techniqueSetName,
                           sizeof(matEntry->techniqueSetName),
                           MaterialFileString(fileData, fileSize, techniqueSetOffset));
        matEntry->nativeStateBits = fileSize >= 48
                                  ? *(const unsigned int *)((const char *)fileData + 44)
                                  : 0;
        matEntry->rawTextureCountHighByte = fileSize > 53
                                          ? *((const unsigned char *)fileData + 53)
                                          : 0;
        matEntry->surfaceFlags = matData->surfaceFlags;
        matEntry->contentFlags = matData->contentFlags;
        toolFlagsWord = matData->toolFlags;
        gameFlags = matData->gameFlags;
        tessSize = matData->tessSize;
        matEntry->toolFlagsWord = toolFlagsWord;
        matEntry->gameFlags = gameFlags;
        matEntry->unused80 = toolFlagsWord & 1;
        matEntry->texSortKey = matData->sortKey;

        /* use default tessellation size if material specifies zero */
        if (tessSize == 0.0f) {
          matEntry->subdivisions = g_matExpandRegion.defaultTessSize;
        } else {
          matEntry->subdivisions = tessSize;
        }

        matEntry->reserved92 = 0;
        matEntry->surfFlags_bit7 = (matEntry->surfaceFlags >> 7) & 1;
        matEntry->globalTexture = (toolFlagsWord & TOOL_GLOBAL_TEXTURE) ? 1 : 0;

        /* check for non-colliding content or tool type */
        contentByte = (unsigned char)matData->contentFlags;
        if ((contentByte & CONTENTS_NONCOLLIDING) || ((toolFlagsWord & TOOL_TYPE_MASK) == TOOL_TYPE_NONCOLLIDE)) {
          matEntry->unused100 = 1;
        } else {
          matEntry->unused100 = 0;
        }

        matEntry->autoTexScaleW = matData->autoTexScaleW;
        matEntry->autoTexScaleH = matData->autoTexScaleH;
        matEntry->reserved120 = 0;
        Material_LoadColorTint(fileData, fileSize, matEntry);
        LoadTechniqueSetSortInfo(matEntry);
        matEntry->hasNonIdentityNormalMap = Material_HasNonIdentityNormalMap(fileData, fileSize);
      }

      /* Native 0x424760 retains the raw FS buffer in its 12-byte record.
         This has to outlive ShaderInfo_t parsing because name lookup and the
         target-indexed cache key are asset-backed in the original. */
      nativeEntry = &s_nativeLoadedMaterials[s_nativeLoadedMaterialCount];
      nativeEntry->materialData = fileData;
      nativeEntry->techniqueSetInfo = FindOrAddTechniqueSetInfo(
          matEntry->techniqueSetName, targetIndex, matEntry);
      nativeEntry->targetIndex = targetIndex;
      s_nativeMaterialExpanded[s_nativeLoadedMaterialCount] = matEntry;
      ++s_nativeLoadedMaterialCount;
      materialsLoaded++;
      return matEntry;
    }
    else
    {
      /* track failed material loads */
      if (materialsFailed < MAX_TRACKED_FAILURES) {
        strncpy(failedMaterials[materialsFailed], materialName ? materialName : "(null)", sizeof(failedMaterials[0]) - 1);
        failedMaterials[materialsFailed][sizeof(failedMaterials[0]) - 1] = 0;
      }
      materialsFailed++;

      if ( !strcmp(materialName, "$default") )
        Com_Error("Cannot find material '$default'");

      printf("Material '%s' is missing\n", materialName);

      /* fall back to $default material */
      return LoadMaterialForTarget("$default", targetIndex);
    }
  }

  materialsCached++;
  return result;
}
