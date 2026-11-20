#include "NativeWadCheckpoint.h"

#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
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
    if (!ActivitySlides.IsEmpty() || PendingScriptDelays != 0 || !ActiveDoors.IsEmpty()
        || TActorIterator<ANativeWadBolt>(GetWorld()))
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
        : TEXT("Could not save. Try again when the area is settled."));
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
    bCheckpointRestored = true;
    UE_LOG(LogTemp, Display, TEXT("Restored WAD checkpoint in %s."), *Save->LevelPackage);
}
