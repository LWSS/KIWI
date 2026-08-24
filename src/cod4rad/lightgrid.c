/*
 * lightgrid.c — Light grid calculation and encoding for static lighting.
 *
 * All functions verified against cod2rad64 LST line by line.
 * Source: lightgrid.cpp (from LST source tags)
 */

#include "cod2rad64.h"

int                      g_gridSampleCount;
StaticModelGridSample_t *g_gridSampleList;
GridSamplePoint_t       *g_gridPoints;
int                      g_gridPointCount;
unsigned char            g_gridSampleArray[MAX_RAD_GRIDSAMPLE_BYTES];
int                      g_gridSampleArrayCount;
int                      g_gridColorCount;
unsigned char            g_gridColorEntries[MAX_RAD_GRIDCOLOR_BYTES];
/* CoD4 stores the runtime light-grid as compact cells plus a row stream, not
 * the temporary GridSampleResult array used while tracing. */
unsigned char            g_cod4LightGridHeader[16404];
int                      g_cod4LightGridHeaderSize;
unsigned char            g_cod4LightGridRows[0x300000];
int                      g_cod4LightGridRowsSize;
unsigned char            g_cod4LightGridEntries[MAX_RAD_GRIDSAMPLE_BYTES];
int                      g_cod4LightGridEntryCount;
unsigned char            g_cod4LightGridColors[MAX_COD4_GRIDCOLOR_BYTES];
int                      g_cod4LightGridColorCount;
/* Retail allocates 168 bytes per annotated grid point before quantization:
 * 56 RGB samples in the renderer-facing byte domain.  Keep that lifetime
 * separate from the temporary compact BSP lumps. */
unsigned char           *g_cod4LightGridVectors;
unsigned short          *g_cod4LightGridAssignments;
/* Per grid point primary-light index (the native 12-byte point record's +10
 * byte): computed by the worker before its light gather, consumed by the
 * gather (a light that IS the cell's primary renders at runtime and must not
 * be baked) and by the LUMP 2 serializer. */
unsigned char           *g_gridPointPrimaries;
static float             s_cod4LightGridBasis[56 * 3];
/* Must be contiguous — the sky-gather path passes &g_gridLightScale0 as a
 * float[3] color pointer to GatherIncidentEnergyInSpaceForLightFromDir,
 * which reads lightColor[0..2]. C doesn't guarantee layout of separately-
 * declared globals, so the compiler can (and did) interleave unrelated
 * globals and clobber the G/B channel reads with garbage. Force them into
 * a single array and expose index aliases to the rest of the code. */
float                    g_gridLightScales[4];
#define g_gridLightScale0 g_gridLightScales[0]
#define g_gridLightScale1 g_gridLightScales[1]
#define g_gridLightScale2 g_gridLightScales[2]
#define g_gridLightScale3 g_gridLightScales[3]
char                     g_gridLogBasePath[MAX_OS_PATH_SHORT];

/*
 * GridSampleResult — grid sample output entry, 8 bytes.
 * Packed grid coords + sky visibility + color index.
 * Stored in g_gridSampleArray, sorted by GridSamplePoint_CompareForSort.
 */
typedef struct GridSampleResult
{
    int packedCoords;           /* +0x00: packed grid coordinates */
    unsigned char coordBits;    /* +0x04: low bits of x/y/z */
    unsigned char skyVis;       /* +0x05: octant sky visibility bits */
    short colorIndex;           /* +0x06: index into grid color table */
    unsigned int producerIndex; /* +0x08: preserves color ownership across qsort */
} GridSampleResult;

static void LightGrid_BuildCod4Basis(void)
{
    int x, y, z, basisIndex;

    basisIndex = 0;
    for (z = 0; z < 4; ++z)
    {
        for (y = 0; y < 4; ++y)
        {
            for (x = 0; x < 4; ++x)
            {
                float *basis;
                float length;

                /* Native 0x411AA0 builds the normalized outer shell of a
                 * 4x4x4 cube: all 64 lattice points except the central 2x2x2. */
                if ((x == 1 || x == 2) && (y == 1 || y == 2)
                    && (z == 1 || z == 2))
                {
                    continue;
                }

                basis = &s_cod4LightGridBasis[basisIndex * 3];
                basis[0] = (float)x * (2.0f / 3.0f) - 1.0f;
                basis[1] = (float)y * (2.0f / 3.0f) - 1.0f;
                basis[2] = (float)z * (2.0f / 3.0f) - 1.0f;
                length = sqrtf(basis[0] * basis[0] + basis[1] * basis[1]
                             + basis[2] * basis[2]);
                basis[0] /= length;
                basis[1] /= length;
                basis[2] /= length;
                ++basisIndex;
            }
        }
    }

    Assert("basisIndex == GFX_LIGHTGRID_SAMPLE_COUNT",
           ".\\lightgrid.cpp", 1170, 0, basisIndex == 56);
}

static void LightGrid_AccumulateCod4Vector(const float *color,
                                            const float *direction,
                                            float *vector)
{
    int sample;

    for (sample = 0; sample < 56; ++sample)
    {
        const float *basis = &s_cod4LightGridBasis[sample * 3];
        /* The retail x86 build evaluates each dot and color multiply-add in
         * the x87 register stack, rounding only when it stores the float.
         * Use double intermediates on x64 to preserve that boundary. */
        float dot = (float)((double)basis[0] * (double)direction[0]
                          + (double)basis[1] * (double)direction[1]
                          + (double)basis[2] * (double)direction[2]);
        if (dot > 0.0f)
        {
            vector[sample * 3 + 0] = (float)(
                (double)vector[sample * 3 + 0] + (double)color[0] * dot);
            vector[sample * 3 + 1] = (float)(
                (double)vector[sample * 3 + 1] + (double)color[1] * dot);
            vector[sample * 3 + 2] = (float)(
                (double)vector[sample * 3 + 2] + (double)color[2] * dot);
        }
    }
}

static void LightGrid_AddUniformCod4Vector(const float *color, float *vector)
{
    int sample;

    for (sample = 0; sample < 56; ++sample)
    {
        vector[sample * 3 + 0] += color[0];
        vector[sample * 3 + 1] += color[1];
        vector[sample * 3 + 2] += color[2];
    }
}

static void LightGrid_AdjustCod4Contrast(float *colors)
{
    float intensity[56];
    float minIntensity = 3.402823466e+38f;
    float maxIntensity = -3.402823466e+38f;
    float baseIntensity;
    float contrast;
    int sample;

    for (sample = 0; sample < 56; ++sample)
    {
        const float *color = &colors[sample * 3];
        float value = color[0] * 0.298999995f
                    + color[1] * 0.587000012f
                    + color[2] * 0.114000000f;
        intensity[sample] = value;
        if (value < minIntensity) minIntensity = value;
        if (value > maxIntensity) maxIntensity = value;
    }

    baseIntensity = (maxIntensity + minIntensity) * 0.5f;
    contrast = maxIntensity - minIntensity;
    if (contrast != 0.0f && contrast < 0.5f)
    {
        float contrastScale = powf(contrast * 2.0f, -g_contrastGain);
        for (sample = 0; sample < 56; ++sample)
        {
            float adjusted = (intensity[sample] - baseIntensity) * contrastScale
                           + baseIntensity;
            float *color = &colors[sample * 3];
            if (adjusted <= 0.0f)
            {
                color[0] = color[1] = color[2] = 0.0f;
            }
            else if (intensity[sample] != 0.0f)
            {
                float scale = adjusted / intensity[sample];
                color[0] *= scale;
                color[1] *= scale;
                color[2] *= scale;
            }
        }
    }
}

static unsigned char LightGrid_EncodeCod4Byte(float value)
{
    if (value <= 0.0f) return 0;
    if (value >= 1.0f) return 255;
    return (unsigned char)(int)(value * 255.0f + 9.313225746e-10f);
}

static void LightGrid_StoreProducerVector(int gridIndex, const float *producer)
{
    float adjusted[56 * 3];
    unsigned char *out;
    int component;

    if (!g_cod4LightGridVectors)
        return;

    /* Native 0x411630 gamma-corrects all 56 colors, adjusts contrast in
     * place, halves the result, then truncates it into the byte domain. */
    for (component = 0; component < 56 * 3; ++component)
        adjusted[component] = GammaCorrectColorChannel(producer[component]);
    LightGrid_AdjustCod4Contrast(adjusted);

    out = g_cod4LightGridVectors + (unsigned long long)gridIndex * 168;
    for (component = 0; component < 56 * 3; ++component)
        out[component] = LightGrid_EncodeCod4Byte(adjusted[component] * 0.5f);
}

typedef struct Cod4LightGridCluster_s {
    int first, count, splitDimension;
    float splitValue, maxStdDev;
} Cod4LightGridCluster_t;

static void LightGrid_QuantizeVectors(void)
{
    Cod4LightGridCluster_t *clusters;
    int *indices;
    int vectorCount, clusterLimit, clusterCount, i;

    if (!g_cod4LightGridVectors || g_gridPointCount <= 0)
        return;

    /* 0x411970 appends one reserved sky/default vector.  It participates in
     * quantization, is moved to palette slot 1, then is omitted from LUMP 2. */
    /* The native quantizer consumes the expanded twelve-byte sample list,
     * not the source point list.  They normally have the same cardinality,
     * but using the produced list matters when a point was suppressed before
     * the worker stage.  0x411970 appends the sky sample after that list. */
    vectorCount = g_gridSampleArrayCount + 1;
    clusterLimit = vectorCount < 0xFFFF ? vectorCount : 0xFFFF;
    clusters = (Cod4LightGridCluster_t *)malloc(
        (unsigned long long)clusterLimit * sizeof(*clusters));
    indices = (int *)malloc((unsigned long long)vectorCount * sizeof(*indices));
    g_cod4LightGridAssignments = (unsigned short *)malloc(
        (unsigned long long)vectorCount * sizeof(*g_cod4LightGridAssignments));
    if (!clusters || !indices || !g_cod4LightGridAssignments)
        ErrorMsg("Couldn't allocate light grid quantizer state\n");

    for (i = 0; i < vectorCount; ++i)
        indices[i] = i;
    clusters[0].first = 0;
    clusters[0].count = vectorCount;
    clusterCount = 1;

    for (;;)
    {
        int bestCluster = 0;
        int clusterIndex;

        for (clusterIndex = 0; clusterIndex < clusterCount; ++clusterIndex)
        {
            Cod4LightGridCluster_t *cluster = &clusters[clusterIndex];
            float greatestVariance = -1.0f;
            int dimension;

            cluster->splitDimension = 0;
            cluster->splitValue = 0.0f;
            for (dimension = 0; dimension < 168; ++dimension)
            {
                unsigned int sum = 0;
                float variance = 0.0f;
                float mean;
                int member;

                for (member = 0; member < cluster->count; ++member)
                {
                    int vectorIndex = indices[cluster->first + member];
                    sum += g_cod4LightGridVectors[
                        (unsigned long long)vectorIndex * 168 + dimension];
                }
                /* 0x411C50 performs the integer mean in x87 precision and
                 * stores one float result. */
                mean = (float)((double)sum / (double)(unsigned int)cluster->count);
                for (member = 0; member < cluster->count; ++member)
                {
                    int vectorIndex = indices[cluster->first + member];
                    float delta = (float)((double)g_cod4LightGridVectors[
                        (unsigned long long)vectorIndex * 168 + dimension]
                        - (double)mean);
                    variance = (float)((double)variance
                                     + (double)delta * (double)delta);
                }
                variance = (float)((double)variance
                                 / (double)(unsigned int)cluster->count);
                if (variance > greatestVariance)
                {
                    greatestVariance = variance;
                    cluster->splitDimension = dimension;
                    cluster->splitValue = mean;
                }
            }
            cluster->maxStdDev = sqrtf(greatestVariance);
            if (cluster->maxStdDev > clusters[bestCluster].maxStdDev)
                bestCluster = clusterIndex;
        }

        /* Native defaults: 65,535 colors maximum and 3.5 byte standard
         * deviation.  It always permits the initial cluster to split once. */
        if (clusterCount >= clusterLimit
            || (clusterCount >= 2 && clusters[bestCluster].maxStdDev <= 3.5f))
        {
            break;
        }

        {
            Cod4LightGridCluster_t *cluster = &clusters[bestCluster];
            int head, tail, end, dimension;
            float splitValue;

            head = cluster->first;
            end = cluster->first + cluster->count;
            tail = end - 1;
            dimension = cluster->splitDimension;
            splitValue = cluster->splitValue;

            if (cluster->maxStdDev <= 0.0f)
            {
                head = cluster->first + ((cluster->count + 1) >> 1);
            }
            else
            {
                while (head <= tail)
                {
                    while (head <= tail
                        && (float)g_cod4LightGridVectors[
                            (unsigned long long)indices[head] * 168 + dimension]
                            <= splitValue)
                    {
                        ++head;
                    }
                    while (head <= tail
                        && (float)g_cod4LightGridVectors[
                            (unsigned long long)indices[tail] * 168 + dimension]
                            >= splitValue)
                    {
                        --tail;
                    }
                    if (head < tail)
                    {
                        int temp = indices[head];
                        indices[head++] = indices[tail];
                        indices[tail--] = temp;
                    }
                }
            }

            if (head == cluster->first || head == end)
                head = cluster->first + ((cluster->count + 1) >> 1);
            clusters[clusterCount].first = head;
            clusters[clusterCount].count = end - head;
            cluster->count = head - cluster->first;
            ++clusterCount;
        }
    }

    for (i = 0; i < clusterCount; ++i)
    {
        Cod4LightGridCluster_t *cluster = &clusters[i];
        int dimension, member;

        for (dimension = 0; dimension < 168; ++dimension)
        {
            unsigned int sum = 0;
            for (member = 0; member < cluster->count; ++member)
            {
                int vectorIndex = indices[cluster->first + member];
                sum += g_cod4LightGridVectors[
                    (unsigned long long)vectorIndex * 168 + dimension];
            }
            g_cod4LightGridColors[i * 168 + dimension] =
                (unsigned char)(int)((double)sum
                    / (double)(unsigned int)cluster->count + 0.5);
        }
        for (member = 0; member < cluster->count; ++member)
        {
            int vectorIndex = indices[cluster->first + member];
            g_cod4LightGridAssignments[vectorIndex] = (unsigned short)i;
        }
    }

    /* 0x412AD0 finishes by exchanging two complete palettes.  Palette zero
     * is the cluster containing the most samples tagged with the primary
     * light index; palette one is the cluster owning the final, appended sky
     * sample.  0x412650 exchanges both the 168-byte colors and every 16-bit
     * assignment (it also exchanges the now-dead cluster records). */
    if (clusterCount > 1)
    {
        int *primaryCounts = (int *)calloc((unsigned long long)clusterCount, sizeof(int));
        int primaryCluster = 0;
        int sampleIndex;

        if (!primaryCounts)
            ErrorMsg("Couldn't allocate light grid palette counters\n");
        for (sampleIndex = 0; sampleIndex < g_gridSampleArrayCount; ++sampleIndex)
        {
            GridSampleResult *sample = &((GridSampleResult *)g_gridSampleArray)[sampleIndex];
            /* Native 0x412A60 counts byte +10 of the twelve-byte working
             * sample against the selected primary light index, while
             * deliberately ignoring the appended final sky sample. */
            if (sample->coordBits & 1)
                ++primaryCounts[g_cod4LightGridAssignments[sample->producerIndex]];
        }
        for (i = 1; i < clusterCount; ++i)
            if (primaryCounts[i] > primaryCounts[primaryCluster]) primaryCluster = i;
        free(primaryCounts);

        if (primaryCluster != 0)
        {
            unsigned char tempColor[168];
            memcpy(tempColor, g_cod4LightGridColors, 168);
            memcpy(g_cod4LightGridColors,
                   g_cod4LightGridColors + primaryCluster * 168, 168);
            memcpy(g_cod4LightGridColors + primaryCluster * 168, tempColor, 168);
            for (i = 0; i < vectorCount; ++i)
            {
                if (g_cod4LightGridAssignments[i] == 0)
                    g_cod4LightGridAssignments[i] = (unsigned short)primaryCluster;
                else if (g_cod4LightGridAssignments[i] == primaryCluster)
                    g_cod4LightGridAssignments[i] = 0;
            }
        }

        {
            int skyCluster = g_cod4LightGridAssignments[vectorCount - 1];
            if (skyCluster != 1)
            {
                unsigned char tempColor[168];
                memcpy(tempColor, g_cod4LightGridColors + 168, 168);
                memcpy(g_cod4LightGridColors + 168,
                       g_cod4LightGridColors + skyCluster * 168, 168);
                memcpy(g_cod4LightGridColors + skyCluster * 168, tempColor, 168);
                for (i = 0; i < vectorCount; ++i)
                {
                    if (g_cod4LightGridAssignments[i] == 1)
                        g_cod4LightGridAssignments[i] = (unsigned short)skyCluster;
                    else if (g_cod4LightGridAssignments[i] == skyCluster)
                        g_cod4LightGridAssignments[i] = 1;
                }
            }
        }
    }

    g_cod4LightGridColorCount = clusterCount;
    free(indices);
    free(clusters);
}

/* external functions not in master header */
extern void qsort(void *base, unsigned long long count, unsigned long long size, void *cmp);
extern void *fopen_wrap(const char *path, const char *mode);
extern void fseek_wrap(void *file, int offset, int whence);
extern int ftell_wrap(void *file);
extern long long fread_wrap(void *dst, int elemSize, long long count, void *file);
extern void fclose_wrap(void *file);
/* BuildFilePath — copies base path to out buffer. Inlined from binary. */
static void BuildFilePath(const char *basePath, char *outPath)
{
    while ((*outPath++ = *basePath++) != '\0') { }
}

/* 0x410160 / 0x410280.  Retail performs this lookup over its expanded
 * twelve-byte working records.  At this stage the x64 port still owns the
 * canonical six-byte point list, so use the same coordinate conversion and
 * binary-search semantics directly on that list.  CalculateLightGrid_SortPoints
 * has established x/y/z order before any light-grid worker is dispatched. */
static int LightGrid_HasPointAtWorldPos(const float *worldPos)
{
    GridSamplePoint_t key;
    int lo = 0, hi = g_gridPointCount - 1;

    if (g_gridPointCount <= 0)
        return 0;
    key.x = (unsigned short)(int)floorf((worldPos[0] + 131072.0f) * 0.03125f + 0.5f);
    key.y = (unsigned short)(int)floorf((worldPos[1] + 131072.0f) * 0.03125f + 0.5f);
    key.z = (unsigned short)(int)floorf((worldPos[2] + 131072.0f) * 0.015625f + 0.5f);
    while (lo <= hi)
    {
        int mid = lo + ((hi - lo) >> 1);
        GridSamplePoint_t *point = &g_gridPoints[mid];
        if (point->x < key.x ||
            (point->x == key.x && (point->y < key.y ||
             (point->y == key.y && point->z < key.z))))
        {
            lo = mid + 1;
        }
        else if (point->x > key.x || point->y > key.y || point->z > key.z)
        {
            hi = mid - 1;
        }
        else
        {
            return 1;
        }
    }
    return 0;
}

/* geometry.cpp 0x40A040: classify an AABB (centre followed by extents)
 * against a plane.  The occupancy tree's recursive collector uses these
 * exact three return values: 0=back, 1=front, 3=straddling. */
static int LightGrid_ClassifyAabbAgainstPlane(const float *plane,
                                              const float *centre,
                                              float planeDist)
{
    float radius = fabsf(centre[0]) * plane[3] +
                   fabsf(centre[1]) * plane[4] +
                   fabsf(centre[2]) * plane[5];
    float distance = plane[0] * centre[0] + plane[1] * centre[1] +
                     plane[2] * centre[2] - planeDist;

    if (distance >= radius)
        return 0;
    if (distance <= -radius)
        return 1;
    return 3;
}

/* The retail 0x409E70/0x409F70 query walks a second triangle occupancy
 * tree and rejects a point inside its 0.1-unit expanded triangle volume.
 * Cod4Rad already owns a native, pointer-safe collision BSP (the tree used
 * by TraceSetup_and_Dispatch), so reuse it instead of maintaining a second
 * 32-bit triangle pool solely for this light-grid predicate. */
static int LightGrid_PointNearGeometry(const float *point)
{
    static const float axes[3][3] = {
        { 0.1f, 0.0f, 0.0f }, { 0.0f, 0.1f, 0.0f }, { 0.0f, 0.0f, 0.1f }
    };
    int axis;

    for (axis = 0; axis < 3; ++axis)
    {
        float start[3], end[3];
        RayHitResult_t hit;
        start[0] = point[0] - axes[axis][0];
        start[1] = point[1] - axes[axis][1];
        start[2] = point[2] - axes[axis][2];
        end[0] = point[0] + axes[axis][0];
        end[1] = point[1] + axes[axis][1];
        end[2] = point[2] + axes[axis][2];
        TraceSetup_and_Dispatch(0, start, end, &hit);
        if (hit.triangle)
            return 1;
    }
    return 0;
}

/* Conservative stand-in for 0x40CF50's octant triangle collection: the
 * octant box holds geometry when any of its four space diagonals crosses a
 * triangle.  Retail clips the actual triangle set into a transient
 * partition; a diagonal sweep answers the same "is there anything in this
 * box at all" gate without the 32-bit pool. */
static int LightGrid_BoxTouchesGeometry(const float *centre, const float *ext)
{
    int i;

    for (i = 0; i < 4; ++i)
    {
        float start[3], end[3];
        RayHitResult_t hit;
        float sx = (i & 1) ? -ext[0] : ext[0];
        float sy = (i & 2) ? -ext[1] : ext[1];

        start[0] = centre[0] - sx;
        start[1] = centre[1] - sy;
        start[2] = centre[2] - ext[2];
        end[0] = centre[0] + sx;
        end[1] = centre[1] + sy;
        end[2] = centre[2] + ext[2];
        TraceSetup_and_Dispatch(0, start, end, &hit);
        if (hit.triangle)
            return 1;
    }
    return 0;
}

/* 0x40DA70.  An octant needs tracing only when its ±16,±16,±32 box actually
 * contains geometry (an empty octant interpolates freely — 0x40CF50 finding
 * no triangles returns 0 immediately) AND some other grid point bordering
 * that box exists, sits clear of geometry, and is reachable from this
 * octant's inner corner.  Retail answers the reachability from its clipped
 * triangle partition (0x40D920/0x40D990); the compiler's collision BSP
 * answers the same question here. */
static int LightGrid_OctantNeedsTrace(const float *point, int octant)
{
    static const float kOctExt[3] = { 16.0f, 16.0f, 32.0f };
    float centre[3], corner[3];
    int candidate;
    int opposite = octant ^ 7;

    centre[0] = point[0] + ((octant & 1) ? -16.0f : 16.0f);
    centre[1] = point[1] + ((octant & 2) ? -16.0f : 16.0f);
    centre[2] = point[2] + ((octant & 4) ? -32.0f : 32.0f);

    if (!LightGrid_BoxTouchesGeometry(centre, kOctExt))
        return 0;

    /* the probe corner sits 0.125 inside the box toward the source point */
    corner[0] = centre[0] + ((opposite & 1) ? -15.875f : 15.875f);
    corner[1] = centre[1] + ((opposite & 2) ? -15.875f : 15.875f);
    corner[2] = centre[2] + ((opposite & 4) ? -31.875f : 31.875f);

    for (candidate = 0; candidate < 8; ++candidate)
    {
        float neighbour[3];
        if (candidate == opposite)
            continue;
        neighbour[0] = centre[0] + ((candidate & 1) ? -15.875f : 15.875f);
        neighbour[1] = centre[1] + ((candidate & 2) ? -15.875f : 15.875f);
        neighbour[2] = centre[2] + ((candidate & 4) ? -31.875f : 31.875f);
        if (!LightGrid_HasPointAtWorldPos(neighbour))
            continue;
        if (LightGrid_PointNearGeometry(neighbour))
            continue;
        /* 0x40D990 is region reachability inside the geometry-partitioned
         * box: a neighbour in the SAME region interpolates fine and is
         * skipped; a neighbour separated from the corner by the octant's
         * triangles forces the renderer to trace. */
        if (TraceVisibility(0, corner, neighbour))
            continue;
        return 1;
    }
    return 0;
}

/* 0x410CF0.  The producer stores the octant decision in the renderer's
 * axis-relative bit ordering rather than the ordinal octant order. */
static unsigned char LightGrid_ComputeNeedsTraceMask(const float *point, int rowAxis)
{
    unsigned char result = 0;
    int octant;

    for (octant = 0; octant < 8; ++octant)
    {
        int bit = 0;
        if (!LightGrid_OctantNeedsTrace(point, octant))
            continue;
        if (octant & 1)
            bit = 2 * (rowAxis == 0) + 2;
        if (octant & 2)
            bit |= 2 * (rowAxis != 0) + 2;
        if (octant & 4)
            bit |= 1;
        result |= (unsigned char)(1u << bit);
    }
    return result;
}

/* 0x410870 reads a second six-byte point list, then removes every point at
 * those coordinates and runs the normal sort/dedup pass.  Its in-place
 * neighbour overwrite is only needed to retain a compact sorted array on
 * the original 32-bit implementation; stable removal has the same remaining
 * point set and is safer when duplicate authored points are present. */
static void LightGrid_ApplyGridNot(void)
{
    char filePath[0x400];
    void *file;
    int fileSize, suppressCount, i, outCount;
    GridSamplePoint_t *suppressPoints;

    if (!g_gridPoints || g_gridPointCount <= 0)
        return;

    BuildFilePath(g_gridLogBasePath, filePath);
    {
        char *end = filePath;
        while (*end) ++end;
        memcpy(end, ".grid_not", sizeof(".grid_not"));
    }
    file = fopen_wrap(filePath, "rb");
    if (!file)
        return;

    fseek_wrap(file, 0, 2);
    fileSize = ftell_wrap(file);
    fseek_wrap(file, 0, 0);
    if (fileSize <= 0 || (fileSize % 6) != 0)
    {
        Com_Printf("Ignoring grid exclusion file '%s': size %i is not a multiple of %i\n",
                   filePath, fileSize, 6);
        fclose_wrap(file);
        return;
    }
    suppressCount = fileSize / 6;
    suppressPoints = (GridSamplePoint_t *)malloc((unsigned long long)fileSize);
    if (!suppressPoints)
    {
        fclose_wrap(file);
        ErrorMsg("couldn't allocate %.2f MB for the light grid exclusions\n",
                 (double)((float)fileSize * (1.0f / (1024.0f * 1024.0f))));
        return;
    }
    if (fread_wrap(suppressPoints, 6, suppressCount, file) != suppressCount)
    {
        fclose_wrap(file);
        free(suppressPoints);
        ErrorMsg("Error while reading %s\n", filePath);
        return;
    }
    fclose_wrap(file);

    outCount = 0;
    for (i = 0; i < g_gridPointCount; ++i)
    {
        GridSamplePoint_t point = g_gridPoints[i];
        int j, suppressed = 0;
        for (j = 0; j < suppressCount; ++j)
        {
            if (point.x == suppressPoints[j].x && point.y == suppressPoints[j].y &&
                point.z == suppressPoints[j].z)
            {
                suppressed = 1;
                break;
            }
        }
        if (!suppressed)
            g_gridPoints[outCount++] = point;
    }
    if (outCount != g_gridPointCount)
        Com_Printf("Suppressed %i light grid points from '%s'\n",
                   g_gridPointCount - outCount, filePath);
    g_gridPointCount = outCount;
    free(suppressPoints);
}
extern int rand_int(void);
extern int FindLightingSamplesAndNormal(int sampleIdx, float *position, float *normal,
    float offset, void *outputLighting, float *outputNormal);
extern void Lighting_GetGatheredLight(void *sample, float *outVars); /* lighting_412550 */
/* MUST match the pointlights.c definition — a shorter local prototype here
 * shifted every argument (lightIndex took a truncated pointer, pos became
 * null) and crashed the grid phase on the first map with a point light. */
extern int PointLightEvaluatePoint(int surfacePrimaryLightIndex, int traceIndex,
    int lightIndex, float *pos, float *normal,
    float *outDir, float *outColor, float *outDot);
extern void AdjustLightingContrast(int sampleCount, int baseIndex, float *srcSamples, float *dstColors);
extern unsigned char EncodeGammaCorrectedByte(float value);
extern short LightGrid_FindOrInsertColor(unsigned char *colorData);
extern int TraceVisibility(int cacheIndex, float *startPos, float *endPos);
extern void TraceSetup_and_Dispatch(int cacheIndex, float *startPos, float *endPos, RayHitResult_t *outHit);

extern float g_sunDirX;
extern float g_sunDirY;
extern float g_sunDirZ;

/* forward declarations for lightgrid internal functions */
extern void CalculateLightGrid_Setup(void);
GridSampleResult *AllocGridSample(int flags, GridSamplePoint_t *pt);
static void CalculateLightGrid_GatherLightInternal(int flags, int gridIndex,
                                                   float *outBuffer,
                                                   float *cod4Vector);

#define GRID_ORIGIN_OFFSET  (-131072.0f)  /* dword_458A90 = 0xC8000000 */
#define GRID_SCALE_XY       0.03125f      /* dword_458784 = 0x3D000000 = 1/32 */
#define GRID_SCALE_Z        0.015625f     /* dword_458A8C = 0x3C800000 = 1/64 */

/* ── LUMP 44/45/2 serialization, native 0x4134A0 → 0x413320 → 0x413160 →
 *    0x412E80 / 0x412CF0 ──────────────────────────────────────────────────
 * The native writer expands each point into a 12-byte working record
 * {u16 pos[3], u16 colorIndex, u8 primaryLightIndex, u8 needsTraceMask},
 * sorts by (pos[rowAxis], pos[colAxis], z), and emits per row: a 12-byte row
 * header, then run records over contiguous columns sharing one z-span, with
 * the entry stream densely filled (fillers for missing cells).  Entry cells
 * are 4 bytes: {u16 colorIndex, u8 primaryLightIndex, u8 needsTraceMask}. */

typedef struct GridCell_s
{
    unsigned short pos[3];
    unsigned short colorIndex;
    unsigned char  primary;
    unsigned char  needsTrace;
} GridCell_t;

static int s_gridRowAxis;   /* dword_61B2D14 */
static int s_gridColAxis;   /* dword_61B2D18 */

/* 0x410110: order by row coordinate, then column, then z. */
static int LightGrid_CellCompare(const void *a, const void *b)
{
    const GridCell_t *ca = (const GridCell_t *)a;
    const GridCell_t *cb = (const GridCell_t *)b;
    if (ca->pos[s_gridRowAxis] != cb->pos[s_gridRowAxis])
        return ca->pos[s_gridRowAxis] < cb->pos[s_gridRowAxis] ? -1 : 1;
    if (ca->pos[s_gridColAxis] != cb->pos[s_gridColAxis])
        return ca->pos[s_gridColAxis] < cb->pos[s_gridColAxis] ? -1 : 1;
    if (ca->pos[2] != cb->pos[2])
        return ca->pos[2] < cb->pos[2] ? -1 : 1;
    return 0;
}

/* 0x411380: a cell sees the sun when the sun ray reaches a sky surface.
 * cacheIndex is the caller's per-worker trace cache slot — slot zero races
 * the parallel grid workers' cache stamps. */
static int LightGrid_SunVisibleAt(int cacheIndex, const float *pos)
{
    float nearPt[3], farPt[3];
    RayHitResult_t hit;

    nearPt[0] = g_sunDirX * 0.125f + pos[0];
    nearPt[1] = g_sunDirY * 0.125f + pos[1];
    nearPt[2] = g_sunDirZ * 0.125f + pos[2];
    farPt[0] = g_sunDirX * 262144.0f + pos[0];
    farPt[1] = g_sunDirY * 262144.0f + pos[1];
    farPt[2] = g_sunDirZ * 262144.0f + pos[2];
    TraceSetup_and_Dispatch(cacheIndex, nearPt, farPt, &hit);
    return !hit.triangle
        || (hit.triangle->material->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY)) != 0;
}

/* 0x4115B0 + 0x411470: a cell with no sun claims the single in-range non-sun
 * primary (several candidates cancel to none); with no candidate, 255 marks
 * a cell whose ±16,±16,±32 neighbourhood (probed at steps 8,8,16) still sees
 * the sun. */
static unsigned char LightGrid_FallbackPrimary(int cacheIndex, const float *pos)
{
    int idx, found = 0;
    float x, y, z;

    for (idx = g_sunPrimaryLightIndex + 1; idx < numBSPPrimaryLights; ++idx)
    {
        BspPrimaryLight_t *light = &bspPrimaryLights[idx];
        float d0 = pos[0] - light->origin[0];
        float d1 = pos[1] - light->origin[1];
        float d2 = pos[2] - light->origin[2];
        if (d0 * d0 + d1 * d1 + d2 * d2 < light->radius * light->radius)
        {
            /* 0x4111C0 additionally narrows spot lights by cone and consults
             * the light's shadow-sample list; over-inclusion here only
             * matters where two non-sun primaries overlap a cell. */
            if (found)
                return 0;
            found = idx;
        }
    }
    if (found)
        return (unsigned char)found;

    for (z = pos[2] - 32.0f; z <= pos[2] + 32.0f; z += 16.0f)
        for (y = pos[1] - 16.0f; y <= pos[1] + 16.0f; y += 8.0f)
            for (x = pos[0] - 16.0f; x <= pos[0] + 16.0f; x += 8.0f)
            {
                float p[3];
                p[0] = x; p[1] = y; p[2] = z;
                if (LightGrid_SunVisibleAt(cacheIndex, p))
                    return 255;
            }
    return 0;
}

/* 0x412CF0: a column gap becomes 2-byte skip records {count, 0}. */
static void LightGrid_WriteSkip(unsigned int gap)
{
    unsigned char *rows = g_cod4LightGridRows;

    if (gap > 255)
    {
        unsigned int n = (gap - 256) / 255 + 1;
        do
        {
            rows[g_cod4LightGridRowsSize++] = 255;
            rows[g_cod4LightGridRowsSize++] = 0;
            gap = (gap & ~0xFFu) | ((gap + 1) & 0xFFu);
        }
        while (--n);
    }
    rows[g_cod4LightGridRowsSize++] = (unsigned char)gap;
    rows[g_cod4LightGridRowsSize++] = 0;
}

/* 0x412E80: emit one run — a dense runLen x height cell block (fillers carry
 * colorIndex 0 and the sun primary), then the run record {count, height,
 * zOffset[, zOffset high byte]}. */
static void LightGrid_FlushRun(int runLen, unsigned short zFirst,
                               unsigned short zLast, unsigned short rowZMin,
                               int rowHeightOver255, GridCell_t *cells,
                               int begin, int end, int *entryCount)
{
    int height = zLast - zFirst + 1;
    int c, dz, idx = begin;
    unsigned short runStartCol = cells[begin].pos[s_gridColAxis];
    unsigned int remaining;
    unsigned char *rows = g_cod4LightGridRows;

    if (height > 255)
        ErrorMsg("light grid vertical variation is too extreme\n");

    for (c = 0; c < runLen; ++c)
    {
        for (dz = 0; dz < height; ++dz)
        {
            unsigned char *entry = g_cod4LightGridEntries + (size_t)*entryCount * 4;
            if (*entryCount >= 0x100000)
                ErrorMsg("MAX_MAP_LIGHTGRID_POINTS exceeded\n");
            if (idx < end && cells[idx].pos[2] == (unsigned short)(zFirst + dz)
                && cells[idx].pos[s_gridColAxis] == (unsigned short)(runStartCol + c))
            {
                *(unsigned short *)entry = cells[idx].colorIndex;
                entry[2] = cells[idx].primary;
                entry[3] = cells[idx].needsTrace;
                ++idx;
            }
            else
            {
                *(unsigned short *)entry = 0;
                entry[2] = (unsigned char)g_sunPrimaryLightIndex;
                entry[3] = 0;
            }
            ++*entryCount;
        }
    }

    remaining = (unsigned int)runLen;
    do
    {
        unsigned int chunk = remaining > 255 ? 255 : remaining;
        rows[g_cod4LightGridRowsSize++] = (unsigned char)chunk;
        rows[g_cod4LightGridRowsSize++] = (unsigned char)height;
        rows[g_cod4LightGridRowsSize++] = (unsigned char)(zFirst - rowZMin);
        if (rowHeightOver255)
            rows[g_cod4LightGridRowsSize++] = (unsigned char)((zFirst - rowZMin) >> 8);
        remaining -= chunk;
    }
    while (remaining);
}

static void LightGrid_BuildCod4Compact(void)
{
    int i, axis, entryCount = 0;
    int cellCount;
    unsigned short mins[3] = { 0xffff, 0xffff, 0xffff };
    unsigned short maxs[3] = { 0, 0, 0 };
    unsigned short *header16 = (unsigned short *)g_cod4LightGridHeader;
    GridCell_t *cells;
    int rowCount;

    if (!g_gridPointCount || !g_gridSampleArrayCount)
        return;

    /* 0x412AD0 clusters the producer's 56 RGB samples into modern 168-byte
     * renderer colors and attaches the resulting 16-bit index to each cell. */
    LightGrid_QuantizeVectors();

    cellCount = g_gridSampleArrayCount;
    cells = (GridCell_t *)malloc((size_t)cellCount * sizeof(GridCell_t));
    if (!cells)
        ErrorMsg("couldn't allocate light grid cell records\n");

    for (i = 0; i < cellCount; ++i)
    {
        GridSampleResult *sample = &((GridSampleResult *)g_gridSampleArray)[i];
        GridSamplePoint_t *pt = &g_gridPoints[sample->producerIndex];
        GridCell_t *cell = &cells[i];

        cell->pos[0] = pt->x;
        cell->pos[1] = pt->y;
        cell->pos[2] = pt->z;
        cell->colorIndex = g_cod4LightGridAssignments
            ? g_cod4LightGridAssignments[sample->producerIndex] : 0;
        /* the worker fixed the cell primary before its gather (0x4116B0) */
        cell->primary = g_gridPointPrimaries
            ? g_gridPointPrimaries[sample->producerIndex] : 0;
        cell->needsTrace = 0;   /* filled after the axis is chosen */

        if (pt->x < mins[0]) mins[0] = pt->x;
        if (pt->x > maxs[0]) maxs[0] = pt->x;
        if (pt->y < mins[1]) mins[1] = pt->y;
        if (pt->y > maxs[1]) maxs[1] = pt->y;
        if (pt->z < mins[2]) mins[2] = pt->z;
        if (pt->z > maxs[2]) maxs[2] = pt->z;
    }

    /* 0x4134A0: the row axis is X unless X's span exceeds Y's. */
    axis = (maxs[1] - mins[1] >= maxs[0] - mins[0]) ? 0 : 1;
    s_gridRowAxis = axis;
    s_gridColAxis = axis ^ 1;
    qsort(cells, cellCount, sizeof(GridCell_t), LightGrid_CellCompare);

    /* 0x410CF0 encodes the octant probes relative to the chosen axis. */
    for (i = 0; i < cellCount; ++i)
    {
        float worldPos[3];
        worldPos[0] = (float)((int)cells[i].pos[0] - 0x1000) * 32.0f;
        worldPos[1] = (float)((int)cells[i].pos[1] - 0x1000) * 32.0f;
        worldPos[2] = (float)((int)cells[i].pos[2] - 0x800) * 64.0f;
        cells[i].needsTrace = LightGrid_ComputeNeedsTraceMask(worldPos, axis);
    }

    memcpy(header16, mins, sizeof(mins));
    memcpy(header16 + 3, maxs, sizeof(maxs));
    *(unsigned int *)(g_cod4LightGridHeader + 12) = (unsigned int)axis;
    *(unsigned int *)(g_cod4LightGridHeader + 16) = (unsigned int)(axis ^ 1);
    rowCount = maxs[axis] - mins[axis] + 1;
    if (rowCount <= 0 || 20 + rowCount * 2 > (int)sizeof(g_cod4LightGridHeader))
        ErrorMsg("light grid row span %i exceeds the header capacity\n", rowCount);
    /* 0x4134A0 presets every row offset to the 0xFFFF empty-row sentinel. */
    memset(header16 + 10, 0xFF, (size_t)rowCount * 2);
    g_cod4LightGridHeaderSize = 20 + 2 * rowCount;

    /* 0x413320 / 0x413160: per row — 12-byte row header, then runs of
     * contiguous columns sharing one z-span, gaps as skip records, and the
     * row's record stream padded to a 4-byte boundary. */
    g_cod4LightGridRowsSize = 0;
    i = 0;
    while (i < cellCount)
    {
        int rowBegin = i, rowEnd, j;
        unsigned short rowCoord = cells[i].pos[axis];
        unsigned short rowZMin = cells[i].pos[2];
        unsigned short rowZMax = cells[i].pos[2];
        unsigned char *rowData;
        int rowHeightOver255;
        int runLen = 0, runBegin = 0;
        unsigned short runStartCol = 0, runZFirst = 0, runZLast = 0;

        for (rowEnd = i; rowEnd < cellCount
             && cells[rowEnd].pos[axis] == rowCoord; ++rowEnd)
        {
            if (cells[rowEnd].pos[2] < rowZMin) rowZMin = cells[rowEnd].pos[2];
            if (cells[rowEnd].pos[2] > rowZMax) rowZMax = cells[rowEnd].pos[2];
        }

        if ((g_cod4LightGridRowsSize >> 2) >= 0xFFFF)
            ErrorMsg("light grid row data exceeds the 16-bit header offset range\n");
        header16[10 + rowCoord - mins[axis]] =
            (unsigned short)(g_cod4LightGridRowsSize >> 2);
        rowData = g_cod4LightGridRows + g_cod4LightGridRowsSize;
        *(unsigned short *)(rowData + 0) = cells[rowBegin].pos[axis ^ 1];
        *(unsigned short *)(rowData + 2) = (unsigned short)
            (cells[rowEnd - 1].pos[axis ^ 1] - cells[rowBegin].pos[axis ^ 1] + 1);
        *(unsigned short *)(rowData + 4) = rowZMin;
        *(unsigned short *)(rowData + 6) = (unsigned short)(rowZMax - rowZMin + 1);
        *(unsigned int *)(rowData + 8) = (unsigned int)entryCount;
        g_cod4LightGridRowsSize += 12;
        rowHeightOver255 = (rowZMax - rowZMin + 1) > 255;

        for (j = rowBegin; j < rowEnd; )
        {
            int colBegin = j;
            unsigned short col = cells[j].pos[axis ^ 1];
            unsigned short colZFirst, colZLast;

            while (j < rowEnd && cells[j].pos[axis ^ 1] == col)
                ++j;
            colZFirst = cells[colBegin].pos[2];
            colZLast = cells[j - 1].pos[2];

            if (runLen && col == (unsigned short)(runStartCol + runLen)
                && colZFirst == runZFirst && colZLast == runZLast
                && runLen < 255)
            {
                ++runLen;
            }
            else
            {
                if (runLen)
                {
                    LightGrid_FlushRun(runLen, runZFirst, runZLast, rowZMin,
                                       rowHeightOver255, cells, runBegin,
                                       colBegin, &entryCount);
                    if (col != (unsigned short)(runStartCol + runLen))
                        LightGrid_WriteSkip(
                            (unsigned int)(unsigned short)(col - runStartCol - runLen));
                }
                runStartCol = col;
                runZFirst = colZFirst;
                runZLast = colZLast;
                runBegin = colBegin;
                runLen = 1;
            }
        }
        if (runLen)
            LightGrid_FlushRun(runLen, runZFirst, runZLast, rowZMin,
                               rowHeightOver255, cells, runBegin, rowEnd,
                               &entryCount);

        g_cod4LightGridRowsSize = (g_cod4LightGridRowsSize + 3) & ~3;
        i = rowEnd;
    }

    g_cod4LightGridEntryCount = entryCount;

    free(cells);
    free(g_cod4LightGridAssignments);
    g_cod4LightGridAssignments = NULL;
}

/*
 * CalculateLightGrid_Worker — calculate light grid for a single point.
 * Address: 0x411910 | Size: 141 bytes
 *
 * ecx=gridIndex, edx=flags
 * Allocates a grid sample, gathers incident light, encodes SH, stores result.
 */
void CalculateLightGrid_Worker(int gridIndex, int flags)
{
    void *gridPoint;
    GridSampleResult *sample;
    float lightBuffer[24]; /* 96 bytes = 0x60, zeroed */
    float cod4Vector[56 * 3];

    gridPoint = (void *)&g_gridPoints[gridIndex];
    sample = (GridSampleResult *)AllocGridSample(flags, gridPoint);
    if (!sample)
        return;

    /* 0x4116B0 fixes the cell's primary light before the gather: the sun if
     * visible (coordBits bit 0, set by AllocGridSample's octant trace), else
     * the unique in-range light, else the near-sun probe. */
    if (g_gridPointPrimaries)
    {
        GridSamplePoint_t *pt = (GridSamplePoint_t *)gridPoint;
        float pos[3];
        unsigned char primary;

        pos[0] = (float)((int)pt->x - 0x1000) * 32.0f;
        pos[1] = (float)((int)pt->y - 0x1000) * 32.0f;
        pos[2] = (float)((int)pt->z - 0x800) * 64.0f;
        primary = (sample->coordBits & 1)
            ? (unsigned char)g_sunPrimaryLightIndex : 0;
        if (!primary)
            primary = LightGrid_FallbackPrimary(flags, pos);
        g_gridPointPrimaries[gridIndex] = primary;
    }

    memset(lightBuffer, 0, 0x60);
    CalculateLightGrid_GatherLightInternal(flags, gridIndex, lightBuffer,
                                           cod4Vector);

    LightGrid_StoreProducerVector(gridIndex, cod4Vector);

    sample->colorIndex = LightGrid_EncodeSH(1.0f, lightBuffer);
}

/*
 * AllocGridTraceDirections — allocate and initialize random trace direction vectors.
 * Address: 0x4119C0 | Size: 235 bytes
 *
 * Allocates g_numTraceDirections * 12 bytes (3 floats per direction).
 * Generates random directions using rand() scaled by a constant,
 * converts to unit vectors via PointOnSphereFromUniformDeviates, then normalizes the array.
 */
void AllocGridTraceDirections(void)
{
    int i;
    float scale = 3.0517578e-5f; /* dword_458B18 = 0x38000000 ~ 1/32768 */

    /* allocate: numDirections * 3 * 4 bytes */
    g_traceDirections = (float *)malloc((unsigned long long)g_numTraceDirections * 3 * sizeof(float));
    if (!g_traceDirections)
        ErrorMsg("Couldn't allocate %i bytes for grid trace directions\n",
                    g_numTraceDirections * 3 * (int)sizeof(float));

    /* LST 0x411A45-0x411A83: call rand_int twice per iter, compute a random
     * sphere point via PointOnSphereFromUniformDeviates (sub_428C50).
     * This pass only advances rand state — the array is immediately
     * overwritten by the Fibonacci-spiral call below. */
    for (i = 0; i < g_numTraceDirections; i++)
    {
        float theta, phi;
        float *dir;

        theta = (float)rand_int() * scale;
        phi = (float)rand_int() * scale;

        dir = g_traceDirections + i * 3;
        PointOnSphereFromUniformDeviates(theta, phi, dir);
    }

    /* LST 0x411AA6: jmp com_math_428EF0 = UniformPointsOnSphere(count, arr, 12).
     * Deterministic golden-angle spiral; overwrites the random array above. */
    UniformPointsOnSphere(g_numTraceDirections, g_traceDirections, 12);
    LightGrid_BuildCod4Basis();
}

/*
 * AddStaticModelLightGridSamples — load grid points from file, allocate buffer.
 * Address: 0x4100E0 | Size: 695 bytes
 *
 * The retail source sequence is .grid, .vclog, .grid_auto, then static-model
 * origin expansion.  These helpers preserve its fixed record validation and
 * automatic/authored point merging.
 */
/* CoD4Rad sub_40F970 centralizes validation for .grid, .grid_auto and
 * .vclog.  Keep the fixed-record checks shared so 0x410C90's source
 * precedence remains explicit in this port. */
static int LightGrid_OpenPointFile(const char *suffix, int recordSize,
                                   char *filePath, void **outFile,
                                   int *outPointCount)
{
    char *end;
    const char *src;
    int fileSize;
    void *file;

    BuildFilePath(g_gridLogBasePath, filePath);
    end = filePath;
    while (*end) ++end;
    src = suffix;
    while ((*end++ = *src++) != '\0')
        ;

    file = fopen_wrap(filePath, "rb");
    if (!file)
        return 0;

    fseek_wrap(file, 0, 2);
    fileSize = ftell_wrap(file);
    fseek_wrap(file, 0, 0);
    if (fileSize <= 0 || (fileSize % recordSize) != 0)
    {
        WarningMsg(1,
            "Ignoring light-grid point file '%s': size %i is not a positive multiple of %i\n",
            filePath, fileSize, recordSize);
        fclose_wrap(file);
        return 0;
    }

    *outFile = file;
    *outPointCount = fileSize / recordSize;
    return 1;
}

static void LightGrid_AllocatePointBuffer(int sourcePointCount)
{
    int totalPointCount = sourcePointCount + g_gridSampleCount * 8;
    unsigned long long byteCount;

    if (totalPointCount <= 0)
        return;

    byteCount = (unsigned long long)totalPointCount * sizeof(GridSamplePoint_t);
    g_gridPoints = (GridSamplePoint_t *)malloc(byteCount);
    if (!g_gridPoints)
    {
        ErrorMsg("couldn't allocate %.2f MB for the light grid points\n",
                 (double)((float)byteCount * (1.0f / (1024.0f * 1024.0f))));
    }
}

static void LightGrid_ReadSixBytePoints(void *file, const char *filePath,
                                        int pointCount)
{
    long long readCount = fread_wrap(&g_gridPoints[g_gridPointCount],
                                     sizeof(GridSamplePoint_t), pointCount, file);
    if (readCount != pointCount)
        ErrorMsg("Error while reading %s\n", filePath);
    g_gridPointCount += pointCount;
}

void AddStaticModelLightGridSamples(void)
{
    char gridPath[0x400];
    char autoPath[0x400];
    void *gridFile = NULL;
    void *autoFile = NULL;
    int gridPointCount = 0;
    int autoPointCount = 0;

    /* assert: pointCount == 0 (line 0x62) */
    Assert("(lightGridGlob.pointCount == 0)", ".\\lightgrid.cpp", 0x62, 0, 1);

    /* assert: points == NULL (line 0x63) */
    Assert("lightGridGlob.points == NULL", ".\\lightgrid.cpp", 0x63, 0, 1);

    /* sub_40FCF0 only wins when an authored .grid exists.  Its allocator
     * sub_40FB70 prepends .grid_auto, so preserve that merge and ordering. */
    if (!LightGrid_OpenPointFile(".grid", sizeof(GridSamplePoint_t),
                                 gridPath, &gridFile, &gridPointCount))
        return;

    LightGrid_OpenPointFile(".grid_auto", sizeof(GridSamplePoint_t),
                            autoPath, &autoFile, &autoPointCount);
    LightGrid_AllocatePointBuffer(gridPointCount + autoPointCount);

    if (autoFile)
    {
        Com_Printf("Using %i automatic light-grid points from '%s'\n",
                   autoPointCount, autoPath);
        LightGrid_ReadSixBytePoints(autoFile, autoPath, autoPointCount);
        fclose_wrap(autoFile);
    }

    Com_Printf("Using %i authored light-grid points from '%s'\n",
               gridPointCount, gridPath);
    LightGrid_ReadSixBytePoints(gridFile, gridPath, gridPointCount);
    fclose_wrap(gridFile);
}

/*
 * AllocGridSample — allocate and initialize a grid sample entry.
 * Address: 0x410F50 | Size: 266 bytes
 *
 * ecx=flags, rdx=gridPoint (GridSamplePoint*)
 * Returns a temporary annotated entry in the global sample array.
 */
GridSampleResult *AllocGridSample(int flags, GridSamplePoint_t *pt)
{
    float pos[3];
    GridSampleResult *entry;
    int packed;
    unsigned char packedByte;

    pos[0] = (float)((int)pt->x - 0x1000) * 32.0f;
    pos[1] = (float)((int)pt->y - 0x1000) * 32.0f;
    pos[2] = (float)((int)pt->z - 0x800) * 64.0f;

    AcquireThreadLock((unsigned int)(uintptr_t)g_gridSampleArray);

    entry = &((GridSampleResult *)g_gridSampleArray)[g_gridSampleArrayCount];
    g_gridSampleArrayCount++;

    ReleaseThreadLock((unsigned int)(uintptr_t)g_gridSampleArray);

    memset(entry, 0, sizeof(*entry));
    entry->producerIndex = (unsigned int)(pt - g_gridPoints);

    /* pack grid coordinates */
    packed = ((int)pt->x << 19) & (int)0xFFE003FF;
    packed |= ((int)pt->y << 8);
    packed &= (int)0xFFFFFC00;
    packed |= ((int)pt->z >> 2);

    packedByte = ((pt->y & 3) | ((pt->x & 3) << 2));
    packedByte = (packedByte << 2) | (pt->z & 3);
    packedByte <<= 2;

    entry->packedCoords = packed;
    entry->coordBits = packedByte;

    TraceOctantSkyVisibility(flags, pos, (unsigned char *)entry);

    return entry;
}

/*
 * GatherIncidentEnergyInSpaceForLightFromDir — accumulate SH lighting from a direction.
 * Address: 0x411060 | Size: 534 bytes
 *
 * rcx=lightColor (float[3]), rdx=direction (float[3]), r8=outBuffer (float[24])
 *
 * For each set of 4 SH basis vectors (stored in global constant tables):
 *   Compute dot(direction, basisVector), if > 0 accumulate color * dot into output.
 * The loop processes 4 basis vectors per iteration, advancing by 0x30 (48 bytes).
 * Output has 8 bands of 3 floats = 24 floats starting at outBuffer[5] (offset 0x14).
 */
void GatherIncidentEnergyInSpaceForLightFromDir(float *lightColor, float *direction, float *outBuffer)
{
    float *basis = g_shBasis;
    float *out = outBuffer + 5; /* start at offset 0x14 = 5 floats */
    float *basisEnd = g_shBasis + 24; /* 8 SH basis vectors * 3 components = 24 floats, binary uses 0x68 bytes */
    int i;

    /* iterate SH basis sets — 4 basis vectors per set, each a vec3 */
    /* the loop is unrolled: processes 4 dot products per iteration */
    while (basis < basisEnd)
    {
        /* basis vector 0 */
        {
            float dot = direction[0] * basis[0] + direction[1] * basis[1] + direction[2] * basis[2];
            if (dot > 0.0f)
            {
                out[-5] += dot * lightColor[0];
                out[-4] += dot * lightColor[1];
                out[-3] += dot * lightColor[2];
            }
        }

        /* basis vector 1 */
        {
            float dot = direction[0] * basis[3] + direction[1] * basis[4] + direction[2] * basis[5];
            if (dot > 0.0f)
            {
                out[-2] += dot * lightColor[0];
                out[-1] += dot * lightColor[1];
                out[0] += dot * lightColor[2];
            }
        }

        /* basis vector 2 */
        {
            float dot = direction[0] * basis[6] + direction[1] * basis[7] + direction[2] * basis[8];
            if (dot > 0.0f)
            {
                out[1] += dot * lightColor[0];
                out[2] += dot * lightColor[1];
                out[3] += dot * lightColor[2];
            }
        }

        /* basis vector 3 */
        {
            float dot = direction[0] * basis[9] + direction[1] * basis[10] + direction[2] * basis[11];
            if (dot > 0.0f)
            {
                out[4] += dot * lightColor[0];
                out[5] += dot * lightColor[1];
                out[6] += dot * lightColor[2];
            }
        }

        basis += 12; /* advance by 48 bytes = 12 floats */
        out += 12;
    }
}

void CalculateLightGrid_GatherLight(int flags, int gridIndex, float *outBuffer)
{
    CalculateLightGrid_GatherLightInternal(flags, gridIndex, outBuffer, NULL);
}

/*
 * CalculateLightGrid_GatherLight — gather all lighting for a grid point.
 * Address: 0x411280 | Size: 937 bytes
 *
 * ecx=flags, edx=gridIndex, r8=outBuffer (float[24] = 96 bytes)
 *
 * Gathers incident light from:
 *   1. Trace directions (sky/bounce light via FindLightingSamplesAndNormal)
 *   2. Point lights (via PointLightEvaluatePoint)
 * Results accumulated into outBuffer via GatherIncidentEnergyInSpaceForLightFromDir.
 */
static void CalculateLightGrid_GatherLightInternal(int flags, int gridIndex,
                                                   float *outBuffer,
                                                   float *cod4Vector)
{
    float pos[3];
    float localColor[3];
    float lightDir[3];
    float lightColor[3];
    float hitData[16]; /* hit results from FindLightingSamplesAndNormal */
    GridSamplePoint_t *pts = (GridSamplePoint_t *)g_gridPoints;
    GridSamplePoint_t *pt = &pts[gridIndex];
    int i, j;

    /* convert grid point to float position */
    pos[0] = (float)((int)pt->x - 0x1000) * 32.0f;
    pos[1] = (float)((int)pt->y - 0x1000) * 32.0f;
    pos[2] = (float)((int)pt->z - 0x800) * 64.0f;

    /* Native 0x4116B0 starts all 56 renderer samples at world ambient. */
    memset(outBuffer, 0, 0x60);
    if (cod4Vector)
    {
        for (i = 0; i < 56; ++i)
        {
            cod4Vector[i * 3 + 0] = g_ambientR;
            cod4Vector[i * 3 + 1] = g_ambientG;
            cod4Vector[i * 3 + 2] = g_ambientB;
        }
    }

    /* gather from trace directions */
    for (i = 0; i < g_numTraceDirections; i++)
    {
        float *dir = g_traceDirections + i * 3;
        int hitCount;

        hitCount = FindLightingSamplesAndNormalNearest(flags, pos, dir, 262144.0f,
            hitData, NULL);

        if (hitCount == 0)
            continue;

        if (hitCount == -1)
        {
            /* sky hit — use global sky light scale */
            GatherIncidentEnergyInSpaceForLightFromDir(
                &g_gridLightScale0, dir, outBuffer);
            if (cod4Vector)
                LightGrid_AccumulateCod4Vector(&g_gridLightScale0, dir,
                                                cod4Vector);
            continue;
        }

        /* process lighting hits */
        for (j = 0; j < hitCount; j++)
        {
            void *sample = *(void **)&hitData[j * 4];
            float weight = hitData[j * 4 + 2];

            Lighting_GetGatheredLight(sample, localColor);

            /* scale color by weight and global scale */
            localColor[0] *= g_gridLightScale3 * weight;
            localColor[1] *= g_gridLightScale3 * weight;
            localColor[2] *= g_gridLightScale3 * weight;

            GatherIncidentEnergyInSpaceForLightFromDir(localColor, dir, outBuffer);
            if (cod4Vector)
                LightGrid_AccumulateCod4Vector(localColor, dir, cod4Vector);
        }
    }

    /* gather from point lights.  0x4116B0: the evaluation is keyed by the
     * cell's PRIMARY light — a light that IS the cell's primary returns 1
     * and is skipped (the renderer applies it at runtime from the grid's
     * primary byte); 2 is a directional contribution, 3 a coincident/near
     * one. */
    {
        int numLights = GetPointLightCount();
        unsigned char cellPrimary = g_gridPointPrimaries
            ? g_gridPointPrimaries[gridIndex] : 0;
        for (i = 0; i < numLights; i++)
        {
            int result = PointLightEvaluatePoint((int)cellPrimary, flags, i,
                pos, NULL, lightDir, lightColor, NULL);

            if (result == 2)
            {
                /* far light — use directional SH accumulation */
                GatherIncidentEnergyInSpaceForLightFromDir(lightColor, lightDir, outBuffer);
                if (cod4Vector)
                    LightGrid_AccumulateCod4Vector(lightColor, lightDir,
                                                    cod4Vector);
            }
            else if (result == 3)
            {
                /* near light — add color directly to all 24 SH coefficients */
                for (j = 0; j < 24; j++)
                {
                    outBuffer[j] += lightColor[j % 3];
                }
                if (cod4Vector)
                    LightGrid_AddUniformCod4Vector(lightColor, cod4Vector);
            }
        }
    }
}

/*
 * LightGrid_EncodeSH — encode SH lighting buffer as gamma-corrected bytes.
 * Address: 0x4117D0 | Size: 320 bytes
 *
 * rcx=buffer (float[24]), rdx=scale(unused, passed to AdjustLightingContrast as baseIndex=-1)
 * Returns encoded color index (short) from LightGrid_FindOrInsertColor.
 */
short LightGrid_EncodeSH(float scale, float *buffer)
{
    float adjusted[8 * 3];     /* 24 floats adjusted */
    unsigned char encoded[24]; /* gamma-corrected bytes, band-major */
    unsigned char rearranged[24]; /* channel-major layout */
    int band, ch;
    /* adjust contrast: 8 SH bands, baseIndex=-1 (use midpoint) */
    AdjustLightingContrast(8, -1, buffer, adjusted);

    /* encode each value as gamma-corrected byte */
    for (band = 0; band < 8; band++)
    {
        for (ch = 0; ch < 3; ch++)
        {
            encoded[band * 3 + ch] = EncodeGammaCorrectedByte(adjusted[band * 3 + ch]);
        }
    }

    /* rearrange: transpose from band-major [b0r,b0g,b0b, b1r,...] to
     * channel-major blocks of 8 bytes each [r0,r1,...,r7, g0,g1,...,g7, b0,...,b7] */
    for (ch = 0; ch < 3; ch++)
    {
        for (band = 0; band < 8; band++)
        {
            rearranged[ch * 8 + band] = encoded[band * 3 + ch];
        }
    }

    return LightGrid_FindOrInsertColor(rearranged);
}

/*
 * LightGrid_FindOrInsertColor — find or insert a color entry in the grid color table.
 * Address: 0x411630 | Size: 414 bytes
 *
 * rcx=colorData (24 bytes)
 * Returns color index (short).
 *
 * Searches existing entries for a match. If found, returns existing index.
 * Otherwise inserts new entry and returns new index.
 */
short LightGrid_FindOrInsertColor(unsigned char *colorData)
{
    int i, ch, band;
    short bestIdx = -1;
    int bestSAD = 0x60; /* initial threshold = 96 */
    int bestMaxDiff = 8;

    AcquireThreadLock((unsigned int)(uintptr_t)g_gridColorEntries);

    if (g_gridColorCount <= 0)
        goto insert_new;

    for (i = 0; i < g_gridColorCount; i++)
    {
        unsigned char *entry = g_gridColorEntries + i * 24;
        unsigned char *input = colorData;
        int sad = 0;
        int maxDiff = 0;

        for (ch = 0; ch < 3; ch++)
        {
            for (band = 0; band < 8; band++)
            {
                int diff = (int)input[band] - (int)entry[band];
                int absDiff = diff < 0 ? -diff : diff;
                sad += absDiff;

                if (sad > bestSAD)
                    goto next_entry;

                if (absDiff > maxDiff)
                {
                    maxDiff = absDiff;
                    if (absDiff > bestMaxDiff)
                        goto next_entry;
                }
            }
            entry += 8;
            input += 8;
        }

        /* exact match */
        if (sad == 0)
        {
            ReleaseThreadLock((unsigned int)(uintptr_t)g_gridColorEntries);
            return (short)i;
        }

        bestIdx = (short)i;
        bestMaxDiff = maxDiff;
        bestSAD = sad;

    next_entry:
        ;
    }

    if (bestIdx >= 0)
    {
        /* close enough match */
        ReleaseThreadLock((unsigned int)(uintptr_t)g_gridColorEntries);
        return bestIdx;
    }

insert_new:
    /* check limit */
    if (g_gridColorCount == 0xFFFF)
    {
        ErrorMsg("MAX_MAP_LIGHTGRID_COLORS (%i) exceeded\n", g_gridColorCount);
    }

    /* insert new entry: copy 24 bytes */
    {
        int newIdx = g_gridColorCount;
        unsigned char *dst = g_gridColorEntries + newIdx * 24;
        g_gridColorCount = newIdx + 1;

        *(long long *)dst = *(long long *)colorData;
        *(long long *)(dst + 8) = *(long long *)(colorData + 8);
        *(long long *)(dst + 16) = *(long long *)(colorData + 16);

        ReleaseThreadLock((unsigned int)(uintptr_t)g_gridColorEntries);
        return (short)newIdx;
    }
}

/*
 * CalculateLightGrid — main entry point for light grid calculation.
 * Address: 0x411AB0 | Size: 390 bytes
 *
 * ecx=flags
 */
static void LightGrid_AppendDefaultProducerVector(void)
{
    float vector[56 * 3];
    int sample, directionIndex;

    for (sample = 0; sample < 56; ++sample)
    {
        vector[sample * 3 + 0] = g_ambientR;
        vector[sample * 3 + 1] = g_ambientG;
        vector[sample * 3 + 2] = g_ambientB;
    }
    for (directionIndex = 0; directionIndex < g_numTraceDirections;
         ++directionIndex)
    {
        float *direction = &g_traceDirections[directionIndex * 3];
        if (direction[2] > 0.0f)
            LightGrid_AccumulateCod4Vector(&g_gridLightScale0, direction,
                                            vector);
    }
    /* 0x411970 appends after the produced sample array, rather than after
     * the pre-worker source-point allocation. */
    LightGrid_StoreProducerVector(g_gridSampleArrayCount, vector);
}

void CalculateLightGrid(int flags)
{
    int numDirs;
    int sourcePointCount;
    float dirScale;

    /* clear counters */
    g_gridColorCount = 0;
    g_gridSampleArrayCount = 0;
    g_cod4LightGridColorCount = 0;
    g_cod4LightGridEntryCount = 0;
    g_cod4LightGridHeaderSize = 0;
    g_cod4LightGridRowsSize = 0;

    /* assert: pointCount == 0 (line 0x142) */
    Assert("lightGridGlob.pointCount == 0", ".\\lightgrid.cpp", 0x142, 0, 1);

    /* load grid points from file */
    AddStaticModelLightGridSamples();

    if (g_gridPointCount == 0)
    {
        /* try vis cache */
        CalculateLightGrid_Setup();

        if (g_gridPointCount == 0)
        {
            /* no file points — allocate for static model samples only */
            if (g_gridSampleCount == 0)
            {
                WarningMsg(1,
                    "WARNING: no light-grid points were found in .grid, .vclog, .grid_auto, or shadow-enabled static models; modern light-grid lumps will not be generated.\n");
                goto done;
            }

            LightGrid_AllocatePointBuffer(0);
        }
    }

    if (g_gridSampleCount > 0)
    {
        Com_Printf("Using %i shadow-enabled static-model light-grid seed origins\n",
                   g_gridSampleCount);
    }

    /* expand static model origins into grid points */
    ExpandStaticModelOrigins();
    sourcePointCount = g_gridPointCount;

    /* sort, validate, deduplicate */
    CalculateLightGrid_SortPoints();
    /* Retail 0x410870 applies .grid_not after reject/dedup, so exclusions
     * affect automatic, vis-cache, and model-origin points alike. */
    LightGrid_ApplyGridNot();

    if (g_gridPointCount == 0)
    {
        WarningMsg(1,
            "WARNING: all %i light-grid source points were rejected or excluded; modern light-grid lumps will not be generated.\n",
            sourcePointCount);
        goto done;
    }

    WarningMsg(1,
        "WARNING: CoD4 light-grid octant partitioning (0x40CF50/0x40D920/0x40D990) and spot-light primary narrowing (0x4111C0) remain approximate; output is structurally compatible but not byte-equivalent to retail.\n");

    /* allocate trace directions */
    AllocGridTraceDirections();

    /* compute lighting scale factors */
    numDirs = g_numTraceDirections;
    dirScale = 4.0f / (float)numDirs;

    /* Native 0x413630 copies E98/E9C/EA0 into the grid sky scale.  Those
     * globals are the diffuse sky/radiosity colour, not the E8C bounce-ping
     * colour used while seeding surface radiosity. */
    g_gridLightScale0 = g_sunRadiosityR * dirScale;
    g_gridLightScale1 = g_sunRadiosityG * dirScale;
    g_gridLightScale2 = g_sunRadiosityB * dirScale;
    g_gridLightScale3 = dirScale;

    g_cod4LightGridVectors = (unsigned char *)malloc(
        (unsigned long long)(g_gridPointCount + 1) * 168);
    if (!g_cod4LightGridVectors)
        ErrorMsg("Couldn't allocate %i bytes for light grid producer vectors\n",
                 (g_gridPointCount + 1) * 168);

    g_gridPointPrimaries = (unsigned char *)calloc((size_t)g_gridPointCount + 1, 1);

    /* calculate lighting for each grid point */
    ForEachLightmapPixel(g_gridPointCount, (void *)CalculateLightGrid_Worker, flags);
    LightGrid_AppendDefaultProducerVector();

    /* sort results */
    qsort(g_gridSampleArray, g_gridSampleArrayCount, sizeof(GridSampleResult),
          (void *)GridSamplePoint_CompareForSort);
    LightGrid_BuildCod4Compact();
    if (!g_cod4LightGridHeaderSize || !g_cod4LightGridRowsSize
        || !g_cod4LightGridEntryCount || !g_cod4LightGridColorCount)
    {
        WarningMsg(1,
            "WARNING: CoD4 light-grid encoding was incomplete (header=%i rows=%i entries=%i colors=%i); the output BSP will not contain a usable modern light grid.\n",
            g_cod4LightGridHeaderSize, g_cod4LightGridRowsSize,
            g_cod4LightGridEntryCount, g_cod4LightGridColorCount);
    }
    else
    {
        Com_Printf("Built CoD4 light grid: %i source points, %i entries, %i colors, %i header bytes, %i row bytes\n",
                   g_gridPointCount, g_cod4LightGridEntryCount,
                   g_cod4LightGridColorCount, g_cod4LightGridHeaderSize,
                   g_cod4LightGridRowsSize);
    }
    free(g_gridPointPrimaries);
    g_gridPointPrimaries = NULL;
    free(g_cod4LightGridVectors);
    g_cod4LightGridVectors = NULL;

done:
    return;
}

/*
 * CalculateLightGrid_Setup — load vis cache grid points from file.
 * Address: 0x4103A0 | Size: 800 bytes
 *
 * Reads optional 24-byte .vclog records, merges six-byte .grid_auto points,
 * and allocates spare capacity for static-model origin expansion.
 */
void CalculateLightGrid_Setup(void)
{
    char visPath[0x400];
    char autoPath[0x400];
    char record[24];
    void *visFile = NULL;
    void *autoFile = NULL;
    int visPointCount = 0;
    int autoPointCount = 0;
    int i;

    /* assert: pointCount == 0 (line 0x90) */
    Assert("(lightGridGlob.pointCount == 0)", ".\\lightgrid.cpp", 0x90, 0, 1);

    /* assert: points == NULL (line 0x91) */
    Assert("lightGridGlob.points == NULL", ".\\lightgrid.cpp", 0x91, 0, 1);

    /* 0x410C90 tries .vclog after an absent .grid.  Whether .vclog exists
     * or not, allocator sub_40FB70 then merges .grid_auto before it. */
    LightGrid_OpenPointFile(".vclog", sizeof(record),
                            visPath, &visFile, &visPointCount);
    LightGrid_OpenPointFile(".grid_auto", sizeof(GridSamplePoint_t),
                            autoPath, &autoFile, &autoPointCount);

    if (!visFile && !autoFile)
        return;

    LightGrid_AllocatePointBuffer(visPointCount + autoPointCount);

    if (autoFile)
    {
        Com_Printf("Using %i automatic light-grid points from '%s'\n",
                   autoPointCount, autoPath);
        LightGrid_ReadSixBytePoints(autoFile, autoPath, autoPointCount);
        fclose_wrap(autoFile);
    }

    if (!visFile)
        return;

    Com_Printf("Using %i light-grid points from vis cache '%s'\n",
               visPointCount, visPath);
    for (i = 0; i < visPointCount; i++)
    {
        long long readCount;
        GridSamplePoint_t *pt;

        readCount = fread_wrap(record, sizeof(record), 1, visFile);
        if (readCount != 1)
        {
            fclose_wrap(visFile);
            ErrorMsg("Error while reading %s\n", visPath);
            return;
        }

        pt = &g_gridPoints[g_gridPointCount];
        pt->x = *(unsigned short *)&record[0];
        pt->y = *(unsigned short *)&record[4];
        pt->z = *(unsigned short *)&record[8];
        ++g_gridPointCount;
    }

    fclose_wrap(visFile);
}

/*
 * CalculateLightGrid_SortPoints — sort, validate, and deduplicate grid points.
 * Address: 0x410860 | Size: 871 bytes
 *
 * 1. Assert pointCount > 0, qsort by GridSamplePoint_Compare
 * 2. Reverse-check last point validity (vis trace + material check)
 * 3. Forward-iterate: remove duplicates and invalid points
 *
 * Constants: dword_458A94 = Z height offset for trace, dword_4577D8 = Z below offset
 */
/* Retail 0x410510: a point must see the world.  Each axis ray must land on
 * a front-facing, unflagged surface (or sky) for the point to survive; a
 * point buried in solid geometry only ever sees backfaces and is removed. */
extern int Lighting_RejectTransportPoint(int sampleIdx, const float *point);

static int LightGrid_SeesNoWorld(const float *pos)
{
    static const float kProbeDirs[6][3] = {
        { 0.0f, 0.0f, -262144.0f }, { 0.0f, 0.0f, 262144.0f },
        { -262144.0f, 0.0f, 0.0f }, { 262144.0f, 0.0f, 0.0f },
        { 0.0f, -262144.0f, 0.0f }, { 0.0f, 262144.0f, 0.0f },
    };
    int i;

    for (i = 0; i < 6; ++i)
    {
        float end[3];
        RayHitResult_t hit;

        end[0] = pos[0] + kProbeDirs[i][0];
        end[1] = pos[1] + kProbeDirs[i][1];
        end[2] = pos[2] + kProbeDirs[i][2];
        TraceSetup_and_Dispatch(0, (float *)pos, end, &hit);
        if (!hit.triangle)
            continue;
        if (!(hit.triangle->material->surfaceFlags & 0x80))
        {
            float facing = hit.triangle->normal[0] * kProbeDirs[i][0]
                         + hit.triangle->normal[1] * kProbeDirs[i][1]
                         + hit.triangle->normal[2] * kProbeDirs[i][2];
            if (facing < 0.0f)
                return 0;
        }
        if (hit.triangle->material->surfaceFlags & SURF_SKY)
            return 0;
    }
    return 1;
}

/* Retail 0x410610/0x410760 prune the loaded point list harder than the x64
 * build's own validation below: points outside the world bounds, points
 * against geometry, points failing the under-world transport test, and
 * points that see no world at all are all dropped before any worker runs. */
static int LightGrid_RetailRejectsPoint(const GridSamplePoint_t *pt)
{
    float pos[3];

    pos[0] = (float)((int)pt->x - 0x1000) * 32.0f;
    pos[1] = (float)((int)pt->y - 0x1000) * 32.0f;
    pos[2] = (float)((int)pt->z - 0x800) * 64.0f;

    /* 0x409E40 tests against the world bounds; the worldspawn model's box
     * is the same quantity here. */
    if (numBSPModels > 0)
    {
        const BspModel_t *world = &bspModels[0];
        if (pos[0] < world->mins[0] || pos[0] > world->maxs[0]
            || pos[1] < world->mins[1] || pos[1] > world->maxs[1]
            || pos[2] < world->mins[2] || pos[2] > world->maxs[2])
            return 1;
    }
    if (LightGrid_PointNearGeometry(pos))
        return 1;                               /* 0x410370 head (0x409F70) */
    if (Lighting_RejectTransportPoint(0, pos))
        return 1;                               /* 0x409E10 */
    if (LightGrid_SeesNoWorld(pos))
        return 1;                               /* 0x410510 */
    return 0;
}

void CalculateLightGrid_SortPoints(void)
{
    float zOffset;
    float zBelow;
    int i;

    /* assert: pointCount > 0 (line 0x12B) */
    Assert("(lightGridGlob.pointCount > 0)", ".\\lightgrid.cpp", 0x12B, 0, 1);

    /* sort grid points */
    qsort(g_gridPoints, g_gridPointCount, 6, (void *)GridSamplePoint_Compare);

    zOffset = 64.0f;    /* dword_458A94 = 0x42800000 */
    zBelow = 262144.0f; /* dword_4577D8 = 0x48800000 */

    /* reverse-check: validate last point, remove if invalid */
    {
        int lastIdx = g_gridPointCount - 1;
        GridSamplePoint_t *pts = (GridSamplePoint_t *)g_gridPoints;
        float pos[3], traceEnd[3];
        RayHitResult_t hit;

        while (lastIdx > 0)
        {
            pos[0] = (float)((int)pts[lastIdx].x - 0x1000) * 32.0f;
            pos[1] = (float)((int)pts[lastIdx].y - 0x1000) * 32.0f;
            pos[2] = (float)((int)pts[lastIdx].z - 0x800) * 64.0f;

            traceEnd[0] = pos[0];
            traceEnd[1] = pos[1];
            traceEnd[2] = pos[2] + zOffset;

            if (TraceVisibility(0, traceEnd, pos))
                break;

            traceEnd[0] = pos[0];
            traceEnd[1] = pos[1];
            traceEnd[2] = pos[2] - zBelow;
            TraceSetup_and_Dispatch(0, pos, traceEnd, &hit);

            if (hit.triangle)
            {
                MaterialDef_t *mat = hit.triangle->material;
                if (!(mat->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY)))
                {
                    if (mat->surfaceFlags != 1)
                        break;
                    if (!((*(unsigned char *)&mat->contents) & 0x80))
                        break;
                }
            }

            /* remove last point */
            g_gridPointCount--;
            lastIdx = g_gridPointCount - 1;
            if (lastIdx <= 0)
                goto epilogue;
        }
    }

    /* forward-iterate: deduplicate and validate.
     * Mirror orig's loop structure exactly — rdi is the iteration counter
     * starting at count-1, rbx is the byte offset of CURRENT (starts at
     * 6*(count-1)), esi is the index of NEXT (starts at count, so the first
     * iteration compares against past-end memory — that read is garbage but
     * orig never treats it as a duplicate unless current happens to match
     * that garbage). Keeping orig's iteration count matters because the
     * first iteration still runs the trace/material validation on the
     * reverse-check-approved last element. */
    {
        long long rbx;       /* byte offset of current */
        int esi;             /* index of next (initially past-end) */
        int rdi;             /* iteration counter */

        if (g_gridPointCount - 1 <= 0)
            goto done_dedup;

        rdi = g_gridPointCount - 1;
        rbx = 6LL * (g_gridPointCount - 1);
        esi = g_gridPointCount;

        do
        {
            GridSamplePoint_t *current = (GridSamplePoint_t *)((char *)g_gridPoints + rbx);
            GridSamplePoint_t *next    = &g_gridPoints[esi];

            /* duplicate check (first iter reads past-end garbage for next) */
            if (current->x == next->x && current->y == next->y && current->z == next->z)
                goto remove_point;

            {
                float pos[3], traceEnd[3];
                RayHitResult_t hit;

                pos[0] = (float)((int)current->x - 0x1000) * 32.0f;
                pos[1] = (float)((int)current->y - 0x1000) * 32.0f;
                pos[2] = (float)((int)current->z - 0x800) * 64.0f;

                traceEnd[0] = pos[0];
                traceEnd[1] = pos[1];
                traceEnd[2] = pos[2] + zOffset;

                if (TraceVisibility(0, traceEnd, pos))
                    goto next_point;

                traceEnd[0] = pos[0];
                traceEnd[1] = pos[1];
                traceEnd[2] = pos[2] - zBelow;
                TraceSetup_and_Dispatch(0, pos, traceEnd, &hit);

                if (hit.triangle)
                {
                    MaterialDef_t *mat = hit.triangle->material;
                    if (!(mat->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY)))
                    {
                        if (mat->surfaceFlags != 1)
                            goto next_point;
                        if (!((*(unsigned char *)&mat->contents) & 0x80))
                            goto next_point;
                    }
                }
            }

        remove_point:
            {
                int newCount = g_gridPointCount - 1;
                GridSamplePoint_t *last = &g_gridPoints[newCount];
                g_gridPointCount = newCount;
                /* write 6 bytes: x,y (4) then z (2). orig uses mov dword + mov word. */
                *(int *)current = *(int *)last;
                current->z = last->z;
            }

        next_point:
            esi--;
            rbx -= 6;
            rdi--;
        } while (rdi != 0);
    }

done_dedup:
    /* Retail 0x410760: prune the surviving points with the stricter 32-bit
     * predicates (stable compaction — later lookups binary-search this
     * array, so the sort order must hold). */
    {
        int src, dst = 0;
        GridSamplePoint_t *pts = (GridSamplePoint_t *)g_gridPoints;

        /* The legacy swap-with-last rejection above does not retain order.
         * Retail's 12-byte-record pass re-establishes it before doing point
         * lookups; otherwise the binary searches can reject valid cells. */
        if (g_gridPointCount > 1)
            qsort(g_gridPoints, g_gridPointCount,
                  sizeof(GridSamplePoint_t), (void *)GridSamplePoint_Compare);

        for (src = 0; src < g_gridPointCount; ++src)
        {
            if (LightGrid_RetailRejectsPoint(&pts[src]))
                continue;
            if (dst != src)
                pts[dst] = pts[src];
            ++dst;
        }
        g_gridPointCount = dst;
    }

epilogue:
    return;
}

/*
 * TraceOctantSkyVisibility — trace sky visibility in 8 octant directions.
 * Address: 0x410BD0 | Size: 890 bytes
 *
 * ecx=flags, rdx=pos (float[3]), r8=outResult (byte[6])
 *
 * For each of 8 octants (corners of a cube around pos):
 *   1. Compute octant box from pos using direction constants
 *   2. Ray trace to check if octant hits geometry
 *   3. If hits valid surface, interpolate hit point, trace downward
 *   4. Check if downward trace hits sky-visible surface
 *   5. Set bit in outResult[5] for each visible octant
 * Finally traces sun direction and sets outResult[4] bit 0 if sun blocked.
 */
void TraceOctantSkyVisibility(int flags, float *pos, unsigned char *outResult)
{
    int octant;
    float posOctScale;      /* dword_458AE8 */
    float negOctScale;      /* dword_457634 */
    float posZScale;        /* dword_458AE4 */
    float negZScale;        /* dword_458AE0 */
    float smallOffset;      /* dword_457704 = 0.125f */
    float largeZOffset;     /* dword_4577D8 = 262144.0f */
    float midScale;         /* dword_458ADC */
    float zConst;           /* dword_458AD8 */
    float traceStart[3];
    float traceEnd[3];
    float traceStart2[3];
    float traceEnd2[3];
    RayHitResult_t hit;
    RayHitResult_t hit2;

    outResult[5] = 0;

    posOctScale = -0.40824827551841735839843750f;
    negOctScale =  0.40824827551841735839843750f;
    posZScale   = -0.81649655103683471679687500f;
    negZScale   =  0.81649655103683471679687500f;
    smallOffset = 0.125f;     /* dword_457704 */
    largeZOffset = 262144.0f; /* dword_4577D8 */
    midScale = 78.1336669921875f; /* dword_458ADC = 0x429C4470 */
    zConst = 0.1f;            /* dword_458AD8 = 0x3DCCCCCD */

    for (octant = 0; octant < 8; octant++)
    {
        float dirX, dirY, dirZ;
        float hitFrac;

        /* select direction per octant bit */
        dirX = (octant & 1) ? posOctScale : negOctScale;
        dirY = (octant & 2) ? posOctScale : negOctScale;
        dirZ = (octant & 4) ? posZScale : negZScale;

        /* compute trace box: start = pos + dir*smallOffset, end = pos + dir*midScale */
        traceStart[0] = dirX * smallOffset + pos[0];
        traceStart[1] = dirY * smallOffset + pos[1];
        traceStart[2] = dirZ * smallOffset + pos[2];

        traceEnd[0] = dirX * midScale + pos[0];
        traceEnd[1] = dirY * midScale + pos[1];
        traceEnd[2] = dirZ * midScale + pos[2];

        /* first trace: find geometry in octant direction */
        TraceSetup_and_Dispatch(flags, traceEnd, traceStart, &hit);


        if (!hit.triangle)
            goto next_octant;

        /* check if hit surface is valid (not sky/clip) */
        if (hit.triangle->material->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY))
            goto next_octant;

        /* interpolate hit point using trace fraction */
        hitFrac = hit.fraction;

        {
            float hitPos[3];
            hitPos[0] = (traceStart[0] - traceEnd[0]) * hitFrac + traceEnd[0];
            hitPos[1] = (traceStart[1] - traceEnd[1]) * hitFrac + traceEnd[1];
            hitPos[2] = (traceStart[2] - traceEnd[2]) * hitFrac + traceEnd[2];
            hitPos[2] -= zConst;

            /* second trace: downward from hit point */
            traceStart2[0] = hitPos[0];
            traceStart2[1] = hitPos[1];
            traceStart2[2] = hitPos[2];
            traceEnd2[0] = hitPos[0];
            traceEnd2[1] = hitPos[1];
            traceEnd2[2] = hitPos[2] - largeZOffset;

            /* This executes inside the parallel grid worker.  Slot zero is
             * shared with worker zero and races the triangle cache stamps;
             * keep both traces on the caller's per-worker cache slot. */
            TraceSetup_and_Dispatch(flags, traceStart2, traceEnd2, &hit2);

            if (!hit2.triangle)
                goto next_octant;

            {
                /* LST 0x410DEA-0x410DFA: ecx = contents (dword), then `cmp [rax+44h], 1;
                 * jnz loc_410DFC` (mark-visible path); `test cl, cl; js loc_410E08` (skip).
                 * `js` tests bit 7 of cl — the low byte — which is CONTENTS_MISSILECLIP
                 * (0x80), NOT CONTENTS_NODROP (0x80000000). */
                MaterialDef_t *mat2 = hit2.triangle->material;
                if (mat2->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY))
                    goto next_octant;
                if (mat2->surfaceFlags == SURF_NODAMAGE &&
                    ((*(unsigned char *)&mat2->contents) & 0x80))
                    goto next_octant;
            }
        }

        /* mark octant as visible */
        outResult[5] |= (1 << octant);

    next_octant:
        ;
    }

    /* trace sun direction for sky visibility */
    {
        float sunStart[3], sunEnd[3];
        RayHitResult_t sunHit;

        sunStart[0] = g_sunDirX * largeZOffset + pos[0];
        sunStart[1] = g_sunDirY * largeZOffset + pos[1];
        sunStart[2] = g_sunDirZ * largeZOffset + pos[2];

        sunEnd[0] = g_sunDirX * smallOffset + pos[0];
        sunEnd[1] = g_sunDirY * smallOffset + pos[1];
        sunEnd[2] = g_sunDirZ * smallOffset + pos[2];

        TraceSetup_and_Dispatch(flags, sunEnd, sunStart, &sunHit);

        if (!sunHit.triangle || (sunHit.triangle->material->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY)))
        {
            outResult[4] |= 1;
        }
    }
}

void ExpandStaticModelOrigins(void)
{
    StaticModelGridSample_t *node;
    int gridX, gridY, gridZ;
    int corner;

    node = g_gridSampleList;
    if (!node)
        return;

    do
    {
        /* unlink from list */
        g_gridSampleList = node->next;

        /* convert float position to grid coordinates */
        gridX = (int)floorf((*(float *)&node->data[0] - GRID_ORIGIN_OFFSET) * GRID_SCALE_XY);
        gridY = (int)floorf((*(float *)&node->data[1] - GRID_ORIGIN_OFFSET) * GRID_SCALE_XY);
        gridZ = (int)floorf((*(float *)&node->data[2] - GRID_ORIGIN_OFFSET) * GRID_SCALE_Z);

        /* generate 8 corner grid points */
        for (corner = 0; corner < 8; corner++)
        {
            int idx = g_gridPointCount;
            GridSamplePoint_t *pt = &g_gridPoints[idx];

            pt->x = (unsigned short)(gridX + (corner & 1));
            pt->y = (unsigned short)(gridY + ((corner >> 1) & 1));
            pt->z = (unsigned short)(gridZ + ((corner >> 2) & 1));

            g_gridPointCount = idx + 1;
        }

        /* free the node */
        free(node);
        node = g_gridSampleList;
    } while (node);
}

/*
 * GridSamplePoint_CompareForSort — qsort comparator for GridSampleEntry.
 * Address: 0x4119A0 | Size: 21 bytes
 *
 * Compare by key (int at +0), then by secondary (byte at +4).
 */
int GridSamplePoint_CompareForSort(const void *a, const void *b)
{
    const GridSampleEntry_t *ea = (const GridSampleEntry_t *)a;
    const GridSampleEntry_t *eb = (const GridSampleEntry_t *)b;
    int diff;

    diff = ea->key - eb->key;
    if (diff != 0)
        return diff;

    return (int)ea->secondary - (int)eb->secondary;
}

/*
 * GridSamplePoint_Compare — qsort comparator for GridSamplePoint.
 * Address: 0x4106C0 | Size: 43 bytes
 *
 * Compare by x, then y, then z (3 unsigned shorts).
 */
int GridSamplePoint_Compare(const void *a, const void *b)
{
    const GridSamplePoint_t *pa = (const GridSamplePoint_t *)a;
    const GridSamplePoint_t *pb = (const GridSamplePoint_t *)b;
    int diff;

    diff = (int)pa->x - (int)pb->x;
    if (diff != 0)
        return diff;

    diff = (int)pa->y - (int)pb->y;
    if (diff != 0)
        return diff;

    return (int)pa->z - (int)pb->z;
}

/*
 * AddStaticModelLightGridSample — allocate and link a grid sample node.
 * Address: 0x410070 | Size: 105 bytes
 *
 * rcx=data (3 ints to copy)
 * Allocates 24 bytes, copies 12 bytes of data, links into g_gridSampleList.
 */
void AddStaticModelLightGridSample(int *data)
{
    StaticModelGridSample_t *node;

    node = (StaticModelGridSample_t *)malloc(sizeof(StaticModelGridSample_t));
    if (!node)
        ErrorMsg("Out of memory on %i bytes for a static model light grid sample\n",
                    (int)sizeof(StaticModelGridSample_t));

    node->data[0] = data[0];
    node->data[1] = data[1];
    node->data[2] = data[2];

    /* link into list */
    node->next = g_gridSampleList;
    g_gridSampleCount++;
    g_gridSampleList = node;
}
