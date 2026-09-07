/*
 * com_memory.c — Memory allocation (Z_Malloc, Hunk, etc).
 */

#include "cod2rad64.h"

static void *g_hunkDataHash[1024];
static char *s_hunkData;          /* base pointer of hunk memory */
static int s_hunkTotal;           /* total hunk size in bytes */
static int hunk_low_temp;         /* low-side temp watermark */
static int hunk_high_permanent;   /* high-side permanent watermark */

#define HUNK_TEMP_MAGIC 0x89537892

static char s_assertDisable_Z_Malloc;
static char s_assertDisable_Z_VirtualAlloc;
static char s_assertDisable_Z_StrDup;
static char s_assertDisable_Hunk_FreeTempMemory;
static char s_assertDisable_Hunk_FreeTempMemory_pos;
static char s_assertDisable_Hunk_AddDataForFile;
static char s_assertDisable_Hunk_AddDataForFile_dup;
static char s_assertDisable_Hunk_AddDataForFile_type;
static char s_assertDisable_Hunk_AllocateTempMemory;
static char s_assertDisable_Hunk_AllocateTempMemory_align;

/*
================
Z_FreeInternal

Frees memory via CRT free. Just a thunk.
================
*/
void Z_FreeInternal(void *ptr)
{
    free(ptr);
}

/*
================
Z_VirtualFree

Releases virtual memory via VirtualFree.
================
*/
void Z_VirtualFree(void *ptr)
{
    VirtualFree(ptr, 0, MEM_RELEASE);
}

/*
================
Z_Malloc

Allocates zeroed memory via malloc. Asserts on failure.
================
*/
void *Z_Malloc(size_t size)
{
    void *buf;

    buf = malloc((size_t)size);
    if (!buf)
    {
        Assert(0, s_assertDisable_Z_Malloc);
        Com_Printf("Z_Malloc: failed to allocate %zu bytes\n", size);
        Com_Error(0, "Z_Malloc: EXE_ERR_OUT_OF_MEMORY");
        return NULL;
    }

    memset(buf, 0, size);
    return buf;
}

/*
================
Z_VirtualAlloc

Allocates memory via VirtualAlloc. Asserts on failure.
================
*/
void *Z_VirtualAlloc(int size)
{
    void *buf;

    buf = VirtualAlloc(NULL, (size_t)size, MEM_COMMIT, PAGE_READWRITE);
    if (!buf)
    {
        Assert(0, s_assertDisable_Z_VirtualAlloc);
        Com_Printf("Z_VirtualAlloc: failed to allocate %d bytes\n", size);
        Com_Error(0, "Z_VirtualAlloc: EXE_ERR_OUT_OF_MEMORY");
    }

    return buf;
}

/*
================
Z_StrDup

Duplicates a string via malloc + memcpy. Used by dvar name interning.
================
*/
char *Z_StrDup(const char *string)
{
    int len;
    char *copy;
    const char *src;
    char *dst;

    len = (int)strlen(string) + 1;
    copy = (char *)malloc(len);
    if (!copy)
    {
        Assert(0, s_assertDisable_Z_StrDup);
        Com_Printf("Z_Malloc: failed to allocate %d bytes\n", len);
        Com_Error(0, "Z_Malloc: EXE_ERR_OUT_OF_MEMORY");
    }

    memset(copy, 0, len);

    /* inline byte copy */
    src = string;
    dst = copy;
    while (*src)
        *dst++ = *src++;
    *dst = 0;

    return copy;
}

/*
================
Hunk_FreeTempMemory

Frees temp memory from the hunk. If hunk not initialized, falls
back to free(). Otherwise validates the magic header and adjusts
the hunk_low_temp watermark.
================
*/
void Hunk_FreeTempMemory(void *buf)
{
    int *hdr;

    Assert(Sys_IsMainThread(), s_assertDisable_Hunk_FreeTempMemory);

    if (!s_hunkData)
    {
        free(buf);
        return;
    }

    hdr = (int *)((char *)buf - 16);

    if (hdr[0] != HUNK_TEMP_MAGIC)
        Com_Error(0, "Hunk_FreeTempMemory: bad magic");

    hdr[0] = 0x89537893; /* invalidate magic */

    Assert(hdr == (void *)(s_hunkData + ((hunk_low_temp - hdr[1] + 15) & ~15)),
           s_assertDisable_Hunk_FreeTempMemory_pos);

    hunk_low_temp -= hdr[1];
}

/*
================
memmove_thunk

Sign-extends count and calls memmove.
================
*/
void *memmove_thunk(void *dest, const void *src, int count)
{
    return memmove(dest, src, (size_t)count);
}

/*
================
memset_thunk

Sign-extends count and calls memset.
================
*/
void *memset_thunk(void *dest, int val, int count)
{
    return memset(dest, val, (size_t)count);
}

/*
================
Hunk_FindDataForFile

Looks up cached hunk data by type and filename.
Uses hash table with linked list chains.
================
*/
void *Hunk_FindDataForFile(int type, const char *name)
{
    int hash;
    HunkDataNode_t *entry;

    hash = FS_HashFileName(name, 1024);
    entry = (HunkDataNode_t *)g_hunkDataHash[hash];

    while (entry)
    {
        if (entry->type == (unsigned char)type
            && !I_stricmp(entry->name, name))
        {
            return entry->data;
        }
        entry = entry->next;
    }

    return NULL;
}

/*
================
Hunk_AddDataForFile

Associates data with a filename in the hunk hash table.
Allocates a node via the provided allocator callback.
================
*/
void *Hunk_AddDataForFile(int type, const char *name, void *data, void *(*allocator)(int))
{
    int hash;
    HunkDataNode_t *entry;
    HunkDataNode_t *node;
    char *dst;

    Assert(Sys_IsMainThread(), s_assertDisable_Hunk_AddDataForFile);

    hash = FS_HashFileName(name, 1024);

    /* assert no duplicate */
    entry = (HunkDataNode_t *)g_hunkDataHash[hash];
    while (entry)
    {
        if (entry->type == (unsigned char)type
            && !I_stricmp(entry->name, name))
        {
            Assert(!entry->data, s_assertDisable_Hunk_AddDataForFile_dup);
            break;
        }
        entry = entry->next;
    }

    /* allocate node: 8(data) + 8(next) + 1(type) + strlen + 1(null) = 18 + strlen */
    node = (HunkDataNode_t *)allocator((int)(offsetof(HunkDataNode_t, name) + strlen(name) + 1));
    if (!node)
        return NULL;
    node->data = data;
    node->type = (unsigned char)type;
    Assert(type == node->type, s_assertDisable_Hunk_AddDataForFile_type);

    /* copy filename into node->name */
    dst = node->name;
    while (*name)
        *dst++ = *name++;
    *dst = 0;

    /* insert at head of hash chain */
    node->next = (HunkDataNode_t *)g_hunkDataHash[hash];
    g_hunkDataHash[hash] = node;

    return node->name; /* returns start of the copied name string */
}

/*
================
Hunk_AllocateTempMemory

Allocates temp memory from the hunk low end with 16-byte alignment.
Falls back to Z_Malloc if hunk not initialized.
================
*/
void *Hunk_AllocateTempMemory(int size)
{
    int alignedPos;
    int totalSize;
    int *header;
    int oldTemp;

    Assert(Sys_IsMainThread(), s_assertDisable_Hunk_AllocateTempMemory);

    if (s_hunkData)
    {
        totalSize = size + 16;
        oldTemp = hunk_low_temp;
        alignedPos = (hunk_low_temp + 15) & ~15;
        hunk_low_temp = alignedPos + totalSize;

        if (hunk_high_permanent + hunk_low_temp > s_hunkTotal)
        {
            Com_Error(1,
                "Hunk_AllocateTempMemory: failed on %i bytes (total %i MB, low %i MB, high %i MB), needs %i more hunk bytes",
                totalSize,
                s_hunkTotal / 0x100000,
                hunk_low_temp / 0x100000,
                hunk_high_permanent / 0x100000,
                hunk_low_temp + hunk_high_permanent - s_hunkTotal);
        }

        header = (int *)(s_hunkData + alignedPos);
        Assert(!((intptr_t)(header + 4) & 15), s_assertDisable_Hunk_AllocateTempMemory_align);

        header[0] = HUNK_TEMP_MAGIC;
        header[1] = hunk_low_temp - oldTemp;
        return header + 4;
    }
    else
    {
        return Z_Malloc(size);
    }
}
