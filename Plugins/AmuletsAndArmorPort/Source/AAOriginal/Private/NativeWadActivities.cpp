#include "NativeWadLevel.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "NativeWadPlayer.h"
#include "TimerManager.h"

namespace
{
    constexpr float TicksPerSecond = 70.0f;
    constexpr double UseReach = 64.0;
    constexpr int32 ScriptBudget = 512;
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

void ANativeWadLevel::CheckActivitySector(const FVector2D& Position, int32& PreviousSector)
{
    if (!HasWadActivities())
    {
        return;
    }
    const FVector2D Local(GetActorTransform().InverseTransformPosition(FVector(Position, GetActorLocation().Z)));
    for (const int32 Sector : ActivitySectors)
    {
        if (ContainsSectorPoint(Sector, Local))
        {
            if (PreviousSector != Sector)
            {
                PreviousSector = Sector;
                RunWadActivity(Sectors[Sector].Activity);
            }
            return;
        }
    }
    PreviousSector = INDEX_NONE;
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
            GetWorldTimerManager().SetTimer(Handle,
                FTimerDelegate::CreateWeakLambda(this, [this, Activity, Index, Player]
                {
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
            Mesh->SetMaterial(0, bUp ? ActivitySwitchUp : ActivitySwitchDown);
            return;
        }
    }
}
