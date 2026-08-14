/*
 * aabbtree.c — AABB tree construction for collision/spatial partitioning.
 */

#include "cod2rad64.h"

static int g_aabbNodeCount;
static float *g_aabbSortMins;
static float *g_aabbSortMaxs;
static float *g_aabbSortEqual;

#define AABB_STACK_ITEMS 0x8000

/*
================
CompareFunction

qsort comparator for floats.
================
*/
int CompareFunction(const void *a, const void *b)
{
    float diff;

    diff = *(const float *)a - *(const float *)b;
    if (diff < 0.0f)
        return -1;
    if (diff > 0.0f)
        return 1;
    return 0;
}

/*
================
AabbFindBestSplitPlane

Sweep all 3 axes to find optimal split.
================
*/
static char s_assertDisable_AabbFind_sum;
static char s_assertDisable_AabbFind_front;
static char s_assertDisable_AabbFind_back;
static char s_assertDisable_AabbFind_split;
static char s_assertDisable_AabbFind_on;

int AabbFindBestSplitPlane(float *itemMins, float *itemMaxs, int *indices,
                           int itemCount, int *outBestAxis, float *outSplitPos)
{
    int i, axis;
    int idx;
    int longestAxis;
    int sortCount, equalCount;
    int axisScale[3];
    float overallMins[3], overallMaxs[3];
    float minVal, maxVal;
    int bestScore;
    int minsIdx, maxsIdx, equalIdx;
    int minsStep, minsCount, equalStep, prevEqualStep;
    int splitTemp;
    float currentSplitPos, nextSplitPos;
    int sideFrontCount, sideBackCount, sideOnCount, sideSplitCount;
    int score;

    /* ClearBounds + gather overall bounds from all indexed items */
    ClearBounds(overallMins, overallMaxs);
    for (i = 0; i < itemCount; i++)
    {
        idx = indices[i];
        ExpandBounds(&itemMins[idx * 3], &itemMaxs[idx * 3], overallMins, overallMaxs);
    }

    /* find longest axis */
    longestAxis = 0;
    if ((overallMaxs[0] - overallMins[0]) > (overallMaxs[1] - overallMins[1]))
        longestAxis = 1;
    if ((overallMaxs[longestAxis] - overallMins[longestAxis]) > (overallMaxs[2] - overallMins[2]))
        longestAxis = 2;

    /* compute axis scale: (range+1)*10 / (longestRange+1), rounded */
    for (i = 0; i < 3; i++)
    {
        float candidateRange;
        candidateRange = (overallMaxs[i] - overallMins[i] + 1.0f) * 10.0f
                       / (overallMaxs[longestAxis] - overallMins[longestAxis] + 1.0f);
        axisScale[i] = (int)ceilf(candidateRange);
    }

    bestScore = INT_MIN; /* INT_MIN */

    /* sweep each axis */
    for (axis = 0; axis < 3; axis++)
    {
        /* separate items into range (mins/maxs) and degenerate (equal) */
        sortCount = 0;
        equalCount = 0;
        for (i = 0; i < itemCount; i++)
        {
            idx = indices[i];
            minVal = itemMins[3 * idx + axis];
            maxVal = itemMaxs[3 * idx + axis];
            if (minVal == maxVal)
            {
                g_aabbSortEqual[equalCount] = minVal;
                equalCount++;
            }
            else
            {
                g_aabbSortMins[sortCount] = minVal;
                g_aabbSortMaxs[sortCount] = maxVal;
                sortCount++;
            }
        }

        /* sort all three arrays */
        qsort(g_aabbSortMins, sortCount, sizeof(float), CompareFunction);
        qsort(g_aabbSortMaxs, sortCount, sizeof(float), CompareFunction);
        qsort(g_aabbSortEqual, equalCount, sizeof(float), CompareFunction);

        /* initialize sweep state */
        sideFrontCount = 0;
        sideSplitCount = 0;
        sideOnCount = 0;
        sideBackCount = itemCount;
        minsIdx = 0;
        maxsIdx = 0;
        equalIdx = 0;
        minsStep = 0;
        prevEqualStep = 0;

        /* initial nextSplitPos = min of first equal and first mins */
        if (g_aabbSortEqual[0] - g_aabbSortMins[0] < 0.0f)
            nextSplitPos = g_aabbSortEqual[0];
        else
            nextSplitPos = g_aabbSortMins[0];

        while (FLT_MAX > nextSplitPos)
        {
            currentSplitPos = nextSplitPos;

            /* advance items entering split zone */
            sideSplitCount += minsStep;
            sideBackCount -= minsStep;
            minsStep = 0;
            nextSplitPos = FLT_MAX;

            /* count mins at currentSplitPos */
            if (minsIdx < sortCount)
            {
                minsCount = 0;
                while (minsIdx + minsCount < sortCount && g_aabbSortMins[minsIdx + minsCount] == currentSplitPos)
                    minsCount++;
                minsIdx += minsCount;
                minsStep = minsCount;
                if (minsIdx < sortCount && g_aabbSortMins[minsIdx] < FLT_MAX)
                    nextSplitPos = g_aabbSortMins[minsIdx];
            }

            /* count maxs at currentSplitPos — items leaving split zone */
            if (maxsIdx < sortCount)
            {
                splitTemp = sideSplitCount;
                while (maxsIdx < sortCount && g_aabbSortMaxs[maxsIdx] == currentSplitPos)
                {
                    splitTemp--;
                    maxsIdx++;
                    sideFrontCount++;
                }
                sideSplitCount = splitTemp;
                if (maxsIdx < sortCount && g_aabbSortMaxs[maxsIdx] < nextSplitPos)
                    nextSplitPos = g_aabbSortMaxs[maxsIdx];
            }

            /* handle degenerate (equal) items */
            sideOnCount -= prevEqualStep;
            sideFrontCount += prevEqualStep;
            equalStep = 0;
            prevEqualStep = 0;
            if (equalIdx < equalCount)
            {
                while (equalIdx + equalStep < equalCount && g_aabbSortEqual[equalIdx + equalStep] == currentSplitPos)
                    equalStep++;
                equalIdx += equalStep;
                prevEqualStep = equalStep;
            }
            sideOnCount += equalStep;
            sideBackCount -= equalStep;
            if (equalIdx < equalCount && g_aabbSortEqual[equalIdx] < nextSplitPos)
                nextSplitPos = g_aabbSortEqual[equalIdx];

            /* asserts: all sides must sum to itemCount, each >= 0 */
            Assert(sideFrontCount + sideBackCount + sideSplitCount + sideOnCount == itemCount, s_assertDisable_AabbFind_sum);
            Assert(sideFrontCount >= 0, s_assertDisable_AabbFind_front);
            Assert(sideBackCount >= 0, s_assertDisable_AabbFind_back);
            Assert(sideSplitCount >= 0, s_assertDisable_AabbFind_split);
            Assert(sideOnCount >= 0, s_assertDisable_AabbFind_on);

            /* score this split position */
            if (sideFrontCount > 1 && sideBackCount > 1)
            {
                score = itemCount + axisScale[axis]
                      - 4 * sideSplitCount
                      - sideOnCount
                      - abs(sideFrontCount - sideBackCount);

                /* bonus for clean gap between current and next */
                if (!sideOnCount && !sideSplitCount && !minsStep)
                {
                    float splitDist = nextSplitPos - currentSplitPos;
                    score += (int)floorf(splitDist + 0.5f);
                }

                if (score > bestScore)
                {
                    bestScore = score;
                    *outBestAxis = axis;
                    if (sideOnCount || sideSplitCount || minsStep)
                        *outSplitPos = currentSplitPos;
                    else
                        *outSplitPos = (currentSplitPos + nextSplitPos) * 0.5f;
                }
            }
        }
    }

    return bestScore != INT_MIN;
}

/*
 * BoundsVolume — compute volume of an AABB.
 * Used inline by AabbPartition for volume-based fallback.
 */
static float BoundsVolume(const float *mins, const float *maxs)
{
    return (maxs[0] - mins[0]) * (maxs[1] - mins[1]) * (maxs[2] - mins[2]);
}

/*
================
AabbPartition

Three-way partition with volume-based fallback.
================
*/
int AabbPartition(int itemCount, AabbTreeBuilder_t *builder, int *indices,
                  int *outFrontCount, int *outMidStart)
{
    int frontIdx, backIdx;
    int frontItem, backItem;
    int midScanIdx;
    float frontMins[3], frontMaxs[3];
    float backMins[3], backMaxs[3];
    float tryFrontMins[3], tryFrontMaxs[3];
    float tryBackMins[3], tryBackMaxs[3];
    float frontVol, backVol, newFrontVol, newBackVol;
    int splitAxis;
    float splitPos;
    float *minsBuf, *maxsBuf;
    int temp;

    minsBuf = builder->itemMins;
    maxsBuf = builder->itemMaxs;

    if (!AabbFindBestSplitPlane(minsBuf, maxsBuf, indices, itemCount, &splitAxis, &splitPos))
        return 0;

    ClearBounds(frontMins, frontMaxs);
    ClearBounds(backMins, backMaxs);

    frontIdx = 0;
    backIdx = itemCount - 1;

    if (backIdx < 0)
        goto finish_check;

scan_pass:
    if (frontIdx > backIdx)
        goto scan_done;

    /* scan front: items fully below splitPos */
    while (frontIdx <= backIdx)
    {
        frontItem = indices[frontIdx];
        if (maxsBuf[3 * frontItem + splitAxis] > splitPos ||
            minsBuf[3 * frontItem + splitAxis] >= splitPos)
            break;
        ExpandBounds(&minsBuf[3 * frontItem], &maxsBuf[3 * frontItem], frontMins, frontMaxs);
        frontIdx++;
    }
    if (frontIdx > backIdx)
        goto scan_done;

    /* scan back: items fully above splitPos */
    while (frontIdx <= backIdx)
    {
        backItem = indices[backIdx];
        if (minsBuf[3 * backItem + splitAxis] < splitPos ||
            maxsBuf[3 * backItem + splitAxis] <= splitPos)
            break;
        ExpandBounds(&minsBuf[3 * backItem], &maxsBuf[3 * backItem], backMins, backMaxs);
        backIdx--;
    }
    if (frontIdx > backIdx)
        goto scan_done;

    /* check if front item belongs to back or vice versa — swap if needed */
    frontItem = indices[frontIdx];
    if (minsBuf[3 * frontItem + splitAxis] < splitPos ||
        maxsBuf[3 * frontItem + splitAxis] <= splitPos)
    {
        backItem = indices[backIdx];
        if (maxsBuf[3 * backItem + splitAxis] > splitPos ||
            minsBuf[3 * backItem + splitAxis] >= splitPos)
            goto mid_scan;
    }
    /* swap front and back */
    indices[frontIdx] = indices[backIdx];
    indices[backIdx] = frontItem;
    goto scan_pass;

mid_scan:
    midScanIdx = frontIdx;
    if (frontIdx >= backIdx)
        goto scan_done;

    for (;;)
    {
        int midItem = indices[midScanIdx];

        /* mid item belongs to back side */
        if (minsBuf[3 * midItem + splitAxis] >= splitPos &&
            maxsBuf[3 * midItem + splitAxis] > splitPos)
        {
            temp = indices[midScanIdx];
            indices[midScanIdx] = indices[backIdx];
            indices[backIdx] = temp;
            goto scan_pass;
        }

        /* mid item belongs to front side */
        if (maxsBuf[3 * midItem + splitAxis] <= splitPos &&
            minsBuf[3 * midItem + splitAxis] < splitPos)
        {
            temp = indices[midScanIdx];
            indices[midScanIdx] = indices[frontIdx];
            indices[frontIdx] = temp;
            goto scan_pass;
        }

        midScanIdx++;
        if (midScanIdx >= backIdx)
            goto scan_done;
    }

scan_done:
    if (frontIdx > backIdx)
        goto finish_check;

    /* check if partition meets minimum size requirements */
    {
        int minPartSize = builder->minPartitionSize;
        if (frontIdx >= minPartSize &&
            (backIdx - frontIdx + 1) >= minPartSize &&
            (itemCount - backIdx - 1) >= minPartSize)
            goto finish_check;
    }

    /* volume-based fallback for straddling items */
vol_pass:
    /* try adding frontIdx item to front */
    tryFrontMins[0] = frontMins[0]; tryFrontMins[1] = frontMins[1]; tryFrontMins[2] = frontMins[2];
    tryFrontMaxs[0] = frontMaxs[0]; tryFrontMaxs[1] = frontMaxs[1]; tryFrontMaxs[2] = frontMaxs[2];
    frontItem = indices[frontIdx];
    ExpandBounds(&minsBuf[3 * frontItem], &maxsBuf[3 * frontItem], tryFrontMins, tryFrontMaxs);
    newFrontVol = BoundsVolume(tryFrontMins, tryFrontMaxs);
    frontVol = BoundsVolume(frontMins, frontMaxs);

    /* try adding frontIdx item to back */
    tryBackMins[0] = backMins[0]; tryBackMins[1] = backMins[1]; tryBackMins[2] = backMins[2];
    tryBackMaxs[0] = backMaxs[0]; tryBackMaxs[1] = backMaxs[1]; tryBackMaxs[2] = backMaxs[2];
    ExpandBounds(&minsBuf[3 * frontItem], &maxsBuf[3 * frontItem], tryBackMins, tryBackMaxs);
    backVol = BoundsVolume(backMins, backMaxs);
    newBackVol = BoundsVolume(tryBackMins, tryBackMaxs);

    if (newBackVol - backVol < newFrontVol - frontVol)
    {
        /* back grows less — try pulling from back end instead */
        if (frontIdx > backIdx)
            goto finish_check;

        do
        {
            tryBackMins[0] = backMins[0]; tryBackMins[1] = backMins[1]; tryBackMins[2] = backMins[2];
            tryBackMaxs[0] = backMaxs[0]; tryBackMaxs[1] = backMaxs[1]; tryBackMaxs[2] = backMaxs[2];
            backItem = indices[backIdx];
            ExpandBounds(&minsBuf[3 * backItem], &maxsBuf[3 * backItem], tryBackMins, tryBackMaxs);
            newBackVol = BoundsVolume(tryBackMins, tryBackMaxs);
            backVol = BoundsVolume(backMins, backMaxs);

            tryFrontMins[0] = frontMins[0]; tryFrontMins[1] = frontMins[1]; tryFrontMins[2] = frontMins[2];
            tryFrontMaxs[0] = frontMaxs[0]; tryFrontMaxs[1] = frontMaxs[1]; tryFrontMaxs[2] = frontMaxs[2];
            ExpandBounds(&minsBuf[3 * backItem], &maxsBuf[3 * backItem], tryFrontMins, tryFrontMaxs);
            newFrontVol = BoundsVolume(tryFrontMins, tryFrontMaxs);
            frontVol = BoundsVolume(frontMins, frontMaxs);

            if (newFrontVol - frontVol < newBackVol - backVol)
                break;

            ExpandBounds(&minsBuf[3 * indices[backIdx]], &maxsBuf[3 * indices[backIdx]], backMins, backMaxs);
            backIdx--;
        } while (frontIdx <= backIdx);

        if (frontIdx > backIdx)
            goto finish_check;

        if (frontIdx == backIdx)
        {
            if (2 * frontIdx < itemCount)
                frontIdx++;
            else
                backIdx--;
            goto finish_check;
        }

        /* swap and continue */
        temp = indices[frontIdx];
        indices[frontIdx] = indices[backIdx];
        indices[backIdx] = temp;
        frontIdx++;
        backIdx--;
        if (frontIdx <= backIdx)
            goto vol_pass;
        goto finish_check;
    }

    /* front grows less — add item to front */
    ExpandBounds(&minsBuf[3 * indices[frontIdx]], &maxsBuf[3 * indices[frontIdx]], frontMins, frontMaxs);
    frontIdx++;
    if (frontIdx <= backIdx)
        goto vol_pass;

finish_check:
    if (!frontIdx || frontIdx == itemCount)
        return 0;

    *outFrontCount = frontIdx;
    *outMidStart = backIdx + 1;
    return 1;
}

/*
================
AabbCreateNode

Allocate subtree nodes for a partition result.
If partition succeeds: creates 2 or 3 child nodes (front, optional mid, back).
If partition fails: creates 1 leaf node.
 * Return value not used by callers.
 */
void AabbCreateNode(AabbTreeNode_t *parent, AabbTreeBuilder_t *builder,
                    int *indices, int offset, int count)
{
    int frontCount, midStart;
    AabbTreeNode_t *node;
    int nodeIdx;

    if (count > builder->minLeafItems &&
        AabbPartition(count, builder, indices + offset, &frontCount, &midStart))
    {
        /* front node */
        nodeIdx = g_aabbNodeCount;
        if (g_aabbNodeCount == builder->maxNodes)
        {
            Error("More than %i AABB nodes needed\n", builder->maxNodes);
            nodeIdx = g_aabbNodeCount;
        }
        g_aabbNodeCount = nodeIdx + 1;
        node = &builder->nodes[nodeIdx];
        node->firstItem = offset + parent->firstItem;
        node->itemCount = frontCount;

        /* optional mid node (if frontCount < midStart) */
        if (frontCount < midStart)
        {
            nodeIdx = g_aabbNodeCount;
            if (g_aabbNodeCount == builder->maxNodes)
            {
                Error("More than %i AABB nodes needed\n", builder->maxNodes);
                nodeIdx = g_aabbNodeCount;
            }
            g_aabbNodeCount = nodeIdx + 1;
            node = &builder->nodes[nodeIdx];
            node->firstItem = frontCount + parent->firstItem + offset;
            node->itemCount = midStart - frontCount;
        }

        /* back node */
        nodeIdx = g_aabbNodeCount;
        if (g_aabbNodeCount == builder->maxNodes)
        {
            Error("More than %i AABB nodes needed\n", builder->maxNodes);
            nodeIdx = g_aabbNodeCount;
        }
        g_aabbNodeCount = nodeIdx + 1;
        node = &builder->nodes[nodeIdx];
        node->firstItem = midStart + parent->firstItem + offset;
        node->itemCount = count - midStart;
    }
    else
    {
        /* leaf — single node for all items */
        nodeIdx = g_aabbNodeCount;
        if (g_aabbNodeCount == builder->maxNodes)
        {
            Error("More than %i AABB nodes needed\n", builder->maxNodes);
            nodeIdx = g_aabbNodeCount;
        }
        g_aabbNodeCount = nodeIdx + 1;
        node = &builder->nodes[nodeIdx];
        node->firstItem = offset + parent->firstItem;
        node->itemCount = count;
    }
}

/*
================
AabbBuildTree_r

Recursive tree construction.
================
*/
int AabbBuildTree_r(AabbTreeNode_t *node, AabbTreeBuilder_t *builder, int *indices)
{
    int frontCount, midStart;
    int i;
    AabbTreeNode_t *children;

    {
        static char s_assertDisable_BuildTree_count;
        Assert(node->itemCount, s_assertDisable_BuildTree_count);
    }

    node->firstChild = g_aabbNodeCount;
    node->childCount = 0;

    if (node->itemCount <= builder->minLeafItems)
        return 0;

    if (!AabbPartition(node->itemCount, builder, indices, &frontCount, &midStart))
        return 0;

    {
        static char s_assertDisable_BuildTree_firstChild;
        Assert(node->firstChild == g_aabbNodeCount, s_assertDisable_BuildTree_firstChild);
    }

    children = &builder->nodes[g_aabbNodeCount];

    /* create front subtree */
    AabbCreateNode(node, builder, indices, 0, frontCount);

    /* create mid subtree if front < midStart */
    if (frontCount < midStart)
        AabbCreateNode(node, builder, indices, frontCount, midStart - frontCount);

    /* create back subtree */
    AabbCreateNode(node, builder, indices, midStart, node->itemCount - midStart);

    node->childCount = g_aabbNodeCount - node->firstChild;

    /* recurse into each child */
    for (i = 0; i < node->childCount; i++)
    {
        AabbBuildTree_r(&children[i], builder,
                        indices + children[i].firstItem - node->firstItem);
    }

    return node->childCount;
}

/*
================
BuildAabbTree

Entry point for AABB tree construction. Returns node count.
================
*/
int BuildAabbTree(AabbTreeBuilder_t *builder)
{
    int itemCount;
    int i;
    int *indexBuf;
    int heapAllocated;
    char *itemDataCopy;
    float *boundsCopy;
    float sortBufMinsLocal[AABB_STACK_ITEMS];
    float sortBufMaxsLocal[AABB_STACK_ITEMS];
    float sortBufEqualLocal[AABB_STACK_ITEMS];
    int indexBufLocal[AABB_STACK_ITEMS];

    itemCount = builder->itemCount;

    if (itemCount > AABB_STACK_ITEMS)
    {
        indexBuf = (int *)malloc(itemCount * sizeof(int));
        g_aabbSortMins = (float *)malloc(itemCount * sizeof(float));
        g_aabbSortMaxs = (float *)malloc(itemCount * sizeof(float));
        g_aabbSortEqual = (float *)malloc(itemCount * sizeof(float));
        heapAllocated = 1;
    }
    else
    {
        indexBuf = indexBufLocal;
        g_aabbSortMins = sortBufMinsLocal;
        g_aabbSortMaxs = sortBufMaxsLocal;
        g_aabbSortEqual = sortBufEqualLocal;
        heapAllocated = 0;
    }

    /* initialize index buffer: identity mapping */
    for (i = 0; i < itemCount; i++)
        indexBuf[i] = i;

    /* initialize root node */
    builder->nodes[0].firstItem = 0;
    builder->nodes[0].itemCount = itemCount;
    g_aabbNodeCount = 1;

    /* build tree recursively */
    AabbBuildTree_r(&builder->nodes[0], builder, indexBuf);

    /* reorder item data to match tree node ordering */
    itemDataCopy = (char *)malloc(itemCount * builder->itemStride);
    memcpy(itemDataCopy, builder->itemData, itemCount * builder->itemStride);
    for (i = 0; i < itemCount; i++)
    {
        memcpy((char *)builder->itemData + i * builder->itemStride,
               itemDataCopy + builder->itemStride * indexBuf[i],
               builder->itemStride);
    }
    free(itemDataCopy);

    /* reorder bounds data if present */
    if (builder->hasBoundsData)
    {
        boundsCopy = (float *)malloc(3 * sizeof(float) * itemCount);

        /* reorder mins */
        memcpy(boundsCopy, builder->itemMins, 3 * sizeof(float) * itemCount);
        for (i = 0; i < itemCount; i++)
        {
            builder->itemMins[3 * i + 0] = boundsCopy[3 * indexBuf[i] + 0];
            builder->itemMins[3 * i + 1] = boundsCopy[3 * indexBuf[i] + 1];
            builder->itemMins[3 * i + 2] = boundsCopy[3 * indexBuf[i] + 2];
        }

        /* reorder maxs */
        memcpy(boundsCopy, builder->itemMaxs, 3 * sizeof(float) * itemCount);
        for (i = 0; i < itemCount; i++)
        {
            builder->itemMaxs[3 * i + 0] = boundsCopy[3 * indexBuf[i] + 0];
            builder->itemMaxs[3 * i + 1] = boundsCopy[3 * indexBuf[i] + 1];
            builder->itemMaxs[3 * i + 2] = boundsCopy[3 * indexBuf[i] + 2];
        }

        free(boundsCopy);
    }

    /* free heap allocations if needed */
    if (heapAllocated)
    {
        free(indexBuf);
        free(g_aabbSortMins);
        free(g_aabbSortMaxs);
        free(g_aabbSortEqual);
    }

    return g_aabbNodeCount;
}
