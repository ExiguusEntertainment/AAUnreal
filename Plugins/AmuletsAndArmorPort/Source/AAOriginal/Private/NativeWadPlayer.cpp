#include "NativeWadPlayer.h"

#include "Algo/AnyOf.h"
#include "Algo/Count.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadEncounter.h"
#include "NativeWadHudData.h"
#include "NativeWadLevel.h"
#include "NativeWadMovement.h"
#include "NativeWadScenery.h"
#include "NativeWadTown.h"
#include "TimerManager.h"

namespace
{
    const FKey RuneKeys[] = {EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour, EKeys::NumPadFive,
        EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine};
}

using namespace NativeWadRules;

ANativeWadPlayer::ANativeWadPlayer(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UNativeWadMovement>(CharacterMovementComponentName))
{
    GetCapsuleComponent()->InitCapsuleSize(CapsuleRadius, CapsuleHalfHeight);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ANativeWadLevel::WaterSurfaceChannel, ECR_Ignore);
    GetCapsuleComponent()->SetCollisionResponseToChannel(ANativeWadLevel::LavaSurfaceChannel, ECR_Ignore);
    BaseEyeHeight = 22.0f;
    bUseControllerRotationYaw = true;
    GetCharacterMovement()->MaxWalkSpeed = 190.0f;
    GetCharacterMovement()->MaxStepHeight = 40.0f;
    ViewCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
    ViewCamera->SetupAttachment(GetCapsuleComponent());
    ViewCamera->SetRelativeLocation(FVector(0, 0, BaseEyeHeight));
    ViewCamera->bUsePawnControlRotation = true;
}

void ANativeWadPlayer::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
    Super::CalcCamera(DeltaTime, OutResult);
    OutResult.PerspectiveNearClipPlane = 1.0f;
}

void ANativeWadPlayer::BeginPlay()
{
    Super::BeginPlay();
    ReviveTransform = GetActorTransform();
    if (auto* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>())
    {
        Travel->UpdateBackgroundMusic(*GetWorld());
    }
    if (TActorIterator<ANativeWadLevel> Level(GetWorld()); Level)
    {
        NativeLevel = *Level;
    }
    ReadTravelOptions(GetWorld()->URL);
    bRestoreCheckpointOnTick = GetWorld()->URL.HasOption(TEXT("WadLoad"));
    if (!bRestoreCheckpointOnTick && !GetWorld()->URL.HasOption(TEXT("WadPhysicalCoins")))
    {
        MoveLegacyCoinsToPurse();
    }
    if (IsInWadTown())
    {
        GetCharacterMovement()->DisableMovement();
    }
}

void ANativeWadPlayer::ReadTravelOptions(const FURL& URL)
{
    TravelHour.Reset();
    double Hour;
    if (const TCHAR* Option = URL.GetOption(TEXT("WadHour="), nullptr);
        Option && LexTryParseString(Hour, Option) && FMath::IsFinite(Hour))
    {
        TravelHour = Hour;
    }
    const auto ReadStat = [&URL](const TCHAR* Key, int32& Stat, int32 Low, int32 High)
    {
        int32 Value = 0;
        if (const TCHAR* Option = URL.GetOption(Key, nullptr); Option && LexTryParseString(Value, Option))
        {
            Stat = FMath::Clamp(Value, Low, High);
        }
    };
    CharacterClass = PriestClass;
    ReadStat(TEXT("WadClass="), CharacterClass, 0, UE_ARRAY_COUNT(CharacterClasses) - 1);
    const FWadCharacterClass& Class = CharacterClasses[CharacterClass];
    FMemory::Memcpy(Attributes, Class.Base, sizeof(Attributes));
    const int32 ClassHealth = (10 + Class.Base[ConstitutionAttribute] / 2) * 100;
    const int32 ClassMana = Class.Base[MagicAttribute] * 100;
    MaxHealth = Health = ClassHealth;
    MaxMana = Mana = ClassMana;
    ReadStat(TEXT("WadLevel="), CharacterLevel, 1, 100);
    ReadStat(TEXT("WadNextXP="), ExperienceNeeded, FirstLevelThreshold, MAX_int32);
    ReadStat(TEXT("WadMaxHealth="), MaxHealth, ClassHealth, 30000);
    ReadStat(TEXT("WadMaxMana="), MaxMana, ClassMana, 30000);
    ReadStat(TEXT("WadHealth="), Health, 1, MaxHealth);
    ReadStat(TEXT("WadMana="), Mana, 0, MaxMana);
    ReadStat(TEXT("WadFood="), Food, 0, MaxFood);
    ReadStat(TEXT("WadWater="), Water, 0, MaxFood);
    ReadStat(TEXT("WadXP="), Experience, 0, MAX_int32);
    ReadStat(TEXT("WadCasts="), SuccessfulCasts, 0, MAX_int32);
    ReadStat(TEXT("WadPoison="), PoisonLevel, 0, MAX_int16);
    SelectedAmmo = 0;
    FMemory::Memzero(AmmoReserve);
    ReadStat(TEXT("WadAmmo="), SelectedAmmo, 0, UE_ARRAY_COUNT(CrossbowAmmo) - 1);
    if (const TCHAR* Value = URL.GetOption(TEXT("WadAmmoReserve="), nullptr))
    {
        TArray<FString> Values;
        FString(Value).ParseIntoArray(Values, TEXT(","));
        for (int32 Index = 0; Index < FMath::Min(Values.Num(), int32(UE_ARRAY_COUNT(AmmoReserve))); ++Index)
        {
            AmmoReserve[Index] = FMath::Clamp(FCString::Atoi(*Values[Index]), 0, int32(MAX_uint16));
        }
    }
    const auto ReadDuration = [&URL](const TCHAR* Key, double Limit)
    {
        double Seconds = 0.0;
        if (const TCHAR* Option = URL.GetOption(Key, nullptr);
            Option && LexTryParseString(Seconds, Option) && FMath::IsFinite(Seconds))
        {
            return FMath::Clamp(Seconds, 0.0, Limit);
        }
        return 0.0;
    };
    FlightSeconds = ReadDuration(TEXT("WadFlight="), FlyingScrollSeconds);
    WaterWalkSeconds = ReadDuration(TEXT("WadWaterWalk="), 8400.0 / TicksPerSecond);
    FeatherfallSeconds = ReadDuration(TEXT("WadFeatherfall="), FeatherfallPotionSeconds);
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(ShieldSeconds); ++Index)
    {
        ShieldSeconds[Index] = ReadDuration(*FString::Printf(TEXT("WadShield%d="), Index), ShieldPotionSeconds);
    }
    for (int32 Index = 0; Index < ItemStatusCount; ++Index)
    {
        ItemStatusSeconds[Index] = ReadDuration(*FString::Printf(TEXT("WadEffect%d="), Index), 1200.0);
        ReadStat(*FString::Printf(TEXT("WadEffectPower%d="), Index), ItemStatusPower[Index], -100, 100);
    }
    if (const TCHAR* Option = URL.GetOption(TEXT("WadParalysis="), nullptr))
    {
        double Seconds = 0.0;
        if (LexTryParseString(Seconds, Option) && FMath::IsFinite(Seconds))
        {
            ParalysisSeconds = FMath::Clamp(Seconds, 0.0, 10.0);
        }
    }
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
                    Attributes[Index] = FMath::Clamp(Attribute, Class.Base[Index], 100);
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
            if (LexTryParseString(Type, *Entry) && Type > 0 && Type <= MAX_uint16)
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
    if (CarriedItems.IsValidIndex(ReadySlot) && !CanUseWadItem(CarriedItems[ReadySlot]))
    {
        ReadySlot = INDEX_NONE;
    }
    const TCHAR* Created = URL.GetOption(TEXT("WadCreated="), nullptr);
    bWadCharacterCreated = Created ? FCString::Atoi(Created) != 0
        : URL.HasOption(TEXT("WadCreated")) || !CarriedItems.IsEmpty() || Experience > 0 || CharacterLevel > 1;
    if (bWadCharacterCreated && !URL.HasOption(TEXT("WadRuneRules")))
    {
        // Earlier port saves assumed unrestricted rune access. Install only the original class's starter runes once.
        if (const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog(); Catalog && Catalog->StartingEquipment.IsValidIndex(CharacterClass))
        {
            for (const int32 Type : Catalog->StartingEquipment[CharacterClass].Items)
            {
                if (IsRuneItem(Type) && GetItemCount(Type) == 0) { CarriedItems.Add(Type); }
            }
        }
    }
    if (CarriedItems.IsValidIndex(ReadySlot) && IsRuneItem(CarriedItems[ReadySlot])) { ReadySlot = INDEX_NONE; }
    for (int32& Worn : WornSlots)
    {
        Worn = INDEX_NONE;
    }
    if (const TCHAR* Option = URL.GetOption(TEXT("WadWorn="), nullptr))
    {
        TArray<FString> Parsed;
        FString(Option).ParseIntoArray(Parsed, TEXT(","));
        for (int32 Location = 0; Location < FMath::Min(Parsed.Num(), int32(EquipmentSlotCount)); ++Location)
        {
            int32 Slot;
            if (LexTryParseString(Slot, *Parsed[Location]) && CarriedItems.IsValidIndex(Slot) && Slot != ReadySlot
                && !IsWornInventorySlot(Slot))
            {
                const FWadInventoryDetails* Details = GetItemRules(CarriedItems[Slot]);
                if (Details && CanUseWadItem(CarriedItems[Slot])
                    && (Details->ArmorSlot == Location || (Location == RightArmSlot && Details->ArmorSlot == LeftArmSlot)))
                {
                    WornSlots[Location] = Slot;
                }
            }
        }
    }
}

void ANativeWadPlayer::SetupPlayerInputComponent(UInputComponent* Input)
{
    Super::SetupPlayerInputComponent(Input);
    if (IsInWadTown())
    {
        SetMouseLook(false);
        Input->BindKey(EKeys::F5, IE_Pressed, this, &ANativeWadPlayer::WadSave);
        Input->BindKey(EKeys::F9, IE_Pressed, this, &ANativeWadPlayer::WadLoad);
        Input->BindKey(EKeys::Left, IE_Pressed, this, &ANativeWadPlayer::TownSelectPrevious);
        Input->BindKey(EKeys::Right, IE_Pressed, this, &ANativeWadPlayer::TownSelectNext);
        Input->BindKey(EKeys::Up, IE_Pressed, this, &ANativeWadPlayer::TownScrollUp);
        Input->BindKey(EKeys::Down, IE_Pressed, this, &ANativeWadPlayer::TownScrollDown);
        Input->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &ANativeWadPlayer::TownScrollUp);
        Input->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &ANativeWadPlayer::TownScrollDown);
        Input->BindKey(FInputChord(EKeys::Q, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::TownQuest);
        Input->BindKey(FInputChord(EKeys::N, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::TownCharacter);
        Input->BindKey(FInputChord(EKeys::I, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::TownInn);
        Input->BindKey(FInputChord(EKeys::C, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::TownInnCommons);
        Input->BindKey(FInputChord(EKeys::L, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::TownInnLarge);
        Input->BindKey(FInputChord(EKeys::U, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::TownInnSuite);
        Input->BindKey(FInputChord(EKeys::S, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::TownStore);
        Input->BindKey(FInputChord(EKeys::B, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::TownBank);
        Input->BindKey(EKeys::Enter, IE_Pressed, this, &ANativeWadPlayer::TownConfirm);
        Input->BindKey(EKeys::Escape, IE_Pressed, this, &ANativeWadPlayer::TownCancel);
        Input->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ANativeWadPlayer::TownClick);
        return;
    }
    Input->BindKey(EKeys::E, IE_Pressed, this, &ANativeWadPlayer::UsePressed);
    Input->BindKey(EKeys::F5, IE_Pressed, this, &ANativeWadPlayer::WadSave);
    Input->BindKey(EKeys::F9, IE_Pressed, this, &ANativeWadPlayer::WadLoad);
    Input->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &ANativeWadPlayer::ClickPressed);
    Input->BindKey(EKeys::LeftControl, IE_Pressed, this, &ANativeWadPlayer::UseReadyHand);
    Input->BindKey(FInputChord(EKeys::NumPadSeven, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::ToggleInventory);
    Input->BindKey(FInputChord(EKeys::NumPadSix, EModifierKey::Alt), IE_Pressed, this, &ANativeWadPlayer::ToggleFinances);
    Input->BindKey(EKeys::I, IE_Pressed, this, &ANativeWadPlayer::ToggleInventory);
    Input->BindKey(EKeys::G, IE_Pressed, this, &ANativeWadPlayer::DropInventoryItem);
    Input->BindKey(EKeys::PageUp, IE_Pressed, this, &ANativeWadPlayer::InventoryPageUp);
    Input->BindKey(EKeys::PageDown, IE_Pressed, this, &ANativeWadPlayer::InventoryPageDown);
    Input->BindKey(EKeys::MouseScrollUp, IE_Pressed, this, &ANativeWadPlayer::InventoryPageUp);
    Input->BindKey(EKeys::MouseScrollDown, IE_Pressed, this, &ANativeWadPlayer::InventoryPageDown);
    Input->BindKey(EKeys::Tab, IE_Pressed, this, &ANativeWadPlayer::ToggleMouseLook);
    for (const FKey& Key : RuneKeys)
    {
        Input->BindKey(Key, IE_Pressed, this, &ANativeWadPlayer::EnterRuneKey);
    }
    Input->BindKey(EKeys::BackSpace, IE_Pressed, this, &ANativeWadPlayer::RuneBackspace);
    Input->BindKey(EKeys::Enter, IE_Pressed, this, &ANativeWadPlayer::CastRejuvenate);
    Input->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &ANativeWadPlayer::CastRejuvenate);
    Input->BindAxisKey(EKeys::MouseX, this, &ANativeWadPlayer::LookYaw);
    Input->BindAxisKey(EKeys::MouseY, this, &ANativeWadPlayer::LookPitch);
    Input->BindKey(EKeys::SpaceBar, IE_Pressed, this, &ANativeWadPlayer::JumpOrRevive);
    Input->BindKey(EKeys::SpaceBar, IE_Released, this, &ANativeWadPlayer::StopJumping);
}

void ANativeWadPlayer::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bRestoreVisitedOnTick && GetController())
    {
        bRestoreVisitedOnTick = false;
        if (bRestoreCheckpointOnTick)
        {
            RestoreWadCheckpoint();
        }
        else
        {
            RestoreVisitedLevel();
        }
    }
    if (IsInWadTown())
    {
        CompleteTownArrival();
        return;
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
        ParalysisSeconds = FMath::Max(0.0, ParalysisSeconds - DeltaSeconds);
        const float Forward = float(Player->IsInputKeyDown(EKeys::W)) - float(Player->IsInputKeyDown(EKeys::S));
        const float Right = float(Player->IsInputKeyDown(EKeys::D)) - float(Player->IsInputKeyDown(EKeys::A));
        FlightSeconds = FMath::Max(0.0, FlightSeconds - DeltaSeconds);
        WaterWalkSeconds = FMath::Max(0.0, WaterWalkSeconds - DeltaSeconds);
        FeatherfallSeconds = FMath::Max(0.0, FeatherfallSeconds - DeltaSeconds);
        for (double& Seconds : ShieldSeconds)
        {
            Seconds = FMath::Max(0.0, Seconds - DeltaSeconds);
        }
        TickItemStatuses(DeltaSeconds);
        UCharacterMovementComponent* Movement = GetCharacterMovement();
        const bool bFlying = CanFly();
        if (bFlying && !Movement->IsFlying())
        {
            Movement->SetMovementMode(MOVE_Flying);
        }
        else if (!bFlying && Movement->IsFlying())
        {
            Movement->SetMovementMode(MOVE_Falling);
        }
        if (!bInventoryOpen && !IsParalyzed())
        {
            MoveInViewDirection(Forward, Right);
        }
        if (bFlying)
        {
            // The original jump input thrusts upward while the effect is active.
            const bool bAscending = !bInventoryOpen && !IsParalyzed() && Player->IsInputKeyDown(EKeys::SpaceBar);
            AddMovementInput(FVector::UpVector, bAscending ? 1.0f : -0.35f);
        }
        if (const ANativeWadLevel* Level = NativeLevel.Get())
        {
            UpdateWaterWalking(*Level);
            UpdateLavaWalking(*Level, DeltaSeconds);
            if (Health <= 0)
            {
                return;
            }
            const FVector Feet = GetActorLocation() - FVector(0, 0, CapsuleHalfHeight);
            const FVector2D CurrentInput = Level->GetCurrentInput(Feet, bOnWaterSurface);
            AddMovementInput(FVector(CurrentInput.GetSafeNormal(), 0), CurrentInput.Size());
        }
        if (ShouldShowEncounterHUD())
        {
            TickNeeds(DeltaSeconds);
        }
        if (ANativeWadLevel* Level = NativeLevel.Get())
        {
            Level->CheckActivitySector(*this, LastActivitySector);
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
    if (Link.CampaignForm == NativeWadTown::Form)
    {
        if (!OpenWadTown())
        {
            ShowInteractionMessage(TEXT("Town is unavailable."));
        }
        return;
    }
    if (Link.CampaignForm != 0)
    {
        ShowInteractionMessage(FString::Printf(TEXT("The exit leads to campaign form %d."), Link.CampaignForm));
        UE_LOG(LogTemp, Display, TEXT("Entered sector %d for WAD campaign form %d. Native form handling is pending."),
            Link.Sector, Link.CampaignForm);
        return;
    }
    const FString Package = Link.Destination.ToSoftObjectPath().GetLongPackageName();
    if (!FPackageName::DoesPackageExist(Package))
    {
        UE_LOG(LogTemp, Warning, TEXT("WAD level link in sector %d needs the native destination %s."), Link.Sector, *Package);
        return;
    }
    UE_LOG(LogTemp, Display, TEXT("Entered WAD level link sector %d (thing %d), loading %s."),
        Link.Sector, Link.SourceThing, *Package);
    TravelToWadLevel(Link.Destination);
}

void ANativeWadPlayer::TravelToWadLevel(const TSoftObjectPtr<UWorld>& Destination)
{
    if (UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
        Travel && Travel->Quest.Active != INDEX_NONE)
    {
        const int32 Map = NativeWadTown::MapNumber(Destination.ToSoftObjectPath().GetLongPackageName());
        if (Map > 0)
        {
            Travel->Quest.ResumeMaps.Add(Travel->Quest.Active, Map);
        }
    }
    ArchiveDeparture();
    UGameplayStatics::OpenLevelBySoftObjectPtr(this, Destination, true, MakeTravelOptions());
}

FString ANativeWadPlayer::MakeTravelOptions() const
{
    FString Options = FString::Printf(TEXT("WadHealth=%d?WadMana=%d?WadFood=%d?WadWater=%d?WadXP=%d?WadReady=%d?WadCasts=%d")
        TEXT("?WadLevel=%d?WadNextXP=%d?WadMaxHealth=%d?WadMaxMana=%d?WadPoison=%d?WadParalysis=%.6f?WadAttributes=%s"),
        Health, Mana, Food, Water, Experience, ReadySlot, SuccessfulCasts, CharacterLevel, ExperienceNeeded, MaxHealth, MaxMana,
        PoisonLevel, ParalysisSeconds, *FString::JoinBy(GetAttributes(), TEXT(","), [](int32 Value) { return LexToString(Value); }));
    const auto WriteDuration = [&Options](const TCHAR* Key, double Seconds)
    {
        if (Seconds > 0.0)
        {
            Options += FString::Printf(TEXT("?%s=%.6f"), Key, Seconds);
        }
    };
    Options += FString::Printf(TEXT("?WadClass=%d"), CharacterClass);
    WriteDuration(TEXT("WadFlight"), FlightSeconds);
    if (SelectedAmmo != 0)
    {
        Options += FString::Printf(TEXT("?WadAmmo=%d"), SelectedAmmo);
    }
    if (Algo::AnyOf(AmmoReserve, [](int32 Quantity) { return Quantity > 0; }))
    {
        Options += TEXT("?WadAmmoReserve=") + FString::JoinBy(MakeArrayView(AmmoReserve), TEXT(","),
            [](int32 Quantity) { return LexToString(Quantity); });
    }
    WriteDuration(TEXT("WadWaterWalk"), WaterWalkSeconds);
    WriteDuration(TEXT("WadFeatherfall"), FeatherfallSeconds);
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(ShieldSeconds); ++Index)
    {
        WriteDuration(*FString::Printf(TEXT("WadShield%d"), Index), ShieldSeconds[Index]);
    }
    for (int32 Index = 0; Index < ItemStatusCount; ++Index)
    {
        if (ItemStatusSeconds[Index] > 0.0)
        {
            WriteDuration(*FString::Printf(TEXT("WadEffect%d"), Index), ItemStatusSeconds[Index]);
            Options += FString::Printf(TEXT("?WadEffectPower%d=%d"), Index, ItemStatusPower[Index]);
        }
    }
    if (const ANativeWadLevel* Level = NativeLevel.Get(); Level && Level->HasDaySky())
    {
        Options += FString::Printf(TEXT("?WadHour=%.9f"), Level->GetTimeOfDayHours());
    }
    else if (TravelHour.IsSet())
    {
        Options += FString::Printf(TEXT("?WadHour=%.9f"), TravelHour.GetValue());
    }
    if (!CarriedItems.IsEmpty())
    {
        Options += TEXT("?WadItems=") + FString::JoinBy(CarriedItems, TEXT(","), [](int32 Type) { return LexToString(Type); });
        Options += TEXT("?WadWorn=") + FString::JoinBy(MakeArrayView(WornSlots), TEXT(","), [](int32 Slot) { return LexToString(Slot); });
        if (CarriedItems.ContainsByPredicate([](int32 Type) { return CoinValueCopper(Type) > 0; }))
        {
            Options += TEXT("?WadPhysicalCoins");
        }
    }
    Options += FString::Printf(TEXT("?WadCreated=%d?WadRuneRules=1"), bWadCharacterCreated);
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
    const FVector Reach = Start + Direction * 64.0;
    const bool bObstructed = GetWorld()->LineTraceSingleByChannel(Hit, Start, Reach, ECC_Visibility, Query);
    FHitResult ItemHit;
    if (GetWorld()->LineTraceSingleByChannel(ItemHit, Start, bObstructed ? Hit.ImpactPoint : Reach,
        ANativeWadPickup::InteractionChannel, Query))
    {
        if (ANativeWadPickup* Pickup = Cast<ANativeWadPickup>(ItemHit.GetActor()))
        {
            return Pickup->Collect(*this);
        }
    }
    if (bObstructed)
    {
        if (ANativeWadPickup* Pickup = Cast<ANativeWadPickup>(Hit.GetActor()))
        {
            return Pickup->Collect(*this);
        }
        if (const ANativeWadScenery* Scenery = Cast<ANativeWadScenery>(Hit.GetActor()))
        {
            if (ANativeWadPickup* Pickup = Scenery->GetPickup())
            {
                return Pickup->Collect(*this);
            }
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
    const int32 Weapon = GetEquippedWeapon();
    const FWadInventoryDetails* Details = GetItemRules(Weapon);
    const bool bAuthored = Details && Details->HitFrame > 0 && Details->SwingFrames >= Details->HitFrame;
    const bool bDagger = Weapon == IronDagger;
    const int32 BaseDamage = bAuthored ? Details->WeaponDamage : bDagger ? DaggerWeaponDamage : 2;
    const int32 BaseSpeed = bAuthored ? Details->WeaponSpeed : bDagger ? DaggerWeaponSpeed : 0;
    const int32 HitFrame = bAuthored ? Details->HitFrame : bDagger ? 30 : 32;
    const int32 SwingFrames = bAuthored ? Details->SwingFrames : bDagger ? 50 : 46;
    const int32 Damage = FMath::Max(0, Attributes[StrengthAttribute] + GetStatusPower(StrengthStatus)) * BaseDamage / 2;
    // STATS.C supplies the 16.16 overlay speed, and the weapon's ANI supplies the timing.
    const int32 FrameSpeed = 40000 + FMath::Clamp(Attributes[SpeedAttribute] + BaseSpeed, 0, 100) * 1100;
    const auto SecondsToFrame = [FrameSpeed](int32 Frame)
    {
        return FMath::DivideAndRoundUp(Frame * 65536, FrameSpeed) / TicksPerSecond;
    };
    NextMeleeTime = Now + SecondsToFrame(SwingFrames);
    FTimerHandle HitTimer;
    GetWorldTimerManager().SetTimer(HitTimer, FTimerDelegate::CreateWeakLambda(this, [this, Damage]
        { ResolveMeleeHit(Damage); }), SecondsToFrame(HitFrame), false);
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
    if (Health <= 0 || Damage <= 0)
    {
        return;
    }
    Health = FMath::Max(0, Health - Damage);
    if (Health == 0)
    {
        UE_LOG(LogTemp, Display, TEXT("WAD player defeated at %s by %d damage; poison %d, flight %.2f seconds."),
            *GetActorLocation().ToCompactString(), Damage, PoisonLevel, FlightSeconds);
        GetCharacterMovement()->DisableMovement();
        DropDefeatedItems();
        Runes.Reset();
        if (bInventoryOpen)
        {
            ToggleInventory();
        }
        ShowInteractionMessage(TEXT("Defeated. Press Space to revive."));
    }
}

void ANativeWadPlayer::JumpOrRevive()
{
    if (Health > 0)
    {
        Jump();
        return;
    }
    if (!NativeLevel.IsValid() || !GetController())
    {
        return;
    }
    SetActorTransform(ReviveTransform, false, nullptr, ETeleportType::TeleportPhysics);
    GetController()->SetControlRotation(ReviveTransform.Rotator());
    GetCharacterMovement()->StopMovementImmediately();
    GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    Health = MaxHealth;
    Mana = MaxMana / 2;
    PoisonLevel = 0;
    ParalysisSeconds = 0.0;
    LastActivitySector = INDEX_NONE;
    LastLevelLink = INDEX_NONE;
    ShowInteractionMessage(TEXT("Revived at the level entrance."));
}

void ANativeWadPlayer::ReceivePhysicalHit(int32 Damage)
{
    ReceiveTypedHit(Damage, NormalDamage);
}

void ANativeWadPlayer::ReceiveTypedHit(int32 Damage, uint8 DamageMask)
{
    if (Health <= 0 || Damage <= 0) { return; }
    if (DamageMask & SpecialDamage)
    {
        if (DamageMask == SpecialDamageFlag(ESpecialDamage::Confuse) && GetController())
        {
            // EFFECT_REORIENT turns left by rand() in the original 16-bit angle space.
            FRotator Rotation = GetController()->GetControlRotation();
            Rotation.Yaw -= FMath::RandRange(0, 32767) * (360.0 / 65536.0);
            GetController()->SetControlRotation(Rotation);
            ShowInteractionMessage(TEXT("You feel dizzy"));
        }
        return;
    }
    // The source rolls one of the six armor locations for electricity, including the shield hand.
    const int32 ArmorClass = DamageMask & DamageFlag(EProjectileDamage::Electricity)
        ? GetArmorClass(FMath::RandRange(0, EquipmentSlotCount - 1)) : 0;
    const FWadPlayerDamage Hit = ResolvePlayerDamage(Damage, DamageMask, GetResistanceMask(), ArmorClass, GetArmorRating());
    if (Hit.Poison > 0)
    {
        PoisonLevel = FMath::Min(MAX_int16, PoisonLevel + Hit.Poison);
        ShowInteractionMessage(TEXT("Poisoned"));
    }
    if (Hit.Mana > 0) { RestoreWadVital(10, -Hit.Mana); }
    ReceiveEncounterHit(Hit.Health);
}

void ANativeWadPlayer::ReceiveTrapPoison(int32 Damage)
{
    ReceiveTypedHit(Damage, DamageFlag(EProjectileDamage::Poison));
}

void ANativeWadPlayer::ReceiveTrapParalysis()
{
    ParalysisSeconds = 10.0;
    GetCharacterMovement()->StopMovementImmediately();
    ConsumeMovementInputVector();
    ShowInteractionMessage(TEXT("Paralyzed"));
}

void ANativeWadPlayer::Jump()
{
    if (!IsParalyzed() && !CanFly())
    {
        Super::Jump();
    }
}

void ANativeWadPlayer::StartWadEarthquake(double Duration)
{
    QuakeTime = Duration;
    ShowInteractionMessage(TEXT("The ground shakes around you!!"));
}

void ANativeWadPlayer::UpdateWaterWalking(const ANativeWadLevel& Level)
{
    const bool bRequested = GetReadyItem() == WaterWalkRing || WaterWalkSeconds > 0.0;
    if (bRequested == bOnWaterSurface)
    {
        return;
    }
    const FVector Position = GetActorLocation();
    const TOptional<double> Surface = Level.FindWaterSurfaceZ(Position);
    if (bRequested)
    {
        const double StandingZ = Surface.Get(0.0) + GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
        if (Surface.IsSet() && Position.Z < StandingZ)
        {
            // Rise from the submerged floor before enabling the water surface for this pawn.
            SetActorLocation(FVector(Position.X, Position.Y, StandingZ + 1.0), true);
            if (GetActorLocation().Z < StandingZ)
            {
                return;
            }
        }
    }
    bOnWaterSurface = bRequested;
    GetCapsuleComponent()->SetCollisionResponseToChannel(ANativeWadLevel::WaterSurfaceChannel,
        bOnWaterSurface ? ECR_Block : ECR_Ignore);
    if (Surface.IsSet() && GetCharacterMovement()->IsMovingOnGround())
    {
        GetCharacterMovement()->SetMovementMode(MOVE_Falling);
    }
}

void ANativeWadPlayer::TickItemStatuses(float DeltaSeconds)
{
    for (double& Seconds : ItemStatusSeconds)
    {
        Seconds = FMath::Max(0.0, Seconds - DeltaSeconds);
    }
    const int32 Concealment = GetStatusPower(InvisibleStatus) ? 190 : GetStatusPower(TranslucentStatus) ? 100 : 0;
    if (Concealment == 0)
    {
        bConcealed = false;
        NextConcealmentRoll = 0.0;
    }
    else if (const double Now = GetWorld()->GetTimeSeconds(); Now >= NextConcealmentRoll)
    {
        bConcealed = FMath::RandRange(0, 199) < FMath::Max(Concealment, Attributes[StealthAttribute] / 2);
        NextConcealmentRoll = Now + (GetVelocity().IsNearlyZero() ? 3.0 : 1.0);
    }
}

void ANativeWadPlayer::UpdateLavaWalking(const ANativeWadLevel& Level, float DeltaSeconds)
{
    const bool bRequested = GetStatusPower(LavaWalkStatus) != 0;
    const FVector Position = GetActorLocation();
    const TOptional<double> Surface = Level.FindLavaSurfaceZ(Position);
    const double HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
    if (bRequested != bOnLavaSurface)
    {
        const double StandingZ = Surface.Get(0.0) + HalfHeight;
        if (bRequested && Surface.IsSet() && Position.Z < StandingZ)
        {
            SetActorLocation(FVector(Position.X, Position.Y, StandingZ + 1.0), true);
            if (GetActorLocation().Z < StandingZ)
            {
                return;
            }
        }
        bOnLavaSurface = bRequested;
        GetCapsuleComponent()->SetCollisionResponseToChannel(ANativeWadLevel::LavaSurfaceChannel,
            bOnLavaSurface ? ECR_Block : ECR_Ignore);
        if (!bOnLavaSurface && Surface.IsSet() && GetCharacterMovement()->IsMovingOnGround())
        {
            GetCharacterMovement()->SetMovementMode(MOVE_Falling);
        }
    }
    if (!bOnLavaSurface && Surface.IsSet() && Position.Z - HalfHeight <= Surface.GetValue() + 1.0)
    {
        LavaDamageTime += DeltaSeconds;
        if (LavaDamageTime >= 0.5)
        {
            LavaDamageTime -= 0.5;
            ReceiveEncounterHit(200);
        }
    }
    else
    {
        LavaDamageTime = 0.0;
    }
}

bool ANativeWadPlayer::AddCoinsToPurse(int32 Type)
{
    const int32 Copper = CoinValueCopper(Type);
    UNativeWadTravelState* Travel = GetGameInstance() ? GetGameInstance()->GetSubsystem<UNativeWadTravelState>() : nullptr;
    if (Copper == 0 || !Travel || Travel->Quest.GetWalletCopper() > FWadQuestProgress::WalletLimit - Copper)
    {
        return false;
    }
    Travel->Quest.SetWalletCopper(Travel->Quest.GetWalletCopper() + Copper);
    UE_LOG(LogTemp, Display, TEXT("Collected WAD coin %d for %d copper. Purse %lld."), Type, Copper, Travel->Quest.GetWalletCopper());
    return true;
}

void ANativeWadPlayer::MoveLegacyCoinsToPurse()
{
    for (int32 Slot = CarriedItems.Num() - 1; Slot >= 0; --Slot)
    {
        if (CoinValueCopper(CarriedItems[Slot]) > 0 && AddCoinsToPurse(CarriedItems[Slot]))
        {
            RemoveInventorySlot(Slot);
        }
    }
}

bool ANativeWadPlayer::CollectItem(int32 Type)
{
    const bool bCoin = CoinValueCopper(Type) > 0;
    if (bCoin && !AddCoinsToPurse(Type))
    {
        ShowInteractionMessage(TEXT("Your purse cannot hold any more coins."));
        return false;
    }
    if (!bCoin)
    {
        CarriedItems.Add(Type);
        UE_LOG(LogTemp, Display, TEXT("Collected WAD item type %d. Count %d."), Type, GetItemCount(Type));
    }
    const FString Description = GetInventoryItemDescription(Type);
    FString Name;
    ShowInteractionMessage(FString::Printf(TEXT("Picked up %s"),
        Description.Split(TEXT(" ["), &Name, nullptr) ? *Name : *Description));
    return true;
}

void ANativeWadPlayer::RestoreWadVital(int32 Effect, int32 Amount)
{
    switch (Effect)
    {
    case 7: Food += FMath::Clamp(Amount, -Food, MaxFood - Food); break;
    case 8: Water += FMath::Clamp(Amount, 0, MaxFood - Water); break;
    case 9: Health += FMath::Clamp(Amount, 0, MaxHealth - Health); break;
    case 10:
    {
        const int32 Before = Mana;
        Mana += FMath::Clamp(Amount, -Mana, MaxMana - Mana);
        if (Amount < 0)
        {
            UE_LOG(LogTemp, Display, TEXT("WAD activity changed mana from %d to %d (authored drain %lld)."), Before, Mana, -int64(Amount));
        }
        break;
    }
    case 11: AwardExperience(Amount); break;
    default: break;
    }
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
                Roll -= CharacterClasses[CharacterClass].Advancement[Index];
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
        UE_LOG(LogTemp, Display, TEXT("WAD %s reached level %d. XP %d, next %d, max health %d, max mana %d."),
            CharacterClasses[CharacterClass].Name, CharacterLevel, Experience, ExperienceNeeded, MaxHealth, MaxMana);
    }
}

void ANativeWadPlayer::EnterRuneKey(FKey Key)
{
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(RuneKeys); ++Index)
    {
        if (Key == RuneKeys[Index])
        {
            EnterRune(Index + 1);
            return;
        }
    }
}

void ANativeWadPlayer::CastRejuvenate()
{
    if (Health <= 0 || bInventoryOpen)
    {
        return;
    }
    for (const TCHAR Rune : Runes)
    {
        if (!HasRune(Rune - TEXT('0')))
        {
            Runes.Reset();
            ShowInteractionMessage(TEXT("You do not have that rune."));
            return;
        }
    }
    const ESpellSchool School = SchoolForClass(CharacterClass);
    if (Runes != TEXT("45") || School != ESpellSchool::Cleric)
    {
        for (const FWadSpell& Spell : SupportedSpells)
        {
            if (Spell.School != School || Runes != Spell.Runes)
            {
                continue;
            }
            Runes.Reset();
            if (Mana <= Spell.Mana)
            {
                ShowInteractionMessage(FString::Printf(TEXT("Insufficient mana for %s."), Spell.Name));
                return;
            }
            if (FMath::RandRange(0, 99) > Attributes[MagicAttribute] + Spell.Hardness + 2 * CharacterLevel)
            {
                Mana -= Spell.Mana / 2;
                ShowInteractionMessage(FString::Printf(TEXT("%s fizzled."), Spell.Name));
                return;
            }
            if (Spell.Effect == ESpellEffect::Projectile)
            {
                const FWadWandProjectile* Projectile = FindWandProjectile(Spell.ProjectileIndex);
                if (!Projectile || !LaunchWadProjectile(*Projectile)) { return; }
            }
            Mana -= Spell.Mana;
            const double Duration = Spell.DurationTicks / double(TicksPerSecond);
            switch (Spell.Effect)
            {
            case ESpellEffect::Projectile: break;
            case ESpellEffect::WaterWalk: WaterWalkSeconds = FMath::Max(WaterWalkSeconds, Duration); break;
            case ESpellEffect::FeatherFall: FeatherfallSeconds = FMath::Max(FeatherfallSeconds, Duration); break;
            case ESpellEffect::Shield:
                ItemStatusSeconds[SpellShieldStatus] = Duration;
                ItemStatusPower[SpellShieldStatus] = Spell.Power;
                break;
            }
            ++SuccessfulCasts;
            ShowInteractionMessage(Spell.Name);
            UE_LOG(LogTemp, Display, TEXT("Cast %s for %d mana."), Spell.Name, Spell.Mana);
            return;
        }
        ShowInteractionMessage(TEXT("No known spell matches those runes."));
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
        const int32 ManaBase = Attributes[MagicAttribute] * (CharacterLevel + 5) * CharacterClasses[CharacterClass].ManaRecoveryPercent / 100;
        const int32 ManaRecovery = (ManaBase >> 2) * Water / MaxFood;
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
