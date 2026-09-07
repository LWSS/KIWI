/*
 * compile.c — Radiosity compilation: lighting transfer, bounce, gather.
 *
 * All functions verified against cod2rad64 LST line by line.
 * Source: compile.cpp (from LST assert strings: .\compile.cpp)
 */

#include "cod2rad64.h"
#include "zlib/zlib.h"
#include <stdlib.h>
#include <math.h>

/*
 * Tetrahedral SH basis constants for 4-band directional lighting.
 * These encode the 4 basis directions of a regular tetrahedron.
 */
static const float k_sqrt2_3  = 0.8164965510f;  /* sqrt(2/3) — dword_457610 */
static const float k_inv_sqrt3 = 0.5773502588f;  /* 1/sqrt(3) — dword_457614 */
static const float k_inv_sqrt2 = 0.7071067691f;  /* 1/sqrt(2) — dword_457624 */
static const float k_inv_sqrt6 = 0.4082482755f;  /* 1/sqrt(6) — dword_457634 */

/* Named constants for raw address reads */
static const float k_skyRayLength = 262144.0f;    /* dword_4577D8 */
static const float k_smallEpsilon = 0.001f;        /* dword_457AF0 */
static const float k_randScale = 0.000061037019f;  /* dword_457BB4 — approx 2.0/32767.0 */
static const float k_largeRadius = 0.999f;         /* dword_457854 = 0x3F7FBE77 = 0.999 as float */
static const float k_initialMaxEnergy = 3.402823e+38f; /* dword_456490 — FLT_MAX */
static const float k_divergeThreshold = 2.0f;      /* dword_457630 = 40000000h (was wrongly 1.1) */

/*
================
GatherSurfaceIncidentEnergyForLightFromDir

Computes 4 tetrahedral SH basis dot products against a direction
vector. For each basis where the dot product is positive, accumulates
(dot * lightColor) into the corresponding output band (3 floats each).

Parameters:
  lightColor — float[3] RGB light color (rcx)
  direction  — float[3] unit direction vector (rdx)
  output     — float[12] accumulator, 4 bands × 3 channels (r8)

Band dot products:
  band0 = dir[2]
  band1 = dir[1]*sqrt(2/3) + dir[2]/sqrt(3)
  band2 = dir[2]/sqrt(3) - dir[0]/sqrt(2) - dir[1]/sqrt(6)
  band3 = dir[0]/sqrt(2) - dir[1]/sqrt(6) + dir[2]/sqrt(3)
================
*/
void GatherSurfaceIncidentEnergyForLightFromDir(float *lightColor, float *direction,
                                                 float *output)
{
    float dot;
    /* band 0: dot = dir[2] */
    dot = direction[2];
    if (dot > 0.0f)
    {
        output[0] += dot * lightColor[0];
        output[1] += dot * lightColor[1];
        output[2] += dot * lightColor[2];
    }

    /* band 1: dot = dir[1]*sqrt(2/3) + dir[0]*0 + dir[2]/sqrt(3) */
    dot = direction[1] * k_sqrt2_3 + direction[2] * k_inv_sqrt3;
    if (dot > 0.0f)
    {
        output[3] += dot * lightColor[0];
        output[4] += dot * lightColor[1];
        output[5] += dot * lightColor[2];
    }

    /* band 2: dot = dir[2]/sqrt(3) - dir[0]/sqrt(2) - dir[1]/sqrt(6) */
    dot = direction[2] * k_inv_sqrt3 - (direction[1] * k_inv_sqrt6 + direction[0] * k_inv_sqrt2);
    if (dot > 0.0f)
    {
        output[6] += dot * lightColor[0];
        output[7] += dot * lightColor[1];
        output[8] += dot * lightColor[2];
    }

    /* band 3: dot = dir[0]/sqrt(2) - dir[1]/sqrt(6) + dir[2]/sqrt(3) */
    dot = direction[0] * k_inv_sqrt2 - direction[1] * k_inv_sqrt6 + direction[2] * k_inv_sqrt3;
    if (dot > 0.0f)
    {
        output[9]  += dot * lightColor[0];
        output[10] += dot * lightColor[1];
        output[11] += dot * lightColor[2];
    }
}

/* memset is from CRT (sub_43C400) */

/* global transfer block pool */
static void *g_transferPool;     /* qword_480A68 */
static int g_transferPoolIdx;    /* dword_480A70 */

typedef struct RelightSuppressedRange_s {
    uint64_t key;
    uint32_t count;
    uint32_t reserved;
} RelightSuppressedRange_t;

typedef struct RelightTransferRef_s {
    uint32_t packedSource;
    float weight;
} RelightTransferRef_t;

static RelightSuppressedRange_t *g_relightSuppressedRanges;
static uint32_t g_relightSuppressedCount;
static uint32_t g_relightSuppressedCapacity;
static int g_relightCacheLoaded;
static float *g_relightSkyStorage;

/* per-thread max bounced energy — originally dword_480A48..480A58, now dynamically sized */
static float *g_bounceEnergy;
static int g_bounceEnergyCount;
static float *g_bounceStage;
static int g_bounceStageCount;

#define TRANSFER_HT_SIZE 4096
#define TRANSFER_HT_MASK (TRANSFER_HT_SIZE - 1)
typedef struct {
    void *fromSample;
    void *toSample;
    int lightIdx;
    TransferEntry_t *entry;
} TransferHTEntry_t;
static __declspec(thread) TransferHTEntry_t g_transferHT[TRANSFER_HT_SIZE];

static char s_assertDisable_Alloc_fromSample;  /* byte_480A7B */
static char s_assertDisable_Alloc_vars;        /* byte_480A7A */
static char s_assertDisable_Alloc_weight;      /* byte_480A79 */
static char s_assertDisable_Alloc_block;       /* byte_480A78 */

static const float k_weightEpsilon = -1.0e-5f; /* dword_4576A0 */

/*
================
AllocLightingTransfer

Looks up or creates a lighting transfer entry from fromSample to
toSample for a given light index. Uses a hash chain hanging off
fromSample->vars[+0x58]. Each hash block holds 15 entries.

If the (toSample, lightIdx) pair already exists, adds weight to it.
Otherwise allocates a new entry via a pool allocator, using binary
search to find the first unused slot in the head block.
================
*/
static unsigned int TransferHash(void *fromSample, void *toSample, int lightIdx)
{
    return ((unsigned int)(uintptr_t)fromSample * 2654435761u
          ^ (unsigned int)(uintptr_t)toSample * 2246822519u
          ^ (unsigned int)lightIdx * 3266489917u) & TRANSFER_HT_MASK;
}

void AllocLightingTransfer(Sample_t *fromSample, void *toSample, int lightIdx,
                           float weight)
{
    SampleVars_t *vars;
    TransferBlock_t *block;
    TransferEntry_t *entry;
    int lo, hi, mid;
    unsigned int htIdx;
    TransferHTEntry_t *slot;

    Assert(fromSample != 0, s_assertDisable_Alloc_fromSample);

    vars = fromSample->vars;
    Assert(vars != 0, s_assertDisable_Alloc_vars);

    Assert(weight >= k_weightEpsilon, s_assertDisable_Alloc_weight);

    if (weight <= 0.0f)
        return;

    /* O(1) hash table lookup — handles both existing and previously-inserted entries */
    htIdx = TransferHash(fromSample, toSample, lightIdx);
    slot = &g_transferHT[htIdx];

    if (slot->fromSample == fromSample && slot->toSample == toSample
        && slot->lightIdx == lightIdx && slot->entry)
    {
        AcquireThreadLock((unsigned int)(uintptr_t)fromSample);
        slot->entry->weight += weight;
        ReleaseThreadLock((unsigned int)(uintptr_t)fromSample);
        return;
    }

    AcquireThreadLock((unsigned int)(uintptr_t)fromSample);

    /* chain walk — only on full hash miss */
    block = vars->transferHead;
    if (block)
    {
        TransferBlock_t *cur = block;
        while (cur)
        {
            int i;
            for (i = 0; i < TRANSFERS_PER_BLOCK; i++)
            {
                entry = &cur->entries[i];
                if (entry->toSample == 0)
                    break; /* A partial head block may precede older full blocks. */
                if (entry->toSample == toSample && entry->lightIdx == lightIdx)
                {
                    weight += entry->weight;
                    slot->fromSample = fromSample;
                    slot->toSample = toSample;
                    slot->lightIdx = lightIdx;
                    slot->entry = entry;
                    goto done;
                }
            }
            cur = (TransferBlock_t *)cur->next;
        }
    }

    if (block)
    {
        if (block->entries[TRANSFERS_PER_BLOCK - 1].toSample == 0)
        {
            lo = 0;
            hi = TRANSFERS_PER_BLOCK - 1;
            while (lo < hi)
            {
                mid = (lo + hi) >> 1;
                if (block->entries[mid].toSample == 0)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            if (lo >= 0)
                goto insert;
        }
    }

    {
        AcquireThreadLock((unsigned int)(uintptr_t)&g_transferPool);

        if (g_transferPoolIdx == 0)
        {
            void *poolMem = malloc(TRANSFER_POOL_SIZE);
            if (!poolMem)
                ErrorMsg("Out of memory allocating light transport block\n");

            memset(poolMem, 0, TRANSFER_POOL_SIZE);

            *(void **)((unsigned char *)poolMem + TRANSFER_POOL_LINK_OFFSET) = g_transferPool;
            g_transferPool = poolMem;
        }

        {
            int idx = g_transferPoolIdx;
            TransferBlock_t *blk;

            g_transferPoolIdx = idx + 1;
            blk = (TransferBlock_t *)((unsigned char *)g_transferPool + idx * TRANSFER_BLOCK_SIZE);

            if (g_transferPoolIdx == TRANSFER_POOL_COUNT)
                g_transferPoolIdx = 0;

            ReleaseThreadLock((unsigned int)(uintptr_t)&g_transferPool);

            Assert(blk != 0, s_assertDisable_Alloc_block);

            blk->next = vars->transferHead;
            vars->transferHead = blk;

            block = blk;
        }

        lo = 0;
    }

insert:
    entry = &vars->transferHead->entries[lo];
    entry->toSample = toSample;
    entry->lightIdx = lightIdx;

    slot->fromSample = fromSample;
    slot->toSample = toSample;
    slot->lightIdx = lightIdx;
    slot->entry = entry;

done:
    entry->weight = weight;
    ReleaseThreadLock((unsigned int)(uintptr_t)fromSample);
}

/* compile.cpp 0x406F00: append one directional direct-light contribution.
 * Retail uses a 16-bit count and a custom growable allocation.  The x64 port
 * retains the exact 24-byte payload and widens only the allocation metadata. */
static void AppendDirectTransport(Sample_t *sample, const float color[3],
                                  const float direction[3])
{
    SampleVars_t *vars;
    DirectTransport_t *record;

    if (!sample || !(vars = sample->vars))
        return;

    AcquireThreadLock((unsigned int)(uintptr_t)sample);
    if (vars->directTransportCount == vars->directTransportCapacity)
    {
        unsigned int newCapacity = vars->directTransportCapacity
                                 ? vars->directTransportCapacity * 2u : 4u;
        DirectTransport_t *newRecords = (DirectTransport_t *)realloc(
            vars->directTransports,
            (size_t)newCapacity * sizeof(*newRecords));
        if (!newRecords)
            Error("Out of memory allocating direct lighting influences\n");
        vars->directTransports = newRecords;
        vars->directTransportCapacity = newCapacity;
    }

    record = &vars->directTransports[vars->directTransportCount++];
    record->color[0] = color[0];
    record->color[1] = color[1];
    record->color[2] = color[2];
    record->direction[0] = direction[0];
    record->direction[1] = direction[1];
    record->direction[2] = direction[2];
    ReleaseThreadLock((unsigned int)(uintptr_t)sample);
}

#define RADTRANS_VERSION 3u
#define RADTRANS_FILENAME "radtrans.bin"

static uint64_t Relight_MakeSuppressedKey(int triangleIndex, int areaIndex)
{
    return ((uint64_t)(uint32_t)triangleIndex << 32)
         | (uint64_t)(uint32_t)areaIndex;
}

static int Relight_CompareSuppressedRanges(const void *left, const void *right)
{
    const RelightSuppressedRange_t *a =
        (const RelightSuppressedRange_t *)left;
    const RelightSuppressedRange_t *b =
        (const RelightSuppressedRange_t *)right;
    if (a->key < b->key)
        return -1;
    if (a->key > b->key)
        return 1;
    return 0;
}

static void Relight_ResetSuppressedRanges(void)
{
    free(g_relightSuppressedRanges);
    g_relightSuppressedRanges = NULL;
    g_relightSuppressedCount = 0;
    g_relightSuppressedCapacity = 0;
}

int Relight_IsSuppressed(int triangleIndex, int areaIndex)
{
    uint64_t key;
    uint32_t lo;
    uint32_t hi;

    if (!g_relightCacheLoaded || !g_relightSuppressedCount)
        return 0;

    key = Relight_MakeSuppressedKey(triangleIndex, areaIndex);
    lo = 0;
    hi = g_relightSuppressedCount;
    while (lo < hi)
    {
        uint32_t mid = lo + ((hi - lo) >> 1);
        const RelightSuppressedRange_t *range =
            &g_relightSuppressedRanges[mid];
        if (key < range->key)
            hi = mid;
        else if (key >= range->key + range->count)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

void Relight_RecordSuppressed(int triangleIndex, int areaIndex)
{
    uint64_t key;
    RelightSuppressedRange_t *range;

    if (g_relightCacheLoaded)
        return;

    key = Relight_MakeSuppressedKey(triangleIndex, areaIndex);
    AcquireThreadLock((unsigned int)(uintptr_t)&g_relightSuppressedRanges);

    if (g_relightSuppressedCount)
    {
        range = &g_relightSuppressedRanges[g_relightSuppressedCount - 1];
        if (range->key + range->count == key)
        {
            range->count++;
            ReleaseThreadLock((unsigned int)(uintptr_t)&g_relightSuppressedRanges);
            return;
        }
    }

    if (g_relightSuppressedCount == g_relightSuppressedCapacity)
    {
        uint32_t newCapacity = g_relightSuppressedCapacity
                             ? g_relightSuppressedCapacity + 1024u : 1024u;
        RelightSuppressedRange_t *newRanges =
            (RelightSuppressedRange_t *)realloc(g_relightSuppressedRanges,
                (size_t)newCapacity * sizeof(*newRanges));
        if (!newRanges)
            Error("Out of memory for %i suppressed lighting samples\n",
                  (int)g_relightSuppressedCount);
        g_relightSuppressedRanges = newRanges;
        g_relightSuppressedCapacity = newCapacity;
    }

    range = &g_relightSuppressedRanges[g_relightSuppressedCount++];
    range->key = key;
    range->count = 1;
    range->reserved = 0;
    ReleaseThreadLock((unsigned int)(uintptr_t)&g_relightSuppressedRanges);
}

static uint32_t RadTrans_DrawVertCrc(void)
{
    size_t byteCount = (size_t)numBSPDrawVerts * 68u;
    if (byteCount > 0xFFFFFFFFu)
        return 0;
    return (uint32_t)crc32(0L, (const Bytef *)bspDrawVerts,
                           (uInt)byteCount);
}

static int RadTrans_Read(FILE *file, void *data, size_t size)
{
    return size == 0 || fread(data, 1, size, file) == size;
}

static int RadTrans_Write(FILE *file, const void *data, size_t size)
{
    return size == 0 || fwrite(data, 1, size, file) == size;
}

static Sample_t **RadTrans_BuildUsefulSampleList(void)
{
    Sample_t **samples;
    LightmapSample_t *allSamples = (LightmapSample_t *)g_lightingSamples;
    int sampleIndex;
    int usefulIndex = 0;

    samples = (Sample_t **)malloc((size_t)g_usefulSampleCount
                                * sizeof(*samples));
    if (!samples)
        Error("Out of memory loading relight file\n");

    for (sampleIndex = 0; sampleIndex < g_totalSampleCount; sampleIndex++)
    {
        if (allSamples[sampleIndex].vars)
            samples[usefulIndex++] = (Sample_t *)&allSamples[sampleIndex];
    }

    if (usefulIndex != g_usefulSampleCount)
        Error("Relight sample table is inconsistent (%i != %i)\n",
              usefulIndex, g_usefulSampleCount);
    return samples;
}

static int RadTrans_ReadAndValidateHeader(FILE *file)
{
    uint32_t value;
    float floatValue;
    unsigned char byteValue;

    if (!RadTrans_Read(file, &value, sizeof(value))
     || value != RADTRANS_VERSION)
        return 0;
    if (!RadTrans_Read(file, &value, sizeof(value))
     || value != (uint32_t)g_totalSampleCount)
        return 0;
    if (!RadTrans_Read(file, &value, sizeof(value))
     || value != (uint32_t)g_usefulSampleCount)
        return 0;
    if (!RadTrans_Read(file, &value, sizeof(value))
     || value != (uint32_t)g_traces)
        return 0;
    if (!RadTrans_Read(file, &value, sizeof(value))
     || value != (uint32_t)g_superSample)
        return 0;
    if (!RadTrans_Read(file, &value, sizeof(value))
     || value != (uint32_t)g_traceFilter)
        return 0;
    if (!RadTrans_Read(file, &floatValue, sizeof(floatValue))
     || memcmp(&floatValue, &g_jitter, sizeof(floatValue)) != 0)
        return 0;
    if (!RadTrans_Read(file, &byteValue, sizeof(byteValue))
     || byteValue != (unsigned char)g_modelShadows)
        return 0;
    if (!RadTrans_Read(file, &value, sizeof(value))
     || value != (uint32_t)g_triCount)
        return 0;
    if (!RadTrans_Read(file, &value, sizeof(value))
     || value != RadTrans_DrawVertCrc())
        return 0;
    return 1;
}

static int RadTrans_ValidatePayload(FILE *file, __int64 fileSize,
                                    Sample_t **usefulSamples,
                                    __int64 *payloadOffset)
{
    uint32_t rangeCount;
    uint32_t sampleIndex;
    uint64_t skipBytes;

    *payloadOffset = _ftelli64(file);
    if (*payloadOffset < 0 || !RadTrans_Read(file, &rangeCount, sizeof(rangeCount)))
        return 0;

    skipBytes = (uint64_t)rangeCount * sizeof(RelightSuppressedRange_t);
    if (skipBytes > (uint64_t)fileSize
     || _ftelli64(file) > fileSize - (__int64)skipBytes
     || _fseeki64(file, (__int64)skipBytes, SEEK_CUR) != 0)
        return 0;

    skipBytes = (uint64_t)(uint32_t)g_usefulSampleCount
              * (uint64_t)(uint32_t)g_traces * sizeof(float);
    if (skipBytes > (uint64_t)fileSize
     || _ftelli64(file) > fileSize - (__int64)skipBytes
     || _fseeki64(file, (__int64)skipBytes, SEEK_CUR) != 0)
        return 0;

    for (sampleIndex = 0; sampleIndex < (uint32_t)g_usefulSampleCount;
         sampleIndex++)
    {
        uint32_t transferCount;
        uint32_t transferIndex;
        if (!RadTrans_Read(file, &transferCount, sizeof(transferCount)))
            return 0;
        if ((uint64_t)transferCount * sizeof(RelightTransferRef_t)
            > (uint64_t)(fileSize - _ftelli64(file)))
            return 0;
        for (transferIndex = 0; transferIndex < transferCount; transferIndex++)
        {
            RelightTransferRef_t ref;
            uint32_t sourceIndex;
            uint32_t directionIndex;
            if (!RadTrans_Read(file, &ref, sizeof(ref)))
                return 0;
            sourceIndex = ref.packedSource & 0x7FFFFFu;
            directionIndex = ref.packedSource >> 23;
            if (sourceIndex >= (uint32_t)g_totalSampleCount
             || directionIndex >= (uint32_t)g_traces
             || !((LightmapSample_t *)g_lightingSamples)[sourceIndex].vars
             || IS_NAN_FLOAT(ref.weight)
             || ref.weight < -1.0e-5f)
                return 0;
        }
        (void)usefulSamples;
    }
    return 1;
}

static int RadTrans_LoadV3(void)
{
    FILE *file;
    __int64 fileSize;
    __int64 payloadOffset;
    Sample_t **usefulSamples;
    uint32_t sampleIndex;
    uint32_t rangeCount;

    file = fopen(RADTRANS_FILENAME, "rb");
    if (!file)
        return 0;
    if (_fseeki64(file, 0, SEEK_END) != 0
     || (fileSize = _ftelli64(file)) < 0
     || _fseeki64(file, 0, SEEK_SET) != 0
     || !RadTrans_ReadAndValidateHeader(file))
    {
        fclose(file);
        return 0;
    }

    usefulSamples = RadTrans_BuildUsefulSampleList();
    if (!RadTrans_ValidatePayload(file, fileSize, usefulSamples, &payloadOffset))
    {
        free(usefulSamples);
        fclose(file);
        return 0;
    }

    if (_fseeki64(file, payloadOffset, SEEK_SET) != 0
     || !RadTrans_Read(file, &rangeCount, sizeof(rangeCount)))
        Error("Couldn't read relight file\n");

    Relight_ResetSuppressedRanges();
    if (rangeCount)
    {
        g_relightSuppressedRanges = (RelightSuppressedRange_t *)malloc(
            (size_t)rangeCount * sizeof(*g_relightSuppressedRanges));
        if (!g_relightSuppressedRanges)
            Error("Out of memory loading relight file\n");
        g_relightSuppressedCount = rangeCount;
        g_relightSuppressedCapacity = rangeCount;
        if (!RadTrans_Read(file, g_relightSuppressedRanges,
                           (size_t)rangeCount
                         * sizeof(*g_relightSuppressedRanges)))
            Error("Couldn't read relight file\n");
        qsort(g_relightSuppressedRanges, rangeCount,
              sizeof(*g_relightSuppressedRanges),
              Relight_CompareSuppressedRanges);
    }

    free(g_relightSkyStorage);
    g_relightSkyStorage = (float *)calloc(
        (size_t)g_usefulSampleCount * (size_t)g_traces, sizeof(float));
    if (!g_relightSkyStorage)
        Error("Out of memory loading relight file\n");
    for (sampleIndex = 0; sampleIndex < (uint32_t)g_usefulSampleCount;
         sampleIndex++)
    {
        usefulSamples[sampleIndex]->vars->skyInfluences =
            &g_relightSkyStorage[(size_t)sampleIndex * (size_t)g_traces];
    }
    if (!RadTrans_Read(file, g_relightSkyStorage,
                       (size_t)g_usefulSampleCount * (size_t)g_traces
                     * sizeof(float)))
        Error("Couldn't read relight file\n");

    for (sampleIndex = 0; sampleIndex < (uint32_t)g_usefulSampleCount;
         sampleIndex++)
    {
        TransferBlock_t *block;
        TransferBlock_t *previous;
        uint32_t transferCount;
        uint32_t transferIndex;
        if (!RadTrans_Read(file, &transferCount, sizeof(transferCount)))
            Error("Couldn't read relight file\n");
        for (transferIndex = 0; transferIndex < transferCount; transferIndex++)
        {
            RelightTransferRef_t ref;
            uint32_t sourceIndex;
            uint32_t directionIndex;
            if (!RadTrans_Read(file, &ref, sizeof(ref)))
                Error("Couldn't read relight file\n");
            sourceIndex = ref.packedSource & 0x7FFFFFu;
            directionIndex = ref.packedSource >> 23;
            AllocLightingTransfer(usefulSamples[sampleIndex],
                &((LightmapSample_t *)g_lightingSamples)[sourceIndex],
                (int)directionIndex, ref.weight);
        }

        /* Native keeps the radtrans references in their serialized flat-array
         * order.  AllocLightingTransfer grows the x64 compatibility chain by
         * prepending 15-entry blocks, so reverse only the block links after
         * loading to recover the native accumulation order. */
        previous = NULL;
        block = usefulSamples[sampleIndex]->vars->transferHead;
        while (block)
        {
            TransferBlock_t *next = block->next;
            block->next = previous;
            previous = block;
            block = next;
        }
        usefulSamples[sampleIndex]->vars->transferHead = previous;
    }

    fclose(file);
    free(usefulSamples);
    g_relightCacheLoaded = 1;
    Com_Printf("----------------------------------------\n"
               "Loading saved light transport for sky and radiosity...\n");
    return 1;
}

static uint32_t RadTrans_CountTransfers(const Sample_t *sample)
{
    const TransferBlock_t *block;
    uint32_t count = 0;
    for (block = sample->vars->transferHead; block; block = block->next)
    {
        int entryIndex;
        for (entryIndex = 0; entryIndex < TRANSFERS_PER_BLOCK; entryIndex++)
        {
            if (!block->entries[entryIndex].toSample)
                break;
            count++;
        }
    }
    return count;
}

static int RadTrans_SaveV3(void)
{
    FILE *file;
    Sample_t **usefulSamples;
    uint32_t value;
    uint32_t sampleIndex;
    unsigned char modelShadows;
    float zeroSky[512] = { 0 };
    int ok = 1;

    file = fopen(RADTRANS_FILENAME, "wb");
    if (!file)
        return 0;
    usefulSamples = RadTrans_BuildUsefulSampleList();

#define RADTRANS_WRITE_VALUE(v) \
    do { if (!RadTrans_Write(file, &(v), sizeof(v))) ok = 0; } while (0)
    value = RADTRANS_VERSION; RADTRANS_WRITE_VALUE(value);
    value = (uint32_t)g_totalSampleCount; RADTRANS_WRITE_VALUE(value);
    value = (uint32_t)g_usefulSampleCount; RADTRANS_WRITE_VALUE(value);
    value = (uint32_t)g_traces; RADTRANS_WRITE_VALUE(value);
    value = (uint32_t)g_superSample; RADTRANS_WRITE_VALUE(value);
    value = (uint32_t)g_traceFilter; RADTRANS_WRITE_VALUE(value);
    RADTRANS_WRITE_VALUE(g_jitter);
    modelShadows = (unsigned char)g_modelShadows;
    RADTRANS_WRITE_VALUE(modelShadows);
    value = (uint32_t)g_triCount; RADTRANS_WRITE_VALUE(value);
    value = RadTrans_DrawVertCrc(); RADTRANS_WRITE_VALUE(value);
    value = g_relightSuppressedCount; RADTRANS_WRITE_VALUE(value);
#undef RADTRANS_WRITE_VALUE

    if (g_relightSuppressedCount)
    {
        qsort(g_relightSuppressedRanges, g_relightSuppressedCount,
              sizeof(*g_relightSuppressedRanges),
              Relight_CompareSuppressedRanges);
        if (!RadTrans_Write(file, g_relightSuppressedRanges,
                            (size_t)g_relightSuppressedCount
                          * sizeof(*g_relightSuppressedRanges)))
            ok = 0;
    }

    for (sampleIndex = 0; sampleIndex < (uint32_t)g_usefulSampleCount;
         sampleIndex++)
    {
        const float *sky = usefulSamples[sampleIndex]->vars->skyInfluences;
        if (!RadTrans_Write(file, sky ? sky : zeroSky,
                            (size_t)g_traces * sizeof(float)))
            ok = 0;
    }

    for (sampleIndex = 0; sampleIndex < (uint32_t)g_usefulSampleCount;
         sampleIndex++)
    {
        Sample_t *sample = usefulSamples[sampleIndex];
        TransferBlock_t *block;
        uint32_t transferCount = RadTrans_CountTransfers(sample);
        if (!RadTrans_Write(file, &transferCount, sizeof(transferCount)))
            ok = 0;
        for (block = sample->vars->transferHead; block; block = block->next)
        {
            int entryIndex;
            for (entryIndex = 0; entryIndex < TRANSFERS_PER_BLOCK; entryIndex++)
            {
                TransferEntry_t *entry = &block->entries[entryIndex];
                RelightTransferRef_t ref;
                ptrdiff_t sourceIndex;
                if (!entry->toSample)
                    break;
                sourceIndex = (LightmapSample_t *)entry->toSample
                            - (LightmapSample_t *)g_lightingSamples;
                if (sourceIndex < 0 || sourceIndex >= g_totalSampleCount
                 || entry->lightIdx < 0 || entry->lightIdx >= g_traces)
                    Error("Invalid light transport reference while saving relight file\n");
                ref.packedSource = ((uint32_t)entry->lightIdx << 23)
                                 | ((uint32_t)sourceIndex & 0x7FFFFFu);
                ref.weight = entry->weight;
                if (!RadTrans_Write(file, &ref, sizeof(ref)))
                    ok = 0;
            }
        }
    }

    free(usefulSamples);
    if (fclose(file) != 0)
        ok = 0;
    return ok;
}

static char s_assertDisable_Normalize_sample;  /* byte_480A7D */
static char s_assertDisable_Normalize_vars;    /* byte_480A7C */

/*
================
NormalizeLightTransfers

Normalizes the sample's accumulated direct inputs and incoming transfer
coefficients by its covered area.
================
*/
void NormalizeLightTransfers(Sample_t *sample)
{
    SampleVars_t *vars;

    Assert(sample != 0, s_assertDisable_Normalize_sample);

    vars = sample->vars;
    Assert(vars != 0, s_assertDisable_Normalize_vars);

    /* Native 0x415E50 normalizes only samples accepted by the transport
     * classifier.  Rejected slots keep their vars allocation for neighbour
     * reconstruction but are not valid bounce sources. */
    if (!vars->validMask)
        return;

    if (sample->areaX2 > 0.0f)
    {
        float invArea = 1.0f / sample->areaX2;
        LightmapSample_t *lightmapSample = (LightmapSample_t *)sample;
        TransferBlock_t *block;
        int traceIndex;
        int channel;
        unsigned int directIndex;

        for (channel = 0; channel < 12; channel++)
            vars->incident[channel] *= invArea;
        vars->unscattered[0] *= invArea;
        vars->unscattered[1] *= invArea;
        vars->unscattered[2] *= invArea;
        /* Native lighting.cpp 0x415E50 normalizes the accumulated incident
         * RGB at vars+0x20 independently of the radiosity ping buffers. */
        vars->gatheredIncident[0] *= invArea;
        vars->gatheredIncident[1] *= invArea;
        vars->gatheredIncident[2] *= invArea;
        vars->coincident[0] *= invArea;
        vars->coincident[1] *= invArea;
        vars->coincident[2] *= invArea;

        for (directIndex = 0;
             directIndex < vars->directTransportCount;
             ++directIndex)
        {
            DirectTransport_t *direct = &vars->directTransports[directIndex];
            direct->color[0] *= invArea;
            direct->color[1] *= invArea;
            direct->color[2] *= invArea;
        }

        for (channel = 0; channel < 4; channel++)
        {
            if (lightmapSample->channelWeight[channel] > 0.0f)
                vars->intensity[channel] /= lightmapSample->channelWeight[channel];
            else
                vars->intensity[channel] = 0.0f;
        }

        if (vars->skyInfluences)
        {
            for (traceIndex = 0; traceIndex < g_traces; traceIndex++)
                vars->skyInfluences[traceIndex] *= invArea;
        }

        for (block = vars->transferHead; block; block = block->next)
        {
            int i;
            for (i = 0; i < TRANSFERS_PER_BLOCK && block->entries[i].toSample; ++i)
                block->entries[i].weight *= invArea;
        }
    }
    else
    {
        vars->validMask = 0;
    }
}

/* TraceSetup_and_Dispatch declared in cod2rad64.h — sub_40CDB0 */
extern int TraceStaticModels(void *startPos, void *endPos);
/* sqrtf is CRT (sub_43B200) */
/* g_vertexData — view into g_vertData[0].lmCoord (stride sizeof(DrawVert_t)=0x44).
 * Binary uses raw byte indexing with stride 0x44 from unk_1270FCC4. */
unsigned char *g_vertexData = (unsigned char *)&g_vertData[0].lmCoord;

/*
================
FindLightingSamplesAndNormal

Traces a ray along the normal from a surface point, finds the
triangle hit, interpolates barycentric UV coordinates from vertex
lighting data, and samples the lightmap. Returns 1 on success,
0 if no hit or invalid triangle, -1 on backface.

Parameters:
  rcx = sampleIdx (32-bit int)
  rdx = position[3]
  r8  = normal[3]
  xmm3 = offset
  [stack+0x20] = outputLighting ptr
  [stack+0x28] = outputNormal ptr (float[3], optional)
================
*/
static int FindLightingSamplesAndNormalMode(int sampleIdx, float *position,
                                            float *normal, float offset,
                                            void *outputLighting,
                                            float *outputNormal,
                                            int nearestSample,
                                            int *outTraceClass)
{
    float startPos[3], endPos[3];
    RayHitResult_t hitResult;
    float smallOffset = 0.125f; /* dword_457704 */
    Triangle_t *tri;
    MaterialDef_t *material;
    float baryU, baryV, baryW;
    float u, v;
    float uvScale = 512.0f; /* dword_457700 */
    int vertIdx0, vertIdx1, vertIdx2;

    if (outTraceClass)
        *outTraceClass = 0;

    /* compute start = position + normal * smallOffset */
    startPos[0] = normal[0] * smallOffset + position[0];
    startPos[1] = normal[1] * smallOffset + position[1];
    startPos[2] = normal[2] * smallOffset + position[2];

    /* compute end = position + normal * (offset + smallOffset) */
    endPos[0] = normal[0] * offset + startPos[0];
    endPos[1] = normal[1] * offset + startPos[1];
    endPos[2] = normal[2] * offset + startPos[2];

    /* no, let me re-read: start = pos + normal*smallOffset, end = pos + normal*(offset)
       Actually:
       xmm4 = normal[1], xmm3 = normal[2], xmm6 = normal[0]
       xmm4 *= offset (xmm7), xmm3 *= offset, xmm6 *= offset → normal*offset
       xmm1 = normal[2]*smallOffset, xmm5 = normal[0]*smallOffset, xmm2 = normal[1]*smallOffset
       xmm1 += pos[2], xmm5 += pos[0], xmm2 += pos[1] → start = pos + normal*smallOffset
       xmm3 += xmm1, xmm6 += xmm5, xmm4 += xmm2 → end = start + normal*offset
    */

    /* trace ray */
    TraceSetup_and_Dispatch(sampleIdx, startPos, endPos, &hitResult);
    tri = hitResult.triangle;

    if (!tri)
        return 0;

    /* copy triangle normal to output if requested */
    if (outputNormal)
    {
        outputNormal[0] = tri->normal[0];
        outputNormal[1] = tri->normal[1];
        outputNormal[2] = tri->normal[2];
    }

    material = tri->material;
    if (material->surfaceFlags & SURF_SKY)
    {
        int blockedByStaticModel = outputNormal
            ? TraceStaticModels(startPos, endPos) : 0;
        if (normal[2] < 0.0f)
            return 0;
        /* Native 0x407080 only normalizes/tests the static-model hit list when
         * the optional output-normal argument is present.  The transport
         * caller passes NULL and therefore accepts the world-sky hit here. */
        if (!blockedByStaticModel && outTraceClass)
            *outTraceClass = 3;
        return blockedByStaticModel ? 0 : -1;
    }

    if (DotProduct(tri->normal, normal) >= 0.0f)
    {
        int traceClass = 2;

        /* geometry.cpp 0x40A100 classifies an upward-facing opposing
         * triangle by probing 0.1 unit behind its centroid toward -Z. */
        if (tri->normal[2] >= 0.5f)
        {
            float probe[3];
            float end[3];
            RayHitResult_t verticalHit;
            MaterialDef_t *verticalMaterial;

            probe[0] = (g_vertData[tri->vertIndex[0]].pos[0]
                      + g_vertData[tri->vertIndex[1]].pos[0]
                      + g_vertData[tri->vertIndex[2]].pos[0])
                     * 0.3333333432674408f - tri->normal[0] * 0.1f;
            probe[1] = (g_vertData[tri->vertIndex[0]].pos[1]
                      + g_vertData[tri->vertIndex[1]].pos[1]
                      + g_vertData[tri->vertIndex[2]].pos[1])
                     * 0.3333333432674408f - tri->normal[1] * 0.1f;
            probe[2] = (g_vertData[tri->vertIndex[0]].pos[2]
                      + g_vertData[tri->vertIndex[1]].pos[2]
                      + g_vertData[tri->vertIndex[2]].pos[2])
                     * 0.3333333432674408f - tri->normal[2] * 0.1f;
            end[0] = probe[0];
            end[1] = probe[1];
            end[2] = probe[2] - 262144.0f;
            TraceSetup_and_Dispatch(sampleIdx, probe, end, &verticalHit);
            if (!verticalHit.triangle)
            {
                traceClass = 1;
            }
            else
            {
                verticalMaterial = verticalHit.triangle->material;
                if ((verticalMaterial->surfaceFlags & SURF_SKY)
                 || (verticalMaterial->contents == 1
                  && (verticalMaterial->surfaceFlags & 0x80)))
                    traceClass = 1;
            }
        }
        if (outTraceClass)
            *outTraceClass = traceClass;
        return 0;
    }

    if (tri->lightmapIdx == LIGHTMAP_NONE)
        return 0;

    baryU = hitResult.baryU;
    baryV = hitResult.baryV;
    baryW = 1.0f - baryU - baryV;

    vertIdx0 = tri->vertIndex[0];
    vertIdx1 = tri->vertIndex[1];
    vertIdx2 = tri->vertIndex[2];

    u = baryW * *(float *)&g_vertexData[vertIdx0 * 0x44]
      + baryU * *(float *)&g_vertexData[vertIdx1 * 0x44]
      + baryV * *(float *)&g_vertexData[vertIdx2 * 0x44];
    v = baryW * *(float *)&g_vertexData[vertIdx0 * 0x44 + 4]
      + baryU * *(float *)&g_vertexData[vertIdx1 * 0x44 + 4]
      + baryV * *(float *)&g_vertexData[vertIdx2 * 0x44 + 4];
    u *= uvScale;
    v *= uvScale;

    /* Retail 0x407080 has two output modes.  The default transport path and
     * light-grid producer pass true and choose one clamped floor sample;
     * ground lighting passes false and receives bilinear samples. */
    if (nearestSample)
    {
        LightingHit_t *hit = (LightingHit_t *)outputLighting;
        LightmapSample_t *sample;
        int s = (int)floorf(u);
        int t = (int)floorf(v);

        if (s < 0) s = 0;
        else if (s > 511) s = 511;
        if (t < 0) t = 0;
        else if (t > 511) t = 511;

        GetLightingSample((int)tri->lightmapIdx, (float)s, (float)t,
                          (void **)&sample);
        if (!sample || sample->weight <= 0.0f)
            return 0;

        hit->sample = (LightingSample_t *)sample;
        hit->weight = 1.0f;
        hit->_pad = 0;
        if (outTraceClass)
            *outTraceClass = 4;
        return 1;
    }

    {
        LightingHit_t *hits = (LightingHit_t *)outputLighting;
        int baseS = (int)floorf(u);
        int baseT = (int)floorf(v);
        float fracS = u - (float)baseS;
        float fracT = v - (float)baseT;
        float sWeights[2] = { 1.0f - fracS, fracS };
        float tWeights[2] = { 1.0f - fracT, fracT };
        float totalWeight = 0.0f;
        int hitCount = 0;
        int dy, dx;

        for (dy = 0; dy < 2; dy++)
        {
            int t = baseT + dy;
            if (t < 0 || t >= 512 || tWeights[dy] == 0.0f)
                continue;
            for (dx = 0; dx < 2; dx++)
            {
                int s = baseS + dx;
                LightmapSample_t *sample;
                float weight = sWeights[dx] * tWeights[dy];
                if (s < 0 || s >= 512 || weight == 0.0f)
                    continue;

                GetLightingSample((int)tri->lightmapIdx, (float)s, (float)t,
                                  (void **)&sample);
                if (!sample || sample->weight <= 0.0f)
                    continue;

                hits[hitCount].sample = (LightingSample_t *)sample;
                hits[hitCount].weight = weight;
                hits[hitCount]._pad = 0;
                totalWeight += weight;
                hitCount++;
            }
        }

        if (hitCount && totalWeight > 0.0f && totalWeight < 0.9990000129f)
        {
            float scale = 1.0f / totalWeight;
            int i;
            for (i = 0; i < hitCount; i++)
                hits[i].weight *= scale;
        }
        if (hitCount && outTraceClass)
            *outTraceClass = 4;
        return hitCount;
    }
}

int FindLightingSamplesAndNormal(int sampleIdx, float *position, float *normal,
                                 float offset, void *outputLighting,
                                 float *outputNormal)
{
    return FindLightingSamplesAndNormalMode(sampleIdx, position, normal, offset,
                                            outputLighting, outputNormal, 0,
                                            NULL);
}

int FindLightingSamplesAndNormalNearest(int sampleIdx, float *position,
                                        float *normal, float offset,
                                        void *outputLighting,
                                        float *outputNormal)
{
    return FindLightingSamplesAndNormalMode(sampleIdx, position, normal, offset,
                                            outputLighting, outputNormal, 1,
                                            NULL);
}

/* geometry.cpp 0x409D50/0x409C90/0x409E10.  Retail uses its multi-hit world
 * trace; the pointer-safe port applies the same terminal-material predicates
 * to the existing software trace. */
static int Lighting_VerticalTraceIsSky(int sampleIdx, const float *point,
                                       int upward, int allowSpecialSurface)
{
    float end[3];
    RayHitResult_t hit;
    MaterialDef_t *material;

    end[0] = point[0];
    end[1] = point[1];
    end[2] = point[2] + (upward ? 262144.0f : -262144.0f);
    TraceSetup_and_Dispatch(sampleIdx, (float *)point, end, &hit);
    if (!hit.triangle)
        return 1;

    material = hit.triangle->material;
    if (material->surfaceFlags & SURF_SKY)
        return 1;
    return allowSpecialSurface && material->contents == 1
        && (material->surfaceFlags & 0x80);
}

int Lighting_RejectTransportPoint(int sampleIdx, const float *point)
{
    return Lighting_VerticalTraceIsSky(sampleIdx, point, 0, 1)
        && !Lighting_VerticalTraceIsSky(sampleIdx, point, 1, 0);
}


static char s_assertDisable_Sky_subSample;    /* byte_480A84 */
static char s_assertDisable_Sky_sample;       /* byte_480A83 */
static char s_assertDisable_Sky_vars;         /* byte_480A82 */
static char s_assertDisable_Sky_area;         /* byte_480A81 */
static char s_assertDisable_Sky_nanEnergy;    /* byte_480A80 */
static char s_assertDisable_Sky_nanResult;    /* byte_480A7F */

/*
================
GatherSkyLighting

Gathers sky illumination for a lighting subsample. Traces a ray
from the surface point along the normal direction to detect sky
visibility. If the sky is visible (backface material hit), accumulates
weighted sky color into the sample's lighting data.

Uses global settings g_sunDirX/C0/C4 for the sky direction
basis coefficients and g_sunColorR/CC/D0 for the sky color.
================
*/
void GatherSkyLighting(int sampleIdx, float *position, float *basis,
                       float skyWeight, float subAreaFactor,
                       unsigned char primaryLightIndex,
                       SubSample_t *subSample)
{
    float startPos[3], endPos[3];
    RayHitResult_t hitResult;
    Triangle_t *tri;
    float skyDir;
    float directEnergy[3];
    float radiosityEnergy[3];
    float localSunDir[3];
    Sample_t *sample;
    SampleVars_t *sampleVars;
    float smallOffset = 0.125f;
    float rayLen;
    float visibility;

    Assert(subSample != 0, s_assertDisable_Sky_subSample);

    sample = *(Sample_t **)subSample;
    Assert(sample != 0, s_assertDisable_Sky_sample);

    sampleVars = sample->vars;
    Assert(sampleVars != 0, s_assertDisable_Sky_vars);

    Assert(subAreaFactor > 0, s_assertDisable_Sky_area);

    /* compute sky direction dot product with surface normal (3rd row of basis at [6..8]) */
    skyDir = g_sunDirY * basis[7] + g_sunDirX * basis[6] + g_sunDirZ * basis[8];
    if (skyDir <= 0.0f)
        return;

    /* compute ray start (offset along normal) and end (further along sky direction) */
    rayLen = k_skyRayLength; /* sky ray length constant */

    startPos[0] = basis[6] * smallOffset + position[0];
    startPos[1] = basis[7] * smallOffset + position[1];
    startPos[2] = basis[8] * smallOffset + position[2];

    endPos[0] = g_sunDirX * rayLen + startPos[0];
    endPos[1] = g_sunDirY * rayLen + startPos[1];
    endPos[2] = g_sunDirZ * rayLen + startPos[2];

    /* trace ray */
    TraceSetup_and_Dispatch(sampleIdx, startPos, endPos, &hitResult);
    tri = hitResult.triangle;

    if (!tri)
        return;

    /* must hit sky (backface material) */
    if (!(tri->material->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY)))
        return;

    /* backface check */
    if (TraceStaticModels(startPos, endPos))
        return;

    /* Native 0x407770 derives a fractional sun visibility from a jittered
     * multi-sample hit mask (popcount/sampleCount, 0x409B60), where alpha-
     * masked materials subtract individual sub-rays and a flagged static-model
     * final hit yields 0.01.  The single-ray trace above is the binary case of
     * that (this trace set has no alpha-masked occluders yet): 1 when the ray
     * reaches sky. */
    visibility = 1.0f;

    /* CoD4 keeps direct sunlight and the energy injected into radiosity as
     * separate colors.  Both scale by visibility * skyWeight (0x407770
     * v30 = vis * a8). */
    directEnergy[0] = g_sunColorR * (visibility * skyWeight);
    directEnergy[1] = g_sunColorG * (visibility * skyWeight);
    directEnergy[2] = g_sunColorB * (visibility * skyWeight);
    radiosityEnergy[0] = g_backfaceLightR * (visibility * skyWeight);
    radiosityEnergy[1] = g_backfaceLightG * (visibility * skyWeight);
    radiosityEnergy[2] = g_backfaceLightB * (visibility * skyWeight);

    Assert(!IS_NAN_FLOAT(directEnergy[0]) && !IS_NAN_FLOAT(directEnergy[1]) && !IS_NAN_FLOAT(directEnergy[2]),
           s_assertDisable_Sky_nanEnergy);

    localSunDir[0] = g_sunDirX * basis[0] + g_sunDirY * basis[1] + g_sunDirZ * basis[2];
    localSunDir[1] = g_sunDirX * basis[3] + g_sunDirY * basis[4] + g_sunDirZ * basis[5];
    localSunDir[2] = skyDir;
    /* 0x4075D0 influence modes: mode 1 (the surface's primary light IS the
     * sun) accumulates vis * subAreaX2 into that 2x2 intensity scalar; any
     * other primary (mode 2) becomes a directional transport record instead
     * and leaves the scalar image untouched.  Feeding the scalar
     * unconditionally lights surfaces the game will never apply the sun to. */
    if (primaryLightIndex == (unsigned char)g_sunPrimaryLightIndex)
    {
        int idx = subSample->s + subSample->t * 2;
        ((float *)sampleVars)[idx] += visibility * subAreaFactor;
    }
    else
    {
        AppendDirectTransport(sample, directEnergy, localSunDir);
    }

    /* 0x407770 pre-scales the colors by visibility and clipped sub-area;
     * 0x4075D0 then applies normal dot sun to accumulated/ping energy. */
    sampleVars->gatheredIncident[0] += directEnergy[0] * skyDir;
    sampleVars->gatheredIncident[1] += directEnergy[1] * skyDir;
    sampleVars->gatheredIncident[2] += directEnergy[2] * skyDir;

    /* `scattered` carries the material reflectivity vector in the recovered
     * retail topology; direct sunlight must not overwrite it. */
    sampleVars->unscattered[0] += radiosityEnergy[0] * skyDir;
    sampleVars->unscattered[1] += radiosityEnergy[1] * skyDir;
    sampleVars->unscattered[2] += radiosityEnergy[2] * skyDir;

    Assert(!IS_NAN_FLOAT(sampleVars->unscattered[0])
        && !IS_NAN_FLOAT(sampleVars->unscattered[1])
        && !IS_NAN_FLOAT(sampleVars->unscattered[2]),
           s_assertDisable_Sky_nanResult);
}

extern void *g_lightDirArray;

/* Native compile.cpp 0x406BA0: inject the normalized per-direction sky
 * visibility into the accumulated light and the first radiosity ping buffer. */
static void SeedSkyLightForSample(Sample_t *sample)
{
    SampleVars_t *vars;
    int i;

    if (!sample || !(vars = sample->vars) || !vars->skyInfluences)
        return;

    for (i = 0; i < g_traces; i++)
    {
        LightDirEntry_t *dir = &((LightDirEntry_t *)g_lightDirArray)[i];
        float weight = vars->skyInfluences[i] * dir->z;
        float energy[3];

        energy[0] = g_sunRadiosityR * weight;
        energy[1] = g_sunRadiosityG * weight;
        energy[2] = g_sunRadiosityB * weight;

        vars->gatheredIncident[0] += energy[0];
        vars->gatheredIncident[1] += energy[1];
        vars->gatheredIncident[2] += energy[2];
        vars->unscattered[0] += energy[0];
        vars->unscattered[1] += energy[1];
        vars->unscattered[2] += energy[2];
    }
}

/* Forward declarations for functions not in cod2rad64.h */
extern int GetPointLightCount(void);
extern void SetLightingSampleAreas(int threadCount);
extern void BuildLightTransfers(int threadCount);
extern void Lighting_InitSamples(void);                                /* lighting_412350 */
extern void BeginProgress(const char *msg);
extern void EndProgress(void);
extern void ForEachUsefulLightingSample(void (*func)(void *), int threadCount);
extern void CalculateGroundLightingForAllStaticModels(void);
/* malloc/free from <stdlib.h> */
extern void Lighting_GetGatheredLight(void *sample, float *outColor);    /* lighting_412550 */
extern void *g_lightDirArray;                                  /* qword_480A60 */
extern int g_totalLightCount;                                  /* dword_480A48 */

/*
================
BounceGatherCallback

Iterates through the lighting transfer chain for a sample,
accumulating bounced light. For each transfer entry, multiplies
the scatter color by g_radiosityScale * entry->weight, then calls
GatherSurfaceIncidentEnergyForLightFromDir to accumulate into
the destination sample's SH bands.
================
*/
void BounceGatherCallback(Sample_t *sample)
{
    SampleVars_t *sampleVars;
    TransferBlock_t *block;
    float bounceColor[3];
    int i;

    sampleVars = sample->vars;
    block = sampleVars->transferHead;

    if (!block)
        return;

    while (block)
    {
        for (i = 0; i < TRANSFERS_PER_BLOCK; i++)
        {
            TransferEntry_t *entry = &block->entries[i];

            if (!entry->toSample)
                break;

            {
                SampleVars_t *sourceVars = entry->toSample->vars;
                float *lightDir = (float *)&((LightDirEntry_t *)g_lightDirArray)[entry->lightIdx];

                /* Native lighting.cpp:641 multiplies the source sample's
                 * accumulated incident RGB by its radiosity colour.  The
                 * radiosity scale is already folded into scattered by
                 * Lighting_ApplyRadiosityScale. */
                bounceColor[0] = sourceVars->gatheredIncident[0] * sourceVars->scattered[0]
                               * entry->weight;
                bounceColor[1] = sourceVars->gatheredIncident[1] * sourceVars->scattered[1]
                               * entry->weight;
                bounceColor[2] = sourceVars->gatheredIncident[2] * sourceVars->scattered[2]
                               * entry->weight;
                GatherSurfaceIncidentEnergyForLightFromDir(bounceColor,
                    lightDir, sampleVars->incident);
            }
        }

        block = (TransferBlock_t *)block->next;
    }
}

/*
================
Compile

Main radiosity compilation entry point. Orchestrates the full
pipeline: collision BSP build, sample area calculation, lighting
setup, light transport, radiosity bounce, lightmap building,
light grid, ground lighting, and cleanup.
================
*/
/* forward decls for debug globals used by Compile() below */

void Compile(int threadCount)
{
    int numLights;

    numLights = GetPointLightCount();
    g_totalLightCount = numLights + 2;

    InitGeometry();
    if (g_embreeEnabled)
        InitEmbreeScene();

    if (g_adaptiveEnabled)
    {
        BeginProgress("Adaptive lightmap analysis...");
        AdaptiveLightmapRepack(threadCount);
        EndProgress();
    }

    if (g_uvRepackEnabled)
    {
        BeginProgress("UV repack...");
        UVRepack();
        EndProgress();
    }

    BeginProgress("Calculating sample areas...");
    /* The recovered rasterizer accumulates shared sample weights as floats.
     * Running that reduction through the dynamic worker queue makes the sum
     * order scheduler-dependent; retail is byte-stable for -Threads 1..4.
     * Keep only this reduction ordered until its native per-thread scratch
     * layout is represented locally. */
    SetLightingSampleAreas(1);
    EndProgress();

    Lighting_InitSamples();

    /* Native runs direct radiosity before constructing transport
     * (0x40C6E0/0x40C1C0 before 0x40C700).  The recovered local raster
     * callback had these operations fused, so split its two modes here. */
    BeginProgress("Getting radiosity color for each sample...");
    g_lightingTransportPass = 0;
    BuildLightTransfers(1);
    EndProgress();

    BeginProgress("Applying radiosity scale...");
    ForEachUsefulLightingSample(Lighting_ApplyRadiosityScale, threadCount);
    EndProgress();

    /* Retail initializes the randomized direction table immediately before
     * validating radtrans.bin; the cache header includes all of these knobs. */
    SetupSampleRadii();

    g_relightCacheLoaded = 0;
    if (g_relightLoadEnabled && RadTrans_LoadV3())
    {
        g_relightSaveEnabled = 0;
        BeginProgress("Building light transport for light sources...");
    }
    else
    {
        g_relightLoadEnabled = 0;
        Relight_ResetSuppressedRanges();
        BeginProgress("Building light transport for everything...");
    }
    g_lightingTransportPass = 1;
    g_lightingSeedSkyPass = 0;
    /* The local chain representation is updated by multiple triangles.  Its
     * native flat-transfer scratch/ordered merge has not yet been recovered,
     * so preserve retail's thread-invariant result by constructing it in work
     * order.  Tracing after construction remains parallel. */
    BuildLightTransfers(1);
    EndProgress();

    if (g_relightSaveEnabled)
        RadTrans_SaveV3();

    BeginProgress("Normalizing transport weights...");
    ForEachUsefulLightingSample(NormalizeLightTransfers, 1);
    EndProgress();

    /* Native 0x416090 seeds the already-recorded, normalized sky influences;
     * it does not replay geometry or consume a second random sequence. */
    BeginProgress("Seeding sky light...");
    ForEachUsefulLightingSample(SeedSkyLightForSample, threadCount);
    EndProgress();

    /* radiosity bounce */
    {
        float epsilon = k_smallEpsilon; /* small constant */
        if (g_maxBounces > 1)
            RadiosityBounce(epsilon, threadCount);
    }

    /* The pre-existing transfer representation is outbound rather than the
     * retail compiler's inbound flat lists.  Materialize its accumulated
     * indirect term into the sample SH field before the retail-equivalent
     * bleed/final-image stages below. */
    ForEachUsefulLightingSample(BounceGatherCallback, threadCount);

    /* Retail bleeds the sample field before converting it to the packed CoD4
     * lightmap images.  Ground and grid lighting follow this phase
     * (cod4rad.exe 0x4081C4..0x408206). */
    BeginProgress("Finding lightmap bleeding...");
    InitBleeding(threadCount);
    EndProgress();

    BeginProgress("Building final lightmaps...");
    BuildFinalLightmaps_TripleLoop();
    EndProgress();

    /* ground lighting */
    BeginProgress("Calculating ground lighting for static models...");
    CalculateGroundLightingForAllStaticModels();
    EndProgress();

    /* light grid */
    BeginProgress("Calculating light grid...");
    CalculateLightGrid(threadCount);
    EndProgress();

    /* free transfer pool blocks */
    while (g_transferPool)
    {
        void *next = *(void **)((unsigned char *)g_transferPool + TRANSFER_POOL_LINK_OFFSET);
        free(g_transferPool);
        g_transferPool = next;
    }

}

/*
================
RadiosityBounce

Performs iterative radiosity bounce passes. Each pass gathers
indirect illumination from neighboring samples weighted by
transfer coefficients and bounce fraction.
================
*/
/*
================
GatherPointLightForSample

Gathers direct illumination from a single point light for a
lighting subsample. Traces shadow rays and accumulates weighted
light contribution into the sample's SH bands.
================
*/
extern int PointLightEvaluatePoint(int surfacePrimaryLightIndex, int sampleIdx,
    int lightIdx, float *position, float *normal, float *outDir,
    float *outColor, float *outArg); /* pointlights_419090 */

static char s_assertDisable_Point_sample;    /* byte_480A88 */
static char s_assertDisable_Point_vars;      /* byte_480A87 */
static char s_assertDisable_Point_nanEnergy; /* byte_480A86 */
static char s_assertDisable_Point_nanResult; /* byte_480A85 */

void GatherPointLightForSample(int sampleIdx, int lightIdx, float *position,
    float *basis, float subAreaFactor, unsigned char primaryLightIndex,
    SubSample_t *subSample)
{
    Sample_t *sample;
    SampleVars_t *sampleVars;
    float lightColor[3];
    float lightDirection[3];
    float outArg;
    float energy[3];
    int result;

    sample = subSample ? subSample->sample : NULL;
    Assert(sample != 0, s_assertDisable_Point_sample);
    sampleVars = sample->vars;
    Assert(sampleVars != 0, s_assertDisable_Point_vars);

    /* call point light gathering — returns 0 (no light), 1 (directional), 2 (ambient) */
    /* binary passes basis+6 (the surface normal, 3rd row of basis) as 4th arg */
    result = PointLightEvaluatePoint(primaryLightIndex, sampleIdx, lightIdx - 2,
        position, basis + 6,
        lightDirection, lightColor, &outArg);

    if (result == 0)
        return;

    /* compute energy = lightColor * subAreaFactor */
    energy[0] = lightColor[0] * subAreaFactor;
    energy[1] = lightColor[1] * subAreaFactor;
    energy[2] = lightColor[2] * subAreaFactor;

    Assert(!IS_NAN_FLOAT(energy[0]) && !IS_NAN_FLOAT(energy[1]) && !IS_NAN_FLOAT(energy[2]),
           s_assertDisable_Point_nanEnergy);

    if (result == 1)
    {
        int intensityIndex = subSample->s + 2 * subSample->t;
        sampleVars->intensity[intensityIndex] += subAreaFactor;
    }
    else if (result == 2)
    {
        /* directional: transform light direction through basis matrix */
        float dir[3];

        dir[0] = lightDirection[1] * basis[1] + lightDirection[0] * basis[0] + lightDirection[2] * basis[2];
        dir[1] = lightDirection[0] * basis[3] + lightDirection[1] * basis[4] + lightDirection[2] * basis[5];
        dir[2] = lightDirection[0] * basis[6] + lightDirection[1] * basis[7] + lightDirection[2] * basis[8];

        AppendDirectTransport(sample, energy, dir);
        GatherSurfaceIncidentEnergyForLightFromDir(energy, dir,
            sampleVars->incident);
    }
    else
    {
        Assert(result == 3, s_assertDisable_Point_sample);

        /* Native LIGHT_INFLUENCE_COINCIDENT is direction independent and is
         * added to every final basis direction before gamma correction. */
        AcquireThreadLock((unsigned int)(uintptr_t)sample);
        sampleVars->coincident[0] += energy[0];
        sampleVars->coincident[1] += energy[1];
        sampleVars->coincident[2] += energy[2];
        ReleaseThreadLock((unsigned int)(uintptr_t)sample);
    }

    /* accumulate energy * areaFactor into unscattered — outArg is GatherPointLight's 7th output */
    {
        sampleVars->unscattered[0] += energy[0] * outArg;
        sampleVars->unscattered[1] += energy[1] * outArg;
        sampleVars->unscattered[2] += energy[2] * outArg;
        sampleVars->gatheredIncident[0] += energy[0] * outArg;
        sampleVars->gatheredIncident[1] += energy[1] * outArg;
        sampleVars->gatheredIncident[2] += energy[2] * outArg;
    }

    Assert(!IS_NAN_FLOAT(sampleVars->unscattered[0])
        && !IS_NAN_FLOAT(sampleVars->unscattered[1])
        && !IS_NAN_FLOAT(sampleVars->unscattered[2]),
           s_assertDisable_Point_nanResult);
}

/*
================
SetupSampleRadii

Computes sample radii for all lighting samples based on their
area and neighbor distances. Used for the radiosity transfer
radius computation.
================
*/
extern void UniformPointsOnHemisphere(int count, float *dirs, int stride); /* com_math_428CF0 */
extern float Vec2DistanceSq(float *a, float *b);                    /* sub_4291D0: distance between 2D points */
/* sqrtf is from CRT (sub_43D3C0) */
float g_invTraces;       /* native 0x407C48: 2.0 / g_traces */

void SetupSampleRadii(void)
{
    int i, j;
    float *dirArray;
    float minDistSq;
    float dist;
    float radius;
    float maxRadius;

    /* allocate light direction array: g_traces entries, 16 bytes each */
    dirArray = (float *)malloc(g_traces * 16);
    g_lightDirArray = dirArray;
    UniformPointsOnHemisphere(g_traces, dirArray, 16);

    /* special case: single trace */
    if (g_traces == 1)
    {
        dirArray[0] = 0.0f;
        dirArray[1] = 0.0f;
    }

    maxRadius = k_largeRadius; /* large initial radius constant */

    /* for each light direction, find minimum distance to neighbors */
    for (i = 0; i < g_traces; i++)
    {
        float *cur = (float *)&((LightDirEntry_t *)g_lightDirArray)[i];
        float len;

        /* compute current vector length */
        len = sqrtf(cur[0] * cur[0] + cur[1] * cur[1]);

        /* find min squared distance to any other direction, scaled by 0.25 */
        minDistSq = (maxRadius - len) * (maxRadius - len);

        for (j = 0; j < g_traces; j++)
        {
            if (j == i)
                continue;

            dist = Vec2DistanceSq(
                (float *)&((LightDirEntry_t *)g_lightDirArray)[j],
                (float *)&((LightDirEntry_t *)g_lightDirArray)[i]);
            dist *= 0.25f; /* dword_4573A4 = 0.25 */

            if (dist < minDistSq)
                minDistSq = dist;
        }

        /* radius = sqrt(minDistSq) * g_jitter */
        radius = sqrtf(minDistSq);
        cur[3] = radius * g_jitter; /* store radius at offset +0xC */
    }

    /* Hemisphere Monte Carlo normalization.  CoD4 integrates over the
     * hemisphere with 2/N; the retained CoD2 port used 1/N, halving both
     * sky seeds and every transport coefficient. */
    g_invTraces = 2.0f / (float)g_traces;
}

/*
================
BuildLightingTransfersForSample

Builds the lighting transfer data for a single sample by tracing
rays to neighbor samples and computing transfer weights.
================
*/
static char s_assertDisable_Build_sample;    /* byte_480A8D */
static char s_assertDisable_Build_vars;      /* byte_480A8C */
static char s_assertDisable_Build_nanWeight; /* byte_480A8B */
static char s_assertDisable_Build_nanBounce; /* byte_480A8A */
static char s_assertDisable_Build_nanResult; /* byte_480A89 */

void BuildLightingTransfersForSample(float *bouncedLight, float weight, Sample_t *sample)
{
    SampleVars_t *sampleVars;
    float weighted[3];

    Assert(sample != 0, s_assertDisable_Build_sample);
    sampleVars = sample->vars;
    Assert(sampleVars != 0, s_assertDisable_Build_vars);
    Assert(!IS_NAN_FLOAT(weight), s_assertDisable_Build_nanWeight);
    Assert(!IS_NAN_FLOAT(bouncedLight[0]) && !IS_NAN_FLOAT(bouncedLight[1])
        && !IS_NAN_FLOAT(bouncedLight[2]), s_assertDisable_Build_nanBounce);

    weighted[0] = weight * bouncedLight[0];
    weighted[1] = weight * bouncedLight[1];
    weighted[2] = weight * bouncedLight[2];

    AcquireThreadLock((unsigned int)(uintptr_t)sample);

    sampleVars->scattered[0] += weighted[0];
    sampleVars->scattered[1] += weighted[1];
    sampleVars->scattered[2] += weighted[2];

    sampleVars->unscattered[0] += weighted[0];
    sampleVars->unscattered[1] += weighted[1];
    sampleVars->unscattered[2] += weighted[2];

    Assert(!IS_NAN_FLOAT(sampleVars->unscattered[0])
        && !IS_NAN_FLOAT(sampleVars->unscattered[1])
        && !IS_NAN_FLOAT(sampleVars->unscattered[2]),
           s_assertDisable_Build_nanResult);

    ReleaseThreadLock((unsigned int)(uintptr_t)sample);
}

/*
================
GatherBounceForSample

Gathers indirect (bounced) illumination for a single sample.
Iterates the transfer chain and accumulates weighted contributions
from neighboring samples' scattered light.
================
*/
static char s_assertDisable_Bounce_source;      /* byte_480A91 */
static char s_assertDisable_Bounce_sourceVars;  /* byte_480A90 */
static char s_assertDisable_Bounce_sampleVars;  /* byte_480A7E */
static char s_assertDisable_Bounce_nanSource;   /* byte_480A8F */
static char s_assertDisable_Bounce_nanBounced;  /* byte_480A8E */

extern float g_energyScale; /* dword_4576F8: 1/3 or similar */

void GatherBounceForSample(Sample_t *sourceSample, int threadIdx)
{
    SampleVars_t *sourceVars;
    float totalEnergy;
    float bouncedLight[3];
    TransferBlock_t *block;
    int i;

    Assert(sourceSample != 0, s_assertDisable_Bounce_source);
    sourceVars = sourceSample->vars;
    Assert(sourceVars != 0, s_assertDisable_Bounce_sourceVars);
    Assert(sourceVars != 0, s_assertDisable_Bounce_sampleVars);

    /* The producer stores transfers on the hit sample: each entry therefore
     * names an incoming source.  This is the same pull topology as retail
     * 0x407C50 (the old port mistakenly pushed entries back to their source). */
    Assert(!IS_NAN_FLOAT(sourceVars->unscattered[0])
        && !IS_NAN_FLOAT(sourceVars->unscattered[1])
        && !IS_NAN_FLOAT(sourceVars->unscattered[2]),
           s_assertDisable_Bounce_nanSource);

    bouncedLight[0] = bouncedLight[1] = bouncedLight[2] = 0.0f;
    block = sourceVars->transferHead;
    while (block)
    {
        for (i = 0; i < TRANSFERS_PER_BLOCK; ++i)
        {
            TransferEntry_t *entry = &block->entries[i];
            SampleVars_t *fromVars;
            float color[3];
            if (!entry->toSample)
                break;
            fromVars = ((Sample_t *)entry->toSample)->vars;
            float cosine = ((LightDirEntry_t *)g_lightDirArray)[entry->lightIdx].z;
            color[0] = fromVars->unscattered[0] * fromVars->scattered[0] * entry->weight * cosine;
            color[1] = fromVars->unscattered[1] * fromVars->scattered[1] * entry->weight * cosine;
            color[2] = fromVars->unscattered[2] * fromVars->scattered[2] * entry->weight * cosine;
            bouncedLight[0] += color[0];
            bouncedLight[1] += color[1];
            bouncedLight[2] += color[2];
        }
        block = block->next;
    }
    /* `scattered` is this pass's destination ping-pong buffer.  RadiosityBounce
     * commits it only after every worker has consumed unscattered. */
    {
        ptrdiff_t index = (LightmapSample_t *)sourceSample - (LightmapSample_t *)g_lightingSamples;
        g_bounceStage[index * 3 + 0] = bouncedLight[0];
        g_bounceStage[index * 3 + 1] = bouncedLight[1];
        g_bounceStage[index * 3 + 2] = bouncedLight[2];
    }

    /* Retail 0x407C50 retains every pass in gatheredIncident while only the
     * ping-pong buffer is replaced for the next pass. */
    sourceVars->gatheredIncident[0] += bouncedLight[0];
    sourceVars->gatheredIncident[1] += bouncedLight[1];
    sourceVars->gatheredIncident[2] += bouncedLight[2];

    totalEnergy = (bouncedLight[0] + bouncedLight[1] + bouncedLight[2]) * g_energyScale;
    if (totalEnergy > g_bounceEnergy[threadIdx + 1])
        g_bounceEnergy[threadIdx + 1] = totalEnergy;
}

static void CommitBounceForSample(Sample_t *sample)
{
    SampleVars_t *vars = sample->vars;
    ptrdiff_t index = (LightmapSample_t *)sample - (LightmapSample_t *)g_lightingSamples;
    vars->unscattered[0] = g_bounceStage[index * 3 + 0];
    vars->unscattered[1] = g_bounceStage[index * 3 + 1];
    vars->unscattered[2] = g_bounceStage[index * 3 + 2];
}

/*
================
RadiosityBounce

Performs iterative radiosity bounce passes until energy converges
below epsilon or diverges. Each pass calls GatherBounceForSample
for all samples via ForEachSample.
================
*/

void RadiosityBounce(float epsilon, int threadCount)
{
    int pass;
    float maxEnergy;
    float firstBounceMax;
    char *msg;
    int i;

    firstBounceMax = k_initialMaxEnergy;

    if (!g_bounceEnergy || g_bounceEnergyCount < threadCount + 1)
    {
        if (g_bounceEnergy) free(g_bounceEnergy);
        g_bounceEnergyCount = threadCount + 1;
        g_bounceEnergy = (float *)calloc(g_bounceEnergyCount, sizeof(float));
    }
    if (!g_bounceStage || g_bounceStageCount < g_totalSampleCount)
    {
        free(g_bounceStage);
        g_bounceStageCount = g_totalSampleCount;
        g_bounceStage = (float *)calloc((size_t)g_bounceStageCount * 3, sizeof(*g_bounceStage));
    }

    pass = 0;
    while (1)
    {
        pass++;

        msg = va("Radiosity bounce %i...", pass);
        BeginProgress(msg);

        /* clear energy accumulators */
        for (i = 1; i <= threadCount; i++)
            g_bounceEnergy[i] = 0.0f;

        /* gather bounce for all samples */
        ForEachUsefulLightingSampleThreaded(GatherBounceForSample, threadCount);

        /* find max energy across all accumulators */
        maxEnergy = g_bounceEnergy[1];
        for (i = 2; i <= threadCount; i++)
        {
            if (g_bounceEnergy[i] > maxEnergy)
                maxEnergy = g_bounceEnergy[i];
        }
        EndProgress();
        ForEachUsefulLightingSample(CommitBounceForSample, 1);

        if (pass == 1)
        {
            firstBounceMax = maxEnergy;
        }
        else
        {
            /* Retail compares every later pass with the first pass; it does
             * not update this baseline after each bounce (0x407F50). */
            if (maxEnergy > firstBounceMax * 2.0f)
            {
                Com_Printf("\n\nAborting radiosity due to a positive feedback loop.\n");
                Com_Printf("This can usually be fixed by changing '-traces' slightly (currently %i).\n", g_traces);
                Com_Printf("Reducing the radiosity scale can also help this (currently %g).\n",
                           (double)g_radiosityScale);
                break;
            }
        }

        /* check convergence */
        if (maxEnergy <= epsilon || pass + 1 == g_maxBounces)
            break;
    }
}

/*
================
FindLightingTransfersForDirection

Traces rays in a specific direction to find lighting transfers
between samples. Used during the transport building phase.
================
*/
/*
================
FindLightingTransfersForDirection

Traces rays in a specific direction from a sample to find lighting
transfers to neighboring samples. Computes transfer weights based
on visibility and distance.

1367 bytes — the largest function in compile.cpp. Contains ray
tracing, transfer allocation, weight computation, and NaN checks.

Parameters:
  ecx = sampleIdx, rdx = sample, r8 = position, r9 = normal,
  [stack] = dirIdx, [stack] = subAreaFactor
================
*/
/* rand is from CRT (sub_43D4C0) */
extern void Vec2Normalize(float *v);

static char s_assertDisable_Dir_areaFactor;  /* byte_480A97 */
static char s_assertDisable_Dir_sampleArea;  /* byte_480A96 */
static char s_assertDisable_Dir_vars;        /* byte_480A95 */
static char s_assertDisable_Dir_incident;    /* byte_480A94 */
static char s_assertDisable_Dir_nanEnergy;   /* byte_480A93 */
static char s_assertDisable_Dir_nanResult;   /* byte_480A92 */

int FindLightingTransfersForDirection(int sampleIdx, Sample_t *sample,
    float *position, float *normal, float subAreaFactor, int dirIdx)
{
    float *dirEntry;
    float randScale;
    float rx, ry;
    float lenSq;
    float pertDir[3]; /* perturbed direction on disk */
    float worldDir[3];
    LightingHit_t hitResults[4];
    float outputNormal[3];
    SampleVars_t *sampleVars;
    int result;
    int traceClass;
    float factor;
    float energy[3];
    int i;

    dirEntry = (float *)&((LightDirEntry_t *)g_lightDirArray)[dirIdx];
    randScale = k_randScale; /* rand scale constant */

    /* generate random point on unit disk via rejection sampling */
    {
        int rej = 0;
        do {
            extern int rand_int(void);
            rx = (float)rand_int() * randScale - 1.0f;
            ry = (float)rand_int() * randScale - 1.0f;
            lenSq = rx * rx + ry * ry;
            rej++;
        } while (lenSq > 1.0f);
    }

    /* compute perturbed direction using radius from dirEntry[3] */
    {
        float radius = dirEntry[3];
        pertDir[0] = dirEntry[0] + radius * rx;
        pertDir[1] = dirEntry[1] + radius * ry;

        /* compute z from unit sphere */
        lenSq = pertDir[0] * pertDir[0] + pertDir[1] * pertDir[1];
        if (lenSq <= 1.0f)
        {
            pertDir[2] = sqrtf(1.0f - lenSq);
        }
        else
        {
            Vec2Normalize(pertDir);
            pertDir[2] = k_smallEpsilon; /* small epsilon */
        }
    }

    Assert(subAreaFactor > 0, s_assertDisable_Dir_areaFactor);
    /* KISAK: >= rather than >.  Transport rejects subtract min(cellArea,
     * weight) from the sample total (geometry.c ProcessLightingSampleArea), so
     * a fully occluded texel legitimately drains to exactly 0 while a sibling
     * 2x2 channel still holds area; retail 32-bit processes that state
     * without complaint.  Negative still means corruption. */
    Assert(sample->areaX2 >= 0, s_assertDisable_Dir_sampleArea);

    sampleVars = sample->vars;
    Assert(sampleVars != 0, s_assertDisable_Dir_vars);
    Assert(sampleVars->incident != 0, s_assertDisable_Dir_incident);

    /* transform perturbed direction to world space via normal basis */
    worldDir[0] = pertDir[0] * normal[0] + pertDir[1] * normal[3] + pertDir[2] * normal[6];
    worldDir[1] = pertDir[0] * normal[1] + pertDir[1] * normal[4] + pertDir[2] * normal[7];
    worldDir[2] = pertDir[0] * normal[2] + pertDir[1] * normal[5] + pertDir[2] * normal[8];

    /* trace ray along world direction */
    result = FindLightingSamplesAndNormalMode(sampleIdx, position, worldDir,
        k_skyRayLength, hitResults, NULL, g_traceFilter == 1, &traceClass);

    if (traceClass <= 2)
        return traceClass;

    factor = g_invTraces * subAreaFactor;

    if (traceClass == 3)
    {
        /* Native records sky visibility per trace during transport; the
         * normalized vector is injected later by the dedicated seed pass. */
        AcquireThreadLock((unsigned int)(uintptr_t)sample);
        if (!sampleVars->skyInfluences)
            sampleVars->skyInfluences = (float *)calloc((size_t)g_traces,
                                                        sizeof(float));
        if (!sampleVars->skyInfluences)
            Error("Out of memory allocating sky influences\n");
        sampleVars->skyInfluences[dirIdx] += factor;
        ReleaseThreadLock((unsigned int)(uintptr_t)sample);
        return 3;
    }
    else if (traceClass == 4)
    {
        if (g_lightingSeedSkyPass)
            return;
        /* hit neighbor samples — allocate transfers */
        for (i = 0; i < result; i++)
        {
            void *toSample = hitResults[i].sample;
            float weight = factor * hitResults[i].weight;
            /* The current sample owns its incoming-source list.  Retail
             * 0x406F90 appends the packed hit sample to currentSample->vars;
             * the previous port accidentally reversed these arguments. */
            AllocLightingTransfer(sample, toSample, dirIdx, weight);
        }
        return 4;
    }

    return 0;
}

/*
================
FindLightingTransfers_inner

Per-subsample lighting transfer loop. For each light direction,
calls FindLightingTransfersForDirection. Then calls GatherSkyLighting
for sky contribution. Finally calls GatherPointLightForSample for
each point light.
================
*/
int FindLightingTransfers_inner(int sampleIdx, float *position, float *normal,
                                float subAreaFactor, float skyFactor,
                                unsigned char primaryLightIndex,
                                SubSample_t *subSample)
{
    int i;
    int hasUsefulDirection = 0;
    int hasClassOne = 0;
    Sample_t *sample = *(Sample_t **)subSample;

    if (g_lightingTransportPass)
    {
        if (!g_relightCacheLoaded)
        {
            /* Native transport dispatcher 0x40C700. */
            for (i = 0; i < g_traces; i++)
            {
                int traceClass = FindLightingTransfersForDirection(
                    sampleIdx, sample, position, normal, subAreaFactor, i);
                if (traceClass == 1)
                    hasClassOne = 1;
                else if (traceClass > 2)
                    hasUsefulDirection = 1;
            }

            /* Native 0x4083A0 returns before setting SampleVars::validMask or
             * gathering direct lights when the hemisphere has neither sky nor
             * a usable lightmapped neighbour. */
            if (!hasUsefulDirection)
                return 0;

            if (hasClassOne)
            {
                float probe[3];
                probe[0] = position[0] + normal[6] * 0.1f;
                probe[1] = position[1] + normal[7] * 0.1f;
                probe[2] = position[2] + normal[8] * 0.1f;
                if (Lighting_RejectTransportPoint(sampleIdx, probe))
                    return 0;
            }
        }

        AcquireThreadLock((unsigned int)(uintptr_t)sample);
        sample->vars->validMask |= (unsigned char)(1u <<
            (subSample->s + 2 * subSample->t));
        ReleaseThreadLock((unsigned int)(uintptr_t)sample);
    }

    if (g_lightingTransportPass)
    {
        GatherSkyLighting(sampleIdx, position, normal, skyFactor,
                          subAreaFactor, primaryLightIndex, subSample);
        for (i = 2; i < g_totalLightCount; i++)
            GatherPointLightForSample(sampleIdx, i, position, normal,
                                       subAreaFactor, primaryLightIndex,
                                       subSample);
    }
    return 1;
}
