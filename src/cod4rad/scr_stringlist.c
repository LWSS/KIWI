/*
 * scr_stringlist.c — Script string table system.
 *
 * Manages interned strings via a hash table with reference counting.
 * Each string handle is a 16-bit index into a memory tree (MT) array.
 */

#include "cod2rad64.h"

#define MT_SIZE         0x80000
#define SL_MAX_ENTRIES  0x4E20

void *g_scrStringMT;

/* assert disable flags */
static char s_assertDisable_SL_Init_hash;
static char s_assertDisable_SL_Init_tail;
static char s_assertDisable_SL_FreeString_refCount;
static char s_assertDisable_SL_FreeString_user;
static char s_assertDisable_SL_FreeString_head;
static char s_assertDisable_SL_FreeString_notSelf;
static char s_assertDisable_SL_FreeString_movable;
static char s_assertDisable_SL_FreeString_notSelf2;
static char s_assertDisable_SL_FreeString_movable2;
static char s_assertDisable_SL_FreeString_free;
static char s_assertDisable_SL_FreeString_freeHead;
static char s_assertDisable_SL_GetStringOfLen;
static char s_assertDisable_SL_GetStringOfLen_sv;
static char s_assertDisable_SL_GetStringOfLen_bounds;
static char s_assertDisable_SL_GetStringOfLen_stat;
static char s_assertDisable_SL_GetStringOfLen_match;
static char s_assertDisable_SL_GetStringOfLen_movable;
static char s_assertDisable_SL_GetStringOfLen_sv2;
static char s_assertDisable_SL_GetStringOfLen_bounds2;
static char s_assertDisable_SL_GetStringOfLen_stat2;
static char s_assertDisable_SL_GetStringOfLen_stat3;
static char s_assertDisable_SL_GetStringOfLen_match2;
static char s_assertDisable_SL_GetStringOfLen_free;
static char s_assertDisable_SL_GetStringOfLen_prev;
static char s_assertDisable_SL_GetStringOfLen_freeSlot;
static char s_assertDisable_SL_GetStringOfLen_hashClean;
static char s_assertDisable_SL_GetStringOfLen_result;
static char s_assertDisable_SL_GetStringOfLen_user;
static char s_assertDisable_SL_GetStringOfLen_final;
static char s_assertDisable_SL_GetStringOfLen_finalMatch;
static char s_assertDisable_SL_FindStringOfLen;
static char s_assertDisable_SL_FindStringOfLen_sv;
static char s_assertDisable_SL_FindStringOfLen_bounds;
static char s_assertDisable_SL_FindStringOfLen_stat;
static char s_assertDisable_SL_FindStringOfLen_match;
static char s_assertDisable_SL_FindStringOfLen_movable;
static char s_assertDisable_SL_FindStringOfLen_sv2;
static char s_assertDisable_SL_FindStringOfLen_bounds2;
static char s_assertDisable_SL_FindStringOfLen_stat2;
static char s_assertDisable_SL_ConvertToString;
static char s_assertDisable_SL_DebugConvertToString;
static char s_assertDisable_SL_Shutdown_sv;
static char s_assertDisable_SL_Shutdown_bounds;
static char s_assertDisable_SL_RemoveRefToString;
static char s_assertDisable_SL_RemoveRefToString_bounds;
static char s_assertDisable_SL_RemoveRefToString_2;
static char s_assertDisable_SL_RemoveRefToString_bounds2;

/*
 * Hash table entry: 4 bytes each.
 *   +0: unsigned short status_next  (low 14 bits = next, bits 14-15 = status)
 *   +2: unsigned short prev_or_value
 *
 * Status bits (HASH_STAT_MASK = 0xC000):
 *   0x0000 = HASH_STAT_FREE
 *   0x4000 = HASH_STAT_MOVABLE
 *   0x8000 = HASH_STAT_HEAD
 *
 * HASH_NEXT_MASK = 0x3FFF
 */
#define HASH_STAT_MASK  0xC000
#define HASH_NEXT_MASK  0x3FFF
#define HASH_STAT_FREE  0x0000
#define HASH_STAT_MOVABLE 0x4000
#define HASH_STAT_HEAD  0x8000
#define SL_HASH_SIZE    0x4000

typedef struct {
    unsigned short status_next;     /* +0 */
    unsigned short prev_or_value;   /* +2 */
} SL_HashEntry;

static SL_HashEntry g_scrStringHash[SL_HASH_SIZE]; /* word_63DC900 */
static unsigned char g_scrStringInited;
static void *g_scrStringShutdownFlag;

/*
 * SL_GetRefStr — returns pointer to the refStr header for a string handle.
 * Each node is 8 bytes: header(4) + str data.
 */
#define SL_GetRefStr(stringValue) ((ScriptStringRef *)((char *)g_scrStringMT + (unsigned int)(stringValue) * MT_NODE_SIZE))

/*
================
SL_InitOrShutdown

Dispatch: if already initialized, calls SL_Shutdown.
Otherwise calls SL_Init.

================
*/
void SL_InitOrShutdown(void)
{
    if (g_scrStringInited)
        SL_Shutdown();
    else
        SL_Init();
}

/*
================
SL_Init

Initializes the script string hash table.
Calls MT_Init, then links all entries 1..0x3FFF into a free list.

================
*/
void SL_Init(void)
{
    int prev;
    unsigned int i;

    MT_Init();

    g_scrStringHash[0].status_next = 0;
    prev = 0;

    for (i = 1; i < SL_HASH_SIZE; i++)
    {
        Assert(!(i & HASH_STAT_MASK),
               s_assertDisable_SL_Init_hash);

        g_scrStringHash[i].status_next = 0;
        g_scrStringHash[prev].status_next |= (unsigned short)i;
        g_scrStringHash[i].prev_or_value = (unsigned short)prev;
        prev = i;
    }

    Assert(!(g_scrStringHash[prev].status_next & HASH_NEXT_MASK),
           s_assertDisable_SL_Init_tail);

    g_scrStringHash[0].prev_or_value = (unsigned short)prev;
    g_scrStringInited = 1;
}

/*
================
SL_HashString

Computes hash for a string of given length.
Uses byte-by-byte with multiplier 0x1F for short strings,
length/4 for long strings. Result range: 1..0x3FFF.
================
*/
static unsigned int SL_HashString(const char *str, unsigned int len)
{
    unsigned int hash;
    unsigned int i;

    if (len < 256)
    {
        hash = 0;
        for (i = 0; i < len; i++)
            hash = hash * 31 + (signed char)str[i];
    }
    else
    {
        hash = len >> 2;
    }

    /* modulo 0x3FFF via multiply-shift trick, then +1 */
    {
        unsigned int q = (unsigned int)(((unsigned long long)hash * 0x40011ULL) >> 32);
        unsigned int t = hash - q;
        t >>= 1;
        t += q;
        t >>= 13;
        t *= 0x3FFF;
        hash -= t;
    }

    return hash + 1;
}

/*
================
SL_FreeString

Removes a string from the hash table and frees its MT allocation.
Called when refcount reaches 0.

Params:
  stringValue — the string handle being freed
  len         — total string length (for hash recomputation and MT free)

================
*/
void SL_FreeString(unsigned int stringValue, ScriptStringRef *refStr, unsigned int len)
{
    unsigned int hash;
    SL_HashEntry *entry;
    SL_HashEntry *newEntry;
    unsigned int next;
    unsigned int prev;

    refStr = SL_GetRefStr(stringValue);

    /* recompute hash from string content */
    hash = SL_HashString(refStr->str, len);

    entry = &g_scrStringHash[hash];

    Assert(refStr->refCount == 0,
           s_assertDisable_SL_FreeString_refCount);

    Assert(refStr->user == 0,
           s_assertDisable_SL_FreeString_user);

    /* free the MT node */
    MT_FreeIndex(stringValue, len + 4);

    /* assert entry is HASH_STAT_HEAD */
    Assert((entry->status_next & HASH_STAT_MASK) == HASH_STAT_HEAD,
           s_assertDisable_SL_FreeString_head);

    /* find and remove from hash chain */
    next = entry->status_next & HASH_NEXT_MASK;
    newEntry = &g_scrStringHash[next];

    if (entry->prev_or_value == stringValue)
    {
        /* our string is at the head */
        if (newEntry == entry)
        {
            /* only entry in chain — entry itself is freed */
            newEntry = entry;
            next = hash;
        }
        else
        {
            /* promote next entry to head: copy its next/value into head slot */
            entry->status_next = (newEntry->status_next & HASH_NEXT_MASK) | HASH_STAT_HEAD;
            entry->prev_or_value = newEntry->prev_or_value;
            g_scrStringShutdownFlag = entry; /* signal SL_Shutdown that hash table restructured */
        }
    }
    else
    {
        /* our string is deeper in the chain — search for it */
        Assert(newEntry != entry, s_assertDisable_SL_FreeString_notSelf);
        Assert((newEntry->status_next & HASH_STAT_MASK) == HASH_STAT_MOVABLE,
               s_assertDisable_SL_FreeString_movable);

        prev = hash;

        while (newEntry->prev_or_value != stringValue)
        {
            prev = next;
            next = newEntry->status_next & HASH_NEXT_MASK;
            newEntry = &g_scrStringHash[next];
            Assert(newEntry != entry, s_assertDisable_SL_FreeString_notSelf2);
            Assert((newEntry->status_next & HASH_STAT_MASK) == HASH_STAT_MOVABLE,
                   s_assertDisable_SL_FreeString_movable2);
        }

        /* unlink: predecessor's next = our next (XOR swap of next bits) */
        {
            unsigned short prevSN = g_scrStringHash[prev].status_next;
            unsigned short ourSN = newEntry->status_next;
            unsigned short xorNext = (prevSN ^ ourSN) & HASH_NEXT_MASK;
            g_scrStringHash[prev].status_next ^= xorNext;
        }
    }

    /* freed entry goes to free list head (doubly linked) */
    Assert(newEntry->status_next & HASH_STAT_MASK,
           s_assertDisable_SL_FreeString_free);

    {
        unsigned short freeHead = g_scrStringHash[0].status_next;
        Assert(!(freeHead & HASH_STAT_MASK),
               s_assertDisable_SL_FreeString_freeHead);

        newEntry->status_next = freeHead;
        newEntry->prev_or_value = 0;
        g_scrStringHash[freeHead].prev_or_value = (unsigned short)next;
        g_scrStringHash[0].status_next = (unsigned short)next;
    }
}

/*
================
SL_GetString

Wrapper: computes strlen then calls SL_GetStringOfLen.

================
*/
unsigned int SL_GetString(const char *str, unsigned int user, int context)
{
    return SL_GetStringOfLen(str, user, (unsigned int)strlen(str) + 1, context);
}

/*
================
SL_GetStringOfLen

Finds or creates a string in the hash table. If the string
already exists, increments refcount and merges user flags.
If not found, allocates a new MT node and hash entry.

Three insertion cases for hash collisions:
  1. Bucket is HASH_STAT_FREE: allocate MT node, set as HEAD
  2. Bucket is HASH_STAT_HEAD (our hash): chain new MOVABLE entry
  3. Bucket is HASH_STAT_MOVABLE (other hash): evict, relocate, insert HEAD

Params:
  str  — string data to intern
  user — user flags (merged with existing on refcount bump)
  len  — string length (bytes, NOT including null)
  context — passed to MT_AllocIndex

================
*/
unsigned int SL_GetStringOfLen(const char *str, unsigned int user, unsigned int len, int context)
{
    unsigned int hash;
    SL_HashEntry *entry;
    SL_HashEntry *newEntry;
    ScriptStringRef *refStr;
    unsigned int stringValue;
    unsigned int freeIdx;
    unsigned int next;
    unsigned int prev;
    unsigned short mtIndex;

    Assert(str, s_assertDisable_SL_GetStringOfLen);

    hash = SL_HashString(str, len);
    entry = &g_scrStringHash[hash];

    /* case: bucket has a HEAD entry for our hash */
    if ((entry->status_next & HASH_STAT_MASK) == HASH_STAT_HEAD)
    {
        /* check head entry */
        stringValue = entry->prev_or_value;
        Assert(stringValue, s_assertDisable_SL_GetStringOfLen_sv);
        Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
               s_assertDisable_SL_GetStringOfLen_bounds);

        refStr = SL_GetRefStr(stringValue);

        if (refStr->len == (unsigned char)len
            && memcmp(refStr->str, str, len) == 0)
        {
            /* found at head — bump refcount, merge user flags */
            if (!((unsigned char)user & refStr->user))
            {
                refStr->refCount += 1;
                refStr->user |= (unsigned char)user;
            }

            Assert(entry->status_next & HASH_STAT_MASK,
                   s_assertDisable_SL_GetStringOfLen_stat);
            Assert(refStr->str == SL_ConvertToString(entry->prev_or_value),
                   s_assertDisable_SL_GetStringOfLen_match);

            return entry->prev_or_value;
        }

        /* walk chain looking for match */
        next = entry->status_next & HASH_NEXT_MASK;
        newEntry = &g_scrStringHash[next];
        prev = hash;

        while (newEntry != entry)
        {
            Assert((newEntry->status_next & HASH_STAT_MASK) == HASH_STAT_MOVABLE,
                   s_assertDisable_SL_GetStringOfLen_movable);

            stringValue = newEntry->prev_or_value;
            Assert(stringValue, s_assertDisable_SL_GetStringOfLen_sv2);
            Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
                   s_assertDisable_SL_GetStringOfLen_bounds2);

            refStr = (char *)g_scrStringMT + stringValue * MT_NODE_SIZE;

            if (refStr->len == (unsigned char)len
                && memcmp(refStr->str, str, len) == 0)
            {
                /* found in chain — promote to head via XOR swap, bump refcount */

                /* 1. prev.next = newEntry.next (skip newEntry in chain) */
                {
                    unsigned short xor1 = (g_scrStringHash[prev].status_next ^ newEntry->status_next) & HASH_NEXT_MASK;
                    g_scrStringHash[prev].status_next ^= xor1;
                }

                /* 2. newEntry.next = entry(head).next */
                {
                    unsigned short xor2 = (entry->status_next ^ newEntry->status_next) & HASH_NEXT_MASK;
                    newEntry->status_next ^= xor2;
                }

                /* 3. entry(head).next = newEntry index */
                entry->status_next = (entry->status_next & HASH_STAT_MASK) | (unsigned short)next;

                /* 4. swap stringValues between head and newEntry */
                {
                    unsigned short tmpVal = entry->prev_or_value;
                    entry->prev_or_value = newEntry->prev_or_value;
                    newEntry->prev_or_value = tmpVal;
                }

                stringValue = entry->prev_or_value;
                refStr = SL_GetRefStr(stringValue);

                if (!((unsigned char)user & refStr->user))
                {
                    refStr->refCount += 1;
                    refStr->user |= (unsigned char)user;
                }

                Assert(newEntry->status_next & HASH_STAT_MASK,
                       s_assertDisable_SL_GetStringOfLen_stat2);
                Assert(entry->status_next & HASH_STAT_MASK,
                       s_assertDisable_SL_GetStringOfLen_stat3);
                Assert(refStr->str == SL_ConvertToString(stringValue),
                       s_assertDisable_SL_GetStringOfLen_match2);

                return stringValue;
            }

            prev = next;
            next = newEntry->status_next & HASH_NEXT_MASK;
            newEntry = &g_scrStringHash[next];
        }

        /* not found in chain — allocate new entry from free list */
        freeIdx = g_scrStringHash[0].status_next & HASH_NEXT_MASK;
        if (!freeIdx)
            Com_Error(1, "exceeded maximum number of script strings (increase STRINGLIST_SIZE)\n");

        mtIndex = MT_AllocIndex(len + 4, context);

        Assert(!(g_scrStringHash[freeIdx].status_next & HASH_STAT_MASK),
               s_assertDisable_SL_GetStringOfLen_free);

        /* remove from free list */
        g_scrStringHash[0].status_next = g_scrStringHash[freeIdx].status_next & HASH_NEXT_MASK;
        g_scrStringHash[g_scrStringHash[0].status_next & HASH_NEXT_MASK].prev_or_value = 0;

        /* link new entry into chain as MOVABLE */
        g_scrStringHash[freeIdx].status_next =
            (entry->status_next & HASH_NEXT_MASK) | HASH_STAT_MOVABLE;
        g_scrStringHash[freeIdx].prev_or_value = entry->prev_or_value;

        entry->status_next = (unsigned short)freeIdx | (entry->status_next & HASH_STAT_MASK);

        stringValue = mtIndex;
        goto fill_and_return_no_status; /* binary skips the status_next overwrite for this path */
    }

    /* case: bucket has a MOVABLE entry (from another hash) or is FREE */
    if ((entry->status_next & HASH_STAT_MASK) == HASH_STAT_MOVABLE)
    {
        /* find predecessor of the MOVABLE in its original chain */
        unsigned int movableNext = entry->status_next & HASH_NEXT_MASK;
        unsigned int predIdx = movableNext;
        SL_HashEntry *pred;

        /* search the MOVABLE's original chain for who points to our hash bucket */
        pred = &g_scrStringHash[predIdx];
        while ((pred->status_next & HASH_NEXT_MASK) != hash)
        {
            predIdx = pred->status_next & HASH_NEXT_MASK;
            pred = &g_scrStringHash[predIdx];
        }
        Assert(predIdx, s_assertDisable_SL_GetStringOfLen_prev);

        /* get a free slot to relocate the MOVABLE into */
        freeIdx = g_scrStringHash[0].status_next & HASH_NEXT_MASK;
        if (!freeIdx)
            Com_Error(1, "exceeded maximum number of script strings\n");

        mtIndex = MT_AllocIndex(len + 4, context);

        Assert(!(g_scrStringHash[freeIdx].status_next & HASH_STAT_MASK),
               s_assertDisable_SL_GetStringOfLen_freeSlot);

        /* remove free slot from free list */
        g_scrStringHash[0].status_next =
            g_scrStringHash[freeIdx].status_next & HASH_NEXT_MASK;
        g_scrStringHash[g_scrStringHash[0].status_next].prev_or_value = 0;

        /* redirect predecessor to free slot */
        g_scrStringHash[predIdx].status_next =
            (g_scrStringHash[predIdx].status_next & HASH_STAT_MASK) | (unsigned short)freeIdx;

        /* copy MOVABLE's chain link + value to free slot */
        g_scrStringHash[freeIdx].status_next =
            (entry->status_next & HASH_NEXT_MASK) | HASH_STAT_MOVABLE;
        g_scrStringHash[freeIdx].prev_or_value = entry->prev_or_value;

        stringValue = mtIndex;
        goto fill_and_return;
    }

    /* case: bucket is FREE — simple allocation */
    {
        unsigned short nextFree = entry->status_next & HASH_NEXT_MASK;
        unsigned short prevFree = entry->prev_or_value;

        mtIndex = MT_AllocIndex(len + 4, context);

        /* unlink from free list */
        g_scrStringHash[prevFree].status_next =
            nextFree | (g_scrStringHash[prevFree].status_next & HASH_STAT_MASK);
        g_scrStringHash[nextFree].prev_or_value = prevFree;

        stringValue = mtIndex;
    }

fill_and_return:
    /* set entry as HEAD with our hash */
    Assert(!(hash & HASH_STAT_MASK),
           s_assertDisable_SL_GetStringOfLen_hashClean);
    entry->status_next = (unsigned short)hash | HASH_STAT_HEAD;

fill_and_return_no_status:
    Assert(stringValue, s_assertDisable_SL_GetStringOfLen_result);

    entry->prev_or_value = (unsigned short)stringValue;

    /* fill MT node header + copy string data */
    refStr = SL_GetRefStr(stringValue);
    memcpy(refStr->str, str, len);
    refStr->user = (unsigned char)user;
    Assert((unsigned char)user == refStr->user,
           s_assertDisable_SL_GetStringOfLen_user);
    refStr->refCount = 1;  /* refCount = 1 */
    refStr->len = (unsigned char)len;

    Assert(entry->status_next & HASH_STAT_MASK,
           s_assertDisable_SL_GetStringOfLen_final);
    Assert(refStr->str == SL_ConvertToString(stringValue),
           s_assertDisable_SL_GetStringOfLen_finalMatch);

    return stringValue;
}

/*
================
SL_FindString

Wrapper: computes strlen then calls SL_FindStringOfLen.

================
*/
unsigned int SL_FindString(const char *str)
{
    return SL_FindStringOfLen(str, (unsigned int)strlen(str) + 1);
}

/*
================
SL_FindStringOfLen

Searches the hash table for an existing string of exact length.
Returns the string handle if found, 0 if not.
When found deep in a chain, swaps entries to improve lookup speed.

================
*/
unsigned int SL_FindStringOfLen(const char *str, unsigned int len)
{
    unsigned int hash;
    SL_HashEntry *headEntry;
    SL_HashEntry *entry;
    unsigned int stringValue;
    ScriptStringRef *refStr;
    unsigned int next;
    unsigned int prev;

    Assert(str, s_assertDisable_SL_FindStringOfLen);

    hash = SL_HashString(str, len);
    headEntry = &g_scrStringHash[hash];

    /* check if hash bucket has a head entry */
    if ((headEntry->status_next & HASH_STAT_MASK) != HASH_STAT_HEAD)
        return 0;

    /* check head entry's string */
    stringValue = headEntry->prev_or_value;
    Assert(stringValue, s_assertDisable_SL_FindStringOfLen_sv);
    Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
           s_assertDisable_SL_FindStringOfLen_bounds);

    refStr = SL_GetRefStr(stringValue);

    if (refStr->len == (unsigned char)len)
    {
        if (memcmp(refStr->str, str, len) == 0)
        {
            /* found at head — assert consistency and return */
            Assert(headEntry->status_next & HASH_STAT_MASK,
                   s_assertDisable_SL_FindStringOfLen_stat);
            Assert(refStr->str == SL_ConvertToString(headEntry->prev_or_value),
                   s_assertDisable_SL_FindStringOfLen_match);
            return headEntry->prev_or_value;
        }
    }

    /* walk the collision chain */
    next = headEntry->status_next & HASH_NEXT_MASK;
    entry = &g_scrStringHash[next];
    prev = hash;

    while (entry != headEntry)
    {
        Assert((entry->status_next & HASH_STAT_MASK) == HASH_STAT_MOVABLE,
               s_assertDisable_SL_FindStringOfLen_movable);

        stringValue = entry->prev_or_value;
        Assert(stringValue, s_assertDisable_SL_FindStringOfLen_sv2);
        Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
               s_assertDisable_SL_FindStringOfLen_bounds2);

        refStr = (char *)g_scrStringMT + stringValue * MT_NODE_SIZE;

        if (refStr->len == (unsigned char)len)
        {
            if (memcmp(refStr->str, str, len) == 0)
            {
                /* found in chain — promote to head position via XOR swap */

                /* 1. prev.next = entry.next (skip entry in chain) */
                {
                    unsigned short xor1 = (g_scrStringHash[prev].status_next ^ entry->status_next) & HASH_NEXT_MASK;
                    g_scrStringHash[prev].status_next ^= xor1;
                }

                /* 2. entry.next = head.next (entry takes head's chain) */
                {
                    unsigned short xor2 = (headEntry->status_next ^ entry->status_next) & HASH_NEXT_MASK;
                    entry->status_next ^= xor2;
                }

                /* 3. head.next = entry index (head points to entry) */
                headEntry->status_next = (headEntry->status_next & HASH_STAT_MASK) | (unsigned short)next;

                /* 4. swap stringValues between head and entry */
                {
                    unsigned short tmpVal = headEntry->prev_or_value;
                    headEntry->prev_or_value = entry->prev_or_value;
                    entry->prev_or_value = tmpVal;
                }

                Assert(entry->status_next & HASH_STAT_MASK,
                       s_assertDisable_SL_FindStringOfLen_stat2);

                return headEntry->prev_or_value;
            }
        }

        prev = next;
        next = entry->status_next & HASH_NEXT_MASK;
        entry = &g_scrStringHash[next];
    }

    return 0;
}

/*
================
SL_ConvertToString

Returns the string data for a string handle.
Returns NULL if handle is 0.

================
*/
const char *SL_ConvertToString(unsigned int stringValue)
{
    if (!stringValue)
        return NULL;

    Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
           s_assertDisable_SL_ConvertToString);

    return SL_GetRefStr(stringValue)->str;
}

/*
================
SL_DebugConvertToString

Returns the string for a handle, or "<NULL>" / "<BINARY>" markers.
Checks if the string is properly null-terminated at expected length.

================
*/
const char *SL_DebugConvertToString(unsigned int stringValue)
{
    ScriptStringRef *refStr;
    const char *str;
    int lastIdx;

    if (!stringValue)
        return "<NULL>";

    Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
           s_assertDisable_SL_DebugConvertToString);

    refStr = SL_GetRefStr(stringValue);
    str = refStr->str;

    /* check if string is properly null-terminated */
    lastIdx = (unsigned char)(refStr->len - 1);
    if (str[lastIdx] == 0)
        return str;

    return "<BINARY>";
}

/*
================
SL_Shutdown

Iterates all hash table entries and releases every interned string.
For each non-FREE entry, reads the stringValue, computes the string
length, and calls SL_RemoveRefToStringOfLen to free it.

================
*/
void SL_Shutdown(void)
{
    unsigned int i;
    SL_HashEntry *entry;
    unsigned int stringValue;
    ScriptStringRef *refStr;
    unsigned int offset;
    int len;

    entry = &g_scrStringHash[1]; /* skip entry 0 (free list head) */

    for (i = 0; i < SL_HASH_SIZE - 1; i++, entry++)
    {
restart_entry:
        if (!(entry->status_next & HASH_STAT_MASK))
            continue; /* FREE entry, skip */

        g_scrStringShutdownFlag = 0;

        stringValue = entry->prev_or_value;
        Assert(stringValue, s_assertDisable_SL_Shutdown_sv);
        Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
               s_assertDisable_SL_Shutdown_bounds);

        refStr = SL_GetRefStr(stringValue);

        /* check user flag bit 2 (0x04) — protected strings are kept */
        if (refStr->user & 0x04) /* user flag bit 2 — protected string */
        {
            refStr->refCount = 1;    /* refCount = 1 */
            refStr->user = 4;      /* user = 0x04 */
        }
        else
        {
            /* clear refCount and user, then free */
            refStr->refCount = 0;
            refStr->user = 0;

            /* compute actual string length */
            offset = (unsigned char)(refStr->len - 1);
            while (refStr->str[offset] != 0)
                offset += 256;
            len = offset + 1;

            SL_FreeString(stringValue, refStr, len);
        }

        /* re-check entry: hash table may have shifted during free */
        if (g_scrStringShutdownFlag)
        {
            if (entry->status_next & HASH_STAT_MASK)
                goto restart_entry;
        }
    }

    /* phase 2: re-allocate protected strings in fresh memory tree */
    {
        extern void *MT_AllocBitmap(void);
        extern void MT_ReAllocFromBitmap(void *allocBits, unsigned int nodeNum, int numBytes); /* scr_memorytree_4217C0 */
        extern void MT_FreeBitmapUnused(void *allocBits);

        void *allocBits = MT_AllocBitmap();
        Assert(allocBits, s_assertDisable_SL_Shutdown_sv);

        /* iterate all hash entries, re-allocate protected strings */
        for (i = 1; i < 0x4000; i++)
        {
            SL_HashEntry *entry = &g_scrStringHash[i];
            if (!(entry->status_next & 0xC000))
                continue; /* skip FREE entries */

            {
                unsigned int sv = entry->prev_or_value;
                ScriptStringRef *ref = SL_GetRefStr(sv);
                if (ref->user & 4)
                {
                    int len = (int)strlen(ref->str);
                    MT_ReAllocFromBitmap(allocBits, sv, len + 5);
                }
            }
        }

        /* phase 3: free unused MT nodes and cleanup */
        MT_FreeBitmapUnused(allocBits);
    }
}

/*
================
SL_RemoveRefToString

Decrements the reference count for a string handle.
If refcount reaches 0, frees the string from the hash table.

Computes string length by scanning from the len hint byte,
then calls the internal removal function if freed.

================
*/
void SL_RemoveRefToString(unsigned int stringValue)
{
    ScriptStringRef *refStr;
    unsigned int offset;
    int len;
    unsigned short *refCount;

    Assert(stringValue, s_assertDisable_SL_RemoveRefToString);

    Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
           s_assertDisable_SL_RemoveRefToString_bounds);

    refStr = SL_GetRefStr(stringValue);

    /* compute actual string length by scanning from len hint */
    offset = (unsigned char)(refStr->len - 1);
    while (refStr->str[offset] != 0)
        offset += 256;
    len = offset + 1;

    Assert(stringValue, s_assertDisable_SL_RemoveRefToString_2);

    Assert((unsigned int)(stringValue * MT_NODE_SIZE) < MT_SIZE,
           s_assertDisable_SL_RemoveRefToString_bounds2);

    /* decrement refcount */
    refCount = &SL_GetRefStr(stringValue)->refCount;
    if (--(*refCount) == 0)
    {
        SL_FreeString(stringValue, refStr, len);
    }
}
