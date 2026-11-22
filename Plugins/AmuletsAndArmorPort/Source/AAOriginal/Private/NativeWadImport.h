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
bool RefreshNativeWadSurfaces(const FString& MapPackage, FOutputDevice& Output);
#endif
