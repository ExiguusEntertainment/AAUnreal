#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "NativeWadCheckpoint.generated.h"

USTRUCT()
struct FWadSavedEncounter
{
    GENERATED_BODY()

    UPROPERTY()
    FTransform Transform;

    UPROPERTY()
    int32 Health = 0;
};

USTRUCT()
struct FWadLevelCheckpoint
{
    GENERATED_BODY()

    UPROPERTY()
    TMap<int32, FVector2D> SectorHeights;

    UPROPERTY()
    TMap<int32, bool> Switches;

    UPROPERTY()
    TMap<int32, int32> LightLevels;

    UPROPERTY()
    TArray<int32> Variables;

    UPROPERTY()
    TArray<uint8> Flags;

    UPROPERTY()
    TArray<FName> RemainingPickups;

    UPROPERTY()
    TMap<FName, FWadSavedEncounter> Encounters;

    UPROPERTY()
    double Hour = 12.0;
};

UCLASS()
class UNativeWadCheckpoint : public USaveGame
{
    GENERATED_BODY()

public:
    static constexpr int32 CurrentVersion = 1;
    static constexpr const TCHAR* Slot = TEXT("NativeWadCheckpoint");

    UPROPERTY()
    int32 Version = CurrentVersion;

    UPROPERTY()
    FString LevelPackage;

    UPROPERTY()
    FString PlayerOptions;

    UPROPERTY()
    FVector Position = FVector::ZeroVector;

    UPROPERTY()
    FRotator View = FRotator::ZeroRotator;

    UPROPERTY()
    double NeedsTime = 0.0;

    UPROPERTY()
    FWadLevelCheckpoint Level;
};
