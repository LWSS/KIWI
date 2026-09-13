#pragma once
#include <universal/q_shared.h>
#include <xanim/xanim.h>
#include <stddef.h>

// Native weapon pointer fields, traversed in this order by both writer and reader.
// The bounce-sound array and four accuracy-graph arrays follow these fields.
enum DB64WeaponFieldType
{
    DB64_WEAPON_STRING,
    DB64_WEAPON_MODEL,
    DB64_WEAPON_EFFECT,
    DB64_WEAPON_SOUND,
    DB64_WEAPON_MATERIAL
};
struct DB64WeaponField
{
    size_t offset;
    int count;
    DB64WeaponFieldType type;
};
static const DB64WeaponField db64WeaponFields[] =
{
    {offsetof(WeaponDef, szInternalName), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, szDisplayName), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, szOverlayName), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, gunXModel), 16, DB64_WEAPON_MODEL},
    {offsetof(WeaponDef, handXModel), 1, DB64_WEAPON_MODEL},
    {offsetof(WeaponDef, szXAnims), NUM_WEAP_ANIMS, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, szModeName), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, viewFlashEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, worldFlashEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, pickupSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, pickupSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, ammoPickupSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, ammoPickupSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, projectileSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, pullbackSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, pullbackSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, fireSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, fireSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, fireLoopSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, fireLoopSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, fireStopSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, fireStopSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, fireLastSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, fireLastSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, emptyFireSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, emptyFireSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, meleeSwipeSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, meleeSwipeSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, meleeHitSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, meleeMissSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, rechamberSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, rechamberSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, reloadSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, reloadSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, reloadEmptySound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, reloadEmptySoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, reloadStartSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, reloadStartSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, reloadEndSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, reloadEndSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, detonateSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, detonateSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, nightVisionWearSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, nightVisionWearSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, nightVisionRemoveSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, nightVisionRemoveSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, altSwitchSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, altSwitchSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, raiseSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, raiseSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, firstRaiseSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, firstRaiseSoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, putawaySound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, putawaySoundPlayer), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, viewShellEjectEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, worldShellEjectEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, viewLastShotEjectEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, worldLastShotEjectEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, reticleCenter), 1, DB64_WEAPON_MATERIAL},
    {offsetof(WeaponDef, reticleSide), 1, DB64_WEAPON_MATERIAL},
    {offsetof(WeaponDef, worldModel), 16, DB64_WEAPON_MODEL},
    {offsetof(WeaponDef, worldClipModel), 1, DB64_WEAPON_MODEL},
    {offsetof(WeaponDef, rocketModel), 1, DB64_WEAPON_MODEL},
    {offsetof(WeaponDef, knifeModel), 1, DB64_WEAPON_MODEL},
    {offsetof(WeaponDef, worldKnifeModel), 1, DB64_WEAPON_MODEL},
    {offsetof(WeaponDef, hudIcon), 1, DB64_WEAPON_MATERIAL},
    {offsetof(WeaponDef, ammoCounterIcon), 1, DB64_WEAPON_MATERIAL},
    {offsetof(WeaponDef, szAmmoName), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, szClipName), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, szSharedAmmoCapName), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, overlayMaterial), 1, DB64_WEAPON_MATERIAL},
    {offsetof(WeaponDef, overlayMaterialLowRes), 1, DB64_WEAPON_MATERIAL},
    {offsetof(WeaponDef, killIcon), 1, DB64_WEAPON_MATERIAL},
    {offsetof(WeaponDef, dpadIcon), 1, DB64_WEAPON_MATERIAL},
    {offsetof(WeaponDef, szAltWeaponName), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, projectileModel), 1, DB64_WEAPON_MODEL},
    {offsetof(WeaponDef, projExplosionEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, projDudEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, projExplosionSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, projDudSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, projTrailEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, projIgnitionEffect), 1, DB64_WEAPON_EFFECT},
    {offsetof(WeaponDef, projIgnitionSound), 1, DB64_WEAPON_SOUND},
    {offsetof(WeaponDef, accuracyGraphName), WEAP_ACCURACY_COUNT, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, szUseHintString), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, dropHintString), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, szScript), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, fireRumble), 1, DB64_WEAPON_STRING},
    {offsetof(WeaponDef, meleeImpactRumble), 1, DB64_WEAPON_STRING},
};

inline bool DB64_ValidateWeaponHeader(const WeaponDef *weapon)
{
    if (!weapon || !weapon->szInternalName)
    {
        return false;
    }
    for (int i = 0; i < 2; ++i)
    {
        if (weapon->accuracyGraphKnotCount[i] < 0 || weapon->accuracyGraphKnotCount[i] > 16 ||
            weapon->originalAccuracyGraphKnotCount[i] < 0 || weapon->originalAccuracyGraphKnotCount[i] > 16 ||
            (!!weapon->accuracyGraphKnots[i] != (weapon->accuracyGraphKnotCount[i] != 0)) ||
            (!!weapon->originalAccuracyGraphKnots[i] != (weapon->originalAccuracyGraphKnotCount[i] != 0)))
        {
            return false;
        }
    }
    return true;
}
