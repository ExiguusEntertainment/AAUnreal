#pragma once

#include "NativeWadTown.h"
#include "NativeWadRules.h"
#include "NativeWadHudData.generated.h"

class UMaterialInterface;
class USoundWave;

USTRUCT()
struct FWadInventoryDetails
{
    GENERATED_BODY()

    UPROPERTY()
    FString Description;

    // OBJDESC is shown before the character learns this full object type; Description retains OBJDESC2.
    UPROPERTY()
    FString UnidentifiedDescription;

    UPROPERTY()
    bool bIdentifyOnUse = false;

    // Source IDENTIFY READIED=1, IDENTIFY ALL=2. Identification is character knowledge, not an item timer.
    UPROPERTY()
    int32 IdentificationScope = 0;

    UPROPERTY()
    int32 StackLimit = 1;
    UPROPERTY()
    int32 SourceCategory = 13; // Original EQUIP_OBJECT_TYPE order, for merchant inventory pages.

    UPROPERTY()
    int32 ValueCopper = 0;

    UPROPERTY()
    bool bStoreBuys = false;

    UPROPERTY()
    bool bBankBuys = false;

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

    // EFFECT_TAKE_*_DAMAGE is distinct from MOD PLAYER HEALTH: it uses the
    // original typed resistance/armor/poison path at the authored trigger.
    UPROPERTY()
    int32 UseDamagePower = 0;
    UPROPERTY()
    uint8 UseDamageMask = 0;
    UPROPERTY()
    int32 ReadyDamagePower = 0;
    UPROPERTY()
    uint8 ReadyDamageMask = 0;

    UPROPERTY()
    int32 Status = INDEX_NONE;
    UPROPERTY()
    int32 StatusTicks = 0;
    UPROPERTY()
    int32 StatusPower = 0;
    UPROPERTY()
    int32 WandProjectile = INDEX_NONE;
    UPROPERTY()
    int32 WandManaCost = 0;

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
    // NeckSlot for an amulet, Ring1Slot for any ring, INDEX_NONE for other items.
    UPROPERTY()
    int32 JewelrySlot = INDEX_NONE;
    UPROPERTY()
    int32 ArmorValue = 0;
    UPROPERTY()
    int32 ArmorClass = 0;

    UPROPERTY()
    uint16 UsableClasses = NativeWadRules::AllClasses;

    UPROPERTY()
    TSoftObjectPtr<UMaterialInterface> WorldMaterial;
    UPROPERTY()
    FIntPoint SpriteSize = FIntPoint::ZeroValue;
    UPROPERTY()
    int32 WorldRadius = 0;
    UPROPERTY()
    uint16 WorldAttributes = 0;
};

USTRUCT()
struct FWadStartingEquipment
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<int32> Items;

    UPROPERTY()
    TArray<int32> Ammo;
};

USTRUCT()
struct FWadCreatureDescription
{
    GENERATED_BODY()

    UPROPERTY()
    FString Name;

    UPROPERTY()
    FString Description;
};

UCLASS()
class UNativeWadHudData : public UDataAsset
{
    GENERATED_BODY()

public:
    static constexpr const TCHAR* ObjectPath = TEXT("/Game/WadInterface/Graphics/ClassicHud.ClassicHud");
    static const UNativeWadHudData* LoadCatalog();
    AAORIGINAL_API FIntPoint GetItemBitmapSize(int32 Type) const;

    UPROPERTY()
    TMap<FName, TObjectPtr<UTexture2D>> Pictures;

    UPROPERTY()
    TMap<int32, TObjectPtr<UTexture2D>> Items;

    UPROPERTY()
    TMap<int32, int32> ItemWeights;

    UPROPERTY()
    TMap<int32, FWadInventoryDetails> ItemDetails;

    UPROPERTY()
    TArray<FWadStartingEquipment> StartingEquipment;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> PlasmaProjectile;

    // Shared cooked parent only. Player power belongs to each camera's private MID.
    UPROPERTY()
    TObjectPtr<UMaterialInterface> NightVisionPostProcess;

    UPROPERTY()
    TMap<int32, TObjectPtr<UMaterialInterface>> AmmoProjectiles;
    UPROPERTY()
    TMap<int32, TObjectPtr<UMaterialInterface>> WandProjectileMaterials;
    UPROPERTY()
    TMap<FName, TSoftObjectPtr<USoundWave>> MusicByMap;

    UPROPERTY()
    TMap<int32, FWadCreatureDescription> CreatureDescriptions;

    // Finished original NOTEdddd bitmaps, independent of which map originally granted the page.
    UPROPERTY()
    TMap<int32, TObjectPtr<UTexture2D>> JournalPageCatalog;

    UPROPERTY()
    TArray<FColor> Palette;

    UPROPERTY()
    FWadTownFont Tiny;

    UPROPERTY()
    FWadTownFont Medium;
};
