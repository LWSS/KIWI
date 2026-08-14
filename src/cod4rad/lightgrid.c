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
} GridSampleResult;

/* external functions not in master header */
extern void qsort(void *base, unsigned long long count, unsigned long long size, void *cmp);
/* BuildFilePath — copies base path to out buffer. Inlined from binary. */
static void BuildFilePath(const char *basePath, char *outPath)
{
    while ((*outPath++ = *basePath++) != '\0') { }
}
extern void *fopen_wrap(const char *path, const char *mode);
extern void fseek_wrap(void *file, int offset, int whence);
extern int ftell_wrap(void *file);
extern long long fread_wrap(void *dst, int elemSize, long long count, void *file);
extern void fclose_wrap(void *file);
extern int rand_int(void);
extern int FindLightingSamplesAndNormal(int sampleIdx, float *position, float *normal,
    float offset, void *outputLighting, float *outputNormal);
extern void Lighting_GetGatheredLight(void *sample, float *outVars); /* lighting_412550 */
extern int PointLightEvaluatePoint(int flags, int lightIndex, float *pos, float *normal,
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

#define GRID_ORIGIN_OFFSET  (-131072.0f)  /* dword_458A90 = 0xC8000000 */
#define GRID_SCALE_XY       0.03125f      /* dword_458784 = 0x3D000000 = 1/32 */
#define GRID_SCALE_Z        0.015625f     /* dword_458A8C = 0x3C800000 = 1/64 */

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

    gridPoint = (void *)&g_gridPoints[gridIndex];
    sample = (GridSampleResult *)AllocGridSample(flags, gridPoint);
    if (!sample)
        return;

    memset(lightBuffer, 0, 0x60);
    CalculateLightGrid_GatherLight(flags, gridIndex, lightBuffer);

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
}

/*
 * AddStaticModelLightGridSamples — load grid points from file, allocate buffer.
 * Address: 0x4100E0 | Size: 695 bytes
 *
 * Reads grid sample points from a binary file (6 bytes per point = GridSamplePoint).
 * Allocates buffer for file points + static model sample expansion.
 * If file not found or invalid, just expands static model samples.
 */
void AddStaticModelLightGridSamples(void)
{
    char filePath[0x400];
    void *file;
    int fileSize;
    int filePointCount;
    int totalPointCount;
    long long readCount;

    /* assert: pointCount == 0 (line 0x62) */
    Assert("(lightGridGlob.pointCount == 0)", ".\\lightgrid.cpp", 0x62, 0, 1);

    /* assert: points == NULL (line 0x63) */
    Assert("lightGridGlob.points == NULL", ".\\lightgrid.cpp", 0x63, 0, 1);

    /* build file path from base path */
    BuildFilePath(g_gridLogBasePath, filePath);

    /* append ".grid" extension (replace last chars) */
    {
        char *end = filePath;
        while (*end) end++;
        /* Retail emits the six-byte suffix with a dword/word pair. */
        memcpy(end, ".grid", 6);
    }

    /* open file */
    file = fopen_wrap(filePath, "rb");
    if (!file)
    {
        Com_Printf("Light grid sample point file '%s' not found, trying legacy .vclog.\n", filePath);
        goto done;
    }

    /* get file size */
    fseek_wrap(file, 0, 2); /* SEEK_END */
    fileSize = ftell_wrap(file);
    fseek_wrap(file, 0, 0); /* SEEK_SET */

    /* validate size is multiple of 6 */
    if (fileSize == 0 || (fileSize % 6) != 0)
    {
        Com_Printf("Ignoring grid logfile '%s': size %i is not a multiple of %i\n",
                     filePath, fileSize, 6);
        goto close_file;
    }

    filePointCount = fileSize / 6;
    g_gridPointCount = filePointCount;

    Com_Printf("Using %i grid points from grid logfile '%s'\n", filePointCount, filePath);

    /* allocate: (filePointCount + staticModelSampleCount * 8) * 6 bytes */
    totalPointCount = filePointCount + g_gridSampleCount * 8;
    g_gridPoints = malloc((unsigned long long)totalPointCount * 3 * 2);
    if (!g_gridPoints)
    {
        fclose_wrap(file);
        ErrorMsg("couldn't allocate %.2f MB for the light grid points\n",
                    (double)((float)fileSize * (1.0f / (1024.0f * 1024.0f))));
    }

    /* read grid points from file */
    readCount = fread_wrap(g_gridPoints, 6, g_gridPointCount, file);
    if (readCount != g_gridPointCount)
    {
        fclose_wrap(file);
        ErrorMsg("Error while reading %s\n", filePath);
        goto close_done;
    }

close_file:
    fclose_wrap(file);

close_done:
done:
    return;
}

/*
 * AllocGridSample — allocate and initialize a grid sample entry.
 * Address: 0x410F50 | Size: 266 bytes
 *
 * ecx=flags, rdx=gridPoint (GridSamplePoint*)
 * Returns pointer to 8-byte entry in global sample array.
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
void CalculateLightGrid_GatherLight(int flags, int gridIndex, float *outBuffer)
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

    /* zero output buffer */
    memset(outBuffer, 0, 0x60);

    /* gather from trace directions */
    for (i = 0; i < g_numTraceDirections; i++)
    {
        float *dir = g_traceDirections + i * 3;
        int hitCount;

        hitCount = FindLightingSamplesAndNormal(flags, pos, dir, 262144.0f,
            hitData, NULL);

        if (hitCount == 0)
            continue;

        if (hitCount == -1)
        {
            /* sky hit — use global sky light scale */
            GatherIncidentEnergyInSpaceForLightFromDir(
                &g_gridLightScale0, dir, outBuffer);
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
        }
    }

    /* gather from point lights */
    {
        int numLights = GetPointLightCount();
        for (i = 0; i < numLights; i++)
        {
            int result = PointLightEvaluatePoint(flags, i, pos, NULL,
                lightDir, lightColor, NULL);

            if (result == 1)
            {
                /* far light — use directional SH accumulation */
                GatherIncidentEnergyInSpaceForLightFromDir(lightColor, lightDir, outBuffer);
            }
            else if (result == 2)
            {
                /* near light — add color directly to all 24 SH coefficients */
                for (j = 0; j < 24; j++)
                {
                    outBuffer[j] += lightColor[j % 3];
                }
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
void CalculateLightGrid(int flags)
{
    int numDirs;
    float dirScale;

    /* clear counters */
    g_gridColorCount = 0;
    g_gridSampleArrayCount = 0;

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
                goto done;

            g_gridPoints = malloc((unsigned long long)g_gridSampleCount * 8 * 3 * 2);
        }
    }

    /* expand static model origins into grid points */
    ExpandStaticModelOrigins();

    /* sort, validate, deduplicate */
    CalculateLightGrid_SortPoints();

    if (g_gridPointCount == 0)
        goto done;

    /* allocate trace directions */
    AllocGridTraceDirections();

    /* compute lighting scale factors */
    numDirs = g_numTraceDirections;
    dirScale = g_lightScale / (float)numDirs;

    g_gridLightScale0 = g_backfaceLightR * dirScale;
    g_gridLightScale1 = g_backfaceLightG * dirScale;
    g_gridLightScale2 = g_backfaceLightB * dirScale;
    g_gridLightScale3 = g_radiosityScale * g_lightScale / (float)numDirs;

    /* calculate lighting for each grid point */
    ForEachLightmapPixel(g_gridPointCount, (void *)CalculateLightGrid_Worker, flags);

    /* sort results */
    qsort(g_gridSampleArray, g_gridSampleArrayCount, 8,
          (void *)GridSamplePoint_CompareForSort);

done:
    return;
}

/*
 * CalculateLightGrid_Setup — load vis cache grid points from file.
 * Address: 0x4103A0 | Size: 800 bytes
 *
 * Similar to AddStaticModelLightGridSamples but reads vis cache file (.viscache).
 * Reads 24-byte records, extracts 3 shorts per record into GridSamplePoint.
 * Allocates combined buffer for vis cache points + static model samples.
 */
void CalculateLightGrid_Setup(void)
{
    char filePath[0x400];
    char record[24]; /* 24-byte vis cache record */
    void *file;
    int fileSize;
    int visPointCount;
    int totalPointCount;
    int i;

    /* assert: pointCount == 0 (line 0x90) */
    Assert("(lightGridGlob.pointCount == 0)", ".\\lightgrid.cpp", 0x90, 0, 1);

    /* assert: points == NULL (line 0x91) */
    Assert("lightGridGlob.points == NULL", ".\\lightgrid.cpp", 0x91, 0, 1);

    /* build file path + ".viscache" extension */
    BuildFilePath(g_gridLogBasePath, filePath);
    {
        char *end = filePath;
        while (*end) end++;
        /* write ".vclog\0" from constants: dword ".vcl" + word "og" + byte 0 */
        *(int *)(end) = 0x6C63762E;   /* ".vcl" (dword_458A84) */
        *(short *)(end + 4) = 0x676F; /* "og" (word_458A88) */
        *(char *)(end + 6) = 0;       /* NUL (byte_458A8A) */
    }

    /* open file */
    file = fopen_wrap(filePath, "rb");
    if (!file)
    {
        Com_Printf("Vis cache logfile '%s' not found; using static model origins only.\n", filePath);
        goto done;
    }

    /* get file size */
    fseek_wrap(file, 0, 2);
    fileSize = ftell_wrap(file);
    fseek_wrap(file, 0, 0);

    /* validate size is multiple of 24 */
    if (fileSize == 0 || (fileSize % 24) != 0)
    {
        Com_Printf("Ignoring vis cache logfile '%s': size %i is not a multiple of %i\n",
                     filePath, fileSize, 24);
        fclose_wrap(file);
        goto epilogue;
    }

    visPointCount = fileSize / 24;

    /* allocate combined buffer: (visPointCount + sampleCount*8) * 6 bytes */
    totalPointCount = visPointCount + g_gridSampleCount * 8;
    g_gridPoints = malloc((unsigned long long)totalPointCount * 3 * 2);
    if (!g_gridPoints)
    {
        fclose_wrap(file);
        Com_Printf("couldn't allocate %.2f MB for the light grid points\n",
                    (double)((float)fileSize * (1.0f / (1024.0f * 1024.0f))));
    }

    Com_Printf("Using %i grid points from vis cache logfile '%s'\n", visPointCount, filePath);

    /* read records and extract grid points */
    for (i = 0; i < visPointCount; i++)
    {
        long long readCount;
        GridSamplePoint_t *pt;

        readCount = fread_wrap(record, 24, 1, file);
        if (readCount != 1)
        {
            fclose_wrap(file);
            Com_Printf("Error while reading %s\n", filePath);
            break;
        }

        pt = &g_gridPoints[i];
        pt->x = *(unsigned short *)&record[0];
        pt->y = *(unsigned short *)&record[4];
        pt->z = *(unsigned short *)&record[8];
    }

    fclose_wrap(file);
    g_gridPointCount = visPointCount;
    goto epilogue;

epilogue:
done:
    return;
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

            TraceSetup_and_Dispatch(0, traceStart2, traceEnd2, &hit2);

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
