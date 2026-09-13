#pragma once
#include <stddef.h>
#include <stdint.h>

struct LinkerModelLodSource
{
    float distance;
    char filename[1024];
};
struct LinkerModelConfig
{
    uint8_t flags;
    float mins[3];
    float maxs[3];
    char physicsPreset[1024];
    LinkerModelLodSource lods[4];
    int32_t collisionLod;
};
// Reads the version-25 prefix. Remaining bytes contain model collision/physics data.
bool Linker_ReadModelConfig(const void *data, size_t size, LinkerModelConfig *config, size_t *consumed, char *error,
                            size_t errorSize);

struct XModelCollSurf_s;
struct LinkerModelCollision
{
    XModelCollSurf_s *surfaces;
    int count;
    int contents;
};
bool Linker_ReadModelCollision(const void *data, size_t size, LinkerModelCollision **collision, size_t *consumed,
                               char *error, size_t errorSize);
void Linker_FreeModelCollision(LinkerModelCollision *collision);

struct LinkerBonePose
{
    float quat[4];
    float trans[3];
    float transWeight;
};
struct LinkerModelSkeleton
{
    uint16_t boneCount;
    uint16_t rootCount;
    uint8_t parents[128];
    float translations[128][3];
    int16_t quaternions[128][4];
    char names[128][256];
    uint8_t classification[128];
    bool useBones;
    LinkerBonePose basePose[128];
};
// Child arrays are indexed relative to rootCount. Caller releases with free().
bool Linker_ReadModelSkeleton(const void *data, size_t size, LinkerModelSkeleton **skeleton, char *error,
                              size_t errorSize);

struct LinkerModelVertex
{
    float normal[3];
    uint8_t color[4]; // Authored BGRA bytes, before renderer packing.
    float texCoord[2];
    float binormal[3];
    float tangent[3];
    float position[3];
    uint16_t bones[4];
    uint16_t weights[3]; // Additional influences; the first weight is implicit.
    uint8_t extraWeights;
};
struct LinkerModelRigidGroup
{
    uint16_t vertexCount;
    uint16_t bone;
};
struct LinkerModelSurfaceSource
{
    uint8_t tileMode;
    uint16_t vertexCount;
    uint16_t triangleCount;
    bool deformed;
    uint16_t rigidGroupCount;
    LinkerModelRigidGroup rigidGroups[128];
    LinkerModelVertex *vertices;
    uint16_t *indices;
};
struct LinkerModelMesh
{
    uint16_t surfaceCount;
    LinkerModelSurfaceSource *surfaces;
};
// Parses authored surfaces without GPU packing or renderer/runtime allocation.
bool Linker_ReadModelMesh(const void *data, size_t size, unsigned int boneCount, LinkerModelMesh **mesh, char *error,
                          size_t errorSize);
void Linker_FreeModelMesh(LinkerModelMesh *mesh);

struct XSurfaceVertexInfo;
// Emits the native skinner's partitioned bone-offset/weight stream. Free vertsBlend with free().
bool Linker_BuildModelBlendInfo(const LinkerModelSurfaceSource *source, unsigned int boneCount,
                                XSurfaceVertexInfo *info, char *error, size_t errorSize);

struct GfxPackedVertex;
union PackedUnitVec;
bool Linker_PackUnitVector(const float *input, PackedUnitVec *out);
// Caller frees the returned vertex array with free().
bool Linker_PackModelVertices(const LinkerModelSurfaceSource *source, GfxPackedVertex **vertices, char *error,
                              size_t errorSize);

struct XSurface;
bool Linker_BuildModelSurfaces(const LinkerModelMesh *mesh, unsigned int boneCount, XSurface **surfaces, char *error,
                               size_t errorSize);
void Linker_FreeModelSurfaces(XSurface *surfaces, unsigned int count);

struct XBoneInfo;
struct LinkerModelMetadata
{
    uint16_t lodSurfaceCounts[4];
    uint16_t surfaceCount;
    uint16_t boneCount;
    char materials[255][256];
    XBoneInfo *boneInfo;
};
bool Linker_ReadModelMetadata(const void *data, size_t size, const LinkerModelConfig *config, unsigned int boneCount,
                              LinkerModelMetadata **metadata, char *error, size_t errorSize);
void Linker_FreeModelMetadata(LinkerModelMetadata *metadata);

struct LinkerModelSource
{
    LinkerModelConfig config;
    LinkerModelSkeleton *skeleton;
    LinkerModelCollision *collision;
    LinkerModelMetadata *metadata;
};
// Combines the xmodel file with its first-LOD xmodelparts file and checks cross-file bone references.
bool Linker_ReadModelSource(const void *modelData, size_t modelSize, const void *skeletonData, size_t skeletonSize,
                            LinkerModelSource **source, char *error, size_t errorSize);
void Linker_FreeModelSource(LinkerModelSource *source);

struct XModel;
struct Material;
struct PhysPreset;
struct PhysGeomList;
// Source, packed surfaces, material table and physics dependencies remain borrowed.
// Copies the name, script-string indices and base pose. Free with Linker_FreeAssembledModel.
bool Linker_AssembleModel(const char *name, const LinkerModelSource *source, XSurface *surfaces,
                          unsigned int surfaceCount, Material **materials, const uint16_t *boneNames,
                          PhysPreset *preset, PhysGeomList *physics, XModel **model, char *error, size_t errorSize);
void Linker_FreeAssembledModel(XModel *model);
