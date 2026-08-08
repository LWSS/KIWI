#pragma once
#include <xanim/xanim.h>
#include <bgame/bg_local.h>

#define MAX_XANIMTREE_NUM       0x80 // 128

struct scrAnimPub_t // sizeof=0x41C
{                                       // ...
    uint animtrees;             // ...
    uint animtree_node;         // ...
    uint animTreeNames;         // ...
    scr_animtree_t xanim_lookup[2][MAX_XANIMTREE_NUM]; // ...
    uint xanim_num[2];          // ...
    uint animTreeIndex;         // ...
    bool animtree_loading;              // ...
    // padding byte
    // padding byte
    // padding byte
};
static_assert(sizeof(scrAnimPub_t) == 0x41C);

struct scrAnimGlob_t // sizeof=0x20C
{                                       // ...
    const char *start;                  // ...
    const char *pos;                    // ...
    uint16_t using_xanim_lookup[2][MAX_XANIMTREE_NUM]; // ...
    int bAnimCheck;                     // ...
};
static_assert(sizeof(scrAnimGlob_t) == 0x20C);

void __cdecl TRACK_scr_animtree();
void __cdecl SetAnimCheck(int bAnimCheck);
void __cdecl Scr_EmitAnimation(char *pos, uint animName, uint sourcePos);
void __cdecl Scr_EmitAnimationInternal(char *pos, uint animName, uint names);
int __cdecl Scr_GetAnimsIndex(const XAnim_s *anims);
XAnim_s *__cdecl Scr_GetAnims(uint index);
void __cdecl Scr_UsingTree(const char *filename, uint sourcePos);
uint __cdecl Scr_UsingTreeInternal(const char *filename, uint *index, int user);
void __cdecl Scr_LoadAnimTreeAtIndex(uint index, void *(__cdecl *Alloc)(int), int user);
int __cdecl Scr_GetAnimTreeSize(uint parentNode);
void __cdecl ConnectScriptToAnim(
    uint names,
    uint16_t index,
    uint filename,
    uint name,
    uint16_t treeIndex);
int __cdecl Scr_CreateAnimationTree(
    uint parentNode,
    uint names,
    XAnim_s *anims,
    uint childIndex,
    const char *parentName,
    uint parentIndex,
    uint filename,
    int treeIndex,
    uint16_t flags);
void __cdecl Scr_CheckAnimsDefined(uint names, uint filename);
bool __cdecl Scr_LoadAnimTreeInternal(const char *filename, uint parentNode, uint names);
void __cdecl Scr_AnimTreeParse(const char *pos, uint parentNode, uint names);
void __cdecl AnimTreeCompileError(const char *msg);
bool __cdecl AnimTreeParseInternal(
    uint parentNode,
    uint names,
    bool bIncludeParent,
    bool bLoop,
    bool bComplete);
int __cdecl GetAnimTreeParseProperties();
scr_animtree_t __cdecl Scr_FindAnimTree(const char *filename);
void __cdecl Scr_FindAnim(const char *filename, const char *animName, scr_anim_s *anim, int user);

extern scrAnimPub_t scrAnimPub;