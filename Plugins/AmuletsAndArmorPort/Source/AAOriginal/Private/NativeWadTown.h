#pragma once

#include "Engine/DataAsset.h"
#include "NativeWadPlayer.h"
#include "NativeWadTown.generated.h"

class UTexture2D;

USTRUCT()
struct FWadTownQuest
{
    GENERATED_BODY()

    UPROPERTY()
    FString Title;
    UPROPERTY()
    FString Goal;
    UPROPERTY()
    FString Description;
    UPROPERTY()
    FString NotesHeading;
    UPROPERTY()
    FString Dialogue;
    UPROPERTY()
    FString Success;
    UPROPERTY()
    FString Failure;
    UPROPERTY()
    FString TurnInMessage;
    UPROPERTY()
    FString GoldMessage;
    UPROPERTY()
    int32 QuestItem = 0;
    UPROPERTY()
    int32 Gold = 0;
    UPROPERTY()
    int32 Experience = 0;
    UPROPERTY()
    bool bKeepItem = true;
    UPROPERTY()
    TArray<int32> Maps;
    UPROPERTY()
    TArray<FString> MapNotes;
};

USTRUCT()
struct FWadTownFont
{
    GENERATED_BODY()

    UPROPERTY()
    TObjectPtr<UTexture2D> Atlas;
    UPROPERTY()
    TArray<uint8> Widths;
    UPROPERTY()
    int32 Height = 0;
};

UCLASS()
class UNativeWadTownData : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TArray<FWadTownQuest> Quests;
    UPROPERTY()
    TMap<FName, TObjectPtr<UTexture2D>> Pictures;
    UPROPERTY()
    FWadTownFont Medium;
    UPROPERTY()
    FWadTownFont Tiny;
    UPROPERTY()
    TArray<FColor> Palette;
};

UCLASS()
class ANativeWadTownContent : public AActor
{
    GENERATED_BODY()

public:
    UPROPERTY()
    TObjectPtr<UNativeWadTownData> Data;
};

namespace NativeWadTown
{
    inline constexpr int32 Form = 20004;
    inline constexpr const TCHAR* Package = TEXT("/Game/Maps/WadTown");
    inline constexpr const TCHAR* ObjectPath = TEXT("/Game/Maps/WadTown.WadTown");
    int32 MapNumber(const FString& Package);
    FString MapPackage(int32 Number);
    const UNativeWadTownData* FindContent(const UWorld& World);
}

UCLASS()
class ANativeWadTownGameMode : public ANativeWadGameMode
{
    GENERATED_BODY()

public:
    ANativeWadTownGameMode();
};

UCLASS()
class ANativeWadTownHUD : public AHUD
{
    GENERATED_BODY()

public:
    virtual void DrawHUD() override;
    void SelectQuest(int32 Direction);
    void OpenQuest();
    void ConfirmAction();
    void CancelAction();
    void Scroll(int32 Direction);
    int32 GetScrollLine() const { return ScrollLine; }
    void OpenService(FName Name);
    void ClickAction(const FVector2D& Position);

private:
    void StartQuest();
    bool bDialogue = false;
    int32 ResultPage = 0;
    int32 ScrollLine = 0;
};
