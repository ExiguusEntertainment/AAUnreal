#include "NativeWadEncounter.h"

#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
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

    bool IsWithinDamageRadius(const ACharacter& Target, const FVector& Origin, float Radius)
    {
        const UCapsuleComponent* Capsule = Target.GetCapsuleComponent();
        const FVector Offset = Target.GetActorLocation() - Origin;
        return Offset.SizeSquared2D() <= FMath::Square(Radius + Capsule->GetScaledCapsuleRadius())
            && FMath::Abs(Offset.Z) <= Radius + Capsule->GetScaledCapsuleHalfHeight();
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

void FWadEncounterProfile::ApplySourceRules()
{
    switch (SourceType)
    {
    case 1006: case 1014: MeleeDamageMask = NormalDamage | DamageFlag(EProjectileDamage::Fire); break;
    case 1007: case 1011: MeleeDamageMask = NormalDamage | DamageFlag(EProjectileDamage::Piercing); break;
    case 1020: MeleeDamageMask = NormalDamage | DamageFlag(EProjectileDamage::ManaDrain); break;
    case 1025: MeleeDamageMask = NormalDamage | DamageFlag(EProjectileDamage::Electricity); break;
    case 1002: MeleeDamageMask = SpecialDamageFlag(ESpecialDamage::Push); break; // Source Push: no player health damage.
    default: break;
    }
    if (SourceType == 1003 || SourceType == 1010 || SourceType == 1023) { bCanFly = true; }
    if (SourceType == 1023)
    {
        // CREDATA.H Griffon: flying berserker, 6-update melee delay, 3 HP regeneration.
        MeleeDelayUpdates = 6;
        UpdateTicks = 7;
        RegenPerUpdate = 3;
    }
    if (SourceType == 1014)
    {
        // CREDATA.H, Eastern Archer (1014) and Fire Effect Bolt (890).
        MeleeDelayUpdates = RangedDelayUpdates = 30;
        MeleeDamage = 500;
        BoltRange = 2000;
        VisionHalfAngle = 90.0f;
        ResistanceMask = NormalDamage | DamageFlag(EProjectileDamage::Fire);
        BoltElement = uint8(EProjectileDamage::Fire);
        ArmorClass = 1; // Cloth=0, chain=1, plate=2.
    }
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
    Profile.ApplySourceRules();
    GetCapsuleComponent()->SetCapsuleSize(Profile.CollisionRadius, Profile.CollisionHalfHeight);
    GetCharacterMovement()->MaxWalkSpeed = Profile.WalkSpeed;
    GetCharacterMovement()->MaxFlySpeed = Profile.WalkSpeed;
    if (Profile.bCanFly) { GetCharacterMovement()->SetMovementMode(MOVE_Flying); }
    if (Profile.SpriteSize.X > 0 && Profile.SpriteSize.Y > 0)
    {
        Sprite->SetRelativeScale3D(FVector(Profile.SpriteSize * 0.01, 1.0));
    }
}

void ANativeWadGuardian::BeginPlay()
{
    Super::BeginPlay();
    Profile.ApplySourceRules();
    // Existing maps serialized the engine's default MaxFlySpeed before source flight was restored.
    GetCharacterMovement()->MaxWalkSpeed = Profile.WalkSpeed;
    GetCharacterMovement()->MaxFlySpeed = Profile.WalkSpeed;
    Health = Profile.MaxHealth;
    if (Profile.bCanFly) { GetCharacterMovement()->SetMovementMode(MOVE_Flying); }
    if (Profile.RailStart != Profile.RailEnd)
    {
        const TActorIterator<ANativeWadLevel> Level(GetWorld());
        RailLevel = Level ? *Level : nullptr;
        GetCharacterMovement()->DisableMovement();
        GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }
}

void ANativeWadGuardian::ConfigureFrom(const ANativeWadGuardian& Example)
{
    Configure(Example.Sprite->GetStaticMesh(), Example.StandingMaterial, Example.DownedMaterial, Example.AttackingMaterial,
        Example.HurtMaterial, Example.BoltMaterial, Example.Profile);
}

void ANativeWadGuardian::ShowSummonPose()
{
    AttackPoseUntil = GetWorld()->GetTimeSeconds() + 20 / TicksPerSecond;
}

bool ANativeWadGuardian::CanSeePlayer(const ANativeWadPlayer& Player) const
{
    if (Player.IsConcealed())
    {
        return false;
    }
    FHitResult Hit;
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(NativeWadGuardianSight), true, this);
    return !GetWorld()->LineTraceSingleByChannel(Hit, GetActorLocation(), Player.GetActorLocation(), ECC_Visibility, Query)
        || Hit.GetActor() == &Player;
}

void ANativeWadGuardian::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    FacePlayer(*Sprite);
    if (Profile.RailStart != Profile.RailEnd)
    {
        TickRailHazard(DeltaSeconds);
        return;
    }
    if (!IsDefeated() && PoisonLevel > 0)
    {
        PoisonTime -= DeltaSeconds;
        while (PoisonTime <= 0.0 && PoisonLevel > 0 && !IsDefeated())
        {
            TakeHit(PoisonLevel * 2);
            PoisonLevel = FMath::Max(0, PoisonLevel - 5);
            PoisonTime += 5.0;
        }
    }
    ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(UGameplayStatics::GetPlayerCharacter(this, 0));
    if (!Player)
    {
        return;
    }
    if (IsDefeated())
    {
        return;
    }

    LogicTime += DeltaSeconds;
    const FVector Offset = Player->GetActorLocation() - GetActorLocation();
    const FVector TowardPlayer = Profile.bCanFly ? Offset.GetSafeNormal() : Offset.GetSafeNormal2D();
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
            Player->ReceiveTypedHit(Profile.MeleeDamage, Profile.MeleeDamageMask);
            if (Player->GetHealth() <= 0)
            {
                UE_LOG(LogTemp, Display, TEXT("%s defeated the player with a melee hit."), *GetActorNameOrLabel());
            }
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

void ANativeWadGuardian::TickRailHazard(float DeltaSeconds)
{
    const ANativeWadLevel* Level = RailLevel.Get();
    if (!Level)
    {
        return;
    }
    const FVector Origin = Level->GetActorLocation();
    FVector Current = GetActorLocation() - Origin;
    const int32 CurrentSector = Level->FindWadSector(FVector2D(Current));
    if (CurrentSector != INDEX_NONE)
    {
        // Passable rail actors have no movement-base physics. Follow the floor beneath them even while an edge blocks XY motion.
        const double Ground = Level->GetWalkingHeight(CurrentSector) + GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        Current.Z = FMath::Max(Ground, Current.Z - 350.0 * DeltaSeconds);
        SetActorLocation(Origin + Current, false, nullptr, ETeleportType::TeleportPhysics);
    }
    const FVector2D ToEnd = Profile.RailEnd - FVector2D(Current);
    const bool bWrap = ToEnd.SizeSquared() < FMath::Square(Profile.CollisionRadius * 0.5f);
    const FVector2D NextXY = bWrap ? Profile.RailStart
        : FVector2D(Current) + ToEnd.GetSafeNormal() * FMath::Min(ToEnd.Size(), double(Profile.WalkSpeed * DeltaSeconds));
    const int32 Sector = Level->FindWadSector(NextXY);
    if (Sector != INDEX_NONE)
    {
        const double Floor = Level->GetWalkingHeight(Sector) + GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        if (bWrap || Floor <= Current.Z + 48.0)
        {
            const double Z = bWrap ? Floor : FMath::Max(Floor, Current.Z);
            SetActorLocation(Origin + FVector(NextXY, Z), false, nullptr, ETeleportType::TeleportPhysics);
        }
    }
    for (TActorIterator<ANativeWadPlayer> Player(GetWorld()); Player; ++Player)
    {
        if (Player->GetHealth() > 0 && IsWithinDamageRadius(**Player, GetActorLocation(), Profile.CollisionRadius))
        {
            Player->ReceiveTypedHit(Profile.MeleeDamage, Profile.MeleeDamageMask);
        }
    }
}

bool ANativeWadGuardian::TakeMeleeHit(ANativeWadPlayer& Player, int32 Damage)
{
    return TakeHit(Damage, &Player, Player.GetMeleeDamageMask());
}

bool ANativeWadGuardian::TakeHit(int32 Damage, ANativeWadPlayer* Credit, uint8 DamageMask)
{
    if (IsDefeated() || Damage <= 0 || DamageMask == 0 || Profile.RailStart != Profile.RailEnd)
    {
        return false;
    }
    if (DamageMask & SpecialDamage)
    {
        if (DamageMask == SpecialDamageFlag(ESpecialDamage::Confuse))
        {
            // CRELOGIC.C loses the target, changes heading, and blocks the previous move.
            bHasTarget = false;
            HitTime = 0.0;
            SetActorRotation(FRotator(0.0, FMath::FRand() * 360.0, 0.0));
            GetCharacterMovement()->StopMovementImmediately();
            ConsumeMovementInputVector();
            return true;
        }
        return false;
    }
    uint8 Resistances = Profile.ResistanceMask;
    if (Profile.bResistsFire)
    {
        Resistances |= DamageFlag(EProjectileDamage::Fire);
    }
    if (Profile.bResistsAcidAndPoison)
    {
        Resistances |= DamageFlag(EProjectileDamage::Acid) | DamageFlag(EProjectileDamage::Poison);
    }
    const int32 BaseDamage = Damage;
    const uint8 AppliedEffects = DamageMask & ~Resistances;
    if (AppliedEffects & DamageFlag(EProjectileDamage::Poison))
    {
        if (PoisonLevel == 0)
        {
            PoisonTime = 5.0;
        }
        PoisonLevel = FMath::Min(int32(MAX_int16), PoisonLevel + Damage / 10);
    }
    if (AppliedEffects & DamageFlag(EProjectileDamage::Fire))
    {
        Damage += BaseDamage >> 2;
    }
    if (AppliedEffects & DamageFlag(EProjectileDamage::Electricity))
    {
        Damage += (BaseDamage * Profile.ArmorClass) / 4;
    }
    if (AppliedEffects & DamageFlag(EProjectileDamage::Piercing))
    {
        Damage += (BaseDamage * Profile.ArmorClass * 4) / 100;
    }
    const int32 Resisted = FMath::CountBits(DamageMask & Resistances);
    Damage -= int32(int64(Damage) * Resisted / FMath::CountBits(DamageMask));
    if (Damage <= 0)
    {
        return false;
    }
    const int32 Applied = FMath::Min(Health, Damage);
    Health -= Applied;
    if (Credit)
    {
        Credit->AwardExperience(Applied);
    }
    HurtUntil = GetWorld()->GetTimeSeconds() + 20 / TicksPerSecond;
    if (IsDefeated())
    {
        RestoreEncounterHealth(0);
        if (Credit)
        {
            Credit->ShowInteractionMessage(Profile.Name + TEXT(" defeated"));
        }
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

void ANativeWadGuardian::RestorePoison(int32 Level, double Time)
{
    PoisonLevel = FMath::Clamp(Level, 0, int32(MAX_int16));
    PoisonTime = FMath::Clamp(Time, 0.0, 5.0);
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

void ANativeWadGuardian::ResetForGenerator()
{
    Health = Profile.MaxHealth;
    PoisonLevel = BoltsFired = MeleeHits = RegeneratedHealth = 0;
    PoisonTime = 5.0;
    MeleeDelay = BoltDelay = 5;
    HitTime = HurtUntil = AttackPoseUntil = LogicTime = 0.0;
    bHasTarget = false;
    Sprite->SetMaterial(0, StandingMaterial);
    GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    GetCharacterMovement()->StopMovementImmediately();
    GetCharacterMovement()->SetMovementMode(Profile.bCanFly ? MOVE_Flying : MOVE_Walking);
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
        Bolt->SetDamageType(EProjectileDamage(Profile.BoltElement));
        const int32 HomingType = Profile.bHomingPlasma ? 4017 : Profile.HomingMissileType;
        if (const FWadWandProjectile* Homing = FindHomingMissile(HomingType))
        {
            Bolt->SetHomingTarget(&Player);
            Bolt->SetBlastRadius(Homing->Radius);
            Bolt->SetDamageMask(Homing->DamageMask);
        }
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
    if (const AActor* Target = HomingTarget.Get())
    {
        constexpr float TurnSpeed = 22.5f * TicksPerSecond / 6.0f;
        const FRotator Heading = FMath::RInterpConstantTo(Velocity.Rotation(),
            (Target->GetActorLocation() - Start).Rotation(), DeltaSeconds, TurnSpeed);
        Velocity = Heading.Vector() * Velocity.Size();
    }
    const FVector End = Start + Velocity * DeltaSeconds;
    FHitResult Hit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(NativeWadBoltFlight), true, this);
    Query.AddIgnoredActor(GetOwner());
    if (GetWorld()->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility,
        FCollisionShape::MakeSphere(5.0f), Query))
    {
        if (BlastRadius > 0.0f)
        {
            ApplyBlast(Hit.Location);
        }
        else if (ANativeWadPlayer* Shooter = Cast<ANativeWadPlayer>(GetOwner()))
        {
            if (ANativeWadGuardian* Guardian = Cast<ANativeWadGuardian>(Hit.GetActor()))
            {
                Guardian->TakeHit(Damage, Shooter, DamageMask);
            }
        }
        else if (Effect == EWadBoltEffect::LockDoor)
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
                DamagePlayer(*Player);
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

void ANativeWadBolt::ApplyForce(ACharacter& Target, const FVector& Origin) const
{
    const bool bPush = DamageMask == SpecialDamageFlag(ESpecialDamage::Push);
    const bool bPull = DamageMask == SpecialDamageFlag(ESpecialDamage::Pull);
    if (!bPush && !bPull) { return; }
    // SERVERSH.C adds damage/90 in source velocity units. OBJMOVE.C advances XV*delta/8 at 70 Hz.
    const float ImpulseSpeed = (Damage / 90) * TicksPerSecond / 8.0f;
    const FVector Direction = (Target.GetActorLocation() - Origin).GetSafeNormal2D();
    Target.GetCharacterMovement()->AddImpulse(Direction * ImpulseSpeed * (bPull ? -1.0f : 1.0f), true);
}

void ANativeWadBolt::ApplyBlast(const FVector& Origin)
{
    ANativeWadPlayer* Shooter = Cast<ANativeWadPlayer>(GetOwner());
    for (TActorIterator<ANativeWadGuardian> Target(GetWorld()); Target; ++Target)
    {
        if (*Target != GetOwner() && !Target->IsHidden() && IsWithinDamageRadius(**Target, Origin, BlastRadius))
        {
            ApplyForce(**Target, Origin);
            Target->TakeHit(Damage, Shooter, DamageMask);
        }
    }
    for (TActorIterator<ANativeWadPlayer> Player(GetWorld()); Player; ++Player)
    {
        if (Player->GetHealth() > 0 && IsWithinDamageRadius(**Player, Origin, BlastRadius))
        {
            ApplyForce(**Player, Origin);
            DamagePlayer(**Player);
        }
    }
}

void ANativeWadBolt::DamagePlayer(ANativeWadPlayer& Player) const
{
    const bool bWasAlive = Player.GetHealth() > 0;
    Player.ReceiveTypedHit(Damage, DamageMask);
    if (bWasAlive && Player.GetHealth() <= 0)
    {
        UE_LOG(LogTemp, Display, TEXT("A missile from %s defeated the player for %d raw damage."),
            GetOwner() ? *GetOwner()->GetActorNameOrLabel() : TEXT("an authored script"), Damage);
    }
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
    if (!(AmmoIndex == INDEX_NONE ? Player.CollectItem(ItemType) : Player.CollectAmmo(AmmoIndex, AmmoQuantity)))
    {
        return false;
    }
    Destroy();
    return true;
}
