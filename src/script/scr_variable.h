#pragma once
#include "scr_stringlist.h"

#undef GetObject

// LWSS: Custom enum typename
enum Vartype_t : int
{
    VAR_UNDEFINED = 0x0,
    VAR_BEGIN_REF = 0x1,
    VAR_POINTER = 0x1,
    VAR_STRING = 0x2,
    VAR_ISTRING = 0x3,
    VAR_VECTOR = 0x4,
    VAR_END_REF = 0x5,
    VAR_FLOAT = 0x5,
    VAR_INTEGER = 0x6,
    VAR_CODEPOS = 0x7,
    VAR_PRECODEPOS = 0x8,
    VAR_FUNCTION = 0x9,
    VAR_STACK = 0xA,
    VAR_ANIMATION = 0xB,
    VAR_DEVELOPER_CODEPOS = 0xC,
    VAR_INCLUDE_CODEPOS = 0xD,
    VAR_THREAD = 0xE,
    VAR_NOTIFY_THREAD = 0xF,
    VAR_TIME_THREAD = 0x10,
    VAR_CHILD_THREAD = 0x11,
    VAR_OBJECT = 0x12,
    VAR_DEAD_ENTITY = 0x13,
    VAR_ENTITY = 0x14,
    VAR_ARRAY = 0x15,
    VAR_DEAD_THREAD = 0x16,
    VAR_COUNT = 0x17,
    VAR_THREAD_LIST = 0x18,
    VAR_ENDON_LIST = 0x19,
};

enum var_stat_t
{
	VAR_STAT_FREE = 0x0,
	VAR_STAT_MOVABLE = 0x20,
	VAR_STAT_HEAD = 0x40,
	VAR_STAT_EXTERNAL = 0x60,
	VAR_STAT_MASK = 0x60,
};

#define VAR_STAT_FREE 0
#define VAR_STAT_MOVABLE 0x20
#define VAR_STAT_MASK 0x60
#define VAR_STAT_EXTERNAL 0x60

#define VAR_MASK 0x1F

#define FIRST_DEAD_OBJECT 0x16

enum scr_classnum_t
{
    CLASS_NUM_ENTITY      = 0x0,
    CLASS_NUM_HUDELEM     = 0x1,
    CLASS_NUM_PATHNODE    = 0x2,
    CLASS_NUM_VEHICLENODE = 0x3,
    CLASS_NUM_COUNT       = 0x4,
};

#define VAR_NAME_BITS 8
#define VAR_NAME_LOW_MASK 0x00FFFFFF

#define MAX_ARRAYINDEX 0x800000

#define VARIABLELIST_CHILD_SIZE 0xFFFE
#define VARIABLELIST_CHILD_BEGIN 0x8002 // 32770 // XBOX(0x6000) // 
#define VARIABLELIST_PARENT_BEGIN 1
#define VARIABLELIST_PARENT_SIZE 0x8000

#define VAR_NAME_HIGH_MASK 0xFFFFFF00

#define OBJECT_NOTIFY_LIST 0x18000
#define OBJECT_STACK 0x18001

#define FIRST_OBJECT 15
#define FIRST_CLEARABLE_OBJECT 0x12
#define FIRST_NONFIELD_OBJECT 0x15
#define FIRST_DEAD_OBJECT 0x16

//#define VAR_NAME_LOW_MASK 0xFF000000

struct VariableStackBuffer // sizeof=0xC
{
    const char *pos;
    uint16_t size;
    uint16_t bufLen;
    uint16_t localId;
    uint8_t time;
    char buf[1];
};
static_assert(sizeof(VariableStackBuffer) == 0xC);

union VariableUnion // sizeof=0x4
{                                       // ...
    VariableUnion(float f)
    {
        floatValue = f;
    }
    VariableUnion(int i)
    {
        intValue = i;
    }
    VariableUnion(char *str)
    {
        codePosValue = str;
    }
    VariableUnion(const char *str)
    {
        codePosValue = str;
    }
    VariableUnion()
    {
        intValue = 0;
    }

    int intValue;
    float floatValue;
    uint stringValue;
    const float *vectorValue;
    const char *codePosValue;
    uint pointerValue;
    VariableStackBuffer *stackValue;
    uint entityOffset;
};
static_assert(sizeof(VariableUnion) == 0x4);

struct VariableValue // sizeof=0x8
{   
    // ...
    VariableUnion u;                    // ...
    Vartype_t type;                           // ...
};
static_assert(sizeof(VariableValue) == 0x8);

union ObjectInfo_u // sizeof=0x2
{                                       // ...
    uint16_t size;
    uint16_t entnum;
    uint16_t nextEntId;
    uint16_t self;
};
static_assert(sizeof(ObjectInfo_u) == 0x2);

struct ObjectInfo // sizeof=0x4
{                                       // ...
    uint16_t refCount;
    ObjectInfo_u u;
};
static_assert(sizeof(ObjectInfo) == 0x4);

union Variable_u // sizeof=0x2
{                                       // ...
    uint16_t prev;
    uint16_t prevSibling;
};
static_assert(sizeof(Variable_u) == 0x2);

struct Variable // sizeof=0x4
{                                       // ...
    uint16_t id;                // ...
    Variable_u u;                       // ...
};
static_assert(sizeof(Variable) == 0x4);

union VariableValueInternal_u // sizeof=0x4
{                                       // ...
    operator int()
    {
        return u.intValue;
    }
    VariableValueInternal_u(int i)
    {
        u.intValue = i;
    }
    VariableValueInternal_u()
    {
        u.intValue = 0;
    }

    uint16_t next;
    VariableUnion u;
    ObjectInfo o;
};
static_assert(sizeof(VariableValueInternal_u) == 0x4);

union VariableValueInternal_w // sizeof=0x4
{                                       // ...
    uint status;
    uint type;
    uint name;
    uint classnum;
    uint notifyName;
    uint waitTime;
    uint parentLocalId;
};
static_assert(sizeof(VariableValueInternal_w) == 0x4);

union VariableValueInternal_v // sizeof=0x2
{                                       // ...
    uint16_t next;
    uint16_t index;
};
static_assert(sizeof(VariableValueInternal_v) == 0x2);

struct VariableValueInternal // sizeof=0x10
{                                       // ...
    Variable hash;                      // ...
    VariableValueInternal_u u;          // ...
    VariableValueInternal_w w;          // ...
    VariableValueInternal_v v;          // ...
    uint16_t nextSibling;       // ...
};
static_assert(sizeof(VariableValueInternal) == 0x10);

struct scrVarDebugPub_t // sizeof=0xE0004
{                                       // ...
    const char* varUsage[0x18000];
    uint16_t extRefCount[0x8000];
    uint16_t refCount[0x8000];
    int leakCount[0x18000];
    bool dummy;
    // padding byte
    // padding byte
    // padding byte
};
static_assert(sizeof(scrVarDebugPub_t) == 0xE0004);

struct scrVarGlob_t // sizeof=0x180000
{                                       // ...
    VariableValueInternal variableList[0x18000]; // ...
};
static_assert(sizeof(scrVarGlob_t) == 0x180000);

struct scr_entref_t // sizeof=0x4
{                                       // ...
    scr_entref_t()
    {
        entnum = 0;
        classnum = CLASS_NUM_ENTITY;
    }
    scr_entref_t(int i)
    {
        entnum = i;
        classnum = i;
    }
    uint16_t entnum;            // ...
    uint16_t classnum;          // ...
};
static_assert(sizeof(scr_entref_t) == 0x4);

struct scr_classStruct_t // sizeof=0xC
{
    scr_classStruct_t(uint16_t _id, uint16_t _entArrayId, char _charID, const char* _name)
    {
        id = _id;
        entArrayId = _entArrayId;
        charId = _charID;
        name = _name;
    }
    uint16_t id;
    uint16_t entArrayId;
    char charId;
    // padding byte
    // padding byte
    // padding byte
    const char *name;
};
static_assert(sizeof(scr_classStruct_t) == 0xC);

struct VariableDebugInfo // sizeof=0x10
{
    const char *pos;
    const char *fileName;
    const char *functionName;
    int varUsage;
};
static_assert(sizeof(VariableDebugInfo) == 0x10);

//void  TRACK_scr_variable(void);
void __cdecl Scr_Cleanup();
bool  IsObject(VariableValueInternal* entryValue);
bool  IsObject(VariableValue* value);
void  Scr_InitVariables(void);
void  Scr_InitVariableRange(uint begin, uint end);
void  Scr_InitClassMap(void);
uint  Scr_GetNumScriptVars(void);
uint  GetVariableKeyObject(uint id);
uint  Scr_GetVarId(uint index);
void  Scr_SetThreadNotifyName(uint startLocalId, uint stringValue);
ushort  Scr_GetThreadNotifyName(uint startLocalId);
void  Scr_SetThreadWaitTime(uint startLocalId, uint waitTime);
void  Scr_ClearWaitTime(uint startLocalId);
uint  Scr_GetThreadWaitTime(uint startLocalId);
uint  GetParentLocalId(uint threadId);
uint  GetSafeParentLocalId(uint threadId);
uint  GetStartLocalId(uint);
uint  AllocValue(void);
uint  AllocObject(void);
uint  Scr_AllocArray(void);
uint  AllocThread(uint self);
uint  AllocChildThread(uint self, uint parentLocalId);
uint  Scr_GetSelf(uint threadId);
void  AddRefToObject(uint id);
void  RemoveRefToEmptyObject(uint id);
int  Scr_GetRefCountToObject(uint id);
float const*  Scr_AllocVector(float const* v);
void  AddRefToVector(float const* vectorValue);
void  RemoveRefToVector(float const* vectorValue);
void  AddRefToValue(int type, VariableUnion u);
void  RemoveRefToValue(int type, VariableUnion u);
inline void RemoveRefToValue(VariableValue *value)
{
    RemoveRefToValue(value->type, value->u);
}
bool  IsValidArrayIndex(uint unsignedValue);
uint  GetInternalVariableIndex(uint unsignedValue);
uint  FindArrayVariable(uint parentId, int intValue);
uint  FindVariable(uint parentId, uint unsignedValue);
uint  FindObjectVariable(uint parentId, uint id);
struct VariableValue  Scr_GetArrayIndexValue(uint name);
void  SetVariableValue(uint id, struct VariableValue* value);
void  SetNewVariableValue(uint id, struct VariableValue* value);
VariableValueInternal_u*  GetVariableValueAddress(uint id);
void  ClearVariableValue(uint id);
uint Scr_EvalVariableObject(uint id);
uint  GetArraySize(uint id);
uint  FindFirstSibling(uint id);
uint  FindNextSibling(uint id);
uint  FindLastSibling(uint parentId);
uint  FindPrevSibling(uint index);
uint  GetVariableName(uint id);
uint GetObject(uint id);
uint GetArray(uint id);
uint FindObject(uint id);
bool  IsFieldObject(uint id);
int  Scr_IsThreadAlive(uint);
bool  IsObjectFree(uint id);
Vartype_t GetValueType(uint id);
uint GetObjectType(uint id);
void  Scr_SetClassMap(uint classnum);
int Scr_GetOffset(uint classnum, const char* name);
uint FindEntityId(uint entnum, uint classnum);
void  SetEmptyArray(uint parentId);
void  Scr_AddArrayKeys(uint parentId);
scr_entref_t Scr_GetEntityIdRef(uint entId);
uint  Scr_FindField(char const* name, int* type);
void  Scr_AddFields(char const* path, char const* extension);
void  Scr_AllocGameVariable(void);
//void  Scr_GetChecksum(int* const);
int  Scr_GetClassnumForCharId(char charId);
//uint  Scr_InitStringSet(void);
uint  Scr_FindAllThreads(uint selfId, uint* threads, uint localId);
uint  Scr_FindAllEndons(uint threadId, uint* names);
//bool  CheckReferences(void);

void  Scr_DumpScriptVariables(bool spreadsheet,
    bool summary,
    bool total,
    bool functionSummary,
    bool lineSort,
    const char* fileName,
    const char* functionName,
    int minCount);

inline void Scr_DumpScriptVariablesDefault(void)
{
    Scr_DumpScriptVariables(0, 0, 0, 0, 0, 0, 0, 0);
}
uint  GetVariableIndexInternal(uint parentId, uint name);
void  ClearObject(uint parentId);
void  Scr_RemoveThreadNotifyName(uint startLocalId);
//void  Scr_RemoveThreadEmptyNotifyName(uint startLocalId);
void  FreeValue(uint id);
uint  GetArrayVariableIndex(uint parentId, uint unsignedValue);
uint  Scr_GetVariableFieldIndex(uint parentId, uint name);
uint  Scr_FindAllVariableField(uint parentId, uint* names);
uint  GetArrayVariable(uint parentId, uint unsignedValue);
uint  GetNewArrayVariable(uint parentId, uint unsignedValue);
uint  GetVariable(uint parentId, uint unsignedValue);
uint  GetNewVariable(uint parentId, uint unsignedValue);
uint  GetObjectVariable(uint parentId, uint id);
uint  GetNewObjectVariable(uint parentId, uint id);
uint  GetNewObjectVariableReverse(uint parentId, uint id);
void  RemoveVariable(uint parentId, uint unsignedValue);
void  RemoveNextVariable(uint parentId);
void  RemoveObjectVariable(uint parentId, uint id);
void  SafeRemoveVariable(uint parentId, uint unsignedValue);
void  RemoveVariableValue(uint parentId, uint index);
void  SetVariableEntityFieldValue(uint entId, uint fieldName, VariableValue* value);
void  SetVariableFieldValue(uint id, VariableValue* value);
VariableValue  Scr_EvalVariable(uint id);
void  Scr_EvalBoolComplement(VariableValue* value);
void  Scr_CastBool(VariableValue* value);
bool  Scr_CastString(VariableValue* value);
void  Scr_CastDebugString(VariableValue* value);
char  Scr_GetEntClassId(uint id);
int  Scr_GetEntNum(uint id);
void  Scr_ClearVector(VariableValue* value);
void  Scr_CastVector(VariableValue* value);
uint Scr_EvalFieldObject(uint tempVariable, VariableValue* value);
void  Scr_UnmatchingTypesError(VariableValue* value1, VariableValue* value2);

void  Scr_EvalOr(VariableValue* value1, VariableValue* value2);
void  Scr_EvalExOr(VariableValue* value1, VariableValue* value2);
void  Scr_EvalAnd(VariableValue* value1, VariableValue* value2);
void  Scr_EvalLess(VariableValue* value1, VariableValue* value2);
void  Scr_EvalGreaterEqual(VariableValue* value1, VariableValue* value2);
void  Scr_EvalGreater(VariableValue* value1, VariableValue* value2);
void  Scr_EvalLessEqual(VariableValue* value1, VariableValue* value2);
void  Scr_EvalShiftLeft(VariableValue* value1, VariableValue* value2);
void  Scr_EvalShiftRight(VariableValue* value1, VariableValue* value2);
void  Scr_EvalPlus(VariableValue* value1, VariableValue* value2);
void  Scr_EvalMinus(VariableValue* value1, VariableValue* value2);
void  Scr_EvalMultiply(VariableValue* value1, VariableValue* value2);
void  Scr_EvalDivide(VariableValue* value1, VariableValue* value2);
void  Scr_EvalMod(VariableValue* value1, VariableValue* value2);

void  Scr_FreeEntityNum(uint entnum, uint classnum);
void  Scr_FreeObjects(void);
void  Scr_AddClassField(uint classnum, char* name, uint offset);
uint  Scr_GetEntityId(uint entnum, uint classnum);
//void  Scr_CopyEntityNum(int, int, uint);
void  Scr_FreeGameVariable(int bComplete);
//bool  Scr_AddStringSet(uint, char const*);

struct ThreadDebugInfo // sizeof=0x8C
{                                       // ...
    const char *pos[32];                // ...
    int posSize;                        // ...
    float varUsage;                     // ...
    float endonUsage;                   // ...
};
static_assert(sizeof(ThreadDebugInfo) == 0x8C);

void  Scr_DumpScriptThreads(void);
void  Scr_ShutdownVariables(void);
void  RemoveRefToObject(uint id);
void  ClearVariableField(uint parentId, uint name, VariableValue* value);
VariableValue  Scr_EvalVariableField(uint id);
void  Scr_EvalSizeValue(VariableValue* value);
void  Scr_EvalBoolNot(VariableValue* value);
void  Scr_EvalEquality(VariableValue* value1, VariableValue* value2);
void  Scr_EvalInequality(VariableValue* value1, VariableValue* value2);
void  Scr_EvalBinaryOperator(int op, VariableValue* value1, VariableValue* value2);
void  Scr_FreeEntityList(void);
void  Scr_RemoveClassMap(uint classnum);
void  Scr_EvalArray(VariableValue* value, VariableValue* index);
uint Scr_EvalArrayRef(uint parentId);
void  ClearArray(uint parentId, VariableValue* value);
void  Scr_FreeValue(uint id);
//void  Scr_ShutdownStringSet(uint);
void  Scr_StopThread(uint threadId);
void  Scr_KillEndonThread(uint threadId);
VariableValue Scr_FindVariableField(uint parentId, uint name);
void  Scr_KillThread(uint parentId);
void  Scr_CheckLeakRange(uint begin, uint end);
void  Scr_CheckLeaks(void);

int  ThreadInfoCompare(_DWORD* info1, _DWORD* info2);
//int  VariableInfoCompare(void const*, void const*);
int VariableInfoFileNameCompare(_DWORD* info1, _DWORD* info2);
int VariableInfoCountCompare(_DWORD* info1, _DWORD* info2);
int VariableInfoFileLineCompare(_DWORD* info1, _DWORD* info2);
uint  FindVariableIndexInternal2(uint name, uint index);
uint FindVariableIndexInternal(uint parentId, uint name);
ushort  AllocVariable(void);
void  FreeVariable(uint id);
uint  AllocEntity(uint classnum, ushort entnum);
float*  Scr_AllocVector(void);
uint  FindArrayVariableIndex(uint parentId, uint unsignedValue);
uint  Scr_FindArrayIndex(uint parentId, VariableValue* index);
float  Scr_GetEntryUsage(uint type, VariableUnion u);
float  Scr_GetEntryUsage(VariableValueInternal* entryValue);
float  Scr_GetObjectUsage(uint parentId);
char*  Scr_GetSourceFile_FastFile(char const* filename);
char*  Scr_GetSourceFile(char const* filename);
void  Scr_AddFieldsForFile(char const* filename);
void  Scr_AddFields_FastFile(char const* path, char const* extension);
//void  CheckReferenceRange(uint, uint);
uint  GetNewVariableIndexInternal3(uint parentId, uint name, uint index);
uint  GetNewVariableIndexInternal2(uint parentId, uint name, uint index);
uint  GetNewVariableIndexReverseInternal2(uint parentId, uint name, uint index);
uint  GetNewVariableIndexInternal(uint parentId, uint name);
uint  GetNewVariableIndexReverseInternal(uint parentId, uint name);
void  MakeVariableExternal(uint index, VariableValueInternal* parentValue);
void  FreeChildValue(uint parentId, uint id);
void  ClearObjectInternal(uint parentId);
uint  GetNewArrayVariableIndex(uint parentId, uint unsignedValue);
void  RemoveArrayVariable(uint parentId, uint unsignedValue);
void  CopyArray(uint parentId, uint newParentId);
void  Scr_CastWeakerPair(VariableValue* value1, VariableValue* value2);
void  Scr_CastWeakerStringPair(VariableValue* value1, VariableValue* value2);
//void  CopyEntity(uint, uint);
float  Scr_GetEndonUsage(uint parentId);
float  Scr_GetThreadUsage(const VariableStackBuffer* stackBuf, float* endonUsage);
int  Scr_MakeValuePrimitive(uint parentId);
void  SafeRemoveArrayVariable(uint parentId, uint unsignedValue);
VariableValue  Scr_EvalVariableEntityField(uint entId, uint fieldName);
void  Scr_ClearThread(uint parentId);
void Scr_GetChecksum(uint *checksum);

void Scr_CopyEntityNum(int fromEntnum, int toEntnum, uint classnum);
void CopyEntity(uint parentId, uint newParentId);

uint Scr_InitStringSet();
void Scr_ShutdownStringSet(uint setId);
int Scr_AddStringSet(uint setId, const char *string);

extern scr_classStruct_t g_classMap[CLASS_NUM_COUNT];
extern scrStringDebugGlob_t *scrStringDebugGlob;
extern scrMemTreePub_t scrMemTreePub;
extern scrVarGlob_t scrVarGlob;;