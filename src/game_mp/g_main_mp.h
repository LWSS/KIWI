#pragma once

#ifdef KISAK_SP
#error This file is for Multi-Player only
#endif

#include <qcommon/qcommon.h>
#include <universal/q_parse.h>

#include <bgame/bg_public.h>
#include <bgame/bg_local.h>

struct gentity_s;

struct entityHandler_t // sizeof=0x28
{
    void(__cdecl *think)(gentity_s *);
    void(__cdecl *reached)(gentity_s *);
    void(__cdecl *blocked)(gentity_s *, gentity_s *);
    void(__cdecl *touch)(gentity_s *, gentity_s *, int);
    void(__cdecl *use)(gentity_s *, gentity_s *, gentity_s *);
    void(__cdecl *pain)(gentity_s *, gentity_s *, int, const float *, const int, const float *, const hitLocation_t, const int);
    void(__cdecl *die)(gentity_s *, gentity_s *, gentity_s *, int, int, const int, const float *, const hitLocation_t, int);
    void(__cdecl *controller)(const gentity_s *, int *);
    int methodOfDeath;
    int splashMethodOfDeath;
};
static_assert(sizeof(entityHandler_t) == (sizeof(void *) == 8 ? 72 : 40));

struct trigger_info_t // sizeof=0xC
{                                       // ...
    uint16_t entnum;
    uint16_t otherEntnum;
    int useCount;
    int otherUseCount;
};
static_assert(sizeof(trigger_info_t) == 0xC);

struct cached_tag_mat_t // sizeof=0x3C
{                                       // ...
    int time;
    int entnum;
    uint16_t name;              // ...
    // padding byte
    // padding byte
    float tagMat[4][3];                 // ...
};
static_assert(sizeof(cached_tag_mat_t) == 0x3C);

struct level_locals_t // sizeof=0x2E6C
{                                       // ...
    struct gclient_s *clients;                 // ...
    gentity_s *gentities;               // ...
    int gentitySize;
    int num_entities;                   // ...
    gentity_s *firstFreeEnt;            // ...
    gentity_s *lastFreeEnt;             // ...
    int logFile;                        // ...
    int initializing;                   // ...
    int clientIsSpawning;               // ...
    objective_t objectives[16];         // ...
    int maxclients;                     // ...
    int framenum;                       // ...
    int time;                           // ...
    int previousTime;                   // ...
    int frametime;                      // ...
    int startTime;                      // ...
    int teamScores[TEAM_NUM_TEAMS];     // ...
    int lastTeammateHealthTime;         // ...
    int bUpdateScoresForIntermission;   // ...
    bool teamHasRadar[TEAM_NUM_TEAMS];  // ...
    int manualNameChange;               // ...
    int numConnectedClients;            // ...
    int sortedClients[64];              // ...
    char voteString[1024];              // ...
    char voteDisplayString[1024];       // ...
    int voteTime;                       // ...
    int voteExecuteTime;                // ...
    int voteYes;                        // ...
    int voteNo;                         // ...
    int numVotingClients;               // ...
    SpawnVar spawnVar;                  // ...
    int savepersist;                    // ...
    EntHandle droppedWeaponCue[32];
    float fFogOpaqueDist;               // ...
    float fFogOpaqueDistSqrd;           // ...
    int remapCount;
    int currentPlayerClone;             // ...
    trigger_info_t pendingTriggerList[256]; // ...
    trigger_info_t currentTriggerList[256]; // ...
    int pendingTriggerListSize;         // ...
    int currentTriggerListSize;         // ...
    int finished;                       // ...
    int bPlayerIgnoreRadiusDamage;      // ...
    int bPlayerIgnoreRadiusDamageLatched; // ...
    int registerWeapons;                // ...
    int bRegisterItems;                 // ...
    int currentEntityThink;             // ...
    int openScriptIOFileHandles[1];     // ...
    char *openScriptIOFileBuffers[1];   // ...
    com_parse_mark_t currentScriptIOLineMark[1]; // ...
    cached_tag_mat_t cachedTagMat;      // ...
    int scriptPrintChannel;             // ...
    float compassMapUpperLeft[2];       // ...
    float compassMapWorldSize[2];       // ...
    float compassNorth[2];              // ...
    scr_vehicle_s *vehicles;            // ...
};
static_assert(sizeof(level_locals_t) == (sizeof(void *) == 8 ? 12440 : 11884));

void __cdecl TRACK_g_main();
int __cdecl G_GetSavePersist();
void __cdecl G_SetSavePersist(int savepersist);
double __cdecl G_GetFogOpaqueDistSqrd();
int __cdecl G_GetClientScore(int clientNum);
int __cdecl G_GetClientArchiveTime(int clientNum);
void __cdecl G_SetClientArchiveTime(int clientNum, int time);
clientState_s *__cdecl G_GetClientState(int clientNum);
struct gclient_s *__cdecl G_GetPlayerState(int clientNum);
int __cdecl G_GetClientSize();
void __cdecl G_FreeEntities();
bool __cdecl G_ExitAfterConnectPaths();
int __cdecl G_IsServerGameSystem(int clientNum);
void __cdecl G_InitGame(int levelTime, int randomSeed, int restart, int savepersist);
const dvar_s *G_RegisterDvars();
void __cdecl G_CreateDObj(
    DObjModel_s *dobjModels,
    uint16_t numModels,
    XAnimTree_s *tree,
    uint handle,
    int unusedLocalClientNum);
DObj_s *__cdecl G_GetDObj(uint handle, int unusedLocalClientNum);
void G_LoadAnimTreeInstances();
void G_PrintAllFastFileErrors();
void __cdecl G_PrintFastFileErrors(const char *fastfile);
void __cdecl G_ShutdownGame(int freeScripts);
int G_FreeAnimTreeInstances();
void __cdecl SendScoreboardMessageToAllIntermissionClients();
void __cdecl CalculateRanks();
int __cdecl SortRanks(uint *a, uint *b);
void __cdecl ExitLevel();
void G_LogPrintf(const char *fmt, ...);
void __cdecl CheckVote();
void __cdecl G_UpdateObjectiveToClients();
void __cdecl G_UpdateHudElemsToClients();
void __cdecl G_RunThink(gentity_s *ent);
void __cdecl DebugDumpAnims();
void __cdecl G_XAnimUpdateEnt(gentity_s *ent);
void __cdecl G_RunFrame(int levelTime);
void __cdecl G_ClientDoPerFrameNotifies(gentity_s *ent);
bool __cdecl DoPerFrameNotify(
    gentity_s *ent,
    bool isCurrently,
    bool wasPreviously,
    uint16_t begin,
    uint16_t end);
void ShowEntityInfo();
void __cdecl ShowEntityInfo_Items(gentity_s *ent);
void __cdecl G_RunFrameForEntity(gentity_s *ent);
void __cdecl G_TraceCapsule(
    trace_t *results,
    const float *start,
    const float *mins,
    const float *maxs,
    const float *end,
    int passEntityNum,
    int contentmask);
int __cdecl G_TraceCapsuleComplete(
    float *start,
    float *mins,
    float *maxs,
    float *end,
    int passEntityNum,
    int contentmask);
void __cdecl G_LocationalTrace(
    trace_t *results,
    float *start,
    float *end,
    int passEntityNum,
    int contentmask,
    uint8_t *priorityMap);
void __cdecl G_LocationalTraceAllowChildren(
    trace_t *results,
    float *start,
    float *end,
    int passEntityNum,
    int contentmask,
    uint8_t *priorityMap);
int __cdecl G_LocationalTracePassed(
    float *start,
    float *end,
    int passEntityNum,
    int passEntityNum1,
    int contentmask,
    uint8_t *priorityMap);
void __cdecl G_SightTrace(int *hitNum, float *start, float *end, int passEntityNum, int contentmask);
void __cdecl G_AddDebugString(const float *xyz, const float *color, float scale, const char *text, int duration);
bool __cdecl OnSameTeam(struct gentity_s *ent1, struct gentity_s *ent2);

extern const dvar_t *pickupPrints;
extern const dvar_t *g_password;
extern const dvar_t *g_smoothClients;
extern const dvar_t *g_voteAbstainWeight;
extern const dvar_t *g_TeamColor_MyTeam;
extern const dvar_t *g_gravity;
extern const dvar_t *g_filterBan;
extern const dvar_t *g_voiceChatsAllowed;
extern const dvar_t *g_deadChat;
extern const dvar_t *radius_damage_debug;
extern const dvar_t *g_dumpAnims;
extern const dvar_t *g_ScoresColor_Allies;
extern const dvar_t *g_friendlyfireDist;
extern const dvar_t *g_dropUpSpeedRand;
extern const dvar_t *g_maxclients;
extern const dvar_t *player_MGUseRadius;
extern const dvar_t *g_TeamName_Allies;
extern const dvar_t *g_debugBullets;
extern const dvar_t *g_synchronousClients;
extern const dvar_t *g_knockback;
extern const dvar_t *player_throwbackInnerRadius;
extern const dvar_t *g_ScoresColor_MyTeam;
extern const dvar_t *g_allowVote;
extern const dvar_t *anim_deltas_debug;
extern const dvar_t *g_dedicated;
extern const dvar_t *g_TeamColor_Allies;
extern const dvar_t *g_antilag;
extern const dvar_t *g_TeamIcon_Allies;
extern const dvar_t *g_playerCollisionEjectSpeed;
extern const dvar_t *g_entinfo;
extern const dvar_t *melee_debug;
extern const dvar_t *g_useholdspawndelay;
extern const dvar_t *g_TeamColor_Free;
extern const dvar_t *g_debugPlayerAnimScript;
extern const dvar_t *g_fogColorReadOnly;
extern const dvar_t *g_dropUpSpeedBase;
extern const dvar_t *g_listEntity;
extern const dvar_t *g_inactivity;
extern const dvar_t *g_TeamIcon_Spectator;
extern const dvar_t *g_redCrosshairs;
extern const dvar_t *g_cheats;
extern const dvar_t *g_TeamColor_Spectator;
extern const dvar_t *g_fogHalfDistReadOnly;
extern const dvar_t *g_maxDroppedWeapons;
extern const dvar_t *g_dropForwardSpeed;
extern const dvar_t *g_ScoresColor_Free;
extern const dvar_t *g_minGrenadeDamageSpeed;
extern const dvar_t *g_dropHorzSpeedRand;
extern const dvar_t *g_voiceChatTalkingDuration;
extern const dvar_t *g_ScoresColor_Spectator;
extern const dvar_t *g_useholdtime;
extern const dvar_t *g_ScoresColor_EnemyTeam;
extern const dvar_t *g_compassShowEnemies;
extern const dvar_t *g_speed;
extern const dvar_t *g_friendlyNameDist;
extern const dvar_t *g_log;
extern const dvar_t *g_TeamName_Axis;
extern const dvar_t *g_TeamIcon_Axis;
extern const dvar_t *bullet_penetrationEnabled;
extern const dvar_t *g_TeamColor_Axis;
extern const dvar_t *g_NoScriptSpam;
extern const dvar_t *g_banIPs;
extern const dvar_t *g_gametype;
extern const dvar_t *g_fogStartDistReadOnly;
extern const dvar_t *g_debugLocDamage;
extern const dvar_t *g_logSync;
extern const dvar_t *g_mantleBlockTimeBuffer;
extern const dvar_t *player_throwbackOuterRadius;
extern const dvar_t *g_oldVoting;
extern const dvar_t *g_ScoresColor_Axis;
extern const dvar_t *g_TeamIcon_Free;
extern const dvar_t *g_motd;
extern const dvar_t *g_TeamColor_EnemyTeam;
extern const dvar_t *g_debugDamage;
extern const dvar_t *g_clonePlayerMaxVelocity;

extern const dvar_t *voice_global;
extern const dvar_t *voice_localEcho;
extern const dvar_t *voice_deadChat;

extern gentity_s g_entities[MAX_GENTITIES];
extern level_locals_t level;
extern bgs_t level_bgs;