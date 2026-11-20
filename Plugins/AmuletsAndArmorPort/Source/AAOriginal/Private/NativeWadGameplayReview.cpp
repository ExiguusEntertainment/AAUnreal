#include "CoreMinimal.h"
#include "NativeWadReview.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/SceneCapture2D.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "HAL/FileManager.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadCheckpoint.h"
#include "NativeWadEncounter.h"
#include "UnrealClient.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "NativeWadTown.h"
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

    enum class ESliceAction { Walk, Use, Ceiling, Ranged, Cover, Rejuvenate, Pickup, Equip, Guardian, EatBerry,
        TimedClosed, TimedReset, TrapStart, TrapCycle, PressureOpen, GateOpen, Checkpoint, Lift, Capture, Travel, Complete };

    struct FSliceStep
    {
        ESliceAction Action;
        FVector Target = FVector::ZeroVector;
        int32 Sector = INDEX_NONE;
        double Height = 0.0;
        const TCHAR* Frame = nullptr;
    };

    using EAction = ESliceAction;
    const FSliceStep SliceRoute[] = {
        {EAction::Capture, {}, INDEX_NONE, 0, TEXT("spawn")},
        // Get into the Elf Guardian's room through door 144.
        {EAction::Walk, {1256,916,0}}, {EAction::Walk, {1268,1192,0}}, {EAction::Walk, {1416,1340,0}},
        {EAction::Use, {1440,1340,57}}, {EAction::Ceiling, {}, 144, 104},
        {EAction::Capture, {}, INDEX_NONE, 0, TEXT("door")},
        {EAction::Ranged}, {EAction::Ceiling, {}, 144, 8}, {EAction::Cover}, {EAction::Rejuvenate},
        {EAction::Use, {1440,1340,57}}, {EAction::Ceiling, {}, 144, 104},
        {EAction::Walk, {1488,1340,0}},
        {EAction::Pickup, {}, NativeWadRules::IronDagger, 0, TEXT("dagger")}, {EAction::Equip, {}, NativeWadRules::IronDagger},
        {EAction::Guardian}, {EAction::Pickup, {}, NativeWadRules::Berry, 0, TEXT("berry")},
        {EAction::Equip, {}, NativeWadRules::Berry}, {EAction::EatBerry}, {EAction::Equip, {}, NativeWadRules::IronDagger},
        // Return through the same door and take the outer path to the puzzle.
        {EAction::Walk, {1480,1340,0}}, {EAction::Use, {1448,1340,57}}, {EAction::Ceiling, {}, 144, 104},
        {EAction::Walk, {1416,1340,0}}, {EAction::Walk, {1220,1788,0}}, {EAction::Walk, {1024,2132,0}},
        {EAction::Walk, {468,2580,0}}, {EAction::Walk, {52,2656,0}}, {EAction::Walk, {-504,2580,0}},
        {EAction::Walk, {-672,2412,0}}, {EAction::Walk, {-680,2400,0}}, {EAction::Walk, {-680,2364,0}},
        {EAction::Walk, {-424,2352,0}}, {EAction::Walk, {-324,2344,0}}, {EAction::Walk, {-168,2404,0}},
        {EAction::Walk, {24,2392,0}}, {EAction::Walk, {364,2272,0}}, {EAction::Walk, {380,2256,0}},
        {EAction::Walk, {416,2188,0}}, {EAction::Walk, {416,2136,0}}, {EAction::Walk, {360,2008,0}},
        {EAction::Walk, {316,1764,0}}, {EAction::Walk, {144,1368,0}}, {EAction::Walk, {144,1240,0}},
        {EAction::Ceiling, {}, 162, 0}, {EAction::TimedClosed},
        {EAction::Walk, {136,1164,0}}, {EAction::Use, {136,1136,49}}, {EAction::Use, {136,1136,49}},
        {EAction::TimedReset},
        {EAction::Walk, {96,1164,0}}, {EAction::TrapStart, {96,1136,49}}, {EAction::TrapCycle},
        {EAction::TrapStart, {96,1136,49}}, {EAction::TrapCycle}, {EAction::Walk, {136,1164,0}},
        // The order keeps each timed door open as the player reaches it.
        {EAction::Use, {136,1136,49}},
        {EAction::Walk, {96,1164,0}}, {EAction::Use, {96,1136,49}},
        {EAction::Walk, {176,1164,0}}, {EAction::Use, {176,1136,49}},
        {EAction::Walk, {16,1164,0}}, {EAction::Walk, {8,1080,0}}, {EAction::Walk, {8,792,0}},
        {EAction::Ceiling, {}, 162, 140}, {EAction::PressureOpen},
        {EAction::Walk, {80,640,0}}, {EAction::Walk, {80,572,0}}, {EAction::Walk, {16,508,0}},
        {EAction::Use, {16,480,10}}, {EAction::GateOpen},
        {EAction::Capture, {}, INDEX_NONE, 0, TEXT("switch")},
        {EAction::Checkpoint},
        // Walk back through the pressure-plate corridor and take the now-open exit.
        {EAction::Walk, {80,580,0}}, {EAction::Walk, {80,696,0}}, {EAction::Walk, {20,816,0}},
        {EAction::Walk, {48,1200,0}}, {EAction::Walk, {144,1460,0}}, {EAction::Walk, {360,1908,0}},
        {EAction::Walk, {392,2112,0}}, {EAction::Walk, {416,2152,0}}, {EAction::Walk, {412,2224,0}},
        {EAction::Walk, {380,2256,0}}, {EAction::Walk, {252,2324,0}}, {EAction::Walk, {-108,2404,0}},
        {EAction::Walk, {-216,2404,0}}, {EAction::Walk, {-336,2344,0}}, {EAction::Walk, {-504,2352,0}},
        {EAction::Walk, {-672,2356,0}}, {EAction::Walk, {-676,2360,0}}, {EAction::Walk, {-708,2424,0}},
        {EAction::Walk, {-864,2920,0}}, {EAction::Walk, {-1148,3204,0}}, {EAction::Walk, {-1536,3500,0}},
        {EAction::Capture, {-1518,3555,70}, INDEX_NONE, 0, TEXT("gate")},
        {EAction::Walk, {-1488,3600,0}}, {EAction::Travel, {-1360,3712,0}},
    };

    const FSliceStep L151Route[] = {
        {EAction::Capture, {}, INDEX_NONE, 0, TEXT("spawn")},
        {EAction::Walk, {528,3420,0}}, {EAction::Walk, {536,3412,0}},
        {EAction::Walk, {772,2912,0}}, {EAction::Walk, {780,2904,0}},
        {EAction::Walk, {1352,2904,0}}, {EAction::Walk, {1924,3064,0}}, {EAction::Walk, {2496,3248,0}},
        {EAction::Pickup, {}, NativeWadRules::FeatherfallPotion, 0, TEXT("potion")},
        {EAction::Equip, {}, NativeWadRules::FeatherfallPotion}, {EAction::Equip, {}, NativeWadRules::IronDagger},
        {EAction::Walk, {2308,2872,0}}, {EAction::Walk, {2364,2316,0}},
        {EAction::Walk, {2788,1868,0}}, {EAction::Walk, {2808,1856,0}},
        {EAction::Walk, {3272,1840,0}}, {EAction::Walk, {3452,1852,0}},
        {EAction::Walk, {3588,1988,0}}, {EAction::Walk, {3588,2560,0}}, {EAction::Walk, {3440,3132,0}},
        {EAction::Walk, {2990,3580,0}}, {EAction::Guardian},
        {EAction::Walk, {3028,3700,0}}, {EAction::Walk, {3100,3708,0}},
        {EAction::Lift, {}, 31, 0}, {EAction::Capture, {3400,3620,0}, INDEX_NONE, 0, TEXT("lift-raised")},
        {EAction::Lift, {}, 31, -250}, {EAction::Capture, {3400,3620,-170}, INDEX_NONE, 0, TEXT("lift-returned")},
        {EAction::Complete},
    };

    enum class ETrailAction { Walk, Leap, Door, Capture, Link, Plate, FloorReset };
    struct FTrailStep
    {
        ETrailAction Action;
        FVector Target;
        int32 Sector = INDEX_NONE;
        FVector2D Edge = FVector2D::ZeroVector;
        const TCHAR* Frame = nullptr;
    };

    const FTrailStep L531Trail[] = {
        {ETrailAction::Walk, {1136,360,0}}, {ETrailAction::Walk, {1360,360,0}},
        {ETrailAction::Walk, {2355,579,0}}, {ETrailAction::Leap, {2318,748,0}, 11, {2337,662}},
        {ETrailAction::Walk, {2827,960,0}}, {ETrailAction::Leap, {2906,1116,0}, 12, {2865,1036}},
        {ETrailAction::Walk, {2712,1096,0}}, {ETrailAction::Walk, {1432,1016,0}},
        {ETrailAction::Walk, {1400,1000,0}}, {ETrailAction::Walk, {1138,970,0}},
        {ETrailAction::Leap, {1260,1094,0}, 14, {1198,1032}},
        {ETrailAction::Walk, {720,2008,0}}, {ETrailAction::Walk, {720,2288,0}}, {ETrailAction::Walk, {1344,2944,0}},
        {ETrailAction::Capture, {1365,2973,62}, INDEX_NONE, {}, TEXT("l531-door-closed")},
        {ETrailAction::Door, {1365,2973,62}, 24},
        {ETrailAction::Capture, {1376,3025,62}, INDEX_NONE, {}, TEXT("l531-door-open")},
        {ETrailAction::Walk, {1368,3040,0}}, {ETrailAction::Walk, {1336,3380,0}},
        {ETrailAction::Walk, {1320,3456,0}}, {ETrailAction::Walk, {1264,3512,0}},
        {ETrailAction::Link, {1104,3512,0}},
    };

    const FTrailStep L31Trail[] = {
        {ETrailAction::Walk, {2536,2630,0}}, {ETrailAction::Door, {2530,2576,230}, 59},
        {ETrailAction::Capture, {2530,2520,226}, INDEX_NONE, {}, TEXT("l31-door-open")},
        {ETrailAction::Walk, {2528,2512,0}}, {ETrailAction::Walk, {3384,2448,0}},
        {ETrailAction::Walk, {3408,2424,0}}, {ETrailAction::Walk, {3392,2296,0}}, {ETrailAction::Walk, {3288,2240,0}},
        {ETrailAction::Walk, {2728,2192,0}}, {ETrailAction::Walk, {2696,2160,0}}, {ETrailAction::Walk, {2720,1912,0}},
        {ETrailAction::Walk, {2776,1880,0}}, {ETrailAction::Walk, {2984,1904,0}}, {ETrailAction::Walk, {3088,2040,0}},
        {ETrailAction::Plate, {}, 135}, {ETrailAction::Capture, {3088,2152,110}, INDEX_NONE, {}, TEXT("l31-pressure-plate")},
    };

    const FTrailStep L31ExitTrail[] = {
        {ETrailAction::FloorReset, {0,0,192}, 135},
        {ETrailAction::Walk, {3008,896,0}}, {ETrailAction::Walk, {2448,344,0}}, {ETrailAction::Walk, {2520,-360,0}},
        {ETrailAction::Walk, {2736,-528,0}}, {ETrailAction::Walk, {3880,-488,0}}, {ETrailAction::Walk, {4024,-360,0}},
        {ETrailAction::Door, {4049,-360,65}, 168},
        {ETrailAction::Capture, {4112,-360,65}, INDEX_NONE, {}, TEXT("l31-exit-door")},
        {ETrailAction::Walk, {4080,-360,0}}, {ETrailAction::Walk, {4200,-416,0}},
        {ETrailAction::Link, {4296,-868,0}},
    };

    const FTrailStep L22Trail[] = {
        {ETrailAction::Walk, {-2584,-528,0}}, {ETrailAction::Walk, {-2632,-528,0}},
        {ETrailAction::Walk, {-2672,-448,0}}, {ETrailAction::Walk, {-2632,-112,0}}, {ETrailAction::Walk, {-2568,-48,0}},
        {ETrailAction::Door, {-2568,0,137}, 134},
        {ETrailAction::Capture, {-2568,50,137}, INDEX_NONE, {}, TEXT("l22-door-open")},
        {ETrailAction::Walk, {-2568,76,0}},
    };

    enum class ETownReview { None, Tour, Failure, Reload, Start };
    constexpr const TCHAR* TownReviewSlot = TEXT("NativeWadTownReview");
    enum class EL22RouteAction { Walk, Door, Switch, RepeatSwitch, Journal, Plate, Capture, Exit };

    struct FL22RouteStep
    {
        FVector Target;
        EL22RouteAction Action = EL22RouteAction::Walk;
        int32 Sector = INDEX_NONE;
        const TCHAR* Frame = nullptr;
    };

    using EL22Action = EL22RouteAction;
    const FL22RouteStep L22TownRoute[] = {
        // From the original start, follow the southwest loop to the trap corridor.
        {{-2584,-528,0}}, {{-2632,-528,0}}, {{-2672,-448,0}}, {{-2632,-112,0}}, {{-2568,-48,0}},
        {{-2568,0,137}, EL22Action::Door, 134}, {{-2568,76,0}}, {{-2688,432,0}}, {{-2672,520,0}},
        {{-2576,568,0}}, {{-2424,552,0}}, {{-2288,456,0}}, {{-2256,392,0}}, {{-2192,252,0}},
        {{-2149,193,57}, EL22Action::Door, 194}, {{-2140,193,0}}, {{-2048,48,0}}, {{-2000,-520,0}},
        {{-1904,-568,0}}, {{-920,-528,0}}, {{-872,-432,0}}, {{-832,-205,0}}, {{-832,-168,0}},
        {{-800,-160,57}, EL22Action::Door, 63}, {{-720,-144,0}}, {{-216,-48,0}}, {{-72,96,0}},
        {{-104,240,0}}, {{-248,304,0}}, {{-311,304,0}}, {{-353,304,49}, EL22Action::Door, 199},
        {{-400,304,0}}, {{-755,232,0}}, {{-802,232,57}, EL22Action::Door, 200},
        {{-832,232,0}}, {{-928,360,0}}, {{-1192,432,0}}, {{-1208,440,0}},
        {{-1208,464,57}, EL22Action::Door, 64}, {{-1208,492,0}}, {{-1200,520,0}}, {{-1136,552,0}},
        {{-520,576,0}}, {{-420,768,0}}, {{-392,816,0}}, {{-328,864,0}}, {{-288,960,0}},
        {{-288,2008,0}}, {{-336,2344,0}}, {{-384,2368,0}}, {{-1140,2360,0}}, {{-1176,2352,0}},
        {{-1192,2320,0}}, {{-1184,2248,0}}, {{-1184,2208,0}},
        {{-1168,2208,49}, EL22Action::Door, 21}, {{-1120,2208,0}}, {{-1056,2144,0}},
        {{-1056,2112,57}, EL22Action::Door, 16}, {{-1056,2088,0}}, {{-1152,2008,0}}, {{-1288,2016,0}},
        {{-1312,2064,0}}, {{-1328,2344,0}}, {{-1336,2352,0}}, {{-1344,2360,0}}, {{-1392,2384,0}},
        {{-1608,2400,0}}, {{-1632,2400,0}}, {{-1632,2416,345}, EL22Action::Door, 101},
        {{-1632,2448,0}}, {{-1688,2480,0}}, {{-1752,2480,0}}, {{-1792,2480,329}, EL22Action::Door, 103},
        {{-1880,2480,0}}, {{-1928,2448,0}}, {{-1936,2432,0}}, {{-1936,2408,0}}, {{-1904,2384,0}},
        {{-1872,2384,249}, EL22Action::Door, 107}, {{-1824,2384,0}}, {{-1784,2368,0}},
        {{-1776,2352,0}}, {{-1776,2112,0}}, {{-1816,2048,0}}, {{-1824,2040,0}},
        {{-1856,2040,220}, EL22Action::Switch, 15},
        {{-1856,2040,220}, EL22Action::RepeatSwitch, 15},
        {{-1856,2040,220}, EL22Action::Capture, INDEX_NONE, TEXT("switch")},
        {{-1706,2065,0}}, {{-1666,2067,208}, EL22Action::Journal}, {{-1816,2048,0}},
        // Double back to the new opening in sector 15 and step onto the exit plate.
        {{-1776,2112,0}}, {{-1776,2352,0}}, {{-1776,2360,0}}, {{-1824,2384,0}},
        {{-1860,2384,249}, EL22Action::Door, 107}, {{-1912,2384,0}}, {{-1928,2400,0}},
        {{-1936,2416,0}}, {{-1936,2440,0}}, {{-1856,2480,0}}, {{-1850,2480,289}, EL22Action::Capture, INDEX_NONE, TEXT("late-passage")},
        {{-1824,2480,289}, EL22Action::Door, 103}, {{-1736,2480,0}}, {{-1656,2448,0}}, {{-1632,2448,0}},
        {{-1632,2432,329}, EL22Action::Door, 101}, {{-1632,2400,0}}, {{-1592,2384,0}},
        {{-1352,2368,0}}, {{-1344,2360,0}}, {{-1336,2352,0}}, {{-1312,2304,0}}, {{-1296,2024,0}},
        {{-1264,2008,0}}, {{-1128,2016,0}}, {{-1056,2080,0}}, {{-1056,2096,57}, EL22Action::Door, 16},
        {{-1056,2120,0}}, {{-968,2176,0}}, {{-920,2176,0}}, {{-896,2128,0}}, {{-912,1976,0}},
        {{-1336,1680,0}}, {{-1432,1488,0}, EL22Action::Plate, 193},
        {{-1376,1584,26}, EL22Action::Capture, INDEX_NONE, TEXT("exit-plate")},
        // Return up the stair and through the exit gate to the campaign form.
        {{-1256,1760,0}}, {{-912,1976,0}}, {{-912,2168,0}}, {{-928,2176,0}}, {{-976,2168,0}},
        {{-1056,2120,0}}, {{-1056,2112,57}, EL22Action::Door, 16}, {{-1056,2088,0}}, {{-1136,2008,0}},
        {{-1288,2016,0}}, {{-1312,2064,0}}, {{-1328,2344,0}}, {{-1336,2352,0}}, {{-1344,2360,0}},
        {{-1392,2384,0}}, {{-1608,2400,0}}, {{-1624,2404,0}}, {{-1624,2416,345}, EL22Action::Door, 101},
        {{-1624,2464,0}}, {{-1620,2710,0}},
        {{-1620,2800,327}, EL22Action::Capture, INDEX_NONE, TEXT("town-exit")},
        {{-1620,2832,0}, EL22Action::Exit},
    };

    struct FWalkReview
    {
        enum class EStage { Spawn, Route, Blocked, Opening, Cross, Reclose, ExitApproach, Arrival };
        enum class ERiverStage { Spawn, Encounter, RiverBank, Drift, WaterView, Route, Ledge, Jump,
            Exit, Arrival, Teleport, Save, Return };

        FName WorldHandle;
        TWeakObjectPtr<UWorld> SourceWorld;
        TWeakObjectPtr<ASceneCapture2D> Camera;
        FString Output;
        EStage Stage = EStage::Spawn;
        double StartTime = 0.0;
        double StageTime = 0.0;
        double SliceProgressTime = 0.0;
        int32 Size = 0;
        int32 NextPoint = 0;
        int32 CaptureFrames = 0;
        int32 CoverBolts = INDEX_NONE;
        int32 CapturedItem = INDEX_NONE;
        int32 HealthBeforeCast = INDEX_NONE;
        int32 ManaBeforeCast = INDEX_NONE;
        int32 FoodBeforeBerry = INDEX_NONE;
        int32 ExperienceBeforeTravel = INDEX_NONE;
        int32 HealthBeforeTravel = INDEX_NONE;
        int32 ManaBeforeTravel = INDEX_NONE;
        int32 FoodBeforeTravel = INDEX_NONE;
        int32 StartingExperience = 0;
        int32 LevelBeforeTravel = 0;
        int32 ThresholdBeforeTravel = 0;
        int32 MaxHealthBeforeTravel = 0;
        int32 MaxManaBeforeTravel = 0;
        TArray<int32> AttributesBeforeTravel;
        double CoverStart = 0.0;
        double GateFloorStarted = 0.0;
        double GateCeilingStarted = 0.0;
        double TimedDoorStarted = 0.0;
        double TrapStarted = 0.0;
        int32 TrapMissilesBefore = 0;
        int32 TrapBlockedBefore = 0;
        bool bLevelLink = false;
        bool bSlice = false;
        bool bSecondMap = false;
        bool bCampaign = false;
        bool bArrivalCaptured = false;
        bool bCheckpointReview = false;
        bool bCheckpointVerified = false;
        bool bRiverReview = false;
        bool bRiverReload = false;
        ERiverStage RiverStage = ERiverStage::Spawn;
        FVector DriftStart = FVector::ZeroVector;
        FVector2D InitialWaterUV = FVector2D::ZeroVector;
        double DriftStarted = 0.0;
        double RiverProgressTime = 0.0;
        bool bJumpStarted = false;
        bool bRavineReview = false;
        bool bAmbientReview = false;
        bool bL22TrapReview = false;
        bool bLiftCheckpointReview = false;
        bool bLiftReload = false;
        bool bDoorReload = false;
        ETownReview TownReview = ETownReview::None;
        TArray<int32> ItemsBeforeTravel;
        int32 ReadyBeforeTravel = INDEX_NONE;
        double HourBeforeTravel = 0.0;
        bool bSavedOpeningDoor = false;
        bool bRavineStarted = false;
        bool bTrailArrived = false;
        bool bL22Journey = false;
        bool bObservedStartupLiftTop = false;
        bool bObservedStartupLiftReturn = false;
        bool bTrailActionPressed = false;
        TArray<FTrailStep> Trail;
        double EmitterAudibleSince = 0.0;
        double SavedLiftDelay = 0.0;
        double SavedDoorOpenTime = 0.0;
        double TrapParalysisStart = 0.0;
        bool bVerifiedTrapDamage = false;
        int32 TrapHealth = 0;
        FVector2D TrapHitPosition = FVector2D::ZeroVector;
        bool bWalking = false;
        bool bCapturedEncounter = false;
        bool bRequestedInventoryFrame = false;
        int32 InventoryFrameHold = 0;
        TOptional<FKey> HeldKey;

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
            if (Player && HeldKey)
            {
                SendKey(*Player, HeldKey.GetValue(), IE_Released);
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
            const double AcceptanceRadius = bRiverReview ? 16.0 : bRavineReview || bLiftCheckpointReview ? 12.0
                : bSecondMap || Stage == EStage::Arrival ? 14.0 : 5.0;
            if (Offset.SizeSquared() < FMath::Square(AcceptanceRadius))
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

        void Aim(ANativeWadPlayer& Player, const FVector& Point)
        {
            Player.GetController()->SetControlRotation((Point - Player.GetPawnViewLocation()).Rotation());
        }

        void Press(ANativeWadPlayer& Player, const FKey& Key)
        {
            SendKey(Player, Key, IE_Pressed);
            HeldKey = Key;
        }

        void RecordTownCharacter(const ANativeWadPlayer& Player)
        {
            ExperienceBeforeTravel = Player.GetExperience();
            HealthBeforeTravel = Player.GetHealth();
            ManaBeforeTravel = Player.GetMana();
            FoodBeforeTravel = Player.GetFood();
            LevelBeforeTravel = Player.GetCharacterLevel();
            ThresholdBeforeTravel = Player.GetExperienceNeeded();
            MaxHealthBeforeTravel = Player.GetMaxHealth();
            MaxManaBeforeTravel = Player.GetMaxMana();
            ItemsBeforeTravel = Player.GetCarriedItems();
            ReadyBeforeTravel = Player.GetReadySlot();
            AttributesBeforeTravel = TArray<int32>(Player.GetAttributes());
        }

        bool MatchesTownCharacter(const ANativeWadPlayer& Player) const
        {
            return ExperienceBeforeTravel == Player.GetExperience() && HealthBeforeTravel == Player.GetHealth()
                && ManaBeforeTravel == Player.GetMana() && FoodBeforeTravel == Player.GetFood()
                && LevelBeforeTravel == Player.GetCharacterLevel() && ThresholdBeforeTravel == Player.GetExperienceNeeded()
                && MaxHealthBeforeTravel == Player.GetMaxHealth() && MaxManaBeforeTravel == Player.GetMaxMana()
                && ItemsBeforeTravel == Player.GetCarriedItems() && ReadyBeforeTravel == Player.GetReadySlot()
                && AttributesBeforeTravel == Player.GetAttributes();
        }

        bool TickL22TownRoute(UWorld& Current, ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadLevel> Level(&Current);
            const FVector2D Position(Player.GetActorLocation());
            if (!Level || Player.GetHealth() <= 0 || NextPoint >= UE_ARRAY_COUNT(L22TownRoute))
            {
                return Finish(&Player, TEXT("L22 lost its native level or living player"));
            }
            if (RiverProgressTime == 0.0)
            {
                if (!Position.Equals({-2432, -228}, 3.0))
                {
                    return Finish(&Player, TEXT("the L22 town route requires the original spawn"));
                }
                RiverProgressTime = FPlatformTime::Seconds();
            }
            const FL22RouteStep& Step = L22TownRoute[NextPoint];
            if (FPlatformTime::Seconds() - RiverProgressTime > 28.0)
            {
                UE_LOG(LogTemp, Error, TEXT("L22 route stalled at %d moving toward %s, sector %d, action %d, health %d."),
                    NextPoint, *Step.Target.ToString(), Level->FindWadSector(Position), int32(Step.Action), Player.GetHealth());
                return Finish(&Player, TEXT("the L22 town route stopped making progress"));
            }
            bool bDone = false;
            FVector2D Heights;
            switch (Step.Action)
            {
            case EL22Action::Walk:
                bDone = WalkTo(Player, FVector2D(Step.Target));
                break;
            case EL22Action::Door:
            {
                const auto* RouteDoor = Level->GetDoors().FindByPredicate([&Step](const FAAWadNativeDoor& Entry)
                    { return Entry.Sector == Step.Sector; });
                if (!RouteDoor || !Level->GetSectorSurfaceHeights(Step.Sector, Heights))
                {
                    return Finish(&Player, TEXT("the L22 route lost an authored door"));
                }
                if (!bTrailActionPressed)
                {
                    Aim(Player, Step.Target);
                    Press(Player, EKeys::E);
                    bTrailActionPressed = true;
                }
                bDone = FMath::IsNearlyEqual(Heights.Y, RouteDoor->OpenZ);
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified E opens L22 door %d to Z%.0f."), Step.Sector, Heights.Y);
                }
                break;
            }
            case EL22Action::Switch:
                if (!bTrailActionPressed)
                {
                    if (!Level->GetSectorSurfaceHeights(Step.Sector, Heights) || Heights.Y != 0.0)
                    {
                        return Finish(&Player, TEXT("the switch gate was supposed to start closed"));
                    }
                    Aim(Player, Step.Target);
                    Press(Player, EKeys::E);
                    bTrailActionPressed = true;
                }
                bDone = Level->GetSectorSurfaceHeights(Step.Sector, Heights) && Heights.Y == 100.0;
                break;
            case EL22Action::RepeatSwitch:
                if (!bTrailActionPressed)
                {
                    Aim(Player, Step.Target);
                    Press(Player, EKeys::E);
                    bTrailActionPressed = true;
                }
                bDone = Level->GetWadScriptBlockCount(1) == 1;
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L22 activity 1 raised ceiling 15 to Z100 and blocked retriggering."));
                }
                break;
            case EL22Action::Journal:
                if (TownReview != ETownReview::Failure && !bTrailActionPressed)
                {
                    Aim(Player, Step.Target);
                    Press(Player, EKeys::E);
                    bTrailActionPressed = true;
                }
                bDone = TownReview == ETownReview::Failure || Player.GetItemCount(NativeWadRules::JugurthaJournal) == 1;
                break;
            case EL22Action::Plate:
                bDone = WalkTo(Player, FVector2D(Step.Target)) && Level->FindWadSector(Position) == 163
                    && Level->GetSectorSurfaceHeights(Step.Sector, Heights) && Heights.Equals({280.0, 380.0}, 0.01);
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified stepping onto L22 sector 163 runs activity 7 and opens exit gate 193 to Z380."));
                }
                break;
            case EL22Action::Capture:
            {
                Aim(Player, Step.Target);
                bool bSaved = false;
                bDone = CaptureView(Player, Step.Frame, bSaved);
                if (bDone && !bSaved)
                {
                    return Finish(&Player, TEXT("could not capture the L22 route"));
                }
                break;
            }
            case EL22Action::Exit:
                RecordTownCharacter(Player);
                HourBeforeTravel = Level->GetTimeOfDayHours();
                if (WalkTo(Player, FVector2D(Step.Target)))
                {
                    return Finish(&Player, TEXT("L22's authored exit did not open town"));
                }
                break;
            }
            if (bDone)
            {
                ++NextPoint;
                bTrailActionPressed = false;
                RiverProgressTime = FPlatformTime::Seconds();
            }
            return true;
        }

        bool CaptureTownFrame(const TCHAR* Name, bool& bSaved)
        {
            const FString Image = FPaths::GetPath(Output) / (FPaths::GetBaseFilename(Output) + TEXT("-") + Name + TEXT(".png"));
            if (CaptureFrames++ == 0)
            {
                FScreenshotRequest::RequestScreenshot(Image, true, false);
            }
            if (CaptureFrames < 20)
            {
                return false;
            }
            CaptureFrames = 0;
            bSaved = IFileManager::Get().FileExists(*Image);
            return true;
        }

        static bool SavedL22Result(const UNativeWadTravelState& Travel)
        {
            const FWadLevelCheckpoint* Snapshot = Travel.VisitedLevels.Find(TEXT("/Game/Maps/L22"));
            return Snapshot && Snapshot->SectorHeights.FindRef(193).Y == 380.0 && Snapshot->SectorHeights.FindRef(15).Y == 100.0
                && Snapshot->Switches.FindRef(1132) && Snapshot->Variables.IsValidIndex(6)
                && Snapshot->Variables[1] == 1 && Snapshot->Variables[6] == 1
                && !Snapshot->RemainingPickups.Contains(FName(TEXT("L22 Jugurtha Journal")));
        }

        bool TickTownArrival(UWorld& Current, ANativeWadPlayer& Player, const UNativeWadTravelState& Travel)
        {
            const bool bSuccess = TownReview == ETownReview::Tour;
            if (!bArrivalCaptured)
            {
                if (bSuccess)
                {
                    ItemsBeforeTravel.RemoveSingle(NativeWadRules::JugurthaJournal);
                }
                if (!Player.HasCompletedWadLevel(TEXT("L22")) || !MatchesTownCharacter(Player)
                    || Travel.Quest.ResultQuest != 0 || Travel.Quest.bSucceeded != bSuccess || !Travel.Quest.bShowResult
                    || Travel.Quest.Gold != (bSuccess ? 10 : 0) || Travel.Quest.ShopTier != (bSuccess ? 1 : 0)
                    || (bSuccess && !SavedL22Result(Travel)))
                {
                    return Finish(&Player, TEXT("L22 returned the wrong quest result, reward, character, or visited level"));
                }
                const TCHAR* Hour = Current.URL.GetOption(TEXT("WadHour="), nullptr);
                if (!Hour || !FMath::IsNearlyEqual(FCString::Atod(Hour), HourBeforeTravel, 0.02))
                {
                    return Finish(&Player, TEXT("the town handoff changed the campaign clock"));
                }
                Camera.Reset();
                bWalking = false;
                if (bSuccess && !Player.SaveWadCheckpoint(TownReviewSlot))
                {
                    return Finish(&Player, TEXT("town checkpoint could not be written"));
                }
                NextPoint = 0;
                bArrivalCaptured = true;
            }
            bool bSaved = false;
            if (NextPoint == 0)
            {
                if (!CaptureTownFrame(bSuccess ? TEXT("quest-success") : TEXT("quest-failure"), bSaved))
                {
                    return true;
                }
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("the town quest result capture was not written"));
                }
                ++NextPoint;
                if (!bSuccess)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified returning from L22 without the journal shows failure and awards no gold."));
                    return Finish(&Player);
                }
            }
            if (Travel.Quest.bShowResult)
            {
                Press(Player, EKeys::Enter);
                return true;
            }
            if (CaptureTownFrame(TEXT("town"), bSaved))
            {
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("the town form capture was not written"));
                }
                UE_LOG(LogTemp, Display, TEXT("Completed the original L22 spawn-to-town route, turned in the journal for ten gold, and saved the original town result."));
                return Finish(&Player);
            }
            return true;
        }

        bool TickTown(UWorld& Current, ANativeWadPlayer& Player)
        {
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                if (HeldKey.GetValue() == EKeys::Q)
                {
                    SendKey(Player, EKeys::LeftAlt, IE_Released);
                }
                HeldKey.Reset();
            }
            const bool bTown = Current.GetAuthGameMode<ANativeWadTownGameMode>() != nullptr;
            const UNativeWadTravelState* Travel = Player.GetGameInstance()->GetSubsystem<UNativeWadTravelState>();
            if (!Travel)
            {
                return Finish(&Player, TEXT("native town lost its campaign state"));
            }
            if (TownReview == ETownReview::Tour || TownReview == ETownReview::Failure)
            {
                if (!bTown)
                {
                    return TickL22TownRoute(Current, Player);
                }
                return Travel->ArrivingFromMap ? true : TickTownArrival(Current, Player, *Travel);
            }
            const bool bReload = TownReview == ETownReview::Reload;
            if (bTown)
            {
                if (bReload && !Player.HasRestoredWadCheckpoint())
                {
                    return true;
                }
                if (NextPoint == 0)
                {
                    const UNativeWadTownData* Content = NativeWadTown::FindContent(Current);
                    if (!Content || Content->Quests.Num() != 7 || Content->Quests[1].Maps[0] != 25
                        || Player.HasCompletedWadLevel(TEXT("L22")) != bReload
                        || Travel->Quest.Active != INDEX_NONE || Travel->Quest.Selected != 0
                        || (bReload && (!Travel->Quest.bShowResult || !Travel->Quest.bSucceeded || Travel->Quest.Gold != 10
                            || Travel->Quest.ShopTier != 1 || Travel->Quest.ResultQuest != 0 || !SavedL22Result(*Travel))))
                    {
                        return Finish(&Player, TEXT("town did not restore the original quest state"));
                    }
                    RecordTownCharacter(Player);
                    const TCHAR* SavedHour = Current.URL.GetOption(TEXT("WadHour="), TEXT("0"));
                    HourBeforeTravel = FCString::Atod(SavedHour);
                    if (bReload)
                    {
                        const auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(TownReviewSlot, 0));
                        if (!Save)
                        {
                            return Finish(&Player, TEXT("town review checkpoint could not be read"));
                        }
                        const FURL SavedURL(nullptr, *(Save->LevelPackage + TEXT("?") + Save->PlayerOptions), TRAVEL_Absolute);
                        HourBeforeTravel = FCString::Atod(SavedURL.GetOption(TEXT("WadHour="), TEXT("0")));
                    }
                    ++NextPoint;
                }
                if (NextPoint == 1)
                {
                    if (Travel->Quest.bShowResult)
                    {
                        Press(Player, EKeys::Enter);
                        return true;
                    }
                    ++NextPoint;
                    if (bReload)
                    {
                        Press(Player, EKeys::Right);
                        return true;
                    }
                }
                if (NextPoint == 2)
                {
                    if (Travel->Quest.Selected != (bReload ? 1 : 0))
                    {
                        return Finish(&Player, TEXT("the native quest selector chose the wrong adventure"));
                    }
                    SendKey(Player, EKeys::LeftAlt, IE_Pressed);
                    Press(Player, EKeys::Q);
                    ++NextPoint;
                    return true;
                }
                if (NextPoint == 3)
                {
                    bool bSaved = false;
                    if (!CaptureTownFrame(TEXT("briefing"), bSaved))
                    {
                        return true;
                    }
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("the authored quest briefing was not captured"));
                    }
                    ++NextPoint;
                    Press(Player, EKeys::Enter);
                }
                return true;
            }
            const FString Map = Current.RemovePIEPrefix(Current.GetMapName());
            const TActorIterator<ANativeWadLevel> Level(&Current);
            const FVector2D ExpectedSpawn = bReload ? FVector2D(298, 1636) : FVector2D(1608, 432);
            if (!Level || Map != (bReload ? TEXT("L25") : TEXT("L30"))
                || !FVector2D(Player.GetActorLocation()).Equals(ExpectedSpawn, 3.0) || !MatchesTownCharacter(Player)
                || Travel->Quest.Active != (bReload ? 1 : 0))
            {
                return Finish(&Player, TEXT("accepting the original quest lost the authored spawn or character state"));
            }
            if (HourBeforeTravel > 0.0 && !FMath::IsNearlyEqual(Level->GetTimeOfDayHours(), HourBeforeTravel, 0.02))
            {
                return Finish(&Player, TEXT("the original quest handoff changed the campaign clock"));
            }
            bool bSaved = false;
            if (!CaptureView(Player, TEXT("arrival"), bSaved))
            {
                return true;
            }
            if (!bSaved)
            {
                return Finish(&Player, TEXT("failed to capture the authored quest spawn"));
            }
            UE_LOG(LogTemp, Display, TEXT("Verified the original town briefing starts %s at its authored spawn with character state intact."), *Map);
            return Finish(&Player);
        }

        bool ReadyInventoryItem(ANativeWadPlayer& Player, int32 Type, bool bReleasedKey)
        {
            if (Player.GetReadyItem() == Type)
            {
                if (!Player.IsInventoryOpen())
                {
                    return true;
                }
                if (!bRequestedInventoryFrame)
                {
                    const FString Path = FPaths::GetPath(Output) / (FPaths::GetBaseFilename(Output) + TEXT("-ready-hand.png"));
                    FScreenshotRequest::RequestScreenshot(Path, true, false);
                    bRequestedInventoryFrame = true;
                    InventoryFrameHold = 8;
                }
                if (InventoryFrameHold > 0)
                {
                    --InventoryFrameHold;
                    return false;
                }
                if (!bReleasedKey)
                {
                    Press(Player, EKeys::NumPadSeven);
                }
                return false;
            }
            if (!Player.IsInventoryOpen())
            {
                if (!bReleasedKey)
                {
                    Press(Player, EKeys::NumPadSeven);
                }
                return false;
            }
            const int32 Slot = Player.GetCarriedItems().Find(Type);
            auto* Controller = Cast<APlayerController>(Player.GetController());
            auto* HUD = Controller ? Cast<ANativeWadHUD>(Controller->GetHUD()) : nullptr;
            if (HUD && Slot != INDEX_NONE)
            {
                const int32 Button = Player.GetSelectedInventorySlot() == Slot ? INDEX_NONE : Slot;
                // Drive the same screen-space hit test used by a mouse click, including in offscreen runs.
                HUD->ClickInventory(HUD->InventorySlotCenter(Button));
            }
            return false;
        }

        bool TimedDoorBlocks(UWorld& World, ANativeWadPlayer& Player)
        {
            FHitResult Hit;
            const FCollisionQueryParams Query(SCENE_QUERY_STAT(ReviewWadTimedDoor), true, &Player);
            return World.SweepSingleByChannel(Hit, FVector(8,1072,31), FVector(8,1008,31), FQuat::Identity, ECC_Pawn,
                FCollisionShape::MakeCapsule(ANativeWadPlayer::CapsuleRadius, ANativeWadPlayer::CapsuleHalfHeight), Query);
        }

        bool TickSlice(UWorld& Current, ANativeWadPlayer& Player)
        {
            using namespace NativeWadRules;
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || Player.GetHealth() == 0)
            {
                return Finish(&Player, TEXT("the playable slice lost its native level or player"));
            }
            if (NextPoint == 0)
            {
                SliceProgressTime = Current.GetTimeSeconds();
                StartingExperience = Player.GetExperience();
                FVector2D Gate;
                const FVector2D Spawn = bSecondMap ? FVector2D(176,3464) : FVector2D(1608,432);
                const bool bStartingSector = bSecondMap
                    ? Level->GetSectorSurfaceHeights(31, Gate) && Gate.X == -250.0
                    : Level->GetSectorSurfaceHeights(129, Gate) && Gate.Y == 0.0;
                if (!FVector2D(Player.GetActorLocation()).Equals(Spawn, 2.0) || !bStartingSector
                    || Player.GetItemCount(Berry) != 0 || Player.GetCharacterLevel() != 1 || Player.GetMana() != MaxMana)
                {
                    return Finish(&Player, TEXT("the slice did not begin at the original spawn with its initial sector heights"));
                }
            }
            bool bReleasedKey = HeldKey.IsSet();
            if (bReleasedKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            const TConstArrayView<FSliceStep> Route = bSecondMap ? MakeArrayView(L151Route) : MakeArrayView(SliceRoute);
            if (!Route.IsValidIndex(NextPoint))
            {
                return Finish(&Player, TEXT("no route steps remain before travel"));
            }
            const FSliceStep& Step = Route[NextPoint];
            if (Current.GetTimeSeconds() - SliceProgressTime > (Step.Action == EAction::Guardian ? 55.0 : 12.0))
            {
                UE_LOG(LogTemp, Warning, TEXT("Slice stalled on action %d, target %s, sector %d, height %.0f."),
                    int32(Step.Action), *Step.Target.ToString(), Step.Sector, Step.Height);
                return Finish(&Player, TEXT("the player did not reach the next slice checkpoint"));
            }
            bool bDone = false;
            bool bSaved = false;
            switch (Step.Action)
            {
            case EAction::Walk:
                bDone = WalkTo(Player, FVector2D(Step.Target));
                break;
            case EAction::Use:
                if (!bReleasedKey)
                {
                    Aim(Player, Step.Target);
                    Press(Player, EKeys::E);
                    bDone = true;
                }
                break;
            case EAction::Ceiling:
            {
                FVector2D Heights;
                bDone = Level->GetSectorSurfaceHeights(Step.Sector, Heights) && Heights.Y == Step.Height;
                break;
            }
            case EAction::Ranged:
            case EAction::Cover:
            {
                const TActorIterator<ANativeWadGuardian> Guardian(&Current);
                if (!Guardian)
                {
                    return Finish(&Player, TEXT("the imported Guardian is missing"));
                }
                if (Step.Action == EAction::Ranged)
                {
                    bDone = Guardian->GetBoltsFired() > 0 && Player.GetHealth() == MaxHealth - 300;
                    if (bDone)
                    {
                        UE_LOG(LogTemp, Display, TEXT("Verified an original Guardian bolt hit for 300 damage."));
                    }
                    break;
                }
                if (Guardian->CanSeePlayer(Player))
                {
                    return Finish(&Player, TEXT("the closed door did not block Guardian sight"));
                }
                if (CoverBolts == INDEX_NONE)
                {
                    CoverBolts = Guardian->GetBoltsFired();
                    CoverStart = Current.GetTimeSeconds();
                }
                if (Guardian->GetBoltsFired() != CoverBolts)
                {
                    return Finish(&Player, TEXT("the Guardian fired after losing sight"));
                }
                bDone = Current.GetTimeSeconds() - CoverStart >= 5.2;
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified no Guardian shots through closed door 144 over a full bolt cooldown."));
                }
                break;
            }
            case EAction::Rejuvenate:
                if (Player.GetSuccessfulCasts() == 1)
                {
                    const int32 Restored = FMath::Min(MaxHealth, HealthBeforeCast + RejuvenatePower);
                    if (Player.GetMana() != ManaBeforeCast - RejuvenateCost || Player.GetHealth() != Restored)
                    {
                        return Finish(&Player, TEXT("Rejuvenate did not use the source mana cost and healing"));
                    }
                    bDone = true;
                }
                else if (!bReleasedKey)
                {
                    if (Player.GetRunes().IsEmpty())
                    {
                        Press(Player, EKeys::NumPadFour);
                    }
                    else if (Player.GetRunes() == TEXT("4"))
                    {
                        Press(Player, EKeys::NumPadFive);
                    }
                    else if (Player.GetRunes() == TEXT("45"))
                    {
                        HealthBeforeCast = Player.GetHealth();
                        ManaBeforeCast = Player.GetMana();
                        Press(Player, EKeys::RightMouseButton);
                    }
                }
                break;
            case EAction::Equip:
                bDone = ReadyInventoryItem(Player, Step.Sector, bReleasedKey);
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Readied item %d through the inventory slot and ready-hand box."), Step.Sector);
                }
                break;
            case EAction::Guardian:
            {
                const TActorIterator<ANativeWadGuardian> It(&Current);
                if (!It)
                {
                    return Finish(&Player, TEXT("the imported Guardian is missing"));
                }
                ANativeWadGuardian* Guardian = *It;
                if (bSecondMap && Player.GetExperience() > StartingExperience)
                {
                    UE_LOG(LogTemp, Display, TEXT("Hit L151's authored %s with the readied dagger. XP %d."),
                        *Guardian->GetEncounterName(), Player.GetExperience());
                    bDone = true;
                    break;
                }
                if (Guardian->IsDefeated())
                {
                    const int32 Earned = Player.GetExperience() - StartingExperience;
                    if (Guardian->GetMeleeHits() == 0 || Earned != GuardianHealth + Guardian->GetRegeneratedHealth())
                    {
                        return Finish(&Player, TEXT("the Guardian melee or source damage-to-experience check failed"));
                    }
                    if (StartingExperience > 0)
                    {
                        const TConstArrayView<int32> Attributes = Player.GetAttributes();
                        int32 Gain = 0;
                        for (int32 Index = 0; Index < AttributeCount; ++Index)
                        {
                            if (Attributes[Index] < PriestAttributes[Index] + 2)
                            {
                                return Finish(&Player, TEXT("a priest attribute missed its guaranteed level gain"));
                            }
                            Gain += Attributes[Index] - PriestAttributes[Index];
                        }
                        const int32 NextThreshold = FirstLevelThreshold + 2000 + 10000 * 2 + 3000 * 4;
                        if (Player.GetExperience() < FirstLevelThreshold || Player.GetCharacterLevel() != 2
                            || Player.GetExperienceNeeded() != NextThreshold || Gain != 18
                            || Player.GetMaxHealth() != MaxHealth + (Attributes[ConstitutionAttribute] + 10) * 10
                            || Player.GetMaxMana() != MaxMana + (Attributes[MagicAttribute] + 10) * 10)
                        {
                            return Finish(&Player, TEXT("the source level-up gains did not apply at the first threshold"));
                        }
                        UE_LOG(LogTemp, Display, TEXT("Verified first priest level-up with 18 attribute points and source health/mana gains."));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified Guardian melee and %d XP from 700 starting health plus %d recovered health."),
                        Earned, Guardian->GetRegeneratedHealth());
                    bDone = true;
                    break;
                }
                if (FVector::Dist2D(Player.GetActorLocation(), Guardian->GetActorLocation()) > 52.0)
                {
                    WalkTo(Player, FVector2D(Guardian->GetActorLocation()));
                    break;
                }
                Stop(Player);
                Aim(Player, Guardian->GetActorLocation());
                if (!bCapturedEncounter)
                {
                    if (!CaptureView(Player, TEXT("guardian"), bSaved))
                    {
                        break;
                    }
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the Guardian"));
                    }
                    bCapturedEncounter = true;
                }
                if (!bReleasedKey)
                {
                    Press(Player, bSecondMap ? EKeys::LeftMouseButton : EKeys::LeftControl);
                }
                break;
            }
            case EAction::Pickup:
            {
                const int32 Type = Step.Sector;
                if (Player.GetItemCount(Type) == 1)
                {
                    bDone = true;
                    break;
                }
                TActorIterator<ANativeWadPickup> Pickup(&Current);
                while (Pickup && Pickup->GetItemType() != Type)
                {
                    ++Pickup;
                }
                if (!Pickup)
                {
                    return Finish(&Player, TEXT("the authored pickup is missing"));
                }
                if (FVector::Dist2D(Player.GetActorLocation(), Pickup->GetActorLocation()) > 40.0)
                {
                    WalkTo(Player, FVector2D(Pickup->GetActorLocation()));
                    break;
                }
                Stop(Player);
                Aim(Player, Pickup->GetActorLocation());
                if (CapturedItem != Type)
                {
                    if (!CaptureView(Player, Step.Frame, bSaved))
                    {
                        break;
                    }
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the authored pickup"));
                    }
                    CapturedItem = Type;
                }
                if (!bReleasedKey)
                {
                    Press(Player, EKeys::E);
                }
                break;
            }
            case EAction::EatBerry:
                if (Player.GetItemCount(Berry) == 0 && FoodBeforeBerry != INDEX_NONE)
                {
                    if (Player.GetFood() != FMath::Min(MaxFood, FoodBeforeBerry + BerryFood))
                    {
                        return Finish(&Player, TEXT("the berry did not restore the source food amount"));
                    }
                    bDone = true;
                }
                else if (!bReleasedKey)
                {
                    FoodBeforeBerry = Player.GetFood();
                    Press(Player, EKeys::LeftMouseButton);
                }
                break;
            case EAction::TimedClosed:
                if (!TimedDoorBlocks(Current, Player))
                {
                    return Finish(&Player, TEXT("the closed timed door did not block the player capsule"));
                }
                bDone = true;
                break;
            case EAction::TimedReset:
            {
                FVector2D Heights;
                Level->GetSectorSurfaceHeights(166, Heights);
                const double Age = Current.GetTimeSeconds() - SliceProgressTime;
                if (TimedDoorStarted == 0.0 && Heights.Y > 0.0)
                {
                    TimedDoorStarted = Age;
                }
                if (Age >= 360.0 / TicksPerSecond + 0.15 && Heights.Y == 0.0)
                {
                    if (Level->GetWadScriptBlockCount(17) != 1
                        || !FMath::IsNearlyEqual(TimedDoorStarted, 220.0 / TicksPerSecond, 0.12))
                    {
                        return Finish(&Player, TEXT("the timed switch did not honor its original delay and repeat block"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified activity 17 blocked a repeat and sector 166 opened on the source delay."));
                    bDone = true;
                }
                break;
            }
            case EAction::TrapStart:
                if (!bCampaign)
                {
                    bDone = true;
                }
                else if (!bReleasedKey)
                {
                    TrapMissilesBefore = Level->GetScriptMissileCount();
                    TrapBlockedBefore = Level->GetScriptMissilesBlocked();
                    TrapStarted = Current.GetTimeSeconds();
                    Aim(Player, Step.Target);
                    Press(Player, EKeys::E);
                    bDone = true;
                }
                break;
            case EAction::TrapCycle:
            {
                const double Elapsed = Current.GetTimeSeconds() - TrapStarted;
                const int32 Launched = Level->GetScriptMissileCount() - TrapMissilesBefore;
                if (bCampaign && Launched > 0 && Elapsed < 360.0 / 70.0 - 0.09)
                {
                    return Finish(&Player, TEXT("the trap launched before the source delay"));
                }
                bDone = !bCampaign || (Launched == 2 && Level->GetScriptMissilesBlocked() == TrapBlockedBefore + 2);
                if (bCampaign && bDone)
                {
                    if (Elapsed > 360.0 / 70.0 + 0.65)
                    {
                        return Finish(&Player, TEXT("the trap did not launch and hit geometry on time"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified the authored trap's two shots hit geometry after %.3fs."), Elapsed);
                }
                break;
            }
            case EAction::PressureOpen:
                for (const int32 Sector : {165, 166, 168})
                {
                    FVector2D Heights;
                    if (!Level->GetSectorSurfaceHeights(Sector, Heights) || Heights.Y != 120)
                    {
                        return Finish(&Player, TEXT("the imported pressure plate did not keep its timed doors open"));
                    }
                }
                if (TimedDoorBlocks(Current, Player))
                {
                    return Finish(&Player, TEXT("the opened timed door still blocked the player capsule"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified the imported pressure plate holds all three doors open with clear collision."));
                bDone = true;
                break;
            case EAction::GateOpen:
            {
                FVector2D Gate;
                FVector2D Floor;
                if (!Level->GetSectorSurfaceHeights(129, Gate) || !Level->GetSectorSurfaceHeights(176, Floor))
                {
                    return Finish(&Player, TEXT("the imported activity has no floor or gate sector"));
                }
                const double Age = Current.GetTimeSeconds() - SliceProgressTime;
                if (GateFloorStarted == 0.0 && Floor.X < 80.0)
                {
                    GateFloorStarted = Age;
                }
                if (GateCeilingStarted == 0.0 && Gate.Y > 0.0)
                {
                    GateCeilingStarted = Age;
                }
                bDone = Gate.Y == 120.0 && Floor.X == 0.0;
                if (bDone)
                {
                    constexpr double TimerTolerance = 0.085;
                    if (!FMath::IsNearlyEqual(GateFloorStarted, 100.0 / TicksPerSecond, TimerTolerance)
                        || !FMath::IsNearlyEqual(GateCeilingStarted, 200.0 / TicksPerSecond, TimerTolerance))
                    {
                        return Finish(&Player, TEXT("the imported activity did not start its slides at the source times"));
                    }
                    if (bCampaign && !Player.IsWadEarthquakeActive())
                    {
                        return Finish(&Player, TEXT("the exit activity did not start the authored earthquake"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified imported activity motion at %.3fs and %.3fs. Gate 129 Z120, floor 176 Z0."),
                        GateFloorStarted, GateCeilingStarted);
                }
                break;
            }
            case EAction::Capture:
                if (!Step.Target.IsZero())
                {
                    Aim(Player, Step.Target);
                }
                bDone = CaptureView(Player, Step.Frame, bSaved);
                if (bDone && !bSaved)
                {
                    return Finish(&Player, TEXT("could not save the slice frame"));
                }
                break;
            case EAction::Checkpoint:
                bDone = !bCampaign || Player.SaveWadCheckpoint(TEXT("NativeWadCheckpointReview"));
                break;
            case EAction::Lift:
            {
                FVector2D Heights;
                const double Feet = Player.GetActorLocation().Z - ANativeWadPlayer::CapsuleHalfHeight;
                bDone = Level->GetSectorSurfaceHeights(Step.Sector, Heights) && Heights.X == Step.Height
                    && FMath::IsNearlyEqual(Feet, Step.Height, 5.0);
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L151 lift sector %d carries the player at floor Z%.0f."),
                        Step.Sector, Step.Height);
                }
                break;
            }
            case EAction::Travel:
                ExperienceBeforeTravel = Player.GetExperience();
                HealthBeforeTravel = Player.GetHealth();
                ManaBeforeTravel = Player.GetMana();
                FoodBeforeTravel = Player.GetFood();
                LevelBeforeTravel = Player.GetCharacterLevel();
                ThresholdBeforeTravel = Player.GetExperienceNeeded();
                MaxHealthBeforeTravel = Player.GetMaxHealth();
                MaxManaBeforeTravel = Player.GetMaxMana();
                AttributesBeforeTravel.Reset();
                AttributesBeforeTravel.Append(Player.GetAttributes().GetData(), Player.GetAttributes().Num());
                if (WalkTo(Player, FVector2D(Step.Target)))
                {
                    return Finish(&Player, TEXT("the player reached the exit marker without traveling"));
                }
                break;
            case EAction::Complete:
                UE_LOG(LogTemp, Display, TEXT("Completed authored L151 encounter, inventory, and lift review."));
                return Finish(&Player);
            }
            if (bDone)
            {
                ++NextPoint;
                SliceProgressTime = Current.GetTimeSeconds();
            }
            return true;
        }

        bool TickL301(UWorld& Current, ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadGuardian> Guardian(&Current);
            if (!Guardian || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the L301 encounter is missing or the player was defeated"));
            }
            if (Player.GetExperience() > ExperienceBeforeTravel)
            {
                UE_LOG(LogTemp, Display, TEXT("Completed the continuous campaign review. The carried dagger hit L301 Guardian thing 246 for %d XP."),
                    Player.GetExperience() - ExperienceBeforeTravel);
                return Finish(&Player);
            }
            if (NextPoint == 0)
            {
                NextPoint += WalkTo(Player, FVector2D(3046, 3218)) ? 1 : 0;
                return true;
            }
            if (FVector::Dist2D(Player.GetActorLocation(), Guardian->GetActorLocation()) > 50.0)
            {
                WalkTo(Player, FVector2D(Guardian->GetActorLocation()));
                return true;
            }
            Stop(Player);
            Aim(Player, Guardian->GetActorLocation());
            if (!bCapturedEncounter)
            {
                bool bSaved = false;
                if (!CaptureView(Player, TEXT("l301-guardian"), bSaved))
                {
                    return true;
                }
                if (!bSaved)
                {
                    return Finish(&Player, TEXT("could not save the L301 encounter frame"));
                }
                bCapturedEncounter = true;
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            else
            {
                Press(Player, EKeys::LeftControl);
            }
            return true;
        }

        bool TickCheckpoint(UWorld& Current, ANativeWadPlayer& Player)
        {
            if (!bCheckpointVerified)
            {
                if (!Player.HasRestoredWadCheckpoint())
                {
                    return FPlatformTime::Seconds() - StartTime < 5.0
                        || Finish(&Player, TEXT("the new Unreal process did not restore the checkpoint"));
                }
                const auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(
                    TEXT("NativeWadCheckpointReview"), 0));
                const TActorIterator<ANativeWadLevel> Level(&Current);
                const TActorIterator<ANativeWadGuardian> Guardian(&Current);
                const TActorIterator<ANativeWadPickup> Pickup(&Current);
                FVector2D Gate, Floor;
                if (!Save || !Level || !Guardian || Pickup || !Guardian->IsDefeated()
                    || Player.GetItemCount(NativeWadRules::IronDagger) != 1 || Player.GetItemCount(NativeWadRules::Berry) != 0
                    || Player.GetReadyItem() != NativeWadRules::IronDagger || !Player.GetActorLocation().Equals(Save->Position, 3.0)
                    || !Level->GetSectorSurfaceHeights(129, Gate) || Gate.Y != 120
                    || !Level->GetSectorSurfaceHeights(176, Floor) || Floor.X != 0)
                {
                    return Finish(&Player, TEXT("restored checkpoint differs from the saved items, encounter, position, or sector heights"));
                }
                const FURL Options(nullptr, *(Save->LevelPackage + TEXT("?") + Save->PlayerOptions), TRAVEL_Absolute);
                const auto SavedStat = [&Options](const TCHAR* Key) { return FCString::Atoi(Options.GetOption(Key, TEXT("-1"))); };
                if (Player.GetHealth() != SavedStat(TEXT("WadHealth=")) || Player.GetMana() != SavedStat(TEXT("WadMana="))
                    || Player.GetFood() != SavedStat(TEXT("WadFood=")) || Player.GetExperience() != SavedStat(TEXT("WadXP="))
                    || Player.GetCharacterLevel() != SavedStat(TEXT("WadLevel="))
                    || Player.GetMaxHealth() != SavedStat(TEXT("WadMaxHealth=")) || Player.GetMaxMana() != SavedStat(TEXT("WadMaxMana=")))
                {
                    return Finish(&Player, TEXT("the character's stats changed after process restart"));
                }
                const int32 PreviousBlocks = Level->GetWadScriptBlockCount(40);
                if (!Player.Use() || Level->GetWadScriptBlockCount(40) != PreviousBlocks + 1)
                {
                    return Finish(&Player, TEXT("the restored exit switch did not block a repeated activation"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified disk checkpoint restored character stats, ready dagger, removed pickups, defeated Guardian, raised gate and exit switch lock."));
                bCheckpointVerified = true;
            }
            bool bSaved = false;
            return !CaptureView(Player, TEXT("restored"), bSaved)
                || Finish(&Player, bSaved ? nullptr : TEXT("could not save the restored checkpoint frame"));
        }

        bool TickRiver(UWorld& Current, ANativeWadPlayer& Player)
        {
            constexpr int32 RiverSector = 37;
            constexpr double RiverSurface = 32.0;
            constexpr const TCHAR* RiverSlot = TEXT("NativeWadRiverReview");
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the river review lost its native level or player"));
            }
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            if (RiverProgressTime == 0.0)
            {
                RiverProgressTime = FPlatformTime::Seconds();
            }
            if (FPlatformTime::Seconds() - RiverProgressTime > 42.0)
            {
                UE_LOG(LogTemp, Warning, TEXT("River review stopped at phase %d, waypoint %d, sector %d."),
                    int32(RiverStage), NextPoint, Level->FindWadSector(FVector2D(Player.GetActorLocation())));
                return Finish(&Player, TEXT("the river route stopped making progress"));
            }
            const auto Advance = [this](ERiverStage Next)
            {
                RiverStage = Next;
                RiverProgressTime = FPlatformTime::Seconds();
            };
            const bool bIsL531 = Current.GetMapName().EndsWith(TEXT("L531"));
            const FVector Position = Player.GetActorLocation();
            const FVector2D XY(Position);
            if (&Current != SourceWorld.Get() && RiverStage == ERiverStage::Exit)
            {
                if (!bIsL531 || !XY.Equals(FVector2D(-144, -232), 3.0) || Player.GetExperience() != ExperienceBeforeTravel
                    || Player.GetHealth() != HealthBeforeTravel || Player.GetEquippedWeapon() != NativeWadRules::IronDagger)
                {
                    return Finish(&Player, TEXT("the authored L301 link did not bring the same player to the L531 spawn"));
                }
                Camera.Reset();
                bWalking = false;
                Advance(ERiverStage::Arrival);
                UE_LOG(LogTemp, Display, TEXT("Verified L301 thing 304 travels to the original L531 spawn, with the same XP, health and ready dagger."));
            }
            bool bSaved = false;
            switch (RiverStage)
            {
            case ERiverStage::Spawn:
                if (bRiverReload)
                {
                    if (!Player.HasRestoredWadCheckpoint())
                    {
                        return true;
                    }
                    if (!bIsL531 || !XY.Equals(FVector2D(1263, 1200), 4.0))
                    {
                        return Finish(&Player, TEXT("L531's post-teleport checkpoint did not survive restart"));
                    }
                    ExperienceBeforeTravel = Player.GetExperience();
                    HealthBeforeTravel = Player.GetHealth();
                    Advance(ERiverStage::Return);
                    Player.TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Maps/L301.L301"))));
                    break;
                }
                if (!XY.Equals(FVector2D(3440, 2944), 3.0) || Player.GetEquippedWeapon() != NativeWadRules::IronDagger)
                {
                    return Finish(&Player, TEXT("the river run requires the original L301 spawn with a ready dagger"));
                }
                Advance(ERiverStage::Encounter);
                break;
            case ERiverStage::Encounter:
            {
                const TActorIterator<ANativeWadGuardian> Encounter(&Current);
                if (!Encounter)
                {
                    return Finish(&Player, TEXT("L301 did not spawn its authored Guardian"));
                }
                if (Encounter->IsDefeated())
                {
                    Stop(Player);
                    Advance(ERiverStage::RiverBank);
                    break;
                }
                if (FVector::Dist2D(Position, Encounter->GetActorLocation()) > 50.0)
                {
                    const FVector2D Approach = Encounter->CanSeePlayer(Player)
                        ? FVector2D(Encounter->GetActorLocation()) : FVector2D(3046, 3218);
                    WalkTo(Player, Approach);
                }
                else
                {
                    Stop(Player);
                    Aim(Player, Encounter->GetActorLocation());
                    Press(Player, EKeys::LeftControl);
                }
                break;
            }
            case ERiverStage::RiverBank:
                if (NextPoint == 0)
                {
                    if (WalkTo(Player, FVector2D(2360, 3220)))
                    {
                        ++NextPoint;
                        RiverProgressTime = FPlatformTime::Seconds();
                    }
                    break;
                }
                if (WalkTo(Player, FVector2D(2256, 3168)))
                {
                    NextPoint = 0;
                    Advance(ERiverStage::Drift);
                }
                break;
            case ERiverStage::Drift:
            {
                const FAAWadNativeSector& Water = Level->GetSectors()[RiverSector];
                const double Feet = Position.Z - ANativeWadPlayer::CapsuleHalfHeight;
                if (Level->FindWadSector(XY) != RiverSector || !Water.bWater || Water.FloorDepth != 20
                    || Level->GetWalkingHeight(RiverSector) != RiverSurface - 20 || !Water.Floor
                    || Water.FloorScrollRate != FVector2D(-87.5, -87.5)
                    || Level->GetCurrentInput(Position + FVector(0, 0, 30)) != FVector2D::ZeroVector)
                {
                    return Finish(&Player, TEXT("water depth, scroll, or above-water current differs from the imported marker"));
                }
                if (!FMath::IsNearlyEqual(Feet, RiverSurface - 20, 3.5))
                {
                    break;
                }
                const TArray<float>& UV = Water.Floor->GetCustomPrimitiveData().Data;
                if (!UV.IsValidIndex(3))
                {
                    return Finish(&Player, TEXT("the water has no runtime texture offset"));
                }
                if (DriftStarted == 0.0)
                {
                    DriftStarted = Current.GetTimeSeconds();
                    DriftStart = Position;
                    InitialWaterUV = FVector2D(UV[2], UV[3]);
                }
                if (Current.GetTimeSeconds() - DriftStarted >= 0.85)
                {
                    const FVector Offset = Position - DriftStart;
                    if (Offset.X > -10.0 || Offset.Y > -10.0 || InitialWaterUV.Equals(FVector2D(UV[2], UV[3])))
                    {
                        return Finish(&Player, TEXT("the moving water did not move the idle player southwest and scroll the surface"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L301 sector 37's water depth, southwest drift (%s), and moving texture."), *Offset.ToString());
                    Advance(ERiverStage::WaterView);
                }
                break;
            }
            case ERiverStage::WaterView:
                Aim(Player, {1616, 2980, 35});
                if (CaptureView(Player, TEXT("current"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not save the water screenshot"));
                    }
                    Advance(ERiverStage::Route);
                }
                break;
            case ERiverStage::Route:
            {
                const FVector2D Waypoints[] = {{1560,2920}, {960,2288}, {712,1144}, {696,760},
                    {688,656}, {672,600}, {384,32}, {320,-40}, {145,-193}};
                if (WalkTo(Player, Waypoints[NextPoint]))
                {
                    RiverProgressTime = FPlatformTime::Seconds();
                    if (++NextPoint == UE_ARRAY_COUNT(Waypoints))
                    {
                        Advance(ERiverStage::Ledge);
                    }
                }
                break;
            }
            case ERiverStage::Ledge:
                if (Level->FindWadSector(XY) == 16 && Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    Aim(Player, FVector(135, -300, Player.GetPawnViewLocation().Z));
                    Walk(Player);
                    if (Player.GetVelocity().Size2D() > 95.0 || XY.Y < -206.0)
                    {
                        Press(Player, EKeys::SpaceBar);
                        bJumpStarted = false;
                        Advance(ERiverStage::Jump);
                    }
                }
                break;
            case ERiverStage::Jump:
                bJumpStarted |= Player.GetCharacterMovement()->IsFalling();
                if (WalkTo(Player, {135,-300}) && bJumpStarted && Level->FindWadSector(XY) == 47
                    && Player.GetCharacterMovement()->IsMovingOnGround())
                {
                    NextPoint = 0;
                    Advance(ERiverStage::Exit);
                    UE_LOG(LogTemp, Display, TEXT("Verified a jump from L301 sector 16 to the raised bank in sector 47."));
                }
                break;
            case ERiverStage::Exit:
                if (NextPoint == 0)
                {
                    if (WalkTo(Player, {-184,-940}))
                    {
                        ++NextPoint;
                    }
                    break;
                }
                ExperienceBeforeTravel = Player.GetExperience();
                HealthBeforeTravel = Player.GetHealth();
                if (WalkTo(Player, {-184,-1058}))
                {
                    return Finish(&Player, TEXT("the L301 exit marker did not travel"));
                }
                break;
            case ERiverStage::Arrival:
                if (!CaptureView(Player, TEXT("l531-arrival"), bSaved))
                {
                    break;
                }
                // The L301 route ends at the authored L531 spawn. Start the separate teleport check by its hallway.
                if (!bSaved || !Player.TeleportTo({1575, 3510, 45}, FRotator::ZeroRotator))
                {
                    return Finish(&Player, TEXT("could not stage the L531 teleport check"));
                }
                Advance(ERiverStage::Teleport);
                break;
            case ERiverStage::Teleport:
                if (XY.Equals(FVector2D(1263, 1200), 6.0))
                {
                    Stop(Player);
                    Advance(ERiverStage::Save);
                }
                else if (WalkTo(Player, {1650,3510}))
                {
                    return Finish(&Player, TEXT("walking into L531 sector 21 did not run its teleport activity"));
                }
                break;
            case ERiverStage::Save:
                if (!CaptureView(Player, TEXT("l531-teleport"), bSaved))
                {
                    break;
                }
                if (!bSaved || !Player.SaveWadCheckpoint(RiverSlot))
                {
                    return Finish(&Player, TEXT("could not save the settled L531 checkpoint"));
                }
                ExperienceBeforeTravel = Player.GetExperience();
                HealthBeforeTravel = Player.GetHealth();
                Player.TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Maps/L301.L301"))));
                Advance(ERiverStage::Return);
                break;
            case ERiverStage::Return:
                if (!bIsL531 && Current.GetTimeSeconds() >= 0.1)
                {
                    const TActorIterator<ANativeWadGuardian> Encounter(&Current);
                    if (!Encounter || !Encounter->IsDefeated() || Player.GetExperience() != ExperienceBeforeTravel
                        || Player.GetHealth() != HealthBeforeTravel || Player.GetEquippedWeapon() != NativeWadRules::IronDagger)
                    {
                        return Finish(&Player, TEXT("revisiting L301 did not recover its defeated Guardian and carried character"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified revisiting L301 %s restores its defeated Guardian and carried character."),
                        bRiverReload ? TEXT("after process restart") : TEXT("in the same session"));
                    return Finish(&Player);
                }
                break;
            }
            return true;
        }

        bool TickRavine(UWorld& Current, ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || Player.GetHealth() <= 0)
            {
                return Finish(&Player, TEXT("the ravine review lost its native level or player"));
            }
            const FVector2D Position(Player.GetActorLocation());
            if (HeldKey)
            {
                SendKey(Player, HeldKey.GetValue(), IE_Released);
                HeldKey.Reset();
            }
            if (!bRavineStarted)
            {
                const FVector2D Start = bL22Journey ? FVector2D(2544, 2775) : FVector2D(-144, -232);
                if (!Position.Equals(Start, 3.0) || Player.GetEquippedWeapon() != NativeWadRules::IronDagger)
                {
                    return Finish(&Player, TEXT("the route requires the original player spawn with a ready dagger"));
                }
                if (bL22Journey)
                {
                    Trail.Append(L31Trail, UE_ARRAY_COUNT(L31Trail));
                    Trail.Append(L31ExitTrail, UE_ARRAY_COUNT(L31ExitTrail));
                }
                else
                {
                    Trail.Append(L531Trail, UE_ARRAY_COUNT(L531Trail));
                }
                bRavineStarted = true;
                RiverProgressTime = FPlatformTime::Seconds();
            }
            if (!bTrailArrived && &Current != SourceWorld.Get())
            {
                const TCHAR* Destination = bL22Journey ? TEXT("L22") : TEXT("L31");
                const FVector2D Spawn = bL22Journey ? FVector2D(-2432, -228) : FVector2D(2544, 2775);
                if (!Current.GetMapName().EndsWith(Destination) || !Position.Equals(Spawn, 3.0)
                    || Player.GetExperience() != ExperienceBeforeTravel || Player.GetHealth() != HealthBeforeTravel
                    || Player.GetFood() != FoodBeforeTravel || Player.GetMana() != ManaBeforeTravel
                    || Player.GetEquippedWeapon() != NativeWadRules::IronDagger)
                {
                    return Finish(&Player, TEXT("the exit did not preserve the character at the original destination spawn"));
                }
                Camera.Reset();
                bWalking = false;
                bTrailArrived = true;
                Trail.Reset();
                if (bL22Journey)
                {
                    Trail.Append(L22Trail, UE_ARRAY_COUNT(L22Trail));
                }
                else
                {
                    Trail.Append(L31Trail, UE_ARRAY_COUNT(L31Trail));
                }
                NextPoint = 0;
                RiverProgressTime = FPlatformTime::Seconds();
                UE_LOG(LogTemp, Display, TEXT("Verified the authored exit travels to the original %s spawn with character state intact."), Destination);
            }
            if (bL22Journey && !bTrailArrived && !bObservedStartupLiftReturn)
            {
                FVector2D Heights;
                if (!Level->GetSectorSurfaceHeights(136, Heights))
                {
                    return Finish(&Player, TEXT("the original startup lift is missing"));
                }
                if (!bObservedStartupLiftTop && Heights.X == 188.0)
                {
                    if (!FMath::IsNearlyEqual(Current.GetTimeSeconds(), 3.0, 0.15))
                    {
                        return Finish(&Player, TEXT("the L31 startup lift reached its top at the wrong time"));
                    }
                    bObservedStartupLiftTop = true;
                }
                else if (bObservedStartupLiftTop && Heights.X == 88.0)
                {
                    if (!FMath::IsNearlyEqual(Current.GetTimeSeconds(), 9.0, 0.15))
                    {
                        return Finish(&Player, TEXT("the L31 startup lift returned at the wrong time"));
                    }
                    bObservedStartupLiftReturn = true;
                    UE_LOG(LogTemp, Display, TEXT("Verified L31 startup lift 136 reached Z188 after 3s and returned to Z88 after 9s."));
                }
            }
            if (!Trail.IsValidIndex(NextPoint))
            {
                UE_LOG(LogTemp, Display, TEXT("%s"), bL22Journey
                    ? TEXT("Completed the continuous L31 to L22 review, including the first L22 door.")
                    : TEXT("Completed the L531 to L31 ravine review, including L31's initial door and pressure plate."));
                return Finish(&Player);
            }
            const FTrailStep& Step = Trail[NextPoint];
            if (FPlatformTime::Seconds() - RiverProgressTime > 23.0)
            {
                UE_LOG(LogTemp, Warning, TEXT("Ravine route stalled in %s at waypoint %d, target %s, sector %d, velocity %s."),
                    *Current.GetMapName(), NextPoint, *Step.Target.ToString(), Level->FindWadSector(Position), *Player.GetVelocity().ToString());
                return Finish(&Player, TEXT("the ravine route stopped making progress"));
            }
            bool bDone = false;
            switch (Step.Action)
            {
            case ETrailAction::Walk:
                bDone = WalkTo(Player, FVector2D(Step.Target));
                break;
            case ETrailAction::Leap:
            {
                const FVector2D Direction = (FVector2D(Step.Target) - Step.Edge).GetSafeNormal();
                if (!bTrailActionPressed && FVector2D::DotProduct(Step.Edge - Position, Direction) <= 72.0
                    && Player.GetVelocity().Size2D() >= 100.0)
                {
                    Press(Player, EKeys::SpaceBar);
                    bTrailActionPressed = true;
                }
                bJumpStarted |= Player.GetCharacterMovement()->IsFalling();
                bDone = WalkTo(Player, FVector2D(Step.Target)) && bJumpStarted
                    && Player.GetCharacterMovement()->IsMovingOnGround() && Level->FindWadSector(Position) == Step.Sector;
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified a player-input jump onto L531 sector %d."), Step.Sector);
                }
                break;
            }
            case ETrailAction::Door:
            {
                FVector2D Heights;
                const FAAWadNativeDoor* TrailDoor = Level->GetDoors().FindByPredicate([&Step](const FAAWadNativeDoor& Entry)
                    { return Entry.Sector == Step.Sector; });
                if (!TrailDoor || !Level->GetSectorSurfaceHeights(Step.Sector, Heights))
                {
                    return Finish(&Player, TEXT("the authored door is missing from the native level"));
                }
                if (!bTrailActionPressed)
                {
                    if (Heights.X != Heights.Y)
                    {
                        return Finish(&Player, TEXT("the authored door was supposed to start closed"));
                    }
                    Aim(Player, Step.Target);
                    Press(Player, EKeys::E);
                    bTrailActionPressed = true;
                }
                if (bL22Journey && !bTrailArrived && Step.Sector == 59 && !bSavedOpeningDoor
                    && Heights.Y > Heights.X + 12.0 && Heights.Y < TrailDoor->OpenZ - 12.0)
                {
                    bSavedOpeningDoor = Player.SaveWadCheckpoint(TEXT("NativeWadDoorReview"));
                }
                bDone = Heights.Y == TrailDoor->OpenZ;
                if (bDone)
                {
                    if (bL22Journey && !bTrailArrived && Step.Sector == 59 && !bSavedOpeningDoor)
                    {
                        return Finish(&Player, TEXT("the L31 opening door checkpoint could not be saved"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified E opens %s door sector %d to Z%.0f."), *Current.GetMapName(), Step.Sector, Heights.Y);
                }
                break;
            }
            case ETrailAction::Capture:
            {
                Aim(Player, Step.Target);
                bool bSaved = false;
                bDone = CaptureView(Player, Step.Frame, bSaved);
                if (bDone && !bSaved)
                {
                    return Finish(&Player, TEXT("could not save the ravine review frame"));
                }
                break;
            }
            case ETrailAction::Link:
                ExperienceBeforeTravel = Player.GetExperience();
                HealthBeforeTravel = Player.GetHealth();
                FoodBeforeTravel = Player.GetFood();
                ManaBeforeTravel = Player.GetMana();
                if (WalkTo(Player, FVector2D(Step.Target)))
                {
                    return Finish(&Player, TEXT("the authored link failed to travel"));
                }
                break;
            case ETrailAction::Plate:
            {
                FVector2D Heights;
                bDone = Level->FindWadSector(Position) == 107 && Level->GetSectorSurfaceHeights(Step.Sector, Heights)
                    && Heights.X == 88.0;
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified stepping onto L31 sector 107 runs activity 3 and lowers floor 135 to Z88."));
                }
                break;
            }
            case ETrailAction::FloorReset:
            {
                FVector2D Heights;
                bDone = Level->GetSectorSurfaceHeights(Step.Sector, Heights) && Heights.X == Step.Target.Z;
                if (bDone && !bObservedStartupLiftReturn)
                {
                    return Finish(&Player, TEXT("the L31 startup lift did not complete a full cycle"));
                }
                if (bDone)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified L31's pressure-plate floor returned to Z%.0f after the source delay."), Heights.X);
                }
                break;
            }
            }
            if (bDone)
            {
                ++NextPoint;
                RiverProgressTime = FPlatformTime::Seconds();
                bTrailActionPressed = false;
                bJumpStarted = false;
            }
            return true;
        }

        bool TickAmbient(UWorld& Current, ANativeWadPlayer& Player)
        {
            constexpr int32 Stream = 3003;
            constexpr int32 Waterfall = 3007;
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || FPlatformTime::Seconds() - StartTime > 35.0)
            {
                return Finish(&Player, TEXT("the ambient audio review could not reach its expected playback states"));
            }
            const auto StageListener = [&Level, &Player](const FVector2D& Point)
            {
                const int32 Sector = Level->FindWadSector(Point);
                return Sector != INDEX_NONE && Player.TeleportTo(FVector(Point, Level->GetWalkingHeight(Sector)
                    + ANativeWadPlayer::CapsuleHalfHeight + 2.0), FRotator::ZeroRotator);
            };
            const int32 Streams = Level->CountPlayingAmbientEmitters(Stream);
            const int32 Waterfalls = Level->CountPlayingAmbientEmitters(Waterfall);
            if (NextPoint == 2)
            {
                if (Streams == 0 && Waterfalls == 0)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified both L301 water loops stop outside their authored 500-unit radius."));
                    return Finish(&Player);
                }
                return true;
            }
            const bool bAudible = NextPoint == 0 ? Streams == 1 && Waterfalls == 0 : Streams == 0 && Waterfalls == 1;
            const double Now = FPlatformTime::Seconds();
            if (!bAudible)
            {
                EmitterAudibleSince = 0.0;
            }
            else if (EmitterAudibleSince == 0.0)
            {
                EmitterAudibleSince = Now;
            }
            // These windows exceed each complete legacy sample at 11 kHz.
            else if (Now - EmitterAudibleSince > (NextPoint == 0 ? 1.6 : 3.4))
            {
                UE_LOG(LogTemp, Display, TEXT("Verified L301's %s loop stays active past one sample duration."),
                    NextPoint == 0 ? TEXT("stream") : TEXT("waterfall"));
                const FVector2D Point = NextPoint == 0 ? FVector2D(-460, 2484) : FVector2D(-460, 3200);
                if (!StageListener(Point))
                {
                    return Finish(&Player, TEXT("could not stage the L301 audio listener"));
                }
                ++NextPoint;
                EmitterAudibleSince = 0.0;
            }
            return true;
        }

        bool TickL22Trap(UWorld& Current, ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadLevel> Level(&Current);
            if (!Level || FPlatformTime::Seconds() - StartTime > 45.0)
            {
                return Finish(&Player, TEXT("the L22 projectile check timed out"));
            }
            const FVector2D Position(Player.GetActorLocation());
            switch (NextPoint)
            {
            case 0:
            {
                const bool bHasSourceExit = Level->GetLevelLinks().ContainsByPredicate([](const FAAWadNativeLevelLink& Link)
                    { return Link.SourceThing == 113 && Link.Sector == 184 && Link.CampaignForm == 20004 && Link.Destination.IsNull(); });
                if (!bHasSourceExit || !Player.TeleportTo({-500,950,37}, FRotator::ZeroRotator))
                {
                    return Finish(&Player, TEXT("could not stage the L22 trap corridor"));
                }
                TrapHealth = Player.GetHealth();
                ++NextPoint;
                break;
            }
            case 1:
                NextPoint += WalkTo(Player, {-500,1020}) ? 1 : 0;
                break;
            case 2:
                if (Level->GetScriptMissileCount(4011) >= 1 && Level->GetScriptMissilesBlocked() >= 2)
                {
                    if (Player.IsParalyzed() || Player.GetHealth() != TrapHealth || Level->GetScriptMissileCount(4016) != 1)
                    {
                        return Finish(&Player, TEXT("the off-axis trap pass should hit the corridor geometry"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified L22's paralyze missile and magic dart hit geometry with the player off-axis."));
                    ++NextPoint;
                }
                break;
            case 3:
                NextPoint += WalkTo(Player, {-500,950}) ? 1 : 0;
                break;
            case 4:
                NextPoint += WalkTo(Player, {-450,950}) ? 1 : 0;
                break;
            case 5:
                if (Player.IsParalyzed())
                {
                    TrapParalysisStart = Current.GetTimeSeconds();
                    TrapHitPosition = Position;
                    TrapHealth = Player.GetHealth();
                    ++NextPoint;
                }
                else if (WalkTo(Player, {-450,1130}))
                {
                    return Finish(&Player, TEXT("the repeated L22 trap did not paralyze the player in the lane"));
                }
                break;
            case 6:
            {
                const double Elapsed = Current.GetTimeSeconds() - TrapParalysisStart;
                if (Player.IsParalyzed())
                {
                    if (FVector2D::Distance(Position, TrapHitPosition) > 2.0)
                    {
                        return Finish(&Player, TEXT("the player moved while paralyzed with movement input held"));
                    }
                    if (Player.GetHealth() < TrapHealth)
                    {
                        if (!bVerifiedTrapDamage && (TrapHealth - Player.GetHealth() != 100 || Level->GetScriptMissileCount(4011) != 2))
                        {
                            return Finish(&Player, TEXT("the following magic dart did not use its source damage"));
                        }
                        bVerifiedTrapDamage = true;
                    }
                }
                else
                {
                    if (!bVerifiedTrapDamage || !FMath::IsNearlyEqual(Elapsed, 10.0, 0.15))
                    {
                        return Finish(&Player, TEXT("the dart impact or ten-second paralysis differed from the source"));
                    }
                    ++NextPoint;
                }
                break;
            }
            case 7:
                if (Position.Y > TrapHitPosition.Y + 20.0)
                {
                    Stop(Player);
                    UE_LOG(LogTemp, Display, TEXT("Verified the L22 trap retriggers, stops movement for ten seconds, deals 100 dart damage, and releases the player."));
                    return Finish(&Player);
                }
                break;
            }
            return true;
        }

        bool TickLiftCheckpoint(UWorld& Current, ANativeWadPlayer& Player)
        {
            constexpr const TCHAR* Slot = TEXT("NativeWadMotionReview");
            const TActorIterator<ANativeWadLevel> Level(&Current);
            FVector2D Lift;
            if (!Level || !Level->GetSectorSurfaceHeights(31, Lift) || FPlatformTime::Seconds() - StartTime > 38.0)
            {
                return Finish(&Player, TEXT("the lift checkpoint review timed out or lost sector 31"));
            }
            const double Feet = Player.GetActorLocation().Z - ANativeWadPlayer::CapsuleHalfHeight;
            const bool bRiding = FMath::IsNearlyEqual(Feet, Lift.X, 5.0);
            if (!bLiftReload)
            {
                if (NextPoint == 0)
                {
                    if (Lift.X != -250.0 || !Player.TeleportTo({3028,3700,-196}, FRotator::ZeroRotator))
                    {
                        return Finish(&Player, TEXT("the L151 lift is missing or starts at the wrong height"));
                    }
                    ++NextPoint;
                }
                else if (NextPoint == 1 && WalkTo(Player, {3100,3708}))
                {
                    ++NextPoint;
                }
                else if (NextPoint == 2 && Lift.X > -190.0 && Lift.X < -65.0 && bRiding
                    && Player.SaveWadCheckpoint(Slot))
                {
                    const auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(Slot, 0));
                    if (!Save || Save->Level.Slides.Num() != 1 || Save->Level.Slides[0].Sector != 31
                        || Save->Level.Delays.Num() != 1 || Save->Level.Delays[0].Activity != 1)
                    {
                        return Finish(&Player, TEXT("the disk checkpoint is missing the active lift or script delay"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Saved an active L151 lift at Z%.2f with %.3fs left on its script delay."),
                        Lift.X, Save->Level.Delays[0].SecondsRemaining);
                    return Finish(&Player);
                }
                return true;
            }
            switch (NextPoint)
            {
            case 0:
                if (Player.HasRestoredWadCheckpoint())
                {
                    const auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(Slot, 0));
                    if (!Save || Save->Level.Delays.Num() != 1 || !bRiding
                        || !Lift.Equals(Save->Level.SectorHeights.FindRef(31), 5.0))
                    {
                        return Finish(&Player, TEXT("the L151 lift did not restore to the height in its checkpoint"));
                    }
                    SavedLiftDelay = Save->Level.Delays[0].SecondsRemaining;
                    StageTime = Current.GetTimeSeconds();
                    ++NextPoint;
                }
                break;
            case 1:
                NextPoint += Lift.X == 0.0 && bRiding ? 1 : 0;
                break;
            case 2:
            {
                bool bSaved = false;
                Aim(Player, {3400,3620,0});
                if (CaptureView(Player, TEXT("lift-after-restart"), bSaved))
                {
                    if (!bSaved)
                    {
                        return Finish(&Player, TEXT("could not capture the resumed L151 lift"));
                    }
                    ++NextPoint;
                }
                break;
            }
            case 3:
                if (Lift.X == -250.0 && bRiding)
                {
                    if (!FMath::IsNearlyEqual(Current.GetTimeSeconds() - StageTime, SavedLiftDelay + 1.0, 0.18))
                    {
                        return Finish(&Player, TEXT("the restored lift returned at the wrong time"));
                    }
                    UE_LOG(LogTemp, Display, TEXT("Verified the saved L151 lift carried the player up and down and resumed its pending timer."));
                    ++NextPoint;
                }
                break;
            case 4:
                if (Current.GetTimeSeconds() - StageTime >= SavedLiftDelay + 2.15)
                {
                    // Start the repeat check from the same dry approach used before saving.
                    if (!Player.TeleportTo({3028,3700,-196}, FRotator::ZeroRotator))
                    {
                        return Finish(&Player, TEXT("could not reset to the authored lift approach for the repeat check"));
                    }
                    ++NextPoint;
                }
                break;
            case 5:
                if (WalkTo(Player, {3100,3708}) && Lift.X > -250.0 && Level->GetWadScriptBlockCount(1) == 0)
                {
                    UE_LOG(LogTemp, Display, TEXT("Verified the restored L151 lift unlocks and starts a second cycle after re-entry."));
                    return Finish(&Player);
                }
                break;
            }
            return true;
        }

        bool TickDoorCheckpoint(UWorld& Current, ANativeWadPlayer& Player)
        {
            const TActorIterator<ANativeWadLevel> Level(&Current);
            FVector2D Heights;
            const FAAWadNativeDoor* CheckpointDoor = Level ? Level->GetDoors().FindByPredicate(
                [](const FAAWadNativeDoor& Entry) { return Entry.Sector == 59; }) : nullptr;
            if (!CheckpointDoor || !Level->GetSectorSurfaceHeights(59, Heights) || FPlatformTime::Seconds() - StartTime > 20.0)
            {
                return Finish(&Player, TEXT("the L31 door checkpoint review timed out or lost sector 59"));
            }
            if (NextPoint == 0 && Player.HasRestoredWadCheckpoint())
            {
                const auto* Save = Cast<UNativeWadCheckpoint>(UGameplayStatics::LoadGameFromSlot(TEXT("NativeWadDoorReview"), 0));
                const FWadSavedDoor* DoorState = Save ? Save->Level.Doors.Find(59) : nullptr;
                if (!DoorState || DoorState->Phase != 0 || Heights.Y <= Heights.X || Heights.Y >= CheckpointDoor->OpenZ)
                {
                    return Finish(&Player, TEXT("the L31 checkpoint did not restore its partially opened door"));
                }
                StageTime = Current.GetTimeSeconds();
                SavedDoorOpenTime = CheckpointDoor->TravelSeconds - DoorState->Elapsed;
                ++NextPoint;
            }
            else if (NextPoint == 1 && Heights.Y == CheckpointDoor->OpenZ)
            {
                if (!FMath::IsNearlyEqual(Current.GetTimeSeconds() - StageTime, SavedDoorOpenTime, 0.15))
                {
                    return Finish(&Player, TEXT("the restored door finished opening at the wrong time"));
                }
                ++NextPoint;
            }
            else if (NextPoint == 2 && Heights.X == Heights.Y)
            {
                const double Expected = SavedDoorOpenTime + CheckpointDoor->HoldSeconds + CheckpointDoor->TravelSeconds;
                if (!FMath::IsNearlyEqual(Current.GetTimeSeconds() - StageTime, Expected, 0.15))
                {
                    return Finish(&Player, TEXT("the restored door finished closing at the wrong time"));
                }
                UE_LOG(LogTemp, Display, TEXT("Verified L31 door 59 resumed its saved opening phase, held, and closed on schedule."));
                return Finish(&Player);
            }
            return true;
        }

        bool Tick()
        {
            UWorld* Current = World();
            APlayerController* Controller = Current ? Current->GetFirstPlayerController() : nullptr;
            ANativeWadPlayer* Player = Controller ? Cast<ANativeWadPlayer>(Controller->GetPawn()) : nullptr;
            const double Now = FPlatformTime::Seconds();
            if (Now - StartTime > (bRiverReview || bRavineReview || TownReview == ETownReview::Tour || TownReview == ETownReview::Failure
                ? 270.0 : bSlice ? 180.0 : 120.0))
            {
                return Finish(Player, TEXT("the review timed out"));
            }
            if (!Player)
            {
                return true;
            }
            if (TownReview != ETownReview::None)
            {
                return TickTown(*Current, *Player);
            }
            if (bCheckpointReview)
            {
                return TickCheckpoint(*Current, *Player);
            }
            if (bRiverReview)
            {
                return TickRiver(*Current, *Player);
            }
            if (bRavineReview)
            {
                return TickRavine(*Current, *Player);
            }
            if (bAmbientReview)
            {
                return TickAmbient(*Current, *Player);
            }
            if (bL22TrapReview)
            {
                return TickL22Trap(*Current, *Player);
            }
            if (bLiftCheckpointReview)
            {
                return TickLiftCheckpoint(*Current, *Player);
            }
            if (bDoorReload)
            {
                return TickDoorCheckpoint(*Current, *Player);
            }
            if (Current != SourceWorld.Get())
            {
                if ((!bLevelLink && !bSlice) || !Current->GetMapName().EndsWith(TEXT("L301")))
                {
                    return Finish(Player, TEXT("travel did not arrive at the source L301 player start"));
                }
                if (Stage != EStage::Arrival)
                {
                    if (!FVector2D(Player->GetActorLocation()).Equals(FVector2D(3440, 2944), 2.0))
                    {
                        return Finish(Player, TEXT("travel did not arrive at the source L301 player start"));
                    }
                    if (bSlice && (Player->GetItemCount(NativeWadRules::Berry) != 0
                        || Player->GetItemCount(NativeWadRules::IronDagger) != 1
                        || Player->GetEquippedWeapon() != NativeWadRules::IronDagger || Player->GetSuccessfulCasts() != 1
                        || Player->GetExperience() != ExperienceBeforeTravel || Player->GetMana() != ManaBeforeTravel
                        || Player->GetFood() != FoodBeforeTravel || Player->GetHealth() != HealthBeforeTravel
                        || Player->GetCharacterLevel() != LevelBeforeTravel || Player->GetExperienceNeeded() != ThresholdBeforeTravel
                        || Player->GetMaxHealth() != MaxHealthBeforeTravel || Player->GetMaxMana() != MaxManaBeforeTravel
                        || AttributesBeforeTravel.Num() != Player->GetAttributes().Num()
                        || FMemory::Memcmp(AttributesBeforeTravel.GetData(), Player->GetAttributes().GetData(),
                            AttributesBeforeTravel.Num() * sizeof(int32)) != 0
                        || Player->GetHealth() <= 0))
                    {
                        return Finish(Player, TEXT("the gameplay state did not carry into L301"));
                    }
                    Camera.Reset();
                    CaptureFrames = 0;
                    bWalking = false;
                    Stage = EStage::Arrival;
                    NextPoint = 0;
                    bCapturedEncounter = false;
                    HeldKey.Reset();
                    UE_LOG(LogTemp, Display, TEXT("Entered native L301 at source player start (3440, 2944)."));
                    if (bSlice)
                    {
                        UE_LOG(LogTemp, Display, TEXT("Completed continuous L30 gameplay slice. L301 arrival: level %d, health %d/%d, mana %d/%d, food %d, XP %d/%d, dagger ready."),
                            Player->GetCharacterLevel(), Player->GetHealth(), Player->GetMaxHealth(), Player->GetMana(), Player->GetMaxMana(),
                            Player->GetFood(), Player->GetExperience(), Player->GetExperienceNeeded());
                    }
                }
                bool bSaved = false;
                if (!bArrivalCaptured)
                {
                    if (!CaptureView(*Player, TEXT("arrival"), bSaved))
                    {
                        return true;
                    }
                    if (!bSaved || !bCampaign)
                    {
                        return Finish(Player, bSaved ? nullptr : TEXT("could not save the L301 arrival frame"));
                    }
                    bArrivalCaptured = true;
                }
                return TickL301(*Current, *Player);
            }

            if (bSlice)
            {
                return TickSlice(*Current, *Player);
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

bool StartNativeWadWalkReview(UWorld* World, const FString& OutputPath, int32 ImageSize, bool bLevelLink, bool bSlice,
    bool bSecondMap, bool bCampaign, bool bCheckpoint)
{
    ANativeWadLevel* Level = GetReviewLevel(World, OutputPath, ImageSize);
    const FWorldContext* Context = Level ? GEngine->GetWorldContextFromWorld(World) : nullptr;
    if (!Context || !World->GetMapName().EndsWith(bSecondMap ? TEXT("L151") : TEXT("L30")))
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
    Review->bSlice = bSlice || bSecondMap || bCampaign;
    Review->bSecondMap = bSecondMap;
    Review->bCampaign = bCampaign;
    Review->bCheckpointReview = bCheckpoint;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float) { return Review->Tick(); }));
    return true;
}

bool StartNativeWadRiverReview(UWorld* World, const FString& OutputPath, int32 ImageSize, bool bReload)
{
    ANativeWadLevel* Level = GetReviewLevel(World, OutputPath, ImageSize);
    const FWorldContext* Context = Level ? GEngine->GetWorldContextFromWorld(World) : nullptr;
    if (!Context || !World->GetMapName().EndsWith(bReload ? TEXT("L531") : TEXT("L301")))
    {
        return false;
    }
    const TSharedRef<FWalkReview> Review = MakeShared<FWalkReview>();
    Review->WorldHandle = Context->ContextHandle;
    Review->SourceWorld = World;
    Review->Output = OutputPath;
    Review->StartTime = FPlatformTime::Seconds();
    Review->Size = ImageSize;
    Review->bRiverReview = true;
    Review->bRiverReload = bReload;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float) { return Review->Tick(); }));
    return true;
}

bool StartNativeWadExtensionReview(UWorld* World, const FString& OutputPath, int32 ImageSize, const FString& Mode)
{
    const ETownReview TownReview = Mode == TEXT("towntour") ? ETownReview::Tour : Mode == TEXT("townfailure") ? ETownReview::Failure
        : Mode == TEXT("townreload") ? ETownReview::Reload : Mode == TEXT("townstart") ? ETownReview::Start : ETownReview::None;
    const bool bStartsInTown = TownReview == ETownReview::Reload || TownReview == ETownReview::Start;
    const bool bHasReviewWorld = bStartsInTown ? World && World->IsGameWorld() && World->GetAuthGameMode<ANativeWadTownGameMode>()
        : GetReviewLevel(World, OutputPath, ImageSize) != nullptr;
    const FWorldContext* Context = bHasReviewWorld ? GEngine->GetWorldContextFromWorld(World) : nullptr;
    const bool bAmbient = Mode == TEXT("ambient");
    const bool bL22Journey = Mode == TEXT("l31tol22");
    const bool bL22Trap = Mode == TEXT("l22trap");
    const bool bLiftCheckpoint = Mode == TEXT("liftsave") || Mode == TEXT("liftreload");
    const bool bDoorReload = Mode == TEXT("doorreload");
    const TCHAR* StartMap = bStartsInTown ? TEXT("WadTown") : bAmbient ? TEXT("L301")
        : bL22Journey || bDoorReload ? TEXT("L31")
        : bL22Trap || TownReview == ETownReview::Tour || TownReview == ETownReview::Failure ? TEXT("L22")
        : bLiftCheckpoint ? TEXT("L151") : TEXT("L531");
    if (!Context || !World->GetMapName().EndsWith(StartMap))
    {
        return false;
    }
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
    const TSharedRef<FWalkReview> Review = MakeShared<FWalkReview>();
    Review->WorldHandle = Context->ContextHandle;
    Review->SourceWorld = World;
    Review->Output = OutputPath;
    Review->StartTime = FPlatformTime::Seconds();
    Review->Size = ImageSize;
    Review->bAmbientReview = bAmbient;
    Review->bRavineReview = Mode == TEXT("ravine") || bL22Journey;
    Review->bL22Journey = bL22Journey;
    Review->bL22TrapReview = bL22Trap;
    Review->bLiftCheckpointReview = bLiftCheckpoint;
    Review->bLiftReload = Mode == TEXT("liftreload");
    Review->bDoorReload = bDoorReload;
    Review->TownReview = TownReview;
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
