#pragma once
// Only included by the raw converter in the standalone linker build.
int LinkerAnimReadFile(const char *path, void **data);
void LinkerAnimFreeFile(void *data);
unsigned int LinkerAnimString(const char *text, int user, int type);
unsigned int LinkerAnimStringSize(const char *text, int user, unsigned int size, int type);
HunkUser *LinkerAnimCreate(int size, const char *name, int fixed, int shared, int type);
void *LinkerAnimAllocate(HunkUser *user, int size, int alignment);
void LinkerAnimDestroy(HunkUser *user);
#define FS_ReadFile LinkerAnimReadFile
#define FS_FreeFile LinkerAnimFreeFile
#define SL_GetString_ LinkerAnimString
#define SL_GetStringOfSize LinkerAnimStringSize
#define Hunk_UserCreate LinkerAnimCreate
#define Hunk_UserAlloc LinkerAnimAllocate
#define Hunk_UserDestroy LinkerAnimDestroy
#define I_strnicmp _strnicmp
