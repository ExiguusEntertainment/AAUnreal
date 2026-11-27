#pragma once

#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "NativeWadRules.h"
#include "NativeWadPlayerEffects.h"
#include "NativeWadPlayer.generated.h"

class UCameraComponent;
class ANativeWadLevel;
class UNativeWadHudData;
class FNativeWadNotesEditor;
class SWidget;
class UMaterialInterface;
struct FWadInventoryDetails;

UCLASS()
class AAORIGINAL_API ANativeWadPlayer : public ACharacter
{
    GENERATED_BODY()

public:
    explicit ANativeWadPlayer(const FObjectInitializer& ObjectInitializer);
    virtual void BeginPlay() override;
    virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FVector GetPawnViewLocation() const override;
    virtual void Jump() override;
    int32 GetWadJumpPower() const;
    bool TryWadLeap(int32 ExtraPower);
    bool BlinkWadForward(int32 Distance);
    bool HasLowGravity() const { return !CanFly() && GetStatusPower(NativeWadRules::LowGravityStatus) > 0; }

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool Use();
    int32 GetHealth() const { return Health; }
    bool IsDefeated() const { return bDefeated; }
    int32 GetMana() const { return Mana; }
    int32 GetFood() const { return Food; }
    int32 GetWater() const { return Water; }
    int32 GetExperience() const { return Experience; }
    int32 GetEquippedWeapon() const;
    uint8 GetMeleeDamageMask() const;
    int32 GetArmorRating() const;
    int32 GetArmorClass(int32 Location) const;
    uint8 GetResistanceMask() const;
    bool IsWornInventorySlot(int32 Slot) const;
    int32 GetJewelryInventorySlot(int32 Location) const;
    bool HasEquippedItem(int32 Type) const;
    bool CanUseWadItem(int32 Type) const;
    int32 GetSuccessfulCasts() const { return SuccessfulCasts; }
    int32 GetMaxHealth() const { return MaxHealth; }
    int32 GetMaxMana() const { return MaxMana; }
    int32 GetCharacterLevel() const { return CharacterLevel; }
    int32 GetExperienceNeeded() const { return ExperienceNeeded; }
    TConstArrayView<int32> GetAttributes() const { return Attributes; }
    int32 GetEffectiveAttribute(int32 Attribute) const;
    int32 GetHealthRegeneration() const;
    int32 GetManaRegeneration() const;
    int32 GetLastMeleeRawDamage() const { return LastMeleeRawDamage; }
    bool WasLastMeleeCritical() const { return bLastMeleeCritical; }
    int32 GetCharacterClass() const { return CharacterClass; }
    bool CanChooseWadClass() const;
    bool ChooseWadClass(int32 Index);
    int32 GetItemCount(int32 Type) const;
    int32 GetAmmoCount(int32 Index) const;
    int32 GetSelectedAmmo() const { return SelectedAmmo; }
    int32 GetAmmoReserve(int32 Index) const { return AmmoReserve[Index]; }
    void SelectAmmo(int32 Index);
    bool CollectAmmo(int32 Index, int32 Quantity);
    void RetrieveAmmo(int32 Index);
    const TArray<int32>& GetCarriedItems() const { return CarriedItems; }
    int32 GetReadySlot() const { return ReadySlot; }
    int32 GetReadyItem() const;
    FString GetInventoryItemDescription(int32 Type, bool bMerchantPreview = false) const;
    bool HasIdentifiedItemType(int32 Type) const;
    bool IdentifyItemType(int32 Type);
    void IdentifyEquippedItems();
    void IdentifyAllCarriedItems();
    void InitializeItemIdentification(bool bKnowCarriedItems);
    void RestoreItemIdentification(const TSet<int32>& Types, bool bRecorded);
    const TSet<int32>& GetIdentifiedItemTypes() const { return IdentifiedItemTypes; }
    bool HasItemIdentificationState() const { return bItemIdentificationInitialized; }
    int32 GetSelectedInventorySlot() const { return SelectedInventorySlot; }
    // One owned object may live in the mouse hand even when every banner is closed.
    int32 GetMouseHandSlot() const { return SelectedInventorySlot; }
    int32 GetMouseHandItem() const { return CarriedItems.IsValidIndex(SelectedInventorySlot) ? CarriedItems[SelectedInventorySlot] : 0; }
    bool TakeInventoryItem(int32 Slot);
    bool CreateWadItemInMouseHand(int32 Type);
    void StoreMouseHandInInventory();
    bool SwapMouseAndReady();
    void ClickArmorSlot(int32 Location);
    bool IsInventoryOpen() const { return bInventoryOpen; }
    bool IsMouseLook() const { return bMouseLook; }
    void ClickInventorySlot(int32 Slot);
    void ClickJewelrySlot(int32 Location);
    FString GetRunes() const { return Runes; }
    bool HasRune(int32 Rune) const;
    bool ShouldShowEncounterHUD() const;
    FString GetInteractionMessage() const;
    void ShowInteractionMessage(FString Message);
    void ReceiveEncounterHit(int32 Damage);
    void ReceivePhysicalHit(int32 Damage);
    void ReceiveTypedHit(int32 Damage, uint8 DamageMask);
    void ReceiveTrapPoison(int32 Damage);
    int32 GetPoisonLevel() const { return PoisonLevel; }
    void ReceiveTrapParalysis();
    void RestoreWadVital(int32 Effect, int32 Amount);
    bool IsParalyzed() const { return ParalysisSeconds > 0.0; }
    bool CanFly() const { return FlightSeconds > 0.0 || (HasEquippedItem(NativeWadRules::FlyingAmulet) && !bReadiedFlightSuppressed); }
    bool HasFeatherfall() const { return FeatherfallSeconds > 0.0 || HasEquippedItem(NativeWadRules::FeatherfallAmulet); }
    bool IsWaterWalking() const { return bOnWaterSurface; }
    bool IsConcealed() const { return bConcealed; }
    int32 GetStatusPower(NativeWadRules::EItemStatus Status) const;
    int32 GetOwnedEffectCount() const { return TimedEffects.Num(); }
    double GetOwnedEffectRemaining(const FString& EffectOwner) const { return TimedEffects.Remaining(EffectOwner); }
    void StartWadEarthquake(double Duration);
    bool IsWadEarthquakeActive() const { return QuakeTime > 0.0; }
    bool SaveWadCheckpoint(const FString& Slot = FString()) const;
    bool HasRestoredWadCheckpoint() const { return bCheckpointRestored; }
    bool HasCompletedWadLevel(const FString& Name) const;
    void TravelToWadLevel(const TSoftObjectPtr<UWorld>& Destination);
    UFUNCTION(Exec)
    void WadSave();
    UFUNCTION(Exec)
    void WadLoad();
    bool CollectItem(int32 Type);
    bool ConsumeItem(int32 Type);
    bool TradeWadTownItem(int32 Selection, bool bSelling, bool bBank);
    bool RentWadRoom(int32 Room);
    void AwardExperience(int32 Amount);

    static constexpr float CapsuleRadius = 10.0f;
    static constexpr float CapsuleHalfHeight = 27.0f;

private:
    friend class ANativeWadHUD;
    friend class ANativeWadTownHUD;
    bool bWadCharacterCreated = false;
    bool bDefeated = false;
    void DefeatWadPlayer(int32 Damage);
    void ToggleFinances();
    void RetrieveCoin(int32 Type);
    void ReturnSelectedCoin();
    void MoveInViewDirection(float Forward, float Right);
    void JumpOrRevive();
    void EnterRuneKey(FKey Key);
    void EnterRune(int32 Rune);
    void DropDefeatedItems();
    void UsePressed();
    void AttackPressed();
    void FireCrossbow();
    void FireWand(const NativeWadRules::FWadWandProjectile& Projectile, int32 ManaCost);
    void ResolveMeleeHit(int32 Damage);
    const FWadInventoryDetails* GetItemRules(int32 Type) const;
    bool RemoveInventorySlot(int32 Slot);
    void EquipArmor(int32 Slot, const FWadInventoryDetails& Details);
    bool EquipJewelry(int32 Slot, const FWadInventoryDetails& Details, int32 Location = INDEX_NONE);
    void UseConsumable(const FWadInventoryDetails& Details);
    void ApplyTimedEffect(const FString& EffectOwner, EWadTimedEffectKind Kind, int32 Status, int32 Power, double Seconds);
    void RefreshTimedEffectCache();
    void ApplyReadiedDefenseOrbEffect();
    bool bApplyingDefenseOrbReady = false;
    void RestoreTimedEffects(const TCHAR* Payload);
    void DropInventoryItem();
    void UseReadyHand();
    void ClickPressed();
    void ToggleInventory();
    void InventoryPageUp();
    void InventoryPageDown();
    void ToggleMouseLook();
    void SetMouseLook(bool bEnabled);
    bool IsInWadTown() const;
    bool OpenWadTown();
    void TownSelectPrevious();
    void TownSelectNext();
    void TownQuest();
    void TownConfirm();
    void TownCancel();
    void TownScrollUp();
    void TownScrollDown();
    void TownInn();
    void TownCharacter();
    void TownInnCommons();
    void TownInnLarge();
    void TownInnSuite();
    void TownStore();
    void TownBank();
    void TownClick();
    void CompleteTownArrival();
    void LookYaw(float Axis);
    void LookPitch(float Axis);
    void RuneBackspace();
    void CastRejuvenate();
    void TickNeeds(float DeltaSeconds);
    void CheckLevelLink();
    bool LaunchWadProjectile(const NativeWadRules::FWadWandProjectile& Projectile);
    void ReadTravelOptions(const FURL& URL);
    void MoveLegacyCoinsToPurse();
    bool AddCoinsToPurse(int32 Type);
    void UpdateWaterWalking(const ANativeWadLevel& Level);
    void TickItemStatuses(float DeltaSeconds);
    void UpdateLavaWalking(const ANativeWadLevel& Level, float DeltaSeconds);
    FString MakeTravelOptions() const;
    void RestoreWadCheckpoint();
    bool ArchiveDeparture();
    void RestoreVisitedLevel();

    TWeakObjectPtr<ANativeWadLevel> NativeLevel;
    FTransform ReviveTransform;
    int32 CharacterClass = NativeWadRules::PriestClass;
    int32 LastLevelLink = INDEX_NONE;
    bool bCheckedLevelLink = false;
    int32 LastActivitySector = INDEX_NONE;
    int32 Health = NativeWadRules::MaxHealth;
    int32 Mana = NativeWadRules::MaxMana;
    int32 MaxHealth = NativeWadRules::MaxHealth;
    int32 MaxMana = NativeWadRules::MaxMana;
    int32 Food = NativeWadRules::MaxFood;
    int32 Water = NativeWadRules::MaxFood;
    int32 CharacterLevel = 1;
    int32 ExperienceNeeded = NativeWadRules::FirstLevelThreshold;
    int32 Attributes[NativeWadRules::AttributeCount] = {15, 20, 35, 15, 20, 25};
    int32 ReadySlot = INDEX_NONE;
    int32 WornSlots[NativeWadRules::EquipmentSlotCount] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
    int32 JewelrySlots[NativeWadRules::JewelrySlotCount] = {INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE, INDEX_NONE};
    int32 SelectedInventorySlot = INDEX_NONE;
    int32 SelectedAmmo = 0;
    int32 AmmoReserve[UE_ARRAY_COUNT(NativeWadRules::CrossbowAmmo)] = {};
    double ItemStatusSeconds[NativeWadRules::ItemStatusCount] = {};
    FWadPlayerEffects TimedEffects;
    int32 ItemStatusPower[NativeWadRules::ItemStatusCount] = {};
    bool bConcealed = false;
    bool bOnLavaSurface = false;
    double NextConcealmentRoll = 0.0;
    double LavaDamageTime = 0.0;
    bool bInventoryOpen = false;
    bool bMouseLook = true;
    bool bRestoreLookAfterInventory = true;
    int32 Experience = 0;
    int32 SuccessfulCasts = 0;
    int32 PoisonLevel = 0;
    double ParalysisSeconds = 0.0;
    double FlightSeconds = 0.0;
    bool bReadiedFlightSuppressed = false; // Includes a worn amulet; only a new READY trigger restores its flight.
    double WaterWalkSeconds = 0.0;
    double FeatherfallSeconds = 0.0;
    double ShieldSeconds[NativeWadRules::ShieldPotionLast - NativeWadRules::ShieldPotionFirst + 1] = {};
    bool bOnWaterSurface = false;
    double QuakeTime = 0.0;
    bool bRestoreCheckpointOnTick = false;
    bool bRestoreVisitedOnTick = true;
    bool bCheckpointRestored = false;
    TOptional<double> TravelHour;
    double NeedsTime = 0.0;
    TArray<int32> CarriedItems;
    // Full uint16 source type IDs: colored item variants have independent knowledge.
    TSet<int32> IdentifiedItemTypes;
    bool bItemIdentificationInitialized = false;
    void StoreItemIdentification();
    void RecordItemUseIdentification(int32 Type);
    bool UseIdentificationItem(int32 Slot);
    FString InteractionMessage;
    FString Runes;
    double MessageUntil = 0.0;
    double NextMeleeTime = 0.0;
    int32 LastMeleeRawDamage = 0;
    bool bLastMeleeCritical = false;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UCameraComponent> ViewCamera;
};

UCLASS()
class ANativeWadHUD : public AHUD
{
    GENERATED_BODY()

public:
    ANativeWadHUD();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void DrawHUD() override;
    void ClickHUD(const FVector2D& Position);
    FVector2D InventorySlotCenter(int32 Slot) const;
    bool HasClassicHud() const { return ClassicData != nullptr; }
    bool IsNotesOpen() const;
    void HandleNotesCharacter(TCHAR Character);
    void HandleNotesKey(const FKey& Key, bool bControl = false, bool bAlt = false);
    void ScrollNotes(int32 Delta);
    int32 GetNotesCursor() const;
    int32 GetNotesScroll() const;
    bool IsOptionsOpen() const;
    bool IsControlsOpen() const;
    int32 GetOptionsScroll() const { return OptionsScroll; }
    int32 GetControlsScroll() const { return ControlsScroll; }
    bool ExamineAt(const FVector2D& ViewportPoint);
    bool IsLookOpen() const;
    void ShowJournalPage(int32 PageId);
    AAORIGINAL_API int32 GetJournalPageId() const;
    int32 GetLookSourceType() const { return LookSourceType; }
    const FString& GetLookName() const { return LookName; }
    const FString& GetLookDescription() const { return LookDescription; }
    int32 GetLookScroll() const { return LookScroll; }

private:
    friend class ANativeWadPlayer;
    enum class EForm : uint8 { Inventory, Equipment, Statistics, Journal, Ammo, Finances, Notes, Options, Controls, Look };
    void ToggleForm(EForm Requested);
    void ToggleNotes();
    void ToggleJournal();
    void ToggleOptions();
    void ToggleControls();
    void ScrollClassicText(int32 Delta);
    void UpdateOptionsSlider(FVector2D ViewportPoint);
    void ChangeInventoryPage(int32 Offset);
    void SyncNotesInput();
    void ReleaseNotesInput(bool bRestoreInput);
    void StoreNotes();
    void DrawFallbackHUD(const ANativeWadPlayer& Player);
    void ClickFallbackInventory(const FVector2D& Position);
    FVector2D FallbackInventorySlotCenter(int32 Slot) const;

    UPROPERTY()
    TObjectPtr<UNativeWadHudData> ClassicData;

    UPROPERTY()
    TObjectPtr<UTexture2D> JournalImage;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> LookPortrait;

    EForm Form = EForm::Inventory;
    int32 InventoryPage = 0;
    int32 JournalPage = 0;
    int32 OptionsScroll = 0;
    int32 ControlsScroll = 0;
    int32 LookScroll = 0;
    int32 LookSourceType = 0;
    FString LookName;
    FString LookDescription;
    FVector2D LookPortraitSize = FVector2D::ZeroVector;
    int32 DraggedOptionSlider = INDEX_NONE;
    TSharedPtr<FNativeWadNotesEditor> NotesEditor;
    TSharedPtr<SWidget> NotesInputWidget;
};

UCLASS()
class AAORIGINAL_API ANativeWadGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ANativeWadGameMode();
};
