#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "NativeWadScript.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "NativeWadCheckpoint.generated.h"

class UTexture2D;

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
struct FWadSavedSlide
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Sector = INDEX_NONE;
    UPROPERTY()
    double From = 0.0;
    UPROPERTY()
    double To = 0.0;
    UPROPERTY()
    double Duration = 0.0;
    UPROPERTY()
    double Elapsed = 0.0;
    UPROPERTY()
    bool bCeiling = false;
    UPROPERTY()
    bool bPauseOnCrush = false;
    UPROPERTY()
    int32 CompletionActivity = INDEX_NONE;
};

USTRUCT()
struct FWadSavedDoor
{
    GENERATED_BODY()

    UPROPERTY()
    uint8 Phase = 0;
    UPROPERTY()
    double StartZ = 0.0;
    UPROPERTY()
    double Elapsed = 0.0;
};

USTRUCT()
struct FWadSavedScriptDelay
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Activity = 0;
    UPROPERTY()
    int32 NextCommand = 0;
    UPROPERTY()
    float SecondsRemaining = 0.0f;
    UPROPERTY()
    bool bHasPlayer = false;
};

USTRUCT()
struct FWadLevelCheckpoint
{
    GENERATED_BODY()

    static constexpr int32 CurrentPickupRevision = 1;

    UPROPERTY()
    TMap<int32, FVector2D> SectorHeights;

    UPROPERTY()
    TMap<int32, bool> Switches;

    UPROPERTY()
    TMap<int32, FName> ActivityWallTextures;

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
    TArray<FWadSavedSlide> Slides;

    UPROPERTY()
    TMap<int32, FWadSavedDoor> Doors;

    UPROPERTY()
    TArray<FWadSavedScriptDelay> Delays;

    UPROPERTY()
    TMap<int32, float> PendingPickupGenerators;

    UPROPERTY()
    TSet<int32> FiredPickupGenerators;

    UPROPERTY()
    TSet<int32> UnlockedItemDoors;

    UPROPERTY()
    TMap<int32, uint16> ScriptObjectAngles;

    UPROPERTY()
    TMap<int32, FWadCreatureGeneratorState> CreatureGenerators;

    UPROPERTY()
    int32 PickupRevision = 0;

    UPROPERTY()
    double Hour = 12.0;
};

USTRUCT()
struct FWadQuestProgress
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Selected = 0;

    UPROPERTY()
    int32 Active = INDEX_NONE;

    UPROPERTY()
    TMap<int32, int32> ResumeMaps;

    UPROPERTY()
    int32 Gold = 0;

    UPROPERTY()
    int32 ShopTier = 0;

    UPROPERTY()
    int32 ResultQuest = INDEX_NONE;

    UPROPERTY()
    bool bSucceeded = false;

    UPROPERTY()
    bool bShowResult = false;
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

    UPROPERTY()
    TMap<FString, FWadLevelCheckpoint> VisitedLevels;

    UPROPERTY()
    TSet<FString> CompletedLevels;

    UPROPERTY()
    FWadQuestProgress Quest;

    UPROPERTY()
    TMap<int32, TSoftObjectPtr<UTexture2D>> JournalPages;

    UPROPERTY()
    bool bTown = false;
};

UCLASS()
class UNativeWadTravelState : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    TMap<FString, FWadLevelCheckpoint> VisitedLevels;
    TSet<FString> CompletedLevels;
    FWadQuestProgress Quest;
    TMap<int32, TSoftObjectPtr<UTexture2D>> JournalPages;
    int32 ArrivingFromMap = 0;
};
