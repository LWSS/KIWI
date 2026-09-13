#pragma once
#include <stddef.h>
#include <string.h>

// Preserve ABI member offsets; do not serialize indeterminate compiler padding.
static void Linker_ClearHeaderPadding(void *data, size_t size, const size_t spans[][2], size_t count)
{
    size_t end = 0;
    for (size_t i = 0; i < count; ++i)
    {
        if (spans[i][0] > end)
        {
            memset((unsigned char *)data + end, 0, spans[i][0] - end);
        }
        end = spans[i][0] + spans[i][1];
    }
    if (end < size)
    {
        memset((unsigned char *)data + end, 0, size - end);
    }
}

#define HEADER_FIELD(type, field) {offsetof(type, field), sizeof(((type *)0)->field)}

static void Linker_ClearPadding_snd_alias_t(void *data)
{
    static const size_t spans[][2] = {
        HEADER_FIELD(snd_alias_t, aliasName), HEADER_FIELD(snd_alias_t, subtitle), HEADER_FIELD(snd_alias_t, secondaryAliasName),
        HEADER_FIELD(snd_alias_t, chainAliasName), HEADER_FIELD(snd_alias_t, soundFile), HEADER_FIELD(snd_alias_t, sequence),
        HEADER_FIELD(snd_alias_t, volMin), HEADER_FIELD(snd_alias_t, volMax), HEADER_FIELD(snd_alias_t, pitchMin),
        HEADER_FIELD(snd_alias_t, pitchMax), HEADER_FIELD(snd_alias_t, distMin), HEADER_FIELD(snd_alias_t, distMax),
        HEADER_FIELD(snd_alias_t, flags), HEADER_FIELD(snd_alias_t, slavePercentage), HEADER_FIELD(snd_alias_t, probability),
        HEADER_FIELD(snd_alias_t, lfePercentage), HEADER_FIELD(snd_alias_t, centerPercentage), HEADER_FIELD(snd_alias_t, startDelay),
        HEADER_FIELD(snd_alias_t, volumeFalloffCurve), HEADER_FIELD(snd_alias_t, envelopMin), HEADER_FIELD(snd_alias_t, envelopMax),
        HEADER_FIELD(snd_alias_t, envelopPercentage), HEADER_FIELD(snd_alias_t, speakerMap),
    };
    Linker_ClearHeaderPadding(data, sizeof(snd_alias_t), spans, sizeof(spans) / sizeof(spans[0]));
}

static void Linker_ClearPadding_WeaponDef(void *data)
{
    static const size_t spans[][2] = {
        HEADER_FIELD(WeaponDef, szInternalName), HEADER_FIELD(WeaponDef, szDisplayName), HEADER_FIELD(WeaponDef, szOverlayName),
        HEADER_FIELD(WeaponDef, gunXModel), HEADER_FIELD(WeaponDef, handXModel), HEADER_FIELD(WeaponDef, szXAnims),
        HEADER_FIELD(WeaponDef, szModeName), HEADER_FIELD(WeaponDef, hideTags), HEADER_FIELD(WeaponDef, notetrackSoundMapKeys),
        HEADER_FIELD(WeaponDef, notetrackSoundMapValues), HEADER_FIELD(WeaponDef, playerAnimType), HEADER_FIELD(WeaponDef, weapType),
        HEADER_FIELD(WeaponDef, weapClass), HEADER_FIELD(WeaponDef, penetrateType), HEADER_FIELD(WeaponDef, impactType),
        HEADER_FIELD(WeaponDef, inventoryType), HEADER_FIELD(WeaponDef, fireType), HEADER_FIELD(WeaponDef, offhandClass),
        HEADER_FIELD(WeaponDef, stance), HEADER_FIELD(WeaponDef, viewFlashEffect), HEADER_FIELD(WeaponDef, worldFlashEffect),
        HEADER_FIELD(WeaponDef, pickupSound), HEADER_FIELD(WeaponDef, pickupSoundPlayer), HEADER_FIELD(WeaponDef, ammoPickupSound),
        HEADER_FIELD(WeaponDef, ammoPickupSoundPlayer), HEADER_FIELD(WeaponDef, projectileSound), HEADER_FIELD(WeaponDef, pullbackSound),
        HEADER_FIELD(WeaponDef, pullbackSoundPlayer), HEADER_FIELD(WeaponDef, fireSound), HEADER_FIELD(WeaponDef, fireSoundPlayer),
        HEADER_FIELD(WeaponDef, fireLoopSound), HEADER_FIELD(WeaponDef, fireLoopSoundPlayer), HEADER_FIELD(WeaponDef, fireStopSound),
        HEADER_FIELD(WeaponDef, fireStopSoundPlayer), HEADER_FIELD(WeaponDef, fireLastSound), HEADER_FIELD(WeaponDef, fireLastSoundPlayer),
        HEADER_FIELD(WeaponDef, emptyFireSound), HEADER_FIELD(WeaponDef, emptyFireSoundPlayer), HEADER_FIELD(WeaponDef, meleeSwipeSound),
        HEADER_FIELD(WeaponDef, meleeSwipeSoundPlayer), HEADER_FIELD(WeaponDef, meleeHitSound), HEADER_FIELD(WeaponDef, meleeMissSound),
        HEADER_FIELD(WeaponDef, rechamberSound), HEADER_FIELD(WeaponDef, rechamberSoundPlayer), HEADER_FIELD(WeaponDef, reloadSound),
        HEADER_FIELD(WeaponDef, reloadSoundPlayer), HEADER_FIELD(WeaponDef, reloadEmptySound), HEADER_FIELD(WeaponDef, reloadEmptySoundPlayer),
        HEADER_FIELD(WeaponDef, reloadStartSound), HEADER_FIELD(WeaponDef, reloadStartSoundPlayer), HEADER_FIELD(WeaponDef, reloadEndSound),
        HEADER_FIELD(WeaponDef, reloadEndSoundPlayer), HEADER_FIELD(WeaponDef, detonateSound), HEADER_FIELD(WeaponDef, detonateSoundPlayer),
        HEADER_FIELD(WeaponDef, nightVisionWearSound), HEADER_FIELD(WeaponDef, nightVisionWearSoundPlayer), HEADER_FIELD(WeaponDef, nightVisionRemoveSound),
        HEADER_FIELD(WeaponDef, nightVisionRemoveSoundPlayer), HEADER_FIELD(WeaponDef, altSwitchSound), HEADER_FIELD(WeaponDef, altSwitchSoundPlayer),
        HEADER_FIELD(WeaponDef, raiseSound), HEADER_FIELD(WeaponDef, raiseSoundPlayer), HEADER_FIELD(WeaponDef, firstRaiseSound),
        HEADER_FIELD(WeaponDef, firstRaiseSoundPlayer), HEADER_FIELD(WeaponDef, putawaySound), HEADER_FIELD(WeaponDef, putawaySoundPlayer),
        HEADER_FIELD(WeaponDef, bounceSound), HEADER_FIELD(WeaponDef, viewShellEjectEffect), HEADER_FIELD(WeaponDef, worldShellEjectEffect),
        HEADER_FIELD(WeaponDef, viewLastShotEjectEffect), HEADER_FIELD(WeaponDef, worldLastShotEjectEffect), HEADER_FIELD(WeaponDef, reticleCenter),
        HEADER_FIELD(WeaponDef, reticleSide), HEADER_FIELD(WeaponDef, iReticleCenterSize), HEADER_FIELD(WeaponDef, iReticleSideSize),
        HEADER_FIELD(WeaponDef, iReticleMinOfs), HEADER_FIELD(WeaponDef, activeReticleType), HEADER_FIELD(WeaponDef, vStandMove),
        HEADER_FIELD(WeaponDef, vStandRot), HEADER_FIELD(WeaponDef, vDuckedOfs), HEADER_FIELD(WeaponDef, vDuckedMove),
        HEADER_FIELD(WeaponDef, vDuckedRot), HEADER_FIELD(WeaponDef, vProneOfs), HEADER_FIELD(WeaponDef, vProneMove),
        HEADER_FIELD(WeaponDef, vProneRot), HEADER_FIELD(WeaponDef, fPosMoveRate), HEADER_FIELD(WeaponDef, fPosProneMoveRate),
        HEADER_FIELD(WeaponDef, fStandMoveMinSpeed), HEADER_FIELD(WeaponDef, fDuckedMoveMinSpeed), HEADER_FIELD(WeaponDef, fProneMoveMinSpeed),
        HEADER_FIELD(WeaponDef, fPosRotRate), HEADER_FIELD(WeaponDef, fPosProneRotRate), HEADER_FIELD(WeaponDef, fStandRotMinSpeed),
        HEADER_FIELD(WeaponDef, fDuckedRotMinSpeed), HEADER_FIELD(WeaponDef, fProneRotMinSpeed), HEADER_FIELD(WeaponDef, worldModel),
        HEADER_FIELD(WeaponDef, worldClipModel), HEADER_FIELD(WeaponDef, rocketModel), HEADER_FIELD(WeaponDef, knifeModel),
        HEADER_FIELD(WeaponDef, worldKnifeModel), HEADER_FIELD(WeaponDef, hudIcon), HEADER_FIELD(WeaponDef, hudIconRatio),
        HEADER_FIELD(WeaponDef, ammoCounterIcon), HEADER_FIELD(WeaponDef, ammoCounterIconRatio), HEADER_FIELD(WeaponDef, ammoCounterClip),
        HEADER_FIELD(WeaponDef, iStartAmmo), HEADER_FIELD(WeaponDef, szAmmoName), HEADER_FIELD(WeaponDef, iAmmoIndex),
        HEADER_FIELD(WeaponDef, szClipName), HEADER_FIELD(WeaponDef, iClipIndex), HEADER_FIELD(WeaponDef, iMaxAmmo),
        HEADER_FIELD(WeaponDef, iClipSize), HEADER_FIELD(WeaponDef, shotCount), HEADER_FIELD(WeaponDef, szSharedAmmoCapName),
        HEADER_FIELD(WeaponDef, iSharedAmmoCapIndex), HEADER_FIELD(WeaponDef, iSharedAmmoCap), HEADER_FIELD(WeaponDef, damage),
        HEADER_FIELD(WeaponDef, playerDamage), HEADER_FIELD(WeaponDef, iMeleeDamage), HEADER_FIELD(WeaponDef, iDamageType),
        HEADER_FIELD(WeaponDef, iFireDelay), HEADER_FIELD(WeaponDef, iMeleeDelay), HEADER_FIELD(WeaponDef, meleeChargeDelay),
        HEADER_FIELD(WeaponDef, iDetonateDelay), HEADER_FIELD(WeaponDef, iFireTime), HEADER_FIELD(WeaponDef, iRechamberTime),
        HEADER_FIELD(WeaponDef, iRechamberBoltTime), HEADER_FIELD(WeaponDef, iHoldFireTime), HEADER_FIELD(WeaponDef, iDetonateTime),
        HEADER_FIELD(WeaponDef, iMeleeTime), HEADER_FIELD(WeaponDef, meleeChargeTime), HEADER_FIELD(WeaponDef, iReloadTime),
        HEADER_FIELD(WeaponDef, reloadShowRocketTime), HEADER_FIELD(WeaponDef, iReloadEmptyTime), HEADER_FIELD(WeaponDef, iReloadAddTime),
        HEADER_FIELD(WeaponDef, iReloadStartTime), HEADER_FIELD(WeaponDef, iReloadStartAddTime), HEADER_FIELD(WeaponDef, iReloadEndTime),
        HEADER_FIELD(WeaponDef, iDropTime), HEADER_FIELD(WeaponDef, iRaiseTime), HEADER_FIELD(WeaponDef, iAltDropTime),
        HEADER_FIELD(WeaponDef, iAltRaiseTime), HEADER_FIELD(WeaponDef, quickDropTime), HEADER_FIELD(WeaponDef, quickRaiseTime),
        HEADER_FIELD(WeaponDef, iFirstRaiseTime), HEADER_FIELD(WeaponDef, iEmptyRaiseTime), HEADER_FIELD(WeaponDef, iEmptyDropTime),
        HEADER_FIELD(WeaponDef, sprintInTime), HEADER_FIELD(WeaponDef, sprintLoopTime), HEADER_FIELD(WeaponDef, sprintOutTime),
        HEADER_FIELD(WeaponDef, nightVisionWearTime), HEADER_FIELD(WeaponDef, nightVisionWearTimeFadeOutEnd), HEADER_FIELD(WeaponDef, nightVisionWearTimePowerUp),
        HEADER_FIELD(WeaponDef, nightVisionRemoveTime), HEADER_FIELD(WeaponDef, nightVisionRemoveTimePowerDown), HEADER_FIELD(WeaponDef, nightVisionRemoveTimeFadeInStart),
        HEADER_FIELD(WeaponDef, fuseTime), HEADER_FIELD(WeaponDef, aiFuseTime), HEADER_FIELD(WeaponDef, requireLockonToFire),
        HEADER_FIELD(WeaponDef, noAdsWhenMagEmpty), HEADER_FIELD(WeaponDef, avoidDropCleanup), HEADER_FIELD(WeaponDef, autoAimRange),
        HEADER_FIELD(WeaponDef, aimAssistRange), HEADER_FIELD(WeaponDef, aimAssistRangeAds), HEADER_FIELD(WeaponDef, aimPadding),
        HEADER_FIELD(WeaponDef, enemyCrosshairRange), HEADER_FIELD(WeaponDef, crosshairColorChange), HEADER_FIELD(WeaponDef, moveSpeedScale),
        HEADER_FIELD(WeaponDef, adsMoveSpeedScale), HEADER_FIELD(WeaponDef, sprintDurationScale), HEADER_FIELD(WeaponDef, fAdsZoomFov),
        HEADER_FIELD(WeaponDef, fAdsZoomInFrac), HEADER_FIELD(WeaponDef, fAdsZoomOutFrac), HEADER_FIELD(WeaponDef, overlayMaterial),
        HEADER_FIELD(WeaponDef, overlayMaterialLowRes), HEADER_FIELD(WeaponDef, overlayReticle), HEADER_FIELD(WeaponDef, overlayInterface),
        HEADER_FIELD(WeaponDef, overlayWidth), HEADER_FIELD(WeaponDef, overlayHeight), HEADER_FIELD(WeaponDef, fAdsBobFactor),
        HEADER_FIELD(WeaponDef, fAdsViewBobMult), HEADER_FIELD(WeaponDef, fHipSpreadStandMin), HEADER_FIELD(WeaponDef, fHipSpreadDuckedMin),
        HEADER_FIELD(WeaponDef, fHipSpreadProneMin), HEADER_FIELD(WeaponDef, hipSpreadStandMax), HEADER_FIELD(WeaponDef, hipSpreadDuckedMax),
        HEADER_FIELD(WeaponDef, hipSpreadProneMax), HEADER_FIELD(WeaponDef, fHipSpreadDecayRate), HEADER_FIELD(WeaponDef, fHipSpreadFireAdd),
        HEADER_FIELD(WeaponDef, fHipSpreadTurnAdd), HEADER_FIELD(WeaponDef, fHipSpreadMoveAdd), HEADER_FIELD(WeaponDef, fHipSpreadDuckedDecay),
        HEADER_FIELD(WeaponDef, fHipSpreadProneDecay), HEADER_FIELD(WeaponDef, fHipReticleSidePos), HEADER_FIELD(WeaponDef, iAdsTransInTime),
        HEADER_FIELD(WeaponDef, iAdsTransOutTime), HEADER_FIELD(WeaponDef, fAdsIdleAmount), HEADER_FIELD(WeaponDef, fHipIdleAmount),
        HEADER_FIELD(WeaponDef, adsIdleSpeed), HEADER_FIELD(WeaponDef, hipIdleSpeed), HEADER_FIELD(WeaponDef, fIdleCrouchFactor),
        HEADER_FIELD(WeaponDef, fIdleProneFactor), HEADER_FIELD(WeaponDef, fGunMaxPitch), HEADER_FIELD(WeaponDef, fGunMaxYaw),
        HEADER_FIELD(WeaponDef, swayMaxAngle), HEADER_FIELD(WeaponDef, swayLerpSpeed), HEADER_FIELD(WeaponDef, swayPitchScale),
        HEADER_FIELD(WeaponDef, swayYawScale), HEADER_FIELD(WeaponDef, swayHorizScale), HEADER_FIELD(WeaponDef, swayVertScale),
        HEADER_FIELD(WeaponDef, swayShellShockScale), HEADER_FIELD(WeaponDef, adsSwayMaxAngle), HEADER_FIELD(WeaponDef, adsSwayLerpSpeed),
        HEADER_FIELD(WeaponDef, adsSwayPitchScale), HEADER_FIELD(WeaponDef, adsSwayYawScale), HEADER_FIELD(WeaponDef, adsSwayHorizScale),
        HEADER_FIELD(WeaponDef, adsSwayVertScale), HEADER_FIELD(WeaponDef, bRifleBullet), HEADER_FIELD(WeaponDef, armorPiercing),
        HEADER_FIELD(WeaponDef, bBoltAction), HEADER_FIELD(WeaponDef, aimDownSight), HEADER_FIELD(WeaponDef, bRechamberWhileAds),
        HEADER_FIELD(WeaponDef, adsViewErrorMin), HEADER_FIELD(WeaponDef, adsViewErrorMax), HEADER_FIELD(WeaponDef, bCookOffHold),
        HEADER_FIELD(WeaponDef, bClipOnly), HEADER_FIELD(WeaponDef, adsFireOnly), HEADER_FIELD(WeaponDef, cancelAutoHolsterWhenEmpty),
        HEADER_FIELD(WeaponDef, suppressAmmoReserveDisplay), HEADER_FIELD(WeaponDef, enhanced), HEADER_FIELD(WeaponDef, laserSightDuringNightvision),
        HEADER_FIELD(WeaponDef, killIcon), HEADER_FIELD(WeaponDef, killIconRatio), HEADER_FIELD(WeaponDef, flipKillIcon),
        HEADER_FIELD(WeaponDef, dpadIcon), HEADER_FIELD(WeaponDef, dpadIconRatio), HEADER_FIELD(WeaponDef, bNoPartialReload),
        HEADER_FIELD(WeaponDef, bSegmentedReload), HEADER_FIELD(WeaponDef, iReloadAmmoAdd), HEADER_FIELD(WeaponDef, iReloadStartAdd),
        HEADER_FIELD(WeaponDef, szAltWeaponName), HEADER_FIELD(WeaponDef, altWeaponIndex), HEADER_FIELD(WeaponDef, iDropAmmoMin),
        HEADER_FIELD(WeaponDef, iDropAmmoMax), HEADER_FIELD(WeaponDef, blocksProne), HEADER_FIELD(WeaponDef, silenced),
        HEADER_FIELD(WeaponDef, iExplosionRadius), HEADER_FIELD(WeaponDef, iExplosionRadiusMin), HEADER_FIELD(WeaponDef, iExplosionInnerDamage),
        HEADER_FIELD(WeaponDef, iExplosionOuterDamage), HEADER_FIELD(WeaponDef, damageConeAngle), HEADER_FIELD(WeaponDef, iProjectileSpeed),
        HEADER_FIELD(WeaponDef, iProjectileSpeedUp), HEADER_FIELD(WeaponDef, iProjectileSpeedForward), HEADER_FIELD(WeaponDef, iProjectileActivateDist),
        HEADER_FIELD(WeaponDef, projLifetime), HEADER_FIELD(WeaponDef, timeToAccelerate), HEADER_FIELD(WeaponDef, projectileCurvature),
        HEADER_FIELD(WeaponDef, projectileModel), HEADER_FIELD(WeaponDef, projExplosion), HEADER_FIELD(WeaponDef, projExplosionEffect),
        HEADER_FIELD(WeaponDef, projExplosionEffectForceNormalUp), HEADER_FIELD(WeaponDef, projDudEffect), HEADER_FIELD(WeaponDef, projExplosionSound),
        HEADER_FIELD(WeaponDef, projDudSound), HEADER_FIELD(WeaponDef, bProjImpactExplode), HEADER_FIELD(WeaponDef, stickiness),
        HEADER_FIELD(WeaponDef, hasDetonator), HEADER_FIELD(WeaponDef, timedDetonation), HEADER_FIELD(WeaponDef, rotate),
        HEADER_FIELD(WeaponDef, holdButtonToThrow), HEADER_FIELD(WeaponDef, freezeMovementWhenFiring), HEADER_FIELD(WeaponDef, lowAmmoWarningThreshold),
        HEADER_FIELD(WeaponDef, parallelBounce), HEADER_FIELD(WeaponDef, perpendicularBounce), HEADER_FIELD(WeaponDef, projTrailEffect),
        HEADER_FIELD(WeaponDef, vProjectileColor), HEADER_FIELD(WeaponDef, guidedMissileType), HEADER_FIELD(WeaponDef, maxSteeringAccel),
        HEADER_FIELD(WeaponDef, projIgnitionDelay), HEADER_FIELD(WeaponDef, projIgnitionEffect), HEADER_FIELD(WeaponDef, projIgnitionSound),
        HEADER_FIELD(WeaponDef, fAdsAimPitch), HEADER_FIELD(WeaponDef, fAdsCrosshairInFrac), HEADER_FIELD(WeaponDef, fAdsCrosshairOutFrac),
        HEADER_FIELD(WeaponDef, adsGunKickReducedKickBullets), HEADER_FIELD(WeaponDef, adsGunKickReducedKickPercent), HEADER_FIELD(WeaponDef, fAdsGunKickPitchMin),
        HEADER_FIELD(WeaponDef, fAdsGunKickPitchMax), HEADER_FIELD(WeaponDef, fAdsGunKickYawMin), HEADER_FIELD(WeaponDef, fAdsGunKickYawMax),
        HEADER_FIELD(WeaponDef, fAdsGunKickAccel), HEADER_FIELD(WeaponDef, fAdsGunKickSpeedMax), HEADER_FIELD(WeaponDef, fAdsGunKickSpeedDecay),
        HEADER_FIELD(WeaponDef, fAdsGunKickStaticDecay), HEADER_FIELD(WeaponDef, fAdsViewKickPitchMin), HEADER_FIELD(WeaponDef, fAdsViewKickPitchMax),
        HEADER_FIELD(WeaponDef, fAdsViewKickYawMin), HEADER_FIELD(WeaponDef, fAdsViewKickYawMax), HEADER_FIELD(WeaponDef, fAdsViewKickCenterSpeed),
        HEADER_FIELD(WeaponDef, fAdsViewScatterMin), HEADER_FIELD(WeaponDef, fAdsViewScatterMax), HEADER_FIELD(WeaponDef, fAdsSpread),
        HEADER_FIELD(WeaponDef, hipGunKickReducedKickBullets), HEADER_FIELD(WeaponDef, hipGunKickReducedKickPercent), HEADER_FIELD(WeaponDef, fHipGunKickPitchMin),
        HEADER_FIELD(WeaponDef, fHipGunKickPitchMax), HEADER_FIELD(WeaponDef, fHipGunKickYawMin), HEADER_FIELD(WeaponDef, fHipGunKickYawMax),
        HEADER_FIELD(WeaponDef, fHipGunKickAccel), HEADER_FIELD(WeaponDef, fHipGunKickSpeedMax), HEADER_FIELD(WeaponDef, fHipGunKickSpeedDecay),
        HEADER_FIELD(WeaponDef, fHipGunKickStaticDecay), HEADER_FIELD(WeaponDef, fHipViewKickPitchMin), HEADER_FIELD(WeaponDef, fHipViewKickPitchMax),
        HEADER_FIELD(WeaponDef, fHipViewKickYawMin), HEADER_FIELD(WeaponDef, fHipViewKickYawMax), HEADER_FIELD(WeaponDef, fHipViewKickCenterSpeed),
        HEADER_FIELD(WeaponDef, fHipViewScatterMin), HEADER_FIELD(WeaponDef, fHipViewScatterMax), HEADER_FIELD(WeaponDef, fightDist),
        HEADER_FIELD(WeaponDef, maxDist), HEADER_FIELD(WeaponDef, accuracyGraphName), HEADER_FIELD(WeaponDef, accuracyGraphKnots),
        HEADER_FIELD(WeaponDef, originalAccuracyGraphKnots), HEADER_FIELD(WeaponDef, accuracyGraphKnotCount), HEADER_FIELD(WeaponDef, originalAccuracyGraphKnotCount),
        HEADER_FIELD(WeaponDef, iPositionReloadTransTime), HEADER_FIELD(WeaponDef, leftArc), HEADER_FIELD(WeaponDef, rightArc),
        HEADER_FIELD(WeaponDef, topArc), HEADER_FIELD(WeaponDef, bottomArc), HEADER_FIELD(WeaponDef, accuracy),
        HEADER_FIELD(WeaponDef, aiSpread), HEADER_FIELD(WeaponDef, playerSpread), HEADER_FIELD(WeaponDef, minTurnSpeed),
        HEADER_FIELD(WeaponDef, maxTurnSpeed), HEADER_FIELD(WeaponDef, pitchConvergenceTime), HEADER_FIELD(WeaponDef, yawConvergenceTime),
        HEADER_FIELD(WeaponDef, suppressTime), HEADER_FIELD(WeaponDef, maxRange), HEADER_FIELD(WeaponDef, fAnimHorRotateInc),
        HEADER_FIELD(WeaponDef, fPlayerPositionDist), HEADER_FIELD(WeaponDef, szUseHintString), HEADER_FIELD(WeaponDef, dropHintString),
        HEADER_FIELD(WeaponDef, iUseHintStringIndex), HEADER_FIELD(WeaponDef, dropHintStringIndex), HEADER_FIELD(WeaponDef, horizViewJitter),
        HEADER_FIELD(WeaponDef, vertViewJitter), HEADER_FIELD(WeaponDef, szScript), HEADER_FIELD(WeaponDef, fOOPosAnimLength),
        HEADER_FIELD(WeaponDef, minDamage), HEADER_FIELD(WeaponDef, minPlayerDamage), HEADER_FIELD(WeaponDef, fMaxDamageRange),
        HEADER_FIELD(WeaponDef, fMinDamageRange), HEADER_FIELD(WeaponDef, destabilizationRateTime), HEADER_FIELD(WeaponDef, destabilizationCurvatureMax),
        HEADER_FIELD(WeaponDef, destabilizeDistance), HEADER_FIELD(WeaponDef, locationDamageMultipliers), HEADER_FIELD(WeaponDef, fireRumble),
        HEADER_FIELD(WeaponDef, meleeImpactRumble), HEADER_FIELD(WeaponDef, adsDofStart), HEADER_FIELD(WeaponDef, adsDofEnd),
    };
    Linker_ClearHeaderPadding(data, sizeof(WeaponDef), spans, sizeof(spans) / sizeof(spans[0]));
}

static void Linker_ClearPadding_FxElemDef(void *data)
{
    static const size_t spans[][2] = {
        HEADER_FIELD(FxElemDef, flags), HEADER_FIELD(FxElemDef, spawn), HEADER_FIELD(FxElemDef, spawnRange),
        HEADER_FIELD(FxElemDef, fadeInRange), HEADER_FIELD(FxElemDef, fadeOutRange), HEADER_FIELD(FxElemDef, spawnFrustumCullRadius),
        HEADER_FIELD(FxElemDef, spawnDelayMsec), HEADER_FIELD(FxElemDef, lifeSpanMsec), HEADER_FIELD(FxElemDef, spawnOrigin),
        HEADER_FIELD(FxElemDef, spawnOffsetRadius), HEADER_FIELD(FxElemDef, spawnOffsetHeight), HEADER_FIELD(FxElemDef, spawnAngles),
        HEADER_FIELD(FxElemDef, angularVelocity), HEADER_FIELD(FxElemDef, initialRotation), HEADER_FIELD(FxElemDef, gravity),
        HEADER_FIELD(FxElemDef, reflectionFactor), HEADER_FIELD(FxElemDef, atlas), HEADER_FIELD(FxElemDef, elemType),
        HEADER_FIELD(FxElemDef, visualCount), HEADER_FIELD(FxElemDef, velIntervalCount), HEADER_FIELD(FxElemDef, visStateIntervalCount),
        HEADER_FIELD(FxElemDef, velSamples), HEADER_FIELD(FxElemDef, visSamples), HEADER_FIELD(FxElemDef, visuals),
        HEADER_FIELD(FxElemDef, collMins), HEADER_FIELD(FxElemDef, collMaxs), HEADER_FIELD(FxElemDef, effectOnImpact),
        HEADER_FIELD(FxElemDef, effectOnDeath), HEADER_FIELD(FxElemDef, effectEmitted), HEADER_FIELD(FxElemDef, emitDist),
        HEADER_FIELD(FxElemDef, emitDistVariance), HEADER_FIELD(FxElemDef, trailDef), HEADER_FIELD(FxElemDef, sortOrder),
        HEADER_FIELD(FxElemDef, lightingFrac), HEADER_FIELD(FxElemDef, useItemClip), HEADER_FIELD(FxElemDef, unused),
    };
    Linker_ClearHeaderPadding(data, sizeof(FxElemDef), spans, sizeof(spans) / sizeof(spans[0]));
    memset((unsigned char *)data + offsetof(FxElemDef, unused), 0, sizeof(((FxElemDef *)0)->unused));
}

static void Linker_ClearPadding_windowDef_t(void *data)
{
    static const size_t spans[][2] = {
        HEADER_FIELD(windowDef_t, name), HEADER_FIELD(windowDef_t, rect), HEADER_FIELD(windowDef_t, rectClient),
        HEADER_FIELD(windowDef_t, group), HEADER_FIELD(windowDef_t, style), HEADER_FIELD(windowDef_t, border),
        HEADER_FIELD(windowDef_t, ownerDraw), HEADER_FIELD(windowDef_t, ownerDrawFlags), HEADER_FIELD(windowDef_t, borderSize),
        HEADER_FIELD(windowDef_t, staticFlags), HEADER_FIELD(windowDef_t, dynamicFlags), HEADER_FIELD(windowDef_t, nextTime),
        HEADER_FIELD(windowDef_t, foreColor), HEADER_FIELD(windowDef_t, backColor), HEADER_FIELD(windowDef_t, borderColor),
        HEADER_FIELD(windowDef_t, outlineColor), HEADER_FIELD(windowDef_t, background),
    };
    Linker_ClearHeaderPadding(data, sizeof(windowDef_t), spans, sizeof(spans) / sizeof(spans[0]));
}

static void Linker_ClearPadding_statement_s(void *data)
{
    static const size_t spans[][2] = {
        HEADER_FIELD(statement_s, numEntries), HEADER_FIELD(statement_s, entries),
    };
    Linker_ClearHeaderPadding(data, sizeof(statement_s), spans, sizeof(spans) / sizeof(spans[0]));
}

static void Linker_ClearPadding_itemDef_s(void *data)
{
    static const size_t spans[][2] = {
        HEADER_FIELD(itemDef_s, window), HEADER_FIELD(itemDef_s, textRect), HEADER_FIELD(itemDef_s, type),
        HEADER_FIELD(itemDef_s, dataType), HEADER_FIELD(itemDef_s, alignment), HEADER_FIELD(itemDef_s, fontEnum),
        HEADER_FIELD(itemDef_s, textAlignMode), HEADER_FIELD(itemDef_s, textalignx), HEADER_FIELD(itemDef_s, textaligny),
        HEADER_FIELD(itemDef_s, textscale), HEADER_FIELD(itemDef_s, textStyle), HEADER_FIELD(itemDef_s, gameMsgWindowIndex),
        HEADER_FIELD(itemDef_s, gameMsgWindowMode), HEADER_FIELD(itemDef_s, text), HEADER_FIELD(itemDef_s, itemFlags),
        HEADER_FIELD(itemDef_s, parent), HEADER_FIELD(itemDef_s, mouseEnterText), HEADER_FIELD(itemDef_s, mouseExitText),
        HEADER_FIELD(itemDef_s, mouseEnter), HEADER_FIELD(itemDef_s, mouseExit), HEADER_FIELD(itemDef_s, action),
        HEADER_FIELD(itemDef_s, onAccept), HEADER_FIELD(itemDef_s, onFocus), HEADER_FIELD(itemDef_s, leaveFocus),
        HEADER_FIELD(itemDef_s, dvar), HEADER_FIELD(itemDef_s, dvarTest), HEADER_FIELD(itemDef_s, onKey),
        HEADER_FIELD(itemDef_s, enableDvar), HEADER_FIELD(itemDef_s, dvarFlags), HEADER_FIELD(itemDef_s, focusSound),
        HEADER_FIELD(itemDef_s, special), HEADER_FIELD(itemDef_s, cursorPos), HEADER_FIELD(itemDef_s, typeData),
        HEADER_FIELD(itemDef_s, imageTrack), HEADER_FIELD(itemDef_s, visibleExp), HEADER_FIELD(itemDef_s, textExp),
        HEADER_FIELD(itemDef_s, materialExp), HEADER_FIELD(itemDef_s, rectXExp), HEADER_FIELD(itemDef_s, rectYExp),
        HEADER_FIELD(itemDef_s, rectWExp), HEADER_FIELD(itemDef_s, rectHExp), HEADER_FIELD(itemDef_s, forecolorAExp),
    };
    Linker_ClearHeaderPadding(data, sizeof(itemDef_s), spans, sizeof(spans) / sizeof(spans[0]));
    Linker_ClearPadding_windowDef_t((unsigned char *)data + offsetof(itemDef_s, window));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(itemDef_s, visibleExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(itemDef_s, textExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(itemDef_s, materialExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(itemDef_s, rectXExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(itemDef_s, rectYExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(itemDef_s, rectWExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(itemDef_s, rectHExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(itemDef_s, forecolorAExp));
}

static void Linker_ClearPadding_menuDef_t(void *data)
{
    static const size_t spans[][2] = {
        HEADER_FIELD(menuDef_t, window), HEADER_FIELD(menuDef_t, font), HEADER_FIELD(menuDef_t, fullScreen),
        HEADER_FIELD(menuDef_t, itemCount), HEADER_FIELD(menuDef_t, fontIndex), HEADER_FIELD(menuDef_t, cursorItem),
        HEADER_FIELD(menuDef_t, fadeCycle), HEADER_FIELD(menuDef_t, fadeClamp), HEADER_FIELD(menuDef_t, fadeAmount),
        HEADER_FIELD(menuDef_t, fadeInAmount), HEADER_FIELD(menuDef_t, blurRadius), HEADER_FIELD(menuDef_t, onOpen),
        HEADER_FIELD(menuDef_t, onClose), HEADER_FIELD(menuDef_t, onESC), HEADER_FIELD(menuDef_t, onKey),
        HEADER_FIELD(menuDef_t, visibleExp), HEADER_FIELD(menuDef_t, allowedBinding), HEADER_FIELD(menuDef_t, soundName),
        HEADER_FIELD(menuDef_t, imageTrack), HEADER_FIELD(menuDef_t, focusColor), HEADER_FIELD(menuDef_t, disableColor),
        HEADER_FIELD(menuDef_t, rectXExp), HEADER_FIELD(menuDef_t, rectYExp), HEADER_FIELD(menuDef_t, items),
    };
    Linker_ClearHeaderPadding(data, sizeof(menuDef_t), spans, sizeof(spans) / sizeof(spans[0]));
    Linker_ClearPadding_windowDef_t((unsigned char *)data + offsetof(menuDef_t, window));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(menuDef_t, visibleExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(menuDef_t, rectXExp));
    Linker_ClearPadding_statement_s((unsigned char *)data + offsetof(menuDef_t, rectYExp));
}

#undef HEADER_FIELD
