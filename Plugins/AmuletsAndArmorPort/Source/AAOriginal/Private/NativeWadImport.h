#pragma once

#include "CoreMinimal.h"

struct FPostProcessSettings;
class UWorld;

void ConfigureWadLumen(FPostProcessSettings& Settings);

#if WITH_EDITOR
bool SaveNativeWadLevel(const FString& MapPackage, FOutputDevice& Output);
bool SaveNativeWadDoors(UWorld* World, FOutputDevice& Output);
bool SaveNativeWadGameplay(UWorld* World, FOutputDevice& Output);
#endif
