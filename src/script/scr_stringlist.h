#pragma once
#include <cstdint>
#include "scr_memorytree.h"

#define HASH_STAT_FREE      0
#define HASH_STAT_MOVABLE   0x10000
#define HASH_STAT_HEAD      0x20000
#define HASH_STAT_MASK      0x30000

#define SL_MAX_STRING_INDEX 0x10000

#define STRINGLIST_SIZE 20'000

union HashEntry_unnamed_type_u
{           
    uint prev;
    uint str;
};

struct HashEntry
{               
    uint status_next;
    HashEntry_unnamed_type_u u;
};

struct __declspec(align(128)) scrStringGlob_t
{                                       
    HashEntry hashTable[20000];         
    bool inited;                        
    HashEntry *nextFreeEntry;
};

 struct RefString
 {
     union
     {
         struct
         {
             uint refCount : 16;
             uint user : 8;
             uint byteLen : 8; // includes null terminator
         };
         volatile uint data;
     };
     char str[1];
 };

 struct RefVector
 {
     union
     {
         struct
         {
             uint refCount : 16;
             uint user : 8;
             uint byteLen : 8;
         };
         volatile int head;
     };
     float vec[3];
 };

//#define MT_NODE_SIZE 12
#define MT_NODE_SIZE (sizeof(MemoryNode))
#define MT_SIZE 0xC0000

struct scrMemTreePub_t
{                     
    char *mt_buffer;  //     scrMemTreePub.mt_buffer = (char*)&scrMemTreeGlob.nodes;
};

struct scrStringDebugGlob_t
{
    volatile uint refCount[65536];
    volatile uint totalRefCount;
    int ignoreLeaks;
};

void SL_Init();
void SL_InitCheckLeaks();

void SL_Shutdown();
void SL_ShutdownSystem(uint user);

void SL_TransferSystem(uint from, uint to);

void SL_BeginLoadScripts();
void SL_EndLoadScripts();

void __cdecl SL_AddUser(uint stringValue, uint user);
void SL_AddUserInternal(RefString* refStr, uint user);

void SL_AddRefToString(uint stringValue);

uint SL_GetString_(const char* str, uint user, mtType_t type);
uint SL_GetStringOfSize(const char* str, uint user, uint len, mtType_t type);
const char* SL_ConvertToString(uint stringValue);
const char *SL_ConvertToStringSafe(uint stringValue);
RefString* GetRefString(uint stringValue);
RefString* GetRefString(const char* str);

void SL_CheckExists(uint stringValue);

uint SL_GetStringForVector(const float* v);
uint SL_GetStringForInt(int i);
uint SL_GetStringForFloat(float f);
uint SL_GetString(const char* str, uint user);
uint SL_GetLowercaseString_(const char* str, uint user, mtType_t type);
uint SL_GetLowercaseString(const char* str, uint user);

void __cdecl SL_TransferRefToUser(uint stringValue, uint user);

int SL_GetRefStringLen(RefString* refString);
int SL_GetStringLen(uint stringValue);

uint SL_FindLowercaseString(const char* str);

const char* SL_DebugConvertToString(uint stringValue);
uint SL_ConvertFromString(const char* str);

uint SL_FindString(const char* str);
void SL_RemoveRefToString(uint stringValue);
void SL_RemoveRefToStringOfSize(uint stringValue, uint len);

int SL_IsLowercaseString(uint stringValue);

void __cdecl Scr_SetString(uint16_t *to, uint from);

uint __cdecl SL_ConvertToLowercase(uint stringValue, uint user, mtType_t type);

uint __cdecl Scr_CreateCanonicalFilename(const char *filename);

void Scr_SetStringFromCharString(uint16_t *to, const char *from);
uint SL_GetUser(uint stringValue);

uint __cdecl Scr_AllocString(char *s, int sys);