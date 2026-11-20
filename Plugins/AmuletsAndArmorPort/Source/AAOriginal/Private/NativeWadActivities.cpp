#include "NativeWadLevel.h"

#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadEncounter.h"
#include "NativeWadPlayer.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWaveProcedural.h"
#include "TimerManager.h"

namespace
{
    constexpr float TicksPerSecond = 70.0f;
    constexpr double UseReach = 64.0;
    constexpr int32 ScriptBudget = 512;
    constexpr int32 PoisonMissileDamage = 250;
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
    const int32 Sector = FindActivitySector(FVector2D(Player.GetActorLocation()));
    if (Sector != INDEX_NONE && PreviousSector != Sector)
    {
        RunWadActivity(Sectors[Sector].Activity, &Player);
    }
    PreviousSector = Sector;
}

int32 ANativeWadLevel::FindActivitySector(const FVector2D& Position) const
{
    const FVector2D Local(GetActorTransform().InverseTransformPosition(FVector(Position, GetActorLocation().Z)));
    for (const int32 Sector : ActivitySectors)
    {
        if (ContainsSectorPoint(Sector, Local))
        {
            return Sector;
        }
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
        case EWadScriptOperation::Wait:
        {
            FTimerHandle Handle;
            ++PendingScriptDelays;
            GetWorldTimerManager().SetTimer(Handle,
                FTimerDelegate::CreateWeakLambda(this, [this, Activity, Index, Player]
                {
                    --PendingScriptDelays;
                    int32 ResumeBudget = ScriptBudget;
                    RunWadScriptCommands(Activity, Index + 1, Player, ResumeBudget);
                }), Command.Ticks / TicksPerSecond, false);
            return;
        }
        case EWadScriptOperation::Message:
            if (ANativeWadPlayer* Pawn = Player.Get())
            {
                Pawn->ShowInteractionMessage(Command.Text);
            }
            break;
        case EWadScriptOperation::SlideFloor:
        case EWadScriptOperation::SlideCeiling:
            StartActivitySlide(Command.Target, Command.Value, Command.Ticks,
                Command.Operation == EWadScriptOperation::SlideCeiling, Command.bPauseOnCrush);
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

void ANativeWadLevel::StartActivitySlide(int32 Sector, double Target, int32 Ticks, bool bCeiling, bool bPauseOnCrush)
{
    FVector2D HeightsAtStart;
    if (!GetSectorSurfaceHeights(Sector, HeightsAtStart))
    {
        return;
    }
    const double From = bCeiling ? HeightsAtStart.Y : HeightsAtStart.X;
    ActivitySlides.RemoveAll([Sector, bCeiling](const FActivitySlide& Slide)
        { return Slide.Sector == Sector && Slide.bCeiling == bCeiling; });
    ActivitySlides.Add({Sector, From, Target, Ticks / TicksPerSecond, 0.0, bCeiling, bPauseOnCrush});
    SetActorTickEnabled(true);
}

void ANativeWadLevel::TickActivitySlides(float DeltaSeconds)
{
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
            ActivitySlides.RemoveAtSwap(Index);
        }
    }
}

void ANativeWadLevel::SetActivitySwitch(int32 SideIndex, bool bUp)
{
    if (!Sides.IsValidIndex(SideIndex))
    {
        return;
    }
    for (const EWadWallBand Band : {EWadWallBand::Middle, EWadWallBand::Lower, EWadWallBand::Upper})
    {
        UStaticMeshComponent* Mesh = Sides[SideIndex].Bands[uint8(Band)];
        if (Mesh)
        {
            ActivitySwitchStates.Add(SideIndex, bUp);
            Mesh->SetMaterial(0, bUp ? ActivitySwitchUp : ActivitySwitchDown);
            return;
        }
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
    UAudioComponent* Audio = nullptr;
    if (Command.Operation == EWadScriptOperation::PlayAreaSound)
    {
        USoundAttenuation* Attenuation = NewObject<USoundAttenuation>();
        Attenuation->Attenuation.AttenuationShapeExtents = FVector::ZeroVector;
        Attenuation->Attenuation.FalloffDistance = float(Command.Ticks);
        const FVector Position = GetActorTransform().TransformPosition(FVector(Command.Position));
        Audio = UGameplayStatics::SpawnSoundAtLocation(this, Wave, Position, FRotator::ZeroRotator,
            Volume, 1.0f, 0.0f, Attenuation);
    }
    else
    {
        Audio = UGameplayStatics::SpawnSound2D(this, Wave, Volume);
    }
    if (Audio)
    {
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
            Component->SetVolumeMultiplier(Volume);
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
                Position, FRotator::ZeroRotator, Volume, 1.0f, 0.0f, Attenuation))
        {
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
    const FVector Start = GetActorTransform().TransformPosition(FVector(Command.Position));
    const FVector Destination = GetActorTransform().TransformPosition(FVector(Command.Aim));
    const FVector Direction = (Destination - Start).GetSafeNormal();
    FActorSpawnParameters Spawn;
    Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (ANativeWadBolt* Missile = GetWorld()->SpawnActor<ANativeWadBolt>(Start, Direction.Rotation(), Spawn))
    {
        Missile->Launch(ScriptMissileMesh, ScriptMissileArt, Direction * (Command.Value * TicksPerSecond),
            PoisonMissileDamage, this);
        ++ScriptMissileCount;
        UE_LOG(LogTemp, Display, TEXT("Launched WAD script missile %d from %s."), Command.Target, *Start.ToString());
    }
}
