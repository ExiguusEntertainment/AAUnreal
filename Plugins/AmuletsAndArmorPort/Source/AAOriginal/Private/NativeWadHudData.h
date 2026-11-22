#pragma once

#include "NativeWadTown.h"
#include "NativeWadHudData.generated.h"

class UMaterialInterface;

USTRUCT()
struct FWadInventoryDetails
{
    GENERATED_BODY()

    UPROPERTY()
    FString Description;

    UPROPERTY()
    int32 StackLimit = 1;

    UPROPERTY()
    int32 Health = 0;
    UPROPERTY()
    int32 Mana = 0;
    UPROPERTY()
    int32 Food = 0;
    UPROPERTY()
    int32 Water = 0;
    UPROPERTY()
    int32 PoisonDivisor = 1;
    UPROPERTY()
    bool bUsable = false;
    UPROPERTY()
    bool bConsumed = false;

    UPROPERTY()
    int32 WeaponDamage = 0;
    UPROPERTY()
    int32 WeaponSpeed = 0;
    UPROPERTY()
    int32 HitFrame = 0;
    UPROPERTY()
    int32 SwingFrames = 0;

    UPROPERTY()
    int32 ArmorSlot = INDEX_NONE;
    UPROPERTY()
    int32 ArmorValue = 0;

    UPROPERTY()
    TSoftObjectPtr<UMaterialInterface> WorldMaterial;
    UPROPERTY()
    FIntPoint SpriteSize = FIntPoint::ZeroValue;
};

UCLASS()
class UNativeWadHudData : public UDataAsset
{
    GENERATED_BODY()

public:
    static constexpr const TCHAR* ObjectPath = TEXT("/Game/WadInterface/Graphics/ClassicHud.ClassicHud");
    static const UNativeWadHudData* LoadCatalog();

    UPROPERTY()
    TMap<FName, TObjectPtr<UTexture2D>> Pictures;

    UPROPERTY()
    TMap<int32, TObjectPtr<UTexture2D>> Items;

    UPROPERTY()
    TMap<int32, int32> ItemWeights;

    UPROPERTY()
    TMap<int32, FWadInventoryDetails> ItemDetails;

    UPROPERTY()
    TArray<FColor> Palette;

    UPROPERTY()
    FWadTownFont Tiny;
};
