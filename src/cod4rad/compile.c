/*
 * compile.c — Radiosity compilation: lighting transfer, bounce, gather.
 */

#include "cod2rad64.h"
#include "zlib/zlib.h"
#include <stdlib.h>
#include <math.h>
#include <xmmintrin.h> /* KIWI: _mm_prefetch (Embree MT transport) */

/*
 * Tetrahedral SH basis constants for 4-band directional lighting.
 * These encode the 4 basis directions of a regular tetrahedron.
 */
static const float k_sqrt2_3  = 0.8164965510f;  /* sqrt(2/3) */
static const float k_inv_sqrt3 = 0.5773502588f;  /* 1/sqrt(3) */
static const float k_inv_sqrt2 = 0.7071067691f;  /* 1/sqrt(2) */
static const float k_inv_sqrt6 = 0.4082482755f;  /* 1/sqrt(6) */

static const float k_skyRayLength = 262144.0f;
static const float k_smallEpsilon = 0.001f;
static const float k_randScale = 0.000061037019f;  /* approx 2.0/32767.0 */
static const float k_largeRadius = 0.999f;
static const float k_initialMaxEnergy = 3.402823e+38f; /* FLT_MAX */
static const float k_divergeThreshold = 2.0f;

/* KIWI: Embree MT transport - one hemisphere ray traced and classified ahead
 * by a worker (FindLightingSamplesAndNormalMode up to the sample weight
 * test, which reads state the ordered pass mutates). */
typedef struct KiwiXferRay_s {
    unsigned char kind;            /* 0 none, 1/2 opposing face, 3 sky, 4 lightmap */
    unsigned char count;           /* kind 4: candidate samples */
    LightmapSample_t *sample[4];   /* kind 4: candidates in the serial order */
    float weight[4];               /* kind 4: their raw filter weights */
} KiwiXferRay_t;

int g_kiwiXferActive;                           /* KIWI: replay running (main thread) */
static uint32_t g_kiwiXferRng;                  /* KIWI: CRT rand() state while active */
static const unsigned char *g_kiwiXferCellRays; /* KIWI: current cell's records, NULL = live */
/* KIWI: the default point filter stores a ray as one word, sample | kind
 * (samples are 16-byte aligned); -TraceFilterLinear keeps KiwiXferRay_t. */
static int g_kiwiXferPacked;
static size_t g_kiwiXferStride;                 /* bytes per stored ray */
static int g_kiwiXferCellSky = -1;              /* KIWI: cell's sun ray result, -1 = trace */

/* KIWI: the MSVC CRT rand() step (per-thread state, srand sets it). */
static __forceinline int KiwiXfer_Rand(uint32_t *state)
{
    *state = *state * 214013u + 2531011u;
    return (int)((*state >> 16) & 0x7fff);
}

/*
================
GatherSurfaceIncidentEnergyForLightFromDir

Computes 4 tetrahedral SH basis dot products against a direction
vector. For each basis where the dot product is positive, accumulates
(dot * lightColor) into the corresponding output band (3 floats each).

Parameters:
  lightColor — float[3] RGB light color
  direction  — float[3] unit direction vector
  output     — float[12] accumulator, 4 bands × 3 channels

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

/* global transfer block pool */
static void *g_transferPool;
static int g_transferPoolIdx;

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

/* per-thread max bounced energy, dynamically sized */
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

static char s_assertDisable_Alloc_fromSample;
static char s_assertDisable_Alloc_vars;
static char s_assertDisable_Alloc_weight;
static char s_assertDisable_Alloc_block;

static const float k_weightEpsilon = -1.0e-5f;

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

/* KIWI: Embree MT replay - exact (fromSample, toSample, lightIdx) index.
 * A sample is indexed whole (its chain scanned once per index epoch), so an
 * index miss means "not in the chain": the O(chain) walk for every new key
 * goes away.  Same entries, same order, same sums as the chain walk. */
typedef struct KiwiXferIndexSlot_s {
    Sample_t *from;
    void *to;
    int lightIdx;
    TransferEntry_t *entry;
} KiwiXferIndexSlot_t;

#define KIWI_XFER_INDEX_BITS 18
#define KIWI_XFER_INDEX_SIZE (1u << KIWI_XFER_INDEX_BITS)
#define KIWI_XFER_INDEX_MAX  (KIWI_XFER_INDEX_SIZE / 2)

static KiwiXferIndexSlot_t *g_kxIndex;
static uint32_t g_kxIndexUsed;
static uint32_t g_kxIndexEpoch;
static uint32_t *g_kxSampleEpoch;   /* per lightmap sample: epoch it was indexed in */

static __forceinline uint32_t KiwiXfer_IndexHash(const void *from, const void *to,
                                                 int lightIdx)
{
    uint64_t h = (uint64_t)(uintptr_t)from * 0x9E3779B97F4A7C15ull
               ^ (uint64_t)(uintptr_t)to * 0xC2B2AE3D27D4EB4Full
               ^ (uint64_t)(uint32_t)lightIdx * 0x165667B19E3779F9ull;
    h ^= h >> 32;
    h *= 0xD6E8FEB86659FD93ull;
    return (uint32_t)(h >> (64 - KIWI_XFER_INDEX_BITS));
}

static void KiwiXfer_IndexClear(void)
{
    memset(g_kxIndex, 0, KIWI_XFER_INDEX_SIZE * sizeof(*g_kxIndex));
    g_kxIndexUsed = 0;
    g_kxIndexEpoch++;
}

static void KiwiXfer_IndexInsert(Sample_t *from, TransferEntry_t *entry)
{
    uint32_t i = KiwiXfer_IndexHash(from, entry->toSample, entry->lightIdx);

    while (g_kxIndex[i].from)
        i = (i + 1) & (KIWI_XFER_INDEX_SIZE - 1);
    g_kxIndex[i].from = from;
    g_kxIndex[i].to = entry->toSample;
    g_kxIndex[i].lightIdx = entry->lightIdx;
    g_kxIndex[i].entry = entry;
    g_kxIndexUsed++;
}

/* Index from's whole chain for this epoch; 0 = too long, walk the chain. */
static int KiwiXfer_IndexSample(Sample_t *from)
{
    long long si = (LightmapSample_t *)from - (LightmapSample_t *)g_lightingSamples;
    TransferBlock_t *cur;
    uint32_t n = 0;
    int i;

    if (g_kxSampleEpoch[si] == g_kxIndexEpoch)
        return 1;
    for (cur = from->vars->transferHead; cur; cur = (TransferBlock_t *)cur->next)
    {
        for (i = 0; i < TRANSFERS_PER_BLOCK && cur->entries[i].toSample; i++)
            n++;
    }
    if (n + 1 > KIWI_XFER_INDEX_MAX)
        return 0;
    if (g_kxIndexUsed + n + 1 > KIWI_XFER_INDEX_MAX)
        KiwiXfer_IndexClear();
    for (cur = from->vars->transferHead; cur; cur = (TransferBlock_t *)cur->next)
    {
        for (i = 0; i < TRANSFERS_PER_BLOCK && cur->entries[i].toSample; i++)
            KiwiXfer_IndexInsert(from, &cur->entries[i]);
    }
    g_kxSampleEpoch[si] = g_kxIndexEpoch;
    return 1;
}

static TransferEntry_t *KiwiXfer_IndexFind(Sample_t *from, void *to, int lightIdx)
{
    uint32_t i = KiwiXfer_IndexHash(from, to, lightIdx);

    while (g_kxIndex[i].from)
    {
        if (g_kxIndex[i].from == from && g_kxIndex[i].to == to
            && g_kxIndex[i].lightIdx == lightIdx)
            return g_kxIndex[i].entry;
        i = (i + 1) & (KIWI_XFER_INDEX_SIZE - 1);
    }
    return NULL;
}

#define KIWI_XFER_PREFETCH_SLOT  6   /* rays ahead: index slot + hit sample */

/* KIWI: Embree MT replay - warm the lines the merge touches for a ray. */
static __forceinline void KiwiXfer_PrefetchRay(Sample_t *from, const unsigned char *rec,
                                               int dirIdx)
{
    const KiwiXferRay_t *ray = (const KiwiXferRay_t *)rec;
    LightmapSample_t *packedSample = NULL;
    int count, c;

    if (g_kiwiXferPacked)
    {
        uintptr_t word = *(const uintptr_t *)rec;
        if ((word & 15) != 4)
            return;
        packedSample = (LightmapSample_t *)(word & ~(uintptr_t)15);
        count = 1;
    }
    else
    {
        if (ray->kind != 4)
            return;
        count = ray->count;
    }
    for (c = 0; c < count; c++)
    {
        LightmapSample_t *to = packedSample ? packedSample : ray->sample[c];

        _mm_prefetch((const char *)&g_kxIndex[KiwiXfer_IndexHash(from, to, dirIdx)],
                     _MM_HINT_T0);
        _mm_prefetch((const char *)&to->weight, _MM_HINT_T0);
    }
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

    /* KIWI: Embree MT replay - exact index instead of the chain walk.  The
     * add is the same IEEE sum (old + new == new + old). */
    if (g_kiwiXferActive && KiwiXfer_IndexSample(fromSample))
    {
        entry = KiwiXfer_IndexFind(fromSample, toSample, lightIdx);
        if (entry)
        {
            entry->weight += weight;
            return;
        }
        slot = NULL;
        AcquireThreadLock((unsigned int)(uintptr_t)fromSample);
        block = vars->transferHead;
        goto find_free;
    }

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

find_free:
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

    if (!slot)
    {
        /* KIWI: Embree MT replay index (a full index restarts the epoch) */
        if (g_kxIndexUsed + 1 > KIWI_XFER_INDEX_MAX)
            KiwiXfer_IndexClear();
        else
            KiwiXfer_IndexInsert(fromSample, entry);
    }
    else
    {
        slot->fromSample = fromSample;
        slot->toSample = toSample;
        slot->lightIdx = lightIdx;
        slot->entry = entry;
    }

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

static char s_assertDisable_Normalize_sample;
static char s_assertDisable_Normalize_vars;

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

/* g_vertexData — view into g_vertData[0].lmCoord (stride sizeof(DrawVert_t)=0x44). */
unsigned char *g_vertexData = (unsigned char *)&g_vertData[0].lmCoord;

/* KIWI: Embree MT transport - the state-dependent tail of
 * FindLightingSamplesAndNormalMode for a worker-classified ray: the
 * sample->weight tests and the filter normalization, in the same order. */
static int KiwiXfer_ResolveRay(const void *rec, void *outputLighting,
                               int nearestSample, int *outTraceClass)
{
    const KiwiXferRay_t *ray = (const KiwiXferRay_t *)rec;
    LightmapSample_t *sample0;
    int kind;

    if (g_kiwiXferPacked)
    {
        uintptr_t word = *(const uintptr_t *)rec;
        kind = (int)(word & 15);
        sample0 = (LightmapSample_t *)(word & ~(uintptr_t)15);
    }
    else
    {
        kind = ray->kind;
        sample0 = ray->sample[0];
    }

    if (kind == 0)
        return 0;
    if (kind < 3)
    {
        if (outTraceClass)
            *outTraceClass = kind;
        return 0;
    }
    if (kind == 3)
    {
        if (outTraceClass)
            *outTraceClass = 3;
        return -1;
    }

    if (nearestSample)
    {
        LightingHit_t *hit = (LightingHit_t *)outputLighting;
        LightmapSample_t *sample = sample0;

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
        float totalWeight = 0.0f;
        int hitCount = 0;
        int c;

        for (c = 0; c < ray->count; c++)
        {
            LightmapSample_t *sample = ray->sample[c];
            float weight = ray->weight[c];

            if (!sample || sample->weight <= 0.0f)
                continue;
            hits[hitCount].sample = (LightingSample_t *)sample;
            hits[hitCount].weight = weight;
            hits[hitCount]._pad = 0;
            totalWeight += weight;
            hitCount++;
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

/*
================
FindLightingSamplesAndNormal

Traces a ray along the normal from a surface point, finds the
triangle hit, interpolates barycentric UV coordinates from vertex
lighting data, and samples the lightmap. Returns 1 on success,
0 if no hit or invalid triangle, -1 on backface.

outputNormal is an optional float[3].
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
    float smallOffset = 0.125f;
    Triangle_t *tri;
    MaterialDef_t *material;
    float baryU, baryV, baryW;
    float u, v;
    float uvScale = 512.0f;
    int vertIdx0, vertIdx1, vertIdx2;

    if (outTraceClass)
        *outTraceClass = 0;

    /* compute start = position + normal * smallOffset */
    startPos[0] = normal[0] * smallOffset + position[0];
    startPos[1] = normal[1] * smallOffset + position[1];
    startPos[2] = normal[2] * smallOffset + position[2];

    /* compute end = start + normal * offset */
    endPos[0] = normal[0] * offset + startPos[0];
    endPos[1] = normal[1] * offset + startPos[1];
    endPos[2] = normal[2] * offset + startPos[2];

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
        /* KIWI: texel s's centre is s + 0.5 - interpolate from there (the floor of u itself
         * blended a centre sample 50/50 with its right neighbour: a half-texel shift). */
        int baseS = (int)floorf(u - 0.5f);
        int baseT = (int)floorf(v - 0.5f);
        float fracS = (u - 0.5f) - (float)baseS;
        float fracT = (v - 0.5f) - (float)baseT;
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


static char s_assertDisable_Sky_subSample;
static char s_assertDisable_Sky_sample;
static char s_assertDisable_Sky_vars;
static char s_assertDisable_Sky_area;
static char s_assertDisable_Sky_nanEnergy;
static char s_assertDisable_Sky_nanResult;

/*
================
GatherSkyLighting

Gathers sky illumination for a lighting subsample. Traces a ray
from the surface point along the normal direction to detect sky
visibility. If the sky is visible (backface material hit), accumulates
weighted sky color into the sample's lighting data.

Uses the global sun direction for the sky direction basis coefficients
and the global sun color for the sky color.
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

    /* KIWI: Embree MT replay - a worker traced this cell's sun ray */
    if (g_kiwiXferCellSky >= 0)
    {
        if (!g_kiwiXferCellSky)
            return;
        goto sun_visible;
    }

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

sun_visible:

    /* Native 0x407770 derives a fractional sun visibility from a jittered
     * multi-sample hit mask (popcount/sampleCount, 0x409B60), where alpha-
     * masked materials subtract individual sub-rays and a flagged static-model
     * final hit yields 0.01.  The single-ray trace above is the binary case of
     * that (this trace set has no alpha-masked occluders yet): 1 when the ray
     * reaches sky. */
    visibility = 1.0f;

    /* CoD4 keeps direct sunlight and the energy injected into radiosity as
     * separate colors.  Both scale by visibility * skyWeight (0x407770). */
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
extern void Lighting_InitSamples(void);
extern void BeginProgress(const char *msg);
extern void EndProgress(void);
extern void CalculateGroundLightingForAllStaticModels(void);
/* malloc/free from <stdlib.h> */
extern void *g_lightDirArray;
extern int g_totalLightCount;

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
     * order on one thread (the tracing runs inside the same pass). */
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
GatherPointLightForSample

Gathers direct illumination from a single point light for a
lighting subsample. Traces shadow rays and accumulates weighted
light contribution into the sample's SH bands.
================
*/
extern int PointLightEvaluatePoint(int surfacePrimaryLightIndex, int sampleIdx,
    int lightIdx, float *position, float *normal, float *outDir,
    float *outColor, float *outArg);

static char s_assertDisable_Point_sample;
static char s_assertDisable_Point_vars;
static char s_assertDisable_Point_nanEnergy;
static char s_assertDisable_Point_nanResult;

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

    /* returns 0 (no light), 1 (primary-light intensity), 2 (directional), 3 (coincident);
     * basis+6 is the surface normal (3rd row of basis) */
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
            dist *= 0.25f;

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
GatherBounceForSample

Gathers indirect (bounced) illumination for a single sample.
Iterates the transfer chain and accumulates weighted contributions
from neighboring samples' scattered light.
================
*/
static char s_assertDisable_Bounce_source;
static char s_assertDisable_Bounce_sourceVars;
static char s_assertDisable_Bounce_sampleVars;
static char s_assertDisable_Bounce_nanSource;
static char s_assertDisable_Bounce_nanBounced;

extern float g_energyScale;

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
     * 0x407C50. */
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

Traces rays in a specific direction from a sample to find lighting
transfers to neighboring samples. Computes transfer weights based
on visibility and distance.

1367 bytes — the largest function in compile.cpp. Contains ray
tracing, transfer allocation, weight computation, and NaN checks.
================
*/
extern void Vec2Normalize(float *v);

static char s_assertDisable_Dir_areaFactor;
static char s_assertDisable_Dir_sampleArea;
static char s_assertDisable_Dir_vars;
static char s_assertDisable_Dir_incident;
static char s_assertDisable_Dir_nanEnergy;
static char s_assertDisable_Dir_nanResult;

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
    SampleVars_t *sampleVars;
    int result;
    int traceClass;
    float factor;
    int i;

    dirEntry = (float *)&((LightDirEntry_t *)g_lightDirArray)[dirIdx];
    randScale = k_randScale; /* rand scale constant */

    /* generate random point on unit disk via rejection sampling */
    {
        int rej = 0;
        do {
            extern int rand_int(void);
            /* KIWI: replay walks the CRT stream itself (same values) */
            if (g_kiwiXferActive)
            {
                rx = (float)KiwiXfer_Rand(&g_kiwiXferRng) * randScale - 1.0f;
                ry = (float)KiwiXfer_Rand(&g_kiwiXferRng) * randScale - 1.0f;
            }
            else
            {
                rx = (float)rand_int() * randScale - 1.0f;
                ry = (float)rand_int() * randScale - 1.0f;
            }
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
    /* >= rather than >.  Transport rejects subtract min(cellArea,
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
            return 4; /* KIWI: was a bare return; the caller reads the class */
        /* hit neighbor samples — allocate transfers */
        for (i = 0; i < result; i++)
        {
            void *toSample = hitResults[i].sample;
            float weight = factor * hitResults[i].weight;
            /* The current sample owns its incoming-source list.  Retail
             * 0x406F90 appends the packed hit sample to currentSample->vars. */
            AllocLightingTransfer(sample, toSample, dirIdx, weight);
        }
        return 4;
    }

    return 0;
}

/* KIWI: Embree MT replay - FindLightingTransfersForDirection over all
 * g_traces rays of a worker-traced cell in one loop (same order, same
 * float math; the per-ray call chain was a third of the merge thread). */
static void KiwiXfer_ReplayDirections(Sample_t *sample, float subAreaFactor,
                                      int *hasClassOne, int *hasUsefulDirection)
{
    const unsigned char *rec = g_kiwiXferCellRays;
    SampleVars_t *sampleVars = sample->vars;
    int nearestSample = g_traceFilter == 1;
    int dirIdx;

    for (dirIdx = 0; dirIdx < g_traces; dirIdx++, rec += g_kiwiXferStride)
    {
        LightingHit_t hitResults[4];
        int traceClass = 0;
        int result, i;
        float factor;

        if (dirIdx + KIWI_XFER_PREFETCH_SLOT < g_traces)
            KiwiXfer_PrefetchRay(sample, rec + KIWI_XFER_PREFETCH_SLOT * g_kiwiXferStride,
                                 dirIdx + KIWI_XFER_PREFETCH_SLOT);

        result = KiwiXfer_ResolveRay(rec, hitResults, nearestSample, &traceClass);
        if (traceClass <= 2)
        {
            if (traceClass == 1)
                *hasClassOne = 1;
            continue;
        }

        *hasUsefulDirection = 1;
        factor = g_invTraces * subAreaFactor;

        if (traceClass == 3)
        {
            if (!sampleVars->skyInfluences)
                sampleVars->skyInfluences = (float *)calloc((size_t)g_traces,
                                                            sizeof(float));
            if (!sampleVars->skyInfluences)
                Error("Out of memory allocating sky influences\n");
            sampleVars->skyInfluences[dirIdx] += factor;
            continue;
        }

        for (i = 0; i < result; i++)
        {
            void *toSample = hitResults[i].sample;
            float weight = factor * hitResults[i].weight;
            AllocLightingTransfer(sample, toSample, dirIdx, weight);
        }
    }
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
            if (g_kiwiXferCellRays)   /* KIWI: Embree MT replay, same result */
                KiwiXfer_ReplayDirections(sample, subAreaFactor,
                                          &hasClassOne, &hasUsefulDirection);
            else
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

/*
================
KIWI: Embree multithreaded light transport

"Building light transport for everything" stays one ordered pass on the
main thread (sample chains, drain and float sums are order dependent).
With Embree on, worker threads trace and classify every cell's hemisphere
rays and its sun ray AHEAD of that pass; the pass then runs unchanged and
takes each cell's results instead of tracing (KiwiXfer_ReplayDirections,
GatherSkyLighting).  Only the tests that read state the pass mutates (hit
sample weights, the area gate) stay in the pass.

The rays depend on the CRT rand() stream, so the plan replays it: the main
thread's rand state is recovered, a positioner thread walks the same LCG
(g_traces disk points per cell that passes the area gate at pass start)
and cuts each work item into trace tasks with their start states.  The
pass uses a cell's results only when its area index, RNG state, position
and basis all match exactly; otherwise it traces live and the plan
restarts after that item (a drained gate; ~10 times per map).  The result
is byte-identical to the one-thread pass.  -Threads 1, relight loads and
AO keep the original path.
================
*/
#define KIWI_XFER_MAX_THREADS 64
#define KIWI_XFER_MAX_AHEAD   4096                  /* items traced ahead */
#define KIWI_XFER_BYTE_BUDGET ((size_t)512 << 20)   /* hit memory ahead */

extern int g_useEmbree;          /* embree_trace.cpp */
extern int g_kiwiEmbreeThreads;  /* cmdline.c */
extern void BuildLightTransfers_Callback(unsigned int triIndex, unsigned int unused);

typedef struct KiwiXferCell_s {
    int areaIndex;
    uint32_t rngStart;
    uint32_t rngEnd;
    float pos[3];
    float basis[9];
    int sky;            /* GatherSkyLighting's sun ray reached the sky */
} KiwiXferCell_t;

typedef struct KiwiXferItem_s {
    int stage;          /* 0 free, 1 claimed, 2 rastered, 3 traced */
    int rngKnown;
    uint32_t rngStart;
    int cellCount;
    KiwiXferCell_t *cells;
    unsigned char *rays;            /* cellCount * traces records, g_kiwiXferStride each */
    size_t bytes;
    int chunkCount;                 /* trace tasks queued by the positioner */
    int chunksDone;
} KiwiXferItem_t;

/* One trace task: a run of an item's cells with its RNG start state. */
#define KIWI_XFER_CHUNK_CELLS 128
typedef struct KiwiXferTask_s {
    int item;
    int cellStart;
    int cellEnd;
    uint32_t rng;
} KiwiXferTask_t;

typedef struct KiwiXferRaster_s {
    LightTransferMapping_t mapping;
    KiwiXferCell_t *cells;
    int count;
    int capacity;
} KiwiXferRaster_t;

static struct {
    SRWLOCK lock;
    CONDITION_VARIABLE cvRaster;   /* positioner: an item was rastered */
    CONDITION_VARIABLE cvWork;     /* tracers: tasks queued or the merge advanced */
    CONDITION_VARIABLE cvTraced;   /* merge: an item is traced */
    KiwiXferItem_t *items;
    int itemCount;
    int nextClaim;
    int merged;
    KiwiXferTask_t *tasks;         /* FIFO, in work order */
    int taskHead;
    int taskTail;
    int taskCap;
    int positionerDone;
    size_t outstanding;
    volatile LONG abort;
    float *snap;                   /* channelWeight[4] per sample at pass start */
    int traces;
    int tracerCount;
    HANDLE threads[KIWI_XFER_MAX_THREADS + 1];
    int threadCount;
    /* merge side (main thread) */
    KiwiXferItem_t *curItem;
    int cursor;
    int desync;
    long long cellsReplayed;
    long long cellsLive;
    int restarts;
    long long waitTicks;           /* merge stalled on the tracers */
    void **garbage;                /* merged buffers, freed by the tracers */
    int garbageCount;
    int garbageCap;
} g_kx;

/* All plan memory lives in a private heap: the CRT heap then sees exactly
 * the serial pass's allocations, so the transfer pool stays as compact as
 * before (interleaving it with plan buffers made the later chain walks of
 * "Building final lightmaps" ~2x slower). */
static HANDLE g_kxHeap;

static void *KiwiXfer_Alloc(size_t size, int zero)
{
    void *p = HeapAlloc(g_kxHeap, zero ? HEAP_ZERO_MEMORY : 0, size ? size : 1);
    if (!p)
        ErrorMsg("Out of memory for traced-ahead light transport\n");
    return p;
}

static void *KiwiXfer_Realloc(void *p, size_t size)
{
    void *q = p ? HeapReAlloc(g_kxHeap, 0, p, size) : HeapAlloc(g_kxHeap, 0, size);
    if (!q)
        ErrorMsg("Out of memory for traced-ahead light transport\n");
    return q;
}

static void KiwiXfer_Free(void *p)
{
    if (p)
        HeapFree(g_kxHeap, 0, p);
}

/* g_kx.lock held. */
static void KiwiXfer_AddGarbage(void *p)
{
    if (!p)
        return;
    if (g_kx.garbageCount == g_kx.garbageCap)
    {
        g_kx.garbageCap = g_kx.garbageCap ? g_kx.garbageCap * 2 : 256;
        g_kx.garbage = (void **)KiwiXfer_Realloc(g_kx.garbage,
            (size_t)g_kx.garbageCap * sizeof(void *));
    }
    g_kx.garbage[g_kx.garbageCount++] = p;
}

/* Frees a list taken from g_kx.garbage (lock not held). */
static void KiwiXfer_FreeGarbage(void **list, int count)
{
    int i;

    for (i = 0; i < count; i++)
        KiwiXfer_Free(list[i]);
    KiwiXfer_Free(list);
}

static float KiwiXfer_ClampArea(float areaX2, float weight)
{
    /* geometry.c ClampAreaToWeight */
    float val = (weight - areaX2 < 0.0f) ? weight : areaX2;
    return (0.0f - val < 0.0f) ? val : 0.0f;
}

/* Same rejection loop and float math as FindLightingTransfersForDirection. */
static __forceinline void KiwiXfer_Disk(uint32_t *rng, float *outX, float *outY)
{
    float randScale = k_randScale;
    float rx, ry, lenSq;

    do {
        rx = (float)KiwiXfer_Rand(rng) * randScale - 1.0f;
        ry = (float)KiwiXfer_Rand(rng) * randScale - 1.0f;
        lenSq = rx * rx + ry * ry;
    } while (lenSq > 1.0f);
    *outX = rx;
    *outY = ry;
}

/* FindLightingSamplesAndNormalMode's opposing-upward probe (1 = sky below). */
static int KiwiXfer_ProbeClass(Triangle_t *tri)
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
    TraceSetup_and_Dispatch(0, probe, end, &verticalHit);
    if (!verticalHit.triangle)
        return 1;
    verticalMaterial = verticalHit.triangle->material;
    if ((verticalMaterial->surfaceFlags & SURF_SKY)
     || (verticalMaterial->contents == 1
      && (verticalMaterial->surfaceFlags & 0x80)))
        return 1;
    return 2;
}

/* FindLightingSamplesAndNormalMode after its trace, minus the weight tests
 * (outputNormal is NULL in transport, so no static-model trace). */
static void KiwiXfer_ClassifyHit(const RayHitResult_t *hitResult,
                                 const float *normal, int nearestSample,
                                 KiwiXferRay_t *ray)
{
    Triangle_t *tri = hitResult->triangle;
    MaterialDef_t *material;
    float baryU, baryV, baryW;
    float u, v;
    float uvScale = 512.0f;
    int vertIdx0, vertIdx1, vertIdx2;

    ray->kind = 0;
    ray->count = 0;
    if (!tri)
        return;

    material = tri->material;
    if (material->surfaceFlags & SURF_SKY)
    {
        if (normal[2] < 0.0f)
            return;
        ray->kind = 3;
        return;
    }

    if (DotProduct(tri->normal, normal) >= 0.0f)
    {
        ray->kind = 2;
        if (tri->normal[2] >= 0.5f)
            ray->kind = (unsigned char)KiwiXfer_ProbeClass(tri);
        return;
    }

    if (tri->lightmapIdx == LIGHTMAP_NONE)
        return;

    baryU = hitResult->baryU;
    baryV = hitResult->baryV;
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

    ray->kind = 4;
    if (nearestSample)
    {
        LightmapSample_t *sample;
        int s = (int)floorf(u);
        int t = (int)floorf(v);

        if (s < 0) s = 0;
        else if (s > 511) s = 511;
        if (t < 0) t = 0;
        else if (t > 511) t = 511;

        GetLightingSample((int)tri->lightmapIdx, (float)s, (float)t,
                          (void **)&sample);
        ray->sample[0] = sample;
        ray->weight[0] = 1.0f;
        ray->count = 1;
        return;
    }

    {
        int baseS = (int)floorf(u - 0.5f);
        int baseT = (int)floorf(v - 0.5f);
        float fracS = (u - 0.5f) - (float)baseS;
        float fracT = (v - 0.5f) - (float)baseT;
        float sWeights[2] = { 1.0f - fracS, fracS };
        float tWeights[2] = { 1.0f - fracT, fracT };
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
                ray->sample[ray->count] = sample;
                ray->weight[ray->count] = weight;
                ray->count++;
            }
        }
    }
}

/* GatherSkyLighting's sun visibility test for a cell (no state read). */
static int KiwiXfer_SunVisible(float *position, float *basis)
{
    float startPos[3], endPos[3];
    float smallOffset = 0.125f;
    float rayLen = k_skyRayLength;
    float skyDir;
    RayHitResult_t hitResult;

    skyDir = g_sunDirY * basis[7] + g_sunDirX * basis[6] + g_sunDirZ * basis[8];
    if (skyDir <= 0.0f)
        return 0;

    startPos[0] = basis[6] * smallOffset + position[0];
    startPos[1] = basis[7] * smallOffset + position[1];
    startPos[2] = basis[8] * smallOffset + position[2];

    endPos[0] = g_sunDirX * rayLen + startPos[0];
    endPos[1] = g_sunDirY * rayLen + startPos[1];
    endPos[2] = g_sunDirZ * rayLen + startPos[2];

    TraceSetup_and_Dispatch(0, startPos, endPos, &hitResult);
    if (!hitResult.triangle)
        return 0;
    if (!(hitResult.triangle->material->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY)))
        return 0;
    if (TraceStaticModels(startPos, endPos))
        return 0;
    return 1;
}

/* Trace one cell's g_traces rays exactly as the serial pass would. */
static void KiwiXfer_TraceCell(uint32_t *rng, float *position, float *normal,
                               unsigned char *out)
{
    int nearestSample = g_traceFilter == 1;
    int dirIdx;

    for (dirIdx = 0; dirIdx < g_traces; dirIdx++)
    {
        float *dirEntry = (float *)&((LightDirEntry_t *)g_lightDirArray)[dirIdx];
        unsigned char *rec = out + (size_t)dirIdx * g_kiwiXferStride;
        KiwiXferRay_t ray;
        float rx, ry, lenSq;
        float pertDir[3];
        float worldDir[3];
        float startPos[3], endPos[3];
        float smallOffset = 0.125f;
        float offset = k_skyRayLength;
        RayHitResult_t hit;

        KiwiXfer_Disk(rng, &rx, &ry);
        {
            float radius = dirEntry[3];
            pertDir[0] = dirEntry[0] + radius * rx;
            pertDir[1] = dirEntry[1] + radius * ry;

            lenSq = pertDir[0] * pertDir[0] + pertDir[1] * pertDir[1];
            if (lenSq <= 1.0f)
            {
                pertDir[2] = sqrtf(1.0f - lenSq);
            }
            else
            {
                Vec2Normalize(pertDir);
                pertDir[2] = k_smallEpsilon;
            }
        }

        worldDir[0] = pertDir[0] * normal[0] + pertDir[1] * normal[3] + pertDir[2] * normal[6];
        worldDir[1] = pertDir[0] * normal[1] + pertDir[1] * normal[4] + pertDir[2] * normal[7];
        worldDir[2] = pertDir[0] * normal[2] + pertDir[1] * normal[5] + pertDir[2] * normal[8];

        startPos[0] = worldDir[0] * smallOffset + position[0];
        startPos[1] = worldDir[1] * smallOffset + position[1];
        startPos[2] = worldDir[2] * smallOffset + position[2];
        endPos[0] = worldDir[0] * offset + startPos[0];
        endPos[1] = worldDir[1] * offset + startPos[1];
        endPos[2] = worldDir[2] * offset + startPos[2];

        TraceSetup_and_Dispatch(0, startPos, endPos, &hit);
        KiwiXfer_ClassifyHit(&hit, worldDir, nearestSample, &ray);
        if (g_kiwiXferPacked)
            *(uintptr_t *)rec = (ray.kind == 4 ? (uintptr_t)ray.sample[0] : 0) | ray.kind;
        else
            memcpy(rec, &ray, sizeof(ray));
    }
}

/* ProcessLightingSampleArea up to GramSchmidt, gated on the pass-start weights. */
static void KiwiXfer_RasterCell(float areaX2, float *centroid,
                                float *polyVerts, int vertCount,
                                void *userData, int areaIndex)
{
    KiwiXferRaster_t *r = (KiwiXferRaster_t *)userData;
    LightTransferMapping_t *mapping = &r->mapping;
    SubSample_t sub;
    long long pixel;
    KiwiXferCell_t *cell;
    float *basis;
    float u, v;

    (void)polyVerts;
    (void)vertCount;

    GetLightingSubSample(mapping->lightmapIdx,
                         centroid[0] * 2.0f, centroid[1] * 2.0f, &sub);
    pixel = (LightmapSample_t *)sub.sample - (LightmapSample_t *)g_lightingSamples;
    if (KiwiXfer_ClampArea(areaX2, g_kx.snap[pixel * 4 + sub.s + sub.t * 2]) == 0.0f)
        return;

    if (r->count == r->capacity)
    {
        int newCapacity = r->capacity ? r->capacity * 2 : 64;
        KiwiXferCell_t *newCells = (KiwiXferCell_t *)KiwiXfer_Realloc(r->cells,
            (size_t)newCapacity * sizeof(*newCells));
        r->cells = newCells;
        r->capacity = newCapacity;
    }
    cell = &r->cells[r->count++];
    cell->areaIndex = areaIndex;

    u = centroid[0];
    v = centroid[1];

    cell->pos[0] = mapping->posConst[0] + u * mapping->posUCoeff[0] + v * mapping->posVCoeff[0];
    cell->pos[1] = mapping->posConst[1] + u * mapping->posUCoeff[1] + v * mapping->posVCoeff[1];
    cell->pos[2] = mapping->posConst[2] + u * mapping->posUCoeff[2] + v * mapping->posVCoeff[2];

    basis = cell->basis;
    basis[0] = mapping->tanConst[0] + u * mapping->tanUCoeff[0] + v * mapping->tanVCoeff[0];
    basis[1] = mapping->tanConst[1] + u * mapping->tanUCoeff[1] + v * mapping->tanVCoeff[1];
    basis[2] = mapping->tanConst[2] + u * mapping->tanUCoeff[2] + v * mapping->tanVCoeff[2];

    basis[3] = mapping->binConst[0] + u * mapping->binUCoeff[0] + v * mapping->binVCoeff[0];
    basis[4] = mapping->binConst[1] + u * mapping->binUCoeff[1] + v * mapping->binVCoeff[1];
    basis[5] = mapping->binConst[2] + u * mapping->binUCoeff[2] + v * mapping->binVCoeff[2];

    basis[6] = mapping->normConst[0] + u * mapping->normUCoeff[0] + v * mapping->normVCoeff[0];
    basis[7] = mapping->normConst[1] + u * mapping->normUCoeff[1] + v * mapping->normVCoeff[1];
    basis[8] = mapping->normConst[2] + u * mapping->normUCoeff[2] + v * mapping->normVCoeff[2];

    GramSchmidt(basis);
}

/* BuildLightTransfers_PerTri's checks, then the cell list of one triangle. */
static void KiwiXfer_RasterItem(int triIndex, KiwiXferItem_t *item)
{
    Triangle_t *tri = &g_triangles[triIndex];
    KiwiXferRaster_t r;
    float *lm0, *lm1, *lm2;

    item->cells = NULL;
    item->cellCount = 0;

    if (tri->lightmapIdx == 0x1F)
        return;

    lm0 = g_vertData[tri->vertIndex[0]].lmCoord;
    lm1 = g_vertData[tri->vertIndex[1]].lmCoord;
    lm2 = g_vertData[tri->vertIndex[2]].lmCoord;
    if (lm0[0] == lm1[0] && lm0[1] == lm1[1])
        return;
    if (lm1[0] == lm2[0] && lm1[1] == lm2[1])
        return;
    if (lm2[0] == lm0[0] && lm2[1] == lm0[1])
        return;

    if (!ComputeLinearMappingForTriangle(tri, &r.mapping))
        return;
    r.mapping.param = 0;
    r.mapping.lightmapIdx = (int)tri->lightmapIdx;
    r.mapping.triangleIndex = triIndex;
    r.mapping.primaryLightIndex = tri->primaryLightIndex;
    r.cells = NULL;
    r.count = 0;
    r.capacity = 0;

    ForEachLightingSampleInTriangle(tri, g_superSample,
                                    (void *)KiwiXfer_RasterCell, &r);
    item->cells = r.cells;
    item->cellCount = r.count;
}

static DWORD WINAPI KiwiXfer_TracerThread(LPVOID param)
{
    (void)param;

    /* Trace tasks first (oldest item first, so the merge is fed), else
     * raster the next item while the window allows. */
    AcquireSRWLockExclusive(&g_kx.lock);
    for (;;)
    {
        KiwiXferItem_t *item;
        int j, c;

        if (g_kx.abort)
            break;

        if (g_kx.taskHead < g_kx.taskTail)
        {
            KiwiXferTask_t task = g_kx.tasks[g_kx.taskHead++];
            uint32_t rng = task.rng;

            item = &g_kx.items[task.item];
            ReleaseSRWLockExclusive(&g_kx.lock);

            for (c = task.cellStart; c < task.cellEnd && !g_kx.abort; c++)
            {
                KiwiXferCell_t *cell = &item->cells[c];
                cell->rngStart = rng;
                KiwiXfer_TraceCell(&rng, cell->pos, cell->basis,
                                   item->rays + (size_t)c * g_kx.traces * g_kiwiXferStride);
                cell->rngEnd = rng;
                cell->sky = KiwiXfer_SunVisible(cell->pos, cell->basis);
            }

            AcquireSRWLockExclusive(&g_kx.lock);
            if (g_kx.abort)
                break;
            if (++item->chunksDone == item->chunkCount)
            {
                item->stage = 3;
                WakeAllConditionVariable(&g_kx.cvTraced);
            }
            continue;
        }

        if (g_kx.nextClaim < g_kx.itemCount
            && g_kx.nextClaim - g_kx.merged < KIWI_XFER_MAX_AHEAD
            && g_kx.outstanding <= KIWI_XFER_BYTE_BUDGET)
        {
            void **garbage = g_kx.garbage;
            int garbageCount = g_kx.garbageCount;

            j = g_kx.nextClaim++;
            item = &g_kx.items[j];
            item->stage = 1;
            g_kx.garbage = NULL;
            g_kx.garbageCount = 0;
            g_kx.garbageCap = 0;
            ReleaseSRWLockExclusive(&g_kx.lock);

            KiwiXfer_FreeGarbage(garbage, garbageCount);
            KiwiXfer_RasterItem(j, item);

            AcquireSRWLockExclusive(&g_kx.lock);
            item->bytes = (size_t)item->cellCount
                        * (sizeof(KiwiXferCell_t) + (size_t)g_kx.traces * g_kiwiXferStride);
            g_kx.outstanding += item->bytes;
            item->stage = 2;
            WakeAllConditionVariable(&g_kx.cvRaster);
            continue;
        }

        if (g_kx.nextClaim >= g_kx.itemCount && g_kx.positionerDone)
            break;
        SleepConditionVariableSRW(&g_kx.cvWork, &g_kx.lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&g_kx.lock);
    return 0;
}

/* g_kx.lock held. */
static void KiwiXfer_PushTask(int item, int cellStart, int cellEnd, uint32_t rng)
{
    KiwiXferTask_t *task;

    if (g_kx.taskTail == g_kx.taskCap)
    {
        if (g_kx.taskHead > 0)
        {
            memmove(g_kx.tasks, g_kx.tasks + g_kx.taskHead,
                    (size_t)(g_kx.taskTail - g_kx.taskHead) * sizeof(*g_kx.tasks));
            g_kx.taskTail -= g_kx.taskHead;
            g_kx.taskHead = 0;
        }
        if (g_kx.taskTail == g_kx.taskCap)
        {
            g_kx.taskCap = g_kx.taskCap ? g_kx.taskCap * 2 : 1024;
            g_kx.tasks = (KiwiXferTask_t *)KiwiXfer_Realloc(g_kx.tasks,
                (size_t)g_kx.taskCap * sizeof(*g_kx.tasks));
        }
    }
    task = &g_kx.tasks[g_kx.taskTail++];
    task->item = item;
    task->cellStart = cellStart;
    task->cellEnd = cellEnd;
    task->rng = rng;
}

/* Walks the RNG over each rastered item's disk points, in work order, and
 * queues the item as trace tasks of KIWI_XFER_CHUNK_CELLS cells. */
static DWORD WINAPI KiwiXfer_PositionerThread(LPVOID param)
{
    int p = (int)(intptr_t)param;
    KiwiXferTask_t *local = NULL;
    int localCap = 0;

    AcquireSRWLockExclusive(&g_kx.lock);
    while (p < g_kx.itemCount)
    {
        KiwiXferItem_t *item = &g_kx.items[p];
        unsigned char *rays = NULL;
        uint32_t rng;
        int count, chunks, c, k;

        while (!g_kx.abort && item->stage < 2)
            SleepConditionVariableSRW(&g_kx.cvRaster, &g_kx.lock, INFINITE, 0);
        if (g_kx.abort)
            break;
        rng = item->rngStart;
        count = item->cellCount;
        ReleaseSRWLockExclusive(&g_kx.lock);

        chunks = (count + KIWI_XFER_CHUNK_CELLS - 1) / KIWI_XFER_CHUNK_CELLS;
        if (chunks > localCap)
        {
            localCap = chunks * 2;
            local = (KiwiXferTask_t *)KiwiXfer_Realloc(local,
                (size_t)localCap * sizeof(*local));
        }
        if (count)
            rays = (unsigned char *)KiwiXfer_Alloc((size_t)count
                                       * g_kx.traces * g_kiwiXferStride, 0);
        for (k = 0, c = 0; c < count; k++)
        {
            int end = c + KIWI_XFER_CHUNK_CELLS < count ? c + KIWI_XFER_CHUNK_CELLS : count;
            long long n = (long long)(end - c) * g_kx.traces;

            local[k].item = p;
            local[k].cellStart = c;
            local[k].cellEnd = end;
            local[k].rng = rng;
            while (n-- > 0)
            {
                float rx, ry;
                KiwiXfer_Disk(&rng, &rx, &ry);
            }
            c = end;
        }

        AcquireSRWLockExclusive(&g_kx.lock);
        item->rays = rays;
        item->chunkCount = chunks;
        item->chunksDone = 0;
        if (g_kx.abort)
            break;
        if (!chunks)
        {
            item->stage = 3;
            WakeAllConditionVariable(&g_kx.cvTraced);
        }
        for (k = 0; k < chunks; k++)
            KiwiXfer_PushTask(local[k].item, local[k].cellStart, local[k].cellEnd, local[k].rng);
        if (chunks)
            WakeAllConditionVariable(&g_kx.cvWork);
        if (p + 1 < g_kx.itemCount)
        {
            g_kx.items[p + 1].rngStart = rng;
            g_kx.items[p + 1].rngKnown = 1;
        }
        p++;
    }
    g_kx.positionerDone = 1;
    WakeAllConditionVariable(&g_kx.cvWork);
    ReleaseSRWLockExclusive(&g_kx.lock);
    KiwiXfer_Free(local);
    return 0;
}

static void KiwiXfer_StartPipeline(int startItem, uint32_t rng)
{
    int i;

    g_kx.abort = 0;
    g_kx.nextClaim = startItem;
    g_kx.merged = startItem;
    g_kx.outstanding = 0;
    g_kx.taskHead = 0;
    g_kx.taskTail = 0;
    g_kx.positionerDone = 0;
    g_kx.items[startItem].rngStart = rng;
    g_kx.items[startItem].rngKnown = 1;

    g_kx.threadCount = 0;
    g_kx.threads[g_kx.threadCount++] = CreateThread(NULL, 0,
        KiwiXfer_PositionerThread, (LPVOID)(intptr_t)startItem, 0, NULL);
    for (i = 0; i < g_kx.tracerCount; i++)
        g_kx.threads[g_kx.threadCount++] = CreateThread(NULL, 0,
            KiwiXfer_TracerThread, NULL, 0, NULL);
    for (i = 0; i < g_kx.threadCount; i++)
    {
        if (!g_kx.threads[i])
            ErrorMsg("Light transport: CreateThread failed (%lu)\n", GetLastError());
    }
}

/* Joins the threads and drops every plan from firstItem on. */
static void KiwiXfer_StopPipeline(int firstItem)
{
    int i;

    AcquireSRWLockExclusive(&g_kx.lock);
    g_kx.abort = 1;
    WakeAllConditionVariable(&g_kx.cvRaster);
    WakeAllConditionVariable(&g_kx.cvWork);
    WakeAllConditionVariable(&g_kx.cvTraced);
    ReleaseSRWLockExclusive(&g_kx.lock);

    for (i = 0; i < g_kx.threadCount; i++)
    {
        WaitForSingleObject(g_kx.threads[i], INFINITE);
        CloseHandle(g_kx.threads[i]);
    }
    g_kx.threadCount = 0;

    for (i = firstItem; i < g_kx.itemCount; i++)
    {
        KiwiXfer_Free(g_kx.items[i].cells);
        KiwiXfer_Free(g_kx.items[i].rays);
        memset(&g_kx.items[i], 0, sizeof(g_kx.items[i]));
    }
    g_kx.outstanding = 0;
    g_kx.taskHead = 0;
    g_kx.taskTail = 0;
    KiwiXfer_FreeGarbage(g_kx.garbage, g_kx.garbageCount);
    g_kx.garbage = NULL;
    g_kx.garbageCount = 0;
    g_kx.garbageCap = 0;
}

/* geometry.c ProcessLightingSampleArea: a cell passed the gate. */
void KiwiXfer_BeginCell(int areaIndex, const float *pos, const float *basis,
                        void *sample)
{
    KiwiXferItem_t *item = g_kx.curItem;

    g_kiwiXferCellRays = NULL;
    g_kiwiXferCellSky = -1;
    while (g_kx.cursor < item->cellCount
           && item->cells[g_kx.cursor].areaIndex < areaIndex)
    {
        g_kx.cursor++;       /* planned cell the drained gate skipped */
        g_kx.desync = 1;
    }
    if (g_kx.cursor < item->cellCount
        && item->cells[g_kx.cursor].areaIndex == areaIndex)
    {
        int idx = g_kx.cursor++;
        const KiwiXferCell_t *cell = &item->cells[idx];

        if (cell->rngStart == g_kiwiXferRng
            && !memcmp(cell->pos, pos, sizeof(cell->pos))
            && !memcmp(cell->basis, basis, sizeof(cell->basis)))
        {
            int d;

            g_kiwiXferCellRays = item->rays + (size_t)idx * g_kx.traces * g_kiwiXferStride;
            g_kiwiXferRng = cell->rngEnd;
            g_kiwiXferCellSky = cell->sky;
            g_kx.cellsReplayed++;
            for (d = 0; d < KIWI_XFER_PREFETCH_SLOT && d < g_kx.traces; d++)
                KiwiXfer_PrefetchRay((Sample_t *)sample,
                                     g_kiwiXferCellRays + d * g_kiwiXferStride, d);
            return;
        }
    }
    g_kx.desync = 1;         /* trace this cell live */
    g_kx.cellsLive++;
}

/* ForEachQuantum(…, 1) work function: the original pass, item by item. */
static void KiwiXfer_MergeItem(unsigned int j, unsigned int threadId)
{
    KiwiXferItem_t *item = &g_kx.items[j];
    LARGE_INTEGER t0, t1;

    QueryPerformanceCounter(&t0);
    AcquireSRWLockExclusive(&g_kx.lock);
    while (item->stage < 3)
        SleepConditionVariableSRW(&g_kx.cvTraced, &g_kx.lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&g_kx.lock);
    QueryPerformanceCounter(&t1);
    g_kx.waitTicks += t1.QuadPart - t0.QuadPart;

    g_kx.curItem = item;
    g_kx.cursor = 0;
    g_kx.desync = 0;
    BuildLightTransfers_Callback(j, threadId);
    g_kiwiXferCellRays = NULL;
    g_kiwiXferCellSky = -1;
    if (g_kx.cursor != item->cellCount)
        g_kx.desync = 1;

    AcquireSRWLockExclusive(&g_kx.lock);
    /* the tracers free it (big frees decommit; keep them off this thread) */
    KiwiXfer_AddGarbage(item->cells);
    KiwiXfer_AddGarbage(item->rays);
    g_kx.outstanding -= item->bytes;
    item->cells = NULL;
    item->rays = NULL;
    item->bytes = 0;
    g_kx.merged = (int)j + 1;
    WakeAllConditionVariable(&g_kx.cvWork);
    ReleaseSRWLockExclusive(&g_kx.lock);

    if (g_kx.desync && (int)j + 1 < g_kx.itemCount)
    {
        /* The plan's RNG offsets are off from here on: re-plan. */
        KiwiXfer_StopPipeline((int)j + 1);
        g_kx.restarts++;
        KiwiXfer_StartPipeline((int)j + 1, g_kiwiXferRng);
    }
}

static DWORD WINAPI KiwiXfer_RandCheckThread(LPVOID param)
{
    uint32_t s = 0x2545F491u;
    int i;

    srand(s);
    for (i = 0; i < 16; i++)
    {
        if (rand() != KiwiXfer_Rand(&s))
            return 0;
    }
    *(int *)param = 1;
    return 0;
}

/* Is rand() the per-thread MSVC LCG?  Tested on a scratch thread. */
static int KiwiXfer_CrtRandIsLcg(void)
{
    int ok = 0;
    HANDLE h = CreateThread(NULL, 0, KiwiXfer_RandCheckThread, &ok, 0, NULL);

    if (!h)
        return 0;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return ok;
}

/* Reads this thread's rand() state from 12 outputs, then restores it. */
static uint32_t KiwiXfer_RecoverRandState(void)
{
    int r[12];
    int i, found = 0;
    uint32_t low, cand = 0, inv, s;

    for (i = 0; i < 12; i++)
        r[i] = rand();
    for (low = 0; low < 65536u; low++)
    {
        uint32_t t = ((uint32_t)r[0] << 16) | low;
        for (i = 1; i < 12; i++)
        {
            if (KiwiXfer_Rand(&t) != r[i])
                break;
        }
        if (i == 12)
        {
            cand = ((uint32_t)r[0] << 16) | low;
            found++;
        }
    }
    if (found != 1)
        ErrorMsg("Light transport: rand() state recovery failed (%i)\n", found);

    /* step back over r[0]; bit 31 is never observable */
    inv = 214013u;
    for (i = 0; i < 5; i++)
        inv *= 2u - 214013u * inv;
    s = (cand - 2531011u) * inv;
    srand(s);
    return s;
}

/* geometry.c BuildLightTransfers: returns 0 to run the original pass. */
int KiwiXfer_BuildLightTransfers(void)
{
    long long sampleCount, k;
    int threads;
    LARGE_INTEGER freq, t0, t1;

    if (!g_useEmbree || !g_lightingTransportPass || g_lightingSeedSkyPass
        || g_relightCacheLoaded || g_aoEnabled || g_triCount <= 0
        || g_traces <= 0 || g_kiwiEmbreeThreads < 2)
        return 0;
    if (!KiwiXfer_CrtRandIsLcg())
        return 0;

    threads = g_kiwiEmbreeThreads;
    if (threads > KIWI_XFER_MAX_THREADS)
        threads = KIWI_XFER_MAX_THREADS;

    memset(&g_kx, 0, sizeof(g_kx));
    InitializeSRWLock(&g_kx.lock);
    InitializeConditionVariable(&g_kx.cvRaster);
    InitializeConditionVariable(&g_kx.cvWork);
    InitializeConditionVariable(&g_kx.cvTraced);
    g_kx.traces = g_traces;
    g_kx.tracerCount = threads;
    g_kx.itemCount = g_triCount;

    g_kxHeap = HeapCreate(0, 0, 0);
    if (!g_kxHeap)
        ErrorMsg("Out of memory for traced-ahead light transport\n");
    sampleCount = (long long)g_lightmapSize * 512 * 512;
    g_kx.items = (KiwiXferItem_t *)KiwiXfer_Alloc((size_t)g_triCount * sizeof(KiwiXferItem_t), 1);
    g_kx.snap = (float *)KiwiXfer_Alloc((size_t)sampleCount * 4 * sizeof(float), 0);
    g_kxIndex = (KiwiXferIndexSlot_t *)KiwiXfer_Alloc(KIWI_XFER_INDEX_SIZE * sizeof(KiwiXferIndexSlot_t), 1);
    g_kxSampleEpoch = (uint32_t *)KiwiXfer_Alloc((size_t)sampleCount * sizeof(uint32_t), 1);
    g_kxIndexUsed = 0;
    g_kxIndexEpoch = 1;
    for (k = 0; k < sampleCount; k++)
        memcpy(&g_kx.snap[k * 4],
               ((LightmapSample_t *)g_lightingSamples)[k].channelWeight,
               4 * sizeof(float));

    g_kiwiXferPacked = g_traceFilter == 1
                    && !((uintptr_t)g_lightingSamples & 15);
    g_kiwiXferStride = g_kiwiXferPacked ? sizeof(uintptr_t) : sizeof(KiwiXferRay_t);

    g_kiwiXferRng = KiwiXfer_RecoverRandState();
    g_kiwiXferCellRays = NULL;
    g_kiwiXferActive = 1;

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    KiwiXfer_StartPipeline(0, g_kiwiXferRng);
    ForEachQuantum((unsigned int)g_triCount, KiwiXfer_MergeItem, 1);
    KiwiXfer_StopPipeline(g_triCount);
    QueryPerformanceCounter(&t1);

    g_kiwiXferActive = 0;
    g_kiwiXferCellRays = NULL;
    srand(g_kiwiXferRng);    /* the CRT stream continues as if serial */

    Com_Printf("Embree transport: %i tracer threads, %lld cells traced ahead, "
               "%lld live, %i re-plans, %.1f s (merge waited %.1f s)\n",
               threads, g_kx.cellsReplayed, g_kx.cellsLive, g_kx.restarts,
               (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart,
               (double)g_kx.waitTicks / (double)freq.QuadPart);    HeapDestroy(g_kxHeap);
    g_kxHeap = NULL;
    g_kxIndex = NULL;
    g_kxSampleEpoch = NULL;
    memset(&g_kx, 0, sizeof(g_kx));
    return 1;
}
