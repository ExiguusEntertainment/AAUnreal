#pragma once

#include "GameFramework/Character.h"
#include "NativeWadRules.h"
#include "NativeWadEncounter.generated.h"

class ANativeWadPlayer;
class ANativeWadLevel;
class UMaterialInterface;
class USphereComponent;
class UStaticMesh;
class UStaticMeshComponent;

enum class EWadBoltEffect : uint8
{
    Damage,
    Poison,
    Paralyze
};

USTRUCT()
struct FWadEncounterProfile
{
    GENERATED_BODY()

    UPROPERTY()
    FString Name = TEXT("Elf Guardian");
    UPROPERTY()
    int32 MaxHealth = NativeWadRules::GuardianHealth;
    UPROPERTY()
    int32 RetreatHealth = 500;
    UPROPERTY()
    int32 MeleeDamage = 100;
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
    bool TakeMeleeHit(ANativeWadPlayer& Player, int32 Damage);
    bool IsDefeated() const { return Health == 0; }
    int32 GetEncounterHealth() const { return Health; }
    void RestoreEncounterHealth(int32 SavedHealth);
    int32 GetBoltsFired() const { return BoltsFired; }
    int32 GetMeleeHits() const { return MeleeHits; }
    int32 GetRegeneratedHealth() const { return RegeneratedHealth; }
    bool CanSeePlayer(const ANativeWadPlayer& Player) const;
    const FString& GetEncounterName() const { return Profile.Name; }

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

    void FireBolt(ANativeWadPlayer& Player);
    int32 Health = NativeWadRules::GuardianHealth;
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
    virtual void Tick(float DeltaSeconds) override;

private:
    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> Sprite;

    FVector Velocity = FVector::ZeroVector;
    int32 Damage = 0;
    EWadBoltEffect Effect = EWadBoltEffect::Damage;
    TWeakObjectPtr<ANativeWadLevel> ScriptTrap;
};

UCLASS()
class AAORIGINAL_API ANativeWadPickup : public AActor
{
    GENERATED_BODY()

public:
    ANativeWadPickup();
    virtual void Tick(float DeltaSeconds) override;
    void Configure(UStaticMesh* Plane, UMaterialInterface* Image, int32 InType);
    void SetAvailable(bool bAvailable);
    bool IsAvailable() const { return !IsHidden(); }
    bool Collect(ANativeWadPlayer& Player);
    int32 GetItemType() const { return ItemType; }

private:
    UPROPERTY(VisibleAnywhere)
    TObjectPtr<USphereComponent> HitTarget;

    UPROPERTY(VisibleAnywhere)
    TObjectPtr<UStaticMeshComponent> Sprite;

    UPROPERTY()
    int32 ItemType = 0;
};
