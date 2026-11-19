#include "CoreMinimal.h"
#include "NativeWadReview.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/SceneCapture2D.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#endif

namespace
{
    constexpr int32 Door = 147;
    constexpr int32 DarkApproach = 14;

    struct FLightingShot
    {
        const TCHAR* Name;
        int32 Input;
        int32 ApproachLight;
        int32 DoorLight;
        bool bLumen = true;
    };

    constexpr FLightingShot L30Shots[] = {
        {TEXT("level-50"), 50, 50, 138}, {TEXT("level-150"), 150, 150, 138}, {TEXT("level-255"), 255, 255, 138},
        {TEXT("level-150-lumen-off"), 150, 150, 138, false},
        {TEXT("rule-closed"), 32, 84, 0}, {TEXT("rule-halfway"), 72, 119, 63},
        {TEXT("rule-open"), 112, 160, 138}, {TEXT("rule-reclosed"), 32, 84, 0},
    };

    constexpr FLightingShot L151Shots[] = {
        {TEXT("day"), 255, 255, 0}, {TEXT("dusk"), 90, 90, 0},
    };

    constexpr FLightingShot L151ClockShots[] = {
        {TEXT("noon"), 12, 255, 0}, {TEXT("midnight"), 0, 35, 0},
    };

    struct FLightingReview
    {
        TWeakObjectPtr<ANativeWadLevel> Level;
        TWeakObjectPtr<ASceneCapture2D> Camera;
        TConstArrayView<FLightingShot> Shots;
        FString Output;
        double InitialCeiling = 0.0;
        double InitialHour = 0.0;
        int32 InitialOutside = 255;
        int32 Size = 0;
        int32 Frame = 0;
        int32 Index = 0;
        bool bOutdoor = false;
        bool bDayClock = false;

        bool Finish(bool bSuccess)
        {
            if (ANativeWadLevel* Map = Level.Get())
            {
                if (Map->HasDaySky() && bOutdoor)
                {
                    Map->SetTimeOfDayHours(InitialHour);
                }
                else if (bOutdoor)
                {
                    Map->SetOutsideLightLevel(InitialOutside);
                }
                else
                {
                    Map->ClearSectorLightOverride(DarkApproach);
                    Map->SetSectorCeilingZ(Door, InitialCeiling);
                }
            }
            if (ASceneCapture2D* Capture = Camera.Get())
            {
                Capture->Destroy();
            }
            if (!bSuccess)
            {
                UE_LOG(LogTemp, Error, TEXT("Native WAD lighting review failed on frame set %d."), Index);
            }
            UE_LOG(LogTemp, Display, TEXT("%s"), bDayClock
                ? TEXT("Native WAD port review finished") : TEXT("Native WAD lighting review finished"));
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
            const FLightingShot& Shot = Shots[Index];
            USceneCaptureComponent2D& Capture = *Actor->GetCaptureComponent2D();
            if (Frame == 0)
            {
                Capture.PostProcessSettings.DynamicGlobalIlluminationMethod = Shot.bLumen
                    ? EDynamicGlobalIlluminationMethod::Lumen : EDynamicGlobalIlluminationMethod::None;
                Capture.PostProcessSettings.ReflectionMethod = Shot.bLumen ? EReflectionMethod::Lumen : EReflectionMethod::None;
                Capture.bCameraCutThisFrame = true;
                if (bDayClock)
                {
                    Map->SetTimeOfDayHours(Shot.Input);
                }
                else if (bOutdoor)
                {
                    Map->SetOutsideLightLevel(Shot.Input);
                }
                else if (Index < 4)
                {
                    Map->SetSectorCeilingZ(Door, 112.0f);
                    Map->SetSectorLightLevel(DarkApproach, Shot.Input);
                }
                else
                {
                    Map->ClearSectorLightOverride(DarkApproach);
                    Map->SetSectorCeilingZ(Door, Shot.Input);
                }
                const int32 Actual = Map->GetSectorLightLevel(bOutdoor ? 52 : DarkApproach);
                const int32 DoorLevel = Map->GetSectorLightLevel(bOutdoor ? 48 : Door);
                UE_LOG(LogTemp, Display, TEXT("Lighting %s produced sector level %d and door level %d."), Shot.Name, Actual, DoorLevel);
                if (Actual != Shot.ApproachLight || DoorLevel != Shot.DoorLight
                    || (!bOutdoor && Index < 4 && Map->GetSectorLightLevel(146) != 190)
                    || (bOutdoor && Map->GetSectorLightLevel(49) != 0))
                {
                    return Finish(false);
                }
            }
            Capture.CaptureScene();
            if (++Frame < 40)
            {
                return true;
            }
            if (!SaveWadReviewFrame(Capture, Output, Shot.Name, Size))
            {
                return Finish(false);
            }
            Frame = 0;
            return ++Index < Shots.Num() || Finish(true);
        }
    };

    void SendKey(ANativeWadPlayer& Pawn, const FKey& Key, EInputEvent Event)
    {
        if (APlayerController* Controller = Cast<APlayerController>(Pawn.GetController()))
        {
            const FInputDeviceId Device = IPlatformInputDeviceMapper::Get().GetDefaultInputDevice();
            Controller->InputKey(FInputKeyEventArgs(nullptr, Device, Key, Event, FPlatformTime::Cycles64()));
        }
    }

    struct FPlayerReview
    {
        enum class EStage { Spawn, Approach, Enter, Obstruct, Cross, Reclose };

        TWeakObjectPtr<ANativeWadLevel> Level;
        TWeakObjectPtr<ANativeWadPlayer> Pawn;
        TWeakObjectPtr<ASceneCapture2D> Camera;
        FString Output;
        EStage Stage = EStage::Spawn;
        int32 Size = 0;
        int32 Warmup = 0;
        double StartTime = 0.0;
        double ApproachTime = 0.0;
        double PreviousHeight = 0.0;
        bool bSawClosing = false;
        bool bUseHeld = false;

        bool Finish(const TCHAR* Failure = nullptr)
        {
            if (ANativeWadPlayer* Player = Pawn.Get())
            {
                SendKey(*Player, EKeys::W, IE_Released);
                SendKey(*Player, EKeys::E, IE_Released);
            }
            if (ASceneCapture2D* Capture = Camera.Get())
            {
                Capture->Destroy();
            }
            if (Failure)
            {
                UE_LOG(LogTemp, Error, TEXT("Native L30 player review failed: %s"), Failure);
            }
            UE_LOG(LogTemp, Display, TEXT("Native L30 player review finished"));
            return false;
        }

        bool Tick()
        {
            ANativeWadLevel* Map = Level.Get();
            ANativeWadPlayer* Player = Pawn.Get();
            ASceneCapture2D* Actor = Camera.Get();
            FVector2D Heights;
            if (!Map || !Player || !Actor || !Player->GetController() || !Map->GetSectorSurfaceHeights(Door, Heights))
            {
                return Finish(TEXT("the saved level did not spawn its first-person player"));
            }
            const double Now = Map->GetWorld()->GetTimeSeconds();
            if (Now - StartTime > 18.0)
            {
                UE_LOG(LogTemp, Warning, TEXT("Player review timed out at stage %d, position %s, door Z %.2f."),
                    int32(Stage), *Player->GetActorLocation().ToString(), Heights.Y);
                return Finish(TEXT("the walk-through timed out"));
            }
            Actor->SetActorLocationAndRotation(Player->GetPawnViewLocation(), Player->GetControlRotation());
            USceneCaptureComponent2D& Capture = *Actor->GetCaptureComponent2D();
            Capture.CaptureScene();
            const auto Save = [&](const TCHAR* Name) { return SaveWadReviewFrame(Capture, Output, Name, Size); };
            const double X = Player->GetActorLocation().X;
            switch (Stage)
            {
            case EStage::Spawn:
                if (++Warmup == 40)
                {
                    if (!FVector2D(Player->GetActorLocation()).Equals(FVector2D(1608, 432), 2.0) || !Save(TEXT("spawn")))
                    {
                        return Finish(TEXT("the player did not appear at the original map start"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Player spawned at the source L30 start XY (1608, 432)."));
                    Player->SetActorLocation(FVector(2745,1516,60), false, nullptr, ETeleportType::TeleportPhysics);
                    Player->GetCharacterMovement()->StopMovementImmediately();
                    Player->GetController()->SetControlRotation(FRotator::ZeroRotator);
                    if (Player->Use())
                    {
                        return Finish(TEXT("use reached a door behind the player"));
                    }
                    Player->GetController()->SetControlRotation(FRotator(0,180,0));
                    SendKey(*Player, EKeys::W, IE_Pressed);
                    ApproachTime = Now;
                    Stage = EStage::Approach;
                }
                break;
            case EStage::Approach:
                if (Now - ApproachTime >= 0.45)
                {
                    if (X < 2717.0 || X > 2722.0 || Heights.Y != 32.0 || !Save(TEXT("closed")))
                    {
                        UE_LOG(LogTemp, Warning, TEXT("Player approach reached X %.2f with door Z %.2f."), X, Heights.Y);
                        return Finish(TEXT("the closed door or facing use interaction did not behave as expected"));
                    }
                    SendKey(*Player, EKeys::E, IE_Pressed);
                    bUseHeld = true;
                    Stage = EStage::Enter;
                }
                break;
            case EStage::Enter:
                if (bUseHeld)
                {
                    SendKey(*Player, EKeys::E, IE_Released);
                    bUseHeld = false;
                }
                if (X <= 2702.0)
                {
                    SendKey(*Player, EKeys::W, IE_Released);
                    Player->GetCharacterMovement()->StopMovementImmediately();
                    if (X < 2695.0 || !Save(TEXT("threshold")))
                    {
                        return Finish(TEXT("the player could not stop underneath the moving door"));
                    }
                    Stage = EStage::Obstruct;
                }
                break;
            case EStage::Obstruct:
                bSawClosing |= Heights.Y < PreviousHeight;
                if (bSawClosing && Heights.Y > PreviousHeight)
                {
                    UE_LOG(LogTemp, Display, TEXT("The actual player pawn reversed the closing door at Z %.2f."), PreviousHeight);
                    SendKey(*Player, EKeys::W, IE_Pressed);
                    Stage = EStage::Cross;
                }
                break;
            case EStage::Cross:
                if (X <= 2676.0)
                {
                    SendKey(*Player, EKeys::W, IE_Released);
                    Player->GetCharacterMovement()->StopMovementImmediately();
                    if (!Save(TEXT("crossed")))
                    {
                        return Finish(TEXT("could not capture the view after crossing"));
                    }
                    Stage = EStage::Reclose;
                }
                break;
            case EStage::Reclose:
                if (Heights.Y == 32.0 && Map->GetSectorLightLevel(DarkApproach) == 84)
                {
                    UE_LOG(LogTemp, Display, TEXT("Player walked through door 147 using W and E. The door reclosed and restored the closed sector light."));
                    return Finish();
                }
                break;
            }
            PreviousHeight = Heights.Y;
            return true;
        }
    };

    ANativeWadLevel* GetReviewLevel(UWorld* World, const FString& Output, int32 Size)
    {
        if (!World || !World->IsGameWorld() || Size < 256 || Size > 2048)
        {
            return nullptr;
        }
        TActorIterator<ANativeWadLevel> Level(World);
        if (!Level)
        {
            return nullptr;
        }
#if WITH_EDITOR
        FAssetCompilingManager::Get().FinishAllCompilation();
#endif
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Output), true);
        return *Level;
    }

    struct FWalkReview
    {
        enum class EStage { Spawn, Route, Blocked, Opening, Cross, Reclose, ExitApproach, Arrival };

        FName WorldHandle;
        TWeakObjectPtr<UWorld> SourceWorld;
        TWeakObjectPtr<ASceneCapture2D> Camera;
        FString Output;
        EStage Stage = EStage::Spawn;
        double StartTime = 0.0;
        double StageTime = 0.0;
        int32 Size = 0;
        int32 NextPoint = 0;
        int32 CaptureFrames = 0;
        bool bLevelLink = false;
        bool bWalking = false;

        UWorld* World() const
        {
            const FWorldContext* Context = GEngine->GetWorldContextFromHandle(WorldHandle);
            return Context ? Context->World() : nullptr;
        }

        bool Finish(ANativeWadPlayer* Player, const TCHAR* Failure = nullptr)
        {
            if (Player && bWalking)
            {
                SendKey(*Player, EKeys::W, IE_Released);
            }
            if (ASceneCapture2D* Actor = Camera.Get())
            {
                Actor->Destroy();
            }
            if (Failure)
            {
                const FString Location = Player ? Player->GetActorLocation().ToString() : TEXT("no player");
                UE_LOG(LogTemp, Error, TEXT("Native WAD walk review failed at stage %d, point %d (%s): %s"),
                    int32(Stage), NextPoint, *Location, Failure);
            }
            UE_LOG(LogTemp, Display, TEXT("Native WAD port review finished"));
            return false;
        }

        void Walk(ANativeWadPlayer& Player)
        {
            if (!bWalking)
            {
                SendKey(Player, EKeys::W, IE_Pressed);
                bWalking = true;
            }
        }

        void Stop(ANativeWadPlayer& Player)
        {
            if (bWalking)
            {
                SendKey(Player, EKeys::W, IE_Released);
                Player.GetCharacterMovement()->StopMovementImmediately();
                bWalking = false;
            }
        }

        bool WalkTo(ANativeWadPlayer& Player, const FVector2D& Target)
        {
            const FVector2D Offset = Target - FVector2D(Player.GetActorLocation());
            if (Offset.SizeSquared() < FMath::Square(5.0))
            {
                Stop(Player);
                return true;
            }
            Player.GetController()->SetControlRotation(FVector(Offset, 0).Rotation());
            Walk(Player);
            return false;
        }

        bool CaptureView(ANativeWadPlayer& Player, const TCHAR* Name, bool& bSaved, double BackUp = 0.0)
        {
            const FRotator Rotation = Player.GetControlRotation();
            const FVector ViewPosition = Player.GetPawnViewLocation() - Rotation.Vector() * BackUp;
            if (!Camera.IsValid())
            {
                Camera = SpawnWadReviewCamera(*Player.GetWorld(), Size, ViewPosition, Rotation);
            }
            ASceneCapture2D* Actor = Camera.Get();
            if (!Actor)
            {
                bSaved = false;
                return true;
            }
            Actor->SetActorLocationAndRotation(ViewPosition, Rotation);
            USceneCaptureComponent2D& Capture = *Actor->GetCaptureComponent2D();
            Capture.CaptureScene();
            if (++CaptureFrames < 40)
            {
                return false;
            }
            CaptureFrames = 0;
            bSaved = SaveWadReviewFrame(Capture, Output, Name, Size);
            return true;
        }

        bool Tick()
        {
            UWorld* Current = World();
            APlayerController* Controller = Current ? Current->GetFirstPlayerController() : nullptr;
            ANativeWadPlayer* Player = Controller ? Cast<ANativeWadPlayer>(Controller->GetPawn()) : nullptr;
            const double Now = FPlatformTime::Seconds();
            if (Now - StartTime > 120.0)
            {
                return Finish(Player, TEXT("the review timed out"));
            }
            if (!Player)
            {
                return true;
            }
            if (Current != SourceWorld.Get())
            {
                if (!bLevelLink || !Current->GetMapName().EndsWith(TEXT("L301"))
                    || !FVector2D(Player->GetActorLocation()).Equals(FVector2D(3440, 2944), 2.0))
                {
                    return Finish(Player, TEXT("travel did not arrive at the source L301 player start"));
                }
                if (Stage != EStage::Arrival)
                {
                    Camera.Reset();
                    CaptureFrames = 0;
                    bWalking = false;
                    Stage = EStage::Arrival;
                    UE_LOG(LogTemp, Display, TEXT("Entered native L301 at source player start (3440, 2944)."));
                }
                bool bSaved = false;
                return !CaptureView(*Player, TEXT("arrival"), bSaved)
                    || Finish(Player, bSaved ? nullptr : TEXT("could not save the L301 arrival frame"));
            }

            const FVector Position = Player->GetActorLocation();
            bool bSaved = false;
            if (bLevelLink)
            {
                if (Stage == EStage::Spawn)
                {
                    // Sector 129's scripted gate is still closed. Stage only the exit review
                    // on its far side, then walk into the actual linked footprint.
                    if (!Player->SetActorLocation(FVector(-1518, 3616, 29), false, nullptr, ETeleportType::TeleportPhysics))
                    {
                        return Finish(Player, TEXT("could not stage beyond the scripted gate"));
                    }
                    Player->GetCharacterMovement()->StopMovementImmediately();
                    Controller->SetControlRotation(FVector(158, 96, 0).Rotation());
                    Stage = EStage::ExitApproach;
                }
                const TActorIterator<ANativeWadLevel> Level(Current);
                if (!Level || Level->GetLevelLinks().Num() != 1 || Level->GetLevelLinks()[0].Sector != 157
                    || Level->GetLevelLinks()[0].SourceThing != 394)
                {
                    return Finish(Player, TEXT("the saved L30 level link differs from the source marker"));
                }
                if (NextPoint == 0)
                {
                    if (Level->FindLevelLinkAt(FVector2D(Player->GetActorLocation())) != INDEX_NONE)
                    {
                        return Finish(Player, TEXT("the review began inside the linked sector"));
                    }
                    if (!CaptureView(*Player, TEXT("exit-approach"), bSaved))
                    {
                        return true;
                    }
                    if (!bSaved)
                    {
                        return Finish(Player, TEXT("could not save the exit approach frame"));
                    }
                    ++NextPoint;
                }
                if (WalkTo(*Player, FVector2D(-1360, 3712)))
                {
                    return Finish(Player, TEXT("the player reached the marker without traveling"));
                }
                return true;
            }

            const TActorIterator<ANativeWadLevel> Level(Current);
            FVector2D Heights;
            if (!Level || !Level->GetSectorSurfaceHeights(Door, Heights))
            {
                return Finish(Player, TEXT("L30 door 147 is missing"));
            }
            switch (Stage)
            {
            case EStage::Spawn:
                if (!FVector2D(Position).Equals(FVector2D(1608, 432), 2.0))
                {
                    return Finish(Player, TEXT("the player did not start at the authored spawn"));
                }
                if (CaptureView(*Player, TEXT("spawn"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(Player, TEXT("could not save the spawn frame"));
                    }
                    Stage = EStage::Route;
                }
                break;
            case EStage::Route:
            {
                static const FVector2D Points[] = {{2044, 988}, {2320, 1292}, {2660, 1516}};
                if (WalkTo(*Player, Points[NextPoint]) && ++NextPoint == UE_ARRAY_COUNT(Points))
                {
                    Controller->SetControlRotation(FRotator::ZeroRotator);
                    StageTime = Current->GetTimeSeconds();
                    Stage = EStage::Blocked;
                    Walk(*Player);
                }
                break;
            }
            case EStage::Blocked:
                if (Current->GetTimeSeconds() - StageTime >= 0.45)
                {
                    Stop(*Player);
                    if (Position.X < 2677.0 || Position.X > 2684.0 || Heights.Y != 32.0
                        || Level->GetSectorLightLevel(DarkApproach) != 84)
                    {
                        return Finish(Player, TEXT("the closed door did not stop the player or its lighting differed"));
                    }
                    if (CaptureView(*Player, TEXT("closed"), bSaved, 60.0))
                    {
                        if (!bSaved)
                        {
                            return Finish(Player, TEXT("could not save the closed door frame"));
                        }
                        SendKey(*Player, EKeys::E, IE_Pressed);
                        Stage = EStage::Opening;
                    }
                }
                break;
            case EStage::Opening:
                SendKey(*Player, EKeys::E, IE_Released);
                if (Heights.Y == 112.0 && Level->GetSectorLightLevel(DarkApproach) == 160
                    && CaptureView(*Player, TEXT("open"), bSaved, 60.0))
                {
                    if (!bSaved)
                    {
                        return Finish(Player, TEXT("could not save the open door frame"));
                    }
                    Stage = EStage::Cross;
                }
                break;
            case EStage::Cross:
                if (WalkTo(*Player, FVector2D(2756, 1516)))
                {
                    Stage = EStage::Reclose;
                }
                break;
            case EStage::Reclose:
                if (Heights.Y == 32.0 && Level->GetSectorLightLevel(DarkApproach) == 84
                    && CaptureView(*Player, TEXT("crossed"), bSaved))
                {
                    UE_LOG(LogTemp, Display, TEXT("Walked L30 from (1608,432) to (%.1f,%.1f) through door 147. Sector 14 light 84 -> 160 -> 84."),
                        Position.X, Position.Y);
                    return Finish(Player, bSaved ? nullptr : TEXT("could not save the crossed door frame"));
                }
                break;
            default:
                return Finish(Player, TEXT("unexpected route stage"));
            }
            return true;
        }
    };
}

bool StartNativeWadLightingReview(UWorld* World, const FString& OutputPath, int32 ImageSize, bool bDayClock)
{
    ANativeWadLevel* Level = GetReviewLevel(World, OutputPath, ImageSize);
    if (!Level)
    {
        return false;
    }
    const bool bOutdoor = World->GetMapName().EndsWith(TEXT("L151"));
    if (bDayClock && (!bOutdoor || !Level->HasDaySky()))
    {
        return false;
    }
    FVector2D Heights = FVector2D::ZeroVector;
    if (bOutdoor)
    {
        if (Level->GetSectors().Num() != 55 || !Level->GetSectors()[52].bOutside || Level->GetDoors().Num() != 1
            || Level->GetDoors()[0].Sector != 48 || Level->GetDoors()[0].OpenZ != 48
            || !Level->GetSectorSurfaceHeights(48, Heights) || !Heights.Equals(FVector2D(-32,-32)))
        {
            return false;
        }
    }
    else if (!World->GetMapName().EndsWith(TEXT("L30")) || !Level->GetSectorSurfaceHeights(Door, Heights))
    {
        return false;
    }
    const FVector Position = bOutdoor ? FVector(1500,2200,100) : FVector(2745,1516,74);
    const FVector Focus = bOutdoor ? FVector(2250,2270,-20) : FVector(2660,1516,65);
    ASceneCapture2D* Camera = SpawnWadReviewCamera(*World, ImageSize, Position, (Focus - Position).Rotation());
    if (!Camera)
    {
        return false;
    }
    const TSharedRef<FLightingReview> Review = MakeShared<FLightingReview>();
    Review->Level = Level;
    Review->Camera = Camera;
    Review->Shots = bDayClock ? TConstArrayView<FLightingShot>(L151ClockShots)
        : bOutdoor ? TConstArrayView<FLightingShot>(L151Shots) : TConstArrayView<FLightingShot>(L30Shots);
    Review->Output = OutputPath;
    Review->InitialCeiling = Heights.Y;
    Review->InitialOutside = Level->GetOutsideLightLevel();
    Review->InitialHour = Level->GetTimeOfDayHours();
    Review->Size = ImageSize;
    Review->bOutdoor = bOutdoor;
    Review->bDayClock = bDayClock;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float) { return Review->Tick(); }));
    return true;
}

bool StartNativeL30WalkReview(UWorld* World, const FString& OutputPath, int32 ImageSize, bool bLevelLink)
{
    ANativeWadLevel* Level = GetReviewLevel(World, OutputPath, ImageSize);
    const FWorldContext* Context = Level ? GEngine->GetWorldContextFromWorld(World) : nullptr;
    if (!Context || !World->GetMapName().EndsWith(TEXT("L30")))
    {
        return false;
    }
    const TSharedRef<FWalkReview> Review = MakeShared<FWalkReview>();
    Review->WorldHandle = Context->ContextHandle;
    Review->SourceWorld = World;
    Review->Output = OutputPath;
    Review->StartTime = FPlatformTime::Seconds();
    Review->Size = ImageSize;
    Review->bLevelLink = bLevelLink;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float) { return Review->Tick(); }));
    return true;
}

bool StartNativeL30PlayerReview(UWorld* World, const FString& OutputPath, int32 ImageSize)
{
    ANativeWadLevel* Level = GetReviewLevel(World, OutputPath, ImageSize);
    APlayerController* Controller = Level ? World->GetFirstPlayerController() : nullptr;
    ANativeWadPlayer* Player = Controller ? Cast<ANativeWadPlayer>(Controller->GetPawn()) : nullptr;
    if (!Player || !World->GetMapName().EndsWith(TEXT("L30")))
    {
        return false;
    }
    ASceneCapture2D* Camera = SpawnWadReviewCamera(*World, ImageSize, Player->GetPawnViewLocation(), Player->GetControlRotation());
    if (!Camera)
    {
        return false;
    }
    const TSharedRef<FPlayerReview> Review = MakeShared<FPlayerReview>();
    Review->Level = Level;
    Review->Pawn = Player;
    Review->Camera = Camera;
    Review->Output = OutputPath;
    Review->Size = ImageSize;
    Review->StartTime = World->GetTimeSeconds();
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float) { return Review->Tick(); }));
    return true;
}
