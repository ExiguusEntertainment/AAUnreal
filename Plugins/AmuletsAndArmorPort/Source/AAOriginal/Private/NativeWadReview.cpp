#include "CoreMinimal.h"
#include "NativeWadReview.h"

#include "Components/CapsuleComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "NativeWadImport.h"
#include "NativeWadLevel.h"
#include "PhysicsEngine/BodySetup.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#endif

namespace
{
    constexpr int32 Door = 147;
    constexpr int32 FramesPerShot = 40;
}

    ASceneCapture2D* SpawnWadReviewCamera(UWorld& World, int32 ImageSize, const FVector& Position, const FRotator& Rotation)
    {
        FActorSpawnParameters SpawnParameters;
        SpawnParameters.ObjectFlags |= RF_Transient;
        ASceneCapture2D* Actor = World.SpawnActor<ASceneCapture2D>(Position, Rotation, SpawnParameters);
        if (!Actor)
        {
            return nullptr;
        }
        USceneCaptureComponent2D* Capture = Actor->GetCaptureComponent2D();
        UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(Actor);
        Target->RenderTargetFormat = RTF_RGBA16f;
        Target->InitAutoFormat(ImageSize, ImageSize);
        Target->UpdateResourceImmediate(true);
        Capture->TextureTarget = Target;
        Capture->FOVAngle = 104.0f;
        Capture->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
        Capture->bCaptureEveryFrame = false;
        Capture->bCaptureOnMovement = false;
        Capture->bAlwaysPersistRenderingState = true;
        Capture->PostProcessBlendWeight = 1.0f;
        ConfigureWadLumen(Capture->PostProcessSettings);
        Capture->PostProcessSettings.bOverride_LumenSurfaceCacheResolution = true;
        Capture->PostProcessSettings.LumenSurfaceCacheResolution = 1.0f;
        return Actor;
    }

    bool SaveWadReviewFrame(const USceneCaptureComponent2D& Capture, const FString& Output, const TCHAR* Name, int32 Size)
    {
        FlushRenderingCommands();
        FTextureRenderTargetResource* Target = Capture.TextureTarget->GameThread_GetRenderTargetResource();
        TArray<FLinearColor> Colors;
        if (!Target || !Target->ReadLinearColorPixels(Colors) || Colors.Num() != Size * Size)
        {
            return false;
        }
        TArray<FColor> Pixels;
        Pixels.Reserve(Colors.Num());
        for (FLinearColor& Color : Colors)
        {
            Color.A = 1.0f;
            Pixels.Add(Color.ToFColorSRGB());
        }
        const FString Filename = FPaths::GetBaseFilename(Output) + TEXT("-") + Name + TEXT(".png");
        const FString Path = FPaths::GetPath(Output) / Filename;
        TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(Size, Size, Pixels, Png);
        if (Png.IsEmpty() || !FFileHelper::SaveArrayToFile(Png, *Path))
        {
            return false;
        }
        UE_LOG(LogTemp, Display, TEXT("Saved L30 review frame: %s"), *Path);
        return true;
    }

namespace
{
    ASceneCapture2D* SpawnL30ReviewCamera(UWorld& World, int32 ImageSize)
    {
        const FVector Position(2745,1516,74);
        return SpawnWadReviewCamera(World, ImageSize, Position, (FVector(2660,1516,65) - Position).Rotation());
    }

    bool ValidateReopenedLevel(UWorld& World, ANativeWadLevel& Level)
    {
        const auto& Sectors = Level.GetSectors();
        if (Sectors.Num() != 179 || Level.GetLineCount() != 1186 || Sectors[13].Light != 255 || Sectors[14].Light != 50)
        {
            UE_LOG(LogTemp, Error, TEXT("Saved L30 is missing its expected sector, line, or light records."));
            return false;
        }
        for (const FAAWadNativeSector& Sector : Sectors)
        {
            for (const UStaticMeshComponent* Mesh : {Sector.Floor.Get(), Sector.Ceiling.Get()})
            {
                if (!Mesh || !Mesh->IsRegistered() || !Mesh->GetStaticMesh() || Mesh->GetStaticMesh()->GetOutermost() == GetTransientPackage()
                    || Mesh->Mobility != EComponentMobility::Movable)
                {
                    UE_LOG(LogTemp, Error, TEXT("Saved L30 contains an invalid floor or ceiling mesh."));
                    return false;
                }
            }
        }

        const FCollisionQueryParams Query(SCENE_QUERY_STAT(L30NativeSectorMotion), true);
        const auto CheckPassage = [&](double Z, float HalfHeight, bool bExpectedBlocked)
        {
            const FVector West(2668, 1516, Z);
            const FVector East(2744, 1516, Z);
            const FCollisionShape Capsule = FCollisionShape::MakeCapsule(6.0f, HalfHeight);
            FHitResult FromWest;
            FHitResult FromEast;
            const bool bWest = World.SweepSingleByChannel(FromWest, West, East, FQuat::Identity, ECC_Pawn, Capsule, Query);
            const bool bEast = World.SweepSingleByChannel(FromEast, East, West, FQuat::Identity, ECC_Pawn, Capsule, Query);
            if (bWest != bExpectedBlocked || bEast != bExpectedBlocked)
            {
                UE_LOG(LogTemp, Warning, TEXT("Capsule at Z %.2f with half height %.2f expected blocked=%d, got %d (%s) from west and %d (%s) from east."),
                    Z, HalfHeight, bExpectedBlocked, bWest, *GetNameSafe(FromWest.GetComponent()), bEast, *GetNameSafe(FromEast.GetComponent()));
                return false;
            }
            return true;
        };
        const auto CheckHeights = [&](double Floor, double Ceiling)
        {
            FVector2D Heights;
            return Level.GetSectorSurfaceHeights(Door, Heights) && Heights.Equals(FVector2D(Floor, Ceiling), 0.01);
        };
        const auto CheckSurface = [&](double From, double To, double ExpectedZ, const UStaticMeshComponent* Mesh)
        {
            FHitResult Hit;
            if (!World.LineTraceSingleByChannel(Hit, FVector(2700,1516,From), FVector(2700,1516,To), ECC_Pawn, Query)
                || Hit.GetComponent() != Mesh || !FMath::IsNearlyEqual(Hit.ImpactPoint.Z, ExpectedZ, 0.1))
            {
                const UBodySetup* Body = Mesh->GetStaticMesh()->GetBodySetup();
                UE_LOG(LogTemp, Warning, TEXT("Expected %s collision at Z %.2f, got %s at Z %.2f. Physics state=%d, mode=%d, hulls=%d, extent=%s."),
                    *GetNameSafe(Mesh), ExpectedZ, *GetNameSafe(Hit.GetComponent()), Hit.ImpactPoint.Z,
                    Mesh->IsPhysicsStateCreated(), int32(Mesh->GetCollisionEnabled()), Body ? Body->AggGeom.ConvexElems.Num() : 0,
                    *Mesh->Bounds.GetBox().ToString());
                return false;
            }
            return true;
        };
        const bool bClosed = CheckHeights(32, 32) && CheckPassage(55, 22, true);
        Level.SetSectorCeilingZ(Door, 75.2f);
        const bool bPartial = CheckHeights(32, 75.2) && CheckPassage(55, 22, true) && CheckPassage(45, 12, false)
            && CheckSurface(52, 100, 75.2, Sectors[Door].Ceiling);
        Level.SetSectorCeilingZ(Door, 128.0f);
        const bool bOpen = CheckHeights(32, 128) && CheckPassage(55, 22, false);
        Level.SetSectorFloorZ(Door, 56.5f);
        const bool bRaisedFloor = CheckHeights(56.5, 128) && CheckPassage(45, 12, true) && CheckPassage(80, 22, false)
            && CheckSurface(90, 40, 56.5, Sectors[Door].Floor);
        Level.SetSectorFloorZ(Door, 32.0f);
        Level.SetSectorCeilingZ(Door, 32.0f);
        const bool bReclosed = CheckHeights(32, 32) && CheckPassage(55, 22, true);
        const UStaticMeshComponent& DoorFloor = *Sectors[Door].Floor;
        const UStaticMeshComponent& AdjacentFloor = *Sectors[146].Floor;
        const TArray<float> AdjacentBefore = AdjacentFloor.GetCustomPrimitiveData().Data;
        const TArray<float> DoorBefore = DoorFloor.GetCustomPrimitiveData().Data;
        const bool bApplied = Level.SetSectorTextureScrollTexels(Door, false, FVector2D(17.0, 11.0))
            && Level.SetSectorLightLevel(Door, 100);
        const TArray<float>& Updated = DoorFloor.GetCustomPrimitiveData().Data;
        const bool bSurfaceControls = bApplied && DoorBefore.IsValidIndex(4) && Updated.IsValidIndex(4)
            && FMath::IsNearlyEqual(Updated[2], -17.0f / 64.0f) && FMath::IsNearlyEqual(Updated[3], -11.0f / 64.0f)
            && Updated[4] > DoorBefore[4] && AdjacentFloor.GetCustomPrimitiveData().Data == AdjacentBefore;
        Level.SetSectorLightLevel(Door, 50);
        Level.SetSectorTextureScrollTexels(Door, false, FVector2D::ZeroVector);
        const bool bPassed = bClosed && bPartial && bOpen && bRaisedFloor && bReclosed && bSurfaceControls;
        UE_LOG(LogTemp, Display, TEXT("Reopened L30 movement and collision: closed=%d partial=%d open=%d raised floor=%d reclosed=%d."),
            bClosed, bPartial, bOpen, bRaisedFloor, bReclosed);
        UE_LOG(LogTemp, Display, TEXT("Reopened L30 independent surface scroll and light adjustment: %d."), bSurfaceControls);
        return bPassed;
    }

    struct FNativeL30Capture
    {
        TWeakObjectPtr<ANativeWadLevel> Level;
        TWeakObjectPtr<ASceneCapture2D> Camera;
        FString OutputPath;
        int32 Size = 0;
        int32 Shot = 0;
        int32 Frames = 0;

        bool Finish(bool bSucceeded)
        {
            if (ANativeWadLevel* Map = Level.Get())
            {
                Map->SetSectorCeilingZ(Door, 32.0f);
            }
            if (ASceneCapture2D* Actor = Camera.Get())
            {
                Actor->Destroy();
            }
            if (!bSucceeded)
            {
                UE_LOG(LogTemp, Error, TEXT("L30 Lumen image export failed."));
            }
            UE_LOG(LogTemp, Display, TEXT("Native L30 review finished"));
            return false;
        }

        bool Tick()
        {
            ANativeWadLevel* Map = Level.Get();
            ASceneCapture2D* Actor = Camera.Get();
            if (!Map || !Actor)
            {
                return Finish(false);
            }
            USceneCaptureComponent2D* Capture = Actor->GetCaptureComponent2D();
            Capture->CaptureScene();
            if (++Frames < FramesPerShot)
            {
                return true;
            }
            static const TCHAR* ShotNames[] = {TEXT("closed"), TEXT("open"), TEXT("open-gi-off")};
            if (!SaveWadReviewFrame(*Capture, OutputPath, ShotNames[Shot], Size))
            {
                return Finish(false);
            }
            Frames = 0;
            switch (++Shot)
            {
            case 1:
                Map->SetSectorCeilingZ(Door, 128.0f);
                break;
            case 2:
                Capture->PostProcessSettings.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
                Capture->PostProcessSettings.ReflectionMethod = EReflectionMethod::None;
                Capture->bCameraCutThisFrame = true;
                break;
            default:
                return Finish(true);
            }
            return true;
        }
    };

    bool CheckL30Doors(const ANativeWadLevel& Level)
    {
        const int32 Expected[][4] = {
            { 464, 144, 8, 104 }, { 465, 145, 60, 176 }, { 466, 147, 32, 112 },
            { 467, 155, 32, 128 }, { 468, 149, 32, 112 }, { 469, 151, 32, 128 }
        };
        const auto& Doors = Level.GetDoors();
        if (Doors.Num() != UE_ARRAY_COUNT(Expected))
        {
            return false;
        }
        for (int32 Index = 0; Index < Doors.Num(); ++Index)
        {
            const FAAWadNativeDoor& Entry = Doors[Index];
            const int32* Values = Expected[Index];
            FVector2D Heights;
            if (Entry.SourceThing != Values[0] || Entry.Sector != Values[1] || Entry.ClosedZ != Values[2]
                || Entry.OpenZ != Values[3] || Entry.TravelSeconds != 1.5f || Entry.HoldSeconds != 2.0f || Entry.bRequiresUnlock
                || !Level.GetSectorSurfaceHeights(Entry.Sector, Heights) || !Heights.Equals(FVector2D(Entry.ClosedZ, Entry.ClosedZ)))
            {
                return false;
            }
        }
        return true;
    }

    struct FNativeL30DoorCapture
    {
        enum class EStage { Preparing, Opening, ObstructedClose, Reopening, Closing };

        TWeakObjectPtr<ANativeWadLevel> Level;
        TWeakObjectPtr<ASceneCapture2D> Camera;
        TWeakObjectPtr<AActor> Obstacle;
        FString OutputPath;
        int32 Size = 0;
        int32 WarmupFrames = 0;
        EStage Stage = EStage::Preparing;
        double ActivatedAt = 0.0;
        double OpenedAt = 0.0;
        double PreviousZ = 32.0;
        bool bOpeningFrameSaved = false;
        bool bObservedClosing = false;

        bool Finish(const TCHAR* Failure = nullptr)
        {
            if (AActor* Actor = Obstacle.Get())
            {
                Actor->Destroy();
            }
            if (ASceneCapture2D* Actor = Camera.Get())
            {
                Actor->Destroy();
            }
            if (Failure)
            {
                UE_LOG(LogTemp, Error, TEXT("L30 door review failed: %s"), Failure);
            }
            UE_LOG(LogTemp, Display, TEXT("Native L30 door review finished"));
            return false;
        }

        bool PassageIsBlocked(const UWorld& World) const
        {
            const FCollisionShape StandingCapsule = FCollisionShape::MakeCapsule(6.0f, 22.0f);
            return World.SweepTestByChannel(FVector(2668,1516,55), FVector(2744,1516,55), FQuat::Identity,
                ECC_Pawn, StandingCapsule, FCollisionQueryParams(SCENE_QUERY_STAT(NativeL30DoorPassage), true));
        }

        AActor* PlaceObstruction(UWorld& World)
        {
            FActorSpawnParameters Params;
            Params.ObjectFlags = RF_Transient;
            AActor* Actor = World.SpawnActor<AActor>(Params);
            if (Actor)
            {
                UCapsuleComponent* Capsule = NewObject<UCapsuleComponent>(Actor);
                Capsule->SetCapsuleSize(6.0f, 16.0f);
                Capsule->SetCollisionProfileName(TEXT("Pawn"));
                Actor->SetRootComponent(Capsule);
                Capsule->SetWorldLocation(FVector(2700,1516,52));
                Capsule->RegisterComponent();
            }
            return Actor;
        }

        bool Tick()
        {
            ANativeWadLevel* Map = Level.Get();
            ASceneCapture2D* Actor = Camera.Get();
            FVector2D Heights;
            FVector2D OtherDoor;
            if (!Map || !Actor || !Map->GetSectorSurfaceHeights(Door, Heights) || Heights.X != 32.0
                || !Map->GetSectorSurfaceHeights(145, OtherDoor) || !OtherDoor.Equals(FVector2D(60.0, 60.0)))
            {
                return Finish(TEXT("door motion changed the floor or an unrelated door"));
            }
            UWorld& World = *Map->GetWorld();
            const double Now = World.GetTimeSeconds();
            if (Stage != EStage::Preparing && Now - ActivatedAt > 14.0)
            {
                return Finish(TEXT("timed out waiting for the door cycle"));
            }
            USceneCaptureComponent2D& Capture = *Actor->GetCaptureComponent2D();
            Capture.CaptureScene();
            switch (Stage)
            {
            case EStage::Preparing:
                if (++WarmupFrames >= FramesPerShot)
                {
                    if (Map->UseDoor(165) || !PassageIsBlocked(World) || !SaveWadReviewFrame(Capture, OutputPath, TEXT("closed"), Size)
                        || !Map->UseDoor(Door))
                    {
                        return Finish(TEXT("closed door activation or initial collision did not match the source"));
                    }
                    ActivatedAt = Now;
                    Stage = EStage::Opening;
                }
                break;
            case EStage::Opening:
                if (!bOpeningFrameSaved && Heights.Y >= 72.0 && Heights.Y < 112.0)
                {
                    bOpeningFrameSaved = SaveWadReviewFrame(Capture, OutputPath, TEXT("opening"), Size);
                }
                if (Heights.Y >= 112.0)
                {
                    if (!bOpeningFrameSaved || !FMath::IsNearlyEqual(Now - ActivatedAt, 1.5, 0.15) || PassageIsBlocked(World)
                        || !SaveWadReviewFrame(Capture, OutputPath, TEXT("open"), Size))
                    {
                        return Finish(TEXT("open height, travel time, or passage differed from the expected result"));
                    }
                    Obstacle = PlaceObstruction(World);
                    if (!Obstacle.IsValid())
                    {
                        return Finish(TEXT("could not place the obstruction test capsule"));
                    }
                    OpenedAt = Now;
                    Stage = EStage::ObstructedClose;
                    UE_LOG(LogTemp, Display, TEXT("Door 147 opened from 32 to 112 in %.3f seconds and cleared the capsule."), Now - ActivatedAt);
                }
                break;
            case EStage::ObstructedClose:
                if (!bObservedClosing && Heights.Y < 112.0)
                {
                    bObservedClosing = true;
                    if (!FMath::IsNearlyEqual(Now - OpenedAt, 2.0, 0.15))
                    {
                        return Finish(TEXT("the initial open hold did not last two seconds"));
                    }
                }
                if (Heights.Y < 67.9)
                {
                    return Finish(TEXT("the closing ceiling passed into the obstruction"));
                }
                if (bObservedClosing && Heights.Y > PreviousZ)
                {
                    Obstacle->Destroy();
                    Obstacle.Reset();
                    Stage = EStage::Reopening;
                    UE_LOG(LogTemp, Display, TEXT("Door 147 reversed at ceiling Z %.2f above the obstruction."), PreviousZ);
                }
                break;
            case EStage::Reopening:
                if (Heights.Y >= 112.0)
                {
                    OpenedAt = Now;
                    Stage = EStage::Closing;
                }
                break;
            case EStage::Closing:
                if (Heights.Y <= 32.0)
                {
                    if (!FMath::IsNearlyEqual(Now - OpenedAt, 3.5, 0.15) || !PassageIsBlocked(World)
                        || !SaveWadReviewFrame(Capture, OutputPath, TEXT("reclosed"), Size))
                    {
                        return Finish(TEXT("the unobstructed closing cycle did not restore the original collision"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Door 147 held then reclosed in %.3f seconds. Saved door cycle passed."), Now - OpenedAt);
                    return Finish();
                }
                break;
            }
            PreviousZ = Heights.Y;
            return true;
        }
    };
}

bool StartNativeL30Review(UWorld* World, const FString& OutputPath, int32 ImageSize)
{
    if (!World || ImageSize < 256 || ImageSize > 2048)
    {
        return false;
    }
    TActorIterator<ANativeWadLevel> Imported(World);
    if (!Imported || Imported->GetSectors().Num() != 179)
    {
        return false;
    }
#if WITH_EDITOR
    // Loading a map in the editor can leave Static Mesh collision waiting on asynchronous compilation.
    FAssetCompilingManager::Get().FinishAllCompilation();
#endif
    if (!ValidateReopenedLevel(*World, **Imported))
    {
        UE_LOG(LogTemp, Error, TEXT("Saved L30 did not pass the movement and material checks."));
    }
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
    ASceneCapture2D* Actor = SpawnL30ReviewCamera(*World, ImageSize);
    if (!Actor)
    {
        return false;
    }
    const TSharedRef<FNativeL30Capture> Review = MakeShared<FNativeL30Capture>();
    Review->Level = *Imported;
    Review->Camera = Actor;
    Review->OutputPath = OutputPath;
    Review->Size = ImageSize;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float) { return Review->Tick(); }));
    return true;
}

bool StartNativeL30DoorReview(UWorld* World, const FString& OutputPath, int32 ImageSize)
{
    if (!World || !World->IsGameWorld() || ImageSize < 256 || ImageSize > 2048)
    {
        return false;
    }
    TActorIterator<ANativeWadLevel> Imported(World);
    if (!Imported || !CheckL30Doors(**Imported))
    {
        return false;
    }
#if WITH_EDITOR
    FAssetCompilingManager::Get().FinishAllCompilation();
#endif
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
    ASceneCapture2D* Camera = SpawnL30ReviewCamera(*World, ImageSize);
    if (!Camera)
    {
        return false;
    }
    UE_LOG(LogTemp, Display, TEXT("Verified all six source-authored doors from the saved L30 level."));
    const TSharedRef<FNativeL30DoorCapture> Review = MakeShared<FNativeL30DoorCapture>();
    Review->Level = *Imported;
    Review->Camera = Camera;
    Review->OutputPath = OutputPath;
    Review->Size = ImageSize;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float) { return Review->Tick(); }));
    return true;
}
