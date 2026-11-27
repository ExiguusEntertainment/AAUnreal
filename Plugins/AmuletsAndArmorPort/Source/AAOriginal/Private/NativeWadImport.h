#pragma once

#include "CoreMinimal.h"

struct FPostProcessSettings;
class UWorld;

void ConfigureWadLumen(FPostProcessSettings& Settings);

#if WITH_EDITOR
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
bool RefreshNativeWadAcidEnvironment(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadPickupMetadata(const FString& MapPackage, FOutputDevice& Output);
bool RefreshNativeWadMissingMaterials(const FString& MapPackage, FOutputDevice& Output);
#endif

#if WITH_EDITOR
bool RefreshNativeWadOrdinaryScenery(const FString& MapPackage, const FString& SourceWadPath, FOutputDevice& Output);
#endif
