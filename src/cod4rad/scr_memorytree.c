/*
 * scr_memorytree.c — Script memory tree (buddy allocator).
 *
 * Power-of-2 buddy allocator for script string interning.
 * Manages MEMORY_NODE_COUNT (65536) nodes.
 * Size classes: 0..16 (MEMORY_NODE_BITS), each representing 2^n nodes in a buddy pair.
 */

#include "cod2rad64.h"

/* forward declarations for internal helpers */
static int MT_GetSize(int numBytes);

static unsigned char g_mtNodeArray[MEMORY_NODE_COUNT * MT_NODE_SIZE]; /* word_633C500 */

/* globals — declared here so they are visible to all functions below */
extern void *g_scrStringMT;
static int g_mtAllocCount;
static int g_mtAllocBytes;
static unsigned char g_mtDebugUsage[MEMORY_NODE_COUNT];
static unsigned char g_mtDebugSize[MEMORY_NODE_COUNT];
static unsigned short g_mtFreeLists[MEMORY_NODE_BITS + 1]; /* word array for free list heads */

/*
 * Lookup tables for fast bit operations in the buddy allocator.
 * Built by MT_Init. Three 256-byte tables at g_mtLookup:
 *   +0x000: highest set bit position for each byte value
 *   +0x100: popcount/bit scan variant
 *   +0x200: trailing zero count variant
 */
static unsigned char g_mtLookup[0x300]; /* unk_63BC500 */

/* assert disable flags */
static char s_assertDisable_MT_AddToFreeList_size;
static char s_assertDisable_MT_AddToFreeList_notSelf;
static char s_assertDisable_MT_AddToFreeList_num;
static char s_assertDisable_MT_AddToFreeList_score;
static char s_assertDisable_MT_AddToFreeList_notMid;
static char s_assertDisable_MT_AddToFreeList_parent;
static char s_assertDisable_MT_AddToFreeList_notNew;
static char s_assertDisable_MT_AddToFreeList_notMid2;
static char s_assertDisable_MT_RemoveFromFreeList_size;
static char s_assertDisable_MT_RemoveFromFreeList_scores;
static char s_assertDisable_MT_IsBuddyFree_size;
static char s_assertDisable_MT_IsBuddyFree_scores;
static char s_assertDisable_MT_GetSize;
static char s_assertDisable_MT_AllocIndex_bytes;
static char s_assertDisable_MT_AllocIndex_range;
static char s_assertDisable_MT_AllocIndex_type;
static char s_assertDisable_MT_AllocIndex_usage;
static char s_assertDisable_MT_AllocIndex_size;
static char s_assertDisable_MT_FreeIndex_size;
static char s_assertDisable_MT_FreeIndex_node;
static char s_assertDisable_MT_FreeIndex_thread;
static char s_assertDisable_MT_FreeIndex_usage;
static char s_assertDisable_MT_FreeIndex_sizeMatch;
static char s_assertDisable_MT_FreeIndex_maxSize;
static char s_assertDisable_MT_FreeIndex_aligned;
static char s_assertDisable_MT_Free_bytes;
static char s_assertDisable_MT_Free_node;
static char s_assertDisable_MT_NodeScore_num;
static char s_assertDisable_MT_NodeScore_nodeNum;
static char s_assertDisable_MT_ReAllocFromBitmap_bytes;
static char s_assertDisable_MT_ReAllocFromBitmap_size;
static char s_assertDisable_MT_ReAllocFromBitmap_usage;
static char s_assertDisable_MT_ReAllocFromBitmap_sizeArr;

/*
================
Internal buddy allocator helpers.

These manage the free list linked lists embedded in the node array.
Each free node stores prev/next links for its size class's free list.

scr_memorytree_420270 = MT_AddToFreeList (230 lines) — adds node to free list
scr_memorytree_420650 = MT_IsBuddyFree (155 lines) — checks if buddy can merge
scr_memorytree_4208B0 = MT_RemoveFromFreeList (200 lines) — removes head from free list
scr_memorytree_420160 = MT_NodeScore (64 lines) — buddy tree node score
scr_memorytree_421540 = MT_GetNodeInfo (165 lines) — debug info helper
================
*/
/*
 * Free list tree node access macros.
 * Each free node stores left/right child at offsets 0 and 2.
 */
#define MT_NODE_LEFT(n)  (*(unsigned short *)((char *)g_mtNodeArray + (n) * MT_NODE_SIZE + 0))
#define MT_NODE_RIGHT(n) (*(unsigned short *)((char *)g_mtNodeArray + (n) * MT_NODE_SIZE + 2))

/*
================
MT_NodeScore

Computes a "score" for a node number using the precomputed lookup
tables. Used to order nodes in the BST (binary search tree) that
forms each size class's free list.

The score determines BST traversal direction:
  lower score → go left, higher score → go right.
================
*/
static int MT_NodeScore(unsigned int nodeNum)
{
    unsigned int num;
    unsigned char lo;
    unsigned char hi;
    int bits_lo;
    int bits_hi;
    int pos;

    Assert(nodeNum, s_assertDisable_MT_NodeScore_nodeNum);
    num = MEMORY_NODE_COUNT - nodeNum;
    Assert(num, s_assertDisable_MT_NodeScore_num);

    lo = (unsigned char)num;
    hi = (unsigned char)(num >> 8);
    bits_lo = g_mtLookup[0x100 + lo];
    bits_hi = g_mtLookup[0x100 + hi];

    if (lo == 0)
        pos = g_mtLookup[hi] + g_mtLookup[lo];
    else
        pos = g_mtLookup[lo];

    return (1 << pos) + (num - bits_lo - bits_hi);
}

/*
================
MT_AddToFreeList

Inserts a node into the BST free list for a given size class.
Traverses the tree comparing scores to find the insertion point,
then links the new node as a leaf.

================
*/
static void MT_AddToFreeList(unsigned int nodeNum, int sizeClass)
{
    unsigned short *parentSlot;
    unsigned int current;
    int newScore;
    int halfRange;
    int midpoint;

    Assert(sizeClass >= 0 && sizeClass <= MEMORY_NODE_BITS,
           s_assertDisable_MT_AddToFreeList_size);

    parentSlot = &g_mtFreeLists[sizeClass];
    current = *parentSlot;

    if (!current)
    {
        /* empty tree — insert as root */
        *parentSlot = (unsigned short)nodeNum;
        MT_NODE_LEFT(nodeNum) = 0;
        MT_NODE_RIGHT(nodeNum) = 0;
        return;
    }

    newScore = MT_NodeScore(nodeNum);
    halfRange = MEMORY_NODE_COUNT;
    midpoint = 0;

    Assert(nodeNum != current, s_assertDisable_MT_AddToFreeList_notSelf);
    Assert((MEMORY_NODE_COUNT - current) != 0, s_assertDisable_MT_AddToFreeList_num);

    /*
     * Score-based treap insertion:
     * - If current node has lower score than new node: new node takes
     *   current's position (treap property: higher score = higher priority)
     * - Navigate left/right based on midpoint comparison of node numbers
     */
    while (1)
    {
        int currentScore = MT_NodeScore(current);

        Assert(currentScore != newScore,
               s_assertDisable_MT_AddToFreeList_score);

        if (currentScore < newScore)
        {
            /* new node has higher priority — insert here, push current down */
            Assert(*parentSlot == current,
                   s_assertDisable_MT_AddToFreeList_parent);
            Assert(current != nodeNum,
                   s_assertDisable_MT_AddToFreeList_notNew);

            *parentSlot = (unsigned short)nodeNum;

            /* copy current's children to new node */
            {
                int savedNodeNum = nodeNum;
                *(unsigned __int64 *)((char *)g_mtNodeArray + nodeNum * MT_NODE_SIZE) =
                    *(unsigned __int64 *)((char *)g_mtNodeArray + current * MT_NODE_SIZE);

                if (!current)
                    break;

                halfRange >>= 1;

                Assert(current != (unsigned int)midpoint,
                       s_assertDisable_MT_AddToFreeList_notMid2);

                /* push displaced node down using midpoint — parentSlot refs saved nodeNum */
                if ((int)current < midpoint)
                {
                    nodeNum = current;
                    current = MT_NODE_LEFT(savedNodeNum);
                    midpoint -= halfRange;
                    parentSlot = &MT_NODE_LEFT(savedNodeNum);
                }
                else
                {
                    nodeNum = current;
                    current = MT_NODE_RIGHT(savedNodeNum);
                    midpoint += halfRange;
                    parentSlot = &MT_NODE_RIGHT(savedNodeNum);
                }
            }

            if (!current)
                break;

            newScore = MT_NodeScore(nodeNum);
        }
        else
        {
            /* current has higher priority — walk down to find position */
            halfRange >>= 1;

            Assert(nodeNum != (unsigned int)midpoint,
                   s_assertDisable_MT_AddToFreeList_notMid);

            if ((int)nodeNum < midpoint)
            {
                midpoint -= halfRange;
                parentSlot = &MT_NODE_LEFT(current);
            }
            else
            {
                midpoint += halfRange;
                parentSlot = &MT_NODE_RIGHT(current);
            }

            current = *parentSlot;
            if (!current)
                break;
        }
    }

    /* insert as leaf */
    *parentSlot = (unsigned short)nodeNum;
    MT_NODE_LEFT(nodeNum) = 0;
    MT_NODE_RIGHT(nodeNum) = 0;
}

/*
================
MT_RemoveFromFreeList

Removes a specific node from the BST free list.
Finds the node by traversing the tree, then replaces it
with its in-order successor or predecessor.

================
*/
static void MT_RemoveFromFreeList(int sizeClass)
{
    /*
     * Removes the root of the BST free list at sizeClass.
     * Uses score-based promotion: the child with higher score becomes
     * the new root. Then iteratively restructures down the tree by
     * swapping node data, until reaching a node with 0 or 1 children.
     *
     * This is essentially the same removal as in MT_IsBuddyFree
     * but always targeting the root node.
     *
     * LST: scr_memorytree_4208B0 (~200 lines, every line audited)
     */

    unsigned short *parentSlot;
    unsigned __int64 children;
    unsigned short left, right;

    Assert(sizeClass >= 0 && sizeClass <= MEMORY_NODE_BITS,
           s_assertDisable_MT_RemoveFromFreeList_size);

    parentSlot = &g_mtFreeLists[sizeClass];
    children = *(unsigned __int64 *)((char *)g_mtNodeArray + (*parentSlot) * MT_NODE_SIZE);

    left = (unsigned short)children;
    right = (unsigned short)(children >> 16);

    /* case: no left child */
    if (!left)
    {
        *parentSlot = right;
        if (!right)
            return;
        /* set up for restructure from right subtree */
        parentSlot = &MT_NODE_RIGHT(*parentSlot);
        goto restructure;
    }

    /* case: no right child — promote left and enter restructure */
    if (!right)
    {
        *parentSlot = left;
        parentSlot = &MT_NODE_LEFT(*parentSlot);
        goto restructure;
    }

    /* case: both children — compare scores */
    {
        int leftScore = MT_NodeScore(left);
        int rightScore = MT_NodeScore(right);

        Assert(leftScore != rightScore,
               s_assertDisable_MT_RemoveFromFreeList_scores);

        if (leftScore < rightScore)
        {
            *parentSlot = right;
            parentSlot = &MT_NODE_RIGHT(*parentSlot); /* binary: +2 when right wins */
        }
        else
        {
            *parentSlot = left;
            parentSlot = &MT_NODE_LEFT(*parentSlot); /* binary: +0 when left wins */
        }
    }

restructure:
    /* iteratively push the hole down until reaching a leaf */
    while (1)
    {
        left = (unsigned short)children;
        right = (unsigned short)(children >> 16);

        if (!left)
        {
            *parentSlot = right;
            if (!right)
                return;
            unsigned __int64 oldData = *(unsigned __int64 *)((char *)g_mtNodeArray + right * MT_NODE_SIZE);
            *(unsigned __int64 *)((char *)g_mtNodeArray + right * MT_NODE_SIZE) = children;
            children = oldData;
            parentSlot = &MT_NODE_RIGHT(right); /* binary: +2 when right promoted */
            continue;
        }

        if (!right)
        {
            *parentSlot = left;
            unsigned __int64 oldData = *(unsigned __int64 *)((char *)g_mtNodeArray + left * MT_NODE_SIZE);
            *(unsigned __int64 *)((char *)g_mtNodeArray + left * MT_NODE_SIZE) = children;
            children = oldData;
            parentSlot = &MT_NODE_LEFT(left); /* binary: +0 when left promoted */
            continue;
        }

        {
            int leftScore = MT_NodeScore(left);
            int rightScore = MT_NodeScore(right);

            if (leftScore < rightScore)
            {
                *parentSlot = right;
                unsigned __int64 oldData = *(unsigned __int64 *)((char *)g_mtNodeArray + right * MT_NODE_SIZE);
                *(unsigned __int64 *)((char *)g_mtNodeArray + right * MT_NODE_SIZE) = children;
                children = oldData;
                parentSlot = &MT_NODE_RIGHT(right); /* binary: +2 when right wins */
            }
            else
            {
                *parentSlot = left;
                unsigned __int64 oldData = *(unsigned __int64 *)((char *)g_mtNodeArray + left * MT_NODE_SIZE);
                *(unsigned __int64 *)((char *)g_mtNodeArray + left * MT_NODE_SIZE) = children;
                children = oldData;
                parentSlot = &MT_NODE_LEFT(left); /* binary: +0 when left wins */
            }
        }
    }
}

/*
================
MT_IsBuddyFree

Searches the BST free list for a specific buddy node.
If found, removes it from the tree and returns 1.
If not found, returns 0.

================
*/
static int MT_IsBuddyFree(unsigned int buddyNum, int sizeClass)
{
    unsigned short *parentSlot;
    unsigned int current;
    int midpoint;
    int halfRange;

    Assert(sizeClass >= 0 && sizeClass <= MEMORY_NODE_BITS,
           s_assertDisable_MT_IsBuddyFree_size);

    parentSlot = &g_mtFreeLists[sizeClass];
    current = *parentSlot;

    if (!current)
        return 0;

    midpoint = 0;
    halfRange = MEMORY_NODE_COUNT;

    /* midpoint-based BST search for buddyNum */
    while (current != buddyNum)
    {
        if (buddyNum == (unsigned int)midpoint)
            return 0; /* can't find it */

        halfRange >>= 1;

        if ((int)buddyNum < midpoint)
        {
            midpoint -= halfRange;
            parentSlot = &MT_NODE_LEFT(current);
        }
        else
        {
            midpoint += halfRange;
            parentSlot = &MT_NODE_RIGHT(current);
        }

        current = *parentSlot;
        if (!current)
            return 0;
    }

    /* found — remove from tree using score-based restructuring */
    {
        unsigned __int64 children = *(unsigned __int64 *)((char *)g_mtNodeArray + current * MT_NODE_SIZE);
        unsigned short left = (unsigned short)children;
        unsigned short right = (unsigned short)(children >> 16);

        if (!left)
        {
            /* no left child — replace with right */
            *parentSlot = right;
            if (!right)
                return 1;
            /* continue restructuring down right subtree */
        }
        else if (!right)
        {
            /* no right child — replace with left */
            *parentSlot = left;
        }
        else
        {
            /* both children — compare scores to pick replacement */
            int leftScore = MT_NodeScore(left);
            int rightScore = MT_NodeScore(right);

            Assert(leftScore != rightScore,
                   s_assertDisable_MT_IsBuddyFree_scores);

            if (leftScore < rightScore)
            {
                /* right has higher score — promote right */
                *parentSlot = right;
                parentSlot = &MT_NODE_RIGHT(*parentSlot);
                /* copy left to new position, continue down */
            }
            else
            {
                /* left has higher score — promote left */
                *parentSlot = left;
                parentSlot = &MT_NODE_LEFT(*parentSlot);
            }

            /* recursive restructure: walk down replacing nodes */
            /* The binary does this iteratively, swapping parent/child
               data and following the tree until reaching a leaf */
            unsigned int node = current;
            unsigned __int64 childData = children;

            while (1)
            {
                left = (unsigned short)childData;
                right = (unsigned short)(childData >> 16);

                if (!left)
                {
                    *parentSlot = right;
                    if (!right)
                        break; /* both zero — done */
                    /* swap data and continue with right's children */
                    {
                        unsigned __int64 oldData = *(unsigned __int64 *)((char *)g_mtNodeArray + right * MT_NODE_SIZE);
                        *(unsigned __int64 *)((char *)g_mtNodeArray + right * MT_NODE_SIZE) = childData;
                        childData = oldData;
                        parentSlot = &MT_NODE_RIGHT(right);
                    }
                    continue;
                }
                if (!right)
                {
                    *parentSlot = left;
                    /* swap data and continue with left's children */
                    {
                        unsigned __int64 oldData = *(unsigned __int64 *)((char *)g_mtNodeArray + left * MT_NODE_SIZE);
                        *(unsigned __int64 *)((char *)g_mtNodeArray + left * MT_NODE_SIZE) = childData;
                        childData = oldData;
                        parentSlot = &MT_NODE_LEFT(left);
                    }
                    continue;
                }

                leftScore = MT_NodeScore(left);
                rightScore = MT_NodeScore(right);

                if (leftScore < rightScore)
                {
                    *parentSlot = right;
                    /* swap data */
                    unsigned __int64 oldData = *(unsigned __int64 *)((char *)g_mtNodeArray + right * MT_NODE_SIZE);
                    *(unsigned __int64 *)((char *)g_mtNodeArray + right * MT_NODE_SIZE) = childData;
                    childData = oldData;
                    parentSlot = &MT_NODE_RIGHT(right); /* binary: +2 when right wins */
                }
                else
                {
                    *parentSlot = left;
                    unsigned __int64 oldData = *(unsigned __int64 *)((char *)g_mtNodeArray + left * MT_NODE_SIZE);
                    *(unsigned __int64 *)((char *)g_mtNodeArray + left * MT_NODE_SIZE) = childData;
                    childData = oldData;
                    parentSlot = &MT_NODE_LEFT(left); /* binary: +0 when left wins */
                }
            }
        }
    }

    return 1;
}

/*
================
MT_Init

Initializes the memory tree buddy allocator.
1. Sets g_scrStringMT to the node array base.
2. Builds 3 lookup tables (256 entries each) for fast bit ops.
3. Initializes free lists: one free block at each size class
   (node 1 at size 0, node 2 at size 1, ... node 32768 at size 15).
4. Clears allocation counters and debug arrays.

================
*/
void MT_Init(void)
{
    unsigned int edx, ecx, eax;
    int i;
    unsigned int nodeNum;

    /* set node array base pointer */
    g_scrStringMT = (void *)g_mtNodeArray;

    /* build lookup tables for byte → size class mapping */
    for (edx = 0; edx < 256; edx++)
    {
        /* table at +0x100: popcount (number of set bits in edx) */
        ecx = 0;
        eax = edx;
        if (edx != 0)
        {
            while (eax)
            {
                if (eax & 1)
                    ecx++;
                eax >>= 1;
            }
        }
        g_mtLookup[0x100 + edx] = (unsigned char)ecx;

        /* table at +0x000: count trailing zeros (0..8, clamped to 8 for edx=0) */
        ecx = 8;
        if (edx != 0)
        {
            do
            {
                ecx--;
                eax = (1 << ecx) - 1;
            } while (edx & eax);
        }
        g_mtLookup[edx] = (unsigned char)ecx;

        /* table at +0x200: bit-length (floor(log2(x))+1 for x>0, 0 for x=0) */
        eax = 0;
        ecx = edx;
        if (edx != 0)
        {
            while (ecx)
            {
                eax++;
                ecx >>= 1;
            }
        }
        g_mtLookup[0x200 + edx] = (unsigned char)eax;
    }

    /* clear free list structures */
    memset(g_mtFreeLists, 0, sizeof(g_mtFreeLists));

    /* explicitly zero first node's left/right links (binary emits these) */
    MT_NODE_LEFT(0) = 0;
    MT_NODE_RIGHT(0) = 0;

    /* initialize free lists: one block per size class */
    nodeNum = 1;
    for (i = 0; i < MEMORY_NODE_BITS; i++)
    {
        MT_AddToFreeList(nodeNum, i);
        nodeNum <<= 1; /* 1, 2, 4, 8, ..., 32768 */
    }

    /* clear counters and debug arrays */
    g_mtAllocCount = 0;
    g_mtAllocBytes = 0;
    memset(g_mtDebugUsage, 0, MEMORY_NODE_COUNT);
    memset(g_mtDebugSize, 0, MEMORY_NODE_COUNT);
}

/*
================
MT_GetSize

Converts a byte count to a buddy allocator size class (0..16).
Uses the precomputed lookup table for fast conversion.
Size class N means the allocation covers 2^N nodes.

LST: inlined in MT_AllocIndex/MT_FreeIndex/MT_Free
================
*/
static int MT_GetSize(int numBytes)
{
    int idx;
    int size;

    if (numBytes >= MEMORY_NODE_COUNT)
        return -1; /* will trigger error in caller */

    idx = (numBytes + 7) / 8 - 1;

    if (idx <= 255)
    {
        size = g_mtLookup[0x200 + idx]; /* binary uses +0x200 table (bit-width) */
    }
    else
    {
        size = g_mtLookup[0x200 + (idx >> 8)] + 8;
    }

    Assert(size >= 0 && size <= MEMORY_NODE_BITS,
           s_assertDisable_MT_GetSize);

    return size;
}

/*
================
MT_AllocIndex

Allocates a node from the buddy allocator.
Finds the smallest free block >= requested size,
removes it from the free list, splits if larger,
and returns the node number.

================
*/
unsigned short MT_AllocIndex(int numBytes, int context)
{
    int size;
    int s;
    unsigned int nodeNum;

    Assert(numBytes > 0, s_assertDisable_MT_AllocIndex_bytes);

    size = MT_GetSize(numBytes);

    if (size > MEMORY_NODE_BITS)
        Com_Error(1, "%s: failed allocation of %d bytes for script usage",
                  "MT_AllocIndex", numBytes);

    /* search for a free block at this size or larger */
    s = size;
    while (s <= MEMORY_NODE_BITS)
    {
        nodeNum = g_mtFreeLists[s];
        if (nodeNum)
            break;
        s++;
    }

    if (s > MEMORY_NODE_BITS)
        Com_Error(1, "%s: failed allocation of %d bytes for script usage",
                  "MT_AllocIndex", numBytes);

    /* remove the found block from its free list */
    MT_RemoveFromFreeList(s);

    /* split down to the requested size */
    while (s != size)
    {
        s--;
        MT_AddToFreeList(nodeNum + (1 << s), s);
    }

    /* validate nodeNum range */
    Assert(nodeNum >= 0 && nodeNum < MEMORY_NODE_COUNT,
           s_assertDisable_MT_AllocIndex_range);

    /* update counters */
    g_mtAllocCount++;
    g_mtAllocBytes += 1 << size;

    /* assert context is valid */
    Assert(context, s_assertDisable_MT_AllocIndex_type);

    /* debug tracking */
    Assert(!g_mtDebugUsage[nodeNum], s_assertDisable_MT_AllocIndex_usage);
    Assert(!g_mtDebugSize[nodeNum], s_assertDisable_MT_AllocIndex_size);

    g_mtDebugUsage[nodeNum] = (unsigned char)context;
    g_mtDebugSize[nodeNum] = (unsigned char)size;

    return (unsigned short)nodeNum;
}

/*
================
MT_FreeIndex

Frees a node back to the buddy allocator.
Merges with buddy if buddy is also free (coalescing),
repeating up the size classes until no more merging possible.

================
*/
void MT_FreeIndex(unsigned int nodeNum, int numBytes)
{
    int size;
    unsigned int buddy;

    size = MT_GetSize(numBytes);

    Assert(size >= 0 && (unsigned)size <= MEMORY_NODE_BITS,
           s_assertDisable_MT_FreeIndex_size);
    Assert(nodeNum > 0 && nodeNum < MEMORY_NODE_COUNT,
           s_assertDisable_MT_FreeIndex_node);
    /* binary has no Sys_IsMainThread assert here — phantom removed */

    /* update counters */
    g_mtAllocCount--;
    g_mtAllocBytes -= 1 << size;

    /* debug tracking */
    Assert(g_mtDebugUsage[nodeNum],
           s_assertDisable_MT_FreeIndex_usage);
    Assert(g_mtDebugSize[nodeNum] == size,
           s_assertDisable_MT_FreeIndex_sizeMatch);

    g_mtDebugUsage[nodeNum] = 0;
    g_mtDebugSize[nodeNum] = 0;

    /* merge with buddy (coalesce) */
    while (1)
    {
        Assert(size <= MEMORY_NODE_BITS,
               s_assertDisable_MT_FreeIndex_maxSize);

        Assert(nodeNum == (nodeNum & ~((1 << size) - 1)),
               s_assertDisable_MT_FreeIndex_aligned);

        buddy = nodeNum ^ (1 << size);

        if (size == MEMORY_NODE_BITS || !MT_IsBuddyFree(buddy, size))
            break;

        /* buddy is free — merge */
        nodeNum &= ~(1 << size);
        size++;
    }

    MT_AddToFreeList(nodeNum, size);
}

/*
================
MT_Free

Frees memory by pointer. Computes the node number from
the pointer offset, then calls MT_FreeIndex.

================
*/
void MT_Free(void *ptr, int numBytes)
{
    unsigned int nodeNum;

    Assert(numBytes > 0, s_assertDisable_MT_Free_bytes);

    nodeNum = (unsigned int)(((unsigned char *)ptr - (unsigned char *)g_scrStringMT) / MT_NODE_SIZE);

    Assert(nodeNum < MEMORY_NODE_COUNT,
           s_assertDisable_MT_Free_node);

    MT_FreeIndex(nodeNum, numBytes);
}

void MT_GetNodeInfo(unsigned int nodeNum); /* forward decl */

/*
================
MT_FreeBitmapUnused

Iterates all 65536 nodes. For each node NOT marked in the
bitmap, calls MT_GetNodeInfo for validation. Then frees
the bitmap via Z_VirtualFree tail call.

================
*/
/*
================
MT_ReAllocFromBitmap

Reallocate a protected string node during SL_Shutdown's re-packing phase.
Inlines MT_GetSize to compute a size class from numBytes, updates alloc
counters and debug shadow arrays, then sets (1 << size) consecutive bits
in the allocation bitmap starting at nodeNum.
================
*/
void MT_ReAllocFromBitmap(void *allocBits, unsigned int nodeNum, int numBytes)
{
    unsigned char *bitmap = (unsigned char *)allocBits;
    int size;
    unsigned int numNodes;
    unsigned int bit;

    Assert(numBytes > 0, s_assertDisable_MT_ReAllocFromBitmap_bytes);

    if (numBytes >= MEMORY_NODE_COUNT)
    {
        Com_Error(1, "%s: failed allocation of %d bytes for script usage",
                  "MT_GetSize: max allocation exceeded", numBytes);
        size = 0;
    }
    else
    {
        int idx = (numBytes + 7) / 8 - 1;
        if (idx <= 0xFF)
            size = g_mtLookup[0x200 + idx];
        else
            size = g_mtLookup[0x200 + (idx >> 8)] + 8;
    }

    Assert(size >= 0 && size <= MEMORY_NODE_BITS,
           s_assertDisable_MT_ReAllocFromBitmap_size);

    g_mtAllocCount++;
    g_mtAllocBytes += 1 << size;

    Assert(!g_mtDebugUsage[nodeNum], s_assertDisable_MT_ReAllocFromBitmap_usage);
    Assert(!g_mtDebugSize[nodeNum],  s_assertDisable_MT_ReAllocFromBitmap_sizeArr);

    g_mtDebugUsage[nodeNum] = 6;
    g_mtDebugSize[nodeNum]  = (unsigned char)size;

    /* set (1 << size) consecutive bits starting at nodeNum */
    numNodes = 1u << size;
    bit = nodeNum;
    while (numNodes)
    {
        bitmap[bit >> 3] |= (unsigned char)(1u << (bit & 7u));
        bit++;
        numNodes--;
    }
}

/*
================
MT_AllocBitmap

Binary at 0x421770. Resets the memory-tree debug allocation counters,
zeros the debug-usage and debug-size shadow arrays, and returns a fresh
8192-byte bitmap from Z_VirtualAlloc.
================
*/
void *MT_AllocBitmap(void)
{
    g_mtAllocCount = 0;
    g_mtAllocBytes = 0;
    memset(g_mtDebugUsage, 0, MEMORY_NODE_COUNT);
    memset(g_mtDebugSize, 0, MEMORY_NODE_COUNT);
    return Z_VirtualAlloc(0x2000);
}

void MT_FreeBitmapUnused(unsigned char *bitmap)
{
    int i;
    unsigned char bitMask;

    bitMask = 2;
    for (i = 1; i < MEMORY_NODE_COUNT; i++)
    {
        if (!(bitmap[i >> 3] & bitMask))
        {
            MT_GetNodeInfo(i);
        }
        bitMask = (bitMask << 1) | (bitMask >> 7); /* rol 1 */
    }

    Z_VirtualFree(bitmap);
}

/*
================
MT_GetNodeInfo

Coalesce a freed node with buddies and add to free list.
Called from MT_FreeBitmapUnused for each unmarked node. Two phases:
1. Upward buddy search: at increasing size classes, check if buddy is free.
   If not free, stop. If free, merge (mask off low bit, go up).
2. Downward coalesce: from original nodeNum at size 0, check buddies going up.
   If buddy is free, merge and continue.
Finally tail-calls MT_AddToFreeList with the resulting merged block.
================
*/
static char s_assertDisable_MT_GetNodeInfo_node;
static char s_assertDisable_MT_GetNodeInfo_size;
static char s_assertDisable_MT_GetNodeInfo_align;
static char s_assertDisable_MT_GetNodeInfo_size2;
static char s_assertDisable_MT_GetNodeInfo_align2;

void MT_GetNodeInfo(unsigned int nodeNum)
{
    int sizeClass;
    int lowBit;
    unsigned int origNodeNum = nodeNum;

    Assert(nodeNum > 0 && nodeNum < MEMORY_NODE_COUNT,
           s_assertDisable_MT_GetNodeInfo_node);

    /* phase 1: upward buddy search from nodeNum */
    sizeClass = 0;
    while (1)
    {
        Assert(sizeClass <= MEMORY_NODE_BITS,
               s_assertDisable_MT_GetNodeInfo_size);

        lowBit = 1 << sizeClass;
        Assert(nodeNum == (nodeNum & ~(unsigned int)(lowBit - 1)),
               s_assertDisable_MT_GetNodeInfo_align);

        if (MT_IsBuddyFree(nodeNum, sizeClass))
        {
            /* buddy is free — add merged block to free list */
            MT_AddToFreeList(nodeNum, sizeClass);
            return;
        }

        if (sizeClass == MEMORY_NODE_BITS)
            break; /* reached max size, go to phase 2 */

        /* merge upward: clear low bit, go to next size */
        nodeNum &= ~lowBit;
        sizeClass++;
    }

    /* phase 2: downward coalesce from original nodeNum */
    sizeClass = 0;
    while (1)
    {
        Assert(sizeClass <= MEMORY_NODE_BITS,
               s_assertDisable_MT_GetNodeInfo_size2);

        lowBit = 1 << sizeClass;
        Assert(origNodeNum == (origNodeNum & ~(unsigned int)(lowBit - 1)),
               s_assertDisable_MT_GetNodeInfo_align2);

        if (sizeClass == MEMORY_NODE_BITS)
            break;

        /* check buddy = nodeNum XOR lowBit */
        if (!MT_IsBuddyFree(origNodeNum ^ lowBit, sizeClass))
            break;

        /* buddy is free — merge: clear low bit, go up */
        origNodeNum &= ~lowBit;
        sizeClass++;
    }

    /* add the final merged block to free list */
    MT_AddToFreeList(origNodeNum, sizeClass);
}

/* MT_Reset phantom deleted — was a duplicate of MT_AllocBitmap (same body at 0x421770). */
