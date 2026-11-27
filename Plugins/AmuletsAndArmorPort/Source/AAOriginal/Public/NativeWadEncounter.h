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
    int32 GetRegeneratedHealth() const { return RegeneratedHealth; }
    bool CanSeePlayer(const ANativeWadPlayer& Player) const;
    const FString& GetEncounterName() const { return Profile.Name; }
    int32 GetSourceType() const { return Profile.SourceType; }

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
    void TickRailHazard(float DeltaSeconds);
    TWeakObjectPtr<ANativeWadLevel> RailLevel;
    int32 Health = NativeWadRules::GuardianHealth;
    int32 PoisonLevel = 0;
    double PoisonTime = 5.0;
    int32 BoltsFired = 0;
    int32 MeleeHits = 0;
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
    int32 GetItemType() const { return ItemType; }
    int32 GetCheckpointRevision() const { return CheckpointRevision; }
    void SetAuthoredAmmo(int32 Index, int32 Quantity) { AmmoIndex = Index; AmmoQuantity = Quantity; }
    int32 GetAmmoIndex() const { return AmmoIndex; }
    int32 GetAmmoQuantity() const { return AmmoQuantity; }
    static constexpr ECollisionChannel InteractionChannel = ECC_GameTraceChannel1;
    static ANativeWadPickup* SpawnDropped(ANativeWadLevel& Level, int32 Type, const FVector& Position);
    bool IsDropped() const { return bDropped; }

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
    bool bDropped = false;

    UPROPERTY()
    TObjectPtr<ANativeWadScenery> Scenery;
};
