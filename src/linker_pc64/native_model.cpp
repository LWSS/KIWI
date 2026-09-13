#include <universal/q_shared.h>
#include <xanim/xmodel.h>
#include <xanim/xanim.h>
#include <universal/com_math.h>
#include <stdlib.h>
#include <limits.h>
#include "native_model.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

void Linker_FreeAssembledModel(XModel *model)
{
    if (model)
    {
        free(model->boneNames);
        free(model->baseMat);
        free(model);
    }
}

bool Linker_AssembleModel(const char *name, const LinkerModelSource *source, XSurface *surfaces,
                          unsigned int surfaceCount, Material **materials, const uint16_t *boneNames,
                          PhysPreset *preset, PhysGeomList *physics, XModel **model, char *error, size_t errorSize)
{
    if (!model || (!error && errorSize))
    {
        return false;
    }
    *model = NULL;
    if (!name || !name[0] || !source || !source->skeleton || !source->metadata || !source->collision || !surfaces ||
        !materials || !boneNames || surfaceCount != source->metadata->surfaceCount || !surfaceCount ||
        surfaceCount > 255)
    {
        return false;
    }
    const LinkerModelSkeleton *skeleton = source->skeleton;
    const unsigned int bones = skeleton->boneCount;
    if (!bones || bones > 128 || skeleton->rootCount > bones || source->metadata->boneCount != bones)
    {
        return false;
    }
    XModel *result = (XModel *)calloc(1, sizeof(XModel) + strlen(name) + 1);
    if (!result)
    {
        return false;
    }
    result->boneNames = (uint16_t *)malloc(bones * sizeof(uint16_t));
    result->baseMat = (DObjAnimMat *)calloc(bones, sizeof(DObjAnimMat));
    bool valid = result->boneNames && result->baseMat;
    if (valid)
    {
        result->name = (char *)(result + 1);
        strcpy((char *)result->name, name);
        result->numBones = (uint8_t)bones;
        result->numRootBones = (uint8_t)skeleton->rootCount;
        memcpy(result->boneNames, boneNames, bones * sizeof(uint16_t));
        for (unsigned int i = 0; i < bones; ++i)
        {
            memcpy(result->baseMat[i].quat, skeleton->basePose[i].quat, sizeof(float[4]));
            memcpy(result->baseMat[i].trans, skeleton->basePose[i].trans, sizeof(float[3]));
            result->baseMat[i].transWeight = skeleton->basePose[i].transWeight;
        }
        if (bones > skeleton->rootCount)
        {
            result->parentList = (uint8_t *)skeleton->parents;
            result->quats = (int16_t *)skeleton->quaternions;
            result->trans = (float *)skeleton->translations;
        }
        result->partClassification = (uint8_t *)skeleton->classification;
        result->boneInfo = source->metadata->boneInfo;
        result->surfs = surfaces;
        result->numsurfs = (uint8_t)surfaceCount;
        result->materialHandles = materials;
        result->collSurfs = source->collision->surfaces;
        result->numCollSurfs = source->collision->count;
        result->contents = source->collision->contents;
        result->collLod = (int16_t)source->config.collisionLod;
        result->flags = source->config.flags;
        result->physPreset = preset;
        result->physGeoms = physics;
        result->memUsage = sizeof(XModel) + bones * (sizeof(uint16_t) + sizeof(DObjAnimMat));
        unsigned int first = 0;
        for (unsigned int lod = 0; valid && lod < 4; ++lod)
        {
            XModelLodInfo *info = &result->lodInfo[lod];
            info->dist = source->config.lods[lod].distance == 0 ? 1000000.0f : source->config.lods[lod].distance;
            if (!source->config.lods[lod].filename[0])
            {
                continue;
            }
            const unsigned int count = source->metadata->lodSurfaceCounts[lod];
            if (lod != result->numLods || !count || count > surfaceCount - first)
            {
                valid = false;
                break;
            }
            ++result->numLods;
            info->numsurfs = (uint16_t)count;
            info->surfIndex = (uint16_t)first;
            info->lod = (uint8_t)lod;
            for (unsigned int i = first; i < first + count; ++i)
            {
                valid = valid && materials[i] != NULL;
                result->lodRampType |= surfaces[i].deformed != 0;
                for (unsigned int p = 0; p < 4; ++p)
                {
                    info->partBits[p] |= surfaces[i].partBits[p];
                }
            }
            first += count;
        }
        valid = valid && first == surfaceCount && result->numLods && result->collLod >= -1 &&
                result->collLod < result->numLods;
        for (unsigned int a = 0; a < 3; ++a)
        {
            result->mins[a] = FLT_MAX;
            result->maxs[a] = -FLT_MAX;
        }
        bool hasVertices = false;
        for (unsigned int s = 0; valid && s < result->lodInfo[0].numsurfs; ++s)
        {
            valid = surfaces[s].vertCount && surfaces[s].verts0;
            for (unsigned int v = 0; valid && v < surfaces[s].vertCount; ++v)
            {
                hasVertices = true;
                for (unsigned int a = 0; a < 3; ++a)
                {
                    const float value = surfaces[s].verts0[v].xyz[a];
                    valid = valid && isfinite(value);
                    if (value < result->mins[a])
                    {
                        result->mins[a] = value;
                    }
                    if (value > result->maxs[a])
                    {
                        result->maxs[a] = value;
                    }
                }
            }
        }
        double radiusSquared = 0;
        for (unsigned int a = 0; a < 3; ++a)
        {
            const double extent = fmax(fabs(result->mins[a]), fabs(result->maxs[a]));
            radiusSquared += extent * extent;
        }
        result->radius = (float)sqrt(radiusSquared);
        valid = valid && hasVertices && isfinite(result->radius);
    }
    if (!valid)
    {
        Linker_FreeAssembledModel(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid model assembly dependencies, LODs or vertices");
        }
        return false;
    }
    *model = result;
    return true;
}

struct ModelCursor
{
    const uint8_t *data;
    size_t size;
    size_t position;
};
static bool Read(ModelCursor *cursor, void *out, size_t size)
{
    if (size > cursor->size - cursor->position)
    {
        return false;
    }
    memcpy(out, cursor->data + cursor->position, size);
    cursor->position += size;
    return true;
}
static bool ReadString(ModelCursor *cursor, char *out, size_t capacity)
{
    const uint8_t *end = (const uint8_t *)memchr(cursor->data + cursor->position, 0, cursor->size - cursor->position);
    if (!end)
    {
        return false;
    }
    const size_t length = end - (cursor->data + cursor->position);
    if (length >= capacity)
    {
        return false;
    }
    return Read(cursor, out, length + 1);
}

bool Linker_ReadModelConfig(const void *data, size_t size, LinkerModelConfig *config, size_t *consumed, char *error,
                            size_t errorSize)
{
    if (!config || !consumed || (!error && errorSize))
    {
        return false;
    }
    *consumed = 0;
    memset(config, 0, sizeof(LinkerModelConfig));
    ModelCursor cursor = {(const uint8_t *)data, size, 0};
    LinkerModelConfig result = {};
    uint16_t version = 0;
    bool ok = data && Read(&cursor, &version, sizeof(uint16_t)) && version == 25 &&
              Read(&cursor, &result.flags, sizeof(uint8_t)) && Read(&cursor, result.mins, sizeof(float[3])) &&
              Read(&cursor, result.maxs, sizeof(float[3])) &&
              ReadString(&cursor, result.physicsPreset, sizeof(result.physicsPreset));
    for (int i = 0; ok && i < 4; ++i)
    {
        ok = Read(&cursor, &result.lods[i].distance, sizeof(float)) &&
             ReadString(&cursor, result.lods[i].filename, sizeof(result.lods[i].filename)) &&
             isfinite(result.lods[i].distance);
    }
    ok = ok && Read(&cursor, &result.collisionLod, sizeof(int32_t)) && result.collisionLod >= -1 &&
         result.collisionLod < 4;
    for (int axis = 0; ok && axis < 3; ++axis)
    {
        ok = isfinite(result.mins[axis]) && isfinite(result.maxs[axis]) && result.mins[axis] <= result.maxs[axis];
    }
    if (!ok)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or truncated version-25 model configuration");
        }
        return false;
    }
    *config = result;
    *consumed = cursor.position;
    return true;
}

void Linker_FreeModelCollision(LinkerModelCollision *collision)
{
    if (collision)
    {
        if (collision->surfaces)
        {
            for (int i = 0; i < collision->count; ++i)
            {
                free(collision->surfaces[i].collTris);
            }
        }
        free(collision->surfaces);
        free(collision);
    }
}

static bool ReadCollision(ModelCursor *cursor, LinkerModelCollision *collision)
{
    int32_t count;
    if (!Read(cursor, &count, sizeof(int32_t)) || count < 0 || count > INT_MAX / sizeof(XModelCollSurf_s) ||
        (size_t)count > (cursor->size - cursor->position) / (sizeof(int32_t) + sizeof(XModelCollTri_s) + 36))
    {
        return false;
    }
    collision->count = count;
    collision->surfaces = count ? (XModelCollSurf_s *)calloc(count, sizeof(XModelCollSurf_s)) : NULL;
    if (count && !collision->surfaces)
    {
        return false;
    }
    for (int i = 0; i < count; ++i)
    {
        XModelCollSurf_s *surface = &collision->surfaces[i];
        int32_t triangles;
        if (!Read(cursor, &triangles, sizeof(int32_t)) || triangles <= 0 ||
            triangles > INT_MAX / sizeof(XModelCollTri_s) ||
            (size_t)triangles > (cursor->size - cursor->position) / sizeof(XModelCollTri_s))
        {
            return false;
        }
        surface->numCollTris = triangles;
        surface->collTris = (XModelCollTri_s *)malloc((size_t)triangles * sizeof(XModelCollTri_s));
        if (!surface->collTris || !Read(cursor, surface->collTris, (size_t)triangles * sizeof(XModelCollTri_s)))
        {
            return false;
        }
        for (int j = 0; j < triangles; ++j)
        {
            const XModelCollTri_s *tri = &surface->collTris[j];
            for (int k = 0; k < 4; ++k)
            {
                if (!isfinite(tri->plane[k]) || !isfinite(tri->svec[k]) || !isfinite(tri->tvec[k]))
                {
                    return false;
                }
            }
            const double length = sqrt((double)tri->plane[0] * tri->plane[0] + (double)tri->plane[1] * tri->plane[1] +
                                       (double)tri->plane[2] * tri->plane[2]);
            if (fabs(length - 1) >= 0.01)
            {
                return false;
            }
        }
        if (!Read(cursor, surface->mins, sizeof(float[3])) || !Read(cursor, surface->maxs, sizeof(float[3])) ||
            !Read(cursor, &surface->boneIdx, sizeof(int32_t)) || !Read(cursor, &surface->contents, sizeof(int32_t)) ||
            !Read(cursor, &surface->surfFlags, sizeof(int32_t)))
        {
            return false;
        }
        surface->contents &= 0xDFFFFFFB;
        if (surface->boneIdx < -1 || surface->boneIdx >= 128 || (surface->contents && surface->boneIdx < 0))
        {
            return false;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!isfinite(surface->mins[axis]) || !isfinite(surface->maxs[axis]) ||
                surface->mins[axis] > surface->maxs[axis])
            {
                return false;
            }
            surface->mins[axis] -= EQUAL_EPSILON;
            surface->maxs[axis] += EQUAL_EPSILON;
        }
        collision->contents |= surface->contents;
    }
    return true;
}

bool Linker_ReadModelCollision(const void *data, size_t size, LinkerModelCollision **collision, size_t *consumed,
                               char *error, size_t errorSize)
{
    if (!collision || !consumed || (!error && errorSize))
    {
        return false;
    }
    *collision = NULL;
    *consumed = 0;
    LinkerModelCollision *result = (LinkerModelCollision *)calloc(1, sizeof(LinkerModelCollision));
    ModelCursor cursor = {(const uint8_t *)data, size, 0};
    if (!data || size > INT_MAX || !result || !ReadCollision(&cursor, result))
    {
        Linker_FreeModelCollision(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid, truncated or unallocatable model collision data");
        }
        return false;
    }
    *collision = result;
    *consumed = cursor.position;
    return true;
}

static bool SkeletonBasePose(LinkerModelSkeleton *skeleton)
{
    for (int i = 0; i < skeleton->rootCount; ++i)
    {
        skeleton->basePose[i].quat[3] = 1;
        skeleton->basePose[i].transWeight = 2;
    }
    for (int i = skeleton->rootCount; i < skeleton->boneCount; ++i)
    {
        const int child = i - skeleton->rootCount;
        const LinkerBonePose *parent = &skeleton->basePose[i - skeleton->parents[child]];
        LinkerBonePose *pose = &skeleton->basePose[i];
        float q[4];
        for (int k = 0; k < 4; ++k)
        {
            q[k] = (float)(skeleton->quaternions[child][k] * 0.00003051850944757462);
        }
        const float *p = parent->quat;
        pose->quat[0] = q[0] * p[3] + q[3] * p[0] + q[2] * p[1] - q[1] * p[2];
        pose->quat[1] = q[1] * p[3] - q[2] * p[0] + q[3] * p[1] + q[0] * p[2];
        pose->quat[2] = q[2] * p[3] + q[1] * p[0] - q[0] * p[1] + q[3] * p[2];
        pose->quat[3] = q[3] * p[3] - q[0] * p[0] - q[1] * p[1] - q[2] * p[2];
        float length = 0;
        for (int k = 0; k < 4; ++k)
        {
            length += pose->quat[k] * pose->quat[k];
        }
        if (length == 0)
        {
            pose->quat[3] = 1;
            pose->transWeight = 2;
        }
        else
        {
            pose->transWeight = 2 / length;
        }
        const float x = p[0] * parent->transWeight;
        const float y = p[1] * parent->transWeight;
        const float z = p[2] * parent->transWeight;
        const float xx = x * p[0], xy = x * p[1], xz = x * p[2], xw = x * p[3];
        const float yy = y * p[1], yz = y * p[2], yw = y * p[3], zz = z * p[2], zw = z * p[3];
        const float *t = skeleton->translations[child];
        pose->trans[0] = t[0] * (1 - (yy + zz)) + t[1] * (xy - zw) + t[2] * (xz + yw) + parent->trans[0];
        pose->trans[1] = t[0] * (xy + zw) + t[1] * (1 - (xx + zz)) + t[2] * (yz - xw) + parent->trans[1];
        pose->trans[2] = t[0] * (xz - yw) + t[1] * (yz + xw) + t[2] * (1 - (xx + yy)) + parent->trans[2];
        if (!isfinite(pose->transWeight))
        {
            return false;
        }
        for (int k = 0; k < 3; ++k)
        {
            if (!isfinite(pose->trans[k]))
            {
                return false;
            }
        }
    }
    // The raw loader builds the base pose before suppressing animated translations.
    if (!skeleton->useBones)
    {
        memset(skeleton->translations, 0, sizeof(skeleton->translations));
    }
    return true;
}

static bool ReadSkeleton(ModelCursor *cursor, LinkerModelSkeleton *skeleton)
{
    uint16_t version, children, roots;
    if (!Read(cursor, &version, sizeof(uint16_t)) || version != 25 || !Read(cursor, &children, sizeof(uint16_t)) ||
        !Read(cursor, &roots, sizeof(uint16_t)) || (uint32_t)children + roots > 128 || !roots)
    {
        return false;
    }
    skeleton->boneCount = children + roots;
    skeleton->rootCount = roots;
    for (int i = 0; i < children; ++i)
    {
        uint8_t parent;
        if (!Read(cursor, &parent, sizeof(uint8_t)) || parent >= i + roots ||
            !Read(cursor, skeleton->translations[i], sizeof(float[3])) ||
            !Read(cursor, skeleton->quaternions[i], sizeof(int16_t[3])))
        {
            return false;
        }
        skeleton->parents[i] = (uint8_t)(i + roots - parent);
        int64_t remaining = (int64_t)32767 * 32767;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!isfinite(skeleton->translations[i][axis]))
            {
                return false;
            }
            const int64_t value = skeleton->quaternions[i][axis];
            remaining -= value * value;
        }
        skeleton->quaternions[i][3] = remaining > 0 ? (int16_t)floor(sqrt((double)remaining) + 0.5) : 0;
    }
    for (int i = 0; i < skeleton->boneCount; ++i)
    {
        if (!ReadString(cursor, skeleton->names[i], sizeof(skeleton->names[i])))
        {
            return false;
        }
    }
    uint8_t useBones;
    if (!Read(cursor, skeleton->classification, skeleton->boneCount) || !Read(cursor, &useBones, sizeof(uint8_t)))
    {
        return false;
    }
    skeleton->useBones = useBones != 0;
    return cursor->position == cursor->size && SkeletonBasePose(skeleton);
}

bool Linker_ReadModelSkeleton(const void *data, size_t size, LinkerModelSkeleton **skeleton, char *error,
                              size_t errorSize)
{
    if (!skeleton || (!error && errorSize))
    {
        return false;
    }
    *skeleton = NULL;
    LinkerModelSkeleton *result = (LinkerModelSkeleton *)calloc(1, sizeof(LinkerModelSkeleton));
    ModelCursor cursor = {(const uint8_t *)data, size, 0};
    if (!data || size > INT_MAX || !result || !ReadSkeleton(&cursor, result))
    {
        free(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid, truncated or unallocatable version-25 model skeleton");
        }
        return false;
    }
    *skeleton = result;
    return true;
}

void Linker_FreeModelMesh(LinkerModelMesh *mesh)
{
    if (mesh)
    {
        for (int i = 0; i < mesh->surfaceCount; ++i)
        {
            free(mesh->surfaces[i].vertices);
            free(mesh->surfaces[i].indices);
        }
        free(mesh->surfaces);
        free(mesh);
    }
}

static bool ReadFiniteFloats(ModelCursor *cursor, float *values, size_t count)
{
    if (!Read(cursor, values, count * sizeof(float)))
    {
        return false;
    }
    for (size_t i = 0; i < count; ++i)
    {
        if (!isfinite(values[i]))
        {
            return false;
        }
    }
    return true;
}

static bool OrderSkinVertices(LinkerModelSurfaceSource *surface)
{
    if (!surface->deformed)
    {
        return true;
    }
    unsigned int counts[4] = {};
    bool ordered = true;
    for (int i = 0; i < surface->vertexCount; ++i)
    {
        const int weight = surface->vertices[i].extraWeights;
        if (++counts[weight] > SHRT_MAX)
        {
            return false;
        }
        if (i && weight < surface->vertices[i - 1].extraWeights)
        {
            ordered = false;
        }
    }
    if (ordered)
    {
        return true;
    }
    LinkerModelVertex *vertices = (LinkerModelVertex *)malloc(surface->vertexCount * sizeof(LinkerModelVertex));
    uint16_t *remap = (uint16_t *)malloc(surface->vertexCount * sizeof(uint16_t));
    if (!vertices || !remap)
    {
        free(vertices);
        free(remap);
        return false;
    }
    unsigned int next[4] = {0, counts[0], counts[0] + counts[1], counts[0] + counts[1] + counts[2]};
    for (int i = 0; i < surface->vertexCount; ++i)
    {
        const unsigned int index = next[surface->vertices[i].extraWeights]++;
        vertices[index] = surface->vertices[i];
        remap[i] = (uint16_t)index;
    }
    for (int i = 0; i < surface->triangleCount * 3; ++i)
    {
        surface->indices[i] = remap[surface->indices[i]];
    }
    free(remap);
    free(surface->vertices);
    surface->vertices = vertices;
    return true;
}

static bool ReadMeshSurface(ModelCursor *cursor, unsigned int boneCount, LinkerModelSurfaceSource *surface)
{
    uint16_t unused;
    if (!Read(cursor, &surface->tileMode, sizeof(uint8_t)) || !Read(cursor, &unused, sizeof(uint16_t)) ||
        !Read(cursor, &surface->vertexCount, sizeof(uint16_t)) ||
        !Read(cursor, &surface->triangleCount, sizeof(uint16_t)) || !surface->vertexCount || !surface->triangleCount)
    {
        return false;
    }
    unsigned int rigidVertices = 0;
    for (;;)
    {
        uint16_t count, bone;
        if (!Read(cursor, &count, sizeof(uint16_t)))
        {
            return false;
        }
        if (!count)
        {
            break;
        }
        if (surface->rigidGroupCount == 128 || !Read(cursor, &bone, sizeof(uint16_t)) || bone >= boneCount ||
            count > surface->vertexCount - rigidVertices)
        {
            return false;
        }
        LinkerModelRigidGroup *group = &surface->rigidGroups[surface->rigidGroupCount++];
        group->vertexCount = count;
        group->bone = bone;
        rigidVertices += count;
    }
    surface->deformed = rigidVertices != surface->vertexCount;
    if (surface->deformed)
    {
        surface->rigidGroupCount = 0;
    }
    const bool singleRigid = surface->rigidGroupCount == 1;
    uint16_t blendCount = 0;
    if (!singleRigid && !Read(cursor, &blendCount, sizeof(uint16_t)))
    {
        return false;
    }
    // Every vertex has at least 60 bytes on disk, followed by three indices per triangle.
    const size_t minimumBytes =
        (size_t)surface->vertexCount * 60 + (size_t)surface->triangleCount * 3 * sizeof(uint16_t);
    if (minimumBytes > cursor->size - cursor->position)
    {
        return false;
    }
    surface->vertices = (LinkerModelVertex *)calloc(surface->vertexCount, sizeof(LinkerModelVertex));
    surface->indices = (uint16_t *)malloc((size_t)surface->triangleCount * 3 * sizeof(uint16_t));
    if (!surface->vertices || !surface->indices)
    {
        return false;
    }
    unsigned int blendsRead = 0;
    for (int i = 0; i < surface->vertexCount; ++i)
    {
        LinkerModelVertex *vertex = &surface->vertices[i];
        if (!ReadFiniteFloats(cursor, vertex->normal, 3) || !Read(cursor, vertex->color, sizeof(uint8_t[4])) ||
            !ReadFiniteFloats(cursor, vertex->texCoord, 2) || !ReadFiniteFloats(cursor, vertex->binormal, 3) ||
            !ReadFiniteFloats(cursor, vertex->tangent, 3))
        {
            return false;
        }
        if (singleRigid)
        {
            vertex->bones[0] = surface->rigidGroups[0].bone;
        }
        else if (!Read(cursor, &vertex->extraWeights, sizeof(uint8_t)) || vertex->extraWeights > 3 ||
                 (!surface->deformed && vertex->extraWeights) || !Read(cursor, &vertex->bones[0], sizeof(uint16_t)) ||
                 vertex->bones[0] >= boneCount)
        {
            return false;
        }
        if (!ReadFiniteFloats(cursor, vertex->position, 3))
        {
            return false;
        }
        for (int j = 0; j < vertex->extraWeights; ++j)
        {
            if (!Read(cursor, &vertex->bones[j + 1], sizeof(uint16_t)) || vertex->bones[j + 1] >= boneCount ||
                !Read(cursor, &vertex->weights[j], sizeof(uint16_t)))
            {
                return false;
            }
        }
        blendsRead += vertex->extraWeights;
    }
    if (blendsRead != blendCount ||
        !Read(cursor, surface->indices, (size_t)surface->triangleCount * 3 * sizeof(uint16_t)))
    {
        return false;
    }
    for (int i = 0; i < surface->triangleCount * 3; ++i)
    {
        if (surface->indices[i] >= surface->vertexCount)
        {
            return false;
        }
    }
    return OrderSkinVertices(surface);
}

bool Linker_ReadModelMesh(const void *data, size_t size, unsigned int boneCount, LinkerModelMesh **mesh, char *error,
                          size_t errorSize)
{
    if (!mesh || (!error && errorSize))
    {
        return false;
    }
    *mesh = NULL;
    ModelCursor cursor = {(const uint8_t *)data, size, 0};
    uint16_t version = 0, count = 0;
    LinkerModelMesh *result = NULL;
    bool valid = data && size <= INT_MAX && boneCount && boneCount <= 128 &&
                 Read(&cursor, &version, sizeof(uint16_t)) && version == 25 &&
                 Read(&cursor, &count, sizeof(uint16_t)) && count && count <= 255 &&
                 count <= (size - cursor.position) / 75;
    if (valid)
    {
        result = (LinkerModelMesh *)calloc(1, sizeof(LinkerModelMesh));
        if (result)
        {
            result->surfaces = (LinkerModelSurfaceSource *)calloc(count, sizeof(LinkerModelSurfaceSource));
        }
        valid = result && result->surfaces;
    }
    if (valid)
    {
        result->surfaceCount = count;
        for (int i = 0; valid && i < count; ++i)
        {
            valid = ReadMeshSurface(&cursor, boneCount, &result->surfaces[i]);
        }
        valid = valid && cursor.position == size;
    }
    if (!valid)
    {
        Linker_FreeModelMesh(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid, truncated or unallocatable version-25 model surfaces");
        }
        return false;
    }
    *mesh = result;
    return true;
}

bool Linker_BuildModelBlendInfo(const LinkerModelSurfaceSource *source, unsigned int boneCount,
                                XSurfaceVertexInfo *info, char *error, size_t errorSize)
{
    if (!info || (!error && errorSize))
    {
        return false;
    }
    memset(info, 0, sizeof(XSurfaceVertexInfo));
    bool valid = source && boneCount && boneCount <= 128 && source->vertexCount && source->vertices;
    size_t words = 0;
    int previous = 0;
    if (valid && source->deformed)
    {
        for (int i = 0; valid && i < source->vertexCount; ++i)
        {
            const LinkerModelVertex *vertex = &source->vertices[i];
            const int extra = vertex->extraWeights;
            valid = extra >= previous && extra <= 3;
            if (!valid)
            {
                break;
            }
            previous = extra;
            for (int j = 0; j <= extra; ++j)
            {
                valid = valid && vertex->bones[j] < boneCount;
            }
            if (info->vertCount[extra] == SHRT_MAX)
            {
                valid = false;
                break;
            }
            ++info->vertCount[extra];
            words += 1 + 2 * extra;
        }
        if (valid)
        {
            info->vertsBlend = (uint16_t *)malloc(words * sizeof(uint16_t));
            valid = info->vertsBlend != NULL;
        }
        if (valid)
        {
            uint16_t *out = info->vertsBlend;
            for (int i = 0; i < source->vertexCount; ++i)
            {
                const LinkerModelVertex *vertex = &source->vertices[i];
                *out++ = (uint16_t)(vertex->bones[0] * sizeof(DObjSkelMat));
                for (int j = 0; j < vertex->extraWeights; ++j)
                {
                    *out++ = (uint16_t)(vertex->bones[j + 1] * sizeof(DObjSkelMat));
                    *out++ = vertex->weights[j];
                }
            }
        }
    }
    if (!valid)
    {
        free(info->vertsBlend);
        memset(info, 0, sizeof(XSurfaceVertexInfo));
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unallocatable model skin partitions");
        }
        return false;
    }
    return true;
}

bool Linker_PackUnitVector(const float *input, PackedUnitVec *out)
{
    if (!input || !out)
    {
        return false;
    }
    double length = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (!isfinite(input[i]))
        {
            return false;
        }
        length += (double)input[i] * input[i];
    }
    if (length == 0)
    {
        return false;
    }
    length = sqrt(length);
    double normal[3] = {input[0] / length, input[1] / length, input[2] / length};
    double bestError = DBL_MAX;
    // The renderer decodes xyz as (byte - 127) * (scale + 192) / 32385.
    for (int scale = 0; scale < 256; ++scale)
    {
        const double decode = (scale + 192.0) / 32385.0;
        PackedUnitVec candidate;
        candidate.array[3] = (uint8_t)scale;
        double error = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            int value = (int)floor(normal[axis] / decode + 127.5);
            if (value < 0)
            {
                value = 0;
            }
            if (value > 255)
            {
                value = 255;
            }
            candidate.array[axis] = (uint8_t)value;
            const double difference = (value - 127) * decode - normal[axis];
            error += difference * difference;
        }
        if (error < bestError)
        {
            bestError = error;
            *out = candidate;
        }
    }
    return true;
}

static uint16_t PackModelTexCoord(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(uint32_t));
    int32_t magnitude = (int32_t)((2 * bits) ^ 0x80000000) >> 14;
    if (magnitude < -16384)
    {
        magnitude = -16384;
    }
    if (magnitude > 16383)
    {
        magnitude = 16383;
    }
    return (uint16_t)((magnitude & 16383) | ((bits >> 16) & 0xC000));
}

bool Linker_PackModelVertices(const LinkerModelSurfaceSource *source, GfxPackedVertex **vertices, char *error,
                              size_t errorSize)
{
    if (!vertices || (!error && errorSize))
    {
        return false;
    }
    *vertices = NULL;
    bool valid = source && source->vertexCount && source->vertices;
    GfxPackedVertex *result = NULL;
    double sums[2] = {};
    bool unitRange[2] = {true, true};
    if (valid)
    {
        for (int i = 0; i < source->vertexCount; ++i)
        {
            for (int axis = 0; axis < 2; ++axis)
            {
                const float coord = source->vertices[i].texCoord[axis];
                valid = valid && isfinite(coord);
                sums[axis] += coord;
                unitRange[axis] = unitRange[axis] && coord >= 0 && coord <= 1;
            }
        }
    }
    float center[2] = {};
    if (valid)
    {
        for (int axis = 0; axis < 2; ++axis)
        {
            center[axis] = unitRange[axis] ? 0.0f : (float)floor(sums[axis] / source->vertexCount + 0.5);
        }
        result = (GfxPackedVertex *)calloc(source->vertexCount, sizeof(GfxPackedVertex));
        valid = result != NULL;
    }
    if (valid)
    {
        for (int i = 0; valid && i < source->vertexCount; ++i)
        {
            const LinkerModelVertex *vertex = &source->vertices[i];
            GfxPackedVertex *out = &result[i];
            valid = Linker_PackUnitVector(vertex->normal, &out->normal) &&
                    Linker_PackUnitVector(vertex->tangent, &out->tangent);
            double cross[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                const int a = (axis + 1) % 3, b = (axis + 2) % 3;
                cross[axis] =
                    (double)vertex->normal[a] * vertex->tangent[b] - (double)vertex->normal[b] * vertex->tangent[a];
                valid = valid && isfinite(vertex->position[axis]) && isfinite(vertex->binormal[axis]);
                out->xyz[axis] = vertex->position[axis];
            }
            const double handedness =
                cross[0] * vertex->binormal[0] + cross[1] * vertex->binormal[1] + cross[2] * vertex->binormal[2];
            out->binormalSign = handedness < 0 ? -1.0f : 1.0f;
            memcpy(out->color.array, vertex->color, sizeof(uint8_t[4]));
            const float u = vertex->texCoord[0] - center[0];
            const float v = vertex->texCoord[1] - center[1];
            valid = valid && isfinite(u) && isfinite(v);
            out->texCoord.packed = ((uint32_t)PackModelTexCoord(u) << 16) | PackModelTexCoord(v);
        }
    }
    if (!valid)
    {
        free(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unallocatable model vertex data");
        }
        return false;
    }
    *vertices = result;
    return true;
}

void Linker_FreeModelSurfaces(XSurface *surfaces, unsigned int count)
{
    if (!surfaces)
    {
        return;
    }
    for (unsigned int i = 0; i < count; ++i)
    {
        free(surfaces[i].verts0);
        free(surfaces[i].triIndices);
        free(surfaces[i].vertInfo.vertsBlend);
        for (unsigned int j = 0; j < surfaces[i].vertListCount; ++j)
        {
            XSurfaceCollisionTree *tree = surfaces[i].vertList ? surfaces[i].vertList[j].collisionTree : NULL;
            if (tree)
            {
                free(tree->nodes);
                free(tree->leafs);
                free(tree);
            }
        }
        free(surfaces[i].vertList);
    }
    free(surfaces);
}

static void ModelTreeBounds(const XSurface *surface, unsigned int first, unsigned int count, float *mins, float *maxs)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        mins[axis] = FLT_MAX;
        maxs[axis] = -FLT_MAX;
    }
    for (unsigned int i = first * 3; i < (first + count) * 3; ++i)
    {
        const float *point = surface->verts0[surface->triIndices[i]].xyz;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (point[axis] < mins[axis])
            {
                mins[axis] = point[axis];
            }
            if (point[axis] > maxs[axis])
            {
                maxs[axis] = point[axis];
            }
        }
    }
}

static void ModelTreeNode(const XSurface *surface, XSurfaceCollisionTree *tree, unsigned int nodeIndex,
                          unsigned int triangleBase, unsigned int first, unsigned int count)
{
    XSurfaceCollisionNode *node = &tree->nodes[nodeIndex];
    float mins[3], maxs[3];
    ModelTreeBounds(surface, triangleBase + first, count, mins, maxs);
    for (int axis = 0; axis < 3; ++axis)
    {
        // Match the runtime float transform and round outward for conservative bounds.
        const double low = floor((float)((mins[axis] + tree->trans[axis]) * tree->scale[axis])) - 1;
        const double high = ceil((float)((maxs[axis] + tree->trans[axis]) * tree->scale[axis])) + 1;
        node->aabb.mins[axis] = (uint16_t)(low < 0 ? 0 : low > 65535 ? 65535 : low);
        node->aabb.maxs[axis] = (uint16_t)(high < 0 ? 0 : high > 65535 ? 65535 : high);
    }
    if (count <= 16)
    {
        node->childBeginIndex = (uint16_t)first;
        node->childCount = (uint16_t)(32768 | count);
        return;
    }
    const unsigned int children = tree->nodeCount;
    tree->nodeCount += 2;
    node->childBeginIndex = (uint16_t)children;
    node->childCount = 2;
    const unsigned int half = count / 2;
    ModelTreeNode(surface, tree, children, triangleBase, first, half);
    ModelTreeNode(surface, tree, children + 1, triangleBase, first + half, count - half);
}

static bool BuildModelCollisionTree(const XSurface *surface, XRigidVertList *list)
{
    if (!list->triCount)
    {
        return true;
    }
    // The high bit in a collision leaf encodes a two-triangle leaf.
    if ((unsigned int)list->triOffset + list->triCount > 32768)
    {
        return false;
    }
    XSurfaceCollisionTree *tree = (XSurfaceCollisionTree *)calloc(1, sizeof(XSurfaceCollisionTree));
    if (!tree)
    {
        return false;
    }
    list->collisionTree = tree;
    tree->leafCount = list->triCount;
    tree->leafs = (XSurfaceCollisionLeaf *)calloc(tree->leafCount, sizeof(XSurfaceCollisionLeaf));
    tree->nodes = (XSurfaceCollisionNode *)calloc(2 * tree->leafCount, sizeof(XSurfaceCollisionNode));
    if (!tree->leafs || !tree->nodes)
    {
        return false;
    }
    float mins[3], maxs[3];
    ModelTreeBounds(surface, list->triOffset, list->triCount, mins, maxs);
    for (int axis = 0; axis < 3; ++axis)
    {
        const double extent = (double)maxs[axis] - mins[axis];
        tree->trans[axis] = -mins[axis];
        tree->scale[axis] = (float)(65534.0 / (extent < 1.0 ? 1.0 : extent));
        // Runtime performs its subtraction in float, so reject an overflowing transform.
        if (!isfinite((maxs[axis] + tree->trans[axis]) * tree->scale[axis]))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < tree->leafCount; ++i)
    {
        tree->leafs[i].triangleBeginIndex = (uint16_t)(list->triOffset + i);
    }
    tree->nodeCount = 1;
    ModelTreeNode(surface, tree, 0, list->triOffset, 0, list->triCount);
    return true;
}

static bool BuildModelSurface(const LinkerModelSurfaceSource *source, unsigned int boneCount, XSurface *surface,
                              char *error, size_t errorSize)
{
    if (!source->triangleCount || source->triangleCount == UINT16_MAX || !source->indices ||
        !Linker_PackModelVertices(source, &surface->verts0, error, errorSize) ||
        !Linker_BuildModelBlendInfo(source, boneCount, &surface->vertInfo, error, errorSize))
    {
        return false;
    }
    surface->tileMode = source->tileMode;
    surface->deformed = source->deformed;
    surface->vertCount = source->vertexCount;
    surface->triCount = (uint16_t)((source->triangleCount + 1) & ~1);
    surface->triIndices = (uint16_t *)malloc(surface->triCount * 3 * sizeof(uint16_t));
    if (!surface->triIndices)
    {
        return false;
    }
    memcpy(surface->triIndices, source->indices, source->triangleCount * 3 * sizeof(uint16_t));
    for (int i = 0; i < source->triangleCount * 3; ++i)
    {
        if (source->indices[i] >= source->vertexCount)
        {
            return false;
        }
    }
    if (surface->triCount != source->triangleCount)
    {
        // The SIMD triangle paths require even counts. Append a degenerate triangle.
        const uint16_t last = source->indices[source->triangleCount * 3 - 1];
        for (int i = 0; i < 3; ++i)
        {
            surface->triIndices[source->triangleCount * 3 + i] = last;
        }
    }
    for (int i = 0; i < source->vertexCount; ++i)
    {
        const LinkerModelVertex *vertex = &source->vertices[i];
        if (vertex->extraWeights > 3)
        {
            return false;
        }
        for (int j = 0; j <= vertex->extraWeights; ++j)
        {
            const unsigned int bone = vertex->bones[j];
            if (bone >= boneCount)
            {
                return false;
            }
            surface->partBits[bone >> 5] |= (int)(UINT32_C(0x80000000) >> (bone & 31));
        }
    }
    if (!source->deformed)
    {
        if (!source->rigidGroupCount || source->rigidGroupCount > 128)
        {
            return false;
        }
        surface->vertListCount = source->rigidGroupCount;
        surface->vertList = (XRigidVertList *)calloc(surface->vertListCount, sizeof(XRigidVertList));
        if (!surface->vertList)
        {
            return false;
        }
        unsigned int vertexBegin = 0, triangle = 0;
        for (unsigned int i = 0; i < surface->vertListCount; ++i)
        {
            const LinkerModelRigidGroup *group = &source->rigidGroups[i];
            XRigidVertList *list = &surface->vertList[i];
            const unsigned int vertexEnd = vertexBegin + group->vertexCount;
            if (!group->vertexCount || vertexEnd > source->vertexCount || group->bone >= boneCount)
            {
                return false;
            }
            list->boneOffset = (uint16_t)(group->bone * sizeof(DObjSkelMat));
            list->vertCount = group->vertexCount;
            list->triOffset = (uint16_t)triangle;
            for (unsigned int vertex = vertexBegin; vertex < vertexEnd; ++vertex)
            {
                if (source->vertices[vertex].bones[0] != group->bone || source->vertices[vertex].extraWeights)
                {
                    return false;
                }
            }
            while (triangle < surface->triCount && surface->triIndices[triangle * 3] < vertexEnd)
            {
                for (int corner = 0; corner < 3; ++corner)
                {
                    const unsigned int vertex = surface->triIndices[triangle * 3 + corner];
                    if (vertex < vertexBegin || vertex >= vertexEnd)
                    {
                        return false;
                    }
                }
                ++triangle;
            }
            list->triCount = (uint16_t)(triangle - list->triOffset);
            vertexBegin = vertexEnd;
        }
        if (vertexBegin != source->vertexCount || triangle != surface->triCount)
        {
            return false;
        }
        for (unsigned int i = 0; i < surface->vertListCount; ++i)
        {
            if (!BuildModelCollisionTree(surface, &surface->vertList[i]))
            {
                return false;
            }
        }
    }
    return true;
}

bool Linker_BuildModelSurfaces(const LinkerModelMesh *mesh, unsigned int boneCount, XSurface **surfaces, char *error,
                               size_t errorSize)
{
    if (!surfaces || (!error && errorSize))
    {
        return false;
    }
    *surfaces = NULL;
    bool valid =
        mesh && mesh->surfaceCount && mesh->surfaceCount <= 255 && mesh->surfaces && boneCount && boneCount <= 128;
    XSurface *result = valid ? (XSurface *)calloc(mesh->surfaceCount, sizeof(XSurface)) : NULL;
    valid = valid && result;
    unsigned int baseVertex = 0, baseTriangle = 0;
    if (valid)
    {
        for (int i = 0; valid && i < mesh->surfaceCount; ++i)
        {
            valid = baseVertex <= UINT16_MAX && baseTriangle <= UINT16_MAX &&
                    BuildModelSurface(&mesh->surfaces[i], boneCount, &result[i], error, errorSize);
            result[i].baseVertIndex = (uint16_t)baseVertex;
            result[i].baseTriIndex = (uint16_t)baseTriangle;
            baseVertex += result[i].vertCount;
            baseTriangle += result[i].triCount;
        }
    }
    if (!valid)
    {
        Linker_FreeModelSurfaces(result, mesh ? mesh->surfaceCount : 0);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unallocatable native model surfaces");
        }
        return false;
    }
    *surfaces = result;
    return true;
}

void Linker_FreeModelMetadata(LinkerModelMetadata *metadata)
{
    if (metadata)
    {
        free(metadata->boneInfo);
        free(metadata);
    }
}

static bool ReadModelMetadata(ModelCursor *cursor, const LinkerModelConfig *config, unsigned int boneCount,
                              LinkerModelMetadata *metadata)
{
    bool ended = false;
    unsigned int lodCount = 0;
    for (int lod = 0; lod < 4; ++lod)
    {
        if (!config->lods[lod].filename[0])
        {
            ended = true;
            continue;
        }
        uint16_t count;
        if (ended || !Read(cursor, &count, sizeof(uint16_t)) || !count || count > 255 - metadata->surfaceCount)
        {
            return false;
        }
        ++lodCount;
        metadata->lodSurfaceCounts[lod] = count;
        for (int i = 0; i < count; ++i)
        {
            char name[256];
            if (!ReadString(cursor, name, sizeof(name)) || !name[0])
            {
                return false;
            }
            const char *material = !strcmp(name, "$default") ? "$default3d" : name;
            char *out = metadata->materials[metadata->surfaceCount++];
            const int length = snprintf(out, sizeof(metadata->materials[0]), "mc/%s", material);
            if (length < 0 || length >= sizeof(metadata->materials[0]))
            {
                return false;
            }
        }
    }
    if (!lodCount || config->collisionLod < -1 || config->collisionLod >= (int)lodCount ||
        cursor->size - cursor->position != boneCount * sizeof(float[6]))
    {
        return false;
    }
    metadata->boneCount = (uint16_t)boneCount;
    metadata->boneInfo = (XBoneInfo *)calloc(boneCount, sizeof(XBoneInfo));
    if (!metadata->boneInfo)
    {
        return false;
    }
    for (unsigned int i = 0; i < boneCount; ++i)
    {
        XBoneInfo *bone = &metadata->boneInfo[i];
        if (!ReadFiniteFloats(cursor, bone->bounds[0], 3) || !ReadFiniteFloats(cursor, bone->bounds[1], 3))
        {
            return false;
        }
        double radiusSquared = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (bone->bounds[0][axis] > bone->bounds[1][axis])
            {
                return false;
            }
            bone->offset[axis] = (float)(((double)bone->bounds[0][axis] + bone->bounds[1][axis]) * 0.5);
            const double half = (double)bone->bounds[1][axis] - bone->offset[axis];
            radiusSquared += half * half;
        }
        if (radiusSquared > FLT_MAX)
        {
            return false;
        }
        bone->radiusSquared = (float)radiusSquared;
    }
    return true;
}

bool Linker_ReadModelMetadata(const void *data, size_t size, const LinkerModelConfig *config, unsigned int boneCount,
                              LinkerModelMetadata **metadata, char *error, size_t errorSize)
{
    if (!metadata || (!error && errorSize))
    {
        return false;
    }
    *metadata = NULL;
    LinkerModelMetadata *result = NULL;
    bool valid = data && config && size <= INT_MAX && boneCount && boneCount <= 128;
    if (valid)
    {
        result = (LinkerModelMetadata *)calloc(1, sizeof(LinkerModelMetadata));
        ModelCursor cursor = {(const uint8_t *)data, size, 0};
        valid = result && ReadModelMetadata(&cursor, config, boneCount, result);
    }
    if (!valid)
    {
        Linker_FreeModelMetadata(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid or unallocatable model materials and bone bounds");
        }
        return false;
    }
    *metadata = result;
    return true;
}

void Linker_FreeModelSource(LinkerModelSource *source)
{
    if (source)
    {
        free(source->skeleton);
        Linker_FreeModelCollision(source->collision);
        Linker_FreeModelMetadata(source->metadata);
        free(source);
    }
}

bool Linker_ReadModelSource(const void *modelData, size_t modelSize, const void *skeletonData, size_t skeletonSize,
                            LinkerModelSource **source, char *error, size_t errorSize)
{
    if (!source || (!error && errorSize))
    {
        return false;
    }
    *source = NULL;
    LinkerModelSource *result = (LinkerModelSource *)calloc(1, sizeof(LinkerModelSource));
    size_t configSize = 0, collisionSize = 0;
    bool valid = result &&
                 Linker_ReadModelConfig(modelData, modelSize, &result->config, &configSize, error, errorSize) &&
                 Linker_ReadModelSkeleton(skeletonData, skeletonSize, &result->skeleton, error, errorSize);
    if (valid)
    {
        const uint8_t *remaining = (const uint8_t *)modelData + configSize;
        valid = Linker_ReadModelCollision(remaining, modelSize - configSize, &result->collision, &collisionSize, error,
                                          errorSize);
        if (valid)
        {
            valid = Linker_ReadModelMetadata(remaining + collisionSize, modelSize - configSize - collisionSize,
                                             &result->config, result->skeleton->boneCount, &result->metadata, error,
                                             errorSize);
        }
    }
    if (valid)
    {
        for (int i = 0; i < result->collision->count; ++i)
        {
            if (result->collision->surfaces[i].boneIdx >= result->skeleton->boneCount)
            {
                if (errorSize)
                {
                    snprintf(error, errorSize, "Model collision references a bone outside its skeleton");
                }
                valid = false;
                break;
            }
        }
    }
    if (!valid)
    {
        Linker_FreeModelSource(result);
        return false;
    }
    *source = result;
    return true;
}
