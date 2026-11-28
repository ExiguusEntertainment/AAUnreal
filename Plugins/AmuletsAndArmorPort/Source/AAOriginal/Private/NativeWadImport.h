#pragma once

#include "CoreMinimal.h"

struct FPostProcessSettings;
class UWorld;
class UTexture2D;
class UMaterialInterface;

void ConfigureWadLumen(FPostProcessSettings& Settings);

#if WITH_EDITOR
AAORIGINAL_API bool MatchesNativeWadShieldFrameTexture(UTexture2D& Texture, bool bLegacy);
AAORIGINAL_API int32 RebindNativeWadShieldFrameMaterials(UWorld& World, UMaterialInterface& OldMaterial, UMaterialInterface& NewMaterial);
bool RefreshNativeWadShieldInitialFrame(const FString& MapPackage, bool bApply, FOutputDevice& Output);
bool SaveNativeWadTown(FOutputDevice& Output);
bool SaveNativeWadHud(FOutputDevice& Output);
bool SaveNativeWadLevel(const FString& MapPackage, const FString& WadPath, FOutputDevice& Output);
bool SaveNativeWadDoors(UWorld* World, FOutputDevice& Output);
bool SaveNativeWadGameplay(UWorld* World, const FString& WadPath, FOutputDevice& Output, bool bPickupsOnly);
bool SaveNativeWadItems(const FString& MapPackage, FOutputDevice& Output);
bool SaveNativeWadItemRules(FOutputDevice& Output);
bool SaveNativeWadNightVision(FOutputDevice& Output);
bool RefreshNativeWadSurfaces(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadSkeletonKnights(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadL151Encounters(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadTerrainFriction(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadAmbientAudio(const FString& MapPackage, const FString& WadPath, FOutputDevice& Output);
bool RefreshNativeWadL41ScriptMissiles(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadPackedEncounterFamilies(const FString& MapPackage,FOutputDevice& Output);
bool RefreshNativeWadPackedDruids(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadAcidEnvironment(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadPickupMetadata(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadMissingMaterials(const FString& MapPackage, FOutputDevice& Output);
#endif

#if WITH_EDITOR
bool RefreshNativeWadOrdinaryScenery(const FString& MapPackage, const FString& SourceWadPath, FOutputDevice& Output);
#endif

#if WITH_EDITOR
bool RefreshNativeWadSourceLighting(const FString& MapPackage, const FString& WadPath, FOutputDevice& Output, bool bPreflightOnly=false);
#endif

#if WITH_EDITOR
bool CreateNativeWadSourceLightingCandidate(const FString& SourcePackage,const FString& CandidatePackage,
    const FString& WadPath,FOutputDevice& Output);
#endif
