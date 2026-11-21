#include "NativeWadScenery.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "NativeWadBillboard.h"
#include "NativeWadLevel.h"
#include "NativeWadRules.h"

ANativeWadScenery::ANativeWadScenery()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;
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
    Sprite->SetAbsolute(false, true, false);
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

    Sprite->SetStaticMesh(Plane);
    Sprite->SetWorldRotation(FRotator(0, 90, 90));
    InitializeMount();
}

void ANativeWadScenery::BeginPlay()
{
    Super::BeginPlay();
    Random.Initialize(ThingId);
    if (!Definition.Frames.IsEmpty())
    {
        InitializeMount();
    }
}

uint16 ANativeWadScenery::GetSourceAngle() const
{
    return uint16(FMath::RoundToInt(GetActorRotation().Yaw * (65536.0 / 360.0)));
}

void ANativeWadScenery::SetSourceAngle(uint16 Angle)
{
    SetActorRotation(FRotator(0, Angle * (360.0 / 65536.0), 0));
}

void ANativeWadScenery::InitializeMount()
{
    const double HalfHeight = Definition.Frames[0].Views[0].Size.Y * 0.5;
    Obstacle->SetBoxExtent(FVector(Definition.Radius, Definition.Radius, HalfHeight));
    Obstacle->SetRelativeLocation(FVector(0, 0, Definition.bHanging ? -HalfHeight : HalfHeight));
    Obstacle->SetWorldRotation(FRotator::ZeroRotator);
    Obstacle->SetCollisionEnabled(Definition.bPassable ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryOnly);
    Sprite->SetAbsolute(false, true, false);
    SetView(0, 0);
    UpdateElevation();
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
    const double Mount = Definition.bHanging ? Heights.Y
        : FMath::Max(Floor, FMath::Min(Floor + (Definition.bRaised ? 32.0 : 0.0), Heights.Y - Height));
    const double WorldZ = WadLevel->GetActorLocation().Z + Mount;
    if (!FMath::IsNearlyEqual(GetActorLocation().Z, WorldZ))
    {
        FVector Location = GetActorLocation();
        Location.Z = WorldZ;
        SetActorLocation(Location);
    }
}

void ANativeWadScenery::SetView(int32 Frame, int32 Angle)
{
    const FWadSceneryView& View = Definition.Frames[Frame].Views[Angle];
    const double Width = View.bMirrored ? -View.Size.X : View.Size.X;
    Sprite->SetMaterial(0, View.Material);
    Sprite->SetRelativeScale3D(FVector(Width * 0.01, View.Size.Y * 0.01, 1.0));
    const double HalfHeight = View.Size.Y * 0.5;
    Sprite->SetRelativeLocation(FVector(0, 0, Definition.bHanging ? -HalfHeight : HalfHeight));
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
    if (!Controller || !Controller->GetViewTarget())
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

    const FVector Facing = NativeWadBillboard::FaceView(*Sprite, *Controller);
    if (Facing.IsNearlyZero())
    {
        return;
    }
    const int32 Views = Definition.Frames[Frame].Views.Num();
    const double Step = 360.0 / Views;
    const double RelativeYaw = FRotator::ClampAxis(GetActorRotation().Yaw - Facing.Rotation().Yaw);
    const int32 Angle = FMath::RoundToInt(RelativeYaw / Step) % Views;
    if (Frame != CurrentFrame || Angle != CurrentAngle)
    {
        SetView(Frame, Angle);
    }
}
