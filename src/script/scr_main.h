#pragma once


#include <universal/com_memory.h>
#include "scr_variable.h"

static const char *var_typename[] =
{
    "undefined",
    "object",
    "string",
    "localized string",
    "vector",
    "float",
    "int",
    "codepos",
    "precodepos",
    "function",
    "stack",
    "animation",
    "developer codepos",
    "include codepos",
    "thread",
    "thread",
    "thread",
    "thread",
    "struct",
    "removed entity",
    "entity",
    "array",
    "removed thread",
};

struct scrVarPub_t // sizeof=0x2007C
{
    char* fieldBuffer;
    uint16_t canonicalStrCount;
    bool developer;
    bool developer_script;
    bool evaluate;
    const char* error_message;
    int error_index;
    uint time;
    uint timeArrayId;
    uint pauseArrayId;
    uint levelId;
    uint gameId;
    uint animId;
    uint freeEntList;
    uint tempVariable;
    bool bInited;
    uint16_t savecount;
    uint checksum;
    uint entId;
    uint entFieldName;
    HunkUser* programHunkUser;
    const char* programBuffer;
    const char* endScriptBuffer;
    uint16_t saveIdMap[32768];
    uint16_t saveIdMapRev[32768];
    bool bScriptProfile;
    float scriptProfileMinTime;
    bool bScriptProfileBuiltin;
    float scriptProfileBuiltinMinTime;
    uint numScriptThreads;
    uint numScriptValues;
    uint numScriptObjects;
    const char* varUsagePos;
    int ext_threadcount;
    int totalObjectRefCount;
    volatile uint totalVectorRefCount;
};
static_assert(sizeof(scrVarPub_t) == (sizeof(void *) == 8 ? 131232 : 0x2007C));

struct PrecacheEntry // sizeof=0x8
{                                       // ...
    uint16_t filename;
    bool include;
    // padding byte
    uint sourcePos;
};
static_assert(sizeof(PrecacheEntry) == 0x8);

extern scrVarPub_t scrVarPub;
extern scrVarDebugPub_t scrVarDebugPubBuf;

bool Scr_IsInOpcodeMemory(char const* pos);
bool Scr_IsIdentifier(char const* token);

int Scr_GetFunctionHandle(char const*, char const*);
uint SL_TransferToCanonicalString(uint);
uint SL_GetCanonicalString(char const*);
void Scr_BeginLoadScriptsRemote(void);
void Scr_BeginLoadAnimTrees(int);
int Scr_ScanFile(byte*, int);
uint Scr_LoadScriptInternal(char const*, struct PrecacheEntry*, int);
uint Scr_LoadScript(char const*);
void Scr_PostCompileScripts(void);
void Scr_EndLoadScripts(void);
void Scr_PrecacheAnimTrees(void* (__cdecl*)(int), int);
void Scr_EndLoadAnimTrees(void);
void Scr_FreeScripts(byte);
void Scr_BeginLoadScripts(void);


//int marker_scr_main      83043248     scr_main.obj
//int Scr_IsInScriptMemory(char const*);

extern scrVarDebugPub_t *scrVarDebugPub;
