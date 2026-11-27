#pragma once

#include "Engine/DataAsset.h"
#include "NativeWadPlayer.h"
#include "NativeWadTown.generated.h"

class UTexture2D;
class UCanvas;

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

void DrawWadBitmapText(UCanvas& Canvas, const FWadTownFont& Font, TConstArrayView<FColor> Palette,
    const FString& Text, FVector2D Origin, double Scale, int32 Width, int32 Rows, int32 Scroll,
    bool bCenter, uint8 Ink);

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
    TMap<int32, int32> StoreStock;
    UPROPERTY()
    TMap<int32, int32> BankStock;
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
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    void SelectQuest(int32 Direction);
    void OpenQuest();
    void OpenCharacterPicker();
    void ConfirmAction();
    void CancelAction();
    void Scroll(int32 Direction);
    int32 GetScrollLine() const { return ScrollLine; }
    void OpenService(FName Name);
    bool RentRoom(int32 Room);
    void ClickAction(const FVector2D& Position);
    AAORIGINAL_API FVector2D ServiceBagItemCenter(int32 Slot) const;
    AAORIGINAL_API FVector2D ServiceStockItemCenter(int32 Type) const;
    AAORIGINAL_API int32 ServiceStockItemPage(int32 Type) const;
    int32 GetServiceStockPage() const{return StockPage;}

private:
    void StartQuest();
    void DrawService(const UNativeWadTownData& Content, const struct FWadQuestProgress& Progress, const ANativeWadPlayer& Player);
    void ClickService(const UNativeWadTownData& Content, struct FWadQuestProgress& Progress, const FVector2D& Point);
    bool IsBankOpen() const { return Service == TEXT("Bank"); }
    enum class EServiceBanner : uint8 { Bag, Equipment, Closed };
    FName Service;
    int32 StockPage = 0;
    int32 BagPage = 0;
    EServiceBanner ServiceBanner = EServiceBanner::Bag;
    bool bDialogue = false;
    bool bCharacterPicker = false;
    int32 ChosenClass = 0;
    int32 ResultPage = 0;
    int32 ScrollLine = 0;
};
