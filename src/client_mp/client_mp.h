#pragma once
#include <qcommon/qcommon.h>

#include <qcommon/ent.h>
#include <qcommon/net_chan_mp.h>

#include <bgame/bg_local.h>

#include <gfx_d3d/r_debug.h>

#include <xanim/xanim.h>

#include <ui_mp/ui_mp.h>

struct GfxConfiguration;

#define MAX_PARSE_ENTITIES 2048
#define MAX_PARSE_CLIENTS 2048
static_assert(((MAX_PARSE_ENTITIES) & (MAX_PARSE_ENTITIES - 1)) == 0, "MAX_PARSE_ENTITIES must be power of 2");
static_assert(((MAX_PARSE_CLIENTS) & (MAX_PARSE_CLIENTS - 1)) == 0, "MAX_PARSE_CLIENTS must be power of 2");

//enum $C2D64A5C68CD67A3D33FF78F5B5E7685 : int
enum ConstStringOffsets // not a real name
{
    CS_GAME_VERSION           = 2,
    CS_MESSAGE                = 3,
    CS_SCORES1                = 4,
    CS_SCORES2                = 5,
    CS_CULLDIST               = 6,
    CS_SUNLIGHT               = 7,
    CS_SUNDIR                 = 8,
    CS_FOGVARS                = 9,
    CS_MOTD                   = 10,
    CS_GAMEENDTIME            = 11,
    CS_MAPCENTER              = 12,
    CS_VOTE_TIME              = 13,
    CS_VOTE_STRING            = 14,
    CS_VOTE_YES               = 15,
    CS_VOTE_NO                = 16,
    CS_VOTE_MAPNAME           = 17,
    CS_VOTE_GAMETYPE          = 18,
    CS_MULTI_MAPWINNER        = 19,
    CS_CODINFO                = 20,
    CS_CODINFO_LAST           = 147,
    CS_CODINFO_VALUE          = 148,
    CS_CODINFO_VALUE_LAST     = 275,
    CS_ENEMY_CROSSHAIR        = 276,
    CS_USE_TRIG_STRINGS       = 277,
    CS_USE_TRIG_STRINGS_LAST  = 308,
    CS_LOCALIZED_STRINGS      = 309,
    CS_LOCALIZED_STRINGS_LAST = 820,
    CS_CASE_INSENSITIVE_BEGIN = 821,
    CS_AMBIENT                = 821,
    CS_NORTHYAW               = 822,
    CS_MINIMAP                = 823,
    CS_VISIONSET_NAKED        = 824,
    CS_VISIONSET_NIGHT        = 825,
    CS_NIGHTVISION            = 826,
    CS_LOC_SEL_MTLS           = 827,
    CS_LOC_SEL_MTLS_LAST      = 829,
    CS_MODELS                 = 830,
    CS_MODELS_LAST            = 1341,
    CS_SOUNDALIASES           = 1342,
    CS_SOUNDALIASES_LAST      = 1597,
    CS_EFFECT_NAMES           = 1598,
    CS_EFFECT_NAMES_LAST      = 1697,
    CS_EFFECT_TAGS            = 1698,
    CS_EFFECT_TAGS_LAST       = 1953,
    CS_SHELLSHOCKS            = 1954,
    CS_SHELLSHOCKS_LAST       = 1969,
    CS_SCRIPT_MENUS           = 1970,
    CS_SCRIPT_MENUS_LAST      = 2001,
    CS_SERVER_MATERIALS       = 2002,
    CS_SERVER_MATERIALS_LAST  = 2257,
    CS_WEAPONFILES            = 2258,
    CS_STATUS_ICONS           = 2259,
    CS_STATUS_ICONS_LAST      = 2266,
    CS_HEAD_ICONS             = 2267,
    CS_HEAD_ICONS_LAST        = 2281,
    CS_TAGS                   = 2282,
    CS_TAGS_LAST              = 2313,
    CS_ITEMS                  = 2314,
    CS_MAX                    = 2315,
};

enum svc_ops_e : int
{
    svc_nop = 0x0,
    svc_gamestate = 0x1,
    svc_configstring = 0x2,
    svc_baseline = 0x3,
    svc_serverCommand = 0x4,
    svc_download = 0x5,
    svc_snapshot = 0x6,
    svc_EOF = 0x7,
};

struct serverAddress_t // sizeof=0x6
{                                       // ...
    uint8_t ip[4];              // ...
    uint16_t port;              // ...
};

struct clSnapshot_t // sizeof=0x2F94
{                                       // XREF: .data:newSnap/r
                                        // clientActive_t/r ...
    int valid;                          // XREF: CL_ParseSnapshot+AF/w
                                        // CL_ParseSnapshot:loc_4A5715/w ...
    int snapFlags;                      // XREF: CL_ParseSnapshot+A1/w
    int serverTime;                     // XREF: CL_ParseSnapshot+52/w
                                        // CL_ParseSnapshot+200/r ...
    int messageNum;                     // XREF: CL_ParseSnapshot+60/w
                                        // CL_ParseSnapshot:loc_4A55D6/r ...
    int deltaNum;                       // XREF: CL_ParseSnapshot+7A/w
                                        // CL_ParseSnapshot+8F/w ...
    int ping;
    int cmdNum;
    playerState_s ps;                   // XREF: CL_ParseSnapshot+1F4/o
                                        // CL_ParseSnapshot+21B/o ...
    int numEntities;
    int numClients;
    int parseEntitiesNum;
    int parseClientsNum;
    int serverCommandNum;               // XREF: CL_ParseSnapshot+41/w
};

struct gameState_t // sizeof=0x2262C
{                                       // XREF: clientActive_t/r
    int stringOffsets[2442];
    char stringData[131072];
    int dataCount;
};

enum StanceState : int
{                                       // XREF: ?CL_SetStance@@YAXHW4StanceState@@@Z/r
    CL_STANCE_STAND = 0x0,
    CL_STANCE_CROUCH = 0x1,
    CL_STANCE_PRONE = 0x2,
};

struct ClientArchiveData // sizeof=0x30
{                                       // XREF: clientActive_t/r
    int serverTime;
    float origin[3];
    float velocity[3];
    int bobCycle;
    int movementDir;
    float viewangles[3];
};

struct outPacket_t // sizeof=0xC
{                                       // XREF: clientActive_t/r
    int p_cmdNumber;
    int p_serverTime;
    int p_realtime;
};
enum sessionState_t : int
{                                       // ...
    SESS_STATE_PLAYING = 0x0,
    SESS_STATE_DEAD = 0x1,
    SESS_STATE_SPECTATOR = 0x2,
    SESS_STATE_INTERMISSION = 0x3,
};

enum clientConnected_t : int
{                                       // ...
    CON_DISCONNECTED = 0x0,
    CON_CONNECTING = 0x1,
    CON_CONNECTED = 0x2,
};
struct playerTeamState_t // sizeof=0x4
{                                       // ...
    int location;
};

struct clientState_s // sizeof=0x64
{                                       // XREF: ?MSG_WriteDeltaClient@@YAXPAUSnapshotInfo_s@@PAUmsg_t@@HPBUclientState_s@@2H@Z/r
                                        // ?MSG_ReadDeltaClient@@YAHPAUmsg_t@@HPBUclientState_s@@PAU2@H@Z/r ...
    int clientIndex;
    team_t team;                        // XREF: SpectatorClientEndFrame(gentity_s *):loc_4F9933/r
    // SpectatorClientEndFrame(gentity_s *):loc_4F9A78/r ...
    int modelindex;
    int attachModelIndex[6];            // XREF: FX_RestorePhysicsData+156/o
    // FX_SavePhysicsData+156/o ...
    int attachTagIndex[6];              // XREF: AimTarget_ProcessEntity(int,centity_s const *)+133/o
    // AimTarget_IsTargetValid+228/o ...
    char name[16];                      // XREF: FX_UpdateEffectBolt+E7/o
    // _memmove:UnwindDown2/o ...
    float maxSprintTimeMultiplier;      // XREF: RB_LogPrintState_0(int,int)+123/o
    // R_ChangeState_0(GfxCmdBufState *,uint)+2E6/o
    int rank;
    int prestige;
    int perks;
    int attachedVehEntNum;
    int attachedVehSlotIndex;           // XREF: .rdata:_hexc_10_32_table/o
};

struct clientSession_t // sizeof=0x110
{                                       // ...
    sessionState_t sessionState;
    int forceSpectatorClient;
    int killCamEntity;
    int status_icon;
    int archiveTime;
    int score;
    int deaths;
    int kills;
    int assists;
    uint16_t scriptPersId;
    // padding byte
    // padding byte
    clientConnected_t connected;
    usercmd_s cmd;
    usercmd_s oldcmd;
    int localClient;
    int predictItemPickup;
    char newnetname[16];
    int maxHealth;
    int enterTime;
    playerTeamState_t teamState;
    int voteCount;
    int teamVoteCount;
    float moveSpeedScaleMultiplier;
    int viewmodelIndex;
    int noSpectate;
    int teamInfo;
    clientState_s cs;
    int psOffsetTime;
};

// KISAKTODO this + above should probably be in client_mp?
struct gclient_s // sizeof=0x3184
{                                       // ...
    playerState_s ps;
    clientSession_t sess;
    int spectatorClient;
    int noclip;
    int ufo;
    int bFrozen;
    int lastCmdTime;
    int buttons;
    int oldbuttons;
    int latched_buttons;
    int buttonsSinceLastFrame;
    float oldOrigin[3];
    float fGunPitch;
    float fGunYaw;
    int damage_blood;
    float damage_from[3];
    int damage_fromWorld;
    int accurateCount;
    int accuracy_shots;
    int accuracy_hits;
    int inactivityTime;
    int inactivityWarning;
    int lastVoiceTime;
    int switchTeamTime;
    float currentAimSpreadScale;
    gentity_s *persistantPowerup;
    int portalID;
    int dropWeaponTime;
    int sniperRifleFiredTime;
    float sniperRifleMuzzleYaw;
    int PCSpecialPickedUpCount;
    EntHandle useHoldEntity;
    int useHoldTime;
    int useButtonDone;
    int iLastCompassPlayerInfoEnt;
    int compassPingTime;
    int damageTime;
    float v_dmg_roll;
    float v_dmg_pitch;
    float swayViewAngles[3];
    float swayOffset[3];
    float swayAngles[3];
    float vLastMoveAng[3];
    float fLastIdleFactor;
    float vGunOffset[3];
    float vGunSpeed[3];
    int weapIdleTime;
    int lastServerTime;
    int lastSpawnTime;
    uint lastWeapon;
    bool previouslyFiring;
    bool previouslyUsingNightVision;
    bool previouslySprinting;
    // padding byte
    int hasRadar;
    int lastStand;
    int lastStandTime;
};

struct clientActive_t // sizeof=0x1B1BDC
{                                       // XREF: .data:clientActive_t * clients/r
    bool usingAds;
    // padding byte
    // padding byte
    // padding byte
    int timeoutcount;
    clSnapshot_t snap;
    bool alwaysFalse;
    // padding byte
    // padding byte
    // padding byte
    int serverTime;
    int oldServerTime;
    int oldFrameServerTime;
    int serverTimeDelta;
    int oldSnapServerTime;
    int extrapolatedSnapshot;
    int newSnapshots;
    gameState_t gameState;
    char mapname[64];
    int parseEntitiesNum;
    int parseClientsNum;
    int mouseDx[2];
    int mouseDy[2];
    int mouseIndex;
    bool stanceHeld;
    // padding byte
    // padding byte
    // padding byte
    StanceState stance;
    StanceState stancePosition;
    int stanceTime;
    int cgameUserCmdWeapon;
    int cgameUserCmdOffHandIndex;
    float cgameFOVSensitivityScale;
    float cgameMaxPitchSpeed;
    float cgameMaxYawSpeed;
    float cgameKickAngles[3];
    float cgameOrigin[3];
    float cgameVelocity[3];
    float cgameViewangles[3];
    int cgameBobCycle;
    int cgameMovementDir;
    int cgameExtraButtons;
    int cgamePredictedDataServerTime;
    float viewangles[3];
    int serverId;
    int skelTimeStamp;                  // XREF: CL_GetSkelTimeStamp(void)+E/r
    volatile uint skelMemPos;            // XREF: CL_AllocSkelMemory(uint)+97/o
    char skelMemory[262144];
    char *skelMemoryStart;              // XREF: CL_AllocSkelMemory(uint)+66/r
                                        // CL_AllocSkelMemory(uint)+BB/r
    bool allowedAllocSkel;
    // padding byte
    // padding byte
    // padding byte
    usercmd_s cmds[128];
    int cmdNumber;
    ClientArchiveData clientArchive[256];
    int clientArchiveIndex;
    outPacket_t outPackets[32];
    clSnapshot_t snapshots[32];         // XREF: Sys_GetPhysicalCpuCount+131/o
                                        // RB_LogPrintState_0(int,int)+19D/o ...
    entityState_s entityBaselines[1024];
    entityState_s parseEntities[2048];  // XREF: CG_CompassUpdateActors(int)+540/o
                                        // CountBitsEnabled(uint)+1B/o ...
    clientState_s parseClients[2048];   // XREF: AimTarget_ProcessEntity(int,centity_s const *)+133/o
                                        // AimTarget_IsTargetValid+228/o ...
    int corruptedTranslationFile;
    char translationVersion[256];
    float vehicleViewYaw;
    float vehicleViewPitch;
};

struct clientConnection_t // sizeof=0x615E8
{                                       // ...
    int qport;
    int clientNum;
    int lastPacketSentTime;
    int lastPacketTime;
    netadr_t serverAddress;
    int connectTime;
    int connectPacketCount;
    char serverMessage[256];
    int challenge;
    int checksumFeed;
    int reliableSequence;
    int reliableAcknowledge;
    char reliableCommands[128][1024];
    int serverMessageSequence;
    int serverCommandSequence;
    int lastExecutedServerCommand;
    char serverCommands[128][1024];     // ...
    bool isServerRestarting;
    // padding byte
    // padding byte
    // padding byte
    int lastClientArchiveIndex;
    char demoName[64];
    int demorecording;
    int demoplaying;
    int isTimeDemo;
    int demowaiting;
    int firstDemoFrameSkipped;
    int demofile;
    int timeDemoLog;
    int timeDemoFrames;
    int timeDemoStart;
    int timeDemoPrev;
    int timeDemoBaseTime;
    netchan_t netchan;
    char netchanOutgoingBuffer[2048];
    char netchanIncomingBuffer[0x20000];
    netProfileInfo_t OOBProf;
    uint8_t statPacketsToSend;
    // padding byte
    // padding byte
    // padding byte
    int statPacketSendTime[7];
};




struct serverInfo_t // sizeof=0x94
{                                       // ...
    netadr_t adr;                       // ...
    uint8_t allowAnonymous;     // ...
    uint8_t bPassword;
    uint8_t pure;
    char consoleDisabled;
    uint8_t netType;            // ...
    uint8_t clients;            // ...
    uint8_t maxClients;         // ...
    uint8_t dirty;              // ...
    char friendlyfire;
    char killcam;
    uint8_t hardware;
    uint8_t mod;
    uint8_t voice;
    uint8_t punkbuster;         // ...
    uint8_t requestCount;       // ...
    // padding byte
    __int16 minPing;                    // ...
    __int16 maxPing;                    // ...
    __int16 ping;                       // ...
    char hostName[32];                  // ...
    char mapName[32];                   // ...
    char game[24];                      // ...
    char gameType[16];                  // ...
    // padding byte
    // padding byte
};

struct clientLogo_t // sizeof=0x18
{                                       // ...
    int startTime;                      // ...
    int duration;                       // ...
    int fadein;                         // ...
    int fadeout;                        // ...
    Material *material[2];              // ...
};
struct vidConfig_t // sizeof=0x30
{                                       // ...
    uint sceneWidth;            // ...
    uint sceneHeight;           // ...
    uint displayWidth;          // ...
    uint displayHeight;         // ...
    uint displayFrequency;      // ...
    int isFullscreen;                   // ...
    float aspectRatioWindow;            // ...
    float aspectRatioScenePixel;        // ...
    float aspectRatioDisplayPixel;      // ...
    uint maxTextureSize;        // ...
    uint maxTextureMaps;        // ...
    bool deviceSupportsGamma;           // ...
    // padding byte
    // padding byte
    // padding byte
};

struct Font_s;

struct clientStatic_t // sizeof=0x2DD070
{                                       // ...
    int quit;                           // ...
    int hunkUsersStarted;               // ...
    char servername[256];               // ...
    int rendererStarted;                // ...
    int soundStarted;                   // ...
    int uiStarted;                      // ...
    int devGuiStarted;                  // ...
    int frametime;                      // ...
    int realtime;                       // ...
    int realFrametime;                  // ...
    clientLogo_t logo;                  // ...
    float mapCenter[3];                 // ...
    int numlocalservers;                // ...
    serverInfo_t localServers[128];     // ...
    int waitglobalserverresponse;       // ...
    int numglobalservers;               // ...
    serverInfo_t globalServers[20000];  // ...
    int numfavoriteservers;             // ...
    serverInfo_t favoriteServers[128];  // ...
    int pingUpdateSource;               // ...
    netadr_t updateServer;
    char updateChallenge[1024];
    char updateInfoString[1024];        // ...
    netadr_t authorizeServer;           // ...
    Material *whiteMaterial;            // ...
    Material *consoleMaterial;          // ...
    Font_s *consoleFont;                // ...
    char autoupdateServerNames[5][64];  // ...
    netadr_t autoupdateServer;          // ...
    vidConfig_t vidConfig;              // ...
    clientDebug_t debug;                // ...
    int download;                       // ...
    char downloadTempName[256];         // ...
    char downloadName[256];             // ...
    int downloadNumber;
    int downloadBlock;                  // ...
    int downloadCount;                  // ...
    int downloadSize;                   // ...
    char downloadList[1024];            // ...
    int downloadRestart;                // ...
    int gameDirChanged;                 // ...
    int wwwDlDisconnected;              // ...
    int wwwDlInProgress;                // ...
    int downloadFlags;                  // ...
    char originalDownloadName[64];      // ...
    float debugRenderPos[3];            // ...
};

enum connstate_t : int
{                                       // ...
    CA_DISCONNECTED = 0x0,
    CA_CINEMATIC = 0x1,
    CA_LOGO = 0x2,
    CA_CONNECTING = 0x3,
    CA_CHALLENGING = 0x4,
    CA_CONNECTED = 0x5,
    CA_SENDINGSTATS = 0x6,
    CA_LOADING = 0x7,
    CA_PRIMED = 0x8,
    CA_ACTIVE = 0x9,
};

struct clientUIActive_t // sizeof=0x10
{
    bool active;
    bool isRunning;
    bool cgameInitialized;
    bool cgameInitCalled;
    int keyCatchers;
    bool displayHUDWithKeycatchUI;
    // padding byte
    // padding byte
    // padding byte
    connstate_t connectionState;
#ifdef _XBOX
    int nextScrollTime;
    bool invited;                       // XREF: CL_CheckForResend(int)+29C/r
                                         // Live_MPAcceptInvite+208/w ...
    // padding byte
    // padding byte
    // padding byte
    int numVoicePacketsSent;
#endif
};

struct ClientVoicePacket_t // sizeof=0x104
{                                       // ...
    uint8_t data[256];
    int dataSize;
};
struct voiceCommunication_t // sizeof=0xA30
{                                       // ...
    ClientVoicePacket_t voicePackets[10];
    int voicePacketCount;
    int voicePacketLastTransmit;
};

struct VoicePacket_t // sizeof=0x105
{                                       // ...
    uint8_t talker;             // ...
    uint8_t data[256];          // ...
    int dataSize;                       // ...
};

struct ping_t // sizeof=0x41C
{                                       // ...
    netadr_t adr;                       // ...
    int start;                          // ...
    int time;                           // ...
    char info[1024];                    // ...
};

// cl_main_mp
//void __cdecl TRACK_cl_main();
void __cdecl CL_SortGlobalServers();
void __cdecl CL_GetAutoUpdate();
struct serverStatus_s *__cdecl CL_GetServerStatus(netadr_t from);
char __cdecl CL_IsLocalClientActive(int localClientNum);
int __cdecl CL_LocalActiveIndexFromClientNum(int localClientNum);
int __cdecl CL_ControllerIndexFromClientNum(int clientIndex);
char __cdecl CL_AllLocalClientsDisconnected();
char __cdecl CL_AnyLocalClientChallenging();
const char *__cdecl CL_GetUsernameForLocalClient();
void __cdecl CL_AddReliableCommand(int localClientNum, const char *cmd);
void __cdecl CL_ShutdownDevGui();
void __cdecl CL_ShutdownHunkUsers();
void __cdecl CL_ShutdownAll(bool destroyWindow);
char __cdecl CL_AnyLocalClientsRunning();
void __cdecl CL_MapLoading(const char *mapname);
void __cdecl CL_ResetSkeletonCache(int localClientNum);
void __cdecl CL_ClearState(int localClientNum);
void __cdecl CL_Disconnect(int localClientNum);
void __cdecl CL_ClearStaticDownload();
void __cdecl CL_ForwardCommandToServer(int localClientNum, const char *string);
void __cdecl CL_RequestAuthorization(int localClientNum);
void __cdecl CL_ForwardToServer_f();
void __cdecl CL_Setenv_f();
void __cdecl CL_DisconnectLocalClient(int localClientNum);
void __cdecl CL_Reconnect_f();
void __cdecl CL_Vid_Restart_f();
void __cdecl CL_Snd_Restart_f();
void __cdecl CL_Configstrings_f();
void __cdecl CL_Clientinfo_f();
bool __cdecl CL_WasMapAlreadyLoaded();
void __cdecl CL_DownloadsComplete(int localClientNum);
void __cdecl CL_CheckForResend(int localClientNum);
int __cdecl CL_HighestPriorityStatPacket(clientConnection_t *clc);
void __cdecl CL_DisconnectError(char *message);
char __cdecl CL_ConnectionlessPacket(int localClientNum, netadr_t from, msg_t *msg, int time);
char __cdecl CL_DispatchConnectionlessPacket(int localClientNum, netadr_t from, msg_t *msg, int time);
void __cdecl CL_DisconnectPacket(int localClientNum, netadr_t from, char *reason);
void __cdecl CL_InitLoad(const char *mapname, const char *gametype);
char __cdecl CL_PacketEvent(int localClientNum, netadr_t from, msg_t *msg, int time);
void __cdecl CL_VoiceTransmit(int localClientNum);
void __cdecl CL_RunOncePerClientFrame(int localClientNum, int msec);
void __cdecl CL_Frame(int localClientNum);
void __cdecl CL_CheckTimeout(int localClientNum);
void __cdecl CL_ServerTimedOut();
void __cdecl CL_CheckUserinfo(int localClientNum);
void __cdecl CL_UpdateInGameState(int localClientNum);
void __cdecl CL_VoiceFrame(int localClientNum);
bool __cdecl CL_IsLocalClientInGame(int localClientNum);
char __cdecl CL_IsClientLocal(int clientNum);
void __cdecl CL_ParseBadPacket_f();
void __cdecl CL_ShutdownRef();
void __cdecl CL_InitRenderer();
void __cdecl CL_ShutdownRenderer(int destroyWindow);
void __cdecl CL_StartHunkUsers();
void CL_InitDevGui();
void __cdecl CL_DevGuiDvar_f();
void __cdecl CL_DevGuiCmd_f();
void __cdecl CL_DevGuiOpen_f();
int __cdecl CL_ScaledMilliseconds();
void __cdecl CL_InitRef();
void __cdecl CL_startSingleplayer_f();
void __cdecl CL_DrawLogo(int localClientNum);
void __cdecl CL_StopLogo(int localClientNum);
void __cdecl CL_PlayLogo_f();
void __cdecl CL_StopLogoOrCinematic(int localClientNum);
void __cdecl CL_ToggleMenu_f();
void __cdecl CL_InitOnceForAllClients();
void __cdecl CL_Disconnect_f();
void __cdecl CL_Init(int localClientNum);
// int __cdecl CountBitsEnabled(uint num);
void __cdecl CL_Shutdown(int localClientNum);
void __cdecl CL_LocalServers_f();
void __cdecl CL_GetPing(int n, char *buf, int buflen, int *pingtime);
void __cdecl CL_ClearPing(uint n);
int __cdecl CL_GetPingQueueCount();
int __cdecl CL_UpdateDirtyPings(int localClientNum, uint source);
void __cdecl CL_ShowIP_f();
void __cdecl CL_SetupForNewServerMap(char *pszMapName, char *pszGametype);
bool __cdecl CL_IsServerLoadingMap();
bool __cdecl CL_IsWaitingOnServerToLoadMap(int localClientNum);
void __cdecl CL_SetWaitingOnServerToLoadMap(int localClientNum, bool waiting);
void __cdecl CL_DrawTextPhysical(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style);
void __cdecl CL_DrawTextPhysicalWithEffects(
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style,
    const float *glowColor,
    struct Material *fxMaterial,
    struct Material *fxMaterialGlow,
    int fxBirthTime,
    int fxLetterTime,
    int fxDecayStartTime,
    int fxDecayDuration);
void __cdecl CL_DrawText(
    const ScreenPlacement *scrPlace,
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    int horzAlign,
    int vertAlign,
    float xScale,
    float yScale,
    const float *color,
    int style);
void __cdecl CL_DrawTextRotate(
    const ScreenPlacement *scrPlace,
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float rotation,
    int horzAlign,
    int vertAlign,
    float xScale,
    float yScale,
    const float *color,
    int style);
void __cdecl CL_DrawTextPhysicalWithCursor(
    char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    float xScale,
    float yScale,
    const float *color,
    int style,
    int cursorPos,
    char cursor);
void __cdecl CL_DrawTextWithCursor(
    const ScreenPlacement *scrPlace,
    const char *text,
    int maxChars,
    Font_s *font,
    float x,
    float y,
    int horzAlign,
    int vertAlign,
    float xScale,
    float yScale,
    const float *color,
    int style,
    int cursorPos,
    char cursor);
bool __cdecl CL_ShouldDisplayHud(int localClientNum);
bool __cdecl CL_IsUIActive(int localClientNum);
struct Font_s *__cdecl CL_RegisterFont(const char *fontName, int imageTrack);
void __cdecl CL_UpdateSound();
float (*__cdecl CL_GetMapCenter())[3];
void __cdecl KISAK_NULLSUB();
int __cdecl CL_GetLocalClientActiveCount();
void __cdecl CL_InitDedicated();

extern const dvar_t *cl_conXOffset;
extern const dvar_t *cl_hudDrawsBehindsUI;
extern const dvar_t *cl_showSend;
extern const dvar_t *input_invertPitch;
extern const dvar_t *cl_avidemo;
extern const dvar_t *cl_nodelta;
extern const dvar_t *cl_showServerCommands;
extern const dvar_t *motd;
extern const dvar_t *cl_connectTimeout;
extern const dvar_t *cl_sensitivity;
extern const dvar_t *cl_forceavidemo;
extern const dvar_t *cl_timeout;
extern const dvar_t *m_yaw;
extern const dvar_t *customclass[5];
extern const dvar_t *m_pitch;
extern const dvar_t *cl_activeAction;
extern const dvar_t *playlist;
extern const dvar_t *cl_debugMessageKey;
extern const dvar_t *systemlink;
extern const dvar_t *nextdemo;
extern const dvar_t *cl_connectionAttempts;
extern const dvar_t *onlinegame;
extern const dvar_t *cl_showMouseRate;
extern const dvar_t *m_forward;
extern const dvar_t *cl_packetdup;
extern const dvar_t *cl_mouseAccel;
extern const dvar_t *cl_maxpackets;
extern const dvar_t *cl_motdString;
extern const dvar_t *onlinegameandhost;
extern const dvar_t *cl_freezeDemo;
extern const dvar_t *cl_showTimeDelta;
extern const dvar_t *input_viewSensitivity;
extern const dvar_t *input_autoAim;
extern const dvar_t *cl_ingame;
extern const dvar_t *cl_inGameVideo;
extern const dvar_t *cl_noprint;
extern const dvar_t *m_side;
extern const dvar_t *cl_profileTextY;
extern const dvar_t *cl_serverStatusResendTime;
extern const dvar_t *m_filter;
extern const dvar_t *cl_profileTextHeight;
extern const dvar_t *cl_shownuments;
extern const dvar_t *splitscreen;
extern const dvar_t *onlineunreankedgameandhost;
extern const dvar_t *cl_freelook;
extern const dvar_t *cl_shownet;

extern const dvar_t *cl_updateavailable;
extern const dvar_t *cl_updatefiles;
extern const dvar_t *cl_updateoldversion;
extern const dvar_t *cl_updateversion;
extern const dvar_t *cl_allowDownload;
extern const dvar_t *cl_wwwDownload;
extern const dvar_t *cl_talking;
extern const dvar_t *cl_bypassMouseInput;
extern const dvar_t *cl_anglespeedkey;
extern const dvar_t *cl_pitchspeed;
extern const dvar_t *cl_yawspeed;
extern const dvar_t *cl_hudDrawsBehindUI;
extern const dvar_t *cl_voice;
extern const dvar_t *name;

//extern ping_t *cl_pinglist;
extern ping_t cl_pinglist[16];

extern BOOL g_waitingForServer;
extern bool cl_waitingOnServerToLoadMap[1];
extern BOOL cl_serverLoadingMap;

extern clientConnection_t clientConnections[STATIC_MAX_LOCAL_CLIENTS];
extern clientUIActive_t clientUIActives[STATIC_MAX_LOCAL_CLIENTS];
extern clientActive_t clients[STATIC_MAX_LOCAL_CLIENTS];

extern clientStatic_t cls;

extern uint frame_msec;

extern char cl_cdkey[34];

extern BOOL updateScreenCalled;

extern const char *svc_strings[256];
extern int autoupdateStarted;
extern char autoupdateFilename[64];
extern int cl_connectedToPureServer;


inline clientActive_t *__cdecl CL_GetLocalClientGlobals(int localClientNum)
{
    iassert(clients);
    iassert(localClientNum == 0);

    return &clients[localClientNum];
}

// cl_cgame_mp
struct snapshot_s;
struct snd_alias_t;
struct refdef_s;
struct MemoryFile;

void __cdecl TRACK_cl_cgame();
void __cdecl CL_ReadDemoMessage(int localClientNum);
void __cdecl CL_GetScreenDimensions(int *width, int *height, float *aspect);
double __cdecl CL_GetScreenAspectRatioDisplayPixel();
int __cdecl CL_GetUserCmd(int localClientNum, int cmdNumber, usercmd_s *ucmd);
int __cdecl CL_GetCurrentCmdNumber(int localClientNum);
void __cdecl CL_GetCurrentSnapshotNumber(int localClientNum, int *snapshotNumber, int *serverTime);
int __cdecl CL_GetSnapshot(int localClientNum, int snapshotNumber, snapshot_s *snapshot);
void __cdecl CL_SetUserCmdWeapons(int localClientNum, int weapon, int offHandIndex);
void __cdecl CL_SetUserCmdAimValues(int localClientNum, const float *kickAngles);
void __cdecl CL_SetUserCmdOrigin(
    int localClientNum,
    const float *origin,
    const float *velocity,
    const float *viewangles,
    int bobCycle,
    int movementDir);
void __cdecl CL_SetFOVSensitivityScale(int localClientNum, float scale);
void __cdecl CL_SetExtraButtons(int localClientNum, int buttons);
void __cdecl CL_DumpReliableCommands(int localClientNum);
int __cdecl CL_CGameNeedsServerCommand(int localClientNum, int serverCommandNumber);
void __cdecl CL_ConfigstringModified(int localClientNum);
void __cdecl CL_CM_LoadMap(char *mapname);
void __cdecl CL_ShutdownCGame(int localClientNum);
bool __cdecl CL_DObjCreateSkelForBone(DObj_s *obj, int boneIndex);
void __cdecl CL_SubtitlePrint(int localClientNum, const char *text, int duration, int lineWidth);
const char *__cdecl CL_GetConfigString(int localClientNum, uint configStringIndex);
snd_alias_t *__cdecl CL_PickSoundAlias(const char *aliasname);
void __cdecl CL_RenderScene(const refdef_s *fd);
void __cdecl CL_DrawStretchPicPhysical(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material);
void __cdecl CL_DrawStretchPicPhysicalRotateXY(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    float angle,
    const float *color,
    Material *material);
void __cdecl CL_DrawStretchPicPhysicalFlipST(
    float x,
    float y,
    float w,
    float h,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material);
void __cdecl CL_DrawStretchPic(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int horzAlign,
    int vertAlign,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material);
void __cdecl CL_DrawStretchPicFlipST(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int horzAlign,
    int vertAlign,
    float s1,
    float t1,
    float s2,
    float t2,
    const float *color,
    Material *material);
void __cdecl CL_DrawStretchPicRotatedST(
    const ScreenPlacement *scrPlace,
    float x,
    float y,
    float w,
    float h,
    int horzAlign,
    int vertAlign,
    float centerS,
    float centerT,
    float radiusST,
    float scaleFinalS,
    float scaleFinalT,
    float angle,
    const float *color,
    Material *material);
void __cdecl CL_CapTurnRate(int localClientNum, float maxPitchSpeed, float maxYawSpeed);
void __cdecl CL_SyncTimes(int localClientNum);
int __cdecl LoadWorld(char *mapname);
void __cdecl CL_StartLoading();
void __cdecl CL_InitCGame(int localClientNum);
void __cdecl CL_FirstSnapshot(int localClientNum);
void __cdecl CL_SetCGameTime(int localClientNum);
void __cdecl CL_AdjustTimeDelta(int localClientNum);
void __cdecl CL_SetADS(int localClientNum, bool ads);
void __cdecl CL_DrawString(int x, int y, char *pszString, int bShadow, int iCharHeight);
void __cdecl CL_DrawRect(int x, int y, int width, int height, const float *color);
void __cdecl CL_ArchiveClientState(int localClientNum, MemoryFile *memFile);
void __cdecl CL_LookupColor(int localClientNum, uint8_t c, float *color);
void __cdecl CL_UpdateColor(int localClientNum);
void __cdecl CL_UpdateColorInternal(const char *var_name, float *color);
int __cdecl CL_IsCgameInitialized(int localClientNum);



// cl_net_chan_mp
void __cdecl CL_Netchan_TransmitNextFragment(netchan_t *chan);
void __cdecl CL_Netchan_Transmit(netchan_t *chan, uint8_t *data, int length);
void __cdecl CL_Netchan_AddOOBProfilePacket(int localClientNum, int iLength);
void __cdecl CL_Netchan_PrintProfileStats(int localClientNum, int bPrintToConsole);
void __cdecl CL_Netchan_UpdateProfileStats(int localClientNum);
void __cdecl CL_ProfDraw(int y, char *pszString);
void __cdecl CL_Netchan_Encode(uint8_t *data, int size);
void __cdecl CL_Netchan_Decode(uint8_t *data, int size);




// cl_parse_mp
void __cdecl TRACK_cl_parse();
void __cdecl SHOWNET(msg_t *msg, const char *s);
void __cdecl CL_SavePredictedOriginForServerTime(
    clientActive_t *cl,
    int serverTime,
    float *predictedOrigin,
    float *predictedVelocity,
    float *viewangles,
    int bobCycle,
    int movementDir);
bool __cdecl CL_GetPredictedOriginForServerTime(
    clientActive_t *cl,
    int serverTime,
    float *predictedOrigin,
    float *predictedVelocity,
    float *viewangles,
    int *bobCycle,
    int *movementDir);
void __cdecl CL_DeltaClient(
    clientActive_t *cl,
    msg_t *msg,
    int time,
    clSnapshot_t *frame,
    uint newnum,
    clientState_s *old,
    int unchanged);
void __cdecl CL_SystemInfoChanged(int localClientNum);
void __cdecl CL_ParseMapCenter(int localClientNum);
void __cdecl CL_ParseServerMessage(int localClientNum, msg_t *msg);
void __cdecl CL_ParseSnapshot(int localClientNum, msg_t *msg);
void __cdecl CL_ParsePacketEntities(
    clientActive_t *cl,
    msg_t *msg,
    int time,
    clSnapshot_t *oldframe,
    clSnapshot_t *newframe);
void __cdecl CL_DeltaEntity(
    clientActive_t *cl,
    msg_t *msg,
    int time,
    clSnapshot_t *frame,
    uint newnum,
    entityState_s *old);
void __cdecl CL_CopyOldEntity(clientActive_t *cl, clSnapshot_t *frame, entityState_s *old);
void __cdecl CL_ParsePacketClients(
    clientActive_t *cl,
    msg_t *msg,
    int time,
    clSnapshot_t *oldframe,
    clSnapshot_t *newframe);
void __cdecl CL_ParseGamestate(int localClientNum, msg_t *msg);
void __cdecl CL_ParseCommandString(int localClientNum, msg_t *msg);



// cl_pose_mp
char *__cdecl CL_AllocSkelMemory(uint size);
int __cdecl CL_GetSkelTimeStamp();
int __cdecl CL_DObjCreateSkelForBones(const DObj_s *obj, int *partBits, DObjAnimMat **pMatOut);



// cl_rank
enum rankTableColumns_t : int
{                                       // ...
    MP_RANKTABLE_RANKID = 0x0,
    MP_RANKTABLE_RANK = 0x1,
    MP_RANKTABLE_MINXP = 0x2,
    MP_RANKTABLE_XPTONEXT = 0x3,
    MP_RANKTABLE_SHORTRANK = 0x4,
    MP_RANKTABLE_FULLRANK = 0x5,
    MP_RANKTABLE_ICON = 0x6,
    MP_RANKTABLE_MAXXP = 0x7,
    MP_RANKTABLE_WEAPUNLOCK = 0x8,
    MP_RANKTABLE_PERKUNLOCK = 0x9,
    MP_RANKTABLE_CHALLENGE = 0xA,
    MP_RANKTABLE_CAMO = 0xB,
    MP_RANKTABLE_ATTACHMENT = 0xC,
    MP_RANKTABLE_LEVEL = 0xD,
    MP_RANKTABLE_DISPLAYLEVEL = 0xE,
    MP_RANKTABLE_COUNT = 0xF,
};
int __cdecl CL_GetRankForXp(int xp);
const char *__cdecl CL_GetRankData(int rank, rankTableColumns_t column);
void __cdecl CL_GetRankIcon(int rank, int prestige, Material **handle);


// cl_voice (different on PC)
void __cdecl CL_WriteVoicePacket(int localClientNum);
void __cdecl CL_VoicePacket(int localClientNum, msg_t *msg);
bool __cdecl CL_IsPlayerTalking(int localClientNum, int talkingClientIndex);


//
// cl_input
//
typedef struct {
    int			down[2];		// key nums holding it down
    unsigned	downtime;		// msec timestamp
    unsigned	msec;			// msec down this frame if both a down and up happened
    qboolean	active;			// current state
    qboolean	wasPressed;		// set when down, not cleared when up
} kbutton_t;

void __cdecl TRACK_cl_input();
void __cdecl CL_ShowSystemCursor(BOOL show);
int __cdecl CL_MouseEvent(int x, int y, int dx, int dy);
void __cdecl CL_SetStance(int localClientNum, StanceState stance);
void __cdecl IN_CenterView();
void __cdecl CL_UpdateCmdButton(int localClientNum, int *cmdButtons, int kbButton, int buttonFlag);
void __cdecl CL_WritePacket(int localClientNum);
void __cdecl CL_SendCmd(int localClientNum);
bool __cdecl CL_ReadyToSendPacket(int localClientNum);
void __cdecl CL_CreateCmdsDuringConnection(int localClientNum);
void __cdecl CL_CreateNewCommands(int localClientNum);
usercmd_s *__cdecl CL_CreateCmd(usercmd_s *result, int localClientNum);
void __cdecl CL_AdjustAngles(int localClientNum);
float __cdecl CL_KeyState(kbutton_t *key);
void __cdecl CL_KeyMove(int localClientNum, usercmd_s *cmd);
void __cdecl CL_StanceButtonUpdate(int localClientNum);
void __cdecl CL_AddCurrentStanceToCmd(int localClientNum, usercmd_s *cmd);
void __cdecl CL_MouseMove(int localClientNum, usercmd_s *cmd);
void __cdecl CL_GetMouseMovement(clientActive_t *cl, float *mx, float *my);
void __cdecl CL_CmdButtons(int localClientNum, usercmd_s *cmd);
void __cdecl CL_FinishMove(int localClientNum, usercmd_s *cmd);
char __cdecl CG_HandleLocationSelectionInput(int localClientNum, usercmd_s *cmd);
void __cdecl CL_Input(int localClientNum);
void __cdecl CL_InitInput();
void __cdecl IN_MLookDown();
void __cdecl IN_MLookUp();
void __cdecl IN_UpDown();
void __cdecl IN_KeyDown(kbutton_t *b);
void __cdecl IN_UpUp();
void __cdecl IN_KeyUp(kbutton_t *b);
void __cdecl IN_DownDown();
void __cdecl IN_DownUp();
void __cdecl IN_LeftDown();
void __cdecl IN_LeftUp();
void __cdecl IN_RightDown();
void __cdecl IN_RightUp();
void __cdecl IN_ForwardDown();
void __cdecl IN_ForwardUp();
void __cdecl IN_BackDown();
void __cdecl IN_BackUp();
void __cdecl IN_LookupDown();
void __cdecl IN_LookupUp();
void __cdecl IN_LookdownDown();
void __cdecl IN_LookdownUp();
void __cdecl IN_MoveleftDown();
void __cdecl IN_MoveleftUp();
void __cdecl IN_MoverightDown();
void __cdecl IN_MoverightUp();
void __cdecl IN_SpeedDown();
void __cdecl IN_SpeedUp();
void __cdecl IN_StrafeDown();
void __cdecl IN_StrafeUp();
void __cdecl IN_Attack_Down();
void __cdecl IN_Attack_Up();
void __cdecl IN_Breath_Down();
void __cdecl IN_Breath_Up();
void __cdecl IN_MeleeBreath_Down();
void __cdecl IN_MeleeBreath_Up();
void __cdecl IN_Frag_Down();
void __cdecl IN_Frag_Up();
void __cdecl IN_Smoke_Down();
void __cdecl IN_Smoke_Up();
void __cdecl IN_BreathSprint_Down();
void __cdecl IN_BreathSprint_Up();
void __cdecl IN_Melee_Down();
void __cdecl IN_Melee_Up();
void __cdecl IN_Activate_Down();
void __cdecl IN_Activate_Up();
void __cdecl IN_Reload_Down();
void __cdecl IN_Reload_Up();
void __cdecl IN_UseReload_Down();
void __cdecl IN_UseReload_Up();
void __cdecl IN_LeanLeft_Down();
void __cdecl IN_LeanLeft_Up();
void __cdecl IN_LeanRight_Down();
void __cdecl IN_LeanRight_Up();
void __cdecl IN_Prone_Down();
void __cdecl IN_Prone_Up();
void __cdecl IN_Stance_Down();
void __cdecl IN_Stance_Up();
void __cdecl IN_ToggleADS();
void __cdecl IN_LeaveADS();
void __cdecl IN_Throw_Down();
void __cdecl IN_Throw_Up();
void __cdecl IN_ToggleADS_Throw_Down();
void __cdecl IN_ToggleADS_Throw_Up();
void __cdecl IN_Speed_Throw_Down();
void __cdecl IN_Speed_Throw_Up();
void __cdecl IN_LowerStance();
void __cdecl IN_RaiseStance();
void __cdecl IN_ToggleCrouch();
void __cdecl CL_ToggleStance(int localClientNum, StanceState preferredStance);
void __cdecl IN_ToggleProne();
void __cdecl IN_GoProne();
void __cdecl IN_GoCrouch();
void __cdecl IN_GoStandDown();
void __cdecl IN_GoStandUp();
void __cdecl IN_SprintDown();
void __cdecl IN_SprintUp();
void __cdecl CL_ShutdownInput();
void __cdecl CL_ClearKeys(int localClientNum);

// cl_ui_mp
struct uiClientState_s // sizeof=0xC08
{                                       // ...
    connstate_t connState;              // ...
    int connectPacketCount;             // ...
    char servername[1024];              // ...
    char updateInfoString[1024];
    char messageString[1024];           // ...
};
void __cdecl CL_GetClientState(int localClientNum, uiClientState_s* state);
void __cdecl CL_SetDisplayHUDWithKeycatchUI(int localClientNum, bool display);
bool __cdecl CL_AllowPopup(int localClientNum);
void __cdecl LAN_ResetPings(int source);
int __cdecl LAN_GetServerCount(int source);
int __cdecl LAN_WaitServerResponse(int source);
void __cdecl LAN_GetServerInfo(int source, uint n, char* buf, int buflen);
int __cdecl LAN_GetServerPing(int source, uint n);
serverInfo_t* __cdecl LAN_GetServerPtr(int source, uint n);
void __cdecl LAN_CleanHostname(const char* hostName, char* cleanHostName);
int __cdecl LAN_CompareHostname(const char* hostName1, const char* hostName2);
int __cdecl LAN_CompareServers(int source, int sortKey, int sortDir, uint s1, uint s2);
void __cdecl LAN_MarkServerDirty(int source, uint n, uint8_t dirty);
int __cdecl LAN_ServerIsDirty(int source, uint n);
int __cdecl LAN_UpdateDirtyPings(int localClientNum, uint source);
void __cdecl Key_KeynumToStringBuf(int keynum, char* buf, int buflen);
int __cdecl CL_GetClientName(int localClientNum, int index, char* buf, int size);
int __cdecl CL_ShutdownUI();
void __cdecl CL_InitUI();



// cl_main_pc_mp
int __cdecl CL_ServerStatus(char *serverAddress, char *serverStatusString, int maxLen);
void __cdecl CL_SetServerInfoByAddress(netadr_t from, char *info, __int16 ping);
void __cdecl CL_SetServerInfo(serverInfo_t *server, char *info, __int16 ping);
void __cdecl CL_ServerInfoPacket(netadr_t from, msg_t *msg, int time);
void __cdecl CL_Connect_f();
//bool __cdecl CL_CDKeyValidate(const char *key, const char *checksum);
bool __cdecl CL_CDKeyValidate(netadr_t addr);

void __cdecl CL_GlobalServers_f();
void __cdecl CL_ServerStatusResponse(netadr_t from, msg_t *msg);
void __cdecl CL_ResetPlayerMuting(uint muteClientIndex);
void __cdecl CL_MutePlayer(int localClientNum, uint muteClientIndex);
bool __cdecl CL_IsPlayerMuted(int localClientNum, uint muteClientIndex);
void __cdecl CL_ClearMutedList();


extern voiceCommunication_t cl_voiceCommunication;
extern serverStatus_s cl_serverStatusList[16];
extern int serverStatusCount;