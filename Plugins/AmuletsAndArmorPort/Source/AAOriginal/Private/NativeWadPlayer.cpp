#include "NativeWadPlayer.h"

#include "Algo/Count.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "TimerManager.h"

using namespace NativeWadRules;

ANativeWadPlayer::ANativeWadPlayer()
{
    GetCapsuleComponent()->InitCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    BaseEyeHeight = 22.0f;
    bUseControllerRotationYaw = true;
    GetCharacterMovement()->MaxWalkSpeed = 190.0f;
    GetCharacterMovement()->MaxStepHeight = 40.0f;
    ViewCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
    ViewCamera->SetupAttachment(GetCapsuleComponent());
    ViewCamera->SetRelativeLocation(FVector(0, 0, BaseEyeHeight));
    ViewCamera->bUsePawnControlRotation = true;
}

void ANativeWadPlayer::BeginPlay()
{
    Super::BeginPlay();
    if (TActorIterator<ANativeWadLevel> Level(GetWorld()); Level)
    {
        NativeLevel = *Level;
    }
    ReadTravelOptions(GetWorld()->URL);
    bRestoreCheckpointOnTick = GetWorld()->URL.HasOption(TEXT("WadLoad"));
}

void ANativeWadPlayer::ReadTravelOptions(const FURL& URL)
{
    const auto ReadStat = [&URL](const TCHAR* Key, int32& Stat, int32 Low, int32 High)
    {
        int32 Value = 0;
        if (const TCHAR* Option = URL.GetOption(Key, nullptr); Option && LexTryParseString(Value, Option))
        {
            Stat = FMath::Clamp(Value, Low, High);
        }
    };
    ReadStat(TEXT("WadLevel="), CharacterLevel, 1, 100);
    ReadStat(TEXT("WadNextXP="), ExperienceNeeded, FirstLevelThreshold, MAX_int32);
    ReadStat(TEXT("WadMaxHealth="), MaxHealth, NativeWadRules::MaxHealth, 30000);
    ReadStat(TEXT("WadMaxMana="), MaxMana, NativeWadRules::MaxMana, 30000);
    ReadStat(TEXT("WadHealth="), Health, 1, MaxHealth);
    ReadStat(TEXT("WadMana="), Mana, 0, MaxMana);
    ReadStat(TEXT("WadFood="), Food, 0, MaxFood);
    ReadStat(TEXT("WadWater="), Water, 0, MaxFood);
    ReadStat(TEXT("WadXP="), Experience, 0, MAX_int32);
    ReadStat(TEXT("WadCasts="), SuccessfulCasts, 0, MAX_int32);
    ReadStat(TEXT("WadPoison="), PoisonLevel, 0, MAX_int16);
    if (const TCHAR* Value = URL.GetOption(TEXT("WadAttributes="), nullptr))
    {
        TArray<FString> Values;
        FString(Value).ParseIntoArray(Values, TEXT(","));
        if (Values.Num() == AttributeCount)
        {
            for (int32 Index = 0; Index < AttributeCount; ++Index)
            {
                int32 Attribute = 0;
                if (LexTryParseString(Attribute, *Values[Index]))
                {
                    Attributes[Index] = FMath::Clamp(Attribute, PriestAttributes[Index], 100);
                }
            }
        }
    }
    if (const TCHAR* Value = URL.GetOption(TEXT("WadItems="), nullptr))
    {
        CarriedItems.Reset();
        TArray<FString> Items;
        FString(Value).ParseIntoArray(Items, TEXT(","));
        for (const FString& Entry : Items)
        {
            int32 Type;
            if (LexTryParseString(Type, *Entry) && Type > 0 && Type <= MAX_int16)
            {
                CarriedItems.Add(Type);
            }
        }
    }
    ReadStat(TEXT("WadReady="), ReadySlot, INDEX_NONE, CarriedItems.Num() - 1);
    if (ReadySlot == INDEX_NONE && URL.GetOption(TEXT("WadReady="), nullptr) == nullptr)
    {
        int32 OldWeapon = 0;
        ReadStat(TEXT("WadWeapon="), OldWeapon, 0, IronDagger);
        if (OldWeapon == IronDagger)
        {
            ReadySlot = CarriedItems.Find(IronDagger);
        }
    }
}

void ANativeWadPlayer::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    Input->BindKey(EKeys::E, IE_Pressed, this, &ANativeWadPlayer::UsePressed);
    Input->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ANativeWadPlayer::ClickPressed);
    Input->BindKey(EKeys::LeftControl, IE_Pressed, this, &ANativeWadPlayer::UseReadyHand);
    Input->BindKey(EKeys::NumPadSeven, IE_Pressed, this, &ANativeWadPlayer::ToggleInventory);
    Input->BindKey(EKeys::Tab, IE_Pressed, this, &ANativeWadPlayer::ToggleMouseLook);
    Input->BindKey(EKeys::NumPadFour, IE_Pressed, this, &ANativeWadPlayer::EnterRuneFour);
    Input->BindKey(EKeys::NumPadFive, IE_Pressed, this, &ANativeWadPlayer::EnterRuneFive);
    Input->BindKey(EKeys::BackSpace, IE_Pressed, this, &ANativeWadPlayer::RuneBackspace);
    Input->BindKey(EKeys::Enter, IE_Pressed, this, &ANativeWadPlayer::CastRejuvenate);
    Input->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &ANativeWadPlayer::CastRejuvenate);
    Input->BindAxisKey(EKeys::MouseX, this, &ANativeWadPlayer::LookYaw);
    Input->BindAxisKey(EKeys::MouseY, this, &ANativeWadPlayer::LookPitch);
    Input->BindKey(EKeys::SpaceBar, IE_Pressed, this, &ANativeWadPlayer::Jump);
    Input->BindKey(EKeys::SpaceBar, IE_Released, this, &ANativeWadPlayer::StopJumping);
}

void ANativeWadPlayer::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bRestoreCheckpointOnTick && GetController())
    {
        bRestoreCheckpointOnTick = false;
        RestoreWadCheckpoint();
    }
    if (QuakeTime > 0.0)
    {
        QuakeTime = FMath::Max(0.0, QuakeTime - DeltaSeconds);
        const double Phase = GetWorld()->GetTimeSeconds() * 22.0;
        ViewCamera->ClearAdditiveOffset();
        if (QuakeTime > 0.0)
        {
            ViewCamera->AddAdditiveOffset(FTransform(FVector(5 * FMath::Sin(Phase), 5 * FMath::Sin(1.3 * Phase),
                5 * FMath::Cos(0.8 * Phase))), 0.0f);
        }
    }
    if (const APlayerController* Player = Cast<APlayerController>(GetController()); Player && Player->IsLocalController() && Health > 0)
    {
        const float Forward = float(Player->IsInputKeyDown(EKeys::W)) - float(Player->IsInputKeyDown(EKeys::S));
        const float Right = float(Player->IsInputKeyDown(EKeys::D)) - float(Player->IsInputKeyDown(EKeys::A));
        if (!bInventoryOpen)
        {
            MoveInViewDirection(Forward, Right);
        }
        if (ShouldShowEncounterHUD())
        {
            TickNeeds(DeltaSeconds);
        }
        if (ANativeWadLevel* Level = NativeLevel.Get())
        {
            Level->CheckActivitySector(FVector2D(GetActorLocation()), LastActivitySector);
        }
        CheckLevelLink();
    }
}

void ANativeWadPlayer::CheckLevelLink()
{
    const ANativeWadLevel* Level = NativeLevel.Get();
    if (!Level || Level->GetLevelLinks().IsEmpty())
    {
        return;
    }
    const int32 LinkIndex = Level->FindLevelLinkAt(FVector2D(GetActorLocation()));
    const bool bEntered = bCheckedLevelLink && LinkIndex != INDEX_NONE && LinkIndex != LastLevelLink;
    LastLevelLink = LinkIndex;
    bCheckedLevelLink = true;
    if (!bEntered)
    {
        return;
    }
    const FAAWadNativeLevelLink& Link = Level->GetLevelLinks()[LinkIndex];
    const FString Package = Link.Destination.ToSoftObjectPath().GetLongPackageName();
    if (!FPackageName::DoesPackageExist(Package))
    {
        UE_LOG(LogTemp, Warning, TEXT("WAD level link in sector %d needs the native destination %s."), Link.Sector, *Package);
        return;
    }
    UE_LOG(LogTemp, Display, TEXT("Entered WAD level link sector %d (thing %d), loading %s."),
        Link.Sector, Link.SourceThing, *Package);
    UGameplayStatics::OpenLevelBySoftObjectPtr(this, Link.Destination, true, MakeTravelOptions());
}

FString ANativeWadPlayer::MakeTravelOptions() const
{
    FString Options = FString::Printf(TEXT("WadHealth=%d?WadMana=%d?WadFood=%d?WadWater=%d?WadXP=%d?WadReady=%d?WadCasts=%d")
        TEXT("?WadLevel=%d?WadNextXP=%d?WadMaxHealth=%d?WadMaxMana=%d?WadPoison=%d?WadAttributes=%s"),
        Health, Mana, Food, Water, Experience, ReadySlot, SuccessfulCasts, CharacterLevel, ExperienceNeeded, MaxHealth, MaxMana,
        PoisonLevel, *FString::JoinBy(GetAttributes(), TEXT(","), [](int32 Value) { return LexToString(Value); }));
    if (const ANativeWadLevel* Level = NativeLevel.Get(); Level && Level->HasDaySky())
    {
        Options += FString::Printf(TEXT("?WadHour=%.9f"), Level->GetTimeOfDayHours());
    }
    if (!CarriedItems.IsEmpty())
    {
        Options += TEXT("?WadItems=") + FString::JoinBy(CarriedItems, TEXT(","), [](int32 Type) { return LexToString(Type); });
    }
    return Options;
}

void ANativeWadPlayer::MoveInViewDirection(float Forward, float Right)
{
    const FRotator Yaw(0.0, GetControlRotation().Yaw, 0.0);
    const FRotationMatrix Basis(Yaw);
    AddMovementInput(Basis.GetUnitAxis(EAxis::X), Forward);
    AddMovementInput(Basis.GetUnitAxis(EAxis::Y), Right);
}

FVector ANativeWadPlayer::GetPawnViewLocation() const
{
    return ViewCamera->GetComponentLocation();
}

bool ANativeWadPlayer::Use()
{
    if (Health == 0 || bInventoryOpen)
    {
        return false;
    }
    const FVector Start = GetPawnViewLocation();
    const FVector Direction = GetControlRotation().Vector();
    FCollisionQueryParams Query(SCENE_QUERY_STAT(UseWadPickup), true, this);
    FHitResult Hit;
    if (GetWorld()->LineTraceSingleByChannel(Hit, Start, Start + Direction * 64.0, ECC_Visibility, Query))
    {
        if (ANativeWadPickup* Pickup = Cast<ANativeWadPickup>(Hit.GetActor()))
        {
            return Pickup->Collect(*this);
        }
    }
    for (TActorIterator<ANativeWadLevel> Level(GetWorld()); Level; ++Level)
    {
        if (Level->UseActivityFromView(*this) || Level->UseDoorFromView(Start, Direction, 112.0, this))
        {
            return true;
        }
    }
    return false;
}

void ANativeWadPlayer::UsePressed()
{
    Use();
}

void ANativeWadPlayer::AttackPressed()
{
    const double Now = GetWorld()->GetTimeSeconds();
    if (Health == 0 || Now < NextMeleeTime)
    {
        return;
    }
    const bool bDagger = GetEquippedWeapon() == IronDagger;
    const int32 Damage = Attributes[StrengthAttribute] * (bDagger ? DaggerWeaponDamage : 2) / 2;
    // STATS.C supplies the 16.16 overlay speed. The ANI files flag the hit before frame 30 or 32.
    const int32 FrameSpeed = 40000 + FMath::Min(100, Attributes[SpeedAttribute] + (bDagger ? DaggerWeaponSpeed : 0)) * 1100;
    const auto SecondsToFrame = [FrameSpeed](int32 Frame)
    {
        return FMath::DivideAndRoundUp(Frame * 65536, FrameSpeed) / TicksPerSecond;
    };
    NextMeleeTime = Now + SecondsToFrame(bDagger ? 50 : 46);
    FTimerHandle HitTimer;
    GetWorldTimerManager().SetTimer(HitTimer, FTimerDelegate::CreateWeakLambda(this, [this, Damage]
        { ResolveMeleeHit(Damage); }), SecondsToFrame(bDagger ? 30 : 32), false);
}

void ANativeWadPlayer::ResolveMeleeHit(int32 Damage)
{
    if (Health <= 0)
    {
        return;
    }
    FHitResult Hit;
    const FVector Start = GetPawnViewLocation();
    const FCollisionQueryParams Query(SCENE_QUERY_STAT(NativeWadMelee), true, this);
    if (GetWorld()->LineTraceSingleByChannel(Hit, Start,
        Start + GetControlRotation().Vector() * 78.0, ECC_Visibility, Query))
    {
        if (ANativeWadGuardian* Guardian = Cast<ANativeWadGuardian>(Hit.GetActor()))
        {
            Guardian->TakeMeleeHit(*this, Damage);
        }
    }
}

int32 ANativeWadPlayer::GetItemCount(int32 Type) const
{
    return Algo::Count(CarriedItems, Type);
}

bool ANativeWadPlayer::ShouldShowEncounterHUD() const
{
    const ANativeWadLevel* Level = NativeLevel.Get();
    return bInventoryOpen || !CarriedItems.IsEmpty() || Experience > 0 || (Level && Level->HasWadActivities());
}

FString ANativeWadPlayer::GetInteractionMessage() const
{
    return GetWorld()->GetTimeSeconds() < MessageUntil ? InteractionMessage : FString();
}

void ANativeWadPlayer::ShowInteractionMessage(FString Message)
{
    InteractionMessage = MoveTemp(Message);
    MessageUntil = GetWorld()->GetTimeSeconds() + 4.0;
}

void ANativeWadPlayer::ReceiveEncounterHit(int32 Damage)
{
    Health = FMath::Max(0, Health - Damage);
    if (Health == 0)
    {
        GetCharacterMovement()->DisableMovement();
        ShowInteractionMessage(TEXT("Defeated"));
    }
}

void ANativeWadPlayer::ReceiveTrapPoison(int32 Damage)
{
    PoisonLevel = FMath::Min(MAX_int16, PoisonLevel + Damage / 10);
    ShowInteractionMessage(TEXT("Poisoned"));
    ReceiveEncounterHit(Damage);
}

void ANativeWadPlayer::StartWadEarthquake(double Duration)
{
    QuakeTime = Duration;
    ShowInteractionMessage(TEXT("The ground shakes around you!!"));
}

void ANativeWadPlayer::CollectItem(int32 Type)
{
    CarriedItems.Add(Type);
    ShowInteractionMessage(Type == Berry ? TEXT("Picked up a berry")
        : Type == IronDagger ? TEXT("Picked up an iron dagger") : TEXT("Picked up an item"));
    UE_LOG(LogTemp, Display, TEXT("Collected WAD item type %d. Count %d."), Type, GetItemCount(Type));
}

void ANativeWadPlayer::AwardExperience(int32 Amount)
{
    Experience = int32(FMath::Min(int64(Experience) + FMath::Max(0, Amount), int64(MAX_int32)));
    while (Experience >= ExperienceNeeded && ExperienceNeeded < MAX_int32)
    {
        ++CharacterLevel;
        const int64 Level = CharacterLevel;
        ExperienceNeeded = int32(FMath::Min(int64(ExperienceNeeded) + 2000 + 10000 * Level + 3000 * Level * Level,
            int64(MAX_int32)));
        for (int32& Attribute : Attributes)
        {
            Attribute += 2;
        }
        for (int32 Point = 0; Point < AttributeCount; ++Point)
        {
            int32 Roll = FMath::RandRange(0, 12);
            for (int32 Index = 0; Index < AttributeCount; ++Index)
            {
                Roll -= PriestAdvancement[Index];
                if (Roll <= 0)
                {
                    ++Attributes[Index];
                    break;
                }
            }
        }
        for (int32& Attribute : Attributes)
        {
            Attribute = FMath::Min(100, Attribute);
        }
        MaxHealth = FMath::Min(30000, MaxHealth + (Attributes[ConstitutionAttribute] + 10) * 10);
        MaxMana = FMath::Min(30000, MaxMana + (Attributes[MagicAttribute] + 10) * 10);
        ShowInteractionMessage(TEXT("You feel more experienced!"));
        UE_LOG(LogTemp, Display, TEXT("WAD priest reached level %d. XP %d, next %d, max health %d, max mana %d."),
            CharacterLevel, Experience, ExperienceNeeded, MaxHealth, MaxMana);
    }
}

void ANativeWadPlayer::CastRejuvenate()
{
    if (Health <= 0 || bInventoryOpen)
    {
        return;
    }
    if (Runes != TEXT("45"))
    {
        ShowInteractionMessage(TEXT("Enter runes 4 and 5 for Rejuvenate"));
        Runes.Reset();
        return;
    }
    Runes.Reset();
    const int32 ManaBefore = Mana;
    const int32 HealthBefore = Health;
    const ERejuvenateResult Result = ResolveRejuvenate(Health, Mana, MaxHealth, Attributes[MagicAttribute], CharacterLevel,
        FMath::RandRange(0, 99));
    if (Result == ERejuvenateResult::InsufficientMana)
    {
        ShowInteractionMessage(TEXT("Insufficient mana for Rejuvenate"));
        return;
    }
    if (Result == ERejuvenateResult::Fizzle)
    {
        ShowInteractionMessage(TEXT("Rejuvenate fizzled"));
        UE_LOG(LogTemp, Display, TEXT("Rejuvenate fizzled. Mana %d -> %d."), ManaBefore, Mana);
        return;
    }
    ++SuccessfulCasts;
    ShowInteractionMessage(TEXT("Rejuvenate"));
    UE_LOG(LogTemp, Display, TEXT("Cast Rejuvenate. Mana %d -> %d, health %d -> %d."), ManaBefore, Mana, HealthBefore, Health);
}

void ANativeWadPlayer::TickNeeds(float DeltaSeconds)
{
    NeedsTime += DeltaSeconds;
    while (NeedsTime >= 6.0)
    {
        NeedsTime -= 6.0;
        const int32 HealthRecovery = (Attributes[ConstitutionAttribute] * (CharacterLevel + 5) >> 3) * Food / MaxFood;
        const int32 ManaRecovery = (Attributes[MagicAttribute] * (CharacterLevel + 5) * 3 >> 2) * Water / MaxFood;
        Health = FMath::Min(MaxHealth, Health + HealthRecovery);
        Mana = FMath::Min(MaxMana, Mana + ManaRecovery);
        Food = FMath::Max(0, Food - 1);
        Water = FMath::Max(0, Water - 1);
        if (PoisonLevel > 0)
        {
            ReceiveEncounterHit(PoisonLevel * 2);
            PoisonLevel = FMath::Max(0, PoisonLevel - FMath::Max(5, Attributes[ConstitutionAttribute] >> 2));
        }
    }
}

ANativeWadGameMode::ANativeWadGameMode()
{
    DefaultPawnClass = ANativeWadPlayer::StaticClass();
    HUDClass = ANativeWadHUD::StaticClass();
}
