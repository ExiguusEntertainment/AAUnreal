#include "NativeWadScenery.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadLevel.h"
#include "NativeWadRules.h"

ANativeWadScenery::ANativeWadScenery()
{
    PrimaryActorTick.bCanEverTick = true;
    SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Anchor")));

    Obstacle = CreateDefaultSubobject<UBoxComponent>(TEXT("Obstacle"));
    Obstacle->SetupAttachment(GetRootComponent());
    Obstacle->SetAbsolute(false, true, false);
    Obstacle->SetCollisionResponseToAllChannels(ECR_Ignore);
    Obstacle->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
    Obstacle->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
    Obstacle->SetGenerateOverlapEvents(false);
    Obstacle->SetCanEverAffectNavigation(false);

    Sprite = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Sprite"));
    Sprite->SetupAttachment(GetRootComponent());
    Sprite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Sprite->SetCastShadow(false);
    Sprite->SetCanEverAffectNavigation(false);
    Sprite->SetAffectDistanceFieldLighting(false);
}

void ANativeWadScenery::Configure(ANativeWadLevel& Level, UStaticMesh* Plane, const FWadSceneryDefinition& InDefinition,
    int32 InSector, int32 InThingId)
{
    WadLevel = &Level;
    Definition = InDefinition;
    Sector = InSector;
    ThingId = InThingId;

    const double HalfHeight = Definition.Frames[0].Views[0].Size.Y * 0.5;
    Obstacle->SetBoxExtent(FVector(Definition.Radius, Definition.Radius, HalfHeight));
    Obstacle->SetRelativeLocation(FVector(0.0, 0.0, HalfHeight));
    Obstacle->SetWorldRotation(FRotator::ZeroRotator);
    Obstacle->SetCollisionEnabled(Definition.bPassable ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);

    Sprite->SetStaticMesh(Plane);
    Sprite->SetRelativeRotation(FRotator(0, 90, 90));
    SetView(0, 0);
    UpdateElevation();
}

void ANativeWadScenery::BeginPlay()
{
    Super::BeginPlay();
    Random.Initialize(ThingId);
}

uint16 ANativeWadScenery::GetSourceAngle() const
{
    return uint16(FMath::RoundToInt(GetActorRotation().Yaw * (65536.0 / 360.0)));
}

void ANativeWadScenery::SetSourceAngle(uint16 Angle)
{
    SetActorRotation(FRotator(0, Angle * (360.0 / 65536.0), 0));
}

void ANativeWadScenery::UpdateElevation()
{
    FVector2D Heights;
    if (!WadLevel || !WadLevel->GetSectorSurfaceHeights(Sector, Heights))
    {
        return;
    }
    const double Height = Definition.Frames[0].Views[0].Size.Y;
    const double Floor = Definition.bStaysAboveLiquid ? Heights.X : WadLevel->GetWalkingHeight(Sector);
    const double Bottom = Definition.bHanging ? Heights.Y - Height
        : FMath::Max(Floor, FMath::Min(Floor + (Definition.bRaised ? 32.0 : 0.0), Heights.Y - Height));
    if (!FMath::IsNearlyEqual(GetActorLocation().Z, Bottom))
    {
        FVector Location = GetActorLocation();
        Location.Z = Bottom;
        SetActorLocation(Location);
    }
}

void ANativeWadScenery::SetView(int32 Frame, int32 Angle)
{
    const FWadSceneryView& View = Definition.Frames[Frame].Views[Angle];
    const double Width = View.bMirrored ? -View.Size.X : View.Size.X;
    Sprite->SetMaterial(0, View.Material);
    Sprite->SetRelativeScale3D(FVector(Width * 0.01, View.Size.Y * 0.01, 1.0));
    Sprite->SetRelativeLocation(FVector(0.0, 0.0, View.Size.Y * 0.5));
    CurrentFrame = Frame;
    CurrentAngle = Angle;
}

void ANativeWadScenery::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (Definition.Frames.IsEmpty())
    {
        return;
    }

    UpdateElevation();
    const APlayerController* Controller = GetWorld()->GetFirstPlayerController();
    const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
    if (!Pawn)
    {
        return;
    }

    int32 Frame = FMath::Max(0, CurrentFrame);
    const int32 FrameCount = Definition.Frames.Num();
    if (FrameCount > 1 && Definition.FrameTicks > 0)
    {
        FrameTime += DeltaSeconds;
        const double Interval = Definition.FrameTicks / double(NativeWadRules::TicksPerSecond);
        while (FrameTime >= Interval)
        {
            FrameTime -= Interval;
            switch (Definition.AnimationType)
            {
            case 1: // Bounce at both endpoints.
            {
                const int32 Period = 2 * (FrameCount - 1);
                AnimationStep = (AnimationStep + 1) % Period;
                Frame = FMath::Min(AnimationStep, Period - AnimationStep);
                break;
            }
            case 2:
                Frame = Random.RandRange(0, FrameCount - 1);
                break;
            default:
                Frame = (Frame + 1) % FrameCount;
                break;
            }
        }
    }

    const FVector Facing = (Pawn->GetPawnViewLocation() - GetActorLocation()).GetSafeNormal2D();
    if (Facing.IsNearlyZero())
    {
        return;
    }
    const FVector Right = FVector::CrossProduct(FVector::UpVector, Facing);
    Sprite->SetWorldRotation(FRotationMatrix::MakeFromXY(Right, -FVector::UpVector).Rotator());
    const int32 Views = Definition.Frames[Frame].Views.Num();
    const double Step = 360.0 / Views;
    const double RelativeYaw = FRotator::ClampAxis(GetActorRotation().Yaw - Facing.Rotation().Yaw);
    const int32 Angle = FMath::RoundToInt(RelativeYaw / Step) % Views;
    if (Frame != CurrentFrame || Angle != CurrentAngle)
    {
        SetView(Frame, Angle);
    }
}
