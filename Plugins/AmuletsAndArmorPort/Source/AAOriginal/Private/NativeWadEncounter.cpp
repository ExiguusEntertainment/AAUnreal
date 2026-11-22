#include "NativeWadEncounter.h"

#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "NativeWadBillboard.h"
#include "NativeWadHudData.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadScenery.h"

namespace
{
    using namespace NativeWadRules;
    void SetSprite(UStaticMeshComponent& Sprite, UStaticMesh* Plane, UMaterialInterface* Material, double Scale)
    {
        Sprite.SetStaticMesh(Plane);
        Sprite.SetMaterial(0, Material);
        Sprite.SetRelativeScale3D(FVector(Scale));
        Sprite.SetCollisionProfileName(TEXT("NoCollision"));
        Sprite.SetCastShadow(false);
        Sprite.SetCanEverAffectNavigation(false);
        Sprite.SetAffectDistanceFieldLighting(false);
    }

    void FacePlayer(UStaticMeshComponent& Sprite)
    {
        const APlayerController* Controller = Sprite.GetWorld()->GetFirstPlayerController();
        if (Controller && Controller->GetViewTarget())
        {
            NativeWadBillboard::FaceView(Sprite, *Controller);
        }
    }
}

ANativeWadGuardian::ANativeWadGuardian()
{
    PrimaryActorTick.bCanEverTick = true;
    AutoPossessAI = EAutoPossessAI::Disabled;
    GetCapsuleComponent()->InitCapsuleSize(20.0f, 26.0f);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    GetCharacterMovement()->bRunPhysicsWithNoController = true;
    GetCharacterMovement()->MaxWalkSpeed = 115.0f;
    Sprite = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("GuardianSprite"));
    Sprite->SetupAttachment(GetRootComponent());
}

void ANativeWadGuardian::Configure(UStaticMesh* Plane, UMaterialInterface* Standing, UMaterialInterface* Downed,
    UMaterialInterface* Attacking, UMaterialInterface* Hurt, UMaterialInterface* Bolt, const FWadEncounterProfile& InProfile)
{
    SetSprite(*Sprite, Plane, Standing, 0.58);
    StandingMaterial = Standing;
    DownedMaterial = Downed;
    AttackingMaterial = Attacking;
    HurtMaterial = Hurt;
    BoltMaterial = Bolt;
    Profile = InProfile;
    GetCapsuleComponent()->SetCapsuleSize(Profile.CollisionRadius, Profile.CollisionHalfHeight);
    GetCharacterMovement()->MaxWalkSpeed = Profile.WalkSpeed;
    if (Profile.SpriteSize.X > 0 && Profile.SpriteSize.Y > 0)
    {
        Sprite->SetRelativeScale3D(FVector(Profile.SpriteSize * 0.01, 1.0));
    }
}

void ANativeWadGuardian::BeginPlay()
{
    Super::BeginPlay();
    Health = Profile.MaxHealth;
}

bool ANativeWadGuardian::CanSeePlayer(const ANativeWadPlayer& Player) const
{
    FHitResult Hit;
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(NativeWadGuardianSight), true, this);
    return !GetWorld()->LineTraceSingleByChannel(Hit, GetActorLocation(), Player.GetActorLocation(), ECC_Visibility, Query)
        || Hit.GetActor() == &Player;
}

void ANativeWadGuardian::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(UGameplayStatics::GetPlayerCharacter(this, 0));
    if (!Player)
    {
        return;
    }
    FacePlayer(*Sprite);
    if (IsDefeated())
    {
        return;
    }

    LogicTime += DeltaSeconds;
    const FVector Offset = Player->GetActorLocation() - GetActorLocation();
    const FVector TowardPlayer = Offset.GetSafeNormal2D();
    const double Distance = Offset.Size2D();
    const bool bRetreat = Health < Profile.RetreatHealth;
    const bool bCloseRetreat = Profile.bKeepsBoltDistance && Distance < Profile.MinBoltRange;
    // CRELOGIC.C extends the table's update by half and shortens the fleeing update by a quarter.
    const int32 UpdateTicks = Profile.UpdateTicks + (Profile.UpdateTicks >> 1);
    const double Interval = (bRetreat ? UpdateTicks - (UpdateTicks >> 2) : UpdateTicks) / TicksPerSecond;
    const int32 Updates = FMath::FloorToInt(LogicTime / Interval);
    LogicTime -= Updates * Interval;
    const int32 Recovery = FMath::Min(Updates * Profile.RegenPerUpdate, Profile.MaxHealth - Health);
    Health += Recovery;
    RegeneratedHealth += Recovery;
    if (bHasTarget)
    {
        MeleeDelay = FMath::Max(0, MeleeDelay - Updates);
        BoltDelay = FMath::Max(0, BoltDelay - Updates);
    }

    const double Now = GetWorld()->GetTimeSeconds();
    UMaterialInterface* Pose = Now < HurtUntil ? HurtMaterial.Get()
        : Now < AttackPoseUntil ? AttackingMaterial.Get() : StandingMaterial.Get();
    if (Pose && Pose != Sprite->GetMaterial(0))
    {
        Sprite->SetMaterial(0, Pose);
    }
    const bool bVisible = Player->GetHealth() > 0 && CanSeePlayer(*Player);
    const bool bMelee = bVisible && Distance < Profile.MeleeRange && FMath::Abs(Offset.Z) <= 26.0;
    if (HitTime > 0.0 && Now >= HitTime)
    {
        HitTime = 0.0;
        if (bMelee)
        {
            ++MeleeHits;
            Player->ReceivePhysicalHit(Profile.MeleeDamage);
        }
    }
    if (!bVisible)
    {
        return;
    }
    if (!bHasTarget)
    {
        if (FVector::DotProduct(GetActorForwardVector(), TowardPlayer)
            < FMath::Cos(FMath::DegreesToRadians(Profile.VisionHalfAngle)))
        {
            return;
        }
        bHasTarget = true;
    }
    if (Now < HurtUntil)
    {
        return;
    }
    const bool bHasMissile = Profile.BoltDamage > 0 && BoltMaterial;
    if (bRetreat || bCloseRetreat || (!bMelee && (!bHasMissile
        || Distance > Profile.BoltRange || Distance <= Profile.MinBoltRange)))
    {
        AddMovementInput(bRetreat || bCloseRetreat ? -TowardPlayer : TowardPlayer, 1.0f, true);
    }
    if (Updates == 0)
    {
        return;
    }
    if (bMelee && Profile.MeleeDamage > 0 && MeleeDelay == 0)
    {
        MeleeDelay = Profile.MeleeDelayUpdates;
        HitTime = Now + 18 / TicksPerSecond;
        AttackPoseUntil = Now + 20 / TicksPerSecond;
    }
    else if (bHasMissile && !bMelee && Distance <= Profile.BoltRange
        && (Profile.bKeepsBoltDistance || Distance > Profile.MinBoltRange || bRetreat) && BoltDelay == 0)
    {
        BoltDelay = Profile.RangedDelayUpdates;
        AttackPoseUntil = Now + 20 / TicksPerSecond;
        FireBolt(*Player);
    }
}

bool ANativeWadGuardian::TakeMeleeHit(ANativeWadPlayer& Player, int32 Damage)
{
    if (IsDefeated() || Damage <= 0)
    {
        return false;
    }
    const int32 Applied = FMath::Min(Health, Damage);
    Health -= Applied;
    Player.AwardExperience(Applied);
    HurtUntil = GetWorld()->GetTimeSeconds() + 20 / TicksPerSecond;
    if (IsDefeated())
    {
        RestoreEncounterHealth(0);
        Player.ShowInteractionMessage(Profile.Name + TEXT(" defeated"));
        UE_LOG(LogTemp, Display, TEXT("Defeated the authored %s."), *Profile.Name);
    }
    return true;
}

void ANativeWadGuardian::RestoreEncounterHealth(int32 SavedHealth)
{
    Health = FMath::Clamp(SavedHealth, 0, Profile.MaxHealth);
    if (IsDefeated())
    {
        GetCharacterMovement()->DisableMovement();
        GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Sprite->SetMaterial(0, DownedMaterial);
    }
    if (IsValid(DefeatPickup))
    {
        if (IsDefeated())
        {
            const double Offset = 12.0 - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
            DefeatPickup->SetActorLocation(GetActorLocation() + FVector(0.0, 0.0, Offset));
        }
        DefeatPickup->SetAvailable(IsDefeated());
    }
}

void ANativeWadGuardian::SetDefeatPickup(ANativeWadPickup* Pickup)
{
    DefeatPickup = Pickup;
    DefeatPickup->SetAvailable(false);
}

void ANativeWadGuardian::SetSpawned(bool bSpawned)
{
    SetActorHiddenInGame(!bSpawned);
    SetActorEnableCollision(bSpawned);
    SetActorTickEnabled(bSpawned);
    GetCharacterMovement()->SetComponentTickEnabled(bSpawned);
}

void ANativeWadGuardian::FireBolt(ANativeWadPlayer& Player)
{
    const FVector Start = GetActorLocation();
    const FVector Direction = (Player.GetActorLocation() + FVector(0, 0, 10) - Start).GetSafeNormal();
    FActorSpawnParameters Parameters;
    Parameters.Owner = this;
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (ANativeWadBolt* Bolt = GetWorld()->SpawnActor<ANativeWadBolt>(Start, Direction.Rotation(), Parameters))
    {
        Bolt->Launch(Sprite->GetStaticMesh(), BoltMaterial, Direction * (30 * TicksPerSecond), Profile.BoltDamage);
        ++BoltsFired;
    }
}

ANativeWadBolt::ANativeWadBolt()
{
    PrimaryActorTick.bCanEverTick = true;
    Sprite = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BoltSprite"));
    SetRootComponent(Sprite);
    Sprite->SetCollisionProfileName(TEXT("NoCollision"));
}

void ANativeWadBolt::Launch(UStaticMesh* Plane, UMaterialInterface* Image, const FVector& InVelocity, int32 InDamage,
    EWadBoltEffect InEffect, ANativeWadLevel* ScriptLevel)
{
    Velocity = InVelocity;
    Damage = InDamage;
    Effect = InEffect;
    ScriptTrap = ScriptLevel;
    if (Plane && Image)
    {
        SetSprite(*Sprite, Plane, Image, 0.16);
    }
    SetLifeSpan(2.0f);
}

void ANativeWadBolt::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const FVector Start = GetActorLocation();
    const FVector End = Start + Velocity * DeltaSeconds;
    FHitResult Hit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(NativeWadBoltFlight), true, this);
    Query.AddIgnoredActor(GetOwner());
    if (GetWorld()->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility,
        FCollisionShape::MakeSphere(5.0f), Query))
    {
        if (Effect == EWadBoltEffect::LockDoor)
        {
            if (ANativeWadLevel* Level = ScriptTrap.Get())
            {
                Level->LockScriptDoorsNear(FVector2D(Hit.ImpactPoint));
            }
        }
        else if (ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(Hit.GetActor()))
        {
            switch (Effect)
            {
            case EWadBoltEffect::Poison:
                Player->ReceiveTrapPoison(Damage);
                break;
            case EWadBoltEffect::Paralyze:
                Player->ReceiveTrapParalysis();
                break;
            case EWadBoltEffect::Damage:
                Player->ReceiveEncounterHit(Damage);
                break;
            case EWadBoltEffect::LockDoor:
                break;
            }
        }
        else if (ANativeWadLevel* Level = ScriptTrap.Get())
        {
            Level->CountBlockedScriptMissile();
        }
        Destroy();
        return;
    }
    SetActorLocation(End);
    FacePlayer(*Sprite);
}

ANativeWadPickup::ANativeWadPickup()
{
    PrimaryActorTick.bCanEverTick = true;
    HitTarget = CreateDefaultSubobject<USphereComponent>(TEXT("PickupHitTarget"));
    SetRootComponent(HitTarget);
    HitTarget->InitSphereRadius(10.0f);
    HitTarget->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    HitTarget->SetCollisionResponseToAllChannels(ECR_Ignore);
    HitTarget->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    Sprite = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PickupSprite"));
    Sprite->SetupAttachment(HitTarget);
}

void ANativeWadPickup::Configure(UStaticMesh* Plane, UMaterialInterface* Image, int32 InType, int32 InCheckpointRevision)
{
    const FVector Scale = InType == IronDagger ? FVector(0.25, 0.25, 0.25) : FVector(0.10, 0.13, 0.13);
    SetSprite(*Sprite, Plane, Image, Scale.Y);
    Sprite->SetRelativeScale3D(Scale);
    ItemType = InType;
    CheckpointRevision = InCheckpointRevision;
}

void ANativeWadPickup::ConfigureAuthored(UStaticMesh* Plane, UMaterialInterface* Image, int32 InType, int32 InCheckpointRevision,
    FIntPoint Size, int32 Radius, bool bPassable, ANativeWadScenery* InScenery)
{
    Configure(Plane, Image, InType, InCheckpointRevision);
    Sprite->SetRelativeScale3D(FVector(Size.X * 0.01, Size.Y * 0.01, 1.0));
    HitTarget->SetSphereRadius(FMath::Max3(8.0f, float(Radius), float(FMath::Min(Size.X, Size.Y)) * 0.5f));
    HitTarget->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
    HitTarget->SetCollisionResponseToChannel(InteractionChannel, ECR_Block);
    HitTarget->SetCollisionResponseToChannel(ECC_Pawn, bPassable || InScenery ? ECR_Ignore : ECR_Block);
    HitTarget->SetCanEverAffectNavigation(false);
    Scenery = InScenery;
    Sprite->SetVisibility(!Scenery);
    if (Scenery)
    {
        Scenery->SetPickup(this);
    }
}

ANativeWadPickup* ANativeWadPickup::SpawnDropped(ANativeWadLevel& Level, int32 Type, const FVector& Position)
{
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    const FWadInventoryDetails* Details = Catalog ? Catalog->ItemDetails.Find(Type) : nullptr;
    const int32 Sector = Level.FindWadSector(FVector2D(Position - Level.GetActorLocation()));
    if (!Details || Details->SpriteSize.X <= 0 || Details->SpriteSize.Y <= 0 || Sector == INDEX_NONE)
    {
        return nullptr;
    }
    UMaterialInterface* Material = Details->WorldMaterial.LoadSynchronous();
    UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    if (!Material || !Plane)
    {
        return nullptr;
    }
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    ANativeWadPickup* Pickup = Level.GetWorld()->SpawnActor<ANativeWadPickup>(Position, FRotator::ZeroRotator, Params);
    if (Pickup)
    {
        Pickup->bDropped = true;
        Pickup->ConfigureAuthored(Plane, Material, Type, 0, Details->SpriteSize, 0, true, nullptr);
        if (!Level.AttachActorToWadFloor(*Pickup, Sector))
        {
            Pickup->Destroy();
            return nullptr;
        }
    }
    return Pickup;
}

void ANativeWadPickup::Destroyed()
{
    if (IsValid(Scenery) && GetWorld() && GetWorld()->IsGameWorld())
    {
        Scenery->SetActorHiddenInGame(true);
        Scenery->SetActorEnableCollision(false);
        Scenery->SetActorTickEnabled(false);
    }
    Super::Destroyed();
}

void ANativeWadPickup::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!Scenery)
    {
        FacePlayer(*Sprite);
    }
}

void ANativeWadPickup::SetAvailable(bool bAvailable)
{
    SetActorHiddenInGame(!bAvailable);
    SetActorEnableCollision(bAvailable);
    SetActorTickEnabled(bAvailable);
}

bool ANativeWadPickup::Collect(ANativeWadPlayer& Player)
{
    if (ItemType <= 0 || IsActorBeingDestroyed() || !IsAvailable())
    {
        return false;
    }
    Player.CollectItem(ItemType);
    Destroy();
    return true;
}
