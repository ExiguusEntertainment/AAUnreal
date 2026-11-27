#include "NativeWadEncounter.h"

#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadPickupMetadata.h"
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

    float SourceFlightSpeed(const FWadEncounterProfile& Profile)
    {
        // CREDATA.H maxVelocity is independent of stepSize. OBJMOVE.C advances XY by XV*ticks/8.
        float Velocity;
        switch (Profile.SourceType)
        {
        case 1003: case 1037: Velocity = 20.0f; break;
        case 1010: case 1012: Velocity = 30.0f; break;
        case 1021: Velocity = 25.0f; break;
        case 1023: Velocity = 15.0f; break;
        default: return Profile.WalkSpeed;
        }
        return Velocity * TicksPerSecond / 8.0f;
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
    switch (SourceType)
    {
    case 1003: case 1005: ChanceAttackCreature = 50; break;
    case 1006: case 1017: ChanceAttackCreature = 70; break;
    case 1008: case 1025: case 1033: case 1035: case 1036: ChanceAttackCreature = 10; break;
    case 1009: case 1019: case 1023: case 1032: ChanceAttackCreature = 20; break;
    case 1011: case 1014: case 1020: ChanceAttackCreature = 25; break;
    case 1018: case 1022: ChanceAttackCreature = 40; break;
    case 1034: ChanceAttackCreature = 60; break;
    default: if (SourceType != 0) { ChanceAttackCreature = 0; } break;
    }
    // CREDATA.H chanceContinueAttack, used after the initial ten-second Berserk interval.
    switch (SourceType)
    {
    case 1003: ChanceContinueAttack = 50; break;
    case 1005: case 1006: case 1008: case 1009: case 1025: case 1033: case 1035: case 1036: ChanceContinueAttack = 10; break;
    case 1011: ChanceContinueAttack = 40; break;
    case 1014: case 1020: ChanceContinueAttack = 25; break;
    case 1017: ChanceContinueAttack = 60; break;
    case 1018: ChanceContinueAttack = 20; break;
    case 1019: ChanceContinueAttack = 15; break;
    case 1022: ChanceContinueAttack = 30; break;
    case 1023: case 1034: ChanceContinueAttack = 80; break;
    case 1032: ChanceContinueAttack = 5; break;
    default: break;
    }
    if (SourceType == 1003 || SourceType == 1010 || SourceType == 1012 || SourceType == 1023 || SourceType == 1021 || SourceType == 1037) { bCanFly = true; }
    if (SourceType == 1033)
    {
        // CREDATA.H Skeleton Knight, distinct from electric Skeleton1025.
        bCanFly = false;
        MaxHealth = 2500;
        RetreatHealth = 100;
        MeleeDamage = 300;
        MeleeDamageMask = NormalDamage;
        MeleeRange = 30.0f;
        MeleeDelayUpdates = 10;
        UpdateTicks = 7;
        WalkSpeed = 70.0f; // step10 *70/(7+(7>>1)).
        CollisionRadius = 15.0f; // Original SKELKNT.INF.
        RegenPerUpdate = 1;
        ResistanceMask = DamageFlag(EProjectileDamage::Poison);
        ArmorClass = 0;
        VisionHalfAngle = 45.0f; // CRELOGIC_DFT_VISION_FIELD=90 degrees.
        SourceMissileType = 0;
        BoltDamage = 0;
        HomingMissileType = 0;
    }
    if (SourceType == 1012)
    {
        MeleeDamageMask = NormalDamage | DamageFlag(EProjectileDamage::Piercing)
            | DamageFlag(EProjectileDamage::ManaDrain) | DamageFlag(EProjectileDamage::Poison);
        ResistanceMask = MeleeDamageMask;
        MeleeDamage = 100;
        MeleeRange = 250.0f;
        MeleeDelayUpdates = 10;
        UpdateTicks = 7;
        RegenPerUpdate = 3;
        BoltDamage = 0;
        VisionHalfAngle = 67.5f;
    }
    if (SourceType == 1017 || SourceType == 1018 || SourceType == 1019)
    {
        bCanFly = false;
        MeleeDamageMask = NormalDamage;
        MeleeDelayUpdates = SourceType == 1018 ? 30 : 10;
        RangedDelayUpdates = 50;
        UpdateTicks = SourceType == 1018 ? 5 : 7;
        RegenPerUpdate = SourceType == 1019 ? 0 : 1;
        VisionHalfAngle = SourceType == 1017 ? 45.0f : SourceType == 1018 ? 135.0f : 90.0f;
        SourceMissileType = SourceType == 1017 ? 0 : SourceType == 1018 ? 886 : 4018;
        HomingMissileType = SourceType == 1019 ? 4018 : 0;
        BoltDamage = SourceType == 1017 ? 0 : SourceType == 1018 ? 300 : 100;
        MinBoltRange = SourceType == 1017 ? 0.0f : 250.0f;
        BoltRange = SourceType == 1017 ? 0 : SourceType == 1018 ? 2000 : 1000;
    }
    if (SourceType == 1005 || SourceType == 1022)
    {
        bCanFly = false;
        MeleeDelayUpdates = 10;
        RangedDelayUpdates = SourceType == 1005 ? 20 : 50;
        RegenPerUpdate = SourceType == 1005 ? 100 : 1;
        UpdateTicks = SourceType == 1005 ? 12 : 7;
        MeleeDamageMask = SourceType == 1005 ? NormalDamage | DamageFlag(EProjectileDamage::Acid) : NormalDamage;
        ResistanceMask = DamageFlag(SourceType == 1005 ? EProjectileDamage::Acid : EProjectileDamage::Poison);
        SourceMissileType = SourceType == 1005 ? 4004 : 4015;
        BoltDamage = SourceType == 1005 ? 500 : 250;
        MinBoltRange = SourceType == 1005 ? 250.0f : 200.0f;
        BoltRange = SourceType == 1005 ? 2000 : 10000;
        VisionHalfAngle = SourceType == 1005 ? 135.0f : 45.0f;
        bDipper = SourceType == 1022;
    }
    if (SourceType == 1021 || SourceType == 1037)
    {
        MeleeDelayUpdates = 10;
        RangedDelayUpdates = 20;
        RegenPerUpdate = SourceType == 1021 ? 8 : 20;
        UpdateTicks = 7;
        MeleeDamageMask = NormalDamage | DamageFlag(EProjectileDamage::Piercing);
        ResistanceMask = DamageFlag(EProjectileDamage::Fire);
        SourceMissileType = 4005;
        BoltDamage = 750;
        MinBoltRange = 0.0f;
        BoltRange = 10000;
        VisionHalfAngle = 135.0f;
        bApproachWhileFiring = true;
    }
    if (SourceType == 1023)
    {
        // CREDATA.H Griffon: flying berserker, 6-update melee delay, 3 HP regeneration.
        MeleeDelayUpdates = 6;
        UpdateTicks = 7;
        RegenPerUpdate = 3;
    }
    if (SourceType == 3001 || SourceType == 3002)
    {
        // CREDATA.H1898/1944: existing Black/White Barbarian families, not packed aliases.
        const bool bBlack = SourceType == 3001;
        MaxHealth = bBlack ? 3000 : 2500;
        RetreatHealth = bBlack ? 500 : 1000;
        MeleeDamage = bBlack ? 400 : 250;
        MeleeDamageMask = NormalDamage;
        MeleeDelayUpdates = 10;
        BoltDamage = 100; // Magic Dart4011 source profile.
        MinBoltRange = bBlack ? 5000.0f : 250.0f;
        BoltRange = bBlack ? 5000 : 10000;
        RangedDelayUpdates = bBlack ? 50 : 25;
        UpdateTicks = bBlack ? 7 : 5;
        WalkSpeed = 10.0f * TicksPerSecond / (UpdateTicks + (UpdateTicks >> 1));
        MeleeRange = 60.0f;
        VisionHalfAngle = 135.0f;
        ArmorClass = bBlack ? 1 : 0;
        RegenPerUpdate = 1;
        ChanceAttackCreature = ChanceContinueAttack = 75;
        bCanFly = false;
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
    if (Profile.bDipper)
    {
        GetCapsuleComponent()->SetCollisionResponseToChannel(ANativeWadLevel::WaterSurfaceChannel, ECR_Ignore);
        GetCapsuleComponent()->SetCollisionResponseToChannel(ANativeWadLevel::LavaSurfaceChannel, ECR_Ignore);
    }
    GetCharacterMovement()->MaxWalkSpeed = Profile.WalkSpeed;
    GetCharacterMovement()->MaxFlySpeed = SourceFlightSpeed(Profile);
    if (Profile.bCanFly) { GetCharacterMovement()->SetMovementMode(MOVE_Flying); }
    if (Profile.SpriteSize.X > 0 && Profile.SpriteSize.Y > 0)
    {
        Sprite->SetRelativeScale3D(FVector(Profile.SpriteSize * 0.01, 1.0));
    }
    // The short source Wolf still needs a valid Unreal capsule. Keep its small sprite's feet at the capsule base.
    Sprite->SetRelativeLocation(FVector(0, 0, Profile.SourceType == 1017 && Profile.SpriteSize.Y > 0
        ? Profile.SpriteSize.Y * 0.5 - GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 0.0));
}

void ANativeWadGuardian::BeginPlay()
{
    Super::BeginPlay();
    Profile.ApplySourceRules();
    // Existing maps serialized the engine's default MaxFlySpeed before source flight was restored.
    GetCharacterMovement()->MaxWalkSpeed = Profile.WalkSpeed;
    GetCharacterMovement()->MaxFlySpeed = SourceFlightSpeed(Profile);
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

FVector2D ANativeWadGuardian::GetPortraitSize() const
{
    if (Profile.SpriteSize.X > 0.0 && Profile.SpriteSize.Y > 0.0) { return Profile.SpriteSize; }
    if (StandingMaterial)
    {
        TArray<UTexture*> Textures;
        StandingMaterial->GetUsedTextures(Textures, EMaterialQualityLevel::High, true, ERHIFeatureLevel::SM5, true);
        for (UTexture* Texture : Textures)
        {
            if (const UTexture2D* Image = Cast<UTexture2D>(Texture); Image && Image->GetSizeX() > 0 && Image->GetSizeY() > 0)
            { return FVector2D(Image->GetSizeX(), Image->GetSizeY()); }
        }
    }
    const FVector Scale = Sprite->GetRelativeScale3D();
    return FVector2D(FMath::Max(1.0, FMath::Abs(Scale.X) * 100.0), FMath::Max(1.0, FMath::Abs(Scale.Y) * 100.0));
}

int32 ANativeWadGuardian::GetLookSourceType() const
{
    if (Profile.SourceType > 0) { return Profile.SourceType; }
    // Earlier saved slices did not assign a combat source ID. Recover only identities established by their importers.
    if (ActorHasTag(TEXT("L30 Elf Guardian")) || ActorHasTag(TEXT("L301 Elf Guardian 246"))) { return 1018; }
    if (Profile.Name == TEXT("Elymas")) { return 50158; }
    if (Profile.Name == TEXT("Exiguus")) { return 1032; }
    if (Profile.Name == TEXT("Gaiseric Magician")) { return 3002; }
    if (Profile.Name == TEXT("Mounted Knight of Andrew")) { return ChurchMountedKnight; }
    if (Profile.Name == TEXT("Knight of Andrew Crossbowman")) { return ChurchFootKnight; }
    if (Profile.Name == TEXT("Bronze Barbarian")) { return 3000; }
    if (Profile.Name == TEXT("Black Barbarian")) { return 3001; }
    if (Profile.Name == TEXT("White Barbarian")) { return 3002; }
    if (Profile.Name == TEXT("Blue Barbarian")) { return 3003; }
    if (Profile.Name == TEXT("Skeleton Knight")) { return 1033; }
    return 0;
}

bool ANativeWadGuardian::CanSeePlayer(const ANativeWadPlayer& Player) const
{
    return CanSeeTarget(Player);
}

bool ANativeWadGuardian::CanSeeTarget(const AActor& Target) const
{
    if (const ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(&Target); Player && Player->IsConcealed()) { return false; }
    FHitResult Hit;
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(NativeWadGuardianSight), true, this);
    FVector SightOrigin = GetActorLocation();
    if (Profile.bDipper && DipperLevel.IsValid())
    {
        const ANativeWadLevel* Level = DipperLevel.Get();
        const int32 Sector = Level->FindWadSector(FVector2D(SightOrigin));
        if (Sector != INDEX_NONE)
        {
            // The source leaves 34 pixels visible while dipped. Scan from that exposed portion.
            SightOrigin.Z = FMath::Max(SightOrigin.Z, Level->GetActorLocation().Z + Level->GetWalkingHeight(Sector) + 17.0);
        }
    }
    return !GetWorld()->LineTraceSingleByChannel(Hit, SightOrigin, Target.GetActorLocation(), ECC_Visibility, Query)
        || Hit.GetActor() == &Target;
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
    SlowSeconds = FMath::Max(0.0, SlowSeconds - DeltaSeconds);
    EarthboundSeconds = FMath::Max(0.0, EarthboundSeconds - DeltaSeconds);
    ApplyControlMovement();
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
    if (IsDefeated())
    {
        if (Profile.bDipper || Profile.SourceType == 1012) { TickDipper(DeltaSeconds, false); }
        return;
    }

    LogicTime += DeltaSeconds;
    const bool bRetreat = Profile.SourceType == 1012 || Profile.SourceType == 1017 ? false
        : Profile.bDipper ? Health <= Profile.RetreatHealth : Health < Profile.RetreatHealth;
    // CRELOGIC.C extends the table's update by half and shortens the fleeing update by a quarter.
    const int32 UpdateTicks = SlowSeconds > 0.0 ? Profile.UpdateTicks * 3 : Profile.UpdateTicks + (Profile.UpdateTicks >> 1);
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

    BerserkSeconds = FMath::Max(0.0, BerserkSeconds - DeltaSeconds);
    ANativeWadGuardian* Enemy = BerserkTarget.Get();
    if (Enemy && (Enemy->IsDefeated() || Enemy->IsHidden() || Enemy->GetCreatureLife() != BerserkTargetLife
        || (BerserkSeconds == 0.0 && Updates > 0 && FMath::RandRange(0, 65535) % 100 >= Profile.ChanceContinueAttack)))
    {
        ClearBerserkTarget();
        Enemy = nullptr;
    }
    AActor* Target = Enemy ? static_cast<AActor*>(Enemy) : static_cast<AActor*>(Player);
    if (!Target) { return; }
    const FVector Offset = Target->GetActorLocation() - GetActorLocation();
    const FVector TowardPlayer = Profile.bCanFly && EarthboundSeconds == 0.0 ? Offset.GetSafeNormal() : Offset.GetSafeNormal2D();
    const double Distance = Offset.Size2D();
    const bool bCloseRetreat = Profile.bKeepsBoltDistance && Distance < Profile.MinBoltRange;
    const double Now = GetWorld()->GetTimeSeconds();
    UMaterialInterface* Pose = Now < HurtUntil ? HurtMaterial.Get()
        : Now < AttackPoseUntil ? AttackingMaterial.Get() : StandingMaterial.Get();
    if (Pose && Pose != Sprite->GetMaterial(0))
    {
        Sprite->SetMaterial(0, Pose);
    }
    const bool bVisible = (Enemy || (Player && !Player->IsDefeated())) && CanSeeTarget(*Target);
    // INavDipper first sinks while scanning, then fully emerges before guard navigation.
    const bool bDipperReady = !Profile.bDipper || TickDipper(DeltaSeconds, bHasTarget);
    bool bMeleeHeight = FMath::Abs(Offset.Z) <= 26.0;
    if (Profile.SourceType == 1005 || Profile.SourceType == 1021 || Profile.SourceType == 1022 || Profile.SourceType == 1037)
    {
        if (const ACharacter* Character = Cast<ACharacter>(Target))
        {
            const double Height = GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.0;
            const double Feet = GetActorLocation().Z - Height * 0.5;
            const double TargetHalfHeight = Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
            // ITargetStandardHitOrShoot attacks from the bottom quarter through the creature's full height.
            bMeleeHeight = Feet + Height >= Target->GetActorLocation().Z - TargetHalfHeight
                && Feet + Height * 0.25 <= Target->GetActorLocation().Z + TargetHalfHeight;
        }
    }
    const bool bMelee = bVisible && Distance < Profile.MeleeRange && bMeleeHeight;
    if (HitTime > 0.0 && Now >= HitTime)
    {
        HitTime = 0.0;
        if (bMelee)
        {
            ++MeleeHits;
            if (Enemy) { Enemy->TakeHit(Profile.MeleeDamage, nullptr, Profile.MeleeDamageMask); }
            else { Player->ReceiveTypedHit(Profile.MeleeDamage, Profile.MeleeDamageMask); }
            if (!Enemy && Player->IsDefeated())
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
    if (!bDipperReady) { return; }
    const bool bHasMissile = Profile.BoltDamage > 0 && BoltMaterial;
    if (bRetreat || bCloseRetreat || (!bMelee && (Profile.bApproachWhileFiring || Profile.bDipper || !bHasMissile
        || Distance > Profile.BoltRange || Distance <= Profile.MinBoltRange)))
    {
        const FVector Direction = bRetreat || bCloseRetreat ? -TowardPlayer : TowardPlayer;
        if (!Profile.bDipper || CanMoveOnLiquid(Direction, DeltaSeconds)) { AddMovementInput(Direction, 1.0f, true); }
        else { GetCharacterMovement()->StopMovementImmediately(); ConsumeMovementInputVector(); }
    }
    if (Updates == 0)
    {
        return;
    }
    if (Profile.SourceType == 1012)
    {
        // ITargetScream triggers ten units inside the radius, then damages the complete area without harming its owner.
        if (MeleeDelay == 0 && Distance < Profile.MeleeRange - 10.0f)
        {
            MeleeDelay = Profile.MeleeDelayUpdates;
            AttackPoseUntil = Now + 20 / TicksPerSecond;
            GetCharacterMovement()->StopMovementImmediately();
            ConsumeMovementInputVector();
            ++ScreamsFired;
            const FVector Origin = GetActorLocation();
            const auto Push = [&](ACharacter& Victim)
            {
                const FVector Direction = (Victim.GetActorLocation() - Origin).GetSafeNormal2D();
                Victim.GetCharacterMovement()->AddImpulse(Direction * ((Profile.MeleeDamage / 90) * TicksPerSecond / 8.0f), true);
            };
            for (TActorIterator<ANativeWadGuardian> Victim(GetWorld()); Victim; ++Victim)
            {
                if (*Victim != this && !Victim->IsHidden() && !Victim->IsDefeated()
                    && IsWithinDamageRadius(**Victim, Origin, Profile.MeleeRange))
                {
                    Push(**Victim);
                    Victim->TakeHit(Profile.MeleeDamage, nullptr, Profile.MeleeDamageMask);
                }
            }
            for (TActorIterator<ANativeWadPlayer> Victim(GetWorld()); Victim; ++Victim)
            {
                if (!Victim->IsDefeated() && IsWithinDamageRadius(**Victim, Origin, Profile.MeleeRange))
                {
                    Push(**Victim);
                    Victim->ReceiveTypedHit(Profile.MeleeDamage, Profile.MeleeDamageMask);
                }
            }
        }
        return;
    }
    if (bMelee && Profile.MeleeDamage > 0 && MeleeDelay == 0)
    {
        MeleeDelay = Profile.MeleeDelayUpdates;
        HitTime = Now + 18 / TicksPerSecond;
        AttackPoseUntil = Now + 20 / TicksPerSecond;
    }
    else if (bHasMissile && !bMelee && Distance <= Profile.BoltRange
        && (!Profile.bDipper || bRetreat)
        && (Profile.bKeepsBoltDistance || Distance > Profile.MinBoltRange || bRetreat) && BoltDelay == 0)
    {
        BoltDelay = Profile.RangedDelayUpdates;
        AttackPoseUntil = Now + 20 / TicksPerSecond;
        FireBolt(*Target);
    }
}

double ANativeWadGuardian::GetDipDepth() const
{
    const ANativeWadLevel* Level = DipperLevel.Get();
    if ((!Profile.bDipper && Profile.SourceType != 1012) || !Level) { return 0.0; }
    const int32 Sector = Level->FindWadSector(FVector2D(GetActorLocation()));
    if (Sector == INDEX_NONE) { return 0.0; }
    const double Floor = Level->GetActorLocation().Z + Level->GetWalkingHeight(Sector);
    return FMath::Max(0.0, Floor + GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - GetActorLocation().Z);
}

bool ANativeWadGuardian::TickDipper(float DeltaSeconds, bool bRise)
{
    if (!DipperLevel.IsValid())
    {
        const TActorIterator<ANativeWadLevel> Level(GetWorld());
        DipperLevel = Level ? *Level : nullptr;
    }
    const ANativeWadLevel* Level = DipperLevel.Get();
    if (!Level) { return false; }
    const int32 Sector = Level->FindWadSector(FVector2D(GetActorLocation()));
    if (Sector == INDEX_NONE) { return false; }
    const double HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    const double Floor = Level->GetActorLocation().Z + Level->GetWalkingHeight(Sector);
    const double TargetZ = Floor + (bRise ? HalfHeight + 2.0 : 34.0 - HalfHeight);
    DipperTime += DeltaSeconds;
    const int32 UpdateTicks = SlowSeconds > 0.0 ? Profile.UpdateTicks * 3 : Profile.UpdateTicks + (Profile.UpdateTicks >> 1);
    const double Interval = UpdateTicks / TicksPerSecond;
    const int32 Updates = FMath::FloorToInt(DipperTime / Interval);
    DipperTime -= Updates * Interval;
    const double Step = Profile.WalkSpeed * 10.0 / TicksPerSecond * Updates;
    FVector Position = GetActorLocation();
    Position.Z = FMath::FInterpConstantTo(Position.Z, TargetZ, 1.0, Step);
    const bool bFullyUp = bRise && FMath::IsNearlyEqual(Position.Z, TargetZ, 0.01);
    if (!bFullyUp)
    {
        // No horizontal moves occur while submerged. Preserve the source body height below the floor.
        GetCharacterMovement()->DisableMovement();
        GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Ignore);
        ConsumeMovementInputVector();
    }
    SetActorLocation(Position, false, nullptr, ETeleportType::TeleportPhysics);
    if (bFullyUp)
    {
        GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
        if (!GetCharacterMovement()->IsMovingOnGround()) { GetCharacterMovement()->SetMovementMode(MOVE_Walking); }
    }
    else if (!bRise && IsDefeated() && FMath::IsNearlyEqual(Position.Z, TargetZ, 0.01))
    {
        // IDeathSink marks the object for destruction once it reaches the dipped height.
        // Retain the actor for checkpoints and generator pooling, but remove it from play.
        SetActorHiddenInGame(true);
        SetActorTickEnabled(false);
    }
    return bFullyUp;
}

bool ANativeWadGuardian::CanMoveOnLiquid(const FVector& Direction, float DeltaSeconds) const
{
    const ANativeWadLevel* Level = DipperLevel.Get();
    if (!Level) { return false; }
    const FVector Position = GetActorLocation() + Direction * FMath::Max(double(Profile.WalkSpeed * DeltaSeconds), 2.0);
    const double Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
    // Match the source stayOnSector rule over the moving object's footprint, not only its center.
    for (const FVector2D Offset : {FVector2D::ZeroVector, FVector2D(Radius, 0), FVector2D(-Radius, 0), FVector2D(0, Radius), FVector2D(0, -Radius)})
    {
        const int32 Sector = Level->FindWadSector(FVector2D(Position) + Offset);
        if (!Level->GetSectors().IsValidIndex(Sector)) { return false; }
        const FAAWadNativeSector& Entry = Level->GetSectors()[Sector];
        if (!Entry.bWater && !Entry.bLava) { return false; }
    }
    return true;
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
        if (!Player->IsDefeated() && IsWithinDamageRadius(**Player, GetActorLocation(), Profile.CollisionRadius))
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
        if (DamageMask == SpecialDamageFlag(ESpecialDamage::Berserk))
        {
            ANativeWadGuardian* Closest = nullptr;
            double Distance = TNumericLimits<double>::Max();
            for (TActorIterator<ANativeWadGuardian> Candidate(GetWorld()); Candidate; ++Candidate)
            {
                if (*Candidate == this || Candidate->IsHidden() || Candidate->IsDefeated()
                    || Candidate->Profile.RailStart != Candidate->Profile.RailEnd) { continue; }
                const double NextDistance = FVector::DistSquared2D(GetActorLocation(), Candidate->GetActorLocation());
                if (NextDistance < Distance) { Closest = *Candidate; Distance = NextDistance; }
            }
            const AActor* Previous = BerserkTarget.Get();
            if (!Previous && bHasTarget) { Previous = UGameplayStatics::GetPlayerCharacter(this, 0); }
            const bool bCandidatePreferred = Closest && (!Previous || (CanSeeTarget(*Closest)
                && (!CanSeeTarget(*Previous) || Distance < FVector::DistSquared2D(GetActorLocation(), Previous->GetActorLocation()))));
            bHasTarget = false;
            HitTime = 0.0;
            if (bCandidatePreferred && FMath::RandRange(0, 65535) % 100 < Profile.ChanceAttackCreature)
            { RestoreBerserkTarget(Closest, 10.0, Closest->GetCreatureLife()); }
            else if (BerserkTarget.IsValid()) { BerserkSeconds = 10.0; }
            return true;
        }
        if (DamageMask == SpecialDamageFlag(ESpecialDamage::Slow)
            || (DamageMask == SpecialDamageFlag(ESpecialDamage::Earthbind) && Profile.bCanFly))
        {
            // CRELOGIC.C uses an absolute deadline; CLIENT.C resets SyncTime to 1 when loading a map.
            const double Remaining = FMath::Max(0.0, (FMath::RandRange(0, 65535) + 2100.0) / TicksPerSecond - GetWorld()->GetTimeSeconds());
            if (DamageMask == SpecialDamageFlag(ESpecialDamage::Slow)) { SlowSeconds = Remaining; }
            else { EarthboundSeconds = Remaining; }
            ApplyControlMovement();
            return true;
        }
        if (DamageMask == SpecialDamageFlag(ESpecialDamage::Confuse))
        {
            // CRELOGIC.C loses the target, changes heading, and blocks the previous move.
            ClearBerserkTarget();
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
        DropDefeatTreasure();
        RestoreEncounterHealth(0);
        if (Credit)
        {
            Credit->ShowInteractionMessage(Profile.Name + TEXT(" defeated"));
        }
        UE_LOG(LogTemp, Display, TEXT("Defeated the authored %s."), *Profile.Name);
    }
    return true;
}

void ANativeWadGuardian::DropDefeatTreasure()
{
    if (bDefeatTreasureHandled) { return; }
    bDefeatTreasureHandled = true;
    const int32 SourceType = GetLookSourceType(); // The known legacy Guardian tags also establish its original treasure identity.
    if (ResolveWadCreatureTreasure(SourceType, 0).ItemType == 0) { return; }
    const TActorIterator<ANativeWadLevel> Level(GetWorld());
    if (!Level) { return; } // Isolated combat fixtures can run without a native map.
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    const int32 OffsetX = uint16(FMath::Rand32()) % 6 - 3;
    const int32 OffsetY = uint16(FMath::Rand32()) % 6 - 3;
    const FWadCreatureTreasure Treasure = ResolveWadCreatureTreasure(SourceType, uint16(FMath::Rand32()));
    const FWadInventoryDetails* Details = Catalog ? Catalog->ItemDetails.Find(Treasure.ItemType) : nullptr;
    if (!Details || Details->SpriteSize.X <= 0 || Details->SpriteSize.Y <= 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("The source treasure for %s is missing catalog item %d."), *Profile.Name, Treasure.ItemType);
        return;
    }
    FVector Position = GetActorLocation();
    Position.X += OffsetX - (Treasure.AmmoIndex != INDEX_NONE ? 2 : 0);
    Position.Y += OffsetY;
    Position.Z += Details->SpriteSize.Y * 0.5 - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    if (ANativeWadPickup* Drop = ANativeWadPickup::SpawnDropped(**Level, Treasure.ItemType, Position))
    {
        Drop->SetOwner(this);
        if (Treasure.AmmoIndex != INDEX_NONE) { Drop->SetAuthoredAmmo(Treasure.AmmoIndex, Treasure.AmmoQuantity); }
        if (Treasure.HasAccessoryData()) { Drop->SetAccessoryData(Treasure.AccessoryData); }
        UE_LOG(LogTemp, Display, TEXT("Released source creature treasure: type %d, ammo %d, quantity %d, auxiliary %d, life %d."),
            Treasure.ItemType, Treasure.AmmoIndex, Treasure.AmmoQuantity, Treasure.AccessoryData, CreatureLife);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Could not spawn source treasure item %d for %s."), Treasure.ItemType, *Profile.Name);
    }
}

void ANativeWadGuardian::RestoreEncounterHealth(int32 SavedHealth)
{
    Health = FMath::Clamp(SavedHealth, 0, Profile.MaxHealth);
    // Reopening a defeated life never rerolls treasure; its existing pickup is restored by the level checkpoint.
    bDefeatTreasureHandled = IsDefeated();
    if (IsDefeated())
    {
        GetCharacterMovement()->DisableMovement();
        GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Sprite->SetMaterial(0, DownedMaterial);
        if (Profile.bDipper || Profile.SourceType == 1012) { TickDipper(0.0f, false); }
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

void ANativeWadGuardian::ApplyControlMovement()
{
    if (IsDefeated()) { return; }
    UCharacterMovementComponent* Movement = GetCharacterMovement();
    // Flyers use source maxVelocity=2; walking steps run half as often (update*3 versus update*1.5).
    const float MoveSpeed = SlowSeconds > 0.0
        ? Profile.bCanFly ? 2.0f * TicksPerSecond / 8.0f : Profile.WalkSpeed * 0.5f : Profile.WalkSpeed;
    Movement->MaxWalkSpeed = MoveSpeed;
    Movement->MaxFlySpeed = SlowSeconds > 0.0 ? MoveSpeed : SourceFlightSpeed(Profile);
    if (Profile.bCanFly)
    {
        if (EarthboundSeconds == 0.0 && !Movement->IsFlying()) { Movement->SetMovementMode(MOVE_Flying); }
        else if (EarthboundSeconds > 0.0 && Movement->IsFlying()) { Movement->SetMovementMode(MOVE_Falling); }
    }
}

void ANativeWadGuardian::RestoreControlEffects(double Slow, double Earthbound)
{
    SlowSeconds = FMath::IsFinite(Slow) ? FMath::Clamp(Slow, 0.0, MaxCreatureControlSeconds) : 0.0;
    EarthboundSeconds = Profile.bCanFly && FMath::IsFinite(Earthbound) ? FMath::Clamp(Earthbound, 0.0, MaxCreatureControlSeconds) : 0.0;
    ApplyControlMovement();
}

void ANativeWadGuardian::ClearBerserkTarget()
{
    BerserkTarget.Reset();
    BerserkSeconds = 0.0;
    BerserkTargetLife = 0;
    HitTime = 0.0;
}

void ANativeWadGuardian::RestoreBerserkTarget(ANativeWadGuardian* Target, double Seconds, int32 TargetLife)
{
    ClearBerserkTarget();
    if (!Target || Target == this || Target->IsDefeated() || Target->IsHidden()
        || Target->GetCreatureLife() != TargetLife || !FMath::IsFinite(Seconds)) { return; }
    BerserkTarget = Target;
    BerserkTargetLife = TargetLife;
    BerserkSeconds = FMath::Clamp(Seconds, 0.0, 10.0);
    bHasTarget = true;
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
    CreatureLife = CreatureLife == MAX_int32 ? MAX_int32 : CreatureLife + 1;
    ClearBerserkTarget();
    Health = Profile.MaxHealth;
    PoisonLevel = BoltsFired = MeleeHits = ScreamsFired = RegeneratedHealth = 0;
    PoisonTime = 5.0;
    SlowSeconds = EarthboundSeconds = 0.0;
    GetCharacterMovement()->MaxWalkSpeed = Profile.WalkSpeed;
    GetCharacterMovement()->MaxFlySpeed = SourceFlightSpeed(Profile);
    MeleeDelay = BoltDelay = 5;
    HitTime = HurtUntil = AttackPoseUntil = LogicTime = 0.0;
    DipperTime = 0.0;
    bHasTarget = false;
    bDefeatTreasureHandled = false;
    Sprite->SetMaterial(0, StandingMaterial);
    GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
    GetCharacterMovement()->StopMovementImmediately();
    GetCharacterMovement()->SetMovementMode(Profile.bCanFly ? MOVE_Flying : MOVE_Walking);
}

void ANativeWadGuardian::FireBolt(AActor& Target)
{
    const FVector Start = GetActorLocation();
    const FVector Direction = (Target.GetActorLocation() + FVector(0, 0, 10) - Start).GetSafeNormal();
    FActorSpawnParameters Parameters;
    Parameters.Owner = this;
    Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (ANativeWadBolt* Bolt = GetWorld()->SpawnActor<ANativeWadBolt>(Start, Direction.Rotation(), Parameters))
    {
        Bolt->Launch(Sprite->GetStaticMesh(), BoltMaterial, Direction * (30 * TicksPerSecond), Profile.BoltDamage);
        Bolt->SetDamageType(EProjectileDamage(Profile.BoltElement));
        if (Profile.SourceMissileType == 886)
        {
            // CREDATA.H normal effect bolt, used by the original Elf Archer.
            Bolt->SetDamageMask(NormalDamage);
            Bolt->SetBlastRadius(20.0f);
        }
        if (Profile.SourceMissileType != 0)
        {
            for (const auto& Missile : WandProjectiles)
            {
                if (Missile.Type != Profile.SourceMissileType) { continue; }
                Bolt->SetBlastRadius(Missile.Radius);
                Bolt->SetDamageMask(Missile.DamageMask);
                Bolt->SetLifeSpan(FMath::Max(2.0f, Profile.BoltRange / (30.0f * TicksPerSecond) + 1.0f));
                break;
            }
        }
        const int32 HomingType = Profile.bHomingPlasma ? 4017 : Profile.HomingMissileType;
        if (const FWadWandProjectile* Homing = FindHomingMissile(HomingType))
        {
            Bolt->SetHomingTarget(&Target);
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
        else if (Effect == EWadBoltEffect::LockDoor)
        {
            if (ANativeWadLevel* Level = ScriptTrap.Get())
            {
                Level->LockScriptDoorsNear(FVector2D(Hit.ImpactPoint));
            }
        }
        else if (ANativeWadGuardian* Guardian = Cast<ANativeWadGuardian>(Hit.GetActor()))
        {
            Guardian->TakeHit(Damage, Cast<ANativeWadPlayer>(GetOwner()), DamageMask);
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
    if (DamageMask == SpecialDamageFlag(ESpecialDamage::Lock) || DamageMask == SpecialDamageFlag(ESpecialDamage::Unlock))
    {
        const TActorIterator<ANativeWadLevel> Level(GetWorld());
        const bool bChanged = Level && Level->ModifyDoorLocksNear(FVector2D(Origin), DamageMask == SpecialDamageFlag(ESpecialDamage::Lock), BlastRadius);
        if (ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(GetOwner()))
        { Player->ShowInteractionMessage(bChanged ? TEXT("Door lock changed.") : TEXT("No door lock changed.")); }
        return;
    }
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
        if (!Player->IsDefeated() && IsWithinDamageRadius(**Player, Origin, BlastRadius))
        {
            ApplyForce(**Player, Origin);
            DamagePlayer(**Player);
        }
    }
}

void ANativeWadBolt::DamagePlayer(ANativeWadPlayer& Player) const
{
    const bool bWasAlive = !Player.IsDefeated();
    Player.ReceiveTypedHit(Damage, DamageMask);
    if (bWasAlive && Player.IsDefeated())
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
    AmmoIndex = INDEX_NONE;
    AmmoQuantity = 0;
    AccessoryData = 0;
    bAccessoryDataRecorded = false;
    bCollectionCommitted = false;
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

double ANativeWadPickup::GetMagicMapRadius() const
{
    const auto* Catalog=UNativeWadHudData::LoadCatalog();
    const auto* Rules=Catalog?Catalog->ItemDetails.Find(ItemType):nullptr;
    return Rules?Rules->WorldRadius:0;
}

bool ANativeWadPickup::IsMagicMapInvisible() const
{
    const auto* Catalog=UNativeWadHudData::LoadCatalog();
    const auto* Rules=Catalog?Catalog->ItemDetails.Find(ItemType):nullptr;
    return Rules&&(Rules->WorldAttributes&0x0004)!=0;
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

bool ANativeWadPickup::SetAccessoryData(int32 Value)
{
    if (Value < 0 || Value > MAX_uint16) { return false; }
    AccessoryData = uint16(Value);
    bAccessoryDataRecorded = true;
    return true;
}

void ANativeWadPickup::RestoreAccessoryData(uint16 Value, bool bRecorded)
{
    bAccessoryDataRecorded = bRecorded;
    AccessoryData = bRecorded ? Value : 0;
}

int32 ANativeWadPickup::GetAmmoIndex() const
{
    return NativeWadPickupMetadata::ResolveAmmo(ItemType, bAccessoryDataRecorded, AccessoryData, AmmoIndex, AmmoQuantity).Index;
}

int32 ANativeWadPickup::GetAmmoQuantity() const
{
    return NativeWadPickupMetadata::ResolveAmmo(ItemType, bAccessoryDataRecorded, AccessoryData, AmmoIndex, AmmoQuantity).Quantity;
}

bool ANativeWadPickup::Collect(ANativeWadPlayer& Player)
{
    if (ItemType <= 0 || bCollectionCommitted || IsActorBeingDestroyed() || !IsAvailable()) { return false; }
    if (ItemType == NativeWadPickupMetadata::JournalObjectType)
    {
        // A journal scroll's GET effect consumes the world object immediately. It never needs
        // a carried-object identity; withdrawing a physical page is separate inventory work.
        const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
        UTexture2D* Image = Catalog && bAccessoryDataRecorded
            && AccessoryData < NativeWadPickupMetadata::MaximumJournalPages
            ? Catalog->JournalPageCatalog.FindRef(AccessoryData).Get() : nullptr;
        auto* Instance = Player.GetGameInstance();
        auto* Travel = Instance ? Instance->GetSubsystem<UNativeWadTravelState>() : nullptr;
        if (!Image || !Travel)
        {
            Player.ShowInteractionMessage(TEXT("You cannot read this journal entry."));
            UE_LOG(LogTemp, Warning, TEXT("Cannot collect source journal object373: recorded=%d, page=%u, saved picture=%d, travel=%d."),
                bAccessoryDataRecorded, uint32(AccessoryData), Image != nullptr, Travel != nullptr);
            return false;
        }
        const bool bAlready = Travel->JournalPages.Contains(AccessoryData);
        bCollectionCommitted = true;
        Travel->JournalPages.Add(AccessoryData, Image);
        Player.ShowInteractionMessage(bAlready ? TEXT("You already had a copy of this note.") : TEXT("You found a journal entry"));
        if (auto* Controller = Player.GetController<APlayerController>())
        { if (auto* Hud = Controller->GetHUD<ANativeWadHUD>()) { Hud->ShowJournalPage(AccessoryData); } }
    }
    else
    {
        const auto Ammo = NativeWadPickupMetadata::ResolveAmmo(ItemType, bAccessoryDataRecorded, AccessoryData, AmmoIndex, AmmoQuantity);
        if (!(Ammo.Index == INDEX_NONE ? Player.CollectItem(ItemType) : Player.CollectAmmo(Ammo.Index, Ammo.Quantity))) { return false; }
        bCollectionCommitted = true;
    }
    Destroy();
    return true;
}
