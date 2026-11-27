#include "NativeWadLevel.h"

#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadEncounter.h"
#include "NativeWadOptions.h"
#include "NativeWadPlayer.h"
#include "NativeWadScenery.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"

namespace
{
    constexpr float TicksPerSecond = 70.0f;
    constexpr double UseReach = 64.0;
    constexpr int32 ScriptBudget = 512;
    constexpr int32 PoisonMissileDamage = 250;
    constexpr int32 PoisonCrossbowDamage = 300;
    constexpr double AmbientRadius = 500.0;

    USoundWaveProcedural* MakeWadSound(const FWadSoundSample& Sample, bool bLoop)
    {
        constexpr int32 SampleRate = 11000;
        USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>();
        Wave->SetSampleRate(SampleRate);
        Wave->NumChannels = 1;
        Wave->Duration = bLoop ? INDEFINITELY_LOOPING_DURATION : float(Sample.UnsignedSamples.Num()) / SampleRate;
        TArray<int16> PCM;
        PCM.Reserve(Sample.UnsignedSamples.Num());
        for (uint8 Byte : Sample.UnsignedSamples)
        {
            PCM.Add(int16((int32(Byte) - 128) * 256));
        }
        Wave->QueueAudio(reinterpret_cast<const uint8*>(PCM.GetData()), PCM.Num() * sizeof(int16));
        if (bLoop)
        {
            Wave->OnSoundWaveProceduralUnderflow.BindLambda(
                [Loop = MoveTemp(PCM)](USoundWaveProcedural* Sound, int32 SamplesRequested)
                {
                    const int32 BytesRequested = SamplesRequested * sizeof(int16);
                    while (Sound->GetAvailableAudioByteCount() < BytesRequested)
                    {
                        Sound->QueueAudio(reinterpret_cast<const uint8*>(Loop.GetData()), Loop.Num() * sizeof(int16));
                    }
                });
        }
        return Wave;
    }
}

bool ANativeWadLevel::UseActivityFromView(ANativeWadPlayer& Player)
{
    if (!HasWadActivities())
    {
        return false;
    }
    const FVector Start = Player.GetPawnViewLocation();
    const FVector End = Start + Player.GetControlRotation().Vector() * UseReach;
    const FTransform& Transform = GetActorTransform();
    const FVector Local = Transform.InverseTransformPosition(Start);
    const FVector Delta = Transform.InverseTransformPosition(End) - Local;
    const FVector2D Ray(Delta);
    double Closest = 1.0;
    int32 Activity = 0;
    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        const FAAWadNativeLine& Line = Lines[Index];
        if (!Line.Activity)
        {
            continue;
        }
        const FVector2D A(Vertices[Line.Endpoints.X]);
        const FVector2D Edge = FVector2D(Vertices[Line.Endpoints.Y]) - A;
        const double Cross = FVector2D::CrossProduct(Ray, Edge);
        if (FMath::IsNearlyZero(Cross))
        {
            continue;
        }
        const double AlongRay = FVector2D::CrossProduct(A - FVector2D(Local), Edge) / Cross;
        const double AlongEdge = FVector2D::CrossProduct(A - FVector2D(Local), Ray) / Cross;
        if (AlongRay < 0.0 || AlongRay >= Closest || AlongEdge < 0.0 || AlongEdge > 1.0)
        {
            continue;
        }
        const double Z = Local.Z + Delta.Z * AlongRay;
        FWadWallLine Wall;
        if (GetWallLine(Index, Wall) && Wall.Surfaces.ContainsByPredicate([Z](const FWadWallSurface& Surface)
            { return Z >= Surface.Bottom && Z <= Surface.Top; }))
        {
            Closest = AlongRay;
            Activity = Line.Activity;
        }
    }
    if (!Activity)
    {
        return false;
    }
    FHitResult Hit;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(UseNativeWadActivity), true, &Player);
    const FVector BeforeWall = FMath::Lerp(Start, End, FMath::Max(0.0, Closest - 0.5 / UseReach));
    return !GetWorld()->LineTraceSingleByChannel(Hit, Start, BeforeWall, ECC_Visibility, Query)
        && RunWadActivity(Activity, &Player);
}

void ANativeWadLevel::CheckActivitySector(ANativeWadPlayer& Player, int32& PreviousSector)
{
    const int32 Sector = FindActivitySector(FVector2D(Player.GetActorLocation()), Player.GetCapsuleComponent()->GetScaledCapsuleRadius(), PreviousSector);
    if (Sector != INDEX_NONE && PreviousSector != Sector)
    {
        UE_LOG(LogTemp, Verbose, TEXT("WAD activity sector %d at %s."), Sector, *Player.GetActorLocation().ToCompactString());
        RunWadActivity(Sectors[Sector].Activity, &Player);
    }
    PreviousSector = Sector;
}

int32 ANativeWadLevel::FindActivitySector(const FVector2D& Position, double Radius, int32 PreviousSector) const
{
    const FVector2D Local(GetActorTransform().InverseTransformPosition(FVector(Position, GetActorLocation().Z)));
    for (const int32 Center : ActivitySectors)
    {
        if (!ContainsSectorPoint(Center, Local))
        {
            continue;
        }

        // OBJMOVE.C and 3D_COLLI.C select the highest floor under the collision footprint.
        int32 Selected = Center;
        if (Radius > 0.0 && LinesBySector.IsValidIndex(Center) && Heights.IsValidIndex(Center))
        {
            for (const int32 LineIndex : LinesBySector[Center])
            {
                const FAAWadNativeLine& Line = Lines[LineIndex];
                const int32 Front = WallTopology.GetSectorForSide(Line.Sides.X);
                const int32 Back = WallTopology.GetSectorForSide(Line.Sides.Y);
                const int32 Neighbor = Front == Center ? Back : Front;
                if (!(Line.Flags & 0x0004) || !Heights.IsValidIndex(Neighbor) || !Sectors[Neighbor].Activity)
                {
                    continue;
                }

                const double Difference = GetWalkingHeight(Neighbor) - GetWalkingHeight(Selected);
                // Keep the current choice across a shared edge if a plate lowers to the same height.
                if (Difference <= 0.0 && (Neighbor != PreviousSector || !FMath::IsNearlyZero(Difference)))
                {
                    continue;
                }

                const FVector Start(FVector2D(Vertices[Line.Endpoints.X]), 0);
                const FVector End(FVector2D(Vertices[Line.Endpoints.Y]), 0);
                if (FMath::PointDistToSegmentSquared(FVector(Local, 0), Start, End) < FMath::Square(Radius))
                {
                    Selected = Neighbor;
                }
            }
        }
        return Selected;
    }
    return INDEX_NONE;
}

const FWadScriptActivity* ANativeWadLevel::FindWadActivity(int32 Activity) const
{
    return ImportedActivities.FindByPredicate([Activity](const FWadScriptActivity& Script) { return Script.Id == Activity; });
}

bool ANativeWadLevel::RunWadActivity(int32 Activity, ANativeWadPlayer* Player)
{
    if (!FindWadActivity(Activity))
    {
        return false;
    }
    UE_LOG(LogTemp, Display, TEXT("Ran imported WAD script activity %d."), Activity);
    int32 Budget = ScriptBudget;
    RunWadScriptCommands(Activity, 0, Player, Budget);
    return true;
}

void ANativeWadLevel::RunWadScriptCommands(int32 Activity, int32 StartCommand, TWeakObjectPtr<ANativeWadPlayer> Player,
    int32& Budget)
{
    const FWadScriptActivity* Script = FindWadActivity(Activity);
    for (int32 Index = StartCommand; Script && Script->Commands.IsValidIndex(Index) && --Budget >= 0; ++Index)
    {
        const FWadScriptCommand& Command = Script->Commands[Index];
        switch (Command.Operation)
        {
        case EWadScriptOperation::SetSwitch:
            SetActivitySwitch(Command.Target, Command.Value != 0);
            break;
        case EWadScriptOperation::ChangeSideTexture:
            SetActivityWallTexture(Command.Target, FName(*Command.Text));
            break;
        case EWadScriptOperation::ToggleWallSwitch:
        {
            const bool bDown = ScriptVariables[Command.Value] == 0;
            ScriptVariables[Command.Value] = bDown ? 1 : 0;
            SetActivityWallTexture(Command.Target, FName(*(bDown ? Command.SwitchDownTexture : Command.Text)));
            const int32 Next = bDown ? Command.SwitchDownActivity : Command.SwitchUpActivity;
            if (Next != INDEX_NONE)
            {
                Activity = Next;
                Script = FindWadActivity(Activity);
                Index = -1;
            }
            break;
        }
        case EWadScriptOperation::AddJournalPage:
            if (UTexture2D* Image = ActivityJournalPages.FindRef(Command.Target))
            {
                if (auto* Travel = UGameInstance::GetSubsystem<UNativeWadTravelState>(GetGameInstance()))
                {
                    Travel->JournalPages.Add(Command.Target, Image);
                }
            }
            break;
        case EWadScriptOperation::Wait:
            ScheduleScriptContinuation(Activity, Index + 1, Player, Command.Ticks / TicksPerSecond);
            return;
        case EWadScriptOperation::Message:
            if (ANativeWadPlayer* Pawn = Player.Get())
            {
                Pawn->ShowInteractionMessage(Command.Text);
            }
            break;
        case EWadScriptOperation::SlideFloor:
        case EWadScriptOperation::SlideCeiling:
            StartActivitySlide(Command.Target, Command.Value, Command.Ticks,
                Command.Operation == EWadScriptOperation::SlideCeiling, Command.bPauseOnCrush, Command.CompletionActivity);
            break;
        case EWadScriptOperation::Block:
            if (ScriptVariables[Command.Target] != 0)
            {
                ++ScriptBlockCounts.FindOrAdd(Activity);
                return;
            }
            ScriptVariables[Command.Target] = 1;
            break;
        case EWadScriptOperation::Unblock:
            ScriptVariables[Command.Target] = 0;
            break;
        case EWadScriptOperation::SetVariable:
            ScriptVariables[Command.Target] = Command.Value;
            break;
        case EWadScriptOperation::IncrementVariable:
            ScriptVariables[Command.Target] = int32(uint32(ScriptVariables[Command.Target]) + 1u);
            break;
        case EWadScriptOperation::ActivateGenerator:
            ActivatePickupGenerator(Command.Target);
            ActivateCreatureGenerator(Command.Target);
            break;
        case EWadScriptOperation::DeactivateGenerator:
            if (FWadCreatureGeneratorState* State = CreatureGeneratorStates.Find(Command.Target))
            {
                State->bPaused = true;
            }
            break;
        case EWadScriptOperation::AddNumbers:
        case EWadScriptOperation::SubtractNumbers:
        case EWadScriptOperation::MultiplyNumbers:
        {
            const uint32 Left = Command.Left.bVariable ? ScriptVariables[Command.Left.Value] : Command.Left.Value;
            const uint32 Right = Command.Right.bVariable ? ScriptVariables[Command.Right.Value] : Command.Right.Value;
            ScriptVariables[Command.Target] = int32(Command.Operation == EWadScriptOperation::AddNumbers ? Left + Right
                : Command.Operation == EWadScriptOperation::SubtractNumbers ? Left - Right : Left * Right);
            break;
        }
        case EWadScriptOperation::ReadObjectAngle:
            if (const ANativeWadScenery* Scenery = ScriptObjects.FindRef(Command.Target).Get())
            {
                ScriptVariables[Command.Value] = Scenery->GetSourceAngle();
            }
            break;
        case EWadScriptOperation::SetObjectAngle:
            if (ANativeWadScenery* Scenery = ScriptObjects.FindRef(Command.Target).Get())
            {
                const int32 Angle = Command.Left.bVariable ? ScriptVariables[Command.Left.Value] : Command.Left.Value;
                Scenery->SetSourceAngle(uint16(Angle));
            }
            break;
        case EWadScriptOperation::RestoreVitals:
            if (ANativeWadPlayer* Pawn = Player.Get())
            {
                Pawn->RestoreWadVital(Command.Target, Command.Value);
            }
            break;
        case EWadScriptOperation::CompareVariable:
        {
            const int32 A = ScriptVariables[Command.Target];
            const int32 B = Command.Value;
            ScriptFlags[0] = A == B;
            ScriptFlags[1] = A != B;
            ScriptFlags[2] = A < B;
            ScriptFlags[3] = A >= B;
            ScriptFlags[4] = A > B;
            ScriptFlags[5] = A <= B;
            ScriptFlags[6] = A <= B;
            ScriptFlags[7] = A >= B;
            break;
        }
        case EWadScriptOperation::IfFlag:
            if (!ScriptFlags[Command.Value])
            {
                break;
            }
            [[fallthrough]];
        case EWadScriptOperation::Jump:
            Activity = Command.Target;
            Script = FindWadActivity(Activity);
            Index = -1;
            break;
        case EWadScriptOperation::Call:
            // A delay in a source subroutine schedules its continuation and returns to the caller immediately.
            RunWadScriptCommands(Command.Target, 0, Player, Budget);
            break;
        case EWadScriptOperation::PlaySound:
        case EWadScriptOperation::PlayAreaSound:
            PlayActivitySound(Command);
            break;
        case EWadScriptOperation::Earthquake:
            if (ANativeWadPlayer* LocalPlayer = Cast<ANativeWadPlayer>(UGameplayStatics::GetPlayerPawn(this, 0)))
            {
                LocalPlayer->StartWadEarthquake(Command.Ticks / TicksPerSecond);
            }
            break;
        case EWadScriptOperation::LaunchMissile:
            LaunchActivityMissile(Command);
            break;
        case EWadScriptOperation::TeleportPlayer:
            if (ANativeWadPlayer* Pawn = Player.Get())
            {
                TeleportActivityPlayer(*Pawn, FVector2D(Command.Position.X, Command.Position.Y));
            }
            break;
        }
    }
    if (Budget == -1)
    {
        UE_LOG(LogTemp, Warning, TEXT("WAD script instruction budget exhausted in activity %d."), Activity);
    }
}

void ANativeWadLevel::ActivatePickupGenerator(int32 Id)
{
    const FWadPickupGenerator* Generator = PickupGenerators.Find(Id);
    if (Generator && IsValid(Generator->Pickup) && !FiredPickupGenerators.Contains(Id) && !PendingPickupGenerators.Contains(Id))
    {
        PendingPickupGenerators.Add(Id, Generator->Delay);
        SetActorTickEnabled(true);
    }
}

void ANativeWadLevel::TickPickupGenerators(float DeltaSeconds)
{
    for (auto It = PendingPickupGenerators.CreateIterator(); It; ++It)
    {
        It.Value() -= DeltaSeconds;
        if (It.Value() <= 0.0f)
        {
            const int32 Id = It.Key();
            if (const FWadPickupGenerator* Generator = PickupGenerators.Find(Id); Generator && IsValid(Generator->Pickup))
            {
                Generator->Pickup->SetAvailable(true);
                UE_LOG(LogTemp, Display, TEXT("WAD generator %d released item %d."), Id, Generator->Pickup->GetItemType());
            }
            FiredPickupGenerators.Add(Id);
            It.RemoveCurrent();
        }
    }
}

void ANativeWadLevel::ActivateCreatureGenerator(int32 Id)
{
    if (const FWadCreatureGenerator* Generator = CreatureGenerators.Find(Id))
    {
        FWadCreatureGeneratorState* State = CreatureGeneratorStates.Find(Id);
        if (!State)
        {
            const float Delay = Generator->Interval + FMath::RandHelper(Generator->RandomInterval);
            State = &CreatureGeneratorStates.Add(Id, {0, Delay});
        }
        State->bPaused = false;
        SetActorTickEnabled(true);
    }
}

bool ANativeWadLevel::HasPendingCreatureGenerators() const
{
    for (const auto& [Id, State] : CreatureGeneratorStates)
    {
        if (State.Remaining > 0.0f && !State.bPaused)
        {
            return true;
        }
    }
    return false;
}

void ANativeWadLevel::TickCreatureGenerators(float DeltaSeconds)
{
    for (auto& [Id, State] : CreatureGeneratorStates)
    {
        const FWadCreatureGenerator& Generator = CreatureGenerators.FindChecked(Id);
        if (State.bPaused || Generator.IsExhausted(State.Released))
        {
            continue;
        }
        ANativeWadGuardian* Summoner = Generator.Summoner.Get();
        if (Summoner && Summoner->IsDefeated())
        {
            State.bPaused = true;
            continue;
        }
        if (Summoner && !Summoner->HasAcquiredTarget())
        {
            continue;
        }
        State.Remaining -= DeltaSeconds;
        if (State.Remaining > 0.0f)
        {
            continue;
        }
        State.Remaining = Generator.Interval + FMath::RandHelper(Generator.RandomInterval);
        if (Generator.Pickups.IsValidIndex(State.Released))
        {
            int32 Active = 0;
            for (TActorIterator<ANativeWadPickup> Item(GetWorld()); Item && Active < Generator.MaxActive; ++Item)
            {
                Active += Item->GetItemType() == Generator.SourceType && Item->IsAvailable();
            }
            if (Generator.MaxActive > 0 && Active >= Generator.MaxActive)
            {
                continue;
            }
            if (ANativeWadPickup* Pickup = Generator.Pickups[State.Released]; IsValid(Pickup))
            {
                Pickup->SetAvailable(true);
                UE_LOG(LogTemp, Display, TEXT("WAD generator %d released item %d (%d/%d)."),
                    Id, Generator.SourceType, State.Released + 1, Generator.NumReleases());
            }
            if (++State.Released == Generator.NumReleases())
            {
                State.Remaining = 0.0f;
            }
            continue;
        }
        if (Generator.MaxActive > 0)
        {
            int32 Active = 0;
            for (TActorIterator<ANativeWadGuardian> It(GetWorld()); It && Active < Generator.MaxActive; ++It)
            {
                if (!It->IsHidden() && !It->IsDefeated() && (Summoner || It->GetSourceType() == Generator.SourceType))
                {
                    ++Active;
                }
            }
            if (Active >= Generator.MaxActive)
            {
                continue;
            }
        }
        int32 CreatureIndex = State.Released;
        if (Generator.bInfinite)
        {
            CreatureIndex = INDEX_NONE;
            for (int32 Offset = 0; Offset < Generator.Creatures.Num(); ++Offset)
            {
                const int32 Index = (State.Released % Generator.Creatures.Num() + Offset) % Generator.Creatures.Num();
                ANativeWadGuardian* Candidate = Generator.Creatures[Index];
                if (IsValid(Candidate) && (Candidate->IsHidden() || Candidate->IsDefeated()))
                {
                    CreatureIndex = Index;
                    break;
                }
            }
            if (CreatureIndex == INDEX_NONE) { continue; }
        }
        ANativeWadGuardian* Creature = Generator.Creatures[CreatureIndex];
        if (!IsValid(Creature))
        {
            UE_LOG(LogTemp, Warning, TEXT("WAD generator %d is missing creature %d in %s."),
                Id, State.Released, *GetWorld()->GetMapName());
            ++State.Released;
            if (Generator.IsExhausted(State.Released))
            {
                State.Remaining = 0.0f;
            }
            continue;
        }
        FVector Spawn = Generator.bInfinite ? GetActorLocation() + FVector(Generator.SpawnXY, 0.0)
            : Creature->GetActorLocation();
        const UCapsuleComponent* Capsule = Creature->GetCapsuleComponent();
        int32 Sector = Generator.Sector;
        if (Summoner)
        {
            const ANativeWadPlayer* Target = Cast<ANativeWadPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
            if (!Target || !Summoner->CanSeePlayer(*Target))
            {
                State.Remaining = 0.1f;
                continue;
            }
            const FVector SummonerLocation = Summoner->GetActorLocation();
            Spawn = SummonerLocation + (Target->GetActorLocation() - SummonerLocation).GetSafeNormal2D() * 75.0;
            Sector = FindWadSector(FVector2D(Spawn - GetActorLocation()));
            if (Sector == INDEX_NONE)
            {
                State.Remaining = 0.1f;
                continue;
            }
            const double Feet = SummonerLocation.Z - Summoner->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
            const double Difference = GetActorLocation().Z + GetWalkingHeight(Sector) - Feet;
            if (Difference < -16.0 || Difference > 30.0)
            {
                State.Remaining = 0.1f;
                continue;
            }
        }
        Spawn.Z = GetActorLocation().Z + GetWalkingHeight(Sector) + Capsule->GetScaledCapsuleHalfHeight() + 2.0;
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(WadCreatureGenerator), false, Creature);
        if (GetWorld()->OverlapBlockingTestByChannel(Spawn, FQuat::Identity, ECC_Pawn,
            FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(), Capsule->GetScaledCapsuleHalfHeight()), Query))
        {
            if (Summoner)
            {
                State.Remaining = 0.1f;
            }
            continue;
        }
        Creature->SetActorLocation(Spawn, false, nullptr, ETeleportType::TeleportPhysics);
        if (Generator.bInfinite) { Creature->ResetForGenerator(); }
        Creature->SetSpawned(true);
        if (Summoner)
        {
            Summoner->ShowSummonPose();
        }
        ++State.Released;
        if (Generator.IsExhausted(State.Released))
        {
            State.Remaining = 0.0f;
        }
        UE_LOG(LogTemp, Display, TEXT("WAD generator %d released %s (%d/%s)."), Id, *Creature->GetEncounterName(),
            State.Released, Generator.bInfinite ? TEXT("unlimited") : *LexToString(Generator.Creatures.Num()));
    }
}

void ANativeWadLevel::ScheduleScriptContinuation(int32 Activity, int32 NextCommand,
    TWeakObjectPtr<ANativeWadPlayer> Player, float Delay)
{
    const uint64 Id = ++NextScriptDelay;
    FPendingScriptDelay& Waiting = PendingScriptDelays.Add(Id, {FTimerHandle(), Activity, NextCommand, Player});
    GetWorldTimerManager().SetTimer(Waiting.Handle, FTimerDelegate::CreateWeakLambda(this, [this, Id]
    {
        const FPendingScriptDelay Resume = PendingScriptDelays.FindChecked(Id);
        PendingScriptDelays.Remove(Id);
        int32 Budget = ScriptBudget;
        RunWadScriptCommands(Resume.Activity, Resume.NextCommand, Resume.Player, Budget);
    }), Delay, false);
}

void ANativeWadLevel::StartActivitySlide(int32 Sector, double Target, int32 Ticks, bool bCeiling,
    bool bPauseOnCrush, int32 CompletionActivity)
{
    FVector2D HeightsAtStart;
    if (!GetSectorSurfaceHeights(Sector, HeightsAtStart))
    {
        return;
    }
    const double From = bCeiling ? HeightsAtStart.Y : HeightsAtStart.X;
    ActivitySlides.RemoveAll([Sector, bCeiling](const FActivitySlide& Slide)
        { return Slide.Sector == Sector && Slide.bCeiling == bCeiling; });
    ActivitySlides.Add({Sector, From, Target, Ticks / TicksPerSecond, 0.0, bCeiling, bPauseOnCrush, CompletionActivity});
    SetActorTickEnabled(true);
}

void ANativeWadLevel::TickActivitySlides(float DeltaSeconds)
{
    TArray<int32, TInlineAllocator<4>> CompletedActivities;
    for (int32 Index = ActivitySlides.Num() - 1; Index >= 0; --Index)
    {
        FActivitySlide& Slide = ActivitySlides[Index];
        const double Age = FMath::Min(Slide.Elapsed + DeltaSeconds, Slide.Duration);
        const double Z = FMath::Lerp(Slide.From, Slide.To, Age / Slide.Duration);
        FVector2D Before;
        if (Slide.bPauseOnCrush && Slide.bCeiling && Slide.To < Slide.From && GetSectorSurfaceHeights(Slide.Sector, Before)
            && IsDoorObstructed(Slide.Sector, Before.Y, Z))
        {
            continue;
        }
        SetSectorHeight(Slide.Sector, Z, Slide.bCeiling);
        Slide.Elapsed = Age;
        if (Age >= Slide.Duration)
        {
            if (Slide.CompletionActivity != INDEX_NONE)
            {
                CompletedActivities.Add(Slide.CompletionActivity);
            }
            ActivitySlides.RemoveAtSwap(Index);
        }
    }
    for (int32 Activity : CompletedActivities)
    {
        RunWadActivity(Activity, Cast<ANativeWadPlayer>(UGameplayStatics::GetPlayerPawn(this, 0)));
    }
}

UStaticMeshComponent* ANativeWadLevel::FindActivitySideBand(int32 SideIndex) const
{
    if (!Sides.IsValidIndex(SideIndex))
    {
        return nullptr;
    }
    for (const EWadWallBand Candidate : {EWadWallBand::Middle, EWadWallBand::Lower, EWadWallBand::Upper})
    {
        if (UStaticMeshComponent* Mesh = Sides[SideIndex].Bands[uint8(Candidate)])
        {
            return Mesh;
        }
    }
    return nullptr;
}

void ANativeWadLevel::SetActivitySwitch(int32 SideIndex, bool bUp)
{
    if (UStaticMeshComponent* Mesh = FindActivitySideBand(SideIndex))
    {
        ActivitySwitchStates.Add(SideIndex, bUp);
        Mesh->SetMaterial(0, bUp ? ActivitySwitchUp : ActivitySwitchDown);
    }
}

void ANativeWadLevel::SetActivityWallTexture(int32 SideIndex, FName Texture)
{
    UMaterialInterface* Material = ActivityWallMaterials.FindRef(Texture);
    if (UStaticMeshComponent* Mesh = FindActivitySideBand(SideIndex); Mesh && Material)
    {
        ActivityWallTextures.Add(SideIndex, Texture);
        Mesh->SetMaterial(0, Material);
    }
}

void ANativeWadLevel::PlayActivitySound(const FWadScriptCommand& Command)
{
    const FWadSoundSample* Sample = ActivitySounds.Find(Command.Target);
    if (!Sample || Sample->UnsignedSamples.IsEmpty())
    {
        return;
    }
    USoundWaveProcedural* Wave = MakeWadSound(*Sample, false);
    const float Volume = FMath::Clamp(Command.Value / 255.0f, 0.0f, 1.0f);
    const float PlaybackVolume = NativeWadOptions::EffectiveVolume(NativeWadOptions::EAudioChannel::Effects, Volume);
    UAudioComponent* Audio = nullptr;
    if (Command.Operation == EWadScriptOperation::PlayAreaSound)
    {
        USoundAttenuation* Attenuation = NewObject<USoundAttenuation>();
        Attenuation->Attenuation.AttenuationShapeExtents = FVector::ZeroVector;
        Attenuation->Attenuation.FalloffDistance = float(Command.Ticks);
        const FVector Position = GetActorTransform().TransformPosition(FVector(Command.Position));
        Audio = UGameplayStatics::SpawnSoundAtLocation(this, Wave, Position, FRotator::ZeroRotator,
            PlaybackVolume, 1.0f, 0.0f, Attenuation);
    }
    else
    {
        Audio = UGameplayStatics::SpawnSound2D(this, Wave, PlaybackVolume);
    }
    if (Audio)
    {
        NativeWadOptions::TrackAudio(Audio, NativeWadOptions::EAudioChannel::Effects, Volume);
        FTimerHandle Handle;
        GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateUObject(Audio, &UAudioComponent::Stop),
            Wave->Duration + 0.05f, false);
        UE_LOG(LogTemp, Display, TEXT("Played WAD script sound %d."), Command.Target);
    }
}

void ANativeWadLevel::TickAmbientEmitters(float DeltaSeconds)
{
    if (AmbientEmitters.IsEmpty() || !GetWorld()->GetAudioDevice())
    {
        return;
    }
    AmbientUpdateTime += DeltaSeconds;
    if (AmbientUpdateTime < 0.1f)
    {
        return;
    }
    AmbientUpdateTime = 0.0f;
    const APawn* Listener = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!Listener)
    {
        return;
    }
    TMap<int32, float> Volumes;
    TMap<int32, int32> LoudestByGroup;
    const FVector2D XY(GetActorTransform().InverseTransformPosition(Listener->GetActorLocation()));
    for (int32 Index = 0; Index < AmbientEmitters.Num(); ++Index)
    {
        const FWadAmbientEmitter& Emitter = AmbientEmitters[Index];
        const double Distance = FVector2D::Distance(XY, FVector2D(Emitter.Position));
        if (Distance >= AmbientRadius)
        {
            continue;
        }
        const float Volume = float(1.0 - Distance / AmbientRadius);
        // Group zero is independent. Other groups play their loudest emitter for this listener.
        if (Emitter.Group != 0)
        {
            if (const int32* Previous = LoudestByGroup.Find(Emitter.Group))
            {
                if (Volumes.FindChecked(*Previous) >= Volume)
                {
                    continue;
                }
                Volumes.Remove(*Previous);
            }
            LoudestByGroup.Add(Emitter.Group, Index);
        }
        Volumes.Add(Index, Volume);
    }
    for (auto It = PlayingEmitters.CreateIterator(); It; ++It)
    {
        UAudioComponent* Component = It.Value();
        if (!IsValid(Component) || !Component->IsPlaying())
        {
            It.RemoveCurrent();
        }
        else if (!Volumes.Contains(It.Key()))
        {
            Component->Stop();
            It.RemoveCurrent();
        }
    }
    for (const auto& [Index, Volume] : Volumes)
    {
        if (UAudioComponent* Component = PlayingEmitters.FindRef(Index))
        {
            NativeWadOptions::TrackAudio(Component, NativeWadOptions::EAudioChannel::Effects, Volume);
            continue;
        }
        const FWadAmbientEmitter& Emitter = AmbientEmitters[Index];
        const FWadSoundSample* Sample = AmbientSamples.Find(Emitter.Sound);
        if (!Sample || Sample->UnsignedSamples.IsEmpty())
        {
            continue;
        }
        USoundAttenuation* Attenuation = NewObject<USoundAttenuation>();
        Attenuation->Attenuation.bAttenuate = false; // The source's horizontal linear falloff is applied above.
        Attenuation->Attenuation.bSpatialize = true;
        const FVector Position = GetActorTransform().TransformPosition(Emitter.Position);
        if (UAudioComponent* Component = UGameplayStatics::SpawnSoundAtLocation(this, MakeWadSound(*Sample, true),
                Position, FRotator::ZeroRotator,
                NativeWadOptions::EffectiveVolume(NativeWadOptions::EAudioChannel::Effects, Volume), 1.0f, 0.0f, Attenuation))
        {
            NativeWadOptions::TrackAudio(Component, NativeWadOptions::EAudioChannel::Effects, Volume);
            PlayingEmitters.Add(Index, Component);
        }
    }
}

int32 ANativeWadLevel::CountPlayingAmbientEmitters(int32 Sound) const
{
    int32 Count = 0;
    for (const auto& [Index, Component] : PlayingEmitters)
    {
        if (AmbientEmitters[Index].Sound == Sound && IsValid(Component) && Component->IsPlaying())
        {
            ++Count;
        }
    }
    return Count;
}

void ANativeWadLevel::LaunchActivityMissile(const FWadScriptCommand& Command)
{
    const NativeWadRules::FWadWandProjectile* Homing = NativeWadRules::FindHomingMissile(Command.Target);
    const EWadBoltEffect Effect = Command.Target == 4015 || Command.Target == NativeWadRules::PoisonCrossbowBolt
        ? EWadBoltEffect::Poison
        : Command.Target == 4016 ? EWadBoltEffect::Paralyze
        : Command.Target == 4012 ? EWadBoltEffect::LockDoor : EWadBoltEffect::Damage;
    const int32 Damage = Homing ? Homing->Damage
        : Command.Target == NativeWadRules::PoisonCrossbowBolt ? PoisonCrossbowDamage
        : Effect == EWadBoltEffect::Poison ? PoisonMissileDamage
        : Command.Target == NativeWadRules::AcidBall ? NativeWadRules::AcidBallDamage
        : Command.Target == 4005 ? 750 : Command.Target == 4006 ? 1000
        : Command.Target == NativeWadRules::PiercingCrossbowBolt ? NativeWadRules::ChurchBoltDamage : 100;
    UMaterialInterface* Art = ScriptMissileMaterials.FindRef(Command.Target).Get();
    if (!Art && Command.Target == 4015)
    {
        Art = ScriptMissileArt.Get();
    }
    const FVector Start = GetActorTransform().TransformPosition(FVector(Command.Position));
    const FVector Destination = GetActorTransform().TransformPosition(FVector(Command.Aim));
    const FVector Offset = Destination - Start;
    FVector Velocity = Offset.GetSafeNormal() * (Command.Value * TicksPerSecond);
    if (Homing)
    {
        // SCRIPT.C uses ServerShootBasicProjectile, which sets XY speed and Z pitch separately.
        const double Distance = Offset.Size2D();
        Velocity = Offset.GetSafeNormal2D() * (Command.Value * TicksPerSecond);
        Velocity.Z = Distance > UE_SMALL_NUMBER
            ? FMath::FloorToDouble(FMath::Clamp(40.0 * Offset.Z / Distance, -50.0, 50.0)) * TicksPerSecond
            : 0.0;
    }
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (ANativeWadBolt* Missile = GetWorld()->SpawnActor<ANativeWadBolt>(Start, Velocity.Rotation(), Spawn))
    {
        Missile->Launch(ScriptMissileMesh, Art, Velocity, Damage, Effect, this);
        if (Homing)
        {
            Missile->SetBlastRadius(Homing->Radius);
            Missile->SetDamageMask(Homing->DamageMask);
        }
        ++ScriptMissileCount;
        ++ScriptMissilesByType.FindOrAdd(Command.Target);
        UE_LOG(LogTemp, Display, TEXT("Launched WAD script missile %d from %s."), Command.Target, *Start.ToString());
    }
}
