#include "NativeWadCheckpoint.h"

#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"
#include "NativeWadEncounter.h"
#include "NativeWadHudData.h"
#include "NativeWadBagState.h"
#include "NativeWadCarriedObjectState.h"
#include "NativeWadLevel.h"
#include "NativeWadOptions.h"
#include "NativeWadPlayer.h"
#include "NativeWadPickupMetadata.h"
#include "NativeWadFlightOwners.h"
#include "NativeWadScenery.h"
#include "NativeWadTown.h"
#include "TimerManager.h"
#include "Sound/SoundWave.h"
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
        if (!Save || Save->Version < 1 || Save->Version > UNativeWadCheckpoint::CurrentVersion) { return nullptr; }
        const FURL URL(nullptr, *(Save->LevelPackage + TEXT("?") + Save->PlayerOptions), TRAVEL_Absolute);
        if (const TCHAR* Defeated = URL.GetOption(TEXT("WadDefeated="), nullptr);
            Defeated && FCString::Strcmp(Defeated, TEXT("0")) != 0 && FCString::Strcmp(Defeated, TEXT("1")) != 0) { return nullptr; }
        if (const TCHAR* Payload = URL.GetOption(TEXT("WadFlightOwners="), nullptr))
        {
            TArray<int32> Owners;
            if (!NativeWadFlightOwners::Decode(Payload, Owners)) { return nullptr; }
        }
        if (const TCHAR* Payload = URL.GetOption(TEXT("WadOwnedEffects="), nullptr))
        {
            FWadPlayerEffects Effects;
            if (!Effects.Decode(Payload)) { return nullptr; }
        }
        // Version4 writers always record both aligned payloads. A stripped
        // modern field cannot masquerade as legacy migration or grant overflow.
        if(Save->Version>=4&&(!URL.GetOption(TEXT("WadObjectData="),nullptr)
            ||!URL.GetOption(TEXT("WadBagLayout="),nullptr))){return nullptr;}
        if(!NativeWadCarriedObjectState::ValidateOptions(URL)){return nullptr;}
        if(!NativeWadBagState::ValidateOptions(URL,NativeWadBagState::CatalogRule)){return nullptr;}
        return Save;
    }
}

void UNativeWadTravelState::UpdateBackgroundMusic(UWorld& World)
{
    const UNativeWadHudData* Catalog = UNativeWadHudData::LoadCatalog();
    const FName Map(*UGameplayStatics::GetCurrentLevelName(&World, true));
    const FSoftObjectPath Path = Catalog ? Catalog->MusicByMap.FindRef(Map).ToSoftObjectPath() : FSoftObjectPath();
    if (Path == RequestedMusic)
    {
        return;
    }
    RequestedMusic = Path;
    if (BackgroundMusic)
    {
        BackgroundMusic->FadeOut(0.2f, 0.0f);
        BackgroundMusic = nullptr;
    }
    if (Path.IsValid())
    {
        TWeakObjectPtr<UWorld> PlaybackWorld(&World);
        auto Start = FStreamableDelegate::CreateWeakLambda(this, [this, PlaybackWorld, Path, Map]
        {
            USoundWave* Track = Cast<USoundWave>(Path.ResolveObject());
            if (!Track || RequestedMusic != Path)
            {
                return;
            }
            UWorld* Current = GetWorld();
            if (!Current || !Current->IsGameWorld())
            {
                Current = PlaybackWorld.Get();
            }
            if (Current)
            {
                BackgroundMusic = UGameplayStatics::SpawnSound2D(Current, Track,
                    NativeWadOptions::EffectiveVolume(NativeWadOptions::EAudioChannel::Music, 0.45f), 1.0f, 0.0f, nullptr, true);
                if (BackgroundMusic)
                {
                    NativeWadOptions::TrackAudio(BackgroundMusic, NativeWadOptions::EAudioChannel::Music, 0.45f);
                    UE_LOG(LogTemp, Display, TEXT("Started WAD music %s for %s."), *Track->GetName(), *Map.ToString());
                }
            }
        });
        UAssetManager::GetStreamableManager().RequestAsyncLoad(Path, MoveTemp(Start));
    }
}

bool UNativeWadTravelState::IsBackgroundMusicPlaying() const
{
    return IsValid(BackgroundMusic) && BackgroundMusic->IsPlaying();
}

FName UNativeWadTravelState::BackgroundMusicName() const
{
    return IsValid(BackgroundMusic) && BackgroundMusic->Sound ? BackgroundMusic->Sound->GetFName() : NAME_None;
}

bool ANativeWadLevel::WriteCheckpoint(FWadLevelCheckpoint& Checkpoint, bool bDiscardProjectiles) const
{
    FString LightingError;
    if(bUseSourceLightFeedback&&(SourceLightState.Version!=NativeWadLightRules::FeedbackStateVersion
        ||!ValidateSourceLightCheckpoint(SourceLightState,LightingError))){return false;}

    if (!bDiscardProjectiles && TActorIterator<ANativeWadBolt>(GetWorld()))
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
    Checkpoint.ActivityWallTextures = ActivityWallTextures;
    Checkpoint.LightLevels = LightOverrides;
    Checkpoint.SourceLighting = bUseSourceLightFeedback?SourceLightState:FWadSourceLightState{};
    Checkpoint.PendingPickupGenerators = PendingPickupGenerators;
    Checkpoint.FiredPickupGenerators = FiredPickupGenerators;
    Checkpoint.UnlockedItemDoors = UnlockedItemDoors;
    Checkpoint.ScriptLockedDoors = ScriptLockedDoors;
    Checkpoint.DoorLockStrengths = DoorLockStrengths;
    Checkpoint.CreatureGenerators = CreatureGeneratorStates;
    Checkpoint.PickupRevision = FWadLevelCheckpoint::CurrentPickupRevision;
    for (const auto& [Id, Object] : ScriptObjects)
    {
        if (IsValid(Object))
        {
            Checkpoint.ScriptObjectAngles.Add(Id, Object->GetSourceAngle());
        }
    }
    Checkpoint.Hour = GetTimeOfDayHours();
    for (const FActivitySlide& Slide : ActivitySlides)
    {
        Checkpoint.Slides.Add({Slide.Sector, Slide.From, Slide.To, Slide.Duration, Slide.Elapsed,
            Slide.bCeiling, Slide.bPauseOnCrush, Slide.CompletionActivity});
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
        FWadSavedScriptDelay& Saved = Checkpoint.Delays.AddDefaulted_GetRef();
        Saved.Activity = Waiting.Activity;
        Saved.NextCommand = Waiting.NextCommand;
        Saved.SecondsRemaining = FMath::Max(0.001f, Remaining);
        Saved.bHasPlayer = Waiting.Player.IsValid();
        if (const FWadScriptActivity* Activity = FindWadActivity(Waiting.Activity))
        {
            Saved.SourceCommandOffset = Activity->LegacyResumeCommands.IsEmpty() ? INDEX_NONE
                : Activity->Commands.IsValidIndex(Waiting.NextCommand)
                ? Activity->Commands[Waiting.NextCommand].SourceOffset : MAX_int32;
        }
    }
    for (TActorIterator<ANativeWadPickup> Item(GetWorld()); Item; ++Item)
    {
        if (Item->IsDropped())
        {
            NativeWadPickupMetadata::WriteSavedDrop(**Item, Checkpoint.DroppedItems.AddDefaulted_GetRef());
        }
        else if (!Item->Tags.IsEmpty())
        {
            Checkpoint.RemainingPickups.Add(Item->Tags[0]);
        }
    }
    for (TActorIterator<ANativeWadGuardian> Encounter(GetWorld()); Encounter; ++Encounter)
    {
        // A source Hydra disappears after its sink death; retain that defeated
        // life so restoring the map cannot bring the hidden corpse back alive.
        if ((!Encounter->IsHidden() || (Encounter->IsDefeated()
            && (Encounter->GetEncounterProfile().bDipper || Encounter->GetSourceType() == 1012)))
            && !Encounter->Tags.IsEmpty())
        {
            const ANativeWadGuardian* Target = Encounter->GetBerserkTarget();
            const FName TargetTag = Target && !Target->Tags.IsEmpty() ? Target->Tags[0] : NAME_None;
            Checkpoint.Encounters.Add(Encounter->Tags[0], {Encounter->GetActorTransform(), Encounter->GetEncounterHealth(),
                Encounter->GetPoisonLevel(), Encounter->GetPoisonTime(), Encounter->GetSlowSeconds(), Encounter->GetEarthboundSeconds(),
                Encounter->GetCreatureLife(), TargetTag, Encounter->GetBerserkSeconds(), Encounter->GetBerserkTargetLife()});
        }
    }
    return true;
}

bool ANativeWadLevel::RestoreCheckpoint(const FWadLevelCheckpoint& Checkpoint)
{
    FString LightingError;
    // Validate before geometry, script, encounter or other checkpoint mutation.
    if(!ValidateSourceLightCheckpoint(Checkpoint.SourceLighting,LightingError)){return false;}

    if (Checkpoint.Variables.Num() != UE_ARRAY_COUNT(ScriptVariables) || Checkpoint.Flags.Num() != UE_ARRAY_COUNT(ScriptFlags))
    {
        return false;
    }
    InitializeTopology();
    for (const auto& [Id, Angle] : Checkpoint.ScriptObjectAngles)
    {
        if (!IsValid(ScriptObjects.FindRef(Id)))
        {
            return false;
        }
    }
    for (const auto& [Id, State] : Checkpoint.CreatureGenerators)
    {
        const FWadCreatureGenerator* Generator = CreatureGenerators.Find(Id);
        if (!Generator || State.Released < 0 || (!Generator->bInfinite && State.Released > Generator->NumReleases())
            || !FMath::IsFinite(State.Remaining) || State.Remaining < 0 || State.Remaining > Generator->Interval + Generator->RandomInterval
            || (!Generator->IsExhausted(State.Released)) != (State.Remaining > 0))
        {
            return false;
        }
    }
    for (const auto& [Id, Remaining] : Checkpoint.PendingPickupGenerators)
    {
        const FWadPickupGenerator* Generator = PickupGenerators.Find(Id);
        if (!Generator || !FMath::IsFinite(Remaining) || Remaining <= 0.0f || Remaining > Generator->Delay
            || Checkpoint.FiredPickupGenerators.Contains(Id))
        {
            return false;
        }
    }
    for (const int32 Id : Checkpoint.FiredPickupGenerators)
    {
        if (!PickupGenerators.Contains(Id))
        {
            return false;
        }
    }
    for (const int32 Sector : Checkpoint.UnlockedItemDoors)
    {
        const int32* Door = DoorBySector.Find(Sector);
        if (!Door || Doors[*Door].RequiredItem <= 0)
        {
            return false;
        }
    }
    for (const int32 Sector : Checkpoint.ScriptLockedDoors)
    {
        if (!DoorBySector.Contains(Sector))
        {
            return false;
        }
    }
    for (const auto& [Sector, Strength] : Checkpoint.DoorLockStrengths)
    {
        if (!DoorBySector.Contains(Sector) || Strength < 0 || Strength > 255) { return false; }
    }
    for (const FWadSavedSlide& Slide : Checkpoint.Slides)
    {
        FVector2D HeightsAtSave;
        if (!GetSectorSurfaceHeights(Slide.Sector, HeightsAtSave) || !FMath::IsFinite(Slide.From) || !FMath::IsFinite(Slide.To)
            || !FMath::IsFinite(Slide.Duration) || !FMath::IsFinite(Slide.Elapsed)
            || Slide.Duration <= 0.0 || Slide.Elapsed < 0.0 || Slide.Elapsed > Slide.Duration
            || (Slide.CompletionActivity != INDEX_NONE && !FindWadActivity(Slide.CompletionActivity)))
        {
            return false;
        }
    }
    for (const auto& [Side, Texture] : Checkpoint.ActivityWallTextures)
    {
        if (!FindActivitySideBand(Side) || !ActivityWallMaterials.Contains(Texture))
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
        if (!Activity || ResolveWadScriptContinuation(*Activity, Waiting.NextCommand, Waiting.SourceCommandOffset) == INDEX_NONE
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
        ActivitySlides.Add({Slide.Sector, Slide.From, Slide.To, Slide.Duration, Slide.Elapsed,
            Slide.bCeiling, Slide.bPauseOnCrush, Slide.CompletionActivity});
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
        const FWadScriptActivity* Activity = FindWadActivity(Waiting.Activity);
        ScheduleScriptContinuation(Waiting.Activity, ResolveWadScriptContinuation(*Activity, Waiting.NextCommand, Waiting.SourceCommandOffset), Waiting.bHasPlayer ? Player : nullptr,
            Waiting.SecondsRemaining);
    }
    PendingPickupGenerators = Checkpoint.PendingPickupGenerators;
    FiredPickupGenerators = Checkpoint.FiredPickupGenerators;
    UnlockedItemDoors = Checkpoint.UnlockedItemDoors;
    ScriptLockedDoors = Checkpoint.ScriptLockedDoors;
    DoorLockStrengths = Checkpoint.DoorLockStrengths;
    CreatureGeneratorStates = Checkpoint.CreatureGenerators;
    for (const auto& [Id, Angle] : Checkpoint.ScriptObjectAngles)
    {
        ScriptObjects.FindChecked(Id)->SetSourceAngle(Angle);
    }
    for (const auto& [Id, Generator] : CreatureGenerators)
    {
        if (Generator.bStartsActive && !CreatureGeneratorStates.Contains(Id))
        {
            ActivateCreatureGenerator(Id);
        }
        const int32 Released = CreatureGeneratorStates.FindRef(Id).Released;
        for (int32 Index = 0; Index < Generator.Creatures.Num(); ++Index)
        {
            if (ANativeWadGuardian* Creature = Generator.Creatures[Index]; IsValid(Creature))
            {
                Creature->SetSpawned(Index < Released);
            }
        }
    }
    if (!ActivitySlides.IsEmpty() || !ActiveDoors.IsEmpty() || !PendingPickupGenerators.IsEmpty() || HasPendingCreatureGenerators())
    {
        SetActorTickEnabled(true);
    }
    for (const auto& [Side, bUp] : Checkpoint.Switches)
    {
        SetActivitySwitch(Side, bUp);
    }
    for (const auto& [Side, Texture] : Checkpoint.ActivityWallTextures)
    {
        SetActivityWallTexture(Side, Texture);
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
    if(!RestoreSourceLightState(Checkpoint.SourceLighting,LightingError)){return false;}
    RefreshSectorLights();
    TSet<const ANativeWadPickup*> DormantPickups;
    for (const auto& [Id, Generator] : CreatureGenerators)
    {
        const int32 Released = CreatureGeneratorStates.FindRef(Id).Released;
        for (int32 Index = 0; Index < Generator.Pickups.Num(); ++Index)
        {
            if (ANativeWadPickup* Pickup = Generator.Pickups[Index]; IsValid(Pickup))
            {
                Pickup->SetAvailable(Index < Released);
                if (Index >= Released)
                {
                    DormantPickups.Add(Pickup);
                }
            }
        }
    }
    for (const auto& [Id, Generator] : PickupGenerators)
    {
        if (IsValid(Generator.Pickup))
        {
            const bool bFired = FiredPickupGenerators.Contains(Id);
            Generator.Pickup->SetAvailable(bFired);
            if (!bFired)
            {
                DormantPickups.Add(Generator.Pickup);
            }
        }
    }
    TArray<ANativeWadPickup*> OldDrops;
    for (TActorIterator<ANativeWadPickup> Item(GetWorld()); Item; ++Item)
    {
        if (Item->IsDropped())
        {
            OldDrops.Add(*Item);
        }
    }
    TArray<ANativeWadPickup*> RestoredDrops;
    for (const FWadSavedDrop& Drop : Checkpoint.DroppedItems)
    {
        ANativeWadPickup* Item = Drop.Position.ContainsNaN() ? nullptr : ANativeWadPickup::SpawnDropped(*this, Drop.Type, Drop.Position);
        if (!Item)
        {
            for (ANativeWadPickup* Restored : RestoredDrops)
            {
                Restored->Destroy();
            }
            return false;
        }
        NativeWadPickupMetadata::RestoreSavedDrop(Drop, *Item);
        RestoredDrops.Add(Item);
    }
    for (ANativeWadPickup* Item : OldDrops)
    {
        Item->Destroy();
    }
    for (TActorIterator<ANativeWadPickup> Item(GetWorld()); Item; ++Item)
    {
        if (!Item->Tags.IsEmpty() && !DormantPickups.Contains(*Item) && !Checkpoint.RemainingPickups.Contains(Item->Tags[0])
            && Item->GetCheckpointRevision() <= Checkpoint.PickupRevision)
        {
            Item->Destroy();
        }
    }
    TMap<FName, ANativeWadGuardian*> RestoredEncounters;
    for (TActorIterator<ANativeWadGuardian> Encounter(GetWorld()); Encounter; ++Encounter)
    {
        if (!Encounter->Tags.IsEmpty())
        {
            if (const FWadSavedEncounter* State = Checkpoint.Encounters.Find(Encounter->Tags[0]))
            {
                Encounter->SetActorTransform(State->Transform, false, nullptr, ETeleportType::TeleportPhysics);
                Encounter->RestoreEncounterHealth(State->Health);
                Encounter->RestorePoison(State->Poison, State->PoisonTime);
                Encounter->RestoreControlEffects(State->SlowSeconds, State->EarthboundSeconds);
                Encounter->RestoreCreatureLife(State->CreatureLife);
                RestoredEncounters.Add(Encounter->Tags[0], *Encounter);
            }
        }
    }
    // Resolve shared creature references after every life and health state has been restored.
    for (const auto& [Tag, Encounter] : RestoredEncounters)
    {
        const FWadSavedEncounter& State = Checkpoint.Encounters.FindChecked(Tag);
        Encounter->RestoreBerserkTarget(RestoredEncounters.FindRef(State.BerserkTarget), State.BerserkSeconds, State.BerserkTargetLife);
    }
    return true;
}

bool ANativeWadPlayer::SaveWadCheckpoint(const FString& Slot) const
{
    if(!HasValidCarriedObjectData()){return false;}
    const auto& CheckedBag=GetBagLayout();const auto CheckedItems=GetBagItems();
    if(!CheckedBag.Validate(CheckedItems)||CheckedBag.Encode(CheckedItems).IsEmpty()){return false;}
    UNativeWadCheckpoint* Save = NewObject<UNativeWadCheckpoint>();
    Save->Version = UNativeWadCheckpoint::CurrentVersion;
    Save->bTown = IsInWadTown();
    const ANativeWadLevel* Level = NativeLevel.Get();
    if (IsDefeated() || (!Save->bTown && (QuakeTime > 0.0 || !GetCharacterMovement()->IsMovingOnGround()
        || !Level || !Level->WriteCheckpoint(Save->Level))))
    {
        return false;
    }
    Save->LevelPackage = CheckpointMap(*GetWorld());
    Save->Position = GetActorLocation();
    Save->View = GetControlRotation();
    Save->NeedsTime = NeedsTime;
    Save->PlayerOptions = MakeTravelOptions();
    const FURL SavedOptions(nullptr,*(Save->LevelPackage+TEXT("?")+Save->PlayerOptions),TRAVEL_Absolute);
    if(!NativeWadCarriedObjectState::ValidateOptions(SavedOptions)){return false;}
    if(!NativeWadBagState::ValidateOptions(SavedOptions,NativeWadBagState::CatalogRule)){return false;}
    Save->bItemIdentificationRecorded = bItemIdentificationInitialized;
    Save->IdentifiedItemTypes = IdentifiedItemTypes;
    if (const UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>())
    {
        Save->VisitedLevels = Travel->VisitedLevels;
        Save->CompletedLevels = Travel->CompletedLevels;
        Save->Quest = Travel->Quest;
        Save->JournalPages = Travel->JournalPages;
        Save->PlayerNotes = Travel->PlayerNotes.Left(999);
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
    if (!Save || Save->LevelPackage != CheckpointMap(*GetWorld()) || Save->bTown != IsInWadTown()
        || Save->Position.ContainsNaN() || Save->View.ContainsNaN()
        || (!Save->bTown && (!Level || !Level->RestoreCheckpoint(Save->Level))))
    {
        UE_LOG(LogTemp, Error, TEXT("No compatible WAD checkpoint for this map."));
        return;
    }
    const FURL SavedURL(nullptr, *(Save->LevelPackage + TEXT("?") + Save->PlayerOptions), TRAVEL_Absolute);
    CarriedItems.Reset();
    CarriedObjectData=FWadCarriedObjectState();
    bCarriedObjectDataInitialized=false;bCarriedObjectDataValid=true;
    ReadTravelOptions(SavedURL);
    RestoreItemIdentification(Save->IdentifiedItemTypes, Save->bItemIdentificationRecorded);
    NeedsTime = FMath::Clamp(Save->NeedsTime, 0.0, 6.0);
    SetActorLocation(Save->Position, false, nullptr, ETeleportType::TeleportPhysics);
    GetCharacterMovement()->StopMovementImmediately();
    GetController()->SetControlRotation(Save->View);

    // Arrival establishes the current sector without replaying a script that was already executed.
    if (Level)
    {
        LastActivitySector = Level->FindActivitySector(FVector2D(Save->Position), GetCapsuleComponent()->GetScaledCapsuleRadius());
        LastLevelLink = Level->FindLevelLinkAt(FVector2D(Save->Position));
        bCheckedLevelLink = true;
    }
    if (UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>())
    {
        Travel->VisitedLevels = Save->VisitedLevels;
        Travel->CompletedLevels = Save->CompletedLevels;
        Travel->Quest = Save->Quest;
        Travel->JournalPages = Save->JournalPages;
        Travel->PlayerNotes = Save->PlayerNotes.Left(999);
        Travel->ArrivingFromMap = 0;
    }
    if (!SavedURL.HasOption(TEXT("WadPhysicalCoins")))
    {
        MoveLegacyCoinsToPurse();
    }
    bCheckpointRestored = true;
    UE_LOG(LogTemp, Display, TEXT("Restored WAD checkpoint in %s."), *Save->LevelPackage);
}

bool ANativeWadPlayer::HasCompletedWadLevel(const FString& Name) const
{
    const UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    return Travel && Travel->CompletedLevels.Contains(Name);
}

bool ANativeWadPlayer::OpenWadTown()
{
    UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    if (IsDefeated() || !Travel || !FPackageName::DoesPackageExist(NativeWadTown::Package) || !ArchiveDeparture())
    {
        return false;
    }
    Travel->CompletedLevels.Add(GetWorld()->RemovePIEPrefix(GetWorld()->GetMapName()));
    Travel->ArrivingFromMap = NativeWadTown::MapNumber(CheckpointMap(*GetWorld()));
    UE_LOG(LogTemp, Display, TEXT("Entered WAD campaign form %d."), NativeWadTown::Form);
    UGameplayStatics::OpenLevelBySoftObjectPtr(this, TSoftObjectPtr<UWorld>(FSoftObjectPath(NativeWadTown::ObjectPath)),
        true, MakeTravelOptions());
    return true;
}

bool ANativeWadPlayer::ArchiveDeparture()
{
    if (IsDefeated()) { return false; }
    StoreItemIdentification();
    const ANativeWadLevel* Level = NativeLevel.Get();
    UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    FWadLevelCheckpoint Snapshot;
    if (Level && Travel && Level->WriteCheckpoint(Snapshot, true))
    {
        const FString Package = CheckpointMap(*GetWorld());
        Travel->VisitedLevels.Add(Package, MoveTemp(Snapshot));
        UE_LOG(LogTemp, Display, TEXT("Archived WAD level %s on departure."), *Package);
        return true;
    }
    return false;
}

void ANativeWadPlayer::RestoreVisitedLevel()
{
    const UNativeWadTravelState* Travel = GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
    if (Travel && (Travel->bItemIdentificationRecorded || bWadCharacterCreated))
    { RestoreItemIdentification(Travel->IdentifiedItemTypes, Travel->bItemIdentificationRecorded); }
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
