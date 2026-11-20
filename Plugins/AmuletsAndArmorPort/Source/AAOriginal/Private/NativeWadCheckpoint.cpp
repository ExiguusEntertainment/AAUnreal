#include "NativeWadCheckpoint.h"

#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "TimerManager.h"
#include "UObject/Package.h"

namespace
{
    FString CheckpointMap(const UWorld& World)
    {
        return FPackageName::GetLongPackagePath(World.GetOutermost()->GetName()) / World.RemovePIEPrefix(World.GetMapName());
    }

    UNativeWadCheckpoint* ReadCheckpoint(const FString& Slot)
    {
        auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(
            Slot.IsEmpty() ? UNativeWadCheckpoint::Slot : Slot, 0));
        return Save && Save->Version == UNativeWadCheckpoint::CurrentVersion ? Save : nullptr;
    }
}

bool ANativeWadLevel::WriteCheckpoint(FWadLevelCheckpoint& Checkpoint) const
{
    if (TActorIterator<ANativeWadBolt>(GetWorld()))
    {
        return false;
    }
    for (int32 Sector = 0; Sector < Sectors.Num(); ++Sector)
    {
        FVector2D Values;
        if (GetSectorSurfaceHeights(Sector, Values))
        {
            Checkpoint.SectorHeights.Add(Sector, Values);
        }
    }
    Checkpoint.Variables.Append(ScriptVariables, UE_ARRAY_COUNT(ScriptVariables));
    for (bool Flag : ScriptFlags)
    {
        Checkpoint.Flags.Add(Flag);
    }
    Checkpoint.Switches = ActivitySwitchStates;
    Checkpoint.LightLevels = LightOverrides;
    Checkpoint.Hour = GetTimeOfDayHours();
    for (const FActivitySlide& Slide : ActivitySlides)
    {
        Checkpoint.Slides.Add({Slide.Sector, Slide.From, Slide.To, Slide.Duration, Slide.Elapsed,
            Slide.bCeiling, Slide.bPauseOnCrush});
    }
    for (const auto& [DoorIndex, State] : ActiveDoors)
    {
        Checkpoint.Doors.Add(Doors[DoorIndex].Sector, {uint8(State.Phase), State.StartZ, State.Elapsed});
    }
    TArray<uint64> DelayIds;
    PendingScriptDelays.GetKeys(DelayIds);
    DelayIds.Sort();
    for (uint64 Id : DelayIds)
    {
        const FPendingScriptDelay& Waiting = PendingScriptDelays.FindChecked(Id);
        const float Remaining = GetWorldTimerManager().GetTimerRemaining(Waiting.Handle);
        Checkpoint.Delays.Add({Waiting.Activity, Waiting.NextCommand, FMath::Max(0.001f, Remaining), Waiting.Player.IsValid()});
    }
    for (TActorIterator<ANativeWadPickup> Item(GetWorld()); Item; ++Item)
    {
        if (!Item->Tags.IsEmpty())
        {
            Checkpoint.RemainingPickups.Add(Item->Tags[0]);
        }
    }
    for (TActorIterator<ANativeWadGuardian> Encounter(GetWorld()); Encounter; ++Encounter)
    {
        if (!Encounter->Tags.IsEmpty())
        {
            Checkpoint.Encounters.Add(Encounter->Tags[0], {Encounter->GetActorTransform(), Encounter->GetEncounterHealth()});
        }
    }
    return true;
}

bool ANativeWadLevel::RestoreCheckpoint(const FWadLevelCheckpoint& Checkpoint)
{
    if (Checkpoint.Variables.Num() != UE_ARRAY_COUNT(ScriptVariables) || Checkpoint.Flags.Num() != UE_ARRAY_COUNT(ScriptFlags))
    {
        return false;
    }
    InitializeTopology();
    for (const FWadSavedSlide& Slide : Checkpoint.Slides)
    {
        FVector2D HeightsAtSave;
        if (!GetSectorSurfaceHeights(Slide.Sector, HeightsAtSave) || !FMath::IsFinite(Slide.From) || !FMath::IsFinite(Slide.To)
            || !FMath::IsFinite(Slide.Duration) || !FMath::IsFinite(Slide.Elapsed)
            || Slide.Duration <= 0.0 || Slide.Elapsed < 0.0 || Slide.Elapsed > Slide.Duration)
        {
            return false;
        }
    }
    for (const auto& [Sector, State] : Checkpoint.Doors)
    {
        if (!DoorBySector.Contains(Sector) || State.Phase > uint8(EDoorPhase::Closing)
            || !FMath::IsFinite(State.StartZ) || !FMath::IsFinite(State.Elapsed) || State.Elapsed < 0.0)
        {
            return false;
        }
        const FAAWadNativeDoor& Door = Doors[DoorBySector.FindChecked(Sector)];
        const float Duration = EDoorPhase(State.Phase) == EDoorPhase::Holding ? Door.HoldSeconds : Door.TravelSeconds;
        if (State.Elapsed > Duration)
        {
            return false;
        }
    }
    for (const FWadSavedScriptDelay& Waiting : Checkpoint.Delays)
    {
        const FWadScriptActivity* Activity = FindWadActivity(Waiting.Activity);
        if (!Activity || Waiting.NextCommand < 0 || Waiting.NextCommand > Activity->Commands.Num()
            || !FMath::IsFinite(Waiting.SecondsRemaining) || Waiting.SecondsRemaining <= 0.0f)
        {
            return false;
        }
    }
    for (const auto& [Sector, Values] : Checkpoint.SectorHeights)
    {
        FVector2D Before;
        if (!GetSectorSurfaceHeights(Sector, Before) || Values.ContainsNaN() || Values.X > Values.Y)
        {
            return false;
        }
    }
    for (const auto& [Sector, Values] : Checkpoint.SectorHeights)
    {
        FVector2D Before;
        GetSectorSurfaceHeights(Sector, Before);
        if (Values.Y > Before.Y)
        {
            SetSectorHeight(Sector, Values.Y, true);
        }
        SetSectorHeight(Sector, Values.X, false);
        SetSectorHeight(Sector, Values.Y, true);
    }
    ActivitySlides.Reset();
    for (const FWadSavedSlide& Slide : Checkpoint.Slides)
    {
        ActivitySlides.Add({Slide.Sector, Slide.From, Slide.To, Slide.Duration, Slide.Elapsed, Slide.bCeiling, Slide.bPauseOnCrush});
    }
    ActiveDoors.Reset();
    for (const auto& [Sector, State] : Checkpoint.Doors)
    {
        ActiveDoors.Add(DoorBySector.FindChecked(Sector), {EDoorPhase(State.Phase), State.StartZ, State.Elapsed});
    }
    // A freshly opened level also runs activity zero. Replace those initial motions and waits with the checkpoint's state.
    for (auto& [Id, Waiting] : PendingScriptDelays)
    {
        GetWorldTimerManager().ClearTimer(Waiting.Handle);
    }
    PendingScriptDelays.Reset();
    ANativeWadPlayer* Player = Cast<ANativeWadPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
    for (const FWadSavedScriptDelay& Waiting : Checkpoint.Delays)
    {
        ScheduleScriptContinuation(Waiting.Activity, Waiting.NextCommand, Waiting.bHasPlayer ? Player : nullptr,
            Waiting.SecondsRemaining);
    }
    if (!ActivitySlides.IsEmpty() || !ActiveDoors.IsEmpty())
    {
        SetActorTickEnabled(true);
    }
    for (const auto& [Side, bUp] : Checkpoint.Switches)
    {
        SetActivitySwitch(Side, bUp);
    }
    FMemory::Memcpy(ScriptVariables, Checkpoint.Variables.GetData(), sizeof(ScriptVariables));
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(ScriptFlags); ++Index)
    {
        ScriptFlags[Index] = Checkpoint.Flags[Index] != 0;
    }
    if (HasDaySky())
    {
        SetTimeOfDayHours(Checkpoint.Hour);
    }
    LightOverrides = Checkpoint.LightLevels;
    RefreshSectorLights();
    for (TActorIterator<ANativeWadPickup> Item(GetWorld()); Item; ++Item)
    {
        if (!Item->Tags.IsEmpty() && !Checkpoint.RemainingPickups.Contains(Item->Tags[0]))
        {
            Item->Destroy();
        }
    }
    for (TActorIterator<ANativeWadGuardian> Encounter(GetWorld()); Encounter; ++Encounter)
    {
        if (!Encounter->Tags.IsEmpty())
        {
            if (const FWadSavedEncounter* State = Checkpoint.Encounters.Find(Encounter->Tags[0]))
            {
                Encounter->SetActorTransform(State->Transform, false, nullptr, ETeleportType::TeleportPhysics);
                Encounter->RestoreEncounterHealth(State->Health);
            }
        }
    }
    return true;
}

bool ANativeWadPlayer::SaveWadCheckpoint(const FString& Slot) const
{
    UNativeWadCheckpoint* Save = NewObject<UNativeWadCheckpoint>();
    const ANativeWadLevel* Level = NativeLevel.Get();
    if (Health <= 0 || QuakeTime > 0.0 || !GetCharacterMovement()->IsMovingOnGround()
        || !Level || !Level->WriteCheckpoint(Save->Level))
    {
        return false;
    }
    Save->LevelPackage = CheckpointMap(*GetWorld());
    Save->Position = GetActorLocation();
    Save->View = GetControlRotation();
    Save->NeedsTime = NeedsTime;
    Save->PlayerOptions = MakeTravelOptions();
    if (const UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>())
    {
        Save->VisitedLevels = Travel->VisitedLevels;
    }
    if (!UGameplayStatics::SaveGameToSlot(Save, Slot.IsEmpty() ? UNativeWadCheckpoint::Slot : Slot, 0))
    {
        return false;
    }
    UE_LOG(LogTemp, Display, TEXT("Saved WAD checkpoint in %s."), *Save->LevelPackage);
    return true;
}

void ANativeWadPlayer::WadSave()
{
    ShowInteractionMessage(SaveWadCheckpoint() ? TEXT("Checkpoint saved")
        : TEXT("Could not save. Try again on solid ground when projectiles and screen effects have cleared."));
}

void ANativeWadPlayer::WadLoad()
{
    const UNativeWadCheckpoint* Save = ReadCheckpoint(FString());
    if (Save && FPackageName::DoesPackageExist(Save->LevelPackage))
    {
        UGameplayStatics::OpenLevel(this, FName(Save->LevelPackage), true, TEXT("WadLoad"));
        return;
    }
    ShowInteractionMessage(TEXT("No compatible checkpoint found"));
}

void ANativeWadPlayer::RestoreWadCheckpoint()
{
    const UNativeWadCheckpoint* Save = ReadCheckpoint(GetWorld()->URL.GetOption(TEXT("WadLoad="), TEXT("")));
    ANativeWadLevel* Level = NativeLevel.Get();
    if (!Save || Save->LevelPackage != CheckpointMap(*GetWorld()) || !Level || Save->Position.ContainsNaN()
        || Save->View.ContainsNaN() || !Level->RestoreCheckpoint(Save->Level))
    {
        UE_LOG(LogTemp, Error, TEXT("No compatible WAD checkpoint for this map."));
        return;
    }
    const FURL SavedURL(nullptr, *(Save->LevelPackage + TEXT("?") + Save->PlayerOptions), TRAVEL_Absolute);
    CarriedItems.Reset();
    ReadTravelOptions(SavedURL);
    NeedsTime = FMath::Clamp(Save->NeedsTime, 0.0, 6.0);
    SetActorLocation(Save->Position, false, nullptr, ETeleportType::TeleportPhysics);
    GetCharacterMovement()->StopMovementImmediately();
    GetController()->SetControlRotation(Save->View);

    // Arrival establishes the current sector without replaying a script that was already executed.
    LastActivitySector = Level->FindActivitySector(FVector2D(Save->Position));
    LastLevelLink = Level->FindLevelLinkAt(FVector2D(Save->Position));
    bCheckedLevelLink = true;
    if (UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>())
    {
        Travel->VisitedLevels = Save->VisitedLevels;
    }
    bCheckpointRestored = true;
    UE_LOG(LogTemp, Display, TEXT("Restored WAD checkpoint in %s."), *Save->LevelPackage);
}

void ANativeWadPlayer::ArchiveDeparture()
{
    const ANativeWadLevel* Level = NativeLevel.Get();
    UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    FWadLevelCheckpoint Snapshot;
    if (Level && Travel && Level->WriteCheckpoint(Snapshot))
    {
        const FString Package = CheckpointMap(*GetWorld());
        Travel->VisitedLevels.Add(Package, MoveTemp(Snapshot));
        UE_LOG(LogTemp, Display, TEXT("Archived WAD level %s on departure."), *Package);
    }
}

void ANativeWadPlayer::RestoreVisitedLevel()
{
    const UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    ANativeWadLevel* Level = NativeLevel.Get();
    if (!Travel || !Level)
    {
        return;
    }
    const FString Package = CheckpointMap(*GetWorld());
    if (const FWadLevelCheckpoint* Snapshot = Travel->VisitedLevels.Find(Package))
    {
        // The campaign clock travels with the player and keeps advancing across map visits.
        const double ArrivalHour = Level->GetTimeOfDayHours();
        if (Level->RestoreCheckpoint(*Snapshot))
        {
            Level->SetTimeOfDayHours(ArrivalHour);
            UE_LOG(LogTemp, Display, TEXT("Restored visited WAD level %s."), *Package);
        }
    }
}
