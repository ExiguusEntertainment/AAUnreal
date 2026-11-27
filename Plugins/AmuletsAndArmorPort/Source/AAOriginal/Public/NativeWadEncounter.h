#pragma once

#include "GameFramework/Character.h"
#include "NativeWadRules.h"
#include "NativeWadEncounter.generated.h"

class ANativeWadPlayer;
class ANativeWadScenery;
class ANativeWadLevel;
class UMaterialInterface;
class USphereComponent;
class UStaticMesh;
class UStaticMeshComponent;

struct FWadCreatureTreasure
{
    int32 ItemType = 0;
    int32 AmmoIndex = INDEX_NONE;
    int32 AmmoQuantity = 0;
    // -1 means no source payload; recorded zero is a valid, distinct value.
    int32 AccessoryData = INDEX_NONE;
    int32 AdditionalItemType = 0; // Verified multi-item ordinary treasure (Jugurtha).
    bool HasAccessoryData() const { return AccessoryData >= 0 && AccessoryData <= MAX_uint16; }
};

// CRELOGIC.C ordinary drops, including Jugurtha's verified two-item case. Full
// source identities: Elymas 50158 must not become Orange Wizard 1006. Quest
// objects remain separate DefeatPickups; Exiguus/Elymas release their extra ring here.
// The helper reproduces branch/table selection, not the original global RNG sequence.
inline FWadCreatureTreasure ResolveWadCreatureTreasure(int32 SourceType, uint16 Roll)
{
    static constexpr int32 BadKnight[12] = {204, 204, 204, 204, 204, 204, 818, 204, 708, 709, 710, 711};
    static constexpr int32 EvilKnight[12] = {102, 102, 102, 204, 204, 204, 102, 204, 708, 709, 710, 711};
    static constexpr int32 Trojan[12] = {37067, 37067, 37067, 104, 104, 104, 105, 12492, 12996, 12997, 12998, 12999};
    switch (SourceType)
    {
    case 1002: return {(Roll & 1) ? 102 : 362};
    case 1006: return (Roll & 1) ? FWadCreatureTreasure{373, INDEX_NONE, 0, 407} : FWadCreatureTreasure{104};
    case 1008: return {147, 2, 6};
    case 1009: return {144, 0, 12};
    case 1011: return {Trojan[Roll % 12]};
    case 1014: return {145, 3, 6}; // Source "fire" quiver; retain the established banner-to-ammo mapping.
    case 1018: return (Roll & 1) ? FWadCreatureTreasure{144, 0, 6} : FWadCreatureTreasure{100};
    case 1019: return {(Roll & 1) ? 202 : 822};
    case 33787: return {(Roll & 1) ? 202 : 825};
    case 46075: return {415, INDEX_NONE, 0, INDEX_NONE, 234};
    case 1032: return {416};
    case 1035: return {BadKnight[Roll % 12]};
    case 1036: return {106};
    case 3000: case 3003: return {101};
    case 3001: case 3002: return {102};
    case 4080: return {EvilKnight[Roll % 12]};
    case 50158: return {417};
    // Mighty Hunter has no unstolen ordinary-death case;
    // Momma Dragon's key is commented out. Packed-family restoration is separate.
    default: return {};
    }
}

enum class EWadBoltEffect : uint8
{
    Damage,
    Poison,
    Paralyze,
    LockDoor
};

USTRUCT()
struct FWadEncounterProfile
{
    GENERATED_BODY()

    void ApplySourceRules();

    UPROPERTY()
    FString Name = TEXT("Elf Guardian");
    UPROPERTY()
    int32 SourceType = 0;
    UPROPERTY()
    int32 MaxHealth = NativeWadRules::GuardianHealth;
    UPROPERTY()
    int32 RetreatHealth = 500;
    UPROPERTY()
    int32 MeleeDamage = 100;
    UPROPERTY()
    uint8 MeleeDamageMask = NativeWadRules::NormalDamage;
    UPROPERTY()
    int32 BoltDamage = 300;
    UPROPERTY()
    int32 BoltRange = 2000;
    UPROPERTY()
    int32 MeleeDelayUpdates = 30;
    UPROPERTY()
    int32 RangedDelayUpdates = 50;

    UPROPERTY()
    float CollisionRadius = 20.0f;
    UPROPERTY()
    float CollisionHalfHeight = 26.0f;
    UPROPERTY()
    FVector2D SpriteSize = FVector2D::ZeroVector;
    UPROPERTY()
    float WalkSpeed = 190.0f;
    UPROPERTY()
    bool bCanFly = false;
    UPROPERTY()
    int32 ChanceAttackCreature = 0;
    UPROPERTY()
    int32 ChanceContinueAttack = 0;
    UPROPERTY()
    int32 UpdateTicks = 5;
    UPROPERTY()
    int32 RegenPerUpdate = 1;
    UPROPERTY()
    float MeleeRange = 60.0f;
    UPROPERTY()
    float MinBoltRange = 250.0f;
    UPROPERTY()
    float VisionHalfAngle = 135.0f;
    UPROPERTY()
    bool bKeepsBoltDistance = false;
    UPROPERTY()
    bool bResistsFire = false;
    UPROPERTY()
    bool bResistsAcidAndPoison = false;
    UPROPERTY()
    int32 ArmorClass = 0;
    UPROPERTY()
    uint8 ResistanceMask = 0;
    UPROPERTY()
    uint8 BoltElement = 0;
    UPROPERTY()
    bool bHomingPlasma = false;
    UPROPERTY()
    int32 HomingMissileType = 0;
    UPROPERTY()
    int32 SourceMissileType = 0;
    UPROPERTY()
    bool bApproachWhileFiring = false;
    UPROPERTY()
    bool bDipper = false;
    UPROPERTY()
    FVector2D RailStart = FVector2D::ZeroVector;
    UPROPERTY()
    FVector2D RailEnd = FVector2D::ZeroVector;
};

UCLASS()
class AAORIGINAL_API ANativeWadGuardian : public ACharacter
{
    GENERATED_BODY()

public:
    ANativeWadGuardian();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    void Configure(UStaticMesh* Plane, UMaterialInterface* Standing, UMaterialInterface* Downed,
        UMaterialInterface* Attacking, UMaterialInterface* Hurt, UMaterialInterface* Bolt, const FWadEncounterProfile& InProfile);
    void ConfigureFrom(const ANativeWadGuardian& Example);
    bool HasAcquiredTarget() const { return bHasTarget; }
    void ShowSummonPose();
    bool TakeMeleeHit(ANativeWadPlayer& Player, int32 Damage);
    bool TakeHit(int32 Damage, ANativeWadPlayer* Credit = nullptr,
        uint8 DamageMask = NativeWadRules::NormalDamage);
    bool IsDefeated() const { return Health == 0; }
    UFUNCTION(BlueprintPure, Category="WAD|Creature")
    int32 GetEncounterHealth() const { return Health; }
    int32 GetPoisonLevel() const { return PoisonLevel; }
    double GetPoisonTime() const { return PoisonTime; }
    void RestorePoison(int32 Level, double Time);
    double GetSlowSeconds() const { return SlowSeconds; }
    double GetEarthboundSeconds() const { return EarthboundSeconds; }
    void RestoreControlEffects(double Slow, double Earthbound);
    ANativeWadGuardian* GetBerserkTarget() const { return BerserkTarget.Get(); }
    double GetBerserkSeconds() const { return BerserkSeconds; }
    int32 GetCreatureLife() const { return CreatureLife; }
    int32 GetBerserkTargetLife() const { return BerserkTargetLife; }
    void RestoreCreatureLife(int32 Life) { CreatureLife = FMath::Max(0, Life); }
    void RestoreBerserkTarget(ANativeWadGuardian* Target, double Seconds, int32 TargetLife);
    void RestoreEncounterHealth(int32 SavedHealth);
    void SetDefeatPickup(class ANativeWadPickup* Pickup);
    void SetSpawned(bool bSpawned);
    void ResetForGenerator();
    int32 GetBoltsFired() const { return BoltsFired; }
    int32 GetMeleeHits() const { return MeleeHits; }
    int32 GetScreamCount() const { return ScreamsFired; }
    int32 GetRegeneratedHealth() const { return RegeneratedHealth; }
    double GetDipDepth() const;
    bool HasHandledDefeatTreasure() const { return bDefeatTreasureHandled; }
    UFUNCTION(BlueprintPure, Category="WAD|Creature")
    UMaterialInterface* GetPortraitMaterial() const { return StandingMaterial.Get(); }
    FVector2D GetPortraitSize() const;
    int32 GetLookSourceType() const;
    bool CanSeePlayer(const ANativeWadPlayer& Player) const;
    const FString& GetEncounterName() const { return Profile.Name; }
    UFUNCTION(BlueprintPure, Category="WAD|Creature")
    int32 GetSourceType() const { return Profile.SourceType; }
    const FWadEncounterProfile& GetEncounterProfile() const { return Profile; }
    double GetMagicMapRadius() const { return Profile.CollisionRadius; }

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UStaticMeshComponent> Sprite;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> DownedMaterial;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> StandingMaterial;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> AttackingMaterial;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> HurtMaterial;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> BoltMaterial;

    UPROPERTY()
    FWadEncounterProfile Profile;

    UPROPERTY()
    TObjectPtr<class ANativeWadPickup> DefeatPickup;

    void ApplyControlMovement();
    double SlowSeconds = 0.0;
    double EarthboundSeconds = 0.0;
    bool CanSeeTarget(const AActor& Target) const;
    void ClearBerserkTarget();
    TWeakObjectPtr<ANativeWadGuardian> BerserkTarget;
    double BerserkSeconds = 0.0;
    int32 CreatureLife = 0;
    int32 BerserkTargetLife = 0;
    void FireBolt(AActor& Target);
    void DropDefeatTreasure();
    bool bDefeatTreasureHandled = false;
    bool TickDipper(float DeltaSeconds, bool bRise);
    bool CanMoveOnLiquid(const FVector& Direction, float DeltaSeconds) const;
    TWeakObjectPtr<ANativeWadLevel> DipperLevel;
    double DipperTime = 0.0;
    void TickRailHazard(float DeltaSeconds);
    TWeakObjectPtr<ANativeWadLevel> RailLevel;
    int32 Health = NativeWadRules::GuardianHealth;
    int32 PoisonLevel = 0;
    double PoisonTime = 5.0;
    int32 BoltsFired = 0;
    int32 MeleeHits = 0;
    int32 ScreamsFired = 0;
    int32 RegeneratedHealth = 0;
    int32 MeleeDelay = 5;
    int32 BoltDelay = 5;
    bool bHasTarget = false;
    double HitTime = 0.0;
    double HurtUntil = 0.0;
    double AttackPoseUntil = 0.0;
    double LogicTime = 0.0;
};

UCLASS()
class ANativeWadBolt : public AActor
{
    GENERATED_BODY()

public:
    ANativeWadBolt();
    void Launch(UStaticMesh* Plane, UMaterialInterface* Image, const FVector& Velocity, int32 Damage,
        EWadBoltEffect Effect = EWadBoltEffect::Damage, ANativeWadLevel* ScriptLevel = nullptr);
    void SetHomingTarget(AActor* Target) { HomingTarget = Target; }
    void SetBlastRadius(float Radius) { BlastRadius = Radius; }
    void SetDamageType(NativeWadRules::EProjectileDamage Element)
    {
        SetDamageMask(NativeWadRules::NormalDamage | NativeWadRules::DamageFlag(Element));
    }
    void SetDamageMask(uint8 Mask) { DamageMask = Mask; }
    virtual void Tick(float DeltaSeconds) override;

private:
    void ApplyBlast(const FVector& Origin);
    void ApplyForce(ACharacter& Target, const FVector& Origin) const;
    void DamagePlayer(ANativeWadPlayer& Player) const;

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> Sprite;

    FVector Velocity = FVector::ZeroVector;
    float BlastRadius = 0.0f;
    uint8 DamageMask = NativeWadRules::NormalDamage;
    int32 Damage = 0;
    EWadBoltEffect Effect = EWadBoltEffect::Damage;
    TWeakObjectPtr<ANativeWadLevel> ScriptTrap;
    TWeakObjectPtr<AActor> HomingTarget;
};

UCLASS()
class AAORIGINAL_API ANativeWadPickup : public AActor
{
    GENERATED_BODY()

public:
    ANativeWadPickup();
    virtual void Tick(float DeltaSeconds) override;
    void Configure(UStaticMesh* Plane, UMaterialInterface* Image, int32 InType, int32 InCheckpointRevision = 0);
    void ConfigureAuthored(UStaticMesh* Plane, UMaterialInterface* Image, int32 InType, int32 InCheckpointRevision,
        FIntPoint Size, int32 Radius, bool bPassable, ANativeWadScenery* InScenery);
    virtual void Destroyed() override;
    void SetAvailable(bool bAvailable);
    bool IsAvailable() const { return !IsHidden(); }
    bool Collect(ANativeWadPlayer& Player);
    UFUNCTION(BlueprintPure, Category="WAD|Pickups")
    int32 GetItemType() const { return ItemType; }
    int32 GetCheckpointRevision() const { return CheckpointRevision; }
    bool SetAccessoryData(int32 Value);
    uint16 GetAccessoryData() const { return AccessoryData; }
    UFUNCTION(BlueprintPure, Category="WAD|Pickups")
    int32 GetSourceAccessoryData() const { return int32(AccessoryData); }
    UFUNCTION(BlueprintPure, Category="WAD|Pickups")
    bool HasAccessoryData() const { return bAccessoryDataRecorded; }
    void RestoreAccessoryData(uint16 Value, bool bRecorded);
    void SetAuthoredAmmo(int32 Index, int32 Quantity) { AmmoIndex = Index; AmmoQuantity = Quantity; }
    int32 GetAmmoIndex() const;
    int32 GetAmmoQuantity() const;
    static constexpr ECollisionChannel InteractionChannel = ECC_GameTraceChannel1;
    static ANativeWadPickup* SpawnDropped(ANativeWadLevel& Level, int32 Type, const FVector& Position);
    UFUNCTION(BlueprintPure, Category="WAD|Pickups")
    bool IsDropped() const { return bDropped; }
    double GetMagicMapRadius() const;
    bool IsMagicMapInvisible() const;

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USphereComponent> HitTarget;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UStaticMeshComponent> Sprite;

    UPROPERTY()
    int32 ItemType = 0;

    UPROPERTY()
    int32 CheckpointRevision = 0;
    UPROPERTY()
    int32 AmmoIndex = INDEX_NONE;
    UPROPERTY()
    int32 AmmoQuantity = 0;
    UPROPERTY()
    bool bAccessoryDataRecorded = false;
    UPROPERTY()
    uint16 AccessoryData = 0;
    bool bCollectionCommitted = false;
    bool bDropped = false;

    UPROPERTY()
    TObjectPtr<ANativeWadScenery> Scenery;
};
