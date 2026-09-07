#include <universal/q_shared.h>
#include <stddef.h>
#include "bg_local.h"
#include "bg_public.h"
#include <qcommon/mem_track.h>
#include <database/database.h>
#include <universal/q_parse.h>
#include <universal/com_memory.h>
#include <universal/com_files.h>
#include <universal/com_sndalias.h>
#include <universal/surfaceflags.h>

//int surfaceTypeSoundListCount 828010f0     bg_weapons_load_obj.obj
//struct SurfaceTypeSoundList *surfaceTypeSoundLists 828011f8     bg_weapons_load_obj.obj

uint g_playerAnimTypeNamesCount;

SurfaceTypeSoundList surfaceTypeSoundLists[16];

const char *stickinessNames[4] =
{
  "Don't stick",
  "Stick to all",
  "Stick to ground",
  "Stick to ground, maintain yaw"
}; // idb
const char *weapIconRatioNames[3] = { "1:1", "2:1", "4:1" }; // idb
const char *ammoCounterClipNames[7] =
{
  "None",
  "Magazine",
  "ShortMagazine",
  "Shotgun",
  "Rocket",
  "Beltfed",
  "AltWeapon"
}; // idb
const char *overlayInterfaceNames[3] = { "None", "Javelin", "Turret Scope" }; // idb
const char *szWeapFireTypeNames[5] =
{
  "Full Auto",
  "Single Shot",
  "2-Round Burst",
  "3-Round Burst",
  "4-Round Burst"
}; // idb
const char *szWeapInventoryTypeNames[4] = { "primary", "offhand", "item", "altmode" }; // idb
const char *penetrateTypeNames[4] = { "none", "small", "medium", "large" }; // idb
const char *szWeapOverlayReticleNames[2] = { "none", "crosshair" }; // idb
const char *szWeapStanceNames[3] = { "stand", "duck", "prone" }; // idb
const char *accuracyDirName[3] = { "aivsai", "aivsplayer", NULL }; // idb
const char *activeReticleNames[3] = { "None", "Pip-On-A-Stick", "Bouncing diamond" }; // idb
const char *szWeapTypeNames[4] = { "bullet", "grenade", "projectile", "binoculars" }; // idb
const char *guidedMissileNames[4] = { "None", "Sidewinder", "Hellfire", "Javelin" }; // idb
const char *offhandClassNames[4] = { "None", "Frag Grenade", "Smoke Grenade", "Flash Grenade" }; // idb
const char *szProjectileExplosionNames[7] = { "grenade", "rocket", "flashbang", "none", "dud", "smoke", "heavy explosive" }; // idb

const char *impactTypeNames[9] =
{
  "none",
  "bullet_small",
  "bullet_large",
  "bullet_ap",
  "shotgun",
  "grenade_bounce",
  "grenade_explode",
  "rocket_explode",
  "projectile_dud"
}; // idb

cspField_t weaponDefFields[502] =
{
  { "displayName", offsetof(WeaponDef, szDisplayName), CSPFT_STRING },
  { "AIOverlayDescription", offsetof(WeaponDef, szOverlayName), CSPFT_STRING },
  { "modeName", offsetof(WeaponDef, szModeName), CSPFT_STRING },
  { "playerAnimType", offsetof(WeaponDef, playerAnimType), WFT_ANIMTYPE },
  { "gunModel", offsetof(WeaponDef, gunXModel[0]), CSPFT_XMODEL },
  { "gunModel2", offsetof(WeaponDef, gunXModel[1]), CSPFT_XMODEL },
  { "gunModel3", offsetof(WeaponDef, gunXModel[2]), CSPFT_XMODEL },
  { "gunModel4", offsetof(WeaponDef, gunXModel[3]), CSPFT_XMODEL },
  { "gunModel5", offsetof(WeaponDef, gunXModel[4]), CSPFT_XMODEL },
  { "gunModel6", offsetof(WeaponDef, gunXModel[5]), CSPFT_XMODEL },
  { "gunModel7", offsetof(WeaponDef, gunXModel[6]), CSPFT_XMODEL },
  { "gunModel8", offsetof(WeaponDef, gunXModel[7]), CSPFT_XMODEL },
  { "gunModel9", offsetof(WeaponDef, gunXModel[8]), CSPFT_XMODEL },
  { "gunModel10", offsetof(WeaponDef, gunXModel[9]), CSPFT_XMODEL },
  { "gunModel11", offsetof(WeaponDef, gunXModel[10]), CSPFT_XMODEL },
  { "gunModel12", offsetof(WeaponDef, gunXModel[11]), CSPFT_XMODEL },
  { "gunModel13", offsetof(WeaponDef, gunXModel[12]), CSPFT_XMODEL },
  { "gunModel14", offsetof(WeaponDef, gunXModel[13]), CSPFT_XMODEL },
  { "gunModel15", offsetof(WeaponDef, gunXModel[14]), CSPFT_XMODEL },
  { "gunModel16", offsetof(WeaponDef, gunXModel[15]), CSPFT_XMODEL },
  { "handModel", offsetof(WeaponDef, handXModel), CSPFT_XMODEL },
  { "hideTags", offsetof(WeaponDef, hideTags[0]), WFT_HIDETAGS },
  { "notetrackSoundMap", offsetof(WeaponDef, notetrackSoundMapKeys[0]), WFT_NOTETRACKSOUNDMAP },
  { "idleAnim", offsetof(WeaponDef, szXAnims[1]), CSPFT_STRING },
  { "emptyIdleAnim", offsetof(WeaponDef, szXAnims[2]), CSPFT_STRING },
  { "fireAnim", offsetof(WeaponDef, szXAnims[3]), CSPFT_STRING },
  { "holdFireAnim", offsetof(WeaponDef, szXAnims[4]), CSPFT_STRING },
  { "lastShotAnim", offsetof(WeaponDef, szXAnims[5]), CSPFT_STRING },
  { "detonateAnim", offsetof(WeaponDef, szXAnims[25]), CSPFT_STRING },
  { "rechamberAnim", offsetof(WeaponDef, szXAnims[6]), CSPFT_STRING },
  { "meleeAnim", offsetof(WeaponDef, szXAnims[7]), CSPFT_STRING },
  { "meleeChargeAnim", offsetof(WeaponDef, szXAnims[8]), CSPFT_STRING },
  { "reloadAnim", offsetof(WeaponDef, szXAnims[9]), CSPFT_STRING },
  { "reloadEmptyAnim", offsetof(WeaponDef, szXAnims[10]), CSPFT_STRING },
  { "reloadStartAnim", offsetof(WeaponDef, szXAnims[11]), CSPFT_STRING },
  { "reloadEndAnim", offsetof(WeaponDef, szXAnims[12]), CSPFT_STRING },
  { "raiseAnim", offsetof(WeaponDef, szXAnims[13]), CSPFT_STRING },
  { "dropAnim", offsetof(WeaponDef, szXAnims[15]), CSPFT_STRING },
  { "firstRaiseAnim", offsetof(WeaponDef, szXAnims[14]), CSPFT_STRING },
  { "altRaiseAnim", offsetof(WeaponDef, szXAnims[16]), CSPFT_STRING },
  { "altDropAnim", offsetof(WeaponDef, szXAnims[17]), CSPFT_STRING },
  { "quickRaiseAnim", offsetof(WeaponDef, szXAnims[18]), CSPFT_STRING },
  { "quickDropAnim", offsetof(WeaponDef, szXAnims[19]), CSPFT_STRING },
  { "emptyRaiseAnim", offsetof(WeaponDef, szXAnims[20]), CSPFT_STRING },
  { "emptyDropAnim", offsetof(WeaponDef, szXAnims[21]), CSPFT_STRING },
  { "sprintInAnim", offsetof(WeaponDef, szXAnims[22]), CSPFT_STRING },
  { "sprintLoopAnim", offsetof(WeaponDef, szXAnims[23]), CSPFT_STRING },
  { "sprintOutAnim", offsetof(WeaponDef, szXAnims[24]), CSPFT_STRING },
  { "nightVisionWearAnim", offsetof(WeaponDef, szXAnims[26]), CSPFT_STRING },
  { "nightVisionRemoveAnim", offsetof(WeaponDef, szXAnims[27]), CSPFT_STRING },
  { "adsFireAnim", offsetof(WeaponDef, szXAnims[28]), CSPFT_STRING },
  { "adsLastShotAnim", offsetof(WeaponDef, szXAnims[29]), CSPFT_STRING },
  { "adsRechamberAnim", offsetof(WeaponDef, szXAnims[30]), CSPFT_STRING },
  { "adsUpAnim", offsetof(WeaponDef, szXAnims[31]), CSPFT_STRING },
  { "adsDownAnim", offsetof(WeaponDef, szXAnims[32]), CSPFT_STRING },
  { "script", offsetof(WeaponDef, szScript), CSPFT_STRING },
  { "weaponType", offsetof(WeaponDef, weapType), WFT_WEAPONTYPE },
  { "weaponClass", offsetof(WeaponDef, weapClass), WFT_WEAPONCLASS },
  { "penetrateType", offsetof(WeaponDef, penetrateType), WFT_PENETRATE_TYPE },
  { "impactType", offsetof(WeaponDef, impactType), WFT_IMPACT_TYPE },
  { "inventoryType", offsetof(WeaponDef, inventoryType), WFT_INVENTORYTYPE },
  { "fireType", offsetof(WeaponDef, fireType), WFT_FIRETYPE },
  { "offhandClass", offsetof(WeaponDef, offhandClass), WFT_OFFHAND_CLASS },
  { "viewFlashEffect", offsetof(WeaponDef, viewFlashEffect), CSPFT_FX },
  { "worldFlashEffect", offsetof(WeaponDef, worldFlashEffect), CSPFT_FX },
  { "pickupSound", offsetof(WeaponDef, pickupSound), CSPFT_SOUND },
  { "pickupSoundPlayer", offsetof(WeaponDef, pickupSoundPlayer), CSPFT_SOUND },
  { "ammoPickupSound", offsetof(WeaponDef, ammoPickupSound), CSPFT_SOUND },
  { "ammoPickupSoundPlayer", offsetof(WeaponDef, ammoPickupSoundPlayer), CSPFT_SOUND },
  { "projectileSound", offsetof(WeaponDef, projectileSound), CSPFT_SOUND },
  { "pullbackSound", offsetof(WeaponDef, pullbackSound), CSPFT_SOUND },
  { "pullbackSoundPlayer", offsetof(WeaponDef, pullbackSoundPlayer), CSPFT_SOUND },
  { "fireSound", offsetof(WeaponDef, fireSound), CSPFT_SOUND },
  { "fireSoundPlayer", offsetof(WeaponDef, fireSoundPlayer), CSPFT_SOUND },
  { "loopFireSound", offsetof(WeaponDef, fireLoopSound), CSPFT_SOUND },
  { "loopFireSoundPlayer", offsetof(WeaponDef, fireLoopSoundPlayer), CSPFT_SOUND },
  { "stopFireSound", offsetof(WeaponDef, fireStopSound), CSPFT_SOUND },
  { "stopFireSoundPlayer", offsetof(WeaponDef, fireStopSoundPlayer), CSPFT_SOUND },
  { "lastShotSound", offsetof(WeaponDef, fireLastSound), CSPFT_SOUND },
  { "lastShotSoundPlayer", offsetof(WeaponDef, fireLastSoundPlayer), CSPFT_SOUND },
  { "emptyFireSound", offsetof(WeaponDef, emptyFireSound), CSPFT_SOUND },
  { "emptyFireSoundPlayer", offsetof(WeaponDef, emptyFireSoundPlayer), CSPFT_SOUND },
  { "meleeSwipeSound", offsetof(WeaponDef, meleeSwipeSound), CSPFT_SOUND },
  { "meleeSwipeSoundPlayer", offsetof(WeaponDef, meleeSwipeSoundPlayer), CSPFT_SOUND },
  { "meleeHitSound", offsetof(WeaponDef, meleeHitSound), CSPFT_SOUND },
  { "meleeMissSound", offsetof(WeaponDef, meleeMissSound), CSPFT_SOUND },
  { "rechamberSound", offsetof(WeaponDef, rechamberSound), CSPFT_SOUND },
  { "rechamberSoundPlayer", offsetof(WeaponDef, rechamberSoundPlayer), CSPFT_SOUND },
  { "reloadSound", offsetof(WeaponDef, reloadSound), CSPFT_SOUND },
  { "reloadSoundPlayer", offsetof(WeaponDef, reloadSoundPlayer), CSPFT_SOUND },
  { "reloadEmptySound", offsetof(WeaponDef, reloadEmptySound), CSPFT_SOUND },
  { "reloadEmptySoundPlayer", offsetof(WeaponDef, reloadEmptySoundPlayer), CSPFT_SOUND },
  { "reloadStartSound", offsetof(WeaponDef, reloadStartSound), CSPFT_SOUND },
  { "reloadStartSoundPlayer", offsetof(WeaponDef, reloadStartSoundPlayer), CSPFT_SOUND },
  { "reloadEndSound", offsetof(WeaponDef, reloadEndSound), CSPFT_SOUND },
  { "reloadEndSoundPlayer", offsetof(WeaponDef, reloadEndSoundPlayer), CSPFT_SOUND },
  { "detonateSound", offsetof(WeaponDef, detonateSound), CSPFT_SOUND },
  { "detonateSoundPlayer", offsetof(WeaponDef, detonateSoundPlayer), CSPFT_SOUND },
  { "nightVisionWearSound", offsetof(WeaponDef, nightVisionWearSound), CSPFT_SOUND },
  { "nightVisionWearSoundPlayer", offsetof(WeaponDef, nightVisionWearSoundPlayer), CSPFT_SOUND },
  { "nightVisionRemoveSound", offsetof(WeaponDef, nightVisionRemoveSound), CSPFT_SOUND },
  { "nightVisionRemoveSoundPlayer", offsetof(WeaponDef, nightVisionRemoveSoundPlayer), CSPFT_SOUND },
  { "raiseSound", offsetof(WeaponDef, raiseSound), CSPFT_SOUND },
  { "raiseSoundPlayer", offsetof(WeaponDef, raiseSoundPlayer), CSPFT_SOUND },
  { "firstRaiseSound", offsetof(WeaponDef, firstRaiseSound), CSPFT_SOUND },
  { "firstRaiseSoundPlayer", offsetof(WeaponDef, firstRaiseSoundPlayer), CSPFT_SOUND },
  { "altSwitchSound", offsetof(WeaponDef, altSwitchSound), CSPFT_SOUND },
  { "altSwitchSoundPlayer", offsetof(WeaponDef, altSwitchSoundPlayer), CSPFT_SOUND },
  { "putawaySound", offsetof(WeaponDef, putawaySound), CSPFT_SOUND },
  { "putawaySoundPlayer", offsetof(WeaponDef, putawaySoundPlayer), CSPFT_SOUND },
  { "bounceSound", offsetof(WeaponDef, bounceSound), WFT_BOUNCE_SOUND },
  { "viewShellEjectEffect", offsetof(WeaponDef, viewShellEjectEffect), CSPFT_FX },
  { "worldShellEjectEffect", offsetof(WeaponDef, worldShellEjectEffect), CSPFT_FX },
  { "viewLastShotEjectEffect", offsetof(WeaponDef, viewLastShotEjectEffect), CSPFT_FX },
  { "worldLastShotEjectEffect", offsetof(WeaponDef, worldLastShotEjectEffect), CSPFT_FX },
  { "reticleCenter", offsetof(WeaponDef, reticleCenter), CSPFT_MATERIAL },
  { "reticleSide", offsetof(WeaponDef, reticleSide), CSPFT_MATERIAL },
  { "reticleCenterSize", offsetof(WeaponDef, iReticleCenterSize), CSPFT_INT },
  { "reticleSideSize", offsetof(WeaponDef, iReticleSideSize), CSPFT_INT },
  { "reticleMinOfs", offsetof(WeaponDef, iReticleMinOfs), CSPFT_INT },
  { "activeReticleType", offsetof(WeaponDef, activeReticleType), WFT_ACTIVE_RETICLE_TYPE },
  { "standMoveF", offsetof(WeaponDef, vStandMove[0]), CSPFT_FLOAT },
  { "standMoveR", offsetof(WeaponDef, vStandMove[1]), CSPFT_FLOAT },
  { "standMoveU", offsetof(WeaponDef, vStandMove[2]), CSPFT_FLOAT },
  { "standRotP", offsetof(WeaponDef, vStandRot[0]), CSPFT_FLOAT },
  { "standRotY", offsetof(WeaponDef, vStandRot[1]), CSPFT_FLOAT },
  { "standRotR", offsetof(WeaponDef, vStandRot[2]), CSPFT_FLOAT },
  { "duckedOfsF", offsetof(WeaponDef, vDuckedOfs[0]), CSPFT_FLOAT },
  { "duckedOfsR", offsetof(WeaponDef, vDuckedOfs[1]), CSPFT_FLOAT },
  { "duckedOfsU", offsetof(WeaponDef, vDuckedOfs[2]), CSPFT_FLOAT },
  { "duckedMoveF", offsetof(WeaponDef, vDuckedMove[0]), CSPFT_FLOAT },
  { "duckedMoveR", offsetof(WeaponDef, vDuckedMove[1]), CSPFT_FLOAT },
  { "duckedMoveU", offsetof(WeaponDef, vDuckedMove[2]), CSPFT_FLOAT },
  { "duckedRotP", offsetof(WeaponDef, vDuckedRot[0]), CSPFT_FLOAT },
  { "duckedRotY", offsetof(WeaponDef, vDuckedRot[1]), CSPFT_FLOAT },
  { "duckedRotR", offsetof(WeaponDef, vDuckedRot[2]), CSPFT_FLOAT },
  { "proneOfsF", offsetof(WeaponDef, vProneOfs[0]), CSPFT_FLOAT },
  { "proneOfsR", offsetof(WeaponDef, vProneOfs[1]), CSPFT_FLOAT },
  { "proneOfsU", offsetof(WeaponDef, vProneOfs[2]), CSPFT_FLOAT },
  { "proneMoveF", offsetof(WeaponDef, vProneMove[0]), CSPFT_FLOAT },
  { "proneMoveR", offsetof(WeaponDef, vProneMove[1]), CSPFT_FLOAT },
  { "proneMoveU", offsetof(WeaponDef, vProneMove[2]), CSPFT_FLOAT },
  { "proneRotP", offsetof(WeaponDef, vProneRot[0]), CSPFT_FLOAT },
  { "proneRotY", offsetof(WeaponDef, vProneRot[1]), CSPFT_FLOAT },
  { "proneRotR", offsetof(WeaponDef, vProneRot[2]), CSPFT_FLOAT },
  { "posMoveRate", offsetof(WeaponDef, fPosMoveRate), CSPFT_FLOAT },
  { "posProneMoveRate", offsetof(WeaponDef, fPosProneMoveRate), CSPFT_FLOAT },
  { "standMoveMinSpeed", offsetof(WeaponDef, fStandMoveMinSpeed), CSPFT_FLOAT },
  { "duckedMoveMinSpeed", offsetof(WeaponDef, fDuckedMoveMinSpeed), CSPFT_FLOAT },
  { "proneMoveMinSpeed", offsetof(WeaponDef, fProneMoveMinSpeed), CSPFT_FLOAT },
  { "posRotRate", offsetof(WeaponDef, fPosRotRate), CSPFT_FLOAT },
  { "posProneRotRate", offsetof(WeaponDef, fPosProneRotRate), CSPFT_FLOAT },
  { "standRotMinSpeed", offsetof(WeaponDef, fStandRotMinSpeed), CSPFT_FLOAT },
  { "duckedRotMinSpeed", offsetof(WeaponDef, fDuckedRotMinSpeed), CSPFT_FLOAT },
  { "proneRotMinSpeed", offsetof(WeaponDef, fProneRotMinSpeed), CSPFT_FLOAT },
  { "worldModel", offsetof(WeaponDef, worldModel[0]), CSPFT_XMODEL },
  { "worldModel2", offsetof(WeaponDef, worldModel[1]), CSPFT_XMODEL },
  { "worldModel3", offsetof(WeaponDef, worldModel[2]), CSPFT_XMODEL },
  { "worldModel4", offsetof(WeaponDef, worldModel[3]), CSPFT_XMODEL },
  { "worldModel5", offsetof(WeaponDef, worldModel[4]), CSPFT_XMODEL },
  { "worldModel6", offsetof(WeaponDef, worldModel[5]), CSPFT_XMODEL },
  { "worldModel7", offsetof(WeaponDef, worldModel[6]), CSPFT_XMODEL },
  { "worldModel8", offsetof(WeaponDef, worldModel[7]), CSPFT_XMODEL },
  { "worldModel9", offsetof(WeaponDef, worldModel[8]), CSPFT_XMODEL },
  { "worldModel10", offsetof(WeaponDef, worldModel[9]), CSPFT_XMODEL },
  { "worldModel11", offsetof(WeaponDef, worldModel[10]), CSPFT_XMODEL },
  { "worldModel12", offsetof(WeaponDef, worldModel[11]), CSPFT_XMODEL },
  { "worldModel13", offsetof(WeaponDef, worldModel[12]), CSPFT_XMODEL },
  { "worldModel14", offsetof(WeaponDef, worldModel[13]), CSPFT_XMODEL },
  { "worldModel15", offsetof(WeaponDef, worldModel[14]), CSPFT_XMODEL },
  { "worldModel16", offsetof(WeaponDef, worldModel[15]), CSPFT_XMODEL },
  { "worldClipModel", offsetof(WeaponDef, worldClipModel), CSPFT_XMODEL },
  { "rocketModel", offsetof(WeaponDef, rocketModel), CSPFT_XMODEL },
  { "knifeModel", offsetof(WeaponDef, knifeModel), CSPFT_XMODEL },
  { "worldKnifeModel", offsetof(WeaponDef, worldKnifeModel), CSPFT_XMODEL },
  { "hudIcon", offsetof(WeaponDef, hudIcon), CSPFT_MATERIAL },
  { "hudIconRatio", offsetof(WeaponDef, hudIconRatio), WFT_ICONRATIO_HUD },
  { "ammoCounterIcon", offsetof(WeaponDef, ammoCounterIcon), CSPFT_MATERIAL },
  { "ammoCounterIconRatio", offsetof(WeaponDef, ammoCounterIconRatio), WFT_ICONRATIO_AMMOCOUNTER },
  { "ammoCounterClip", offsetof(WeaponDef, ammoCounterClip), WFT_AMMOCOUNTER_CLIPTYPE },
  { "startAmmo", offsetof(WeaponDef, iStartAmmo), CSPFT_INT },
  { "ammoName", offsetof(WeaponDef, szAmmoName), CSPFT_STRING },
  { "clipName", offsetof(WeaponDef, szClipName), CSPFT_STRING },
  { "maxAmmo", offsetof(WeaponDef, iMaxAmmo), CSPFT_INT },
  { "clipSize", offsetof(WeaponDef, iClipSize), CSPFT_INT },
  { "shotCount", offsetof(WeaponDef, shotCount), CSPFT_INT },
  { "sharedAmmoCapName", offsetof(WeaponDef, szSharedAmmoCapName), CSPFT_STRING },
  { "sharedAmmoCap", offsetof(WeaponDef, iSharedAmmoCap), CSPFT_INT },
  { "damage", offsetof(WeaponDef, damage), CSPFT_INT },
  { "playerDamage", offsetof(WeaponDef, playerDamage), CSPFT_INT },
  { "meleeDamage", offsetof(WeaponDef, iMeleeDamage), CSPFT_INT },
  { "minDamage", offsetof(WeaponDef, minDamage), CSPFT_INT },
  { "minPlayerDamage", offsetof(WeaponDef, minPlayerDamage), CSPFT_INT },
  { "maxDamageRange", offsetof(WeaponDef, fMaxDamageRange), CSPFT_FLOAT },
  { "minDamageRange", offsetof(WeaponDef, fMinDamageRange), CSPFT_FLOAT },
  { "destabilizationRateTime", offsetof(WeaponDef, destabilizationRateTime), CSPFT_FLOAT },
  { "destabilizationCurvatureMax", offsetof(WeaponDef, destabilizationCurvatureMax), CSPFT_FLOAT },
  { "destabilizeDistance", offsetof(WeaponDef, destabilizeDistance), CSPFT_INT },
  { "fireDelay", offsetof(WeaponDef, iFireDelay), CSPFT_MILLISECONDS },
  { "meleeDelay", offsetof(WeaponDef, iMeleeDelay), CSPFT_MILLISECONDS },
  { "meleeChargeDelay", offsetof(WeaponDef, meleeChargeDelay), CSPFT_MILLISECONDS },
  { "fireTime", offsetof(WeaponDef, iFireTime), CSPFT_MILLISECONDS },
  { "rechamberTime", offsetof(WeaponDef, iRechamberTime), CSPFT_MILLISECONDS },
  { "rechamberBoltTime", offsetof(WeaponDef, iRechamberBoltTime), CSPFT_MILLISECONDS },
  { "holdFireTime", offsetof(WeaponDef, iHoldFireTime), CSPFT_MILLISECONDS },
  { "detonateTime", offsetof(WeaponDef, iDetonateTime), CSPFT_MILLISECONDS },
  { "detonateDelay", offsetof(WeaponDef, iDetonateDelay), CSPFT_MILLISECONDS },
  { "meleeTime", offsetof(WeaponDef, iMeleeTime), CSPFT_MILLISECONDS },
  { "meleeChargeTime", offsetof(WeaponDef, meleeChargeTime), CSPFT_MILLISECONDS },
  { "reloadTime", offsetof(WeaponDef, iReloadTime), CSPFT_MILLISECONDS },
  { "reloadShowRocketTime", offsetof(WeaponDef, reloadShowRocketTime), CSPFT_MILLISECONDS },
  { "reloadEmptyTime", offsetof(WeaponDef, iReloadEmptyTime), CSPFT_MILLISECONDS },
  { "reloadAddTime", offsetof(WeaponDef, iReloadAddTime), CSPFT_MILLISECONDS },
  { "reloadStartTime", offsetof(WeaponDef, iReloadStartTime), CSPFT_MILLISECONDS },
  { "reloadStartAddTime", offsetof(WeaponDef, iReloadStartAddTime), CSPFT_MILLISECONDS },
  { "reloadEndTime", offsetof(WeaponDef, iReloadEndTime), CSPFT_MILLISECONDS },
  { "dropTime", offsetof(WeaponDef, iDropTime), CSPFT_MILLISECONDS },
  { "raiseTime", offsetof(WeaponDef, iRaiseTime), CSPFT_MILLISECONDS },
  { "altDropTime", offsetof(WeaponDef, iAltDropTime), CSPFT_MILLISECONDS },
  { "altRaiseTime", offsetof(WeaponDef, iAltRaiseTime), CSPFT_MILLISECONDS },
  { "quickDropTime", offsetof(WeaponDef, quickDropTime), CSPFT_MILLISECONDS },
  { "quickRaiseTime", offsetof(WeaponDef, quickRaiseTime), CSPFT_MILLISECONDS },
  { "firstRaiseTime", offsetof(WeaponDef, iFirstRaiseTime), CSPFT_MILLISECONDS },
  { "emptyRaiseTime", offsetof(WeaponDef, iEmptyRaiseTime), CSPFT_MILLISECONDS },
  { "emptyDropTime", offsetof(WeaponDef, iEmptyDropTime), CSPFT_MILLISECONDS },
  { "sprintInTime", offsetof(WeaponDef, sprintInTime), CSPFT_MILLISECONDS },
  { "sprintLoopTime", offsetof(WeaponDef, sprintLoopTime), CSPFT_MILLISECONDS },
  { "sprintOutTime", offsetof(WeaponDef, sprintOutTime), CSPFT_MILLISECONDS },
  { "nightVisionWearTime", offsetof(WeaponDef, nightVisionWearTime), CSPFT_MILLISECONDS },
  { "nightVisionWearTimeFadeOutEnd", offsetof(WeaponDef, nightVisionWearTimeFadeOutEnd), CSPFT_MILLISECONDS },
  { "nightVisionWearTimePowerUp", offsetof(WeaponDef, nightVisionWearTimePowerUp), CSPFT_MILLISECONDS },
  { "nightVisionRemoveTime", offsetof(WeaponDef, nightVisionRemoveTime), CSPFT_MILLISECONDS },
  { "nightVisionRemoveTimePowerDown", offsetof(WeaponDef, nightVisionRemoveTimePowerDown), CSPFT_MILLISECONDS },
  { "nightVisionRemoveTimeFadeInStart", offsetof(WeaponDef, nightVisionRemoveTimeFadeInStart), CSPFT_MILLISECONDS },
  { "fuseTime", offsetof(WeaponDef, fuseTime), CSPFT_MILLISECONDS },
  { "aifuseTime", offsetof(WeaponDef, aiFuseTime), CSPFT_MILLISECONDS },
  { "requireLockonToFire", offsetof(WeaponDef, requireLockonToFire), CSPFT_QBOOLEAN },
  { "noAdsWhenMagEmpty", offsetof(WeaponDef, noAdsWhenMagEmpty), CSPFT_QBOOLEAN },
  { "avoidDropCleanup", offsetof(WeaponDef, avoidDropCleanup), CSPFT_QBOOLEAN },
  { "autoAimRange", offsetof(WeaponDef, autoAimRange), CSPFT_FLOAT },
  { "aimAssistRange", offsetof(WeaponDef, aimAssistRange), CSPFT_FLOAT },
  { "aimAssistRangeAds", offsetof(WeaponDef, aimAssistRangeAds), CSPFT_FLOAT },
  { "aimPadding", offsetof(WeaponDef, aimPadding), CSPFT_FLOAT },
  { "enemyCrosshairRange", offsetof(WeaponDef, enemyCrosshairRange), CSPFT_FLOAT },
  { "crosshairColorChange", offsetof(WeaponDef, crosshairColorChange), CSPFT_QBOOLEAN },
  { "moveSpeedScale", offsetof(WeaponDef, moveSpeedScale), CSPFT_FLOAT },
  { "adsMoveSpeedScale", offsetof(WeaponDef, adsMoveSpeedScale), CSPFT_FLOAT },
  { "sprintDurationScale", offsetof(WeaponDef, sprintDurationScale), CSPFT_FLOAT },
  { "idleCrouchFactor", offsetof(WeaponDef, fIdleCrouchFactor), CSPFT_FLOAT },
  { "idleProneFactor", offsetof(WeaponDef, fIdleProneFactor), CSPFT_FLOAT },
  { "gunMaxPitch", offsetof(WeaponDef, fGunMaxPitch), CSPFT_FLOAT },
  { "gunMaxYaw", offsetof(WeaponDef, fGunMaxYaw), CSPFT_FLOAT },
  { "swayMaxAngle", offsetof(WeaponDef, swayMaxAngle), CSPFT_FLOAT },
  { "swayLerpSpeed", offsetof(WeaponDef, swayLerpSpeed), CSPFT_FLOAT },
  { "swayPitchScale", offsetof(WeaponDef, swayPitchScale), CSPFT_FLOAT },
  { "swayYawScale", offsetof(WeaponDef, swayYawScale), CSPFT_FLOAT },
  { "swayHorizScale", offsetof(WeaponDef, swayHorizScale), CSPFT_FLOAT },
  { "swayVertScale", offsetof(WeaponDef, swayVertScale), CSPFT_FLOAT },
  { "swayShellShockScale", offsetof(WeaponDef, swayShellShockScale), CSPFT_FLOAT },
  { "adsSwayMaxAngle", offsetof(WeaponDef, adsSwayMaxAngle), CSPFT_FLOAT },
  { "adsSwayLerpSpeed", offsetof(WeaponDef, adsSwayLerpSpeed), CSPFT_FLOAT },
  { "adsSwayPitchScale", offsetof(WeaponDef, adsSwayPitchScale), CSPFT_FLOAT },
  { "adsSwayYawScale", offsetof(WeaponDef, adsSwayYawScale), CSPFT_FLOAT },
  { "adsSwayHorizScale", offsetof(WeaponDef, adsSwayHorizScale), CSPFT_FLOAT },
  { "adsSwayVertScale", offsetof(WeaponDef, adsSwayVertScale), CSPFT_FLOAT },
  { "rifleBullet", offsetof(WeaponDef, bRifleBullet), CSPFT_QBOOLEAN },
  { "armorPiercing", offsetof(WeaponDef, armorPiercing), CSPFT_QBOOLEAN },
  { "boltAction", offsetof(WeaponDef, bBoltAction), CSPFT_QBOOLEAN },
  { "aimDownSight", offsetof(WeaponDef, aimDownSight), CSPFT_QBOOLEAN },
  { "rechamberWhileAds", offsetof(WeaponDef, bRechamberWhileAds), CSPFT_QBOOLEAN },
  { "adsViewErrorMin", offsetof(WeaponDef, adsViewErrorMin), CSPFT_FLOAT },
  { "adsViewErrorMax", offsetof(WeaponDef, adsViewErrorMax), CSPFT_FLOAT },
  { "clipOnly", offsetof(WeaponDef, bClipOnly), CSPFT_QBOOLEAN },
  { "cookOffHold", offsetof(WeaponDef, bCookOffHold), CSPFT_QBOOLEAN },
  { "adsFire", offsetof(WeaponDef, adsFireOnly), CSPFT_QBOOLEAN },
  { "cancelAutoHolsterWhenEmpty", offsetof(WeaponDef, cancelAutoHolsterWhenEmpty), CSPFT_QBOOLEAN },
  { "suppressAmmoReserveDisplay", offsetof(WeaponDef, suppressAmmoReserveDisplay), CSPFT_QBOOLEAN },
  { "enhanced", offsetof(WeaponDef, enhanced), CSPFT_QBOOLEAN },
  { "laserSightDuringNightvision", offsetof(WeaponDef, laserSightDuringNightvision), CSPFT_QBOOLEAN },
  { "killIcon", offsetof(WeaponDef, killIcon), CSPFT_MATERIAL },
  { "killIconRatio", offsetof(WeaponDef, killIconRatio), WFT_ICONRATIO_KILL },
  { "flipKillIcon", offsetof(WeaponDef, flipKillIcon), CSPFT_QBOOLEAN },
  { "dpadIcon", offsetof(WeaponDef, dpadIcon), CSPFT_MATERIAL },
  { "dpadIconRatio", offsetof(WeaponDef, dpadIconRatio), WFT_ICONRATIO_DPAD },
  { "noPartialReload", offsetof(WeaponDef, bNoPartialReload), CSPFT_QBOOLEAN },
  { "segmentedReload", offsetof(WeaponDef, bSegmentedReload), CSPFT_QBOOLEAN },
  { "reloadAmmoAdd", offsetof(WeaponDef, iReloadAmmoAdd), CSPFT_INT },
  { "reloadStartAdd", offsetof(WeaponDef, iReloadStartAdd), CSPFT_INT },
  { "altWeapon", offsetof(WeaponDef, szAltWeaponName), CSPFT_STRING },
  { "dropAmmoMin", offsetof(WeaponDef, iDropAmmoMin), CSPFT_INT },
  { "dropAmmoMax", offsetof(WeaponDef, iDropAmmoMax), CSPFT_INT },
  { "blocksProne", offsetof(WeaponDef, blocksProne), CSPFT_QBOOLEAN },
  { "silenced", offsetof(WeaponDef, silenced), CSPFT_QBOOLEAN },
  { "explosionRadius", offsetof(WeaponDef, iExplosionRadius), CSPFT_INT },
  { "explosionRadiusMin", offsetof(WeaponDef, iExplosionRadiusMin), CSPFT_INT },
  { "explosionInnerDamage", offsetof(WeaponDef, iExplosionInnerDamage), CSPFT_INT },
  { "explosionOuterDamage", offsetof(WeaponDef, iExplosionOuterDamage), CSPFT_INT },
  { "damageConeAngle", offsetof(WeaponDef, damageConeAngle), CSPFT_FLOAT },
  { "projectileSpeed", offsetof(WeaponDef, iProjectileSpeed), CSPFT_INT },
  { "projectileSpeedUp", offsetof(WeaponDef, iProjectileSpeedUp), CSPFT_INT },
  { "projectileSpeedForward", offsetof(WeaponDef, iProjectileSpeedForward), CSPFT_INT },
  { "projectileActivateDist", offsetof(WeaponDef, iProjectileActivateDist), CSPFT_INT },
  { "projectileLifetime", offsetof(WeaponDef, projLifetime), CSPFT_FLOAT },
  { "timeToAccelerate", offsetof(WeaponDef, timeToAccelerate), CSPFT_FLOAT },
  { "projectileCurvature", offsetof(WeaponDef, projectileCurvature), CSPFT_FLOAT },
  { "projectileModel", offsetof(WeaponDef, projectileModel), CSPFT_XMODEL },
  { "projExplosionType", offsetof(WeaponDef, projExplosion), WFT_PROJ_EXPLOSION },
  { "projExplosionEffect", offsetof(WeaponDef, projExplosionEffect), CSPFT_FX },
  { "projExplosionEffectForceNormalUp", offsetof(WeaponDef, projExplosionEffectForceNormalUp), CSPFT_QBOOLEAN },
  { "projExplosionSound", offsetof(WeaponDef, projExplosionSound), CSPFT_SOUND },
  { "projDudEffect", offsetof(WeaponDef, projDudEffect), CSPFT_FX },
  { "projDudSound", offsetof(WeaponDef, projDudSound), CSPFT_SOUND },
  { "projImpactExplode", offsetof(WeaponDef, bProjImpactExplode), CSPFT_QBOOLEAN },
  { "stickiness", offsetof(WeaponDef, stickiness), WFT_STICKINESS },
  { "hasDetonator", offsetof(WeaponDef, hasDetonator), CSPFT_QBOOLEAN },
  { "timedDetonation", offsetof(WeaponDef, timedDetonation), CSPFT_QBOOLEAN },
  { "rotate", offsetof(WeaponDef, rotate), CSPFT_QBOOLEAN },
  { "holdButtonToThrow", offsetof(WeaponDef, holdButtonToThrow), CSPFT_QBOOLEAN },
  { "freezeMovementWhenFiring", offsetof(WeaponDef, freezeMovementWhenFiring), CSPFT_QBOOLEAN },
  { "lowAmmoWarningThreshold", offsetof(WeaponDef, lowAmmoWarningThreshold), CSPFT_FLOAT },
  { "parallelDefaultBounce", offsetof(WeaponDef, parallelBounce[0]), CSPFT_FLOAT },
  { "parallelBarkBounce", offsetof(WeaponDef, parallelBounce[1]), CSPFT_FLOAT },
  { "parallelBrickBounce", offsetof(WeaponDef, parallelBounce[2]), CSPFT_FLOAT },
  { "parallelCarpetBounce", offsetof(WeaponDef, parallelBounce[3]), CSPFT_FLOAT },
  { "parallelClothBounce", offsetof(WeaponDef, parallelBounce[4]), CSPFT_FLOAT },
  { "parallelConcreteBounce", offsetof(WeaponDef, parallelBounce[5]), CSPFT_FLOAT },
  { "parallelDirtBounce", offsetof(WeaponDef, parallelBounce[6]), CSPFT_FLOAT },
  { "parallelFleshBounce", offsetof(WeaponDef, parallelBounce[7]), CSPFT_FLOAT },
  { "parallelFoliageBounce", offsetof(WeaponDef, parallelBounce[8]), CSPFT_FLOAT },
  { "parallelGlassBounce", offsetof(WeaponDef, parallelBounce[9]), CSPFT_FLOAT },
  { "parallelGrassBounce", offsetof(WeaponDef, parallelBounce[10]), CSPFT_FLOAT },
  { "parallelGravelBounce", offsetof(WeaponDef, parallelBounce[11]), CSPFT_FLOAT },
  { "parallelIceBounce", offsetof(WeaponDef, parallelBounce[12]), CSPFT_FLOAT },
  { "parallelMetalBounce", offsetof(WeaponDef, parallelBounce[13]), CSPFT_FLOAT },
  { "parallelMudBounce", offsetof(WeaponDef, parallelBounce[14]), CSPFT_FLOAT },
  { "parallelPaperBounce", offsetof(WeaponDef, parallelBounce[15]), CSPFT_FLOAT },
  { "parallelPlasterBounce", offsetof(WeaponDef, parallelBounce[16]), CSPFT_FLOAT },
  { "parallelRockBounce", offsetof(WeaponDef, parallelBounce[17]), CSPFT_FLOAT },
  { "parallelSandBounce", offsetof(WeaponDef, parallelBounce[18]), CSPFT_FLOAT },
  { "parallelSnowBounce", offsetof(WeaponDef, parallelBounce[19]), CSPFT_FLOAT },
  { "parallelWaterBounce", offsetof(WeaponDef, parallelBounce[20]), CSPFT_FLOAT },
  { "parallelWoodBounce", offsetof(WeaponDef, parallelBounce[21]), CSPFT_FLOAT },
  { "parallelAsphaltBounce", offsetof(WeaponDef, parallelBounce[22]), CSPFT_FLOAT },
  { "parallelCeramicBounce", offsetof(WeaponDef, parallelBounce[23]), CSPFT_FLOAT },
  { "parallelPlasticBounce", offsetof(WeaponDef, parallelBounce[24]), CSPFT_FLOAT },
  { "parallelRubberBounce", offsetof(WeaponDef, parallelBounce[25]), CSPFT_FLOAT },
  { "parallelCushionBounce", offsetof(WeaponDef, parallelBounce[26]), CSPFT_FLOAT },
  { "parallelFruitBounce", offsetof(WeaponDef, parallelBounce[27]), CSPFT_FLOAT },
  { "parallelPaintedMetalBounce", offsetof(WeaponDef, parallelBounce[28]), CSPFT_FLOAT },
  { "perpendicularDefaultBounce", offsetof(WeaponDef, perpendicularBounce[0]), CSPFT_FLOAT },
  { "perpendicularBarkBounce", offsetof(WeaponDef, perpendicularBounce[1]), CSPFT_FLOAT },
  { "perpendicularBrickBounce", offsetof(WeaponDef, perpendicularBounce[2]), CSPFT_FLOAT },
  { "perpendicularCarpetBounce", offsetof(WeaponDef, perpendicularBounce[3]), CSPFT_FLOAT },
  { "perpendicularClothBounce", offsetof(WeaponDef, perpendicularBounce[4]), CSPFT_FLOAT },
  { "perpendicularConcreteBounce", offsetof(WeaponDef, perpendicularBounce[5]), CSPFT_FLOAT },
  { "perpendicularDirtBounce", offsetof(WeaponDef, perpendicularBounce[6]), CSPFT_FLOAT },
  { "perpendicularFleshBounce", offsetof(WeaponDef, perpendicularBounce[7]), CSPFT_FLOAT },
  { "perpendicularFoliageBounce", offsetof(WeaponDef, perpendicularBounce[8]), CSPFT_FLOAT },
  { "perpendicularGlassBounce", offsetof(WeaponDef, perpendicularBounce[9]), CSPFT_FLOAT },
  { "perpendicularGrassBounce", offsetof(WeaponDef, perpendicularBounce[10]), CSPFT_FLOAT },
  { "perpendicularGravelBounce", offsetof(WeaponDef, perpendicularBounce[11]), CSPFT_FLOAT },
  { "perpendicularIceBounce", offsetof(WeaponDef, perpendicularBounce[12]), CSPFT_FLOAT },
  { "perpendicularMetalBounce", offsetof(WeaponDef, perpendicularBounce[13]), CSPFT_FLOAT },
  { "perpendicularMudBounce", offsetof(WeaponDef, perpendicularBounce[14]), CSPFT_FLOAT },
  { "perpendicularPaperBounce", offsetof(WeaponDef, perpendicularBounce[15]), CSPFT_FLOAT },
  { "perpendicularPlasterBounce", offsetof(WeaponDef, perpendicularBounce[16]), CSPFT_FLOAT },
  { "perpendicularRockBounce", offsetof(WeaponDef, perpendicularBounce[17]), CSPFT_FLOAT },
  { "perpendicularSandBounce", offsetof(WeaponDef, perpendicularBounce[18]), CSPFT_FLOAT },
  { "perpendicularSnowBounce", offsetof(WeaponDef, perpendicularBounce[19]), CSPFT_FLOAT },
  { "perpendicularWaterBounce", offsetof(WeaponDef, perpendicularBounce[20]), CSPFT_FLOAT },
  { "perpendicularWoodBounce", offsetof(WeaponDef, perpendicularBounce[21]), CSPFT_FLOAT },
  { "perpendicularAsphaltBounce", offsetof(WeaponDef, perpendicularBounce[22]), CSPFT_FLOAT },
  { "perpendicularCeramicBounce", offsetof(WeaponDef, parallelBounce[23]), CSPFT_FLOAT },
  { "perpendicularPlasticBounce", offsetof(WeaponDef, parallelBounce[24]), CSPFT_FLOAT },
  { "perpendicularRubberBounce", offsetof(WeaponDef, parallelBounce[25]), CSPFT_FLOAT },
  { "perpendicularCushionBounce", offsetof(WeaponDef, perpendicularBounce[26]), CSPFT_FLOAT },
  { "perpendicularFruitBounce", offsetof(WeaponDef, perpendicularBounce[27]), CSPFT_FLOAT },
  { "perpendicularPaintedMetalBounce", offsetof(WeaponDef, perpendicularBounce[28]), CSPFT_FLOAT },
  { "projTrailEffect", offsetof(WeaponDef, projTrailEffect), CSPFT_FX },
  { "projectileRed", offsetof(WeaponDef, vProjectileColor[0]), CSPFT_FLOAT },
  { "projectileGreen", offsetof(WeaponDef, vProjectileColor[1]), CSPFT_FLOAT },
  { "projectileBlue", offsetof(WeaponDef, vProjectileColor[2]), CSPFT_FLOAT },
  { "guidedMissileType", offsetof(WeaponDef, guidedMissileType), WFT_GUIDED_MISSILE_TYPE },
  { "maxSteeringAccel", offsetof(WeaponDef, maxSteeringAccel), CSPFT_FLOAT },
  { "projIgnitionDelay", offsetof(WeaponDef, projIgnitionDelay), CSPFT_INT },
  { "projIgnitionEffect", offsetof(WeaponDef, projIgnitionEffect), CSPFT_FX },
  { "projIgnitionSound", offsetof(WeaponDef, projIgnitionSound), CSPFT_SOUND },
  { "adsTransInTime", offsetof(WeaponDef, iAdsTransInTime), CSPFT_MILLISECONDS },
  { "adsTransOutTime", offsetof(WeaponDef, iAdsTransOutTime), CSPFT_MILLISECONDS },
  { "adsIdleAmount", offsetof(WeaponDef, fAdsIdleAmount), CSPFT_FLOAT },
  { "adsIdleSpeed", offsetof(WeaponDef, adsIdleSpeed), CSPFT_FLOAT },
  { "adsZoomFov", offsetof(WeaponDef, fAdsZoomFov), CSPFT_FLOAT },
  { "adsZoomInFrac", offsetof(WeaponDef, fAdsZoomInFrac), CSPFT_FLOAT },
  { "adsZoomOutFrac", offsetof(WeaponDef, fAdsZoomOutFrac), CSPFT_FLOAT },
  { "adsOverlayShader", offsetof(WeaponDef, overlayMaterial), CSPFT_MATERIAL },
  { "adsOverlayShaderLowRes", offsetof(WeaponDef, overlayMaterialLowRes), CSPFT_MATERIAL },
  { "adsOverlayReticle", offsetof(WeaponDef, overlayReticle), WFT_OVERLAYRETICLE },
  { "adsOverlayInterface", offsetof(WeaponDef, overlayInterface), WFT_OVERLAYINTERFACE },
  { "adsOverlayWidth", offsetof(WeaponDef, overlayWidth), CSPFT_FLOAT },
  { "adsOverlayHeight", offsetof(WeaponDef, overlayHeight), CSPFT_FLOAT },
  { "adsBobFactor", offsetof(WeaponDef, fAdsBobFactor), CSPFT_FLOAT },
  { "adsViewBobMult", offsetof(WeaponDef, fAdsViewBobMult), CSPFT_FLOAT },
  { "adsAimPitch", offsetof(WeaponDef, fAdsAimPitch), CSPFT_FLOAT },
  { "adsCrosshairInFrac", offsetof(WeaponDef, fAdsCrosshairInFrac), CSPFT_FLOAT },
  { "adsCrosshairOutFrac", offsetof(WeaponDef, fAdsCrosshairOutFrac), CSPFT_FLOAT },
  { "adsReloadTransTime", offsetof(WeaponDef, iPositionReloadTransTime), CSPFT_MILLISECONDS },
  { "adsGunKickReducedKickBullets", offsetof(WeaponDef, adsGunKickReducedKickBullets), CSPFT_INT },
  { "adsGunKickReducedKickPercent", offsetof(WeaponDef, adsGunKickReducedKickPercent), CSPFT_FLOAT },
  { "adsGunKickPitchMin", offsetof(WeaponDef, fAdsGunKickPitchMin), CSPFT_FLOAT },
  { "adsGunKickPitchMax", offsetof(WeaponDef, fAdsGunKickPitchMax), CSPFT_FLOAT },
  { "adsGunKickYawMin", offsetof(WeaponDef, fAdsGunKickYawMin), CSPFT_FLOAT },
  { "adsGunKickYawMax", offsetof(WeaponDef, fAdsGunKickYawMax), CSPFT_FLOAT },
  { "adsGunKickAccel", offsetof(WeaponDef, fAdsGunKickAccel), CSPFT_FLOAT },
  { "adsGunKickSpeedMax", offsetof(WeaponDef, fAdsGunKickSpeedMax), CSPFT_FLOAT },
  { "adsGunKickSpeedDecay", offsetof(WeaponDef, fAdsGunKickSpeedDecay), CSPFT_FLOAT },
  { "adsGunKickStaticDecay", offsetof(WeaponDef, fAdsGunKickStaticDecay), CSPFT_FLOAT },
  { "adsViewKickPitchMin", offsetof(WeaponDef, fAdsViewKickPitchMin), CSPFT_FLOAT },
  { "adsViewKickPitchMax", offsetof(WeaponDef, fAdsViewKickPitchMax), CSPFT_FLOAT },
  { "adsViewKickYawMin", offsetof(WeaponDef, fAdsViewKickYawMin), CSPFT_FLOAT },
  { "adsViewKickYawMax", offsetof(WeaponDef, fAdsViewKickYawMax), CSPFT_FLOAT },
  { "adsViewKickCenterSpeed", offsetof(WeaponDef, fAdsViewKickCenterSpeed), CSPFT_FLOAT },
  { "adsSpread", offsetof(WeaponDef, fAdsSpread), CSPFT_FLOAT },
  { "guidedMissileType", offsetof(WeaponDef, guidedMissileType), WFT_GUIDED_MISSILE_TYPE },
  { "hipSpreadStandMin", offsetof(WeaponDef, fHipSpreadStandMin), CSPFT_FLOAT },
  { "hipSpreadDuckedMin", offsetof(WeaponDef, fHipSpreadDuckedMin), CSPFT_FLOAT },
  { "hipSpreadProneMin", offsetof(WeaponDef, fHipSpreadProneMin), CSPFT_FLOAT },
  { "hipSpreadMax", offsetof(WeaponDef, hipSpreadStandMax), CSPFT_FLOAT },
  { "hipSpreadDuckedMax", offsetof(WeaponDef, hipSpreadDuckedMax), CSPFT_FLOAT },
  { "hipSpreadProneMax", offsetof(WeaponDef, hipSpreadProneMax), CSPFT_FLOAT },
  { "hipSpreadDecayRate", offsetof(WeaponDef, fHipSpreadDecayRate), CSPFT_FLOAT },
  { "hipSpreadFireAdd", offsetof(WeaponDef, fHipSpreadFireAdd), CSPFT_FLOAT },
  { "hipSpreadTurnAdd", offsetof(WeaponDef, fHipSpreadTurnAdd), CSPFT_FLOAT },
  { "hipSpreadMoveAdd", offsetof(WeaponDef, fHipSpreadMoveAdd), CSPFT_FLOAT },
  { "hipSpreadDuckedDecay", offsetof(WeaponDef, fHipSpreadDuckedDecay), CSPFT_FLOAT },
  { "hipSpreadProneDecay", offsetof(WeaponDef, fHipSpreadProneDecay), CSPFT_FLOAT },
  { "hipReticleSidePos", offsetof(WeaponDef, fHipReticleSidePos), CSPFT_FLOAT },
  { "hipIdleAmount", offsetof(WeaponDef, fHipIdleAmount), CSPFT_FLOAT },
  { "hipIdleSpeed", offsetof(WeaponDef, hipIdleSpeed), CSPFT_FLOAT },
  { "hipGunKickReducedKickBullets", offsetof(WeaponDef, hipGunKickReducedKickBullets), CSPFT_INT },
  { "hipGunKickReducedKickPercent", offsetof(WeaponDef, hipGunKickReducedKickPercent), CSPFT_FLOAT },
  { "hipGunKickPitchMin", offsetof(WeaponDef, fHipGunKickPitchMin), CSPFT_FLOAT },
  { "hipGunKickPitchMax", offsetof(WeaponDef, fHipGunKickPitchMax), CSPFT_FLOAT },
  { "hipGunKickYawMin", offsetof(WeaponDef, fHipGunKickYawMin), CSPFT_FLOAT },
  { "hipGunKickYawMax", offsetof(WeaponDef, fHipGunKickYawMax), CSPFT_FLOAT },
  { "hipGunKickAccel", offsetof(WeaponDef, fHipGunKickAccel), CSPFT_FLOAT },
  { "hipGunKickSpeedMax", offsetof(WeaponDef, fHipGunKickSpeedMax), CSPFT_FLOAT },
  { "hipGunKickSpeedDecay", offsetof(WeaponDef, fHipGunKickSpeedDecay), CSPFT_FLOAT },
  { "hipGunKickStaticDecay", offsetof(WeaponDef, fHipGunKickStaticDecay), CSPFT_FLOAT },
  { "hipViewKickPitchMin", offsetof(WeaponDef, fHipViewKickPitchMin), CSPFT_FLOAT },
  { "hipViewKickPitchMax", offsetof(WeaponDef, fHipViewKickPitchMax), CSPFT_FLOAT },
  { "hipViewKickYawMin", offsetof(WeaponDef, fHipViewKickYawMin), CSPFT_FLOAT },
  { "hipViewKickYawMax", offsetof(WeaponDef, fHipViewKickYawMax), CSPFT_FLOAT },
  { "hipViewKickCenterSpeed", offsetof(WeaponDef, fHipViewKickCenterSpeed), CSPFT_FLOAT },
  { "leftArc", offsetof(WeaponDef, leftArc), CSPFT_FLOAT },
  { "rightArc", offsetof(WeaponDef, rightArc), CSPFT_FLOAT },
  { "topArc", offsetof(WeaponDef, topArc), CSPFT_FLOAT },
  { "bottomArc", offsetof(WeaponDef, bottomArc), CSPFT_FLOAT },
  { "accuracy", offsetof(WeaponDef, accuracy), CSPFT_FLOAT },
  { "aiSpread", offsetof(WeaponDef, aiSpread), CSPFT_FLOAT },
  { "playerSpread", offsetof(WeaponDef, playerSpread), CSPFT_FLOAT },
  { "maxVertTurnSpeed", offsetof(WeaponDef, maxTurnSpeed[0]), CSPFT_FLOAT },
  { "maxHorTurnSpeed", offsetof(WeaponDef, maxTurnSpeed[1]), CSPFT_FLOAT },
  { "minVertTurnSpeed", offsetof(WeaponDef, minTurnSpeed[0]), CSPFT_FLOAT },
  { "minHorTurnSpeed", offsetof(WeaponDef, minTurnSpeed[1]), CSPFT_FLOAT },
  { "pitchConvergenceTime", offsetof(WeaponDef, pitchConvergenceTime), CSPFT_FLOAT },
  { "yawConvergenceTime", offsetof(WeaponDef, yawConvergenceTime), CSPFT_FLOAT },
  { "suppressionTime", offsetof(WeaponDef, suppressTime), CSPFT_FLOAT },
  { "maxRange", offsetof(WeaponDef, maxRange), CSPFT_FLOAT },
  { "animHorRotateInc", offsetof(WeaponDef, fAnimHorRotateInc), CSPFT_FLOAT },
  { "playerPositionDist", offsetof(WeaponDef, fPlayerPositionDist), CSPFT_FLOAT },
  { "stance", offsetof(WeaponDef, stance), WFT_STANCE },
  { "useHintString", offsetof(WeaponDef, szUseHintString), CSPFT_STRING },
  { "dropHintString", offsetof(WeaponDef, dropHintString), CSPFT_STRING },
  { "horizViewJitter", offsetof(WeaponDef, horizViewJitter), CSPFT_FLOAT },
  { "vertViewJitter", offsetof(WeaponDef, vertViewJitter), CSPFT_FLOAT },
  { "fightDist", offsetof(WeaponDef, fightDist), CSPFT_FLOAT },
  { "maxDist", offsetof(WeaponDef, maxDist), CSPFT_FLOAT },
  { "aiVsAiAccuracyGraph", offsetof(WeaponDef, accuracyGraphName[0]), CSPFT_STRING },
  { "aiVsPlayerAccuracyGraph", offsetof(WeaponDef, accuracyGraphName[1]), CSPFT_STRING },
  { "locNone", offsetof(WeaponDef, locationDamageMultipliers[0]), CSPFT_FLOAT },
  { "locHelmet", offsetof(WeaponDef, locationDamageMultipliers[1]), CSPFT_FLOAT },
  { "locHead", offsetof(WeaponDef, locationDamageMultipliers[2]), CSPFT_FLOAT },
  { "locNeck", offsetof(WeaponDef, locationDamageMultipliers[3]), CSPFT_FLOAT },
  { "locTorsoUpper", offsetof(WeaponDef, locationDamageMultipliers[4]), CSPFT_FLOAT },
  { "locTorsoLower", offsetof(WeaponDef, locationDamageMultipliers[5]), CSPFT_FLOAT },
  { "locRightArmUpper", offsetof(WeaponDef, locationDamageMultipliers[6]), CSPFT_FLOAT },
  { "locRightArmLower", offsetof(WeaponDef, locationDamageMultipliers[8]), CSPFT_FLOAT },
  { "locRightHand", offsetof(WeaponDef, locationDamageMultipliers[10]), CSPFT_FLOAT },
  { "locLeftArmUpper", offsetof(WeaponDef, locationDamageMultipliers[7]), CSPFT_FLOAT },
  { "locLeftArmLower", offsetof(WeaponDef, locationDamageMultipliers[9]), CSPFT_FLOAT },
  { "locLeftHand", offsetof(WeaponDef, locationDamageMultipliers[11]), CSPFT_FLOAT },
  { "locRightLegUpper", offsetof(WeaponDef, locationDamageMultipliers[12]), CSPFT_FLOAT },
  { "locRightLegLower", offsetof(WeaponDef, locationDamageMultipliers[14]), CSPFT_FLOAT },
  { "locRightFoot", offsetof(WeaponDef, locationDamageMultipliers[16]), CSPFT_FLOAT },
  { "locLeftLegUpper", offsetof(WeaponDef, locationDamageMultipliers[13]), CSPFT_FLOAT },
  { "locLeftLegLower", offsetof(WeaponDef, locationDamageMultipliers[15]), CSPFT_FLOAT },
  { "locLeftFoot", offsetof(WeaponDef, locationDamageMultipliers[17]), CSPFT_FLOAT },
  { "locGun", offsetof(WeaponDef, locationDamageMultipliers[18]), CSPFT_FLOAT },
  { "fireRumble", offsetof(WeaponDef, fireRumble), CSPFT_STRING },
  { "meleeImpactRumble", offsetof(WeaponDef, meleeImpactRumble), CSPFT_STRING },
  { "adsDofStart", offsetof(WeaponDef, adsDofStart), CSPFT_FLOAT },
  { "adsDofEnd", offsetof(WeaponDef, adsDofEnd), CSPFT_FLOAT }
}; // idb

// const char *szWeapTypeNames[4] = { "bullet", "grenade", "projectile", "binoculars" }; // idb
const char *szWeapClassNames[10] =
{
  "rifle",
  "mg",
  "smg",
  "spread",
  "pistol",
  "grenade",
  "rocketlauncher",
  "turret",
  "non-player",
  "item"
}; // idb

char *g_playerAnimTypeNames[64];

WeaponDef bg_defaultWeaponDefs;

char *__cdecl BG_GetPlayerAnimTypeName(int index)
{
    return g_playerAnimTypeNames[index];
}

void __cdecl TRACK_bg_weapons_load_obj()
{
    track_static_alloc_internal(szWeapOverlayReticleNames, sizeof(szWeapOverlayReticleNames), "szWeapOverlayReticleNames", 9);
    track_static_alloc_internal(szWeapStanceNames, sizeof(szWeapStanceNames), "szWeapStanceNames", 9);
    track_static_alloc_internal(weaponDefFields, sizeof(weaponDefFields), "weaponDefFields", 9);
    track_static_alloc_internal(&bg_defaultWeaponDefs, sizeof(bg_defaultWeaponDefs), "bg_defaultWeaponDefs", 9);
    track_static_alloc_internal(penetrateTypeNames, sizeof(penetrateTypeNames), "penetrateTypeNames", 9);
    track_static_alloc_internal(szWeapTypeNames, sizeof(szWeapTypeNames), "szWeapTypeNames", 9);
    track_static_alloc_internal(szWeapClassNames, sizeof(szWeapClassNames), "szWeapClassNames", 9);
    track_static_alloc_internal(g_playerAnimTypeNames, sizeof(g_playerAnimTypeNames), "g_playerAnimTypeNames", 9);
    track_static_alloc_internal(szWeapInventoryTypeNames, sizeof(szWeapInventoryTypeNames), "szWeapInventoryTypeNames", 9);
}

const char *__cdecl BG_GetWeaponTypeName(weapType_t type)
{
    bcassert(type < WEAPTYPE_NUM, ARRAY_COUNT(szWeapTypeNames));

    return szWeapTypeNames[type];
}

const char *__cdecl BG_GetWeaponClassName(weapClass_t type)
{
    bcassert(type < WEAPCLASS_NUM, ARRAY_COUNT(szWeapClassNames));

    return szWeapClassNames[type];
}

const char *__cdecl BG_GetWeaponInventoryTypeName(weapInventoryType_t type)
{
    bcassert(type < WEAPINVENTORYCOUNT, ARRAY_COUNT(szWeapInventoryTypeNames));

    return szWeapInventoryTypeNames[type];
}

#ifdef KISAK_MP
void __cdecl BG_LoadWeaponStrings()
{
    uint i; // [esp+0h] [ebp-4h]

    for (i = 0; i < g_playerAnimTypeNamesCount; ++i)
        BG_InitWeaponString(i, g_playerAnimTypeNames[i]);
}
#endif

void __cdecl BG_LoadPlayerAnimTypes()
{
#ifdef KISAK_MP
    char v0; // [esp+3h] [ebp-29h]
    char *v1; // [esp+8h] [ebp-24h]
    const char *v2; // [esp+Ch] [ebp-20h]
    char *buf; // [esp+20h] [ebp-Ch]
    const char *text_p; // [esp+24h] [ebp-8h] BYREF
    const char *token; // [esp+28h] [ebp-4h]

    g_playerAnimTypeNamesCount = 0;
    buf = Com_LoadRawTextFile("mp/playeranimtypes.txt");
    if (!buf)
        Com_Error(ERR_DROP, "Couldn',27h,'t load file %s", "mp/playeranimtypes.txt");
    text_p = buf;
    Com_BeginParseSession("BG_AnimParseAnimScript");
    while (1)
    {
        token = (const char *)Com_Parse(&text_p);
        if (!token || !*token)
            break;
        if (g_playerAnimTypeNamesCount >= 0x40)
            Com_Error(ERR_DROP, "Player anim type array size exceeded");
        g_playerAnimTypeNames[g_playerAnimTypeNamesCount] = (char *)Hunk_Alloc(
            strlen(token) + 1,
            "BG_LoadPlayerAnimTypes",
            9);
        v2 = token;
        v1 = g_playerAnimTypeNames[g_playerAnimTypeNamesCount];
        do
        {
            v0 = *v2;
            *v1++ = *v2++;
        } while (v0);
        ++g_playerAnimTypeNamesCount;
    }
    Com_EndParseSession();
    Com_UnloadRawTextFile(buf);
#elif KISAK_SP
    g_playerAnimTypeNamesCount = 1;
    g_playerAnimTypeNames[0] = (char*)"none";
#endif
}

void __cdecl InitWeaponDef(WeaponDef *weapDef)
{
    const cspField_t *pField; // [esp+4h] [ebp-8h]
    int iField; // [esp+8h] [ebp-4h]

    weapDef->szInternalName = "";
    iField = 0;
    pField = weaponDefFields;
    while (iField < 502)
    {
        if (pField->iFieldType == CSPFT_STRING)
            *(const char **)((char *)weapDef + pField->iOffset) = "";
        ++iField;
        ++pField;
    }
}

char __cdecl G_ParseAIWeaponAccurayGraphFile(
    const char *buffer,
    const char *fileName,
    float (*knots)[2],
    int *knotCount)
{
    int v4; // eax
    long double v5; // st7
    long double v6; // st7
    int knotCountIndex; // [esp+0h] [ebp-8h]
    parseInfo_t *tokenb; // [esp+4h] [ebp-4h]
    parseInfo_t *token; // [esp+4h] [ebp-4h]
    parseInfo_t *tokena; // [esp+4h] [ebp-4h]

    iassert(buffer);
    iassert(fileName);
    iassert(knots);
    iassert(knotCount);

    Com_BeginParseSession(fileName);
    tokenb = Com_Parse(&buffer);
    v4 = atoi(tokenb->token);
    *knotCount = v4;
    knotCountIndex = 0;
    while (1)
    {
        token = Com_Parse(&buffer);
        if (!token->token[0])
            break;
        if (token->token[0] == 125)
            break;
        v5 = atof(token->token);
        (*knots)[2 * knotCountIndex] = v5;
        tokena = Com_Parse(&buffer);
        if (!tokena->token[0] || tokena->token[0] == 125)
            break;
        v6 = atof(tokena->token);
        (*knots)[2 * knotCountIndex++ + 1] = v6;
        if (knotCountIndex >= 16)
        {
            Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" has too many graph knots\n", fileName);
            Com_EndParseSession();
            return 0;
        }
    }
    Com_EndParseSession();
    if (knotCountIndex == *knotCount)
    {
        if ((*knots)[2 * knotCountIndex - 2] == 1.0)
        {
            return 1;
        }
        else
        {
            Com_PrintError(CON_CHANNEL_SERVER, "ERROR: \"%s\" Range must be 0.0 to 1.0\n", fileName);
            return 0;
        }
    }
    else
    {
        Com_PrintError(CON_CHANNEL_SERVER, "ERROR: \"%s\" Error in parsing an ai weapon accuracy file\n", fileName);
        return 0;
    }
}

char __cdecl G_ParseWeaponAccurayGraphInternal(
    WeaponDef *weaponDef,
    const char *dirName,
    const char *graphName,
    float (*knots)[2],
    int *knotCount)
{
    signed int v6; // [esp+10h] [ebp-205Ch]
    char string[64]; // [esp+14h] [ebp-2058h] BYREF
    char buffer[8196]; // [esp+54h] [ebp-2018h] BYREF
    const char *last; // [esp+205Ch] [ebp-10h]
    int knotCounta; // [esp+2060h] [ebp-Ch] BYREF
    int f; // [esp+2064h] [ebp-8h] BYREF
    int len; // [esp+2068h] [ebp-4h]

    last = "WEAPONACCUFILE";
    len = strlen("WEAPONACCUFILE");
    iassert(weaponDef);
    iassert(graphName);
    iassert(knots);
    iassert(knotCount);
    iassert(dirName);

    if (weaponDef->weapType && weaponDef->weapType != WEAPTYPE_PROJECTILE)
        return 1;

    if (!*graphName)
        return 1;

    snprintf(string, ARRAYSIZE(string), "accuracy/%s/%s", dirName, graphName);
    v6 = FS_FOpenFileByMode(string, &f, FS_READ);
    if (v6 >= 0)
    {
        FS_Read((uint8_t *)buffer, len, f);
        buffer[len] = 0;
        if (!strncmp(buffer, last, len))
        {
            if (v6 - len < 0x2000)
            {
                memset((uint8_t *)buffer, 0, 0x2000u);
                FS_Read((uint8_t *)buffer, v6 - len, f);
                buffer[v6 - len] = 0;
                FS_FCloseFile(f);
                knotCounta = 0;
                if (G_ParseAIWeaponAccurayGraphFile(buffer, string, knots, &knotCounta))
                {
                    *knotCount = knotCounta;
                    return 1;
                }
                else
                {
                    return 0;
                }
            }
            else
            {
                Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" Is too long of an ai weapon accuracy file to parse\n", string);
                FS_FCloseFile(f);
                return 0;
            }
        }
        else
        {
            Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: \"%s\" does not appear to be an ai weapon accuracy file\n", string);
            FS_FCloseFile(f);
            return 0;
        }
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_SERVER, "WARNING: Could not load ai weapon accuracy file '%s'\n", string);
        return 0;
    }
}

char __cdecl G_ParseWeaponAccurayGraphs(WeaponDef *weaponDef)
{
    uint size; // [esp+4h] [ebp-8Ch]
    int weaponType; // [esp+8h] [ebp-88h]
    int accuracyGraphKnotCount; // [esp+Ch] [ebp-84h] BYREF
    float accuracyGraphKnots[16][2]; // [esp+10h] [ebp-80h] BYREF

    for (weaponType = 0; weaponType < 2; ++weaponType)
    {
        memset((uint8_t *)accuracyGraphKnots, 0, sizeof(accuracyGraphKnots));
        accuracyGraphKnotCount = 0;
        if (!G_ParseWeaponAccurayGraphInternal(
            weaponDef,
            accuracyDirName[weaponType],
            weaponDef->accuracyGraphName[weaponType],
            accuracyGraphKnots,
            &accuracyGraphKnotCount))
            return 0;
        if (accuracyGraphKnotCount > 0)
        {
            size = 8 * accuracyGraphKnotCount;
            weaponDef->accuracyGraphKnots[weaponType] = (float (*)[2])Hunk_AllocLowAlign(
                8 * accuracyGraphKnotCount,
                4,
                "G_ParseWeaponAccurayGraphs",
                9);
            weaponDef->originalAccuracyGraphKnots[weaponType] = weaponDef->accuracyGraphKnots[weaponType];
            memcpy((uint8_t *)weaponDef->accuracyGraphKnots[weaponType], (uint8_t *)accuracyGraphKnots, size);
            weaponDef->accuracyGraphKnotCount[weaponType] = accuracyGraphKnotCount;
            weaponDef->originalAccuracyGraphKnotCount[weaponType] = weaponDef->accuracyGraphKnotCount[weaponType];
        }
    }
    return 1;
}

WeaponDef *__cdecl BG_LoadDefaultWeaponDef_LoadObj()
{
    InitWeaponDef(&bg_defaultWeaponDefs);
    bg_defaultWeaponDefs.szInternalName = "none";
    bg_defaultWeaponDefs.accuracyGraphName[0] = "noweapon.accu";
    bg_defaultWeaponDefs.accuracyGraphName[1] = "noweapon.accu";
    bg_defaultWeaponDefs.sprintDurationScale = 1.75;
    G_ParseWeaponAccurayGraphs(&bg_defaultWeaponDefs);
    return &bg_defaultWeaponDefs;
}

WeaponDef *__cdecl BG_LoadDefaultWeaponDef()
{
    if (IsFastFileLoad())
        return BG_LoadDefaultWeaponDef_FastFile();
    else
        return BG_LoadDefaultWeaponDef_LoadObj();
}

WeaponDef *__cdecl BG_LoadDefaultWeaponDef_FastFile()
{
    return DB_FindXAssetHeader(ASSET_TYPE_WEAPON, "none").weapon;
}

int __cdecl Weapon_GetStringArrayIndex(const char *value, char **stringArray, int arraySize)
{
    int arrayIndex; // [esp+0h] [ebp-4h]

    iassert(value);
    iassert(stringArray);

    for (arrayIndex = 0; arrayIndex < arraySize; ++arrayIndex)
    {
        if (!I_stricmp(value, stringArray[arrayIndex]))
            return arrayIndex;
    }
    return -1;
}

snd_alias_list_t **__cdecl BG_RegisterSurfaceTypeSounds(const char *surfaceSoundBase)
{
    char *v2; // eax
    snd_alias_list_t *SoundAlias; // eax
    char v4; // [esp+3h] [ebp-131h]
    char *v5; // [esp+8h] [ebp-12Ch]
    const char *v6; // [esp+Ch] [ebp-128h]
    snd_alias_list_t **result; // [esp+20h] [ebp-114h]
    char aliasName[260]; // [esp+24h] [ebp-110h] BYREF
    snd_alias_list_t *defaultAliasList; // [esp+12Ch] [ebp-8h]
    int i; // [esp+130h] [ebp-4h]

    iassert(surfaceSoundBase);

    if (!*surfaceSoundBase)
        return 0;

    for (i = 0; i < surfaceTypeSoundListCount; ++i)
    {
        if (!I_strcmp(surfaceTypeSoundLists[i].surfaceSoundBase, surfaceSoundBase))
            return surfaceTypeSoundLists[i].soundAliasList;
    }
    if (surfaceTypeSoundListCount == 16)
        Com_Error(ERR_DROP, "Exceeded MAX_SURFACE_TYPE_SOUND_LISTS (%d)", 16);

    result = (snd_alias_list_t **)Hunk_AllocLow(29 * sizeof(snd_alias_list_t *), "BG_RegisterSurfaceTypeSounds", 15);
    Com_sprintf(aliasName, 0x100u, "%s_default", surfaceSoundBase);
    defaultAliasList = Com_FindSoundAlias(aliasName);
    for (i = 0; i < 29; ++i)
    {
        v2 = (char*)Com_SurfaceTypeToName(i);
        Com_sprintf(aliasName, 0x100u, "%s_%s", surfaceSoundBase, v2);
        SoundAlias = Com_FindSoundAlias(aliasName);
        result[i] = SoundAlias;
        if (!result[i])
            result[i] = defaultAliasList;
    }
    surfaceTypeSoundLists[surfaceTypeSoundListCount].surfaceSoundBase = (char *)Hunk_AllocLow(
        strlen(surfaceSoundBase) + 1,
        "BG_RegisterSurfaceTypeSounds",
        15);
    v6 = surfaceSoundBase;
    v5 = surfaceTypeSoundLists[surfaceTypeSoundListCount].surfaceSoundBase;
    do
    {
        v4 = *v6;
        *v5++ = *v6++;
    } while (v4);
    surfaceTypeSoundLists[surfaceTypeSoundListCount++].soundAliasList = result;
    return result;
}

int __cdecl BG_ParseWeaponDefSpecificFieldType(uint8_t *pStruct, const char *pValue, int iFieldType)
{
    uint16_t LowercaseString_DONE; // ax
    uint16_t v5; // ax
    int result; // eax
    char v7; // [esp+3h] [ebp-91h]
    char *v8; // [esp+8h] [ebp-8Ch]
    const char *v9; // [esp+Ch] [ebp-88h]
    int v10; // [esp+10h] [ebp-84h]
    const char *pos; // [esp+38h] [ebp-5Ch] BYREF
    int numHideTags; // [esp+3Ch] [ebp-58h]
    int numNoteTrackMappings; // [esp+40h] [ebp-54h]
    char keyName[64]; // [esp+44h] [ebp-50h] BYREF
    int arrayIndex; // [esp+88h] [ebp-Ch]
    const char *token; // [esp+8Ch] [ebp-8h]
    WeaponDef *weapDef; // [esp+90h] [ebp-4h]

    iassert(pStruct);
    iassert(pValue);

    weapDef = (WeaponDef *)pStruct;
    switch (iFieldType)
    {
    case WFT_WEAPONTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)szWeapTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon type %s in %s", pValue, weapDef->szInternalName);
        weapDef->weapType = (weapType_t)arrayIndex;
        goto LABEL_86;
    case WFT_WEAPONCLASS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)szWeapClassNames, 10);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon class %s in %s", pValue, weapDef->szInternalName);
        weapDef->weapClass = (weapClass_t)arrayIndex;
        goto LABEL_86;
    case WFT_OVERLAYRETICLE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapOverlayReticleNames, 2);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon reticle %s in %s", pValue, weapDef->szInternalName);
        weapDef->overlayReticle = (weapOverlayReticle_t)arrayIndex;
        goto LABEL_86;
    case WFT_PENETRATE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)penetrateTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon penetrate type %s in %s", pValue, weapDef->szInternalName);
        weapDef->penetrateType = (PenetrateType)arrayIndex;
        goto LABEL_86;
    case WFT_IMPACT_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)impactTypeNames, 9);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon impact type %s in %s", pValue, weapDef->szInternalName);
        weapDef->impactType = (ImpactType)arrayIndex;
        goto LABEL_86;
    case WFT_STANCE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapStanceNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon stance %s in %s", pValue, weapDef->szInternalName);
        weapDef->stance = (weapStance_t)arrayIndex;
        goto LABEL_86;
    case WFT_PROJ_EXPLOSION:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szProjectileExplosionNames, 7);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon projExplosion %s in %s", pValue, weapDef->szInternalName);
        weapDef->projExplosion = (weapProjExposion_t)arrayIndex;
        goto LABEL_86;
    case WFT_OFFHAND_CLASS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)offhandClassNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon offhand class %s in %s", pValue, weapDef->szInternalName);
        weapDef->offhandClass = (OffhandClass)arrayIndex;
        goto LABEL_86;
    case WFT_ANIMTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, g_playerAnimTypeNames, g_playerAnimTypeNamesCount);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon player anim type %s in %s", pValue, weapDef->szInternalName);
        weapDef->playerAnimType = arrayIndex;
        goto LABEL_86;
    case WFT_ACTIVE_RETICLE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)activeReticleNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon active reticle type %s in %s", pValue, weapDef->szInternalName);
        weapDef->activeReticleType = (activeReticleType_t)arrayIndex;
        goto LABEL_86;
    case WFT_GUIDED_MISSILE_TYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)guidedMissileNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon guided missile type %s in %s", pValue, weapDef->szInternalName);
        weapDef->guidedMissileType = (guidedMissileType_t)arrayIndex;
        goto LABEL_86;
    case WFT_BOUNCE_SOUND:
        weapDef->bounceSound = BG_RegisterSurfaceTypeSounds(pValue);
        goto LABEL_86;
    case WFT_STICKINESS:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)stickinessNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon stickiness %s in %s", pValue, weapDef->szInternalName);
        weapDef->stickiness = (WeapStickinessType)arrayIndex;
        goto LABEL_86;
    case WFT_OVERLAYINTERFACE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)overlayInterfaceNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon overlay interface %s in %s", pValue, weapDef->szInternalName);
        weapDef->overlayInterface = (WeapOverlayInteface_t)arrayIndex;
        goto LABEL_86;
    case WFT_INVENTORYTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapInventoryTypeNames, 4);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon inventory type %s in %s", pValue, weapDef->szInternalName);
        weapDef->inventoryType = (weapInventoryType_t)arrayIndex;
        goto LABEL_86;
    case WFT_FIRETYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)szWeapFireTypeNames, 5);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon firetype %s in %s", pValue, weapDef->szInternalName);
        weapDef->fireType = (weapFireType_t)arrayIndex;
        goto LABEL_86;
    case WFT_AMMOCOUNTER_CLIPTYPE:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char**)ammoCounterClipNames, 7);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon ammo counter clip %s in %s", pValue, weapDef->szInternalName);
        weapDef->ammoCounterClip = (ammoCounterClipType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_HUD:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon hud icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->hudIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_AMMOCOUNTER:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon ammo counter icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->ammoCounterIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_KILL:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon kill icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->killIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_ICONRATIO_DPAD:
        arrayIndex = Weapon_GetStringArrayIndex(pValue, (char **)weapIconRatioNames, 3);
        if (arrayIndex < 0)
            Com_Error(ERR_DROP, "Unknown weapon dpad icon ratio %s in %s", pValue, weapDef->szInternalName);
        weapDef->dpadIconRatio = (weaponIconRatioType_t)arrayIndex;
        goto LABEL_86;
    case WFT_HIDETAGS:
        numHideTags = 0;
        pos = pValue;
        while (1)
        {
            token = (const char *)Com_Parse(&pos);
            if (!pos)
                break;
            if (numHideTags >= 8)
                Com_Error(ERR_DROP, "maximum hide tags (%s) exceeded: %i > %i'", token, numHideTags, 8);
            weapDef->hideTags[numHideTags] = SL_GetStringOfSize((char *)token, 0, strlen(token) + 1, MT_TYPE_MODEL_PART);
            weapDef->hideTags[numHideTags] = SL_ConvertToLowercase(weapDef->hideTags[numHideTags], 0, MT_TYPE_MODEL_PART);
            ++numHideTags;
        }
        goto LABEL_86;
    case WFT_NOTETRACKSOUNDMAP:
        numNoteTrackMappings = 0;
        pos = pValue;
        keyName[0] = 0;
        while (1)
        {
            token = (const char *)Com_Parse(&pos);
            if (!pos)
                break;
            if (numNoteTrackMappings >= 16)
                Com_Error(ERR_DROP, "Max notetrack-to-sound mappings (%i) exceeded with entry '%s'", 16, token);
            if (keyName[0])
            {
                LowercaseString_DONE = SL_GetLowercaseString(keyName, 0);
                weapDef->notetrackSoundMapKeys[numNoteTrackMappings] = LowercaseString_DONE;
                v5 = SL_GetLowercaseString(token, 0);
                weapDef->notetrackSoundMapValues[numNoteTrackMappings++] = v5;
                keyName[0] = 0;
            }   
            else
            {
                v10 = strlen(token);
                if (v10 >= 63)
                    Com_Error(ERR_DROP, "Notetrack - to - sound: keyname \"%s\" is too long(length % i / % i).", token, v10, 63);
                v9 = token;
                v8 = keyName;
                do
                {
                    v7 = *v9;
                    *v8++ = *v9++;
                } while (v7);
            }
        }
        if (keyName[0])
            Com_PrintWarning(
                CON_CHANNEL_DONT_FILTER,
                "Notetrack-to-Sound: Weapon '%s' has bad entry; notetrack '%s' doesn't have a corresponding sound.\n",
                weapDef->szInternalName,
                keyName);
    LABEL_86:
        result = 1;
        break;
    default:
        Com_Error(ERR_DROP, "Bad field type %i in %s", iFieldType, weapDef->szInternalName);
        result = 0;
        break;
    }
    return result;
}

void __cdecl BG_SetupTransitionTimes(WeaponDef *weapDef)
{
    double v1; // st7
    double v2; // st7

    if (weapDef->iAdsTransInTime <= 0)
        v1 = 1.0 / (float)300.0;
    else
        v1 = 1.0 / (double)weapDef->iAdsTransInTime;
    weapDef->fOOPosAnimLength[0] = v1;
    if (weapDef->iAdsTransOutTime <= 0)
        v2 = 1.0 / (float)500.0;
    else
        v2 = 1.0 / (double)weapDef->iAdsTransOutTime;
    weapDef->fOOPosAnimLength[1] = v2;
}

void __cdecl BG_CheckWeaponDamageRanges(WeaponDef *weapDef)
{
    if (weapDef->fMaxDamageRange <= 0.0f)
        weapDef->fMaxDamageRange = 999999.0f;
    if (weapDef->fMinDamageRange <= 0.0f)
        weapDef->fMinDamageRange = 999999.12f;
}

void __cdecl BG_CheckProjectileValues(WeaponDef *weaponDef)
{
    iassert(weaponDef->weapType == WEAPTYPE_PROJECTILE);

    if ((double)weaponDef->iProjectileSpeed <= 0.0)
        Com_Error(ERR_DROP, "Projectile speed for WeapType %s must be greater than 0.0", weaponDef->szDisplayName);

    if (weaponDef->destabilizationCurvatureMax >= 1000000000.0f || weaponDef->destabilizationCurvatureMax < 0.0)
        Com_Error(
            ERR_DROP,
            "Destabilization angle for for WeapType %s must be between 0 and 45 degrees",
            weaponDef->szDisplayName);

    if (weaponDef->destabilizationRateTime < 0.0)
        Com_Error(ERR_DROP, "Destabilization rate time for for WeapType %s must be non-negative", weaponDef->szDisplayName);
}

WeaponDef *__cdecl BG_LoadWeaponDefInternal(const char *one, const char *two)
{
    snd_alias_list_t *SoundAlias; // eax
    snd_alias_list_t *v4; // eax
    snd_alias_list_t *v5; // eax
    snd_alias_list_t *v6; // eax
    snd_alias_list_t *v7; // eax
    char buffer[10244]; // [esp+1Ch] [ebp-2858h] BYREF
    int f; // [esp+2820h] [ebp-54h] BYREF
    int len; // [esp+2824h] [ebp-50h]
    signed int v11; // [esp+2828h] [ebp-4Ch]
    char dest[64]; // [esp+282Ch] [ebp-48h] BYREF
    WeaponDef *weapDef; // [esp+2870h] [ebp-4h]

    len = strlen("WEAPONFILE");
    weapDef = (WeaponDef *)Hunk_AllocLow(sizeof(WeaponDef), "BG_LoadWeaponDefInternal", 9);
    InitWeaponDef(weapDef);
    Com_sprintf(dest, 0x40u, "weapons/%s/%s", one, two);
    v11 = FS_FOpenFileByMode(dest, &f, FS_READ);
    if (v11 >= 0)
    {
        FS_Read((uint8_t *)buffer, len, f);
        buffer[len] = 0;
        if (!strncmp(buffer, "WEAPONFILE", len))
        {
            if ((uint)(v11 - len) < 0x2800)
            {
                memset((uint8_t *)buffer, 0, 0x2800u);
                FS_Read((uint8_t *)buffer, v11 - len, f);
                buffer[v11 - len] = 0;
                FS_FCloseFile(f);
                if (Info_Validate(buffer))
                {
                    SetConfigString((char **)weapDef, two);
                    if (ParseConfigStringToStructCustomSize(
                        (uint8_t *)weapDef,
                        weaponDefFields,
                        502,
                        buffer,
                        WFT_NUM_FIELD_TYPES,
                        BG_ParseWeaponDefSpecificFieldType,
                        SetConfigString2))
                    {
                        if (I_stricmp(two, "defaultweapon_mp"))
                        {
                            if (!weapDef->viewLastShotEjectEffect)
                                weapDef->viewLastShotEjectEffect = weapDef->viewShellEjectEffect;
                            if (!weapDef->worldLastShotEjectEffect)
                                weapDef->worldLastShotEjectEffect = weapDef->worldShellEjectEffect;
                            if (!weapDef->raiseSound)
                            {
                                SoundAlias = Com_FindSoundAlias("weap_raise");
                                weapDef->raiseSound = SoundAlias;
                            }
                            if (!weapDef->putawaySound)
                            {
                                v4 = Com_FindSoundAlias("weap_putaway");
                                weapDef->putawaySound = v4;
                            }
                            if (!weapDef->pickupSound)
                            {
                                v5 = Com_FindSoundAlias("weap_pickup");
                                weapDef->pickupSound = v5;
                            }
                            if (!weapDef->ammoPickupSound)
                            {
                                v6 = Com_FindSoundAlias("weap_ammo_pickup");
                                weapDef->ammoPickupSound = v6;
                            }
                            if (!weapDef->emptyFireSound)
                            {
                                v7 = Com_FindSoundAlias("weap_dryfire_smg_npc");
                                weapDef->emptyFireSound = v7;
                            }
                        }
                        BG_SetupTransitionTimes(weapDef);
                        BG_CheckWeaponDamageRanges(weapDef);
                        if (weapDef->enemyCrosshairRange > 15000.0)
                            Com_Error(ERR_DROP, "Enemy crosshair ranges should be less than %f ", 15000.0);
                        if (weapDef->weapType == WEAPTYPE_PROJECTILE)
                            BG_CheckProjectileValues(weapDef);
                        if (G_ParseWeaponAccurayGraphs(weapDef))
                        {
                            I_strlwr((char *)weapDef->szAmmoName);
                            I_strlwr((char *)weapDef->szClipName);
                            return weapDef;
                        }
                        else
                        {
                            return 0;
                        }
                    }
                    else
                    {
                        return 0;
                    }
                }
                else
                {
                    Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: \"%s\" is not a valid weapon file\n", dest);
                    return 0;
                }
            }
            else
            {
                Com_PrintWarning(
                    CON_CHANNEL_PLAYERWEAP,
                    "WARNING: \"%s\" Is too long of a weapon file to parse (fileLength = %d identifierLength = %d)\n",
                    dest,
                    v11,
                    len);
                FS_FCloseFile(f);
                return 0;
            }
        }
        else
        {
            Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: \"%s\" does not appear to be a weapon file\n", dest);
            FS_FCloseFile(f);
            return 0;
        }
    }
    else
    {
        Com_PrintWarning(CON_CHANNEL_PLAYERWEAP, "WARNING: Could not load weapon file '%s'\n", dest);
        return 0;
    }
}


WeaponDef *__cdecl BG_LoadWeaponDef_LoadObj(const char *name)
{
    WeaponDef *weapDef; // [esp+0h] [ebp-4h]

    if (!*name)
        return 0;
#ifdef KISAK_MP
    weapDef = BG_LoadWeaponDefInternal("mp", name);
#elif KISAK_SP
    weapDef = BG_LoadWeaponDefInternal("sp", name);
#endif
    if (weapDef)
        return weapDef;

#ifdef KISAK_MP
    weapDef = BG_LoadWeaponDefInternal("mp", "defaultweapon_mp");
#elif KISAK_SP
    weapDef = BG_LoadWeaponDefInternal("sp", "defaultweapon");
#endif

    if (!weapDef)
        Com_Error(ERR_DROP, "BG_LoadWeaponDef: Could not find default weapon");

    SetConfigString((char **)weapDef, name);
    return weapDef;
}
