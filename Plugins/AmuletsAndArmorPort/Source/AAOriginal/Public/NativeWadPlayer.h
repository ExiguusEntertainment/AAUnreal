#pragma once

#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "NativeWadRules.h"
#include "NativeWadPlayer.generated.h"

class UCameraComponent;
class ANativeWadLevel;
class UNativeWadHudData;

UCLASS()
class AAORIGINAL_API ANativeWadPlayer : public ACharacter
{
    GENERATED_BODY()

public:
    ANativeWadPlayer();
    virtual void BeginPlay() override;
    virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual FVector GetPawnViewLocation() const override;
    virtual void Jump() override;

    UFUNCTION(BlueprintCallable, Category="WAD|Runtime")
    bool Use();
    int32 GetHealth() const { return Health; }
    int32 GetMana() const { return Mana; }
    int32 GetFood() const { return Food; }
    int32 GetWater() const { return Water; }
    int32 GetExperience() const { return Experience; }
    int32 GetEquippedWeapon() const;
    int32 GetSuccessfulCasts() const { return SuccessfulCasts; }
    int32 GetMaxHealth() const { return MaxHealth; }
    int32 GetMaxMana() const { return MaxMana; }
    int32 GetCharacterLevel() const { return CharacterLevel; }
    int32 GetExperienceNeeded() const { return ExperienceNeeded; }
    TConstArrayView<int32> GetAttributes() const { return Attributes; }
    int32 GetItemCount(int32 Type) const;
    const TArray<int32>& GetCarriedItems() const { return CarriedItems; }
    int32 GetReadySlot() const { return ReadySlot; }
    int32 GetReadyItem() const;
    int32 GetSelectedInventorySlot() const { return SelectedInventorySlot; }
    bool IsInventoryOpen() const { return bInventoryOpen; }
    bool IsMouseLook() const { return bMouseLook; }
    void ClickInventorySlot(int32 Slot);
    FString GetRunes() const { return Runes; }
    bool ShouldShowEncounterHUD() const;
    FString GetInteractionMessage() const;
    void ShowInteractionMessage(FString Message);
    void ReceiveEncounterHit(int32 Damage);
    void ReceiveTrapPoison(int32 Damage);
    void ReceiveTrapParalysis();
    void RestoreWadVital(int32 Effect, int32 Amount);
    bool IsParalyzed() const { return ParalysisSeconds > 0.0; }
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
    void CollectItem(int32 Type);
    bool ConsumeItem(int32 Type);
    void AwardExperience(int32 Amount);

    static constexpr float CapsuleRadius = 10.0f;
    static constexpr float CapsuleHalfHeight = 27.0f;

private:
    friend class ANativeWadHUD;
    void MoveInViewDirection(float Forward, float Right);
    void UsePressed();
    void AttackPressed();
    void ResolveMeleeHit(int32 Damage);
    void ConsumeBerry();
    void UseReadyHand();
    void ClickPressed();
    void ToggleInventory();
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
    void TownStore();
    void TownBank();
    void TownClick();
    void CompleteTownArrival();
    void LookYaw(float Axis);
    void LookPitch(float Axis);
    void EnterRuneFour();
    void EnterRuneFive();
    void RuneBackspace();
    void CastRejuvenate();
    void TickNeeds(float DeltaSeconds);
    void CheckLevelLink();
    void ReadTravelOptions(const FURL& URL);
    FString MakeTravelOptions() const;
    void RestoreWadCheckpoint();
    bool ArchiveDeparture();
    void RestoreVisitedLevel();

    TWeakObjectPtr<ANativeWadLevel> NativeLevel;
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
    int32 SelectedInventorySlot = INDEX_NONE;
    bool bInventoryOpen = false;
    bool bMouseLook = true;
    bool bRestoreLookAfterInventory = true;
    int32 Experience = 0;
    int32 SuccessfulCasts = 0;
    int32 PoisonLevel = 0;
    double ParalysisSeconds = 0.0;
    double QuakeTime = 0.0;
    bool bRestoreCheckpointOnTick = false;
    bool bRestoreVisitedOnTick = true;
    bool bCheckpointRestored = false;
    TOptional<double> TravelHour;
    double NeedsTime = 0.0;
    TArray<int32> CarriedItems;
    FString InteractionMessage;
    FString Runes;
    double MessageUntil = 0.0;
    double NextMeleeTime = 0.0;

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
    virtual void DrawHUD() override;
    void ClickHUD(const FVector2D& Position);
    FVector2D InventorySlotCenter(int32 Slot) const;
    bool HasClassicHud() const { return ClassicData != nullptr; }

private:
    friend class ANativeWadPlayer;
    void DrawFallbackHUD(const ANativeWadPlayer& Player);
    void ClickFallbackInventory(const FVector2D& Position);
    FVector2D FallbackInventorySlotCenter(int32 Slot) const;

    UPROPERTY()
    TObjectPtr<UNativeWadHudData> ClassicData;

    UPROPERTY()
    TObjectPtr<UTexture2D> JournalImage;

    enum class EForm : uint8 { Inventory, Statistics, Journal };
    EForm Form = EForm::Inventory;
    int32 InventoryPage = 0;
    int32 JournalPage = 0;
};

UCLASS()
class AAORIGINAL_API ANativeWadGameMode : public AGameModeBase
{
    GENERATED_BODY()

public:
    ANativeWadGameMode();
};
