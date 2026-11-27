#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "NativeWadScript.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "NativeWadCheckpoint.generated.h"

class UTexture2D;
class UAudioComponent;

USTRUCT()
struct FWadSavedEncounter
{
    GENERATED_BODY()

    UPROPERTY()
    FTransform Transform;

    UPROPERTY()
    int32 Health = 0;
    UPROPERTY()
    int32 Poison = 0;
    UPROPERTY()
    double PoisonTime = 5.0;
    UPROPERTY()
    double SlowSeconds = 0.0;
    UPROPERTY()
    double EarthboundSeconds = 0.0;
    UPROPERTY()
    int32 CreatureLife = 0;
    UPROPERTY()
    FName BerserkTarget;
    UPROPERTY()
    double BerserkSeconds = 0.0;
    UPROPERTY()
    int32 BerserkTargetLife = 0;
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
    // Absent in older saves. MAX_int32 represents the end of an activity.
    UPROPERTY()
    int32 SourceCommandOffset = INDEX_NONE;

};

USTRUCT()
struct FWadSavedDrop
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Type = 0;
    UPROPERTY()
    FVector Position = FVector::ZeroVector;
    UPROPERTY()
    int32 AmmoIndex = INDEX_NONE;
    UPROPERTY()
    int32 AmmoQuantity = 0;
    // Additive source payload: absent fields in earlier native saves retain legacy ammo amounts.
    UPROPERTY()
    bool bAccessoryDataRecorded = false;
    UPROPERTY()
    uint16 AccessoryData = 0;
};

USTRUCT()
struct FWadLevelCheckpoint
{
    GENERATED_BODY()

    static constexpr int32 AndrewNecklaceRevision = 1;
    static constexpr int32 ElmoreKeysRevision = 2;
    static constexpr int32 AuthoredItemsRevision = 3;
    static constexpr int32 CurrentPickupRevision = AuthoredItemsRevision;

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
    TArray<FWadSavedDrop> DroppedItems;

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
    TSet<int32> ScriptLockedDoors;

    UPROPERTY()
    TMap<int32, int32> DoorLockStrengths;

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
    int32 CopperRemainder = 0;

    UPROPERTY()
    int64 BankCopper = 0;

    static constexpr int64 WalletLimit = int64(MAX_int32) * 100 + 99;
    int64 GetWalletCopper() const { return int64(Gold) * 100 + CopperRemainder; }
    void SetWalletCopper(int64 Copper)
    {
        Copper = FMath::Clamp(Copper, int64(0), WalletLimit);
        Gold = int32(Copper / 100);
        CopperRemainder = int32(Copper % 100);
    }

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
    static constexpr int32 CurrentVersion = 3;
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
    FString PlayerNotes;

    // Older snapshots had only fully revealed descriptions. Migration retains knowledge of carried types.
    UPROPERTY()
    bool bItemIdentificationRecorded = false;
    UPROPERTY()
    TSet<int32> IdentifiedItemTypes;

    UPROPERTY()
    bool bTown = false;
};

UCLASS()
class UNativeWadTravelState : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    void UpdateBackgroundMusic(UWorld& World);
    bool IsBackgroundMusicPlaying() const;
    FName BackgroundMusicName() const;
    TMap<FString, FWadLevelCheckpoint> VisitedLevels;
    TSet<FString> CompletedLevels;
    FWadQuestProgress Quest;
    TMap<int32, TSoftObjectPtr<UTexture2D>> JournalPages;
    FString PlayerNotes;
    bool bItemIdentificationRecorded = false;
    TSet<int32> IdentifiedItemTypes;
    int32 ArrivingFromMap = 0;

private:
    UPROPERTY(Transient)
    TObjectPtr<UAudioComponent> BackgroundMusic;
    FSoftObjectPath RequestedMusic;
};
