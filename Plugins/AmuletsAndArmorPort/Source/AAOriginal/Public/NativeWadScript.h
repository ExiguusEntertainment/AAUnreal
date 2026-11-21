#pragma once

#include "CoreMinimal.h"
#include "NativeWadScript.generated.h"

UENUM()
enum class EWadScriptOperation : uint8
{
    SetSwitch,
    Wait,
    Message,
    SlideFloor,
    SlideCeiling,
    SetVariable,
    CompareVariable,
    IfFlag,
    Jump,
    Call,
    Block,
    Unblock,
    PlaySound,
    PlayAreaSound,
    Earthquake,
    LaunchMissile,
    TeleportPlayer,
    IncrementVariable,
    ActivateGenerator,
    AddNumbers,
    SubtractNumbers,
    MultiplyNumbers,
    ReadObjectAngle,
    SetObjectAngle,
    RestoreVitals,
    ChangeSideTexture,
    AddJournalPage,
    ToggleWallSwitch,
    DeactivateGenerator
};

USTRUCT()
struct FWadScriptNumber
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Value = 0;

    UPROPERTY()
    bool bVariable = false;
};

USTRUCT()
struct FWadScriptCommand
{
    GENERATED_BODY()

    UPROPERTY()
    int32 CompletionActivity = INDEX_NONE;

    UPROPERTY()
    EWadScriptOperation Operation = EWadScriptOperation::Wait;

    UPROPERTY()
    int32 Target = 0;

    UPROPERTY()
    int32 Value = 0;

    UPROPERTY()
    int32 Ticks = 0;

    UPROPERTY()
    bool bPauseOnCrush = false;

    UPROPERTY()
    FString Text;

    UPROPERTY()
    FString SwitchDownTexture;

    UPROPERTY()
    int32 SwitchUpActivity = INDEX_NONE;

    UPROPERTY()
    int32 SwitchDownActivity = INDEX_NONE;

    UPROPERTY()
    FIntVector Position = FIntVector::ZeroValue;

    UPROPERTY()
    FIntVector Aim = FIntVector::ZeroValue;

    UPROPERTY()
    FWadScriptNumber Left;

    UPROPERTY()
    FWadScriptNumber Right;
};

USTRUCT()
struct FWadSoundSample
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<uint8> UnsignedSamples;
};

USTRUCT()
struct FWadScriptActivity
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Id = 0;

    UPROPERTY()
    TArray<FWadScriptCommand> Commands;
};

USTRUCT()
struct FWadCreatureGeneratorState
{
    GENERATED_BODY()

    UPROPERTY()
    int32 Released = 0;

    UPROPERTY()
    float Remaining = 0.0f;

    UPROPERTY(SaveGame)
    bool bPaused = false;
};

#if WITH_EDITOR
bool ImportWadScriptActivities(const FString& Filename, TConstArrayView<int32> ActivityIds, int32 SectorCount,
    int32 SideCount, TArray<FWadScriptActivity>& Out, FOutputDevice& Output);
bool ImportWadSounds(const FString& Filename, const TArray<FWadScriptActivity>& Activities,
    TMap<int32, FWadSoundSample>& Sounds, FOutputDevice& Output, TConstArrayView<int32> UnavailableSounds = {});
bool ImportWadSoundSamples(const FString& Filename, TSet<int32> Required,
    TMap<int32, FWadSoundSample>& Sounds, FOutputDevice& Output, TConstArrayView<int32> UnavailableSounds = {});
#endif
